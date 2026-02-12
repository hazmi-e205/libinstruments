#ifndef INSTRUMENTS_TUNNEL_QUIC_H
#define INSTRUMENTS_TUNNEL_QUIC_H

#include "../../include/instruments/types.h"
#include "userspace_network.h"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Socket types needed for CreateUdpSocket declaration
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

#ifdef INSTRUMENTS_HAS_QUIC
#include <picoquic.h>
#endif

namespace instruments {

// Tunnel parameters received from the device during handshake
struct TunnelParameters {
    std::string clientAddress;    // IPv6 address assigned to us
    std::string serverAddress;    // IPv6 address of the device
    uint16_t serverRSDPort = 0;   // RSD port on the device
    uint32_t mtu = 1280;          // Maximum transmission unit
};

// QUICTunnel - establishes a QUIC tunnel to an iOS 17+ device.
//
// The tunnel flow (using picoquic + lwIP userspace networking):
// 1. Connect to device tunnel port via QUIC (picoquic)
// 2. Open a bidirectional stream for parameter exchange
// 3. Send clientHandshakeRequest with MTU
// 4. Receive serverHandshakeResponse with addresses and RSD port
// 5. Initialize UserspaceNetwork (lwIP) with tunnel addresses
// 6. Forward QUIC datagrams ↔ lwIP IPv6 packets
//
// No root/admin privileges required - uses userspace TCP/IP stack.
class QUICTunnel {
public:
    QUICTunnel();
    ~QUICTunnel();

    // Connect to the device's tunnel port
    // address: IPv6 link-local address of the device
    // tunnelPort: port for tunnel connection (from RSD or manual pairing)
    Error Connect(const std::string& address, uint16_t tunnelPort);

    // Get tunnel parameters (valid after Connect succeeds)
    const TunnelParameters& GetParameters() const { return m_params; }

    // Close the tunnel
    void Close();

    // Check if tunnel is active
    bool IsActive() const { return m_active.load(); }

    // Get the server address for RSD connections
    const std::string& ServerAddress() const { return m_params.serverAddress; }
    uint16_t ServerRSDPort() const { return m_params.serverRSDPort; }

    // Get the userspace network for creating TCP connections through tunnel
    UserspaceNetwork* GetNetwork() { return &m_network; }

private:
    TunnelParameters m_params;
    std::atomic<bool> m_active{false};
    std::thread m_forwardThread;
    UserspaceNetwork m_network;

#ifdef INSTRUMENTS_HAS_QUIC
    picoquic_quic_t* m_quic = nullptr;
    picoquic_cnx_t* m_cnx = nullptr;
    int m_udpSocket = -1;

    // Datagram queue for outgoing packets (lwIP → QUIC)
    std::mutex m_dgMutex;
    std::vector<std::vector<uint8_t>> m_outgoingDatagrams;

    // QUIC callback
    static int QuicCallback(picoquic_cnx_t* cnx, uint64_t stream_id,
                            uint8_t* bytes, size_t length,
                            picoquic_call_back_event_t fin_or_event,
                            void* callback_ctx, void* stream_ctx);

    // Handshake: exchange parameters on stream 0
    Error PerformHandshake();

    // Forwarding loop (runs in m_forwardThread)
    void ForwardLoop();

    // Create and bind UDP socket
    Error CreateUdpSocket(const struct sockaddr* addr, socklen_t addrLen);
#endif
};

} // namespace instruments

#endif // INSTRUMENTS_TUNNEL_QUIC_H
