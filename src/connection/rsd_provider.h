#ifndef INSTRUMENTS_RSD_PROVIDER_H
#define INSTRUMENTS_RSD_PROVIDER_H

#include "../../include/instruments/types.h"
#include "xpc_message.h"
#include "userspace_network.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <libimobiledevice/libimobiledevice.h>

namespace instruments {

// RSD Service entry discovered from the device
struct RSDServiceEntry {
    std::string name;
    uint16_t port = 0;
};

// RSDProvider - Remote Service Discovery for iOS 17+.
// Connects to the device's RSD port (58783) via tunnel and discovers available services.
//
// RSD Protocol:
// 1. TCP connect to device:58783 through userspace tunnel (lwIP)
// 2. HTTP/2 connection preface + SETTINGS exchange
// 3. XPC InitHandshake on HTTP/2 streams
// 4. Receive XPC service discovery response (UDID + service port map)
// 5. Use port mappings to connect to individual services
class RSDProvider {
public:
    RSDProvider();
    ~RSDProvider();

    // Connect to RSD on a tunneled device using the userspace network (self-managed QUIC)
    Error Connect(const std::string& tunnelAddress, uint16_t rsdPort,
                  UserspaceNetwork* network);

    // Legacy overload (returns NotSupported without network)
    Error Connect(const std::string& tunnelAddress, uint16_t rsdPort);

    // Connect to RSD using a plain OS TCP socket (for external tunnel tools:
    // pymobiledevice3 remote start-tunnel, go-ios tunnel start, etc.).
    // The external tool creates a real network interface; the device is
    // reachable at the given address via normal OS TCP.
    Error ConnectDirect(const std::string& address, uint16_t rsdPort);

    // Connect to RSD directly via USB using idevice_connect() (iOS 17+ USB path).
    // Uses usbmuxd TCP forwarding to reach the device's RSD port — no tunnel or
    // external tools required. Reference: go-ios directly connects to port 58783.
    Error ConnectViaIDevice(idevice_t device, uint16_t rsdPort = DefaultPort);

    // Connect to RSD using a pre-connected OS socket fd.
    // Used for USB QUIC tunnel path (fd from QUICTunnel::CreateTunnelSocket).
    // The fd is NOT closed by this method; caller is responsible.
    Error ConnectViaFd(int socketFd);

    // Get the UDID from the RSD handshake
    const std::string& GetUDID() const { return m_udid; }

    // Get all discovered services
    const std::map<std::string, RSDServiceEntry>& GetServices() const { return m_services; }

    // Find a specific service, returns port or 0 if not found
    uint16_t FindServicePort(const std::string& serviceName) const;

    // Find a service with fallback to shim
    uint16_t FindServicePortWithShim(const std::string& serviceName) const;

    // RSD default port
    static constexpr uint16_t DefaultPort = 58783;

private:
    std::string m_udid;
    std::map<std::string, RSDServiceEntry> m_services;
    std::shared_ptr<UserspaceTcpConnection> m_tcpConn;

    // Shared HTTP/2 + XPC handshake logic used by ConnectDirect and ConnectViaIDevice.
    // sendFn(data)     → sends bytes, returns false on error
    // recvMoreFn(buf)  → appends received bytes to buf, returns false on disconnect
    Error DoRSDHandshake(
        std::function<bool(const std::vector<uint8_t>&)> sendFn,
        std::function<bool(std::vector<uint8_t>&)> recvMoreFn);

    // Parse XPC service discovery response body
    void ParseServiceResponse(const NSObject& body);
};

} // namespace instruments

#endif // INSTRUMENTS_RSD_PROVIDER_H
