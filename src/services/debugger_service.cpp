#include "../../include/instruments/debugger_service.h"
#include "../util/log.h"

#include <libimobiledevice/debugserver.h>
#include <libimobiledevice/libimobiledevice.h>

#include <csignal>
#include <cstring>
#include <cstdio>
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
#define CLOSE_SOCKET closesocket
#define SOCK_ERR     WSAGetLastError()
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#define CLOSE_SOCKET close
#define SOCK_ERR     errno
#endif

namespace instruments {

static const char* TAG = "DebuggerService";

// ---------------------------------------------------------------------------
// cancel_receive shim — debugserver_client_set_receive_params expects int(*)().
// We use a thread-local pointer so each DebugLoop thread has its own flag.
// ---------------------------------------------------------------------------
static thread_local const std::atomic<bool>* tl_stopFlag = nullptr;
static int CancelReceive() {
    return (tl_stopFlag && tl_stopFlag->load()) ? 1 : 0;
}

// Helper: send a command and return the response string (caller must free).
// Returns nullptr if no response or on error.
static char* SendCmd(debugserver_client_t client, const char* name,
                     int argc, char* argv[])
{
    debugserver_command_t cmd = nullptr;
    // API: debugserver_command_new(name, argc, argv[], out_command*)
    debugserver_command_new(name, argc, argv, &cmd);
    char* response = nullptr;
    debugserver_client_send_command(client, cmd, &response, nullptr);
    debugserver_command_free(cmd);
    return response;
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

DebuggerService::DebuggerService(std::shared_ptr<DeviceConnection> connection)
    : m_connection(std::move(connection))
{
#ifdef _WIN32
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

DebuggerService::~DebuggerService() {
    Stop();
    StopDebugServer();
#ifdef _WIN32
    WSACleanup();
#endif
}

// ===========================================================================
// LaunchWithDebugger
// ===========================================================================

Error DebuggerService::LaunchWithDebugger(
    const std::string& bundleId,
    const std::string& appPath,
    const std::string& workingDir,
    const std::vector<std::string>& args,
    const std::map<std::string, std::string>& env,
    bool detach,
    std::function<void(const std::string&)> outputCb,
    std::function<void(Error, const std::string&)> errorCb)
{
    if (m_running.load()) {
        INST_LOG_WARN(TAG, "LaunchWithDebugger: already running, stopping previous session");
        Stop();
    }

    idevice_t device = m_connection->GetDevice();
    if (!device) {
        INST_LOG_ERROR(TAG, "LaunchWithDebugger: no device");
        return Error::ConnectionFailed;
    }

    debugserver_client_t client = nullptr;
    if (debugserver_client_start_service(device, &client, "libinstruments") != DEBUGSERVER_E_SUCCESS) {
        INST_LOG_ERROR(TAG, "Could not start com.apple.debugserver. Mount a developer disk image first.");
        return Error::ServiceStartFailed;
    }

    m_stopRequested.store(false);
    if (debugserver_client_set_receive_params(client, CancelReceive, 250) != DEBUGSERVER_E_SUCCESS) {
        debugserver_client_free(client);
        INST_LOG_ERROR(TAG, "debugserver_client_set_receive_params failed");
        return Error::ServiceStartFailed;
    }

    // QSetMaxPacketSize:
    {
        char sizeStr[] = "102400";
        char* argv1[1] = { sizeStr };
        char* r = SendCmd(client, "QSetMaxPacketSize:", 1, argv1);
        if (r) {
            bool ok = strncmp(r, "OK", 2) == 0;
            free(r);
            if (!ok) {
                debugserver_client_free(client);
                INST_LOG_ERROR(TAG, "QSetMaxPacketSize rejected");
                return Error::ServiceStartFailed;
            }
        }
    }

    // QSetWorkingDir:
    {
        // must be mutable — the API takes char*[], not const char*[]
        std::string wd = workingDir;
        char* argv1[1] = { wd.data() };
        char* r = SendCmd(client, "QSetWorkingDir:", 1, argv1);
        if (r) {
            bool ok = strncmp(r, "OK", 2) == 0;
            free(r);
            if (!ok) {
                debugserver_client_free(client);
                INST_LOG_ERROR(TAG, "QSetWorkingDir rejected");
                return Error::ServiceStartFailed;
            }
        }
    }

    // Environment variables — API takes char** response (we ignore it)
    for (const auto& [key, value] : env) {
        std::string kv = key + "=" + value;
        char* envResp = nullptr;
        debugserver_client_set_environment_hex_encoded(client, kv.c_str(), &envResp);
        if (envResp) free(envResp);
    }

    // Set argv: [appPath, extra args...]
    // API: debugserver_client_set_argv(client, argc, char* argv[], char** response)
    {
        std::vector<std::string> argStrs;
        argStrs.push_back(appPath);
        for (const auto& a : args) argStrs.push_back(a);

        std::vector<char*> argPtrs;
        for (auto& s : argStrs) argPtrs.push_back(s.data());

        char* argResp = nullptr;
        debugserver_client_set_argv(client, static_cast<int>(argPtrs.size()),
                                    argPtrs.data(), &argResp);
        if (argResp) free(argResp);
    }

    // qLaunchSuccess
    {
        char* r = SendCmd(client, "qLaunchSuccess", 0, nullptr);
        if (r) {
            bool ok = strncmp(r, "OK", 2) == 0;
            if (!ok) {
                char* decoded = nullptr;
                debugserver_decode_string(r + 1, strlen(r) - 1, &decoded);
                std::string errMsg = decoded ? decoded : r;
                if (decoded) free(decoded);
                free(r);
                debugserver_client_free(client);
                INST_LOG_ERROR(TAG, "qLaunchSuccess failed: %s", errMsg.c_str());
                if (errorCb) errorCb(Error::ServiceStartFailed, errMsg);
                return Error::ServiceStartFailed;
            }
            free(r);
        }
    }

    if (detach) {
        char* r = SendCmd(client, "D", 0, nullptr);
        if (r) free(r);
        debugserver_client_free(client);
        INST_LOG_INFO(TAG, "Launched '%s' and detached", bundleId.c_str());
        return Error::Success;
    }

    {
        std::lock_guard<std::mutex> lock(m_debugMutex);
        m_debugClient = client;
    }
    m_running.store(true);

    m_debugThread = std::thread([this, outputCb, errorCb]() {
        tl_stopFlag = &m_stopRequested;
        DebugLoop(outputCb, errorCb);
        tl_stopFlag = nullptr;
    });

    return Error::Success;
}

// ---------------------------------------------------------------------------
// DebugLoop — background thread: set thread, continue, receive until done
// ---------------------------------------------------------------------------

void DebuggerService::DebugLoop(
    std::function<void(const std::string&)> outputCb,
    std::function<void(Error, const std::string&)> errorCb)
{
    debugserver_client_t client;
    {
        std::lock_guard<std::mutex> lock(m_debugMutex);
        client = m_debugClient;
    }
    if (!client) {
        m_running.store(false);
        return;
    }

    int exitStatus = -1;
    std::string stdoutBuf;

    // Hc0 — set thread
    {
        char* r = SendCmd(client, "Hc0", 0, nullptr);
        if (r) {
            bool ok = strncmp(r, "OK", 2) == 0;
            free(r);
            if (!ok) {
                INST_LOG_ERROR(TAG, "Hc0 rejected");
                if (errorCb) errorCb(Error::ServiceStartFailed, "Failed to set thread");
                CleanupDebugger();
                return;
            }
        }
    }

    // c — continue; first response may arrive here or in the receive loop
    debugserver_error_t dres;
    char* response = nullptr;
    {
        debugserver_command_t cmd = nullptr;
        debugserver_command_new("c", 0, nullptr, &cmd);
        dres = debugserver_client_send_command(client, cmd, &response, nullptr);
        debugserver_command_free(cmd);
    }

    // Parse one packet; returns false when the process has exited/stopped
    auto handlePacket = [&](char* r) -> bool {
        if (!r) return true;
        if (r[0] == 'O') {
            char* decoded = nullptr;
            debugserver_decode_string(r + 1, strlen(r) - 1, &decoded);
            if (decoded) {
                stdoutBuf += decoded;
                free(decoded);
                size_t pos;
                while ((pos = stdoutBuf.find('\n')) != std::string::npos) {
                    std::string line = stdoutBuf.substr(0, pos);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (outputCb) outputCb(line);
                    stdoutBuf.erase(0, pos + 1);
                }
            }
            return true;
        } else if (r[0] == 'T') {
            INST_LOG_DEBUG(TAG, "Thread stopped: %s", r + 1);
            exitStatus = 128 + SIGABRT;
            return false;
        } else if (r[0] == 'E') {
            INST_LOG_DEBUG(TAG, "Debugserver error: %s", r + 1);
            return true;
        } else if (r[0] == 'W' || r[0] == 'X') {
            char* decoded = nullptr;
            debugserver_decode_string(r + 1, strlen(r) - 1, &decoded);
            if (decoded) {
                exitStatus = static_cast<unsigned char>(decoded[0]) + (r[0] == 'X' ? 128 : 0);
                free(decoded);
            }
            return false;
        }
        return true;
    };

    bool loopRunning = true;
    while (!m_stopRequested.load() && loopRunning) {
        if (dres != DEBUGSERVER_E_SUCCESS && dres != DEBUGSERVER_E_TIMEOUT) {
            INST_LOG_DEBUG(TAG, "Receive error %d", static_cast<int>(dres));
            break;
        }
        if (response) {
            bool cont = handlePacket(response);
            free(response); response = nullptr;
            if (!cont) { loopRunning = false; break; }
        }
        if (exitStatus >= 0) break;
        dres = debugserver_client_receive_response(client, &response, nullptr);
    }

    // Flush any partial output line
    if (!stdoutBuf.empty() && outputCb) {
        if (stdoutBuf.back() == '\r') stdoutBuf.pop_back();
        outputCb(stdoutBuf);
    }
    if (response) { free(response); response = nullptr; }

    // Disable cancel so teardown commands get through
    debugserver_client_set_receive_params(client, nullptr, 5000);

    // \x03 — interrupt
    {
        char* r = SendCmd(client, "\x03", 0, nullptr);
        if (r) free(r);
    }

    // k — kill
    {
        char* r = SendCmd(client, "k", 0, nullptr);
        if (r) free(r);
    }

    CleanupDebugger();

    if (errorCb) {
        if (exitStatus >= 0)
            errorCb(Error::Success, "Process exited with status " + std::to_string(exitStatus));
        else
            errorCb(Error::Success, "Debugger stopped");
    }
}

void DebuggerService::CleanupDebugger() {
    std::lock_guard<std::mutex> lock(m_debugMutex);
    if (m_debugClient) {
        debugserver_client_free(m_debugClient);
        m_debugClient = nullptr;
    }
    m_running.store(false);
    m_stopRequested.store(false);
}

void DebuggerService::Stop() {
    if (!m_running.load()) return;
    m_stopRequested.store(true);
    if (m_debugThread.joinable()) {
        m_debugThread.join();
    }
}

// ===========================================================================
// StartDebugServer (proxy)
// ===========================================================================

// Intercept $qLaunchGDBServer;#4b and reply with local port for LLDB.
// Mirrors idevicedebugserverproxy.c intercept_packet().
bool DebuggerService::InterceptLldbPacket(char* buf, int* len) const {
    static const char kReq[] = "$qLaunchGDBServer;#4b";
    const int kReqLen = static_cast<int>(sizeof(kReq) - 1);

    if (*len != kReqLen || memcmp(buf, kReq, static_cast<size_t>(kReqLen)) != 0)
        return false;

    char body[64];
    snprintf(body, sizeof(body), "port:%u;", static_cast<unsigned>(m_proxyPort.load()));

    int sum = 0;
    for (size_t i = 0; i < strlen(body); ++i) sum += static_cast<unsigned char>(body[i]);
    sum &= 0xFF;

    *len = snprintf(buf, 256, "$%s#%02x", body, sum);
    return true;
}

Error DebuggerService::StartDebugServer(uint16_t hostPort, bool enableLldb,
                                         uint16_t* outActualPort)
{
    if (m_proxyRunning.load()) {
        INST_LOG_WARN(TAG, "StartDebugServer: already running");
        return Error::Success;
    }

    socket_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == DEBUGGER_SOCKET_INVALID) {
        INST_LOG_ERROR(TAG, "Failed to create listen socket: %d", SOCK_ERR);
        return Error::InternalError;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(hostPort);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0) {
        INST_LOG_ERROR(TAG, "bind failed on port %u: %d", hostPort, SOCK_ERR);
        CLOSE_SOCKET(sock);
        return Error::InternalError;
    }

    struct sockaddr_in bound = {};
    socklen_t boundLen = sizeof(bound);
    getsockname(sock, reinterpret_cast<struct sockaddr*>(&bound), &boundLen);
    uint16_t actualPort = ntohs(bound.sin_port);

    if (listen(sock, 10) != 0) {
        INST_LOG_ERROR(TAG, "listen failed: %d", SOCK_ERR);
        CLOSE_SOCKET(sock);
        return Error::InternalError;
    }

    m_listenSocket = sock;
    m_proxyPort.store(actualPort);
    m_enableLldb = enableLldb;
    m_proxyRunning.store(true);

    if (outActualPort) *outActualPort = actualPort;

    INST_LOG_INFO(TAG, "DebugServer proxy listening on 0.0.0.0:%u (lldb=%s)",
                  actualPort, enableLldb ? "yes" : "no");

    m_acceptThread = std::thread([this]() { AcceptLoop(); });
    return Error::Success;
}

void DebuggerService::AcceptLoop() {
    while (m_proxyRunning.load()) {
#ifdef _WIN32
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(m_listenSocket, &rfds);
        struct timeval tv = {1, 0};
        int ret = ::select(0, &rfds, nullptr, nullptr, &tv);
#else
        struct pollfd pfd = {m_listenSocket, POLLIN, 0};
        int ret = poll(&pfd, 1, 1000);
#endif
        if (ret <= 0) continue;

        socket_t clientFd = accept(m_listenSocket, nullptr, nullptr);
        if (clientFd == DEBUGGER_SOCKET_INVALID) continue;

        INST_LOG_DEBUG(TAG, "Accepted debugserver proxy connection");

        std::thread([this, clientFd]() {
            RelayConnection(clientFd);
        }).detach();
    }

    CLOSE_SOCKET(m_listenSocket);
    m_listenSocket = DEBUGGER_SOCKET_INVALID;
}

void DebuggerService::RelayConnection(socket_t clientFd) {
    idevice_t device = m_connection->GetDevice();
    if (!device) {
        INST_LOG_ERROR(TAG, "RelayConnection: no device");
        CLOSE_SOCKET(clientFd);
        return;
    }

    debugserver_client_t dbgClient = nullptr;
    if (debugserver_client_start_service(device, &dbgClient, "libinstruments") != DEBUGSERVER_E_SUCCESS) {
        INST_LOG_ERROR(TAG, "RelayConnection: could not start debugserver");
        CLOSE_SOCKET(clientFd);
        return;
    }

    const int kBufSize = 65536;
    std::vector<char> buf(static_cast<size_t>(kBufSize));
    int dtimeout = 1;

    bool done = false;
    while (m_proxyRunning.load() && !done) {
        // Poll host socket for incoming data (1 s timeout for stoppability)
#ifdef _WIN32
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(clientFd, &rfds);
        struct timeval tv = {1, 0};
        int ret = ::select(0, &rfds, nullptr, nullptr, &tv);
#else
        struct pollfd pfd = {clientFd, POLLIN, 0};
        int ret = poll(&pfd, 1, 1000);
#endif
        if (ret < 0) break;

        if (ret > 0) {
            // host → device
            int n = static_cast<int>(recv(clientFd, buf.data(), static_cast<size_t>(kBufSize), 0));
            if (n <= 0) {
                INST_LOG_DEBUG(TAG, "Client disconnected");
                break;
            }

            bool intercepted = false;
            if (m_enableLldb) {
                int plen = n;
                if (InterceptLldbPacket(buf.data(), &plen)) {
                    ::send(clientFd, buf.data(), plen, 0);
                    intercepted = true;
                }
            }

            if (!intercepted) {
                uint32_t sent = 0;
                debugserver_client_send(dbgClient, buf.data(), static_cast<uint32_t>(n), &sent);
            }
        }

        // device → host (drain all available data)
        do {
            uint32_t r = 0;
            debugserver_error_t derr = debugserver_client_receive_with_timeout(
                dbgClient, buf.data(), static_cast<uint32_t>(kBufSize), &r,
                static_cast<unsigned int>(dtimeout));
            if (r > 0) {
                ::send(clientFd, buf.data(), static_cast<int>(r), 0);
                dtimeout = 1;
            } else if (derr == DEBUGSERVER_E_TIMEOUT) {
                dtimeout = 5;
                break;
            } else {
                INST_LOG_DEBUG(TAG, "Debugserver connection closed by device");
                done = true;
                break;
            }
        } while (true);
    }

    debugserver_client_free(dbgClient);
    CLOSE_SOCKET(clientFd);
    INST_LOG_DEBUG(TAG, "RelayConnection closed");
}

void DebuggerService::StopDebugServer() {
    if (!m_proxyRunning.load()) return;
    m_proxyRunning.store(false);
    if (m_acceptThread.joinable()) {
        m_acceptThread.join();
    }
    INST_LOG_INFO(TAG, "DebugServer proxy stopped");
}

} // namespace instruments
