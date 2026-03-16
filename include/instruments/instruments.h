#ifndef INSTRUMENTS_INSTRUMENTS_H
#define INSTRUMENTS_INSTRUMENTS_H

// Main include header for libinstruments
// Include this single header to get the full public API.

#include "types.h"
#include "device_connection.h"
#include "tunnel_manager.h"
#include "dtx_connection.h"
#include "dtx_channel.h"
#include "dtx_message.h"
#include "process_service.h"
#include "performance_service.h"
#include "fps_service.h"
#include "xctest_service.h"
#include "wda_service.h"
#include "port_forwarder.h"

namespace instruments {

// Instruments - main facade providing convenient access to all services.
//
// Usage:
//   // From device UDID (simplest)
//   auto inst = Instruments::Create("00008030-001A35E22EF8802E");
//
//   // From existing idevice_t
//   auto inst = Instruments::Create(myDevice);
//
//   // Use services
//   std::vector<ProcessInfo> procs;
//   inst->Process().GetProcessList(procs);
//
//   inst->FPS().Start(1000, [](const FPSData& d) { ... });
//   inst->FPS().Stop();
class Instruments {
public:
    ~Instruments();

    // Factory: create from device UDID (auto-detects protocol, auto-tunnels if needed)
    static std::shared_ptr<Instruments> Create(const std::string& udid);

    // Factory: create from existing idevice_t (caller retains ownership)
    static std::shared_ptr<Instruments> Create(idevice_t device);

    // Factory: create from existing idevice_t and lockdownd_client_t (caller retains ownership)
    // Reuses the provided lockdown client instead of creating a new one
    static std::shared_ptr<Instruments> Create(idevice_t device, lockdownd_client_t lockdown);

    // Factory: create from an external tunnel (iOS 17+).
    // Start a tunnel externally first:
    //   pymobiledevice3: python3 -m pymobiledevice3 remote start-tunnel
    //   go-ios:          ios tunnel start
    // Then pass the address and RSD port printed by that command.
    // address:    IPv6 address of the device inside the tunnel
    // rsdPort:    RSD port (default 58783)
    // iosVersion: iOS version string for protocol selection (default "17.0")
    static std::shared_ptr<Instruments> CreateFromTunnel(
        const std::string& address,
        uint16_t rsdPort = 58783,
        const std::string& iosVersion = "17.0");

    // Access individual services (lazy-initialized)
    ProcessService& Process();
    PerformanceService& Performance();
    FPSService& FPS();
    XCTestService& XCTest();
    WDAService& WDA();
    PortForwarder& Ports();

    // Get the underlying connection
    std::shared_ptr<DeviceConnection> Connection() const { return m_connection; }

    // Get device info
    DeviceInfo GetDeviceInfo() { return m_connection->GetDeviceInfo(); }

    // Set log level for the library
    static void SetLogLevel(LogLevel level);

private:
    explicit Instruments(std::shared_ptr<DeviceConnection> connection);

    std::shared_ptr<DeviceConnection> m_connection;

    // Lazy-initialized services
    std::unique_ptr<ProcessService> m_process;
    std::unique_ptr<PerformanceService> m_performance;
    std::unique_ptr<FPSService> m_fps;
    std::unique_ptr<XCTestService> m_xctest;
    std::unique_ptr<WDAService> m_wda;
    std::unique_ptr<PortForwarder> m_ports;
};

} // namespace instruments

#endif // INSTRUMENTS_INSTRUMENTS_H
