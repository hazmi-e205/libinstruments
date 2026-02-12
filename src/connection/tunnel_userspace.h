#ifndef INSTRUMENTS_TUNNEL_USERSPACE_H
#define INSTRUMENTS_TUNNEL_USERSPACE_H

#include "../../include/instruments/types.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace instruments {

// UserspaceTunnel - implements a userspace TUN device for cross-platform
// TCP tunneling without requiring root/admin privileges.
//
// Instead of creating an OS-level TUN device, this implementation provides
// a TCP proxy that tunnels connections through the QUIC tunnel by:
// 1. Listening on a local TCP port
// 2. Accepting connections
// 3. Routing TCP data through the userspace network stack to the device
//
// On macOS/Linux: Can use OS-level utun/tun device (requires root)
// On Windows: Uses userspace TCP relay
// Cross-platform: Always available via TCP relay mode
class UserspaceTunnel {
public:
    UserspaceTunnel();
    ~UserspaceTunnel();

    // Start a TCP relay from localPort to remoteAddr:remotePort through the tunnel
    // Returns the actual local port used (may differ if 0 was requested)
    Error StartTCPRelay(uint16_t localPort, const std::string& remoteAddr,
                        uint16_t remotePort, uint16_t& outLocalPort);

    // Stop a specific relay
    void StopTCPRelay(uint16_t localPort);

    // Stop all relays
    void StopAll();

    // Check if any relay is active
    bool IsActive() const { return m_active.load(); }

private:
    struct TCPRelay {
        uint16_t localPort = 0;
        std::string remoteAddr;
        uint16_t remotePort = 0;
        std::atomic<bool> running{false};
        std::thread acceptThread;
    };

    std::vector<std::unique_ptr<TCPRelay>> m_relays;
    std::atomic<bool> m_active{false};
};

} // namespace instruments

#endif // INSTRUMENTS_TUNNEL_USERSPACE_H
