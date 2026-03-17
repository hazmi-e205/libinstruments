#include "../../include/instruments/device_connection.h"
#include "service_connector.h"
#include "rsd_provider.h"
#include "tunnel_quic.h"
#include "../util/log.h"
#include <array>
#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif

// Platform headers for direct USB-NCM IPv6 discovery (TryDirectNCMConnection)
#ifdef _WIN32
#include <ws2tcpip.h>
#include <iphlpapi.h>   // GetAdaptersAddresses, GetIpNetTable2
#include <netioapi.h>   // MIB_IPNET_TABLE2
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>    // getifaddrs
#include <net/if.h>     // if_nametoindex
#include <poll.h>
#include <fcntl.h>
// Linux netlink for NDP neighbor table
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/neighbour.h>
#endif

// Undefine Windows API macro that collides with StartService method name
// <winsvc.h> (via <windows.h>) defines StartService as StartServiceW
#ifdef StartService
#undef StartService
#endif

namespace instruments {

static const char* TAG = "DeviceConnection";

static std::string BuildServiceListDebugString(const std::map<std::string, uint16_t>& services) {
    std::ostringstream oss;
    bool first = true;
    for (const auto& [name, port] : services) {
        if (!first) {
            oss << ", ";
        }
        first = false;
        oss << name << ":" << port;
    }
    return oss.str();
}

static bool TryFindInstrumentServicePort(const std::map<std::string, uint16_t>& services,
                                         std::string& outServiceName,
                                         uint16_t& outPort) {
    static const std::array<std::string, 6> kCandidates = {
        ServiceName::Instruments17Plus,
        std::string(ServiceName::Instruments17Plus) + ".shim.remote",
        std::string(ServiceName::Instruments17Plus) + ".remote",
        ServiceName::Instruments14To16,
        ServiceName::InstrumentsPre14,
        std::string(ServiceName::Instruments14To16) + ".shim.remote",
    };

    for (const auto& candidate : kCandidates) {
        auto it = services.find(candidate);
        if (it != services.end() && it->second != 0) {
            outServiceName = it->first;
            outPort = it->second;
            return true;
        }
    }
    return false;
}

DeviceConnection::DeviceConnection() = default;

void DeviceConnection::TryUsbRSD() {
    // Attempt USB RSD for iOS 17+ devices: connect to RSD port 58783 via usbmuxd.
    // NOTE: This works only if the device exposes port 58783 via usbmuxd TCP forwarding.
    // In practice this succeeds only on some early iOS 17.x builds; iOS 18+ (iOS 26+)
    // devices refuse the connection (CONNREFUSED). Most iOS 17+ USB devices require an
    // external CoreDevice QUIC tunnel — use go-ios `ios tunnel start` or
    // pymobiledevice3 `remote start-tunnel`, then call Instruments::CreateFromTunnel().
    if (m_protocol != IOSProtocol::RSD || !m_device) return;

    RSDProvider rsd;
    Error err = rsd.ConnectViaIDevice(m_device);
    if (err != Error::Success) {
        INST_LOG_WARN(TAG, "iOS 17+ USB RSD via usbmuxd failed (%s) — "
                     "this is expected on iOS 18+ (iOS 26+). "
                     "Attempting USB CoreDevice QUIC tunnel...",
                     ErrorToString(err));
        TryUsbQUIC();
        return;
    }

    for (const auto& [name, entry] : rsd.GetServices()) {
        m_rsdServices[name] = entry.port;
    }
    m_isUsbRsd = true;
    m_isTunnel = true;  // reuse the tunnel path in CreateInstrumentConnection
    INST_LOG_INFO(TAG, "iOS 17+ USB RSD: %zu services discovered", m_rsdServices.size());
}

// ---------- Direct USB-NCM IPv6 helpers ----------

// Create a connected IPv6 TCP socket to [addr]:port with a timeout.
// scopeId is required for link-local addresses (interface index).
// Returns fd >= 0 on success, -1 on failure.
static int ConnectIPv6WithTimeout(const std::string& addr, uint32_t scopeId,
                                   uint16_t port, int timeoutMs) {
#ifdef _WIN32
    SOCKET fd = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (fd == INVALID_SOCKET) return -1;
    u_long nbMode = 1;
    ioctlsocket(fd, FIONBIO, &nbMode);
    struct sockaddr_in6 sa = {};
    sa.sin6_family   = AF_INET6;
    sa.sin6_port     = htons(port);
    sa.sin6_scope_id = scopeId;
    inet_pton(AF_INET6, addr.c_str(), &sa.sin6_addr);
    int ret = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    if (ret == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
        closesocket(fd); return -1;
    }
    fd_set wfds; FD_ZERO(&wfds); FD_SET(fd, &wfds);
    struct timeval tv = { timeoutMs / 1000, (timeoutMs % 1000) * 1000 };
    if (select(0, nullptr, &wfds, nullptr, &tv) <= 0) {
        closesocket(fd); return -1;
    }
    int sockErr = 0; int errLen = sizeof(sockErr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&sockErr), &errLen);
    if (sockErr != 0) { closesocket(fd); return -1; }
    nbMode = 0; ioctlsocket(fd, FIONBIO, &nbMode);
    return static_cast<int>(fd);
#else
    int fd = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_in6 sa = {};
    sa.sin6_family   = AF_INET6;
    sa.sin6_port     = htons(port);
    sa.sin6_scope_id = scopeId;
    inet_pton(AF_INET6, addr.c_str(), &sa.sin6_addr);
    int ret = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    if (ret < 0 && errno != EINPROGRESS) { ::close(fd); return -1; }
    struct pollfd pfd = { fd, POLLOUT, 0 };
    if (poll(&pfd, 1, timeoutMs) <= 0) { ::close(fd); return -1; }
    int sockErr = 0; socklen_t errLen = sizeof(sockErr);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &sockErr, &errLen);
    if (sockErr != 0) { ::close(fd); return -1; }
    fcntl(fd, F_SETFL, flags);
    return fd;
#endif
}

struct NCMCandidate { std::string ipv6; uint32_t scopeId; };

// Discover iOS device IPv6 addresses on Apple USB-NCM network interfaces.
// Windows: GetAdaptersAddresses (filter by "Apple" description) + GetIpNetTable2
// Linux:   sysfs idVendor check + netlink RTM_GETNEIGH for neighbor table
static const char* NCM_TAG = "DeviceConnection";

// Send a UDP datagram to ff02::1 (all-nodes multicast) on the given interface.
// This triggers NDP neighbour solicitation, causing the iOS device to advertise
// its link-local IPv6 and populate the host's NDP cache — no admin needed.
static void TriggerNDPProbe(uint32_t ifIndex) {
#ifdef _WIN32
    SOCKET s = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;
    DWORD idx = ifIndex;
    setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_IF,
               reinterpret_cast<const char*>(&idx), sizeof(idx));
    struct sockaddr_in6 dest = {};
    dest.sin6_family   = AF_INET6;
    dest.sin6_port     = htons(9);   // discard port — nobody listens, that's fine
    dest.sin6_scope_id = ifIndex;
    inet_pton(AF_INET6, "ff02::1", &dest.sin6_addr);
    const char probe[1] = {0};
    sendto(s, probe, 1, 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    closesocket(s);
#else
    int s = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s < 0) return;
    unsigned int idx = ifIndex;
    setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_IF, &idx, sizeof(idx));
    struct sockaddr_in6 dest = {};
    dest.sin6_family   = AF_INET6;
    dest.sin6_port     = htons(9);
    dest.sin6_scope_id = ifIndex;
    inet_pton(AF_INET6, "ff02::1", &dest.sin6_addr);
    const char probe[1] = {0};
    sendto(s, probe, 1, 0, reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    ::close(s);
#endif
}

static std::vector<NCMCandidate> DiscoverAppleNCMCandidates() {
    std::vector<NCMCandidate> result;

#ifdef _WIN32
    // --- Windows: find Apple USB-NCM adapters, then get neighbor IPv6 ---
    ULONG bufLen = 15000;
    std::vector<uint8_t> buf(bufLen);
    ULONG rc;
    do {
        // Use AF_UNSPEC to enumerate ALL adapters regardless of address family.
        // AF_INET6 would skip the Apple USB-NCM adapter when it has no IPv6 yet.
        rc = GetAdaptersAddresses(AF_UNSPEC,
            GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
            nullptr,
            reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &bufLen);
        if (rc == ERROR_BUFFER_OVERFLOW) buf.resize(bufLen);
    } while (rc == ERROR_BUFFER_OVERFLOW);
    if (rc != NO_ERROR) return result;

    // Log all adapters and collect Apple USB-NCM interface indices.
    // Apple's driver may name the adapter "Apple Mobile Device Ethernet",
    // "Apple USB Ethernet Adapter", or similar (may vary by locale/version).
    std::vector<NET_IFINDEX> appleIfIndices;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        char descNarrow[256] = {};
        if (a->Description) {
            WideCharToMultiByte(CP_UTF8, 0, a->Description, -1,
                                descNarrow, sizeof(descNarrow) - 1, nullptr, nullptr);
        }
        std::wstring desc = a->Description ? a->Description : L"";
        bool isApple = desc.find(L"Apple")  != std::wstring::npos
                    || desc.find(L"iPhone") != std::wstring::npos
                    || desc.find(L"iPad")   != std::wstring::npos
                    || desc.find(L"iPod")   != std::wstring::npos;
        INST_LOG_INFO(NCM_TAG, "NCM: adapter idx=%u '%s'%s",
                     a->IfIndex, descNarrow, isApple ? " [Apple]" : "");
        if (isApple) appleIfIndices.push_back(a->IfIndex);
    }
    if (appleIfIndices.empty()) {
        INST_LOG_WARN(NCM_TAG, "TryDirectNCMConnection: no Apple USB-NCM adapter found. "
                     "iOS 18+ requires the Apple Mobile Device NCM driver. "
                     "Install 'Apple Devices' from the Microsoft Store to enable USB-NCM support, "
                     "then reconnect the device.");
        return result;
    }

    // Trigger NDP neighbor discovery on each Apple interface, then wait for responses.
    for (auto idx : appleIfIndices) TriggerNDPProbe(idx);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Read IPv6 neighbor table — should now contain the device's link-local address.
    PMIB_IPNET_TABLE2 pTable = nullptr;
    if (GetIpNetTable2(AF_INET6, &pTable) != NO_ERROR || !pTable) {
        INST_LOG_WARN(NCM_TAG, "TryDirectNCMConnection: GetIpNetTable2 failed");
        return result;
    }
    for (ULONG i = 0; i < pTable->NumEntries; i++) {
        const MIB_IPNET_ROW2& row = pTable->Table[i];
        if (row.Address.si_family != AF_INET6) continue;
        bool isApple = false;
        for (auto idx : appleIfIndices) {
            if (row.InterfaceIndex == idx) { isApple = true; break; }
        }
        if (!isApple) continue;
        // Only link-local (fe80::/10)
        const uint8_t* a6 = row.Address.Ipv6.sin6_addr.u.Byte;
        if ((a6[0] & 0xFE) != 0xFE || (a6[1] & 0xC0) != 0x80) continue;
        char ipStr[INET6_ADDRSTRLEN] = {};
        inet_ntop(AF_INET6, &row.Address.Ipv6.sin6_addr, ipStr, sizeof(ipStr));
        INST_LOG_INFO(NCM_TAG, "TryDirectNCMConnection: neighbor [%s] on ifIdx=%u",
                     ipStr, row.InterfaceIndex);
        result.push_back({ipStr, row.InterfaceIndex});
    }
    FreeMibTable(pTable);
    if (result.empty()) {
        INST_LOG_WARN(NCM_TAG, "TryDirectNCMConnection: Apple adapter found but NDP cache "
                     "has no link-local neighbors (device may not have responded to probe)");
    }

#else
    // --- Linux: find Apple USB interfaces via sysfs ---
    struct ifaddrs* iflist = nullptr;
    if (getifaddrs(&iflist) != 0) return result;

    std::vector<std::pair<int, std::string>> appleIfaces; // {ifindex, ifname}
    for (auto* ifa = iflist; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name) continue;
        std::string sysPath = std::string("/sys/class/net/") + ifa->ifa_name + "/device/idVendor";
        FILE* f = fopen(sysPath.c_str(), "r");
        if (!f) continue;
        char vendorBuf[16] = {};
        fread(vendorBuf, 1, sizeof(vendorBuf) - 1, f);
        fclose(f);
        if (strstr(vendorBuf, "05ac") == nullptr && strstr(vendorBuf, "0x05ac") == nullptr) continue;
        int idx = if_nametoindex(ifa->ifa_name);
        if (idx > 0) {
            INST_LOG_INFO(NCM_TAG, "NCM: Apple USB interface '%s' idx=%d", ifa->ifa_name, idx);
            appleIfaces.push_back({idx, ifa->ifa_name});
        }
    }
    freeifaddrs(iflist);
    if (appleIfaces.empty()) {
        INST_LOG_WARN(NCM_TAG, "TryDirectNCMConnection: no Apple USB-NCM interface found in sysfs");
        return result;
    }

    // Trigger NDP probe then wait
    for (auto& [idx, name] : appleIfaces)
        TriggerNDPProbe(static_cast<uint32_t>(idx));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Query NDP neighbor table via netlink RTM_GETNEIGH
    int nlSock = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (nlSock < 0) return result;

    struct {
        struct nlmsghdr nlh;
        struct ndmsg    ndm;
    } req = {};
    req.nlh.nlmsg_len   = NLMSG_LENGTH(sizeof(struct ndmsg));
    req.nlh.nlmsg_type  = RTM_GETNEIGH;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    req.nlh.nlmsg_seq   = 1;
    req.ndm.ndm_family  = AF_INET6;
    if (send(nlSock, &req, req.nlh.nlmsg_len, 0) < 0) {
        ::close(nlSock);
        return result;
    }

    char recvBuf[16384];
    bool done = false;
    while (!done) {
        ssize_t n = recv(nlSock, recvBuf, sizeof(recvBuf), 0);
        if (n <= 0) break;
        for (struct nlmsghdr* nlh2 = reinterpret_cast<struct nlmsghdr*>(recvBuf);
             NLMSG_OK(nlh2, static_cast<unsigned>(n));
             nlh2 = NLMSG_NEXT(nlh2, n)) {
            if (nlh2->nlmsg_type == NLMSG_DONE) { done = true; break; }
            if (nlh2->nlmsg_type == NLMSG_ERROR) { done = true; break; }
            if (nlh2->nlmsg_type != RTM_NEWNEIGH) continue;
            struct ndmsg* ndm2 = reinterpret_cast<struct ndmsg*>(NLMSG_DATA(nlh2));
            if (ndm2->ndm_family != AF_INET6) continue;
            bool isApple = false;
            for (auto& [idx, name] : appleIfaces) {
                if (ndm2->ndm_ifindex == idx) { isApple = true; break; }
            }
            if (!isApple) continue;
            struct rtattr* rta = reinterpret_cast<struct rtattr*>(
                reinterpret_cast<uint8_t*>(ndm2) + sizeof(struct ndmsg));
            int rtaLen = static_cast<int>(nlh2->nlmsg_len) - NLMSG_LENGTH(sizeof(struct ndmsg));
            for (; RTA_OK(rta, rtaLen); rta = RTA_NEXT(rta, rtaLen)) {
                if (rta->rta_type != NDA_DST) continue;
                if (RTA_PAYLOAD(rta) != 16) continue;
                const uint8_t* a6 = reinterpret_cast<const uint8_t*>(RTA_DATA(rta));
                if ((a6[0] & 0xFE) != 0xFE || (a6[1] & 0xC0) != 0x80) continue;
                char ipStr[INET6_ADDRSTRLEN] = {};
                inet_ntop(AF_INET6, a6, ipStr, sizeof(ipStr));
                INST_LOG_INFO(NCM_TAG, "TryDirectNCMConnection: neighbor [%s] ifIdx=%d",
                             ipStr, ndm2->ndm_ifindex);
                result.push_back({ipStr, static_cast<uint32_t>(ndm2->ndm_ifindex)});
            }
        }
    }
    ::close(nlSock);
    if (result.empty()) {
        INST_LOG_WARN(NCM_TAG, "TryDirectNCMConnection: Apple interface found but NDP cache empty");
    }
#endif

    return result;
}

void DeviceConnection::TryUsbQUIC() {
#ifdef INSTRUMENTS_HAS_QUIC
    // Helper: try CDTunnel via CoreDeviceProxy, do RSD handshake, populate services.
    // Defined as a lambda to keep it scoped and avoid unused-function warnings without QUIC.
    auto tryCDTunnelAndRSD = [](QUICTunnel& tunnel, idevice_t device,
                                std::map<std::string, uint16_t>& servicesOut,
                                std::string& serverAddrOut, uint16_t& rsdPortOut) -> bool {
        Error err = tunnel.ConnectViaCoreDeviceProxy(device);
        if (err != Error::Success) return false;

        int rsdFd = tunnel.CreateTunnelSocket(tunnel.ServerAddress(), tunnel.ServerRSDPort());
        if (rsdFd < 0) {
            INST_LOG_ERROR("DeviceConnection", "CDTunnel: CreateTunnelSocket for RSD failed");
            return false;
        }

        RSDProvider rsd;
        Error rsdErr = rsd.ConnectViaFd(rsdFd);
#ifdef _WIN32
        closesocket(static_cast<SOCKET>(rsdFd));
#else
        ::close(rsdFd);
#endif

        if (rsdErr != Error::Success || rsd.GetServices().empty()) {
            if (rsdErr == Error::Success) {
                INST_LOG_WARN("DeviceConnection", "CDTunnel: RSD handshake returned no services");
            } else {
                INST_LOG_WARN("DeviceConnection", "CDTunnel: RSD handshake failed: %s",
                             ErrorToString(rsdErr));
            }
            return false;
        }

        for (const auto& [name, entry] : rsd.GetServices()) {
            servicesOut[name] = entry.port;
        }
        serverAddrOut = tunnel.ServerAddress();
        rsdPortOut = tunnel.ServerRSDPort();
        return true;
    };

    if (m_protocol != IOSProtocol::RSD || !m_device) return;

    int major = 0, minor = 0, patch = 0;
    ServiceConnector::ParseVersion(m_iosVersion, major, minor, patch);

    // Phase 0: Try CDTunnel (CoreDeviceProxy) — works on iOS 17.4+, iOS 18+/26+.
    // Uses standard lockdownd + simple TCP protocol (no QUIC, no admin, no NCM driver).
    // This is the preferred path and works with iTunes-only (no Apple Devices app).
    {
        INST_LOG_INFO(TAG, "iOS %s: trying CDTunnel (com.apple.internal.devicecompute.CoreDeviceProxy)...",
                     m_iosVersion.c_str());
        auto cdTunnel = std::make_shared<QUICTunnel>();
        std::map<std::string, uint16_t> services;
        std::string serverAddr;
        uint16_t rsdPort = 0;
        if (tryCDTunnelAndRSD(*cdTunnel, m_device, services, serverAddr, rsdPort)) {
            m_quicTunnel = cdTunnel;
            m_tunnelAddress = serverAddr;
            m_tunnelRsdPort = rsdPort;
            m_isTunnel = true;
            m_isUsbQuic = true;
            m_rsdServices = std::move(services);
            INST_LOG_INFO(TAG, "iOS %s CDTunnel active: %zu services discovered",
                         m_iosVersion.c_str(), m_rsdServices.size());
            return;
        }
        INST_LOG_DEBUG(TAG, "CDTunnel unavailable on iOS %s — trying other methods...",
                      m_iosVersion.c_str());
    }

    if (major > 17) {
        // iOS 18+/26+: CDTunnel should have worked; if it didn't, try USB-NCM approaches.
        // (CDTunnel is not available if device is not yet trusted, or service is absent.)

        // Phase 1: USB-NCM via Apple's usbmuxd PREFER_NETWORK — requires Apple Devices app.
        INST_LOG_INFO(TAG, "iOS %s: trying USB-NCM network path (Apple usbmuxd)...",
                     m_iosVersion.c_str());
        TryNetworkRSD();
        if (m_isUsbRsd) return;

        // Phase 2: Direct USB-NCM IPv6 — enumerate host adapters, find Apple NCM interface.
        TryDirectNCMConnection();
        if (m_isDirectNCM) return;

        // Phase 3: Nothing worked.
        INST_LOG_WARN(TAG, "iOS %s: automatic USB tunnel failed. "
                     "CDTunnel requires the device to be trusted (unlocked/paired). "
                     "On Windows, install 'Apple Devices' from the Microsoft Store for USB-NCM. "
                     "Alternatively, use an external CoreDevice tunnel:\n"
                     "  go-ios:          ios tunnel start --userspace --udid=<UDID>\n"
                     "  pymobiledevice3: python3 -m pymobiledevice3 remote start-tunnel\n"
                     "Then call Instruments::CreateFromTunnel(address, port, \"%s\").",
                     m_iosVersion.c_str(), m_iosVersion.c_str());
        return;
    }

    // iOS 17.0-17.3: CDTunnel not available — fall back to QUIC over lockdown stream.
    INST_LOG_INFO(TAG, "iOS %s: CDTunnel unavailable — trying QUIC lockdown tunnel...",
                 m_iosVersion.c_str());

    auto quicTunnel = std::make_shared<QUICTunnel>();
    Error err = quicTunnel->ConnectViaUSB(m_device);
    if (err != Error::Success) {
        INST_LOG_WARN(TAG, "USB QUIC tunnel failed: %s — "
                     "use an external CoreDevice tunnel: "
                     "go-ios: `ios tunnel start --udid=<UDID>`, "
                     "pymobiledevice3: `python3 -m pymobiledevice3 remote start-tunnel`. "
                     "Then call Instruments::CreateFromTunnel(address, port, iosVersion).",
                     ErrorToString(err));
        return;
    }

    int rsdFd = quicTunnel->CreateTunnelSocket(
        quicTunnel->ServerAddress(), quicTunnel->ServerRSDPort());
    if (rsdFd < 0) {
        INST_LOG_ERROR(TAG, "TryUsbQUIC: CreateTunnelSocket for RSD failed");
        return;
    }

    RSDProvider rsd;
    Error rsdErr = rsd.ConnectViaFd(rsdFd);
#ifdef _WIN32
    closesocket(static_cast<SOCKET>(rsdFd));
#else
    ::close(rsdFd);
#endif

    if (rsdErr != Error::Success || rsd.GetServices().empty()) {
        INST_LOG_WARN(TAG, "TryUsbQUIC: RSD via USB QUIC failed: %s", ErrorToString(rsdErr));
        return;
    }

    m_quicTunnel = quicTunnel;
    m_tunnelAddress = quicTunnel->ServerAddress();
    m_tunnelRsdPort = quicTunnel->ServerRSDPort();
    m_isTunnel = true;
    m_isUsbQuic = true;

    for (const auto& [name, entry] : rsd.GetServices()) {
        m_rsdServices[name] = entry.port;
    }

    INST_LOG_INFO(TAG, "iOS %s USB QUIC tunnel active: %zu services discovered",
                 m_iosVersion.c_str(), m_rsdServices.size());
#else
    INST_LOG_WARN(TAG, "TryUsbQUIC: INSTRUMENTS_HAS_QUIC not enabled — "
                 "cannot use USB CoreDevice tunnel (CDTunnel or QUIC). "
                 "Use an external CoreDevice tunnel instead.");
#endif
}

void DeviceConnection::TryNetworkRSD() {
    // iOS 18+/26+: Apple's usbmuxd (from iTunes / Apple Devices app) registers
    // iOS 18+ USB-connected devices with their USB-NCM IPv6 link-local address.
    // idevice_new_with_options(..., PREFER_NETWORK) returns a NETWORK-type idevice_t,
    // and idevice_connect() on that device uses direct TCP — bypassing the usbmuxd
    // port-forwarding that returns CONNREFUSED for port 58783 on iOS 18+.
    if (!m_device) return;

    char* udidStr = nullptr;
    if (idevice_get_udid(m_device, &udidStr) != IDEVICE_E_SUCCESS || !udidStr) {
        INST_LOG_DEBUG(TAG, "TryNetworkRSD: idevice_get_udid failed");
        return;
    }

    idevice_t netDevice = nullptr;
    idevice_error_t ierr = idevice_new_with_options(
        &netDevice, udidStr,
        static_cast<idevice_options>(IDEVICE_LOOKUP_NETWORK | IDEVICE_LOOKUP_PREFER_NETWORK));
    free(udidStr);

    if (ierr != IDEVICE_E_SUCCESS || !netDevice) {
        INST_LOG_DEBUG(TAG, "TryNetworkRSD: no network device found for this UDID via usbmuxd "
                      "(usbmuxd may not expose USB-NCM IPv6 for iOS 18+ on this system)");
        return;
    }

    RSDProvider rsd;
    Error err = rsd.ConnectViaIDevice(netDevice);
    if (err != Error::Success) {
        idevice_free(netDevice);
        INST_LOG_DEBUG(TAG, "TryNetworkRSD: RSD via network device failed: %s", ErrorToString(err));
        return;
    }

    m_networkDevice = netDevice;
    m_isUsbRsd = true;
    m_isTunnel = true;
    for (const auto& [name, entry] : rsd.GetServices()) {
        m_rsdServices[name] = entry.port;
    }
    INST_LOG_INFO(TAG, "iOS 18+ USB-NCM network RSD: %zu services discovered",
                 m_rsdServices.size());
}

void DeviceConnection::TryDirectNCMConnection() {
    // iOS 18+/26+: find device's link-local IPv6 on USB-NCM adapter, connect via TCP.
    // Works without admin — uses host OS network adapter APIs (no libusb).
    if (m_protocol != IOSProtocol::RSD) return;

    INST_LOG_INFO(TAG, "iOS %s: trying direct USB-NCM IPv6 discovery...", m_iosVersion.c_str());

    auto candidates = DiscoverAppleNCMCandidates();
    if (candidates.empty()) {
        INST_LOG_DEBUG(TAG, "TryDirectNCMConnection: no Apple USB-NCM adapters or neighbors found");
        return;
    }

    for (auto& c : candidates) {
        INST_LOG_DEBUG(TAG, "TryDirectNCMConnection: trying [%s]:58783 (scope=%u)",
                      c.ipv6.c_str(), c.scopeId);

        int fd = ConnectIPv6WithTimeout(c.ipv6, c.scopeId, 58783, 500);
        if (fd < 0) {
            INST_LOG_DEBUG(TAG, "TryDirectNCMConnection: connect to [%s]:58783 failed",
                          c.ipv6.c_str());
            continue;
        }

        RSDProvider rsd;
        Error err = rsd.ConnectViaFd(fd);
#ifdef _WIN32
        closesocket(static_cast<SOCKET>(fd));
#else
        ::close(fd);
#endif
        if (err != Error::Success || rsd.GetServices().empty()) {
            INST_LOG_DEBUG(TAG, "TryDirectNCMConnection: RSD at [%s] failed: %s",
                          c.ipv6.c_str(), ErrorToString(err));
            continue;
        }

        INST_LOG_INFO(TAG, "iOS %s direct USB-NCM: %zu services at [%s]",
                     m_iosVersion.c_str(), rsd.GetServices().size(), c.ipv6.c_str());
        m_ncmAddress  = c.ipv6;
        m_ncmScopeId  = c.scopeId;
        m_isDirectNCM = true;
        m_isTunnel    = true;
        for (const auto& [name, entry] : rsd.GetServices()) {
            m_rsdServices[name] = entry.port;
        }
        return;
    }
    INST_LOG_DEBUG(TAG, "TryDirectNCMConnection: no device responded on port 58783");
}

DeviceConnection::~DeviceConnection() {
    if (m_lockdown && m_ownsLockdown) {
        lockdownd_client_free(m_lockdown);
        m_lockdown = nullptr;
    }
    if (m_device && m_ownsDevice) {
        idevice_free(m_device);
        m_device = nullptr;
    }
    if (m_networkDevice) {
        idevice_free(m_networkDevice);
        m_networkDevice = nullptr;
    }
}

std::shared_ptr<DeviceConnection> DeviceConnection::FromUDID(const std::string& udid) {
    auto conn = std::shared_ptr<DeviceConnection>(new DeviceConnection());

    idevice_error_t err = idevice_new_with_options(
        &conn->m_device, udid.c_str(),
        static_cast<idevice_options>(IDEVICE_LOOKUP_USBMUX | IDEVICE_LOOKUP_NETWORK));

    if (err != IDEVICE_E_SUCCESS) {
        INST_LOG_ERROR(TAG, "Failed to create device for UDID %s: error %d",
                      udid.c_str(), err);
        return nullptr;
    }

    conn->m_ownsDevice = true;
    conn->m_iosVersion = ServiceConnector::GetIOSVersion(conn->m_device);
    conn->m_protocol = ServiceConnector::DetectProtocol(conn->m_device);
    conn->TryUsbRSD();

    INST_LOG_INFO(TAG, "Connected to %s (iOS %s, protocol=%d, usbRsd=%d)",
                 udid.c_str(), conn->m_iosVersion.c_str(),
                 static_cast<int>(conn->m_protocol), conn->m_isUsbRsd ? 1 : 0);

    return conn;
}

std::shared_ptr<DeviceConnection> DeviceConnection::FromDevice(idevice_t device) {
    if (!device) return nullptr;

    auto conn = std::shared_ptr<DeviceConnection>(new DeviceConnection());
    conn->m_device = device;
    conn->m_ownsDevice = false;
    conn->m_iosVersion = ServiceConnector::GetIOSVersion(device);
    conn->m_protocol = ServiceConnector::DetectProtocol(device);
    conn->TryUsbRSD();

    INST_LOG_INFO(TAG, "Using existing device (iOS %s, protocol=%d, usbRsd=%d)",
                 conn->m_iosVersion.c_str(), static_cast<int>(conn->m_protocol),
                 conn->m_isUsbRsd ? 1 : 0);

    return conn;
}

std::shared_ptr<DeviceConnection> DeviceConnection::FromDevice(idevice_t device, lockdownd_client_t lockdown) {
    if (!device) return nullptr;

    auto conn = std::shared_ptr<DeviceConnection>(new DeviceConnection());
    conn->m_device = device;
    conn->m_ownsDevice = false;
    conn->m_lockdown = lockdown;
    conn->m_ownsLockdown = false;
    conn->m_iosVersion = ServiceConnector::GetIOSVersion(device, lockdown);
    conn->m_protocol = ServiceConnector::DetectProtocol(device, lockdown);
    conn->TryUsbRSD();

    INST_LOG_INFO(TAG, "Using existing device with lockdown (iOS %s, protocol=%d, usbRsd=%d)",
                 conn->m_iosVersion.c_str(), static_cast<int>(conn->m_protocol),
                 conn->m_isUsbRsd ? 1 : 0);

    return conn;
}

std::shared_ptr<DeviceConnection> DeviceConnection::FromTunnel(
    const std::string& address, uint16_t rsdPort, const std::string& iosVersion) {

    if (address.empty()) {
        INST_LOG_ERROR(TAG, "FromTunnel: address is empty");
        return nullptr;
    }

    auto conn = std::shared_ptr<DeviceConnection>(new DeviceConnection());
    conn->m_tunnelAddress = address;
    conn->m_tunnelRsdPort = rsdPort;
    conn->m_isTunnel = true;
    conn->m_iosVersion = iosVersion;
    conn->m_protocol = IOSProtocol::RSD;

    INST_LOG_INFO(TAG, "FromTunnel: performing RSD service discovery at [%s]:%u",
                 address.c_str(), rsdPort);

    RSDProvider rsd;
    Error err = rsd.ConnectDirect(address, rsdPort);
    if (err != Error::Success) {
        INST_LOG_ERROR(TAG, "FromTunnel: RSD discovery failed: %s", ErrorToString(err));
        return nullptr;
    }

    // Copy the discovered service map
    for (const auto& [name, entry] : rsd.GetServices()) {
        conn->m_rsdServices[name] = entry.port;
    }

    INST_LOG_INFO(TAG, "FromTunnel: connected — %zu services discovered",
                 conn->m_rsdServices.size());
    return conn;
}

DeviceInfo DeviceConnection::GetDeviceInfo() {
    if (m_deviceInfoLoaded) return m_deviceInfo;

    m_deviceInfo.version = m_iosVersion;
    m_deviceInfo.protocol = m_protocol;

    ServiceConnector::ParseVersion(m_iosVersion,
                                   m_deviceInfo.versionMajor,
                                   m_deviceInfo.versionMinor,
                                   m_deviceInfo.versionPatch);

    // Get UDID and device name
    if (m_device) {
        lockdownd_client_t lockdown = m_lockdown;
        bool owns_lockdown = false;

        // Create temporary lockdown if not already provided
        // NOTE: For remote devices, caller should provide lockdown client
        if (!lockdown) {
            lockdownd_error_t lerr = lockdownd_client_new_with_handshake(m_device, &lockdown, "libinstruments");
            if (lerr != LOCKDOWN_E_SUCCESS) {
                lockdown = nullptr;
            }
            owns_lockdown = true;
        }

        if (lockdown) {
            // UDID
            char* udid = nullptr;
            idevice_get_udid(m_device, &udid);
            if (udid) {
                m_deviceInfo.udid = udid;
                free(udid);
            }

            // Device name
            plist_t nameNode = nullptr;
            if (lockdownd_get_value(lockdown, nullptr, "DeviceName", &nameNode) == LOCKDOWN_E_SUCCESS && nameNode) {
                char* name = nullptr;
                plist_get_string_val(nameNode, &name);
                if (name) {
                    m_deviceInfo.name = name;
                    plist_mem_free(name);
                }
                plist_free(nameNode);
            }

            if (owns_lockdown) {
                lockdownd_client_free(lockdown);
            }
        }
    }

    m_deviceInfoLoaded = true;
    return m_deviceInfo;
}

std::unique_ptr<DTXConnection> DeviceConnection::CreateInstrumentConnection() {
    // iOS 18+ USB QUIC tunnel path
    if (m_isTunnel && m_isUsbQuic && m_quicTunnel) {
#ifdef INSTRUMENTS_HAS_QUIC
        auto* qt = static_cast<QUICTunnel*>(m_quicTunnel.get());

        std::string resolvedServiceName;
        uint16_t servicePort = 0;
        if (!TryFindInstrumentServicePort(m_rsdServices, resolvedServiceName, servicePort)) {
            std::string available = BuildServiceListDebugString(m_rsdServices);
            INST_LOG_ERROR(TAG, "CreateInstrumentConnection (USB QUIC): no compatible Instruments service found "
                          "(%zu services available): %s", m_rsdServices.size(), available.c_str());
            return nullptr;
        }
        INST_LOG_INFO(TAG, "CreateInstrumentConnection (USB QUIC): using service '%s' at [%s]:%u",
                     resolvedServiceName.c_str(), m_tunnelAddress.c_str(), servicePort);

        int fd = qt->CreateTunnelSocket(m_tunnelAddress, servicePort);
        if (fd < 0) {
            INST_LOG_ERROR(TAG, "CreateInstrumentConnection (USB QUIC): CreateTunnelSocket failed");
            return nullptr;
        }

        auto conn = DTXConnection::CreateFromFd(fd);
        if (!conn) {
            INST_LOG_ERROR(TAG, "CreateInstrumentConnection (USB QUIC): failed to create DTX connection");
            return nullptr;
        }

        Error connectErr = conn->Connect();
        if (connectErr != Error::Success) {
            INST_LOG_ERROR(TAG, "CreateInstrumentConnection (USB QUIC): DTX handshake failed: %s",
                          ErrorToString(connectErr));
            return nullptr;
        }

        return conn;
#else
        INST_LOG_ERROR(TAG, "CreateInstrumentConnection (USB QUIC): INSTRUMENTS_HAS_QUIC not enabled");
        return nullptr;
#endif
    }

    // iOS 18+/26+ direct USB-NCM TCP path: plain IPv6 TCP to RSD-discovered service port.
    // Activated by TryDirectNCMConnection() — no tunnel, no QUIC, no libusb.
    if (m_isDirectNCM) {
        std::string resolvedServiceName;
        uint16_t servicePort = 0;
        if (!TryFindInstrumentServicePort(m_rsdServices, resolvedServiceName, servicePort)) {
            std::string available = BuildServiceListDebugString(m_rsdServices);
            INST_LOG_ERROR(TAG, "CreateInstrumentConnection (direct NCM): no compatible service "
                          "(%zu available): %s", m_rsdServices.size(), available.c_str());
            return nullptr;
        }
        INST_LOG_INFO(TAG, "CreateInstrumentConnection (direct NCM): service '%s' at [%s]:%u",
                     resolvedServiceName.c_str(), m_ncmAddress.c_str(), servicePort);

        int fd = ConnectIPv6WithTimeout(m_ncmAddress, m_ncmScopeId, servicePort, 3000);
        if (fd < 0) {
            INST_LOG_ERROR(TAG, "CreateInstrumentConnection (direct NCM): connect to "
                          "[%s]:%u failed", m_ncmAddress.c_str(), servicePort);
            return nullptr;
        }

        auto conn = DTXConnection::CreateFromFd(fd);
        if (!conn) {
            INST_LOG_ERROR(TAG, "CreateInstrumentConnection (direct NCM): CreateFromFd failed");
            return nullptr;
        }
        Error connectErr = conn->Connect();
        if (connectErr != Error::Success) {
            INST_LOG_ERROR(TAG, "CreateInstrumentConnection (direct NCM): DTX handshake failed: %s",
                          ErrorToString(connectErr));
            return nullptr;
        }
        return conn;
    }

    // iOS 17+ USB RSD path: connect to the RSD-discovered service port via idevice_connect.
    // Uses usbmuxd TCP forwarding — no tunnel address or SSL needed.
    if (m_isTunnel && m_isUsbRsd) {
        std::string resolvedServiceName;
        uint16_t servicePort = 0;
        if (!TryFindInstrumentServicePort(m_rsdServices, resolvedServiceName, servicePort)) {
            std::string available = BuildServiceListDebugString(m_rsdServices);
            INST_LOG_ERROR(TAG, "CreateInstrumentConnection (USB RSD): no compatible Instruments service found "
                          "(%zu services available): %s", m_rsdServices.size(), available.c_str());
            return nullptr;
        }
        INST_LOG_INFO(TAG, "CreateInstrumentConnection (USB RSD): connecting to port %u for %s",
                     servicePort, resolvedServiceName.c_str());

        // iOS 18+/26+ uses m_networkDevice (NETWORK-type, direct TCP to USB-NCM IPv6).
        // iOS 17 uses m_device (USB-type, usbmuxd port-forwarding).
        idevice_t connDevice = m_networkDevice ? m_networkDevice : m_device;
        idevice_connection_t idevConn = nullptr;
        idevice_error_t ierr = idevice_connect(connDevice, servicePort, &idevConn);
        if (ierr != IDEVICE_E_SUCCESS || !idevConn) {
            INST_LOG_ERROR(TAG, "CreateInstrumentConnection (USB RSD): idevice_connect to "
                          "port %u failed: %d", servicePort, ierr);
            return nullptr;
        }

        // DTXConnection takes ownership of idevConn (no SSL for iOS 17+ RSD)
        auto dtx = DTXConnection::Create(idevConn, false);
        if (!dtx) {
            idevice_disconnect(idevConn);
            INST_LOG_ERROR(TAG, "CreateInstrumentConnection (USB RSD): failed to create DTX connection");
            return nullptr;
        }

        Error connectErr = dtx->Connect();
        if (connectErr != Error::Success) {
            INST_LOG_ERROR(TAG, "CreateInstrumentConnection (USB RSD): DTX handshake failed: %s",
                          ErrorToString(connectErr));
            return nullptr;
        }

        return dtx;
    }

    // iOS 17+ external tunnel path: connect via raw TCP using RSD-discovered port
    if (m_isTunnel) {
        std::string resolvedServiceName;
        uint16_t servicePort = 0;
        if (!TryFindInstrumentServicePort(m_rsdServices, resolvedServiceName, servicePort)) {
            std::string available = BuildServiceListDebugString(m_rsdServices);
            INST_LOG_ERROR(TAG, "CreateInstrumentConnection: no compatible Instruments service found in RSD map "
                          "(%zu services available): %s", m_rsdServices.size(), available.c_str());
            return nullptr;
        }
        INST_LOG_INFO(TAG, "CreateInstrumentConnection (tunnel): connecting to [%s]:%u for %s",
                     m_tunnelAddress.c_str(), servicePort, resolvedServiceName.c_str());

        auto conn = DTXConnection::CreateFromTCP(m_tunnelAddress, servicePort);
        if (!conn) {
            INST_LOG_ERROR(TAG, "CreateInstrumentConnection (tunnel): TCP connect failed");
            return nullptr;
        }

        Error connectErr = conn->Connect();
        if (connectErr != Error::Success) {
            INST_LOG_ERROR(TAG, "CreateInstrumentConnection (tunnel): DTX handshake failed: %s",
                          ErrorToString(connectErr));
            return nullptr;
        }

        return conn;
    }

    // Standard USB/network path (iOS 12-16)
    lockdownd_service_descriptor_t service = nullptr;
    IOSProtocol protocol;
    Error err = ServiceConnector::StartInstrumentService(m_device, &service, &protocol, m_lockdown);
    if (err != Error::Success || !service) {
        if (m_protocol == IOSProtocol::RSD) {
            int major = 0, minor = 0, patch = 0;
            ServiceConnector::ParseVersion(m_iosVersion, major, minor, patch);
            if (major > 17) {
                INST_LOG_ERROR(TAG, "iOS %s requires an external CoreDevice tunnel.\n"
                              "  go-ios:          ios tunnel start --udid=<UDID>\n"
                              "  pymobiledevice3: python3 -m pymobiledevice3 remote start-tunnel\n"
                              "Then use: Instruments::CreateFromTunnel(address, rsdPort, \"%s\")",
                              m_iosVersion.c_str(), m_iosVersion.c_str());
            } else {
                INST_LOG_ERROR(TAG, "Failed to start instrument service — iOS 17+ requires a tunnel connection. "
                              "Use DeviceConnection::FromTunnel() or Instruments::CreateFromTunnel().");
            }
        } else {
            INST_LOG_ERROR(TAG, "Failed to start instrument service");
        }
        return nullptr;
    }

    std::string serviceName = ServiceConnector::GetInstrumentServiceName(protocol);
    bool sslHandshakeOnly = ServiceConnector::NeedsSSLHandshakeOnly(serviceName) && service->ssl_enabled;

    INST_LOG_DEBUG(TAG, "Creating DTX connection: service=%s, sslHandshakeOnly=%d, ssl_enabled=%d",
                  serviceName.c_str(), sslHandshakeOnly ? 1 : 0, service->ssl_enabled ? 1 : 0);

    auto conn = DTXConnection::Create(m_device, service, sslHandshakeOnly);
    lockdownd_service_descriptor_free(service);

    if (!conn) {
        INST_LOG_ERROR(TAG, "Failed to create DTX connection");
        return nullptr;
    }

    Error connectErr = conn->Connect();
    if (connectErr != Error::Success) {
        INST_LOG_ERROR(TAG, "Failed to connect DTX: %s", ErrorToString(connectErr));
        return nullptr;
    }

    return conn;
}

std::unique_ptr<DTXConnection> DeviceConnection::CreateServiceConnection(const std::string& serviceName) {
    lockdownd_service_descriptor_t service = nullptr;
    Error err = ServiceConnector::StartService(m_device, serviceName, &service, m_lockdown);
    if (err != Error::Success || !service) {
        return nullptr;
    }

    bool sslHandshakeOnly = ServiceConnector::NeedsSSLHandshakeOnly(serviceName) && service->ssl_enabled;
    auto conn = DTXConnection::Create(m_device, service, sslHandshakeOnly);
    lockdownd_service_descriptor_free(service);

    if (!conn) return nullptr;

    Error connectErr = conn->Connect();
    if (connectErr != Error::Success) return nullptr;

    return conn;
}

Error DeviceConnection::StartService(const std::string& serviceId,
                                     lockdownd_service_descriptor_t* outService) {
    return ServiceConnector::StartService(m_device, serviceId, outService, m_lockdown);
}

} // namespace instruments
