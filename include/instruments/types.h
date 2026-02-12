#ifndef INSTRUMENTS_TYPES_H
#define INSTRUMENTS_TYPES_H

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace instruments {

// Error codes returned by all public API methods
enum class Error {
    Success = 0,
    ConnectionFailed,
    ServiceStartFailed,
    ProtocolError,
    Timeout,
    DeviceNotFound,
    TunnelFailed,
    InvalidArgument,
    NotSupported,
    Cancelled,
    InternalError,
};

// iOS version protocol level
enum class IOSProtocol {
    Legacy,        // iOS < 14 (com.apple.instruments.remoteserver)
    Modern,        // iOS 14-16 (DVTSecureSocketProxy)
    RSD,           // iOS 17+ (dtservicehub via RSD tunnel)
};

// Device information
struct DeviceInfo {
    std::string udid;
    std::string name;
    std::string version;        // e.g. "16.4.1"
    int versionMajor = 0;
    int versionMinor = 0;
    int versionPatch = 0;
    IOSProtocol protocol = IOSProtocol::Legacy;
};

// Process information from running process list
struct ProcessInfo {
    int64_t pid = 0;
    std::string name;
    std::string bundleId;
    std::string realAppName;
    bool isApplication = false;
    uint64_t startDate = 0;
};

// System-level performance metrics from sysmontap
struct SystemMetrics {
    double cpuTotalLoad = 0.0;
    double cpuUserLoad = 0.0;
    double cpuSystemLoad = 0.0;
    uint64_t cpuCount = 0;
    uint64_t enabledCPUs = 0;
    uint64_t memUsed = 0;
    uint64_t memFree = 0;
    uint64_t diskBytesRead = 0;
    uint64_t diskBytesWritten = 0;
    uint64_t netBytesIn = 0;
    uint64_t netBytesOut = 0;
    uint64_t netPacketsIn = 0;
    uint64_t netPacketsOut = 0;
};

// Per-process performance metrics from sysmontap
struct ProcessMetrics {
    int64_t pid = 0;
    std::string name;
    double cpuUsage = 0.0;
    uint64_t memResident = 0;       // physFootprint
    uint64_t memAnon = 0;
    uint64_t memVirtual = 0;
    uint64_t diskBytesRead = 0;
    uint64_t diskBytesWritten = 0;
    uint64_t threads = 0;
};

// FPS monitoring data from graphics.opengl
struct FPSData {
    double fps = 0.0;
    double gpuUtilization = 0.0;
};

// XCTest result for individual test case
struct TestResult {
    enum class Status { Passed, Failed, Errored, Skipped };

    std::string suiteName;
    std::string className;
    std::string methodName;
    Status status = Status::Passed;
    double duration = 0.0;
    std::string errorMessage;
    std::string errorFile;
    int errorLine = 0;
};

// Tunnel information
struct TunnelInfo {
    std::string address;        // IPv6 address
    uint16_t rsdPort = 0;       // RSD port on tunnel
    std::string udid;
};

// Callback types
using FPSCallback = std::function<void(const FPSData&)>;
using SystemPerfCallback = std::function<void(const SystemMetrics&)>;
using ProcessPerfCallback = std::function<void(const std::vector<ProcessMetrics>&)>;
using XCTestCallback = std::function<void(const TestResult&)>;
using LogCallback = std::function<void(const std::string&)>;
using ErrorCallback = std::function<void(Error, const std::string&)>;

// Well-known instrument service names
namespace ServiceName {
    constexpr const char* InstrumentsPre14 = "com.apple.instruments.remoteserver";
    constexpr const char* Instruments14To16 = "com.apple.instruments.remoteserver.DVTSecureSocketProxy";
    constexpr const char* Instruments17Plus = "com.apple.instruments.dtservicehub";
    constexpr const char* AppService = "com.apple.coredevice.appservice";
    constexpr const char* TestManagerD = "com.apple.testmanagerd.lockdown";
    constexpr const char* TestManagerDSecure = "com.apple.testmanagerd.lockdown.secure";
}

// Well-known DTX channel identifiers
namespace ChannelId {
    constexpr const char* DeviceInfo = "com.apple.instruments.server.services.deviceinfo";
    constexpr const char* ProcessControl = "com.apple.instruments.server.services.processcontrol";
    constexpr const char* ProcessControlPosix = "com.apple.instruments.server.services.processcontrol.posixspawn";
    constexpr const char* Sysmontap = "com.apple.instruments.server.services.sysmontap";
    constexpr const char* GraphicsOpenGL = "com.apple.instruments.server.services.graphics.opengl";
    constexpr const char* Screenshot = "com.apple.instruments.server.services.screenshot";
    constexpr const char* MobileNotifications = "com.apple.instruments.server.services.mobilenotifications";
    constexpr const char* XPCControl = "com.apple.instruments.server.services.device.xpccontrol";
    constexpr const char* AppListing = "com.apple.instruments.server.services.device.applictionListing";
    constexpr const char* ActivityTraceTap = "com.apple.instruments.server.services.activitytracetap";
    constexpr const char* ConditionInducer = "com.apple.instruments.server.services.ConditionInducer";
    constexpr const char* LocationSimulation = "com.apple.instruments.server.services.LocationSimulation";
    constexpr const char* Assets = "com.apple.instruments.server.services.assets";
}

// DTX protocol constants
namespace DTXProtocol {
    constexpr uint32_t Magic = 0x795B3D1F;
    constexpr uint32_t HeaderLength = 32;
    constexpr uint32_t PayloadHeaderLength = 16;
    constexpr int DefaultTimeoutMs = 5000;
}

// Log level for library-wide logging
enum class LogLevel {
    None = 0,
    Error,
    Warn,
    Info,
    Debug,
    Trace,
};

// Return human-readable error string
inline const char* ErrorToString(Error err) {
    switch (err) {
        case Error::Success:            return "Success";
        case Error::ConnectionFailed:   return "Connection failed";
        case Error::ServiceStartFailed: return "Service start failed";
        case Error::ProtocolError:      return "Protocol error";
        case Error::Timeout:            return "Timeout";
        case Error::DeviceNotFound:     return "Device not found";
        case Error::TunnelFailed:       return "Tunnel failed";
        case Error::InvalidArgument:    return "Invalid argument";
        case Error::NotSupported:       return "Not supported";
        case Error::Cancelled:          return "Cancelled";
        case Error::InternalError:      return "Internal error";
    }
    return "Unknown error";
}

} // namespace instruments

#endif // INSTRUMENTS_TYPES_H
