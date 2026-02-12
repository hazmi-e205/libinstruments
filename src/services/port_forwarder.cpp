#include "../../include/instruments/port_forwarder.h"
#include "../util/log.h"
#include <libimobiledevice/libimobiledevice.h>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
using socket_t = SOCKET;
#define SOCKET_INVALID INVALID_SOCKET
#define CLOSE_SOCKET closesocket
#define SOCK_ERR WSAGetLastError()
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
using socket_t = int;
#define SOCKET_INVALID (-1)
#define CLOSE_SOCKET close
#define SOCK_ERR errno
#endif

namespace instruments {

static const char* TAG = "PortForwarder";

PortForwarder::PortForwarder(std::shared_ptr<DeviceConnection> connection)
    : m_connection(std::move(connection))
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

PortForwarder::~PortForwarder() {
    StopAll();
#ifdef _WIN32
    WSACleanup();
#endif
}

Error PortForwarder::Forward(uint16_t hostPort, uint16_t devicePort,
                              uint16_t* outActualPort) {
    auto entry = std::make_unique<ForwardEntry>();
    entry->hostPort = hostPort;
    entry->devicePort = devicePort;

    // Create listening socket
    socket_t sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == SOCKET_INVALID) {
        INST_LOG_ERROR(TAG, "Failed to create socket: %d", SOCK_ERR);
        return Error::InternalError;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(hostPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        INST_LOG_ERROR(TAG, "Failed to bind to port %u: %d", hostPort, SOCK_ERR);
        CLOSE_SOCKET(sock);
        return Error::InternalError;
    }

    // Get actual port if 0 was requested
    struct sockaddr_in boundAddr = {};
    socklen_t addrLen = sizeof(boundAddr);
    getsockname(sock, reinterpret_cast<struct sockaddr*>(&boundAddr), &addrLen);
    uint16_t actualPort = ntohs(boundAddr.sin_port);
    entry->hostPort = actualPort;

    if (outActualPort) *outActualPort = actualPort;

    if (listen(sock, 10) != 0) {
        INST_LOG_ERROR(TAG, "Failed to listen: %d", SOCK_ERR);
        CLOSE_SOCKET(sock);
        return Error::InternalError;
    }

    entry->listenSocket = static_cast<decltype(entry->listenSocket)>(sock);
    entry->running.store(true);

    INST_LOG_INFO(TAG, "Forwarding localhost:%u -> device:%u", actualPort, devicePort);

    // Start accept loop
    ForwardEntry* entryPtr = entry.get();
    entry->acceptThread = std::thread([this, entryPtr]() {
        AcceptLoop(entryPtr);
    });

    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.push_back(std::move(entry));

    return Error::Success;
}

void PortForwarder::AcceptLoop(ForwardEntry* entry) {
    socket_t listenSock = static_cast<socket_t>(entry->listenSocket);

    while (entry->running.load()) {
        // Use select with timeout for stoppability
#ifdef _WIN32
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(listenSock, &readfds);
        struct timeval tv = {1, 0};
        int ret = select(0, &readfds, nullptr, nullptr, &tv);
#else
        struct pollfd pfd = {listenSock, POLLIN, 0};
        int ret = poll(&pfd, 1, 1000);
#endif
        if (ret <= 0) continue;

        socket_t clientSock = accept(listenSock, nullptr, nullptr);
        if (clientSock == SOCKET_INVALID) continue;

        uint16_t devicePort = entry->devicePort;

        // Spawn relay thread for this connection
        std::thread([this, clientSock, devicePort]() {
            idevice_t device = m_connection->GetDevice();
            if (!device) {
                CLOSE_SOCKET(clientSock);
                return;
            }

            // Connect to device port via usbmuxd
            idevice_connection_t deviceConn = nullptr;
            idevice_error_t err = idevice_connect(device, devicePort, &deviceConn);
            if (err != IDEVICE_E_SUCCESS || !deviceConn) {
                INST_LOG_ERROR(TAG, "Failed to connect to device port %u: %d", devicePort, err);
                CLOSE_SOCKET(clientSock);
                return;
            }

            INST_LOG_DEBUG(TAG, "New relay connection to device port %u", devicePort);

            // Bidirectional relay
            constexpr size_t BUF_SIZE = 16384;
            auto buf = std::make_unique<char[]>(BUF_SIZE);
            bool running = true;

            // Device -> Host thread
            std::thread deviceToHost([&]() {
                char localBuf[BUF_SIZE];
                while (running) {
                    uint32_t bytesRead = 0;
                    idevice_error_t rerr = idevice_connection_receive_timeout(
                        deviceConn, localBuf, BUF_SIZE, &bytesRead, 1000);
                    if (rerr == IDEVICE_E_TIMEOUT) continue;
                    if (rerr != IDEVICE_E_SUCCESS || bytesRead == 0) {
                        running = false;
                        break;
                    }
                    size_t sent = 0;
                    while (sent < bytesRead && running) {
                        int n = send(clientSock, localBuf + sent,
                                     static_cast<int>(bytesRead - sent), 0);
                        if (n <= 0) { running = false; break; }
                        sent += n;
                    }
                }
            });

            // Host -> Device
            while (running) {
#ifdef _WIN32
                fd_set readfds;
                FD_ZERO(&readfds);
                FD_SET(clientSock, &readfds);
                struct timeval tv = {1, 0};
                int ret = select(0, &readfds, nullptr, nullptr, &tv);
#else
                struct pollfd pfd = {clientSock, POLLIN, 0};
                int ret = poll(&pfd, 1, 1000);
#endif
                if (ret <= 0) {
                    if (ret < 0) running = false;
                    continue;
                }

                int n = recv(clientSock, buf.get(), BUF_SIZE, 0);
                if (n <= 0) { running = false; break; }

                uint32_t bytesSent = 0;
                idevice_error_t serr = idevice_connection_send(
                    deviceConn, buf.get(), n, &bytesSent);
                if (serr != IDEVICE_E_SUCCESS) {
                    running = false;
                    break;
                }
            }

            deviceToHost.join();
            idevice_disconnect(deviceConn);
            CLOSE_SOCKET(clientSock);
            INST_LOG_DEBUG(TAG, "Relay connection closed for device port %u", devicePort);
        }).detach();
    }

    CLOSE_SOCKET(listenSock);
}

void PortForwarder::StopForward(uint16_t hostPort) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
        if ((*it)->hostPort == hostPort) {
            (*it)->running.store(false);
            if ((*it)->acceptThread.joinable()) {
                (*it)->acceptThread.join();
            }
            INST_LOG_INFO(TAG, "Stopped forwarding port %u", hostPort);
            m_entries.erase(it);
            return;
        }
    }
}

void PortForwarder::StopAll() {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& entry : m_entries) {
        entry->running.store(false);
        if (entry->acceptThread.joinable()) {
            entry->acceptThread.join();
        }
    }
    m_entries.clear();
    INST_LOG_INFO(TAG, "All port forwarding stopped");
}

bool PortForwarder::IsActive() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return !m_entries.empty();
}

std::map<uint16_t, uint16_t> PortForwarder::GetForwardedPorts() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::map<uint16_t, uint16_t> result;
    for (const auto& entry : m_entries) {
        result[entry->hostPort] = entry->devicePort;
    }
    return result;
}

} // namespace instruments
