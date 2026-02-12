#ifndef INSTRUMENTS_XCTEST_SERVICE_H
#define INSTRUMENTS_XCTEST_SERVICE_H

#include "device_connection.h"
#include "types.h"
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace instruments {

// XCTest configuration
struct XCTestConfig {
    std::string bundleId;               // App under test bundle ID
    std::string testRunnerBundleId;     // XCTestRunner app bundle ID
    std::string xctestConfigName;       // e.g., "MyTests.xctest"
    std::map<std::string, std::string> env;   // Environment variables
    std::vector<std::string> args;      // Launch arguments
    std::vector<std::string> testsToRun;     // Empty = run all
    std::vector<std::string> testsToSkip;
};

// XCTestService - runs XCTest bundles on iOS devices.
//
// The test execution flow:
// 1. Connect to testmanagerd via DTX
// 2. Create IDE proxy channels (IDE_DaemonConnection + IDE_Interface)
// 3. Initiate test session with capabilities
// 4. Launch test runner app (via processcontrol or AppService)
// 5. Authorize test process
// 6. Start test plan execution
// 7. Receive callbacks for test lifecycle events
// 8. Stop when tests complete or explicitly stopped
//
// For iOS 17+, uses tunnel connection and AppService for launching.
class XCTestService {
public:
    explicit XCTestService(std::shared_ptr<DeviceConnection> connection);
    ~XCTestService();

    // Run tests with result callback
    Error Run(const XCTestConfig& config,
              XCTestCallback resultCb,
              LogCallback logCb = nullptr,
              ErrorCallback errorCb = nullptr);

    // Stop test execution
    void Stop();

    // Check if tests are running
    bool IsRunning() const { return m_running.load(); }

private:
    Error RunWithDTX(const XCTestConfig& config,
                     XCTestCallback resultCb,
                     LogCallback logCb,
                     ErrorCallback errorCb);

    std::shared_ptr<DeviceConnection> m_connection;
    std::unique_ptr<DTXConnection> m_dtxConnection;
    std::unique_ptr<DTXConnection> m_dtxConnection2;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopping{false};
    int64_t m_testRunnerPid = 0;
};

} // namespace instruments

#endif // INSTRUMENTS_XCTEST_SERVICE_H
