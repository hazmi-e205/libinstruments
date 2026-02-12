#ifndef INSTRUMENTS_WDA_SERVICE_H
#define INSTRUMENTS_WDA_SERVICE_H

#include "device_connection.h"
#include "port_forwarder.h"
#include "xctest_service.h"
#include "types.h"
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace instruments {

// WDA configuration
struct WDAConfig {
    std::string bundleId;               // WDA app bundle ID (e.g., "com.facebook.WebDriverAgentRunner.xctrunner")
    std::string testRunnerBundleId;     // WDA test runner bundle ID
    std::string xctestConfigName = "WebDriverAgentRunner.xctest";
    uint16_t wdaPort = 8100;            // Host port for WDA HTTP server
    uint16_t mjpegPort = 9100;          // Host port for MJPEG stream
    uint16_t deviceWdaPort = 8100;      // Device port for WDA HTTP server
    uint16_t deviceMjpegPort = 9100;    // Device port for MJPEG stream
    std::map<std::string, std::string> env;
    std::vector<std::string> args;
};

// WDAService - runs WebDriverAgent on an iOS device with port forwarding.
//
// WDA is essentially a UI test bundle (XCTest) that runs a built-in HTTP
// server for remote automation. This service:
// 1. Launches WDA via the XCTest infrastructure
// 2. Forwards WDA HTTP port (default 8100)
// 3. Forwards MJPEG streaming port (default 9100)
// 4. Streams WDA logs via callback
//
// Usage:
//   WDAConfig config;
//   config.bundleId = "com.facebook.WebDriverAgentRunner.xctrunner";
//   config.testRunnerBundleId = "com.facebook.WebDriverAgentRunner.xctrunner";
//   config.wdaPort = 8100;
//   config.mjpegPort = 9100;
//
//   WDAService wda(connection);
//   wda.Start(config,
//       [](const std::string& log) { printf("%s\n", log.c_str()); });
//   // WDA now available at http://localhost:8100
//   // ... later ...
//   wda.Stop();
class WDAService {
public:
    explicit WDAService(std::shared_ptr<DeviceConnection> connection);
    ~WDAService();

    // Start WDA with port forwarding
    Error Start(const WDAConfig& config,
                LogCallback logCb = nullptr,
                ErrorCallback errorCb = nullptr);

    // Stop WDA and port forwarding
    void Stop();

    // Check if WDA is running
    bool IsRunning() const { return m_running.load(); }

    // Get the actual forwarded ports
    uint16_t GetWDAPort() const { return m_actualWdaPort; }
    uint16_t GetMJPEGPort() const { return m_actualMjpegPort; }

private:
    std::shared_ptr<DeviceConnection> m_connection;
    std::unique_ptr<XCTestService> m_xctest;
    std::unique_ptr<PortForwarder> m_portForwarder;
    std::thread m_wdaThread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopping{false};
    uint16_t m_actualWdaPort = 0;
    uint16_t m_actualMjpegPort = 0;
};

} // namespace instruments

#endif // INSTRUMENTS_WDA_SERVICE_H
