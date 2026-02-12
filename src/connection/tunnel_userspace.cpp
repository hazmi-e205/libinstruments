#include "tunnel_userspace.h"
#include "../util/log.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
using socket_t = SOCKET;
#define SOCKET_INVALID INVALID_SOCKET
#define CLOSE_SOCKET closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using socket_t = int;
#define SOCKET_INVALID (-1)
#define CLOSE_SOCKET close
#endif

namespace instruments {

static const char* TAG = "UserspaceTunnel";

UserspaceTunnel::UserspaceTunnel() {
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

UserspaceTunnel::~UserspaceTunnel() {
    StopAll();
#ifdef _WIN32
    WSACleanup();
#endif
}

Error UserspaceTunnel::StartTCPRelay(uint16_t localPort, const std::string& remoteAddr,
                                     uint16_t remotePort, uint16_t& outLocalPort) {
    INST_LOG_INFO(TAG, "Starting TCP relay: localhost:%u -> %s:%u",
                 localPort, remoteAddr.c_str(), remotePort);

    auto relay = std::make_unique<TCPRelay>();
    relay->localPort = localPort;
    relay->remoteAddr = remoteAddr;
    relay->remotePort = remotePort;
    relay->running.store(true);

    // Create listening socket
    socket_t listenSock = socket(AF_INET, SOCK_STREAM, 0);
    if (listenSock == SOCKET_INVALID) {
        INST_LOG_ERROR(TAG, "Failed to create socket");
        return Error::InternalError;
    }

    // Allow address reuse
    int reuse = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR,
              reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    // Bind
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(localPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(listenSock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        INST_LOG_ERROR(TAG, "Failed to bind to port %u", localPort);
        CLOSE_SOCKET(listenSock);
        return Error::InternalError;
    }

    // Get actual port if 0 was requested
    struct sockaddr_in boundAddr = {};
    socklen_t addrLen = sizeof(boundAddr);
    getsockname(listenSock, reinterpret_cast<struct sockaddr*>(&boundAddr), &addrLen);
    outLocalPort = ntohs(boundAddr.sin_port);
    relay->localPort = outLocalPort;

    if (listen(listenSock, 5) != 0) {
        INST_LOG_ERROR(TAG, "Failed to listen on port %u", outLocalPort);
        CLOSE_SOCKET(listenSock);
        return Error::InternalError;
    }

    INST_LOG_INFO(TAG, "TCP relay listening on port %u", outLocalPort);

    // Start accept thread
    // Note: The actual tunneling through QUIC/TUN is not yet implemented.
    // This provides the TCP listening infrastructure that would be connected
    // to the QUIC tunnel's userspace network stack.
    relay->acceptThread = std::thread([sock = listenSock, &running = relay->running]() {
        while (running.load()) {
            // TODO: Accept connections and forward through tunnel
            // For now, just keep the socket open
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sock, &readfds);

            struct timeval tv = {1, 0}; // 1 second timeout
            int ret = select(static_cast<int>(sock) + 1, &readfds, nullptr, nullptr, &tv);
            if (ret > 0 && FD_ISSET(sock, &readfds)) {
                socket_t clientSock = accept(sock, nullptr, nullptr);
                if (clientSock != SOCKET_INVALID) {
                    // TODO: Forward through tunnel
                    CLOSE_SOCKET(clientSock);
                }
            }
        }
        CLOSE_SOCKET(sock);
    });

    m_active.store(true);
    m_relays.push_back(std::move(relay));
    return Error::Success;
}

void UserspaceTunnel::StopTCPRelay(uint16_t localPort) {
    for (auto it = m_relays.begin(); it != m_relays.end(); ++it) {
        if ((*it)->localPort == localPort) {
            (*it)->running.store(false);
            if ((*it)->acceptThread.joinable()) {
                (*it)->acceptThread.join();
            }
            m_relays.erase(it);
            break;
        }
    }

    if (m_relays.empty()) {
        m_active.store(false);
    }
}

void UserspaceTunnel::StopAll() {
    for (auto& relay : m_relays) {
        relay->running.store(false);
        if (relay->acceptThread.joinable()) {
            relay->acceptThread.join();
        }
    }
    m_relays.clear();
    m_active.store(false);
}

} // namespace instruments
