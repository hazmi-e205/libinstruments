#ifndef INSTRUMENTS_DEVICE_CONNECTION_H
#define INSTRUMENTS_DEVICE_CONNECTION_H

#include "types.h"
#include "dtx_connection.h"
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <memory>
#include <string>

namespace instruments {

// DeviceConnection - abstracts the connection to an iOS device regardless
// of the iOS version or transport type (USB, network, tunnel).
//
// Provides factory methods to create connections from UDID, existing idevice_t,
// or tunnel parameters. Automatically detects the iOS version and selects
// the appropriate protocol and service names.
class DeviceConnection : public std::enable_shared_from_this<DeviceConnection> {
public:
    ~DeviceConnection();

    // Factory: create from UDID (auto-detects and connects)
    static std::shared_ptr<DeviceConnection> FromUDID(const std::string& udid);

    // Factory: create from existing idevice_t (caller retains ownership)
    static std::shared_ptr<DeviceConnection> FromDevice(idevice_t device);

    // Factory: create from remote usbmux proxy (e.g., sonic-gidevice shared port)
    // Uses idevice_new_remote() to connect to a remote usbmux service.
    // NOT an RSD tunnel - this is for go-ios/sonic-gidevice remote proxies.
    static std::shared_ptr<DeviceConnection> FromTunnel(
        const std::string& tunnelAddress, uint16_t rsdPort);

    // Get the underlying idevice_t handle
    idevice_t GetDevice() const { return m_device; }

    // Get detected protocol level
    IOSProtocol GetProtocol() const { return m_protocol; }

    // Get device info (lazy-loaded)
    DeviceInfo GetDeviceInfo();

    // Create a DTX connection to the instruments service
    std::unique_ptr<DTXConnection> CreateInstrumentConnection();

    // Create a DTX connection to a specific service
    std::unique_ptr<DTXConnection> CreateServiceConnection(const std::string& serviceName);

    // Start a lockdown service
    Error StartService(const std::string& serviceId,
                       lockdownd_service_descriptor_t* outService);

    // Get the iOS version string
    const std::string& IOSVersion() const { return m_iosVersion; }

    // Check if this device uses RSD (iOS 17+)
    bool IsRSD() const { return m_protocol == IOSProtocol::RSD; }

private:
    DeviceConnection();

    idevice_t m_device = nullptr;
    bool m_ownsDevice = false;
    IOSProtocol m_protocol = IOSProtocol::Modern;
    std::string m_iosVersion;
    DeviceInfo m_deviceInfo;
    bool m_deviceInfoLoaded = false;

    // Tunnel connection info
    std::string m_tunnelAddress;
    uint16_t m_tunnelRsdPort = 0;
    bool m_isTunnel = false;
};

} // namespace instruments

#endif // INSTRUMENTS_DEVICE_CONNECTION_H
