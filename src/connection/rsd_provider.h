#ifndef INSTRUMENTS_RSD_PROVIDER_H
#define INSTRUMENTS_RSD_PROVIDER_H

#include "../../include/instruments/types.h"
#include "xpc_message.h"
#include "userspace_network.h"
#include <map>
#include <memory>
#include <string>

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

    // Connect to RSD on a tunneled device using the userspace network
    Error Connect(const std::string& tunnelAddress, uint16_t rsdPort,
                  UserspaceNetwork* network);

    // Legacy overload (returns NotSupported without network)
    Error Connect(const std::string& tunnelAddress, uint16_t rsdPort);

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

    // Parse XPC service discovery response body
    void ParseServiceResponse(const NSObject& body);
};

} // namespace instruments

#endif // INSTRUMENTS_RSD_PROVIDER_H
