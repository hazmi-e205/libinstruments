#include "../../include/instruments/fps_service.h"
#include "../nskeyedarchiver/nsobject.h"
#include "../util/log.h"

namespace instruments {

static const char* TAG = "FPSService";

FPSService::FPSService(std::shared_ptr<DeviceConnection> connection)
    : m_connection(std::move(connection))
{
}

FPSService::~FPSService() {
    Stop();
}

Error FPSService::Start(uint32_t sampleIntervalMs,
                         FPSCallback callback,
                         ErrorCallback errorCb) {
    if (m_running.load()) {
        Stop();
    }

    // Create persistent DTX connection
    m_dtxConnection = m_connection->CreateInstrumentConnection();
    if (!m_dtxConnection) {
        if (errorCb) errorCb(Error::ConnectionFailed, "Failed to create instrument connection");
        return Error::ConnectionFailed;
    }

    // Create graphics.opengl channel
    m_channel = m_dtxConnection->MakeChannelWithIdentifier(ChannelId::GraphicsOpenGL);
    if (!m_channel) {
        if (errorCb) errorCb(Error::ServiceStartFailed, "Failed to create graphics channel");
        return Error::ServiceStartFailed;
    }

    // Query available statistics (optional, for logging)
    auto statsMsg = DTXMessage::CreateWithSelector("availableStatistics");
    m_channel->SendMessageSync(statsMsg);

    // Query driver names (optional, for logging)
    auto driversMsg = DTXMessage::CreateWithSelector("driverNames");
    m_channel->SendMessageSync(driversMsg);

    // Set sampling rate
    float rate = static_cast<float>(sampleIntervalMs) / 1000.0f;
    auto rateMsg = DTXMessage::CreateWithSelector("setSamplingRate:");
    rateMsg->AppendAuxiliary(NSObject(rate));
    m_channel->SendMessageSync(rateMsg);

    // Set message handler for streaming FPS data
    m_channel->SetMessageHandler([this, callback, errorCb](
        std::shared_ptr<DTXMessage> msg) {
        if (!m_running.load()) return;

        auto payload = msg->PayloadObject();
        if (!payload) return;

        FPSData fpsData;

        if (payload->IsDict()) {
            // Parse CoreAnimationFramesPerSecond from the response
            if (payload->HasKey("CoreAnimationFramesPerSecond")) {
                fpsData.fps = payload->IsDict()
                    ? (*payload)["CoreAnimationFramesPerSecond"].ToNumber()
                    : 0.0;
            }
            if (payload->HasKey("DeviceUtilization")) {
                fpsData.gpuUtilization = (*payload)["DeviceUtilization"].ToNumber();
            }
            // Alternative key names
            if (payload->HasKey("fps")) {
                fpsData.fps = (*payload)["fps"].ToNumber();
            }
            if (payload->HasKey("GpuUtilization")) {
                fpsData.gpuUtilization = (*payload)["GpuUtilization"].ToNumber();
            }
        } else if (payload->IsFloat() || payload->IsInt()) {
            // Some iOS versions return just the FPS value
            fpsData.fps = payload->ToNumber();
        }

        if (callback) {
            callback(fpsData);
        }
    });

    // Start sampling
    auto startMsg = DTXMessage::CreateWithSelector("startSamplingAtTimeInterval:");
    startMsg->AppendAuxiliary(NSObject(0.0));
    auto response = m_channel->SendMessageSync(startMsg);

    m_running.store(true);
    INST_LOG_INFO(TAG, "FPS monitoring started (interval=%ums)", sampleIntervalMs);

    return Error::Success;
}

void FPSService::Stop() {
    if (!m_running.exchange(false)) return;

    INST_LOG_INFO(TAG, "Stopping FPS monitoring");

    if (m_channel) {
        auto stopMsg = DTXMessage::CreateWithSelector("stopSampling");
        m_channel->SendMessageSync(stopMsg, 2000);
        m_channel->Cancel();
        m_channel.reset();
    }

    if (m_dtxConnection) {
        m_dtxConnection->Disconnect();
        m_dtxConnection.reset();
    }
}

} // namespace instruments
