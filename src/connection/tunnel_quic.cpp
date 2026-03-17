#include "tunnel_quic.h"
#include "../util/log.h"
#include <cctype>
#include <cstring>

#ifdef INSTRUMENTS_HAS_QUIC

#include <picoquic.h>
#include <picoquic_utils.h>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
using socket_t = SOCKET;
#define SOCKET_INVALID INVALID_SOCKET
#define CLOSE_SOCKET closesocket
#define SOCKET_ERROR_CODE WSAGetLastError()
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
using socket_t = int;
#define SOCKET_INVALID (-1)
#define CLOSE_SOCKET ::close
#define SOCKET_ERROR_CODE errno
#endif

#include <plist/plist.h>
#include <chrono>
#include <condition_variable>
#include <future>

namespace instruments {

static const char* TAG = "QUICTunnel";

static bool ConnectIDeviceWithTimeout(idevice_t device, uint16_t port,
                                      std::chrono::milliseconds timeout,
                                      idevice_connection_t* outConn,
                                      idevice_error_t* outErr) {
    if (!outConn || !outErr) return false;

    std::packaged_task<std::pair<idevice_error_t, idevice_connection_t>()> task(
        [device, port]() {
            idevice_connection_t conn = nullptr;
            idevice_error_t err = idevice_connect(device, port, &conn);
            return std::make_pair(err, conn);
        });

    auto fut = task.get_future();
    std::thread worker(std::move(task));

    if (fut.wait_for(timeout) != std::future_status::ready) {
        // idevice_connect/usbmuxd_connect can block indefinitely on some setups.
        // Detach the worker and fail fast so the app can continue fallback logic.
        worker.detach();
        *outConn = nullptr;
        *outErr = IDEVICE_E_TIMEOUT;
        return false;
    }

    auto result = fut.get();
    worker.join();
    *outErr = result.first;
    *outConn = result.second;
    return (result.first == IDEVICE_E_SUCCESS && result.second != nullptr);
}

static void SetIDeviceRecvTimeoutMs(idevice_connection_t conn, int timeoutMs) {
    if (!conn || timeoutMs < 0) return;
    int sockFd = -1;
    idevice_connection_get_fd(conn, &sockFd);
    if (sockFd < 0) return;
#ifdef _WIN32
    DWORD tmoMs = static_cast<DWORD>(timeoutMs);
    setsockopt(static_cast<SOCKET>(sockFd), SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tmoMs), sizeof(tmoMs));
#else
    struct timeval tmo = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    setsockopt(sockFd, SOL_SOCKET, SO_RCVTIMEO, &tmo, sizeof(tmo));
#endif
}

// Internal context passed to picoquic callback
struct QUICCallbackContext {
    QUICTunnel* tunnel = nullptr;

    // Stream 0 handshake state
    bool handshakeComplete = false;
    bool handshakeFailed = false;
    std::vector<uint8_t> streamRecvBuffer;
    bool streamFinReceived = false;

    // Datagram receive
    bool connectionReady = false;
};

QUICTunnel::QUICTunnel() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
    std::memset(&m_fakeLocalAddr, 0, sizeof(m_fakeLocalAddr));
    std::memset(&m_fakeRemoteAddr, 0, sizeof(m_fakeRemoteAddr));
}

QUICTunnel::~QUICTunnel() {
    Close();
#ifdef _WIN32
    WSACleanup();
#endif
}

int QUICTunnel::QuicCallback(picoquic_cnx_t* cnx, uint64_t stream_id,
                              uint8_t* bytes, size_t length,
                              picoquic_call_back_event_t fin_or_event,
                              void* callback_ctx, void* stream_ctx) {
    auto* ctx = static_cast<QUICCallbackContext*>(callback_ctx);
    if (!ctx || !ctx->tunnel) return -1;

    auto event = fin_or_event;

    switch (event) {
    case picoquic_callback_stream_data:
    case picoquic_callback_stream_fin:
        // Stream data received (handshake response on stream 0)
        if (bytes && length > 0) {
            ctx->streamRecvBuffer.insert(ctx->streamRecvBuffer.end(),
                                         bytes, bytes + length);
        }
        if (event == picoquic_callback_stream_fin) {
            ctx->streamFinReceived = true;
        }
        break;

    case picoquic_callback_ready:
        INST_LOG_INFO(TAG, "QUIC connection ready");
        ctx->connectionReady = true;
        break;

    case picoquic_callback_almost_ready:
        INST_LOG_DEBUG(TAG, "QUIC connection almost ready");
        break;

    case picoquic_callback_datagram: {
        // Incoming datagram = IPv6 packet from device -> inject into lwIP
        if (bytes && length > 0) {
            ctx->tunnel->m_network.InjectPacket(bytes, length);
        }
        break;
    }

    case picoquic_callback_prepare_datagram: {
        // picoquic asks us to provide a datagram to send
        std::lock_guard<std::mutex> lock(ctx->tunnel->m_dgMutex);
        if (!ctx->tunnel->m_outgoingDatagrams.empty()) {
            auto& dgram = ctx->tunnel->m_outgoingDatagrams.front();
            if (dgram.size() <= length) {
                uint8_t* buf = picoquic_provide_datagram_buffer_ex(
                    bytes, dgram.size(), picoquic_datagram_active_any_path);
                if (buf) {
                    std::memcpy(buf, dgram.data(), dgram.size());
                }
            }
            ctx->tunnel->m_outgoingDatagrams.erase(
                ctx->tunnel->m_outgoingDatagrams.begin());
        } else {
            // Nothing to send - mark inactive
            picoquic_provide_datagram_buffer_ex(
                bytes, 0, picoquic_datagram_not_active);
        }
        break;
    }

    case picoquic_callback_datagram_acked:
    case picoquic_callback_datagram_lost:
    case picoquic_callback_datagram_spurious:
        // Datagram delivery notifications - ignore for now
        break;

    case picoquic_callback_close:
    case picoquic_callback_application_close:
        INST_LOG_WARN(TAG, "QUIC connection closed");
        ctx->tunnel->m_active.store(false);
        break;

    case picoquic_callback_stateless_reset:
        INST_LOG_WARN(TAG, "QUIC stateless reset");
        ctx->tunnel->m_active.store(false);
        break;

    default:
        break;
    }

    return 0;
}

Error QUICTunnel::CreateUdpSocket(const struct sockaddr* addr, socklen_t addrLen) {
    int af = addr->sa_family;
    socket_t sock = socket(af, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == SOCKET_INVALID) {
        INST_LOG_ERROR(TAG, "Failed to create UDP socket: %d", SOCKET_ERROR_CODE);
        return Error::ConnectionFailed;
    }

    // Set non-blocking
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(sock, FIONBIO, &mode);
#else
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
#endif

    // Bind to any address (let OS assign port)
    if (af == AF_INET6) {
        struct sockaddr_in6 bindAddr = {};
        bindAddr.sin6_family = AF_INET6;
        bindAddr.sin6_addr = in6addr_any;
        bindAddr.sin6_port = 0;
        if (bind(sock, reinterpret_cast<struct sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
            INST_LOG_ERROR(TAG, "Failed to bind UDP socket: %d", SOCKET_ERROR_CODE);
            CLOSE_SOCKET(sock);
            return Error::ConnectionFailed;
        }
    } else {
        struct sockaddr_in bindAddr = {};
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_addr.s_addr = INADDR_ANY;
        bindAddr.sin_port = 0;
        if (bind(sock, reinterpret_cast<struct sockaddr*>(&bindAddr), sizeof(bindAddr)) != 0) {
            INST_LOG_ERROR(TAG, "Failed to bind UDP socket: %d", SOCKET_ERROR_CODE);
            CLOSE_SOCKET(sock);
            return Error::ConnectionFailed;
        }
    }

    m_udpSocket = static_cast<int>(sock);
    return Error::Success;
}

Error QUICTunnel::Connect(const std::string& address, uint16_t tunnelPort) {
    INST_LOG_INFO(TAG, "Connecting QUIC tunnel to [%s]:%u", address.c_str(), tunnelPort);

    if (address.empty() || tunnelPort == 0) {
        INST_LOG_ERROR(TAG, "Invalid address or port");
        return Error::InvalidArgument;
    }

    // Resolve the destination address
    struct sockaddr_storage destAddr = {};
    socklen_t destAddrLen = 0;

    struct sockaddr_in6* addr6 = reinterpret_cast<struct sockaddr_in6*>(&destAddr);
    addr6->sin6_family = AF_INET6;
    addr6->sin6_port = htons(tunnelPort);
    if (inet_pton(AF_INET6, address.c_str(), &addr6->sin6_addr) != 1) {
        INST_LOG_ERROR(TAG, "Invalid IPv6 address: %s", address.c_str());
        return Error::InvalidArgument;
    }
    destAddrLen = sizeof(struct sockaddr_in6);

    // Create UDP socket
    Error err = CreateUdpSocket(reinterpret_cast<struct sockaddr*>(&destAddr), destAddrLen);
    if (err != Error::Success) return err;

    // Create callback context
    auto* cbCtx = new QUICCallbackContext();
    cbCtx->tunnel = this;

    // Create picoquic QUIC context
    uint64_t currentTime = picoquic_current_time();
    m_quic = picoquic_create(
        8,          // max connections
        nullptr,    // cert file (client mode, no server cert needed)
        nullptr,    // key file
        nullptr,    // cert root (we'll skip verification)
        "quic-tunnel",  // default ALPN
        QuicCallback,
        cbCtx,
        nullptr,    // cnx_id callback
        nullptr,    // cnx_id callback data
        nullptr,    // reset seed
        currentTime,
        nullptr,    // simulated time (use wall time)
        nullptr,    // ticket file
        nullptr,    // ticket encryption key
        0           // ticket encryption key length
    );

    if (!m_quic) {
        INST_LOG_ERROR(TAG, "Failed to create QUIC context");
        delete cbCtx;
        CLOSE_SOCKET(static_cast<socket_t>(m_udpSocket));
        m_udpSocket = -1;
        return Error::InternalError;
    }

    // Skip TLS certificate verification (iOS tunnel uses self-signed certs)
    picoquic_set_null_verifier(m_quic);

    // Configure transport parameters for datagram support
    picoquic_tp_t tp = {};
    tp.max_datagram_frame_size = 1500;
    tp.initial_max_data = 1024 * 1024;
    tp.initial_max_stream_data_bidi_local = 256 * 1024;
    tp.initial_max_stream_data_bidi_remote = 256 * 1024;
    tp.initial_max_stream_data_uni = 256 * 1024;
    tp.initial_max_stream_id_bidir = 100;
    tp.initial_max_stream_id_unidir = 100;
    tp.max_idle_timeout = 30000000; // 30 seconds in microseconds
    tp.max_packet_size = PICOQUIC_MAX_PACKET_SIZE;
    tp.active_connection_id_limit = 4;
    picoquic_set_default_tp(m_quic, &tp);

    // Create client connection
    m_cnx = picoquic_create_client_cnx(
        m_quic,
        reinterpret_cast<struct sockaddr*>(&destAddr),
        currentTime,
        0,              // preferred version (0 = latest)
        "quic-tunnel",  // SNI
        "quic-tunnel",  // ALPN
        QuicCallback,
        cbCtx
    );

    if (!m_cnx) {
        INST_LOG_ERROR(TAG, "Failed to create QUIC connection");
        picoquic_free(m_quic);
        m_quic = nullptr;
        delete cbCtx;
        CLOSE_SOCKET(static_cast<socket_t>(m_udpSocket));
        m_udpSocket = -1;
        return Error::InternalError;
    }

    // Start the QUIC client handshake
    if (picoquic_start_client_cnx(m_cnx) != 0) {
        INST_LOG_ERROR(TAG, "Failed to start QUIC client handshake");
        picoquic_free(m_quic);
        m_quic = nullptr;
        delete cbCtx;
        CLOSE_SOCKET(static_cast<socket_t>(m_udpSocket));
        m_udpSocket = -1;
        return Error::ConnectionFailed;
    }

    INST_LOG_INFO(TAG, "QUIC handshake started, waiting for connection...");

    // Run the QUIC event loop until connected or timeout
    uint8_t sendBuffer[PICOQUIC_MAX_PACKET_SIZE];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (!cbCtx->connectionReady) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "QUIC connection timeout");
            picoquic_free(m_quic);
            m_quic = nullptr;
            m_cnx = nullptr;
            delete cbCtx;
            CLOSE_SOCKET(static_cast<socket_t>(m_udpSocket));
            m_udpSocket = -1;
            return Error::Timeout;
        }

        currentTime = picoquic_current_time();

        // Prepare outgoing packets
        struct sockaddr_storage addrTo = {}, addrFrom = {};
        int ifIndex = 0;
        size_t sendLength = 0;
        picoquic_connection_id_t logCid = {};
        picoquic_cnx_t* lastCnx = nullptr;

        int ret = picoquic_prepare_next_packet(m_quic, currentTime,
            sendBuffer, sizeof(sendBuffer), &sendLength,
            &addrTo, &addrFrom, &ifIndex, &logCid, &lastCnx);

        if (ret != 0) {
            INST_LOG_ERROR(TAG, "picoquic_prepare_next_packet failed: %d", ret);
            break;
        }

        if (sendLength > 0) {
            sendto(static_cast<socket_t>(m_udpSocket), reinterpret_cast<const char*>(sendBuffer),
                   static_cast<int>(sendLength), 0,
                   reinterpret_cast<struct sockaddr*>(&addrTo),
                   static_cast<int>(destAddrLen));
        }

        // Receive incoming packets
        uint8_t recvBuffer[PICOQUIC_MAX_PACKET_SIZE];
        struct sockaddr_storage recvFrom = {};
        socklen_t fromLen = sizeof(recvFrom);

#ifdef _WIN32
        int recvLen = recvfrom(static_cast<socket_t>(m_udpSocket),
            reinterpret_cast<char*>(recvBuffer), sizeof(recvBuffer), 0,
            reinterpret_cast<struct sockaddr*>(&recvFrom), &fromLen);
#else
        ssize_t recvLen = recvfrom(m_udpSocket,
            recvBuffer, sizeof(recvBuffer), 0,
            reinterpret_cast<struct sockaddr*>(&recvFrom), &fromLen);
#endif

        if (recvLen > 0) {
            currentTime = picoquic_current_time();
            picoquic_incoming_packet(m_quic, recvBuffer, static_cast<size_t>(recvLen),
                reinterpret_cast<struct sockaddr*>(&recvFrom),
                reinterpret_cast<struct sockaddr*>(&destAddr),
                0, 0, currentTime);
        } else {
            // No data available - sleep briefly to avoid busy loop
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        // Check for connection failure
        picoquic_state_enum state = picoquic_get_cnx_state(m_cnx);
        if (state == picoquic_state_disconnected ||
            state == picoquic_state_handshake_failure ||
            state == picoquic_state_handshake_failure_resend) {
            INST_LOG_ERROR(TAG, "QUIC handshake failed (state=%d)", static_cast<int>(state));
            picoquic_free(m_quic);
            m_quic = nullptr;
            m_cnx = nullptr;
            delete cbCtx;
            CLOSE_SOCKET(static_cast<socket_t>(m_udpSocket));
            m_udpSocket = -1;
            return Error::ConnectionFailed;
        }
    }

    INST_LOG_INFO(TAG, "QUIC connection established, performing tunnel handshake...");

    // Perform tunnel parameter handshake on stream 0
    err = PerformHandshake();
    if (err != Error::Success) {
        INST_LOG_ERROR(TAG, "Tunnel handshake failed");
        picoquic_free(m_quic);
        m_quic = nullptr;
        m_cnx = nullptr;
        delete cbCtx;
        CLOSE_SOCKET(static_cast<socket_t>(m_udpSocket));
        m_udpSocket = -1;
        return err;
    }

    // Initialize userspace network with tunnel parameters
    err = m_network.Init(m_params.clientAddress, m_params.serverAddress, m_params.mtu);
    if (err != Error::Success) {
        INST_LOG_ERROR(TAG, "Failed to initialize userspace network");
        picoquic_free(m_quic);
        m_quic = nullptr;
        m_cnx = nullptr;
        delete cbCtx;
        CLOSE_SOCKET(static_cast<socket_t>(m_udpSocket));
        m_udpSocket = -1;
        return err;
    }

    // Set output callback: lwIP packets -> QUIC datagrams
    m_network.SetOutputCallback([this](const uint8_t* data, size_t len) {
        {
            std::lock_guard<std::mutex> lock(m_dgMutex);
            m_outgoingDatagrams.emplace_back(data, data + len);
        }
        // Signal picoquic that datagrams are ready
        if (m_cnx) {
            picoquic_mark_datagram_ready(m_cnx, 1);
        }
    });

    // Start forwarding thread
    m_active.store(true);
    m_forwardThread = std::thread([this]() { ForwardLoop(); });

    INST_LOG_INFO(TAG, "QUIC tunnel active: client=%s server=%s rsdPort=%u",
                 m_params.clientAddress.c_str(), m_params.serverAddress.c_str(),
                 m_params.serverRSDPort);

    return Error::Success;
}

// ---------- ConnectViaCoreDeviceProxy (CDTunnel - iOS 17.4+, iOS 18+/26+) ----------

Error QUICTunnel::ConnectViaCoreDeviceProxy(idevice_t device) {
    INST_LOG_INFO(TAG, "ConnectViaCoreDeviceProxy: starting CoreDeviceProxy service");

    // 1. Start lockdown service com.apple.internal.devicecompute.CoreDeviceProxy
    lockdownd_client_t lockdown = nullptr;
    if (lockdownd_client_new_with_handshake(device, &lockdown, "libinstruments")
            != LOCKDOWN_E_SUCCESS) {
        INST_LOG_ERROR(TAG, "ConnectViaCoreDeviceProxy: lockdown client failed");
        return Error::ConnectionFailed;
    }

    lockdownd_service_descriptor_t svcDesc = nullptr;
    lockdownd_error_t lerr = lockdownd_start_service(
        lockdown, "com.apple.internal.devicecompute.CoreDeviceProxy", &svcDesc);
    lockdownd_client_free(lockdown);

    if (lerr != LOCKDOWN_E_SUCCESS || !svcDesc) {
        INST_LOG_DEBUG(TAG, "ConnectViaCoreDeviceProxy: service not available (err=%d) — "
                      "device may be iOS <17.4 or not trusted", lerr);
        return Error::ConnectionFailed;
    }

    bool needSsl = (svcDesc->ssl_enabled != 0);
    uint16_t port = svcDesc->port;
    lockdownd_service_descriptor_free(svcDesc);

    INST_LOG_INFO(TAG, "ConnectViaCoreDeviceProxy: started on port %u (ssl=%d)", port, (int)needSsl);

    // 2. Connect to service port
    INST_LOG_DEBUG(TAG, "ConnectViaCoreDeviceProxy: connecting to service port %u...", port);
    idevice_error_t ierr = IDEVICE_E_UNKNOWN_ERROR;
    if (!ConnectIDeviceWithTimeout(device, port, std::chrono::seconds(8), &m_idevConn, &ierr)) {
        if (ierr == IDEVICE_E_TIMEOUT) {
            INST_LOG_ERROR(TAG, "ConnectViaCoreDeviceProxy: idevice_connect timed out on port %u", port);
        } else {
            INST_LOG_ERROR(TAG, "ConnectViaCoreDeviceProxy: idevice_connect failed: %d", ierr);
        }
        return Error::ConnectionFailed;
    }
    if (ierr != IDEVICE_E_SUCCESS || !m_idevConn) {
        INST_LOG_ERROR(TAG, "ConnectViaCoreDeviceProxy: idevice_connect failed: %d", ierr);
        return Error::ConnectionFailed;
    }

    // 3. Enable SSL if the service descriptor requires it
    if (needSsl) {
        INST_LOG_INFO(TAG, "ConnectViaCoreDeviceProxy: starting SSL handshake...");
        ierr = idevice_connection_enable_ssl(m_idevConn);
        if (ierr != IDEVICE_E_SUCCESS) {
            INST_LOG_ERROR(TAG, "ConnectViaCoreDeviceProxy: SSL handshake failed: %d", ierr);
            idevice_disconnect(m_idevConn);
            m_idevConn = nullptr;
            return Error::ConnectionFailed;
        }
        INST_LOG_INFO(TAG, "ConnectViaCoreDeviceProxy: SSL handshake complete");

        // Set socket-level receive timeout so SSL_read() doesn't block indefinitely.
        // idevice_connection_receive_timeout() ignores its timeout for SSL connections
        // because SSL_read() is blocking — SO_RCVTIMEO fixes this at the OS level.
        SetIDeviceRecvTimeoutMs(m_idevConn, 3000);
        INST_LOG_DEBUG(TAG, "ConnectViaCoreDeviceProxy: SO_RCVTIMEO=3000ms set for handshake");
    }

    // 4. CDTunnel handshake
    Error err = PerformCDTunnelHandshake();
    if (err != Error::Success) {
        idevice_disconnect(m_idevConn);
        m_idevConn = nullptr;
        return err;
    }

    // After handshake, keep read timeouts short so ForwardLoop can continue
    // draining lwIP output (SYN/data) instead of stalling in SSL_read for seconds.
    SetIDeviceRecvTimeoutMs(m_idevConn, 50);
    INST_LOG_DEBUG(TAG, "ConnectViaCoreDeviceProxy: SO_RCVTIMEO=50ms set for runtime forwarding");

    // 5. Initialize lwIP network with tunnel parameters
    err = m_network.Init(m_params.clientAddress, m_params.serverAddress, m_params.mtu);
    if (err != Error::Success) {
        INST_LOG_ERROR(TAG, "ConnectViaCoreDeviceProxy: failed to init userspace network");
        idevice_disconnect(m_idevConn);
        m_idevConn = nullptr;
        return err;
    }

    // 6. Set output callback: lwIP -> CDTunnel output queue (ForwardLoop sends it)
    m_network.SetOutputCallback([this](const uint8_t* data, size_t len) {
        std::lock_guard<std::mutex> lock(m_cdOutputMutex);
        m_cdOutputQueue.emplace_back(data, data + len);
    });

    m_isCDTunnel = true;

    // 7. Start forwarding thread
    m_active.store(true);
    m_forwardThread = std::thread([this]() { ForwardLoop(); });

    INST_LOG_INFO(TAG, "ConnectViaCoreDeviceProxy: CDTunnel active: client=%s server=%s rsd=%u",
                 m_params.clientAddress.c_str(), m_params.serverAddress.c_str(),
                 m_params.serverRSDPort);

    return Error::Success;
}

// ---------- PerformCDTunnelHandshake ----------

Error QUICTunnel::PerformCDTunnelHandshake() {
    INST_LOG_INFO(TAG, "CDTunnel: starting handshake");

    // Request: "CDTunnel\0" (9 bytes) + 1-byte JSON length + JSON
    const char* reqJson = "{\"type\":\"clientHandshakeRequest\",\"mtu\":1280}";
    const size_t jsonLen = std::strlen(reqJson);

    uint8_t reqBuf[256];
    size_t pos = 0;
    std::memcpy(reqBuf + pos, "CDTunnel", 8); pos += 8;
    reqBuf[pos++] = 0x00;  // null terminator of magic
    reqBuf[pos++] = static_cast<uint8_t>(jsonLen);
    std::memcpy(reqBuf + pos, reqJson, jsonLen); pos += jsonLen;

    uint32_t sent = 0;
    idevice_error_t sendErr = idevice_connection_send(
        m_idevConn, reinterpret_cast<const char*>(reqBuf),
        static_cast<uint32_t>(pos), &sent);
    if (sendErr != IDEVICE_E_SUCCESS || sent != static_cast<uint32_t>(pos)) {
        INST_LOG_ERROR(TAG, "CDTunnel: failed to send handshake request (err=%d sent=%u of %u)",
                       sendErr, sent, (unsigned)pos);
        return Error::ConnectionFailed;
    }
    INST_LOG_INFO(TAG, "CDTunnel: request sent (%u bytes), waiting for response...", sent);

    // Response: "CDTunnel" (8 bytes) + 1 unknown byte + 1 length byte = 10 bytes header
    // Then <length> bytes of JSON body
    uint8_t respHeader[10];
    size_t totalRecv = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    while (totalRecv < sizeof(respHeader)) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "CDTunnel: handshake response header timeout");
            return Error::Timeout;
        }
        uint32_t recvd = 0;
        idevice_connection_receive_timeout(m_idevConn,
            reinterpret_cast<char*>(respHeader + totalRecv),
            static_cast<uint32_t>(sizeof(respHeader) - totalRecv),
            &recvd, 1000);
        totalRecv += recvd;
    }

    // Verify magic
    if (std::memcmp(respHeader, "CDTunnel", 8) != 0) {
        INST_LOG_ERROR(TAG, "CDTunnel: invalid response magic");
        return Error::ProtocolError;
    }

    // go-ios uses header[9] as body length; iOS 26.x appears to put the length
    // at header[8] instead (with header[9]=0 being a type/version byte).
    // Try [9] first; if zero, fall back to [8] as a single-byte length.
    uint32_t bodyLen = static_cast<uint32_t>(respHeader[9]);
    if (bodyLen == 0 && respHeader[8] != 0) {
        bodyLen = static_cast<uint32_t>(respHeader[8]);
        INST_LOG_DEBUG(TAG, "CDTunnel: byte[9]=0, using byte[8]=%u as bodyLen", bodyLen);
    }
    if (bodyLen == 0) {
        INST_LOG_ERROR(TAG, "CDTunnel: handshake response has zero body length (header[8]=%02x [9]=%02x)",
                       respHeader[8], respHeader[9]);
        return Error::ProtocolError;
    }

    std::vector<uint8_t> jsonBody(bodyLen);
    totalRecv = 0;
    while (totalRecv < bodyLen) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "CDTunnel: handshake response body timeout");
            return Error::Timeout;
        }
        uint32_t recvd = 0;
        idevice_connection_receive_timeout(m_idevConn,
            reinterpret_cast<char*>(jsonBody.data() + totalRecv),
            static_cast<uint32_t>(bodyLen - totalRecv),
            &recvd, 1000);
        totalRecv += recvd;
    }

    // Log header bytes and JSON for debugging
    INST_LOG_DEBUG(TAG, "CDTunnel response header[8]=%02x [9]=%02x bodyLen=%u",
                  respHeader[8], respHeader[9], (unsigned)bodyLen);
    INST_LOG_INFO(TAG, "CDTunnel response JSON: %.*s",
                  static_cast<int>(bodyLen), jsonBody.data());

    // Parse JSON response with libplist
    plist_t root = nullptr;
    plist_from_json(reinterpret_cast<const char*>(jsonBody.data()),
                    static_cast<uint32_t>(bodyLen), &root);
    if (!root) {
        INST_LOG_ERROR(TAG, "CDTunnel: failed to parse handshake JSON");
        return Error::ProtocolError;
    }

    // Log root plist type for debugging
    plist_type rootType = plist_get_node_type(root);
    INST_LOG_DEBUG(TAG, "CDTunnel: plist root type=%d (DICT=5)", (int)rootType);

    // Helper: case-insensitive plist dict key lookup — iOS versions vary casing
    auto dictGetCI = [](plist_t dict, const char* key) -> plist_t {
        if (!dict || plist_get_node_type(dict) != PLIST_DICT) return nullptr;
        // Try exact match first
        plist_t node = plist_dict_get_item(dict, key);
        if (node) return node;
        // Fall back to case-insensitive scan
        plist_dict_iter it = nullptr;
        plist_dict_new_iter(dict, &it);
        if (!it) return nullptr;
        char* iterKey = nullptr;
        plist_t iterVal = nullptr;
        plist_t found = nullptr;
        while (true) {
            plist_dict_next_item(dict, it, &iterKey, &iterVal);
            if (!iterKey) break;
            bool match = true;
            const char* a = key; const char* b = iterKey;
            while (*a && *b) {
                if (std::tolower((unsigned char)*a) != std::tolower((unsigned char)*b)) {
                    match = false; break;
                }
                ++a; ++b;
            }
            if (match && *a == '\0' && *b == '\0') {
                found = iterVal;
                plist_mem_free(iterKey);
                break;
            }
            plist_mem_free(iterKey);
        }
        plist_mem_free(it);
        return found;
    };

    // Log all top-level keys at DEBUG level so we can diagnose key name mismatches
    {
        plist_dict_iter it = nullptr;
        plist_dict_new_iter(root, &it);
        if (it) {
            char* k = nullptr; plist_t v = nullptr;
            while (true) {
                plist_dict_next_item(root, it, &k, &v);
                if (!k) break;
                INST_LOG_DEBUG(TAG, "CDTunnel JSON key: '%s'", k);
                plist_mem_free(k);
            }
            plist_mem_free(it);
        }
    }

    // Extract ServerAddress (try PascalCase, then camelCase via CI helper)
    plist_t serverAddrNode = dictGetCI(root, "ServerAddress");
    if (serverAddrNode) {
        char* addr = nullptr;
        plist_get_string_val(serverAddrNode, &addr);
        if (addr) { m_params.serverAddress = addr; plist_mem_free(addr); }
    }

    // Extract ServerRSDPort
    plist_t rsdPortNode = dictGetCI(root, "ServerRSDPort");
    if (rsdPortNode) {
        uint64_t p = 0;
        plist_get_uint_val(rsdPortNode, &p);
        m_params.serverRSDPort = static_cast<uint16_t>(p);
    }

    // Extract ClientParameters.Address and Mtu
    plist_t clientParams = dictGetCI(root, "ClientParameters");
    if (clientParams) {
        plist_t addrNode = dictGetCI(clientParams, "Address");
        if (addrNode) {
            char* addr = nullptr;
            plist_get_string_val(addrNode, &addr);
            if (addr) { m_params.clientAddress = addr; plist_mem_free(addr); }
        }
        plist_t mtuNode = dictGetCI(clientParams, "Mtu");
        if (mtuNode) {
            uint64_t mtu = 0;
            plist_get_uint_val(mtuNode, &mtu);
            if (mtu > 0) m_params.mtu = static_cast<uint32_t>(mtu);
        }
    }
    plist_free(root);

    if (m_params.clientAddress.empty() || m_params.serverAddress.empty()) {
        INST_LOG_ERROR(TAG, "CDTunnel: handshake response missing addresses");
        return Error::ProtocolError;
    }
    if (m_params.serverRSDPort == 0) m_params.serverRSDPort = 58783;

    INST_LOG_INFO(TAG, "CDTunnel handshake complete: client=%s server=%s rsd=%u mtu=%u",
                 m_params.clientAddress.c_str(), m_params.serverAddress.c_str(),
                 m_params.serverRSDPort, m_params.mtu);

    return Error::Success;
}

// ---------- ConnectViaUSB (QUIC - iOS 17.x lockdown path) ----------

Error QUICTunnel::ConnectViaUSB(idevice_t device) {
    INST_LOG_INFO(TAG, "ConnectViaUSB: starting CoreDevice tunnel service");

    // iOS 18+/26 naming varies by build / entitlement profile. Try a small
    // compatibility set before failing.
    static const char* COREDEVICE_SERVICES[] = {
        "com.apple.internal.dt.coredevice.free.tunnelservice",
        "com.apple.internal.dt.coredevice.untrusted.tunnelservice",
        "com.apple.coredevice.tunnelservice",
        "com.apple.coredevice.untrusted.tunnelservice",
        nullptr
    };

    // 1. Start lockdown service
    lockdownd_client_t lockdown = nullptr;
    if (lockdownd_client_new_with_handshake(device, &lockdown, "libinstruments")
            != LOCKDOWN_E_SUCCESS) {
        INST_LOG_ERROR(TAG, "ConnectViaUSB: lockdown client failed");
        return Error::ConnectionFailed;
    }

    lockdownd_service_descriptor_t svcDesc = nullptr;
    lockdownd_error_t lerr = LOCKDOWN_E_INVALID_SERVICE;
    const char* selectedService = nullptr;
    for (int i = 0; COREDEVICE_SERVICES[i] != nullptr; ++i) {
        const char* candidate = COREDEVICE_SERVICES[i];
        lerr = lockdownd_start_service(lockdown, candidate, &svcDesc);
        if (lerr == LOCKDOWN_E_SUCCESS && svcDesc) {
            selectedService = candidate;
            break;
        }
        INST_LOG_DEBUG(TAG, "ConnectViaUSB: start_service(%s) failed: %d", candidate, lerr);
    }
    lockdownd_client_free(lockdown);

    if (lerr != LOCKDOWN_E_SUCCESS || !svcDesc || !selectedService) {
        INST_LOG_ERROR(TAG, "ConnectViaUSB: failed to start CoreDevice tunnel service (last error: %d)", lerr);
        return Error::ConnectionFailed;
    }
    INST_LOG_INFO(TAG, "ConnectViaUSB: started %s on port %u", selectedService, svcDesc->port);

    // 2. Connect to service port
    idevice_error_t ierr = IDEVICE_E_UNKNOWN_ERROR;
    if (!ConnectIDeviceWithTimeout(device, svcDesc->port, std::chrono::seconds(8), &m_idevConn, &ierr)) {
        if (ierr == IDEVICE_E_TIMEOUT) {
            INST_LOG_ERROR(TAG, "ConnectViaUSB: idevice_connect timed out on port %u", svcDesc->port);
        } else {
            INST_LOG_ERROR(TAG, "ConnectViaUSB: idevice_connect failed: %d", ierr);
        }
        lockdownd_service_descriptor_free(svcDesc);
        return Error::ConnectionFailed;
    }
    lockdownd_service_descriptor_free(svcDesc);

    if (ierr != IDEVICE_E_SUCCESS || !m_idevConn) {
        INST_LOG_ERROR(TAG, "ConnectViaUSB: idevice_connect failed: %d", ierr);
        return Error::ConnectionFailed;
    }

    m_isUsb = true;

    // 3. Set up fake addresses for picoquic (stream transport needs nominal addrs)
    auto* local6 = reinterpret_cast<struct sockaddr_in6*>(&m_fakeLocalAddr);
    auto* remote6 = reinterpret_cast<struct sockaddr_in6*>(&m_fakeRemoteAddr);
    local6->sin6_family = AF_INET6;
    local6->sin6_port = htons(12345);
    inet_pton(AF_INET6, "::1", &local6->sin6_addr);
    remote6->sin6_family = AF_INET6;
    remote6->sin6_port = htons(58784);
    inet_pton(AF_INET6, "::2", &remote6->sin6_addr);

    // 4. Create picoquic context
    auto* cbCtx = new QUICCallbackContext();
    cbCtx->tunnel = this;

    uint64_t currentTime = picoquic_current_time();
    m_quic = picoquic_create(8, nullptr, nullptr, nullptr, "quic-tunnel",
                             QuicCallback, cbCtx, nullptr, nullptr, nullptr,
                             currentTime, nullptr, nullptr, nullptr, 0);
    if (!m_quic) {
        INST_LOG_ERROR(TAG, "ConnectViaUSB: failed to create QUIC context");
        delete cbCtx;
        idevice_disconnect(m_idevConn);
        m_idevConn = nullptr;
        m_isUsb = false;
        return Error::InternalError;
    }

    picoquic_set_null_verifier(m_quic);

    // Configure transport params (same as Connect())
    picoquic_tp_t tp = {};
    tp.max_datagram_frame_size = 1500;
    tp.initial_max_data = 1024 * 1024;
    tp.initial_max_stream_data_bidi_local = 256 * 1024;
    tp.initial_max_stream_data_bidi_remote = 256 * 1024;
    tp.initial_max_stream_data_uni = 256 * 1024;
    tp.initial_max_stream_id_bidir = 100;
    tp.initial_max_stream_id_unidir = 100;
    tp.max_idle_timeout = 30000000;
    tp.max_packet_size = PICOQUIC_MAX_PACKET_SIZE;
    tp.active_connection_id_limit = 4;
    picoquic_set_default_tp(m_quic, &tp);

    m_cnx = picoquic_create_client_cnx(m_quic,
        reinterpret_cast<struct sockaddr*>(&m_fakeRemoteAddr),
        currentTime, 0, "quic-tunnel", "quic-tunnel",
        QuicCallback, cbCtx);

    if (!m_cnx) {
        INST_LOG_ERROR(TAG, "ConnectViaUSB: failed to create QUIC connection");
        picoquic_free(m_quic);
        m_quic = nullptr;
        delete cbCtx;
        idevice_disconnect(m_idevConn);
        m_idevConn = nullptr;
        m_isUsb = false;
        return Error::InternalError;
    }

    if (picoquic_start_client_cnx(m_cnx) != 0) {
        INST_LOG_ERROR(TAG, "ConnectViaUSB: failed to start QUIC handshake");
        picoquic_free(m_quic);
        m_quic = nullptr;
        m_cnx = nullptr;
        delete cbCtx;
        idevice_disconnect(m_idevConn);
        m_idevConn = nullptr;
        m_isUsb = false;
        return Error::ConnectionFailed;
    }

    INST_LOG_INFO(TAG, "ConnectViaUSB: QUIC handshake started over USB stream...");

    // 5. QUIC connection loop over USB stream I/O
    uint8_t sendBuf[PICOQUIC_MAX_PACKET_SIZE];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);

    while (!cbCtx->connectionReady) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "ConnectViaUSB: QUIC connection timeout");
            picoquic_free(m_quic);
            m_quic = nullptr;
            m_cnx = nullptr;
            delete cbCtx;
            idevice_disconnect(m_idevConn);
            m_idevConn = nullptr;
            m_isUsb = false;
            return Error::Timeout;
        }

        currentTime = picoquic_current_time();

        struct sockaddr_storage addrTo = {}, addrFrom = {};
        int ifIndex = 0;
        size_t sendLength = 0;
        picoquic_connection_id_t logCid = {};
        picoquic_cnx_t* lastCnx = nullptr;

        picoquic_prepare_next_packet(m_quic, currentTime,
            sendBuf, sizeof(sendBuf), &sendLength,
            &addrTo, &addrFrom, &ifIndex, &logCid, &lastCnx);

        if (sendLength > 0) {
            SendFramed(sendBuf, sendLength);
        }

        uint8_t recvBuf[PICOQUIC_MAX_PACKET_SIZE];
        int pktLen = RecvFramedPartial(recvBuf, sizeof(recvBuf));
        if (pktLen > 0) {
            currentTime = picoquic_current_time();
            picoquic_incoming_packet(m_quic, recvBuf, (size_t)pktLen,
                reinterpret_cast<struct sockaddr*>(&m_fakeRemoteAddr),
                reinterpret_cast<struct sockaddr*>(&m_fakeLocalAddr),
                0, 0, currentTime);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        picoquic_state_enum state = picoquic_get_cnx_state(m_cnx);
        if (state == picoquic_state_disconnected ||
            state == picoquic_state_handshake_failure ||
            state == picoquic_state_handshake_failure_resend) {
            INST_LOG_ERROR(TAG, "ConnectViaUSB: QUIC handshake failed (state=%d)", (int)state);
            picoquic_free(m_quic);
            m_quic = nullptr;
            m_cnx = nullptr;
            delete cbCtx;
            idevice_disconnect(m_idevConn);
            m_idevConn = nullptr;
            m_isUsb = false;
            return Error::ConnectionFailed;
        }
    }

    INST_LOG_INFO(TAG, "ConnectViaUSB: QUIC connected, performing tunnel handshake...");

    // 6. Tunnel parameter handshake (reuses PerformHandshake with m_isUsb flag)
    Error err = PerformHandshake();
    if (err != Error::Success) {
        picoquic_free(m_quic);
        m_quic = nullptr;
        m_cnx = nullptr;
        delete cbCtx;
        idevice_disconnect(m_idevConn);
        m_idevConn = nullptr;
        m_isUsb = false;
        return err;
    }

    // 7. Initialize lwIP network
    err = m_network.Init(m_params.clientAddress, m_params.serverAddress, m_params.mtu);
    if (err != Error::Success) {
        picoquic_free(m_quic);
        m_quic = nullptr;
        m_cnx = nullptr;
        delete cbCtx;
        idevice_disconnect(m_idevConn);
        m_idevConn = nullptr;
        m_isUsb = false;
        return err;
    }

    // Set output callback: lwIP packets -> QUIC datagrams
    m_network.SetOutputCallback([this](const uint8_t* data, size_t len) {
        {
            std::lock_guard<std::mutex> lock(m_dgMutex);
            m_outgoingDatagrams.emplace_back(data, data + len);
        }
        if (m_cnx) {
            picoquic_mark_datagram_ready(m_cnx, 1);
        }
    });

    // 8. Start forwarding thread
    m_active.store(true);
    m_forwardThread = std::thread([this]() { ForwardLoop(); });

    INST_LOG_INFO(TAG, "ConnectViaUSB: tunnel active: client=%s server=%s rsd=%u",
                 m_params.clientAddress.c_str(), m_params.serverAddress.c_str(),
                 m_params.serverRSDPort);

    return Error::Success;
}

// ---------- SendFramed / RecvFramedPartial ----------
// These are the primary I/O functions used by PerformHandshake() and ForwardLoop().
// They use idevice TCP stream framing (iOS 17.x USB lockdown path).

Error QUICTunnel::SendFramed(const uint8_t* buf, size_t len) {
    uint32_t lenBE = htonl(static_cast<uint32_t>(len));
    uint8_t header[4];
    std::memcpy(header, &lenBE, 4);

    uint32_t sent = 0;
    idevice_error_t err = idevice_connection_send(m_idevConn,
        reinterpret_cast<const char*>(header), 4, &sent);
    if (err != IDEVICE_E_SUCCESS || sent != 4) {
        INST_LOG_ERROR(TAG, "SendFramed: header send failed (err=%d sent=%u)", err, sent);
        return Error::ConnectionFailed;
    }

    sent = 0;
    uint32_t remaining = static_cast<uint32_t>(len);
    const uint8_t* ptr = buf;
    while (remaining > 0) {
        err = idevice_connection_send(m_idevConn,
            reinterpret_cast<const char*>(ptr), remaining, &sent);
        if (err != IDEVICE_E_SUCCESS || sent == 0) {
            INST_LOG_ERROR(TAG, "SendFramed: data send failed (err=%d sent=%u)", err, sent);
            return Error::ConnectionFailed;
        }
        ptr += sent;
        remaining -= sent;
    }
    return Error::Success;
}

int QUICTunnel::RecvFramedPartial(uint8_t* outBuf, size_t maxLen) {
    // Try to receive more data (non-blocking: timeout=0)
    uint8_t tmp[4096];
    uint32_t received = 0;
    idevice_error_t err = idevice_connection_receive_timeout(
        m_idevConn, reinterpret_cast<char*>(tmp), sizeof(tmp), &received, 0);
    if (err == IDEVICE_E_SUCCESS && received > 0) {
        m_streamRecvBuf.insert(m_streamRecvBuf.end(), tmp, tmp + received);
    }

    if (m_streamRecvBuf.size() < 4) return 0;

    uint32_t pktLen = 0;
    std::memcpy(&pktLen, m_streamRecvBuf.data(), 4);
    pktLen = ntohl(pktLen);

    if (m_streamRecvBuf.size() < 4 + pktLen) return 0;
    if (pktLen > maxLen) {
        // Discard oversized packet
        m_streamRecvBuf.erase(m_streamRecvBuf.begin(),
                              m_streamRecvBuf.begin() + 4 + pktLen);
        return 0;
    }

    std::memcpy(outBuf, m_streamRecvBuf.data() + 4, pktLen);
    m_streamRecvBuf.erase(m_streamRecvBuf.begin(),
                          m_streamRecvBuf.begin() + 4 + pktLen);
    return static_cast<int>(pktLen);
}

// ---------- MakeLoopbackPair ----------

int QUICTunnel::MakeLoopbackPair(int fds[2]) {
#ifdef _WIN32
    SOCKET listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) return -1;

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(listener, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        closesocket(listener);
        return -1;
    }
    ::listen(listener, 1);
    int addrLen = sizeof(addr);
    ::getsockname(listener, reinterpret_cast<struct sockaddr*>(&addr), &addrLen);

    SOCKET client = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET) {
        closesocket(listener);
        return -1;
    }
    ::connect(client, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));

    SOCKET server = ::accept(listener, nullptr, nullptr);
    closesocket(listener);
    if (server == INVALID_SOCKET) {
        closesocket(client);
        return -1;
    }

    fds[0] = static_cast<int>(client);
    fds[1] = static_cast<int>(server);
    return 0;
#else
    return ::socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
#endif
}

// ---------- SubmitToLoop / DrainTasks / DrainBridges ----------

void QUICTunnel::SubmitToLoop(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(m_taskMutex);
    m_pendingTasks.push_back(std::move(fn));
}

void QUICTunnel::DrainTasks() {
    std::vector<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(m_taskMutex);
        tasks.swap(m_pendingTasks);
    }
    for (auto& fn : tasks) fn();
}

void QUICTunnel::DrainBridges() {
    std::lock_guard<std::mutex> lock(m_bridgeMutex);
    for (auto& bridge : m_bridges) {
        if (!bridge->connected.load() || !bridge->lwipConn) continue;
        if (bridge->internalFd < 0) continue;

        uint8_t buf[4096];
        for (;;) {
#ifdef _WIN32
            int n = ::recv(static_cast<SOCKET>(bridge->internalFd),
                           reinterpret_cast<char*>(buf), sizeof(buf), 0);
#else
            ssize_t n = ::recv(bridge->internalFd, buf, sizeof(buf), MSG_DONTWAIT);
#endif
            if (n <= 0) break;
            bridge->lwipConn->Send(buf, static_cast<size_t>(n));
        }
    }
}

// ---------- CreateTunnelSocket ----------

int QUICTunnel::CreateTunnelSocket(const std::string& destIPv6, uint16_t port) {
    if (!m_active.load() || !m_network.IsInitialized()) {
        INST_LOG_ERROR(TAG, "CreateTunnelSocket: tunnel not active");
        return -1;
    }

    int fds[2];
    if (MakeLoopbackPair(fds) != 0) {
        INST_LOG_ERROR(TAG, "CreateTunnelSocket: MakeLoopbackPair failed");
        return -1;
    }

    // Make fds[1] (internal/bridge side) non-blocking
#ifdef _WIN32
    u_long mode = 1;
    ioctlsocket(static_cast<SOCKET>(fds[1]), FIONBIO, &mode);
#else
    int flags = fcntl(fds[1], F_GETFL, 0);
    fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);
#endif

    auto bridge = std::make_shared<SocketBridge>();
    bridge->externalFd = fds[0];
    bridge->internalFd = fds[1];

    // Shared waiter for connection notification
    struct ConnWaiter {
        std::mutex mutex;
        std::condition_variable cv;
        bool connected = false;
        bool failed = false;
    };
    auto waiter = std::make_shared<ConnWaiter>();

    SubmitToLoop([this, destIPv6, port, bridge, waiter]() {
        std::shared_ptr<UserspaceTcpConnection> lwipConn;
        Error err = m_network.TcpConnect(destIPv6, port, lwipConn);
        if (err != Error::Success) {
            std::lock_guard<std::mutex> lock(waiter->mutex);
            waiter->failed = true;
            waiter->cv.notify_one();
            return;
        }

        bridge->lwipConn = lwipConn;

        lwipConn->SetConnectedCallback([bridge, waiter](bool success) {
            bridge->connected.store(success);
            std::lock_guard<std::mutex> lock(waiter->mutex);
            if (success) waiter->connected = true;
            else waiter->failed = true;
            waiter->cv.notify_one();
        });

        int internalFd = bridge->internalFd;
        lwipConn->SetRecvCallback([internalFd](const uint8_t* data, size_t len) {
            ::send(
#ifdef _WIN32
                static_cast<SOCKET>(internalFd),
                reinterpret_cast<const char*>(data),
                static_cast<int>(len), 0
#else
                internalFd, data, len, 0
#endif
            );
        });

        lwipConn->SetErrorCallback([bridge, waiter](Error) {
            std::lock_guard<std::mutex> lock(waiter->mutex);
            if (!waiter->connected) {
                waiter->failed = true;
                waiter->cv.notify_one();
            }
        });

        {
            std::lock_guard<std::mutex> bLock(m_bridgeMutex);
            m_bridges.push_back(bridge);
        }
    });

    // Wait for connection (ForwardLoop will DrainTasks and trigger ConnectedCallback)
    std::unique_lock<std::mutex> lock(waiter->mutex);
    bool ok = waiter->cv.wait_for(lock, std::chrono::seconds(10),
        [&waiter]() { return waiter->connected || waiter->failed; });

    if (!ok || !waiter->connected || waiter->failed) {
        if (!ok) {
            INST_LOG_ERROR(TAG, "CreateTunnelSocket: connection to [%s]:%u timed out waiting for lwIP connect",
                          destIPv6.c_str(), port);
        } else {
            INST_LOG_ERROR(TAG, "CreateTunnelSocket: connection to [%s]:%u failed (lwIP error callback)",
                          destIPv6.c_str(), port);
        }
#ifdef _WIN32
        closesocket(static_cast<SOCKET>(fds[0]));
        closesocket(static_cast<SOCKET>(fds[1]));
#else
        ::close(fds[0]);
        ::close(fds[1]);
#endif
        return -1;
    }

    INST_LOG_INFO(TAG, "CreateTunnelSocket: connected to [%s]:%u -> fd=%d",
                 destIPv6.c_str(), port, fds[0]);
    return fds[0];
}

// ---------- PerformHandshake ----------

Error QUICTunnel::PerformHandshake() {
    auto* cbCtx = static_cast<QUICCallbackContext*>(picoquic_get_callback_context(m_cnx));
    if (!cbCtx) return Error::InternalError;

    // Send clientHandshakeRequest on stream 0 (client-initiated bidirectional)
    const char* request = "{\"type\":\"clientHandshakeRequest\",\"mtu\":1280}";
    size_t reqLen = std::strlen(request);

    int ret = picoquic_add_to_stream(m_cnx, 0,
        reinterpret_cast<const uint8_t*>(request), reqLen, 1 /* set_fin */);
    if (ret != 0) {
        INST_LOG_ERROR(TAG, "Failed to send handshake request: %d", ret);
        return Error::ProtocolError;
    }

    INST_LOG_DEBUG(TAG, "Sent handshake request: %s", request);

    // Drive QUIC until we get the response
    uint8_t sendBuffer[PICOQUIC_MAX_PACKET_SIZE];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);

    // For UDP mode: get destination address for sendto
    struct sockaddr_storage destAddr = {};
    socklen_t destAddrLen = sizeof(destAddr);
    if (!m_isUsb) {
        getpeername(static_cast<socket_t>(m_udpSocket),
                    reinterpret_cast<struct sockaddr*>(&destAddr), &destAddrLen);
    }

    while (!cbCtx->streamFinReceived) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "Handshake response timeout");
            return Error::Timeout;
        }

        uint64_t currentTime = picoquic_current_time();

        // Prepare and send
        struct sockaddr_storage addrTo = {}, addrFrom = {};
        int ifIndex = 0;
        size_t sendLength = 0;
        picoquic_connection_id_t logCid = {};
        picoquic_cnx_t* lastCnx = nullptr;

        ret = picoquic_prepare_next_packet(m_quic, currentTime,
            sendBuffer, sizeof(sendBuffer), &sendLength,
            &addrTo, &addrFrom, &ifIndex, &logCid, &lastCnx);

        if (ret != 0) break;

        if (sendLength > 0) {
            if (m_isUsb) {
                SendFramed(sendBuffer, sendLength);
            } else {
                sendto(static_cast<socket_t>(m_udpSocket),
                       reinterpret_cast<const char*>(sendBuffer),
                       static_cast<int>(sendLength), 0,
                       reinterpret_cast<struct sockaddr*>(&addrTo),
                       static_cast<int>(destAddrLen));
            }
        }

        // Receive
        if (m_isUsb) {
            uint8_t recvBuffer[PICOQUIC_MAX_PACKET_SIZE];
            int pktLen = RecvFramedPartial(recvBuffer, sizeof(recvBuffer));
            if (pktLen > 0) {
                currentTime = picoquic_current_time();
                picoquic_incoming_packet(m_quic, recvBuffer, (size_t)pktLen,
                    reinterpret_cast<struct sockaddr*>(&m_fakeRemoteAddr),
                    reinterpret_cast<struct sockaddr*>(&m_fakeLocalAddr),
                    0, 0, currentTime);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        } else {
            uint8_t recvBuffer[PICOQUIC_MAX_PACKET_SIZE];
            struct sockaddr_storage recvFrom = {};
            socklen_t fromLen = sizeof(recvFrom);

#ifdef _WIN32
            int recvLen = recvfrom(static_cast<socket_t>(m_udpSocket),
                reinterpret_cast<char*>(recvBuffer), sizeof(recvBuffer), 0,
                reinterpret_cast<struct sockaddr*>(&recvFrom), &fromLen);
#else
            ssize_t recvLen = recvfrom(m_udpSocket,
                recvBuffer, sizeof(recvBuffer), 0,
                reinterpret_cast<struct sockaddr*>(&recvFrom), &fromLen);
#endif

            if (recvLen > 0) {
                currentTime = picoquic_current_time();
                picoquic_incoming_packet(m_quic, recvBuffer, static_cast<size_t>(recvLen),
                    reinterpret_cast<struct sockaddr*>(&recvFrom),
                    reinterpret_cast<struct sockaddr*>(&destAddr),
                    0, 0, currentTime);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }

    // Parse the response JSON
    auto& buf = cbCtx->streamRecvBuffer;
    if (buf.empty()) {
        INST_LOG_ERROR(TAG, "Empty handshake response");
        return Error::ProtocolError;
    }

    INST_LOG_DEBUG(TAG, "Handshake response: %.*s", static_cast<int>(buf.size()), buf.data());

    // Parse with libplist (JSON -> plist -> extract fields)
    plist_t root = nullptr;
    plist_from_json(reinterpret_cast<const char*>(buf.data()),
                    static_cast<uint32_t>(buf.size()), &root);
    if (!root) {
        INST_LOG_ERROR(TAG, "Failed to parse handshake JSON");
        return Error::ProtocolError;
    }

    // Extract fields
    plist_t typeNode = plist_dict_get_item(root, "type");
    if (typeNode) {
        char* typeStr = nullptr;
        plist_get_string_val(typeNode, &typeStr);
        if (typeStr) {
            if (std::strcmp(typeStr, "serverHandshakeResponse") != 0) {
                INST_LOG_ERROR(TAG, "Unexpected response type: %s", typeStr);
                plist_mem_free(typeStr);
                plist_free(root);
                return Error::ProtocolError;
            }
            plist_mem_free(typeStr);
        }
    }

    plist_t clientParams = plist_dict_get_item(root, "clientParameters");
    if (clientParams) {
        plist_t addrNode = plist_dict_get_item(clientParams, "address");
        if (addrNode) {
            char* addr = nullptr;
            plist_get_string_val(addrNode, &addr);
            if (addr) {
                m_params.clientAddress = addr;
                plist_mem_free(addr);
            }
        }
        plist_t mtuNode = plist_dict_get_item(clientParams, "mtu");
        if (mtuNode) {
            uint64_t mtu = 0;
            plist_get_uint_val(mtuNode, &mtu);
            m_params.mtu = static_cast<uint32_t>(mtu);
        }
    }

    plist_t serverAddrNode = plist_dict_get_item(root, "serverAddress");
    if (serverAddrNode) {
        char* addr = nullptr;
        plist_get_string_val(serverAddrNode, &addr);
        if (addr) {
            m_params.serverAddress = addr;
            plist_mem_free(addr);
        }
    }

    plist_t rsdPortNode = plist_dict_get_item(root, "serverRSDPort");
    if (rsdPortNode) {
        uint64_t port = 0;
        plist_get_uint_val(rsdPortNode, &port);
        m_params.serverRSDPort = static_cast<uint16_t>(port);
    }

    plist_free(root);

    if (m_params.clientAddress.empty() || m_params.serverAddress.empty()) {
        INST_LOG_ERROR(TAG, "Handshake response missing addresses");
        return Error::ProtocolError;
    }

    if (m_params.serverRSDPort == 0) {
        m_params.serverRSDPort = 58783; // default RSD port
    }

    INST_LOG_INFO(TAG, "Tunnel handshake complete: client=%s server=%s rsd=%u mtu=%u",
                 m_params.clientAddress.c_str(), m_params.serverAddress.c_str(),
                 m_params.serverRSDPort, m_params.mtu);

    return Error::Success;
}

// ---------- ForwardLoop ----------

void QUICTunnel::ForwardLoop() {
    INST_LOG_INFO(TAG, "Forwarding thread started");

    uint8_t sendBuffer[PICOQUIC_MAX_PACKET_SIZE];
    uint8_t recvBuffer[PICOQUIC_MAX_PACKET_SIZE];

    // For UDP mode: get the peer address for incoming packet attribution
    struct sockaddr_storage destAddr = {};
    socklen_t destAddrLen = sizeof(destAddr);
    if (!m_isUsb && !m_isCDTunnel) {
        getpeername(static_cast<socket_t>(m_udpSocket),
                    reinterpret_cast<struct sockaddr*>(&destAddr), &destAddrLen);
    }

    while (m_active.load()) {
        // Process pending tasks from other threads (lwIP thread safety)
        DrainTasks();

        if (m_isCDTunnel) {
            // --- CDTunnel path: raw IPv6 packets over SSL TCP ---

            // Receive: read available bytes into recv buffer (non-blocking)
            uint8_t tmp[4096];
            uint32_t recvd = 0;
            idevice_error_t rerr = idevice_connection_receive_timeout(m_idevConn,
                reinterpret_cast<char*>(tmp), sizeof(tmp), &recvd, 20);
            if (recvd > 0) {
                m_cdRecvBuf.insert(m_cdRecvBuf.end(), tmp, tmp + recvd);
            } else if (rerr != IDEVICE_E_TIMEOUT && rerr != IDEVICE_E_SUCCESS) {
                INST_LOG_DEBUG(TAG, "CDTunnel: receive returned err=%d", static_cast<int>(rerr));
            }

            // Extract and inject complete IPv6 packets into lwIP
            while (m_cdRecvBuf.size() >= 40) {
                if ((m_cdRecvBuf[0] & 0xF0) != 0x60) {
                    // Not an IPv6 packet — stream is corrupt
                    INST_LOG_ERROR(TAG, "CDTunnel: invalid IPv6 packet (byte0=0x%02x), closing",
                                  m_cdRecvBuf[0]);
                    m_active.store(false);
                    break;
                }
                uint16_t payloadLen = (static_cast<uint16_t>(m_cdRecvBuf[4]) << 8)
                                    | static_cast<uint16_t>(m_cdRecvBuf[5]);
                size_t totalLen = 40u + payloadLen;
                if (m_cdRecvBuf.size() < totalLen) break;  // incomplete packet

                m_network.InjectPacket(m_cdRecvBuf.data(), totalLen);
                m_cdRecvBuf.erase(m_cdRecvBuf.begin(),
                                  m_cdRecvBuf.begin() + static_cast<ptrdiff_t>(totalLen));
            }

            // Send: drain output queue (lwIP -> device)
            std::vector<std::vector<uint8_t>> toSend;
            {
                std::lock_guard<std::mutex> lock(m_cdOutputMutex);
                toSend.swap(m_cdOutputQueue);
            }
            for (auto& pkt : toSend) {
                uint32_t sentBytes = 0;
                idevice_connection_send(m_idevConn,
                    reinterpret_cast<const char*>(pkt.data()),
                    static_cast<uint32_t>(pkt.size()), &sentBytes);
            }

        } else {
            // --- QUIC path (USB framed stream or UDP) ---

            uint64_t currentTime = picoquic_current_time();

            // Prepare and send outgoing QUIC packets
            bool hasSomethingToSend = true;
            while (hasSomethingToSend) {
                struct sockaddr_storage addrTo = {}, addrFrom = {};
                int ifIndex = 0;
                size_t sendLength = 0;
                picoquic_connection_id_t logCid = {};
                picoquic_cnx_t* lastCnx = nullptr;

                int ret = picoquic_prepare_next_packet(m_quic, currentTime,
                    sendBuffer, sizeof(sendBuffer), &sendLength,
                    &addrTo, &addrFrom, &ifIndex, &logCid, &lastCnx);

                if (ret != 0 || sendLength == 0) {
                    hasSomethingToSend = false;
                    break;
                }

                if (m_isUsb) {
                    SendFramed(sendBuffer, sendLength);
                } else {
                    sendto(static_cast<socket_t>(m_udpSocket),
                           reinterpret_cast<const char*>(sendBuffer),
                           static_cast<int>(sendLength), 0,
                           reinterpret_cast<struct sockaddr*>(&addrTo),
                           static_cast<int>(destAddrLen));
                }
            }

            // Receive incoming packets
            if (m_isUsb) {
                int pktLen = RecvFramedPartial(recvBuffer, sizeof(recvBuffer));
                while (pktLen > 0) {
                    currentTime = picoquic_current_time();
                    picoquic_incoming_packet(m_quic, recvBuffer, (size_t)pktLen,
                        reinterpret_cast<struct sockaddr*>(&m_fakeRemoteAddr),
                        reinterpret_cast<struct sockaddr*>(&m_fakeLocalAddr),
                        0, 0, currentTime);
                    pktLen = RecvFramedPartial(recvBuffer, sizeof(recvBuffer));
                }
            } else {
                // Receive incoming UDP packets (non-blocking)
                struct sockaddr_storage recvFrom = {};
                socklen_t fromLen = sizeof(recvFrom);

#ifdef _WIN32
                int recvLen = recvfrom(static_cast<socket_t>(m_udpSocket),
                    reinterpret_cast<char*>(recvBuffer), sizeof(recvBuffer), 0,
                    reinterpret_cast<struct sockaddr*>(&recvFrom), &fromLen);
                while (recvLen > 0) {
#else
                ssize_t recvLen = recvfrom(m_udpSocket,
                    recvBuffer, sizeof(recvBuffer), 0,
                    reinterpret_cast<struct sockaddr*>(&recvFrom), &fromLen);
                while (recvLen > 0) {
#endif
                    currentTime = picoquic_current_time();
                    picoquic_incoming_packet(m_quic, recvBuffer, static_cast<size_t>(recvLen),
                        reinterpret_cast<struct sockaddr*>(&recvFrom),
                        reinterpret_cast<struct sockaddr*>(&destAddr),
                        0, 0, currentTime);

                    fromLen = sizeof(recvFrom);
#ifdef _WIN32
                    recvLen = recvfrom(static_cast<socket_t>(m_udpSocket),
                        reinterpret_cast<char*>(recvBuffer), sizeof(recvBuffer), 0,
                        reinterpret_cast<struct sockaddr*>(&recvFrom), &fromLen);
#else
                    recvLen = recvfrom(m_udpSocket,
                        recvBuffer, sizeof(recvBuffer), 0,
                        reinterpret_cast<struct sockaddr*>(&recvFrom), &fromLen);
#endif
                }
            }

            // Check connection state
            picoquic_state_enum state = picoquic_get_cnx_state(m_cnx);
            if (state == picoquic_state_disconnected) {
                INST_LOG_WARN(TAG, "QUIC connection disconnected");
                m_active.store(false);
                break;
            }
        } // end QUIC path

        // Bridge OS sockets <-> lwIP TCP connections (both paths)
        DrainBridges();

        // Poll lwIP timers (both paths)
        m_network.Poll();

        // Brief sleep to avoid burning CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    INST_LOG_INFO(TAG, "Forwarding thread stopped");
}

// ---------- Close ----------

void QUICTunnel::Close() {
    if (!m_active.exchange(false)) {
        // Wasn't active, but still clean up if needed
        if (m_quic) {
            auto* cbCtx = static_cast<QUICCallbackContext*>(
                picoquic_get_default_callback_context(m_quic));
            picoquic_free(m_quic);
            m_quic = nullptr;
            m_cnx = nullptr;
            delete cbCtx;
        }
        if (m_udpSocket >= 0) {
            CLOSE_SOCKET(static_cast<socket_t>(m_udpSocket));
            m_udpSocket = -1;
        }
        if (m_isUsb && m_idevConn) {
            idevice_disconnect(m_idevConn);
            m_idevConn = nullptr;
        }
        m_isUsb = false;
        return;
    }

    INST_LOG_INFO(TAG, "Closing QUIC tunnel");

    // Shut down all socket bridges
    {
        std::lock_guard<std::mutex> lock(m_bridgeMutex);
        for (auto& bridge : m_bridges) {
            if (bridge->internalFd >= 0) {
                CLOSE_SOCKET(static_cast<socket_t>(bridge->internalFd));
                bridge->internalFd = -1;
            }
            if (bridge->externalFd >= 0) {
                CLOSE_SOCKET(static_cast<socket_t>(bridge->externalFd));
                bridge->externalFd = -1;
            }
        }
        m_bridges.clear();
    }

    // Wait for forwarding thread
    if (m_forwardThread.joinable()) {
        m_forwardThread.join();
    }

    // Shutdown userspace network
    m_network.Shutdown();

    // Close QUIC
    if (m_cnx) {
        picoquic_close(m_cnx, 0);
    }

    if (m_quic) {
        auto* cbCtx = static_cast<QUICCallbackContext*>(
            picoquic_get_default_callback_context(m_quic));
        picoquic_free(m_quic);
        m_quic = nullptr;
        m_cnx = nullptr;
        delete cbCtx;
    }

    // Close UDP socket (Wi-Fi mode)
    if (m_udpSocket >= 0) {
        CLOSE_SOCKET(static_cast<socket_t>(m_udpSocket));
        m_udpSocket = -1;
    }

    // Close USB stream connection (QUIC USB or CDTunnel path)
    if ((m_isUsb || m_isCDTunnel) && m_idevConn) {
        idevice_disconnect(m_idevConn);
        m_idevConn = nullptr;
    }

    m_isUsb = false;
    m_isCDTunnel = false;
}

} // namespace instruments

#else // !INSTRUMENTS_HAS_QUIC

namespace instruments {

static const char* TAG = "QUICTunnel";

QUICTunnel::QUICTunnel() {
    std::memset(&m_fakeLocalAddr, 0, sizeof(m_fakeLocalAddr));
    std::memset(&m_fakeRemoteAddr, 0, sizeof(m_fakeRemoteAddr));
}

QUICTunnel::~QUICTunnel() {
    Close();
}

Error QUICTunnel::Connect(const std::string& address, uint16_t tunnelPort) {
    INST_LOG_INFO(TAG, "Connecting QUIC tunnel to [%s]:%u", address.c_str(), tunnelPort);
    INST_LOG_WARN(TAG, "QUIC tunnel requires picoquic library (INSTRUMENTS_HAS_QUIC). "
                 "Use external tunnel manager (pymobiledevice3 or go-ios) instead.");
    INST_LOG_WARN(TAG, "You can start a tunnel externally and pass the tunnel "
                 "address/port to DeviceConnection::FromTunnel()");
    return Error::NotSupported;
}

Error QUICTunnel::ConnectViaCoreDeviceProxy(idevice_t device) {
    (void)device;
    INST_LOG_WARN(TAG, "ConnectViaCoreDeviceProxy requires INSTRUMENTS_HAS_QUIC build flag (lwIP needed).");
    return Error::NotSupported;
}

Error QUICTunnel::ConnectViaUSB(idevice_t device) {
    (void)device;
    INST_LOG_WARN(TAG, "ConnectViaUSB requires INSTRUMENTS_HAS_QUIC build flag.");
    return Error::NotSupported;
}

int QUICTunnel::CreateTunnelSocket(const std::string& destIPv6, uint16_t port) {
    (void)destIPv6;
    (void)port;
    INST_LOG_WARN(TAG, "CreateTunnelSocket requires INSTRUMENTS_HAS_QUIC build flag.");
    return -1;
}

void QUICTunnel::Close() {
    if (!m_active.exchange(false)) return;

    INST_LOG_INFO(TAG, "Closing tunnel");

    if (m_forwardThread.joinable()) {
        m_forwardThread.join();
    }

    if ((m_isUsb || m_isCDTunnel) && m_idevConn) {
        idevice_disconnect(m_idevConn);
        m_idevConn = nullptr;
    }

    m_isUsb = false;
    m_isCDTunnel = false;
}

void QUICTunnel::SubmitToLoop(std::function<void()> fn) {
    (void)fn;
}

void QUICTunnel::DrainTasks() {}

void QUICTunnel::DrainBridges() {}

} // namespace instruments

#endif // INSTRUMENTS_HAS_QUIC
