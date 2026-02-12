#include "xctest_proxy.h"
#include "../nskeyedarchiver/nsobject.h"
#include "../util/log.h"
#include <chrono>

namespace instruments {

static const char* TAG = "XCTestProxy";

XCTestProxy::XCTestProxy(XCTestCallback resultCb,
                         LogCallback logCb,
                         ErrorCallback errorCb)
    : m_resultCb(std::move(resultCb))
    , m_logCb(std::move(logCb))
    , m_errorCb(std::move(errorCb))
{
}

void XCTestProxy::DispatchMessage(std::shared_ptr<DTXMessage> message) {
    if (!message) return;

    std::string selector = message->Selector();
    auto auxObjects = message->AuxiliaryObjects();

    INST_LOG_DEBUG(TAG, "Dispatch: %s (aux=%zu)", selector.c_str(), auxObjects.size());

    // Test lifecycle callbacks from testmanagerd
    if (selector == "_XCT_didBeginExecutingTestPlan") {
        INST_LOG_INFO(TAG, "Test plan execution started");
        if (m_logCb) m_logCb("Test plan execution started");
    }
    else if (selector == "_XCT_didFinishExecutingTestPlan") {
        HandleTestPlanFinished();
    }
    else if (selector == "_XCT_testCaseDidStartForTestClass:method:" && auxObjects.size() >= 2) {
        std::string className = auxObjects[0].IsString() ? auxObjects[0].AsString() : "";
        std::string methodName = auxObjects[1].IsString() ? auxObjects[1].AsString() : "";
        HandleTestCaseStarted(className, methodName);
    }
    else if (selector == "_XCT_testCaseDidFinishForTestClass:method:withStatus:duration:" &&
             auxObjects.size() >= 4) {
        std::string className = auxObjects[0].IsString() ? auxObjects[0].AsString() : "";
        std::string methodName = auxObjects[1].IsString() ? auxObjects[1].AsString() : "";
        std::string status = auxObjects[2].IsString() ? auxObjects[2].AsString() : "passed";
        double duration = auxObjects[3].ToNumber();
        HandleTestCaseFinished(className, methodName, status, duration);
    }
    else if (selector == "_XCT_testCaseDidFailForTestClass:method:withMessage:file:line:" &&
             auxObjects.size() >= 5) {
        std::string className = auxObjects[0].IsString() ? auxObjects[0].AsString() : "";
        std::string methodName = auxObjects[1].IsString() ? auxObjects[1].AsString() : "";
        std::string errorMsg = auxObjects[2].IsString() ? auxObjects[2].AsString() : "";
        std::string file = auxObjects[3].IsString() ? auxObjects[3].AsString() : "";
        int line = static_cast<int>(auxObjects[4].ToNumber());
        HandleTestCaseFailed(className, methodName, errorMsg, file, line);
    }
    else if (selector == "_XCT_testSuite:didStartAt:" && auxObjects.size() >= 1) {
        std::string suiteName = auxObjects[0].IsString() ? auxObjects[0].AsString() : "";
        HandleTestSuiteStarted(suiteName);
    }
    else if ((selector.find("_XCT_testSuite:didFinishAt:") == 0 ||
              selector == "_XCT_testSuiteDidFinish:") && auxObjects.size() >= 1) {
        std::string suiteName = auxObjects[0].IsString() ? auxObjects[0].AsString() : "";
        HandleTestSuiteFinished(suiteName);
    }
    else if (selector == "_XCT_logMessage:" && auxObjects.size() >= 1) {
        std::string msg = auxObjects[0].IsString() ? auxObjects[0].AsString() : "";
        HandleLogMessage(msg);
    }
    else if (selector == "_XCT_logDebugMessage:" && auxObjects.size() >= 1) {
        std::string msg = auxObjects[0].IsString() ? auxObjects[0].AsString() : "";
        INST_LOG_DEBUG(TAG, "Debug: %s", msg.c_str());
        if (m_logCb) m_logCb("[DEBUG] " + msg);
    }
    else if (selector == "_XCT_initializationForUITestingDidFailWithError:" ||
             selector == "_XCT_didFailToBootstrapWithError:") {
        std::string errorStr = auxObjects.empty() ? "Unknown error"
            : (auxObjects[0].IsString() ? auxObjects[0].AsString() : auxObjects[0].ToJson());
        INST_LOG_ERROR(TAG, "Test initialization failed: %s", errorStr.c_str());
        if (m_errorCb) m_errorCb(Error::InternalError, "Test init failed: " + errorStr);
        HandleTestPlanFinished();
    }
    else if (selector == "_XCT_testRunnerReadyWithCapabilities:" ||
             selector == "_XCT_testBundleReadyWithProtocolVersion:minimumVersion:") {
        INST_LOG_DEBUG(TAG, "Test runner ready: %s", selector.c_str());
    }
    else if (selector == "_XCT_reportSelfDiagnosisIssue:description:") {
        std::string desc = auxObjects.size() >= 2 && auxObjects[1].IsString()
            ? auxObjects[1].AsString() : "unknown issue";
        INST_LOG_WARN(TAG, "Self-diagnosis: %s", desc.c_str());
        if (m_logCb) m_logCb("[DIAG] " + desc);
    }
    else if (selector.find("_XCT_") == 0) {
        INST_LOG_DEBUG(TAG, "Unhandled XCTest callback: %s", selector.c_str());
    }
}

void XCTestProxy::HandleTestCaseStarted(const std::string& className,
                                          const std::string& methodName) {
    m_currentClass = className;
    m_currentMethod = methodName;
    INST_LOG_INFO(TAG, "Test started: %s/%s", className.c_str(), methodName.c_str());
    if (m_logCb) {
        m_logCb("Test started: " + className + "/" + methodName);
    }
}

void XCTestProxy::HandleTestCaseFinished(const std::string& className,
                                           const std::string& methodName,
                                           const std::string& status,
                                           double duration) {
    TestResult result;
    result.suiteName = m_currentSuite;
    result.className = className;
    result.methodName = methodName;
    result.duration = duration;

    if (status == "passed" || status == "1") {
        result.status = TestResult::Status::Passed;
    } else if (status == "failed" || status == "0") {
        result.status = TestResult::Status::Failed;
    } else {
        result.status = TestResult::Status::Errored;
    }

    m_results.push_back(result);

    INST_LOG_INFO(TAG, "Test %s: %s/%s (%.3fs)",
                 status.c_str(), className.c_str(), methodName.c_str(), duration);

    if (m_resultCb) {
        m_resultCb(result);
    }
}

void XCTestProxy::HandleTestCaseFailed(const std::string& className,
                                         const std::string& methodName,
                                         const std::string& message,
                                         const std::string& file,
                                         int line) {
    // Find the last result for this test case and update it
    for (auto it = m_results.rbegin(); it != m_results.rend(); ++it) {
        if (it->className == className && it->methodName == methodName) {
            it->status = TestResult::Status::Failed;
            it->errorMessage = message;
            it->errorFile = file;
            it->errorLine = line;
            break;
        }
    }

    INST_LOG_ERROR(TAG, "Test failed: %s/%s - %s (%s:%d)",
                  className.c_str(), methodName.c_str(),
                  message.c_str(), file.c_str(), line);
}

void XCTestProxy::HandleTestSuiteStarted(const std::string& suiteName) {
    m_currentSuite = suiteName;
    INST_LOG_INFO(TAG, "Suite started: %s", suiteName.c_str());
}

void XCTestProxy::HandleTestSuiteFinished(const std::string& suiteName) {
    INST_LOG_INFO(TAG, "Suite finished: %s", suiteName.c_str());
}

void XCTestProxy::HandleTestPlanFinished() {
    INST_LOG_INFO(TAG, "Test plan execution finished (%zu results)", m_results.size());
    if (m_logCb) m_logCb("Test plan execution finished");

    m_finished.store(true);
    {
        std::lock_guard<std::mutex> lock(m_finishedMutex);
        m_finishedCv.notify_all();
    }
}

void XCTestProxy::HandleLogMessage(const std::string& message) {
    INST_LOG_DEBUG(TAG, "Log: %s", message.c_str());
    if (m_logCb) m_logCb(message);
}

bool XCTestProxy::WaitForCompletion(int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_finishedMutex);
    return m_finishedCv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                                  [this]{ return m_finished.load(); });
}

} // namespace instruments
