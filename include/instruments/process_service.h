#ifndef INSTRUMENTS_PROCESS_SERVICE_H
#define INSTRUMENTS_PROCESS_SERVICE_H

#include "device_connection.h"
#include "types.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace instruments {

// ProcessService - manages process operations on iOS devices.
// Uses DTX deviceinfo channel for listing and processcontrol for launch/kill.
//
// For iOS 17+, also supports AppService XPC protocol for process operations.
class ProcessService {
public:
    explicit ProcessService(std::shared_ptr<DeviceConnection> connection);
    ~ProcessService();

    // Get list of running processes
    Error GetProcessList(std::vector<ProcessInfo>& outProcesses);

    // Launch an application by bundle ID
    // Returns the PID of the launched process
    Error LaunchApp(const std::string& bundleId,
                    const std::map<std::string, std::string>& env,
                    const std::vector<std::string>& args,
                    bool killExisting,
                    int64_t& outPid);

    // Launch with defaults (no env/args, kill existing)
    Error LaunchApp(const std::string& bundleId, int64_t& outPid);

    // Kill a process by PID
    Error KillProcess(int64_t pid);

    // Disable memory limit for a process
    Error DisableMemoryLimit(int64_t pid);

private:
    // DTX-based implementations (iOS < 17)
    Error GetProcessListDTX(std::vector<ProcessInfo>& outProcesses);
    Error LaunchAppDTX(const std::string& bundleId,
                       const std::map<std::string, std::string>& env,
                       const std::vector<std::string>& args,
                       bool killExisting, int64_t& outPid);
    Error KillProcessDTX(int64_t pid);

    // XPC-based implementations (iOS 17+)
    Error GetProcessListXPC(std::vector<ProcessInfo>& outProcesses);
    Error LaunchAppXPC(const std::string& bundleId,
                       const std::map<std::string, std::string>& env,
                       const std::vector<std::string>& args,
                       bool killExisting, int64_t& outPid);
    Error KillProcessXPC(int64_t pid);

    std::shared_ptr<DeviceConnection> m_connection;
};

} // namespace instruments

#endif // INSTRUMENTS_PROCESS_SERVICE_H
