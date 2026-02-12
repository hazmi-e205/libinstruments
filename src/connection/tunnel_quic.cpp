#include "tunnel_quic.h"
#include "../util/log.h"

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
#include <cstring>
#include <chrono>

namespace instruments {

static const char* TAG = "QUICTunnel";

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

Error QUICTunnel::PerformHandshake() {
    auto* cbCtx = static_cast<QUICCallbackContext*>(picoquic_get_callback_context(m_cnx));
    if (!cbCtx) return Error::InternalError;

    // Send clientHandshakeRequest on stream 0 (client-initiated bidirectional)
    const char* request = "{\"type\":\"clientHandshakeRequest\",\"mtu\":1280}";
    size_t reqLen = std::strlen(request);

    // Stream 0 is client-initiated bidirectional
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

    struct sockaddr_storage destAddr = {};
    socklen_t destAddrLen = sizeof(destAddr);
    getpeername(static_cast<socket_t>(m_udpSocket),
                reinterpret_cast<struct sockaddr*>(&destAddr), &destAddrLen);

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
            sendto(static_cast<socket_t>(m_udpSocket), reinterpret_cast<const char*>(sendBuffer),
                   static_cast<int>(sendLength), 0,
                   reinterpret_cast<struct sockaddr*>(&addrTo),
                   static_cast<int>(destAddrLen));
        }

        // Receive
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

    // Parse the response JSON
    // Expected: {"type":"serverHandshakeResponse","clientParameters":{"address":"...","mtu":1280},
    //            "serverAddress":"...","serverRSDPort":58783}
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

void QUICTunnel::ForwardLoop() {
    INST_LOG_INFO(TAG, "Forwarding thread started");

    uint8_t sendBuffer[PICOQUIC_MAX_PACKET_SIZE];
    uint8_t recvBuffer[PICOQUIC_MAX_PACKET_SIZE];

    // Get the peer address for incoming packet attribution
    struct sockaddr_storage destAddr = {};
    socklen_t destAddrLen = sizeof(destAddr);
    getpeername(static_cast<socket_t>(m_udpSocket),
                reinterpret_cast<struct sockaddr*>(&destAddr), &destAddrLen);

    while (m_active.load()) {
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

            sendto(static_cast<socket_t>(m_udpSocket),
                   reinterpret_cast<const char*>(sendBuffer),
                   static_cast<int>(sendLength), 0,
                   reinterpret_cast<struct sockaddr*>(&addrTo),
                   static_cast<int>(destAddrLen));
        }

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

        // Poll lwIP timers
        m_network.Poll();

        // Check connection state
        picoquic_state_enum state = picoquic_get_cnx_state(m_cnx);
        if (state == picoquic_state_disconnected) {
            INST_LOG_WARN(TAG, "QUIC connection disconnected");
            m_active.store(false);
            break;
        }

        // Brief sleep to avoid burning CPU
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    INST_LOG_INFO(TAG, "Forwarding thread stopped");
}

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
        return;
    }

    INST_LOG_INFO(TAG, "Closing QUIC tunnel");

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

    // Close socket
    if (m_udpSocket >= 0) {
        CLOSE_SOCKET(static_cast<socket_t>(m_udpSocket));
        m_udpSocket = -1;
    }
}

} // namespace instruments

#else // !INSTRUMENTS_HAS_QUIC

namespace instruments {

static const char* TAG = "QUICTunnel";

QUICTunnel::QUICTunnel() = default;

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

void QUICTunnel::Close() {
    if (!m_active.exchange(false)) return;

    INST_LOG_INFO(TAG, "Closing QUIC tunnel");

    if (m_forwardThread.joinable()) {
        m_forwardThread.join();
    }
}

} // namespace instruments

#endif // INSTRUMENTS_HAS_QUIC
