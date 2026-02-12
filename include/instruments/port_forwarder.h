#ifndef INSTRUMENTS_PORT_FORWARDER_H
#define INSTRUMENTS_PORT_FORWARDER_H

#include "device_connection.h"
#include "types.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace instruments {

// PortForwarder - forwards TCP ports from the host to the iOS device.
// Uses usbmuxd to establish connections to device ports.
//
// Usage:
//   auto fwd = PortForwarder(connection);
//   fwd.Forward(8100, 8100);   // host:8100 -> device:8100
//   fwd.Forward(9100, 9100);   // host:9100 -> device:9100
//   // ... later ...
//   fwd.StopAll();
class PortForwarder {
public:
    explicit PortForwarder(std::shared_ptr<DeviceConnection> connection);
    ~PortForwarder();

    // Forward a port (host:hostPort -> device:devicePort)
    // If hostPort is 0, a random port is assigned.
    // Returns the actual host port used via outActualPort.
    Error Forward(uint16_t hostPort, uint16_t devicePort,
                  uint16_t* outActualPort = nullptr);

    // Stop forwarding a specific host port
    void StopForward(uint16_t hostPort);

    // Stop all forwarding
    void StopAll();

    // Check if any forwarding is active
    bool IsActive() const;

    // Get list of active forwarded ports (host -> device)
    std::map<uint16_t, uint16_t> GetForwardedPorts() const;

private:
    struct ForwardEntry {
        uint16_t hostPort = 0;
        uint16_t devicePort = 0;
        std::atomic<bool> running{false};
        std::thread acceptThread;
#ifdef _WIN32
        uintptr_t listenSocket = ~static_cast<uintptr_t>(0);
#else
        int listenSocket = -1;
#endif
    };

    void AcceptLoop(ForwardEntry* entry);
    void RelayConnection(int clientFd, uint16_t devicePort);

    std::shared_ptr<DeviceConnection> m_connection;
    std::vector<std::unique_ptr<ForwardEntry>> m_entries;
    mutable std::mutex m_mutex;
};

} // namespace instruments

#endif // INSTRUMENTS_PORT_FORWARDER_H
