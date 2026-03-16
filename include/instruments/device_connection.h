#ifndef INSTRUMENTS_DEVICE_CONNECTION_H
#define INSTRUMENTS_DEVICE_CONNECTION_H

#include "types.h"
#include "dtx_connection.h"
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <map>
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

    // Factory: create from existing idevice_t and lockdownd_client_t (caller retains ownership)
    // Reuses the provided lockdown client instead of creating new ones
    static std::shared_ptr<DeviceConnection> FromDevice(idevice_t device, lockdownd_client_t lockdown);

    // Factory: create from an external tunnel address (iOS 17+).
    // address:   IPv6 (or IPv4) address of the device inside the tunnel
    //            e.g. "fd61:f62:b40d:23c9::1" from pymobiledevice3/go-ios
    // rsdPort:   RSD port (default 58783)
    // iosVersion: reported iOS version string (e.g. "17.4.1"); used to set
    //            the protocol to IOSProtocol::RSD.
    // Performs RSD service discovery synchronously on the calling thread.
    // Returns nullptr on failure.
    static std::shared_ptr<DeviceConnection> FromTunnel(
        const std::string& address,
        uint16_t rsdPort = 58783,
        const std::string& iosVersion = "17.0");

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
    // Undefine Windows API macro that collides with this method name
#ifdef StartService
#undef StartService
#endif
    Error StartService(const std::string& serviceId,
                       lockdownd_service_descriptor_t* outService);

    // Get the iOS version string
    const std::string& IOSVersion() const { return m_iosVersion; }

    // Check if this device uses RSD (iOS 17+)
    bool IsRSD() const { return m_protocol == IOSProtocol::RSD; }

    // Check if USB RSD discovery succeeded (services available without a tunnel)
    bool HasRSDServices() const { return !m_rsdServices.empty(); }

    // Check if this connection uses USB RSD (iOS 17+ via usbmuxd, no external tunnel)
    bool IsUsbRsd() const { return m_isUsbRsd; }

private:
    DeviceConnection();

    // Try USB RSD service discovery on iOS 17+ devices.
    // Called internally by FromUDID/FromDevice after protocol detection.
    void TryUsbRSD();

    // Try USB CoreDevice QUIC tunnel (iOS 17.x only; iOS 18+ uses TryNetworkRSD).
    void TryUsbQUIC();

    // Try RSD via USB-NCM network path for iOS 18+/26+.
    // Apple's usbmuxd (from iTunes/Apple Devices) registers iOS 18+ USB devices
    // with their USB-NCM IPv6 address. idevice_new_with_options(PREFER_NETWORK)
    // returns a NETWORK-type device for which idevice_connect() uses direct TCP,
    // bypassing the usbmuxd TCP port-forwarding that fails on iOS 18+.
    void TryNetworkRSD();

    // Try iOS 18+/26+ direct USB-NCM TCP connection without admin privileges.
    // Enumerates host network adapters to find the Apple USB-NCM virtual interface,
    // reads the NDP neighbor table to find the device's link-local IPv6 address,
    // then connects directly via TCP to port 58783 — same as go-ios, no libusb needed.
    void TryDirectNCMConnection();

    idevice_t m_device = nullptr;
    bool m_ownsDevice = false;
    lockdownd_client_t m_lockdown = nullptr;
    bool m_ownsLockdown = false;
    IOSProtocol m_protocol = IOSProtocol::Modern;
    std::string m_iosVersion;
    DeviceInfo m_deviceInfo;
    bool m_deviceInfoLoaded = false;

    // Tunnel connection info (set by FromTunnel())
    std::string m_tunnelAddress;
    uint16_t m_tunnelRsdPort = 0;
    bool m_isTunnel = false;

    // RSD-discovered service port map: service name → TCP port
    // Populated by FromTunnel() (via RSDProvider::ConnectDirect) or
    // by TryUsbRSD() (via RSDProvider::ConnectViaIDevice) for iOS 17+ USB devices.
    std::map<std::string, uint16_t> m_rsdServices;

    // true when RSD was discovered via USB (idevice_connect), not an external tunnel.
    // CreateInstrumentConnection() uses idevice_connect to reach the service port.
    bool m_isUsbRsd = false;

    // USB QUIC tunnel (iOS 17.x only, via ConnectViaUSB)
    // Held as shared_ptr<void> to avoid including tunnel_quic.h in the public header.
    std::shared_ptr<void> m_quicTunnel;
    bool m_isUsbQuic = false;

    // iOS 18+/26+ direct USB-NCM TCP path (no tunnel, no libusb, no admin).
    // Set by TryDirectNCMConnection() when device's IPv6 is found via host NIC adapter.
    bool m_isDirectNCM = false;
    std::string m_ncmAddress;  // device's link-local IPv6 on the USB-NCM interface
    uint32_t m_ncmScopeId = 0; // interface index (scope_id for link-local connect)

    // iOS 18+/26+ USB-NCM network device.
    // Set by TryNetworkRSD() when Apple's usbmuxd exposes the USB device as a
    // NETWORK-type entry (USB NCM interface). Used by CreateInstrumentConnection()
    // to call idevice_connect(m_networkDevice, servicePort) which does direct TCP
    // to the device's link-local IPv6 — no usbmuxd port-forwarding involved.
    idevice_t m_networkDevice = nullptr;
};

} // namespace instruments

#endif // INSTRUMENTS_DEVICE_CONNECTION_H
