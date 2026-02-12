#include "../../include/instruments/xctest_service.h"
#include "../../include/instruments/process_service.h"
#include "xctest_proxy.h"
#include "../connection/service_connector.h"
#include "../nskeyedarchiver/nsobject.h"
#include "../util/log.h"
#include <chrono>
#include <thread>

namespace instruments {

static const char* TAG = "XCTestService";

XCTestService::XCTestService(std::shared_ptr<DeviceConnection> connection)
    : m_connection(std::move(connection))
{
}

XCTestService::~XCTestService() {
    Stop();
}

Error XCTestService::Run(const XCTestConfig& config,
                          XCTestCallback resultCb,
                          LogCallback logCb,
                          ErrorCallback errorCb) {
    if (m_running.load()) {
        Stop();
    }

    m_stopping.store(false);
    m_running.store(true);

    Error result = RunWithDTX(config, resultCb, logCb, errorCb);

    m_running.store(false);
    return result;
}

Error XCTestService::RunWithDTX(const XCTestConfig& config,
                                 XCTestCallback resultCb,
                                 LogCallback logCb,
                                 ErrorCallback errorCb) {
    // Step 1: Create two DTX connections to testmanagerd
    std::string testManagerService = ServiceConnector::GetTestManagerServiceName(
        m_connection->GetProtocol());

    m_dtxConnection = m_connection->CreateServiceConnection(testManagerService);
    if (!m_dtxConnection) {
        INST_LOG_ERROR(TAG, "Failed to connect to testmanagerd (connection 1)");
        if (errorCb) errorCb(Error::ConnectionFailed, "Failed to connect to testmanagerd");
        return Error::ConnectionFailed;
    }

    m_dtxConnection2 = m_connection->CreateServiceConnection(testManagerService);
    if (!m_dtxConnection2) {
        INST_LOG_ERROR(TAG, "Failed to connect to testmanagerd (connection 2)");
        if (errorCb) errorCb(Error::ConnectionFailed, "Failed to connect to testmanagerd (2)");
        return Error::ConnectionFailed;
    }

    // Step 2: Create proxy channel on connection 1
    auto proxyChannel = m_dtxConnection->MakeChannelWithIdentifier(XCTestProxy::ProxyChannelName);
    if (!proxyChannel) {
        INST_LOG_ERROR(TAG, "Failed to create proxy channel");
        if (errorCb) errorCb(Error::ServiceStartFailed, "Failed to create XCTest proxy channel");
        return Error::ServiceStartFailed;
    }

    // Step 3: Create proxy dispatcher
    auto proxy = std::make_shared<XCTestProxy>(resultCb, logCb, errorCb);

    // Register message handler on proxy channel
    proxyChannel->SetMessageHandler([proxy](std::shared_ptr<DTXMessage> msg) {
        proxy->DispatchMessage(msg);
    });

    // Step 4: Initiate session with capabilities
    auto daemonChannel = m_dtxConnection->MakeChannelWithIdentifier(
        "dtxproxy:XCTestManager_IDEInterface:XCTestManager_DaemonConnectionInterface");

    if (daemonChannel) {
        // Generate session identifier (UUID-like)
        std::string sessionId = "libinstruments-xctest-session";

        // Prepare capabilities
        NSObject::DictType capabilities;
        capabilities["XCTIssue capability"] = NSObject(true);
        capabilities["skippedTest capability"] = NSObject(true);

        auto initMsg = DTXMessage::CreateWithSelector(
            "_IDE_initiateControlSessionWithCapabilities:");
        NSObject capsObj(std::move(capabilities));
        capsObj.SetClassName("XCTCapabilities");
        capsObj.SetClassHierarchy({"XCTCapabilities", "NSObject"});
        initMsg->AppendAuxiliary(capsObj);

        auto response = daemonChannel->SendMessageSync(initMsg, 10000);
        if (response) {
            INST_LOG_INFO(TAG, "Session initiated");
        }
    }

    // Step 5: Launch test runner app
    ProcessService procService(m_connection);

    std::map<std::string, std::string> env = config.env;
    env["NSUnbufferedIO"] = "YES";
    env["DYLD_INSERT_LIBRARIES"] = "";  // Can be customized
    env["XCTestConfigurationFilePath"] = "";  // Will be set by test runner

    std::vector<std::string> args = config.args;

    int64_t pid = 0;
    Error launchErr = procService.LaunchApp(config.testRunnerBundleId,
                                             env, args, true, pid);
    if (launchErr != Error::Success) {
        INST_LOG_ERROR(TAG, "Failed to launch test runner: %s", ErrorToString(launchErr));
        if (errorCb) errorCb(launchErr, "Failed to launch test runner");
        return launchErr;
    }

    m_testRunnerPid = pid;
    INST_LOG_INFO(TAG, "Test runner launched with PID %lld", (long long)pid);

    // Step 6: Authorize test process
    if (daemonChannel) {
        auto authMsg = DTXMessage::CreateWithSelector(
            "_IDE_authorizeTestSessionWithProcessID:");
        authMsg->AppendAuxiliary(NSObject(pid));
        daemonChannel->SendMessageSync(authMsg, 10000);
    }

    // Step 7: Start test plan execution
    if (daemonChannel) {
        auto startMsg = DTXMessage::CreateWithSelector(
            "_IDE_startExecutingTestPlanWithProtocolVersion:");
        startMsg->AppendAuxiliary(NSObject(static_cast<int64_t>(36)));
        daemonChannel->SendMessageSync(startMsg, 10000);
    }

    INST_LOG_INFO(TAG, "Waiting for test completion...");

    // Step 8: Wait for completion or stop signal
    while (!proxy->IsFinished() && !m_stopping.load()) {
        if (!m_dtxConnection->IsConnected()) {
            INST_LOG_INFO(TAG, "DTX connection closed, tests finished");
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Step 9: Kill test runner
    if (m_testRunnerPid > 0) {
        procService.KillProcess(m_testRunnerPid);
        m_testRunnerPid = 0;
    }

    // Cleanup
    if (proxyChannel) proxyChannel->Cancel();
    if (daemonChannel) daemonChannel->Cancel();
    if (m_dtxConnection) m_dtxConnection->Disconnect();
    if (m_dtxConnection2) m_dtxConnection2->Disconnect();
    m_dtxConnection.reset();
    m_dtxConnection2.reset();

    size_t passed = 0, failed = 0;
    for (const auto& r : proxy->Results()) {
        if (r.status == TestResult::Status::Passed) passed++;
        else failed++;
    }

    INST_LOG_INFO(TAG, "Tests complete: %zu passed, %zu failed", passed, failed);
    return Error::Success;
}

void XCTestService::Stop() {
    if (!m_running.load()) return;

    INST_LOG_INFO(TAG, "Stopping XCTest execution");
    m_stopping.store(true);

    // Kill test runner if running
    if (m_testRunnerPid > 0 && m_connection) {
        ProcessService procService(m_connection);
        procService.KillProcess(m_testRunnerPid);
        m_testRunnerPid = 0;
    }

    // Disconnect DTX
    if (m_dtxConnection) m_dtxConnection->Disconnect();
    if (m_dtxConnection2) m_dtxConnection2->Disconnect();
}

} // namespace instruments
