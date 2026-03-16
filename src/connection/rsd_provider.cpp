#include "rsd_provider.h"
#include "http2_framer.h"
#include "../util/log.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
using rsd_socket_t = SOCKET;
#define RSD_SOCKET_INVALID INVALID_SOCKET
#define RSD_CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
using rsd_socket_t = int;
#define RSD_SOCKET_INVALID (-1)
#define RSD_CLOSE_SOCKET ::close
#endif

namespace instruments {

static const char* TAG = "RSDProvider";

RSDProvider::RSDProvider() = default;
RSDProvider::~RSDProvider() = default;

Error RSDProvider::Connect(const std::string& tunnelAddress, uint16_t rsdPort) {
    INST_LOG_WARN(TAG, "Direct RSD handshake requires UserspaceNetwork. "
                 "Use Connect(address, port, network) instead.");
    return Error::NotSupported;
}

Error RSDProvider::Connect(const std::string& tunnelAddress, uint16_t rsdPort,
                           UserspaceNetwork* network) {
    if (!network || !network->IsInitialized()) {
        INST_LOG_ERROR(TAG, "UserspaceNetwork not initialized");
        return Error::InvalidArgument;
    }

    INST_LOG_INFO(TAG, "Connecting to RSD at [%s]:%u", tunnelAddress.c_str(), rsdPort);

    // Step 1: Create TCP connection through the tunnel
    std::shared_ptr<UserspaceTcpConnection> tcpConn;
    Error err = network->TcpConnect(tunnelAddress, rsdPort, tcpConn);
    if (err != Error::Success) {
        INST_LOG_ERROR(TAG, "Failed to create TCP connection to RSD");
        return err;
    }

    // Wait for connection to establish (lwIP TCP handshake)
    std::mutex connMutex;
    std::condition_variable connCv;
    bool connected = false;
    bool connError = false;

    tcpConn->SetErrorCallback([&](Error) {
        std::lock_guard<std::mutex> lock(connMutex);
        connError = true;
        connCv.notify_one();
    });

    // Poll the network until connected or timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!tcpConn->IsConnected() && !connError) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "TCP connection to RSD timed out");
            return Error::Timeout;
        }
        network->Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (connError || !tcpConn->IsConnected()) {
        INST_LOG_ERROR(TAG, "TCP connection to RSD failed");
        return Error::ConnectionFailed;
    }

    INST_LOG_INFO(TAG, "TCP connected to RSD");
    m_tcpConn = tcpConn;

    // Step 2: Accumulate received data
    std::mutex recvMutex;
    std::vector<uint8_t> recvBuffer;
    bool recvDone = false;

    tcpConn->SetRecvCallback([&](const uint8_t* data, size_t length) {
        std::lock_guard<std::mutex> lock(recvMutex);
        recvBuffer.insert(recvBuffer.end(), data, data + length);
    });

    tcpConn->SetErrorCallback([&](Error) {
        std::lock_guard<std::mutex> lock(recvMutex);
        recvDone = true;
    });

    // Step 3: Send HTTP/2 connection preface
    auto preface = Http2Framer::MakeConnectionPreface();
    err = tcpConn->Send(preface.data(), preface.size());
    if (err != Error::Success) {
        INST_LOG_ERROR(TAG, "Failed to send HTTP/2 preface");
        return err;
    }

    // Also send a window update for connection-level flow control
    auto windowUpdate = Http2Framer::MakeWindowUpdateFrame(0, 1048576);
    tcpConn->Send(windowUpdate.data(), windowUpdate.size());

    INST_LOG_DEBUG(TAG, "Sent HTTP/2 connection preface");

    // Step 4: Wait for server SETTINGS
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool gotServerSettings = false;

    while (!gotServerSettings && !recvDone) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "Timeout waiting for server SETTINGS");
            return Error::Timeout;
        }

        network->Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Try to decode frames
        std::lock_guard<std::mutex> lock(recvMutex);
        size_t offset = 0;
        while (offset < recvBuffer.size()) {
            H2Frame frame;
            size_t consumed = Http2Framer::DecodeFrame(
                recvBuffer.data() + offset, recvBuffer.size() - offset, frame);
            if (consumed == 0) break;
            offset += consumed;

            if (frame.type == H2FrameType::Settings && !(frame.flags & H2Flags::Ack)) {
                INST_LOG_DEBUG(TAG, "Received server SETTINGS");
                // Send SETTINGS ACK
                auto ack = Http2Framer::MakeSettingsFrame(true);
                tcpConn->Send(ack.data(), ack.size());
                gotServerSettings = true;
            }
        }
        if (offset > 0) {
            recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + offset);
        }
    }

    if (!gotServerSettings) {
        INST_LOG_ERROR(TAG, "Failed to receive server SETTINGS");
        return Error::ProtocolError;
    }

    // Step 5: Send XPC InitHandshake on stream 1 (client-initiated bidi)
    XPCMessage initMsg;
    initMsg.flags = XPCFlags::AlwaysSet | XPCFlags::InitHandshake;
    initMsg.messageId = 1;

    auto xpcData = initMsg.Encode();

    // Send as HTTP/2 DATA frame on stream 1
    // First send empty HEADERS to open the stream
    auto headers = Http2Framer::MakeHeadersFrame(1, {}, false);
    tcpConn->Send(headers.data(), headers.size());

    auto dataFrame = Http2Framer::MakeDataFrame(1, xpcData.data(), xpcData.size(), false);
    err = tcpConn->Send(dataFrame.data(), dataFrame.size());
    if (err != Error::Success) {
        INST_LOG_ERROR(TAG, "Failed to send XPC InitHandshake");
        return err;
    }

    INST_LOG_DEBUG(TAG, "Sent XPC InitHandshake on stream 1");

    // Step 6: Receive the XPC response (service discovery)
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool gotXPCResponse = false;

    while (!gotXPCResponse && !recvDone) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "Timeout waiting for XPC response");
            return Error::Timeout;
        }

        network->Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Try to decode frames
        std::lock_guard<std::mutex> lock(recvMutex);
        size_t offset = 0;
        while (offset < recvBuffer.size()) {
            H2Frame frame;
            size_t consumed = Http2Framer::DecodeFrame(
                recvBuffer.data() + offset, recvBuffer.size() - offset, frame);
            if (consumed == 0) break;
            offset += consumed;

            if (frame.type == H2FrameType::Data && !frame.payload.empty()) {
                // Try to decode as XPC message
                XPCMessage xpcResp;
                if (XPCMessage::Decode(frame.payload, xpcResp)) {
                    INST_LOG_INFO(TAG, "Received XPC response (flags=0x%x msgId=%llu)",
                                 xpcResp.flags, (unsigned long long)xpcResp.messageId);

                    if (xpcResp.body.IsDict()) {
                        ParseServiceResponse(xpcResp.body);
                        gotXPCResponse = true;
                    }
                }
            } else if (frame.type == H2FrameType::Settings) {
                if (!(frame.flags & H2Flags::Ack)) {
                    auto ack = Http2Framer::MakeSettingsFrame(true);
                    tcpConn->Send(ack.data(), ack.size());
                }
            } else if (frame.type == H2FrameType::WindowUpdate) {
                // Acknowledge window updates silently
            }
        }
        if (offset > 0) {
            recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + offset);
        }
    }

    if (!gotXPCResponse) {
        INST_LOG_ERROR(TAG, "Failed to receive XPC service discovery response");
        return Error::ProtocolError;
    }

    INST_LOG_INFO(TAG, "RSD handshake complete: UDID=%s, %zu services discovered",
                 m_udid.c_str(), m_services.size());

    return Error::Success;
}

Error RSDProvider::DoRSDHandshake(
    std::function<bool(const std::vector<uint8_t>&)> sendFn,
    std::function<bool(std::vector<uint8_t>&)> recvMoreFn)
{
    std::vector<uint8_t> recvBuf;

    // Step 1: Send HTTP/2 connection preface + window update
    if (!sendFn(Http2Framer::MakeConnectionPreface())) {
        INST_LOG_ERROR(TAG, "RSD: failed to send HTTP/2 preface");
        return Error::ConnectionFailed;
    }
    sendFn(Http2Framer::MakeWindowUpdateFrame(0, 1048576));
    INST_LOG_DEBUG(TAG, "RSD: sent HTTP/2 preface");

    // Step 2: Wait for server SETTINGS
    bool gotServerSettings = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (!gotServerSettings) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "RSD: timeout waiting for server SETTINGS");
            return Error::Timeout;
        }
        if (!recvMoreFn(recvBuf)) {
            INST_LOG_ERROR(TAG, "RSD: connection closed waiting for SETTINGS");
            return Error::ConnectionFailed;
        }
        size_t offset = 0;
        while (offset < recvBuf.size()) {
            H2Frame frame;
            size_t consumed = Http2Framer::DecodeFrame(recvBuf.data() + offset,
                                                        recvBuf.size() - offset, frame);
            if (consumed == 0) break;
            offset += consumed;
            if (frame.type == H2FrameType::Settings && !(frame.flags & H2Flags::Ack)) {
                sendFn(Http2Framer::MakeSettingsFrame(true));
                gotServerSettings = true;
            }
        }
        if (offset > 0) recvBuf.erase(recvBuf.begin(), recvBuf.begin() + offset);
    }

    // Step 3: Send XPC InitHandshake on stream 1
    XPCMessage initMsg;
    initMsg.flags = XPCFlags::AlwaysSet | XPCFlags::InitHandshake;
    initMsg.messageId = 1;
    auto xpcData = initMsg.Encode();

    sendFn(Http2Framer::MakeHeadersFrame(1, {}, false));
    if (!sendFn(Http2Framer::MakeDataFrame(1, xpcData.data(), xpcData.size(), false))) {
        INST_LOG_ERROR(TAG, "RSD: failed to send XPC InitHandshake");
        return Error::ConnectionFailed;
    }
    INST_LOG_DEBUG(TAG, "RSD: sent XPC InitHandshake on stream 1");

    // Step 4: Receive XPC service discovery response
    bool gotXPCResponse = false;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (!gotXPCResponse) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "RSD: timeout waiting for XPC response");
            return Error::Timeout;
        }
        if (!recvMoreFn(recvBuf)) {
            INST_LOG_ERROR(TAG, "RSD: connection closed waiting for XPC response");
            return Error::ProtocolError;
        }
        size_t offset = 0;
        while (offset < recvBuf.size()) {
            H2Frame frame;
            size_t consumed = Http2Framer::DecodeFrame(recvBuf.data() + offset,
                                                        recvBuf.size() - offset, frame);
            if (consumed == 0) break;
            offset += consumed;

            if (frame.type == H2FrameType::Data && !frame.payload.empty()) {
                XPCMessage xpcResp;
                if (XPCMessage::Decode(frame.payload, xpcResp)) {
                    INST_LOG_INFO(TAG, "RSD: received XPC response (flags=0x%x msgId=%llu)",
                                 xpcResp.flags, (unsigned long long)xpcResp.messageId);
                    if (xpcResp.body.IsDict()) {
                        ParseServiceResponse(xpcResp.body);
                        gotXPCResponse = true;
                    }
                }
            } else if (frame.type == H2FrameType::Settings && !(frame.flags & H2Flags::Ack)) {
                sendFn(Http2Framer::MakeSettingsFrame(true));
            }
        }
        if (offset > 0) recvBuf.erase(recvBuf.begin(), recvBuf.begin() + offset);
    }

    if (!gotXPCResponse) {
        INST_LOG_ERROR(TAG, "RSD: no XPC service response received");
        return Error::ProtocolError;
    }

    INST_LOG_INFO(TAG, "RSD handshake complete: UDID=%s, %zu services",
                 m_udid.c_str(), m_services.size());
    return Error::Success;
}

Error RSDProvider::ConnectDirect(const std::string& address, uint16_t rsdPort) {
    INST_LOG_INFO(TAG, "ConnectDirect: connecting to RSD at [%s]:%u", address.c_str(), rsdPort);

#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

    char portStr[8];
    std::snprintf(portStr, sizeof(portStr), "%u", rsdPort);

    struct addrinfo hints = {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo* res = nullptr;
    if (getaddrinfo(address.c_str(), portStr, &hints, &res) != 0 || !res) {
        INST_LOG_ERROR(TAG, "ConnectDirect: getaddrinfo failed for [%s]:%u", address.c_str(), rsdPort);
        return Error::ConnectionFailed;
    }

    rsd_socket_t sock = RSD_SOCKET_INVALID;
    for (auto* rp = res; rp != nullptr; rp = rp->ai_next) {
        sock = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock == RSD_SOCKET_INVALID) continue;
        if (::connect(sock, rp->ai_addr, static_cast<int>(rp->ai_addrlen)) == 0) break;
        RSD_CLOSE_SOCKET(sock);
        sock = RSD_SOCKET_INVALID;
    }
    freeaddrinfo(res);

    if (sock == RSD_SOCKET_INVALID) {
        INST_LOG_ERROR(TAG, "ConnectDirect: TCP connect failed to [%s]:%u", address.c_str(), rsdPort);
        return Error::ConnectionFailed;
    }

    INST_LOG_INFO(TAG, "ConnectDirect: TCP connected to RSD");

    auto sendFn = [&](const std::vector<uint8_t>& data) -> bool {
        size_t sent = 0;
        while (sent < data.size()) {
#ifdef _WIN32
            int n = ::send(sock, reinterpret_cast<const char*>(data.data() + sent),
                           static_cast<int>(data.size() - sent), 0);
#else
            ssize_t n = ::send(sock, data.data() + sent, data.size() - sent, 0);
#endif
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    };

    auto recvMoreFn = [&](std::vector<uint8_t>& buf) -> bool {
        uint8_t tmp[4096];
#ifdef _WIN32
        int n = ::recv(sock, reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
#else
        ssize_t n = ::recv(sock, tmp, sizeof(tmp), 0);
#endif
        if (n <= 0) return false;
        buf.insert(buf.end(), tmp, tmp + n);
        return true;
    };

    Error err = DoRSDHandshake(sendFn, recvMoreFn);
    RSD_CLOSE_SOCKET(sock);

    if (err == Error::Success) {
        INST_LOG_INFO(TAG, "ConnectDirect: complete — UDID=%s, %zu services",
                     m_udid.c_str(), m_services.size());
    }
    return err;
}

Error RSDProvider::ConnectViaIDevice(idevice_t device, uint16_t rsdPort) {
    INST_LOG_INFO(TAG, "ConnectViaIDevice: connecting to RSD port %u via USB (usbmuxd)", rsdPort);

    idevice_connection_t conn = nullptr;
    idevice_error_t ierr = idevice_connect(device, rsdPort, &conn);
    if (ierr != IDEVICE_E_SUCCESS || !conn) {
        INST_LOG_ERROR(TAG, "ConnectViaIDevice: idevice_connect to port %u failed: %d", rsdPort, ierr);
        return Error::ConnectionFailed;
    }

    INST_LOG_INFO(TAG, "ConnectViaIDevice: USB TCP connected to RSD port %u", rsdPort);

    auto sendFn = [&](const std::vector<uint8_t>& data) -> bool {
        const char* ptr = reinterpret_cast<const char*>(data.data());
        size_t remaining = data.size();
        while (remaining > 0) {
            uint32_t sent = 0;
            idevice_error_t e = idevice_connection_send(
                conn, ptr, static_cast<uint32_t>(remaining), &sent);
            if (e != IDEVICE_E_SUCCESS || sent == 0) return false;
            ptr += sent;
            remaining -= sent;
        }
        return true;
    };

    auto recvMoreFn = [&](std::vector<uint8_t>& buf) -> bool {
        char tmp[4096];
        uint32_t received = 0;
        idevice_error_t e = idevice_connection_receive_timeout(
            conn, tmp, sizeof(tmp), &received, 5000);
        if (e != IDEVICE_E_SUCCESS || received == 0) return false;
        buf.insert(buf.end(), tmp, tmp + received);
        return true;
    };

    Error err = DoRSDHandshake(sendFn, recvMoreFn);
    idevice_disconnect(conn);

    if (err == Error::Success) {
        INST_LOG_INFO(TAG, "ConnectViaIDevice: complete — UDID=%s, %zu services",
                     m_udid.c_str(), m_services.size());
    }
    return err;
}

Error RSDProvider::ConnectViaFd(int socketFd) {
    INST_LOG_INFO(TAG, "ConnectViaFd: doing RSD handshake on pre-connected socket");

    auto sendFn = [socketFd](const std::vector<uint8_t>& data) -> bool {
        size_t sent = 0;
        while (sent < data.size()) {
#ifdef _WIN32
            int n = ::send(static_cast<SOCKET>(socketFd),
                           reinterpret_cast<const char*>(data.data() + sent),
                           static_cast<int>(data.size() - sent), 0);
#else
            ssize_t n = ::send(socketFd, data.data() + sent, data.size() - sent, 0);
#endif
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    };

    auto recvMoreFn = [socketFd](std::vector<uint8_t>& buf) -> bool {
        uint8_t tmp[4096];
#ifdef _WIN32
        int n = ::recv(static_cast<SOCKET>(socketFd),
                       reinterpret_cast<char*>(tmp), sizeof(tmp), 0);
#else
        ssize_t n = ::recv(socketFd, tmp, sizeof(tmp), 0);
#endif
        if (n <= 0) return false;
        buf.insert(buf.end(), tmp, tmp + static_cast<size_t>(n));
        return true;
    };

    return DoRSDHandshake(sendFn, recvMoreFn);
}

void RSDProvider::ParseServiceResponse(const NSObject& body) {
    // The XPC response body is a dictionary containing:
    // "Properties" → dict with device info including "UniqueDeviceID"
    // "Services" → dict mapping service name → {"Port": <int>}

    if (body.HasKey("Properties")) {
        const auto& props = body["Properties"];
        if (props.IsDict() && props.HasKey("UniqueDeviceID")) {
            m_udid = props["UniqueDeviceID"].AsString();
            INST_LOG_INFO(TAG, "Device UDID: %s", m_udid.c_str());
        }
    }

    if (body.HasKey("Services")) {
        const auto& services = body["Services"];
        if (services.IsDict()) {
            for (const auto& [name, value] : services.AsDict()) {
                RSDServiceEntry entry;
                entry.name = name;
                if (value.IsDict() && value.HasKey("Port")) {
                    entry.port = static_cast<uint16_t>(value["Port"].AsInt64());
                }
                if (entry.port > 0) {
                    m_services[name] = entry;
                    INST_LOG_DEBUG(TAG, "  Service: %s -> port %u", name.c_str(), entry.port);
                }
            }
        }
    }
}

uint16_t RSDProvider::FindServicePort(const std::string& serviceName) const {
    auto it = m_services.find(serviceName);
    if (it != m_services.end()) {
        return it->second.port;
    }
    return 0;
}

uint16_t RSDProvider::FindServicePortWithShim(const std::string& serviceName) const {
    // First try exact match
    uint16_t port = FindServicePort(serviceName);
    if (port > 0) return port;

    // Try with .shim.remote suffix (iOS 17+ shim services)
    std::string shimName = serviceName + ".shim.remote";
    return FindServicePort(shimName);
}

} // namespace instruments
