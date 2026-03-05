#ifndef INSTRUMENTS_FPS_SERVICE_H
#define INSTRUMENTS_FPS_SERVICE_H

#include "device_connection.h"
#include "types.h"
#include <atomic>
#include <memory>

namespace instruments {

// FPSService - monitors GPU frame rate and utilization using the
// graphics.opengl DTX service.
//
// Usage:
//   auto fps = FPSService(connection);
//   fps.Start(1000, [](const FPSData& d) { printf("FPS: %.0f\n", d.fps); });
//   // ... later ...
//   fps.Stop();
class FPSService {
public:
    explicit FPSService(std::shared_ptr<DeviceConnection> connection);
    ~FPSService();

    // Start FPS monitoring
    // sampleIntervalMs: how often to sample (in milliseconds)
    Error Start(uint32_t sampleIntervalMs,
                FPSCallback callback,
                ErrorCallback errorCb = nullptr);

    // Stop monitoring
    void Stop();

    // Check if monitoring is active
    bool IsRunning() const { return m_running.load(); }

private:
    std::shared_ptr<DeviceConnection> m_connection;
    std::unique_ptr<DTXConnection> m_dtxConnection;
    std::shared_ptr<DTXChannel> m_channel;
    std::atomic<bool> m_running{false};
};

} // namespace instruments

#endif // INSTRUMENTS_FPS_SERVICE_H
