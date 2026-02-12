#ifndef INSTRUMENTS_PERFORMANCE_SERVICE_H
#define INSTRUMENTS_PERFORMANCE_SERVICE_H

#include "device_connection.h"
#include "types.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace instruments {

// Configuration for performance monitoring
struct PerfConfig {
    uint32_t sampleIntervalMs = 1000;          // Sampling interval in ms
    std::vector<std::string> systemAttributes;  // Auto-populated if empty
    std::vector<std::string> processAttributes; // Auto-populated if empty
};

// PerformanceService - monitors system and per-process performance metrics
// using the sysmontap DTX service.
//
// Usage:
//   auto perf = PerformanceService(connection);
//   perf.Start(config,
//       [](const SystemMetrics& m) { printf("CPU: %.1f%%\n", m.cpuTotalLoad); },
//       [](const vector<ProcessMetrics>& p) { ... });
//   // ... later ...
//   perf.Stop();
class PerformanceService {
public:
    explicit PerformanceService(std::shared_ptr<DeviceConnection> connection);
    ~PerformanceService();

    // Get available system attributes
    Error GetSystemAttributes(std::vector<std::string>& outAttrs);

    // Get available process attributes
    Error GetProcessAttributes(std::vector<std::string>& outAttrs);

    // Start monitoring with callbacks
    Error Start(const PerfConfig& config,
                SystemPerfCallback systemCb,
                ProcessPerfCallback processCb = nullptr,
                ErrorCallback errorCb = nullptr);

    // Stop monitoring
    void Stop();

    // Check if monitoring is active
    bool IsRunning() const { return m_running.load(); }

private:
    Error GetAttributes(const std::string& selector, std::vector<std::string>& outAttrs);
    void ParseSysmontapMessage(const NSObject& data,
                                SystemPerfCallback systemCb,
                                ProcessPerfCallback processCb);

    std::shared_ptr<DeviceConnection> m_connection;
    std::unique_ptr<DTXConnection> m_dtxConnection;
    std::shared_ptr<DTXChannel> m_channel;
    std::atomic<bool> m_running{false};
    std::vector<std::string> m_processAttributes;
};

} // namespace instruments

#endif // INSTRUMENTS_PERFORMANCE_SERVICE_H
