#ifndef INSTRUMENTS_XCTEST_PROXY_H
#define INSTRUMENTS_XCTEST_PROXY_H

#include "../../include/instruments/dtx_channel.h"
#include "../../include/instruments/types.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace instruments {

// XCTestProxy - DTX proxy dispatcher that handles incoming XCTest
// callbacks from the device's testmanagerd service.
//
// Implements the IDE side of the DTX proxy channel:
// dtxproxy:XCTestManager_IDEInterface:XCTestManager_DaemonConnectionInterface
//
// The proxy receives method invocations from the device and translates
// them into TestResult callbacks.
class XCTestProxy {
public:
    XCTestProxy(XCTestCallback resultCb,
                LogCallback logCb,
                ErrorCallback errorCb);

    // Handle an incoming DTX message from the test runner
    void DispatchMessage(std::shared_ptr<DTXMessage> message);

    // Check if test execution has completed
    bool IsFinished() const { return m_finished.load(); }

    // Wait for test completion (with timeout)
    bool WaitForCompletion(int timeoutMs);

    // Get all collected test results
    const std::vector<TestResult>& Results() const { return m_results; }

    // DTX proxy channel identifier
    static constexpr const char* ProxyChannelName =
        "dtxproxy:XCTestManager_IDEInterface:XCTestManager_DaemonConnectionInterface";

private:
    void HandleTestCaseStarted(const std::string& className, const std::string& methodName);
    void HandleTestCaseFinished(const std::string& className, const std::string& methodName,
                                const std::string& status, double duration);
    void HandleTestCaseFailed(const std::string& className, const std::string& methodName,
                              const std::string& message, const std::string& file, int line);
    void HandleTestSuiteStarted(const std::string& suiteName);
    void HandleTestSuiteFinished(const std::string& suiteName);
    void HandleTestPlanFinished();
    void HandleLogMessage(const std::string& message);

    XCTestCallback m_resultCb;
    LogCallback m_logCb;
    ErrorCallback m_errorCb;

    std::vector<TestResult> m_results;
    std::string m_currentSuite;
    std::string m_currentClass;
    std::string m_currentMethod;

    std::atomic<bool> m_finished{false};
    std::mutex m_finishedMutex;
    std::condition_variable m_finishedCv;
};

} // namespace instruments

#endif // INSTRUMENTS_XCTEST_PROXY_H
