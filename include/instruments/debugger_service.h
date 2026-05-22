#ifndef INSTRUMENTS_DEBUGGER_SERVICE_H
#define INSTRUMENTS_DEBUGGER_SERVICE_H

#include "device_connection.h"
#include "types.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
using socket_t = SOCKET;
#define DEBUGGER_SOCKET_INVALID INVALID_SOCKET
// winsock2.h / windows.h define ERROR as 0, which collides with enum members
// named ERROR in consumer code. Undefine it immediately after the include.
#ifdef ERROR
#undef ERROR
#endif
#else
using socket_t = int;
#define DEBUGGER_SOCKET_INVALID (-1)
#endif

// Forward-declare to avoid pulling in debugserver.h in the public header
struct debugserver_client_private;
using debugserver_client_t = struct debugserver_client_private*;

namespace instruments {

// DebuggerService - launches apps via Apple's debugserver and proxies
// the debugserver connection to a local TCP port for external debugger tools.
//
// LaunchWithDebugger:
//   Connects to com.apple.debugserver, sets up the app environment, launches
//   the app, and streams stdout/stderr output via a callback. Call Stop() to
//   interrupt and kill the process. Supports detach mode (launch then detach).
//
// StartDebugServer:
//   Proxies com.apple.debugserver to a local TCP socket, accessible on all
//   interfaces (0.0.0.0). Multiple simultaneous clients are supported, each
//   getting their own debugserver session. Optionally intercepts
//   qLaunchGDBServer packets to support LLDB's gdbserver mode.
//
// Usage:
//   auto& dbg = inst->Debug();
//
//   // Launch with debugger
//   dbg.LaunchWithDebugger(bundleId, appPath, workingDir, {}, {},
//       false,   // detach = false → stay attached
//       [](const std::string& line)   { /* stdout output */ },
//       [](Error e, const std::string& msg) { /* error/stop */ });
//   // ... later ...
//   dbg.Stop();
//
//   // Proxy debugserver
//   uint16_t actualPort = 0;
//   dbg.StartDebugServer(0, true, &actualPort);  // 0 = auto-assign, lldb = true
//   // clients connect to localhost:actualPort
//   dbg.StopDebugServer();
class DebuggerService {
public:
    explicit DebuggerService(std::shared_ptr<DeviceConnection> connection);
    ~DebuggerService();

    // -------------------------------------------------------------------------
    // LaunchWithDebugger
    // -------------------------------------------------------------------------

    // Launch an app via debugserver and stream its output.
    //
    // bundleId:    App bundle identifier (e.g. "com.example.MyApp")
    // appPath:     Full on-device path to the app binary
    //              (e.g. "/private/var/containers/Bundle/.../MyApp.app/MyApp")
    // workingDir:  App container path used as working directory
    //              (e.g. "/private/var/mobile/Containers/Data/Application/...")
    // args:        Additional command-line arguments for the app
    // env:         Environment variables (KEY=VALUE format or KEY + VALUE pairs)
    // detach:      If true, launch the app and immediately detach. outputCb and
    //              errorCb are not invoked after launch.
    // outputCb:    Called for each decoded output line from the app
    // errorCb:     Called when the debugger stops (naturally or via Stop())
    //
    // Returns Success if the debugger session started successfully. The app
    // continues running on the background thread; outputCb/errorCb receive
    // updates asynchronously.
    Error LaunchWithDebugger(
        const std::string& bundleId,
        const std::string& appPath,
        const std::string& workingDir,
        const std::vector<std::string>& args,
        const std::map<std::string, std::string>& env,
        bool detach,
        std::function<void(const std::string& line)> outputCb,
        std::function<void(Error, const std::string& msg)> errorCb);

    // Stop the attached debugger session.
    // Sends interrupt (\x03) then kill (k) to the device process, waits for
    // the background thread to finish, then frees the debugserver connection.
    void Stop();

    // True while a debugger session is running.
    bool IsRunning() const { return m_running.load(); }

    // -------------------------------------------------------------------------
    // StartDebugServer (proxy)
    // -------------------------------------------------------------------------

    // Start proxying com.apple.debugserver to a local TCP port.
    //
    // hostPort:       Local port to listen on. Pass 0 to auto-assign.
    // enableLldb:     If true, intercept qLaunchGDBServer packets and respond
    //                 with the local port so LLDB can use gdbserver mode.
    // outActualPort:  Set to the actual listening port (useful when hostPort=0).
    //
    // The server listens on 0.0.0.0 (all interfaces) so it is reachable from
    // other machines on the LAN. Each accepted TCP connection spawns its own
    // debugserver session on the device.
    Error StartDebugServer(uint16_t hostPort, bool enableLldb = false,
                           uint16_t* outActualPort = nullptr);

    // Stop the proxy server and all active relay connections.
    void StopDebugServer();

    // True while the proxy server is listening.
    bool IsDebugServerRunning() const { return m_proxyRunning.load(); }

    // Return the port the proxy is listening on (0 if not running).
    uint16_t GetDebugServerPort() const { return m_proxyPort.load(); }

private:
    // --- LaunchWithDebugger internals ---
    void DebugLoop(std::function<void(const std::string&)> outputCb,
                   std::function<void(Error, const std::string&)> errorCb);
    void CleanupDebugger();
    // --- StartDebugServer internals ---
    void AcceptLoop();
    void RelayConnection(socket_t clientFd);

    // Intercept qLaunchGDBServer and rewrite to respond with local port.
    // Returns true if the packet was intercepted and a reply was written into buf.
    bool InterceptLldbPacket(char* buf, int* len) const;

    std::shared_ptr<DeviceConnection> m_connection;

    // Debugger session state
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopRequested{false};
    debugserver_client_t m_debugClient{nullptr};
    std::thread m_debugThread;
    std::mutex m_debugMutex;

    // Proxy state
    std::atomic<bool> m_proxyRunning{false};
    std::atomic<uint16_t> m_proxyPort{0};
    bool m_enableLldb{false};
    socket_t m_listenSocket{DEBUGGER_SOCKET_INVALID};
    std::thread m_acceptThread;
    std::mutex m_proxyMutex;
};

} // namespace instruments

#endif // INSTRUMENTS_DEBUGGER_SERVICE_H
