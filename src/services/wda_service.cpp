#include "../../include/instruments/wda_service.h"
#include "../util/log.h"

namespace instruments {

static const char* TAG = "WDAService";

WDAService::WDAService(std::shared_ptr<DeviceConnection> connection)
    : m_connection(std::move(connection))
{
}

WDAService::~WDAService() {
    Stop();
}

Error WDAService::Start(const WDAConfig& config,
                         LogCallback logCb,
                         ErrorCallback errorCb) {
    if (m_running.load()) {
        Stop();
    }

    m_stopping.store(false);
    m_running.store(true);

    // Step 1: Start port forwarding
    m_portForwarder = std::make_unique<PortForwarder>(m_connection);

    // Forward WDA HTTP port
    Error err = m_portForwarder->Forward(config.wdaPort, config.deviceWdaPort,
                                          &m_actualWdaPort);
    if (err != Error::Success) {
        INST_LOG_ERROR(TAG, "Failed to forward WDA port %u -> %u",
                      config.wdaPort, config.deviceWdaPort);
        if (errorCb) errorCb(err, "Failed to forward WDA port");
        m_running.store(false);
        return err;
    }
    INST_LOG_INFO(TAG, "WDA HTTP port forwarded: localhost:%u -> device:%u",
                 m_actualWdaPort, config.deviceWdaPort);

    // Forward MJPEG port
    err = m_portForwarder->Forward(config.mjpegPort, config.deviceMjpegPort,
                                    &m_actualMjpegPort);
    if (err != Error::Success) {
        INST_LOG_ERROR(TAG, "Failed to forward MJPEG port %u -> %u",
                      config.mjpegPort, config.deviceMjpegPort);
        if (errorCb) errorCb(err, "Failed to forward MJPEG port");
        m_portForwarder->StopAll();
        m_running.store(false);
        return err;
    }
    INST_LOG_INFO(TAG, "MJPEG port forwarded: localhost:%u -> device:%u",
                 m_actualMjpegPort, config.deviceMjpegPort);

    // Step 2: Launch WDA as XCTest in a background thread
    m_xctest = std::make_unique<XCTestService>(m_connection);

    XCTestConfig testConfig;
    testConfig.bundleId = config.bundleId;
    testConfig.testRunnerBundleId = config.testRunnerBundleId;
    testConfig.xctestConfigName = config.xctestConfigName;
    testConfig.env = config.env;
    testConfig.args = config.args;

    // Add WDA-specific environment variables
    testConfig.env["USE_PORT"] = std::to_string(config.deviceWdaPort);
    testConfig.env["MJPEG_SERVER_PORT"] = std::to_string(config.deviceMjpegPort);

    // Run XCTest in a background thread (it blocks until completion)
    m_wdaThread = std::thread([this, testConfig, logCb, errorCb]() {
        INST_LOG_INFO(TAG, "Starting WDA test runner...");

        Error result = m_xctest->Run(testConfig,
            // Test result callback (WDA doesn't produce test results)
            [](const TestResult&) {},
            // Log callback
            [logCb](const std::string& log) {
                if (logCb) logCb(log);
            },
            // Error callback
            [this, errorCb](Error err, const std::string& msg) {
                INST_LOG_ERROR(TAG, "WDA error: %s", msg.c_str());
                if (errorCb) errorCb(err, msg);
            }
        );

        if (result != Error::Success && !m_stopping.load()) {
            INST_LOG_ERROR(TAG, "WDA test runner exited: %s", ErrorToString(result));
            if (errorCb) errorCb(result, "WDA test runner exited unexpectedly");
        }

        INST_LOG_INFO(TAG, "WDA test runner stopped");
        m_running.store(false);
    });

    INST_LOG_INFO(TAG, "WDA started - HTTP: localhost:%u, MJPEG: localhost:%u",
                 m_actualWdaPort, m_actualMjpegPort);

    return Error::Success;
}

void WDAService::Stop() {
    if (!m_running.load() && !m_wdaThread.joinable()) return;

    INST_LOG_INFO(TAG, "Stopping WDA...");
    m_stopping.store(true);

    // Stop XCTest (kills test runner)
    if (m_xctest) {
        m_xctest->Stop();
    }

    // Wait for WDA thread
    if (m_wdaThread.joinable()) {
        m_wdaThread.join();
    }

    // Stop port forwarding
    if (m_portForwarder) {
        m_portForwarder->StopAll();
        m_portForwarder.reset();
    }

    m_xctest.reset();
    m_running.store(false);
    m_actualWdaPort = 0;
    m_actualMjpegPort = 0;

    INST_LOG_INFO(TAG, "WDA stopped");
}

} // namespace instruments
