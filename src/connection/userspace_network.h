#ifndef INSTRUMENTS_USERSPACE_NETWORK_H
#define INSTRUMENTS_USERSPACE_NETWORK_H

#include "../../include/instruments/types.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Forward-declare lwIP types to avoid leaking lwIP headers
struct netif;
struct tcp_pcb;
struct pbuf;
struct ip6_addr;
typedef struct ip6_addr ip6_addr_t;

namespace instruments {

// Represents a single TCP connection through the userspace network stack.
// Data flows: application ←→ lwIP TCP ←→ IPv6 packets ←→ QUIC datagrams ←→ device
class UserspaceTcpConnection {
public:
    ~UserspaceTcpConnection();

    // Send data over this TCP connection (non-blocking, queues in lwIP)
    Error Send(const uint8_t* data, size_t length);

    // Set callback for received data
    using RecvCallback = std::function<void(const uint8_t* data, size_t length)>;
    void SetRecvCallback(RecvCallback cb) { m_recvCb = std::move(cb); }

    // Set callback for connection errors
    using ErrorCallback = std::function<void(Error err)>;
    void SetErrorCallback(ErrorCallback cb) { m_errorCb = std::move(cb); }

    // Close this connection
    void Close();

    bool IsConnected() const { return m_connected.load(); }

private:
    friend class UserspaceNetwork;
    UserspaceTcpConnection() = default;

    tcp_pcb* m_pcb = nullptr;
    std::atomic<bool> m_connected{false};
    RecvCallback m_recvCb;
    ErrorCallback m_errorCb;

    // lwIP callbacks (static, casted through arg pointer)
    static int8_t OnConnected(void* arg, tcp_pcb* tpcb, int8_t err);
    static int8_t OnRecv(void* arg, tcp_pcb* tpcb, struct pbuf* p, int8_t err);
    static int8_t OnSent(void* arg, tcp_pcb* tpcb, uint16_t len);
    static void OnError(void* arg, int8_t err);
};

// UserspaceNetwork - bridges lwIP userspace TCP/IP stack with QUIC datagrams.
//
// Architecture:
//   Application TCP connections
//        ↕ (lwIP raw TCP API)
//   lwIP TCP/IP stack (NO_SYS mode)
//        ↕ (IPv6 packets)
//   UserspaceNetwork netif
//        ↕ (output callback)
//   QUIC datagram frames
//        ↕ (picoquic)
//   iOS device tunnel
//
// All lwIP operations must happen on the same thread (the forwarding thread).
// The Poll() method drives lwIP timers and should be called regularly.
class UserspaceNetwork {
public:
    UserspaceNetwork();
    ~UserspaceNetwork();

    // Initialize the lwIP stack with tunnel parameters.
    // localIPv6: our IPv6 address (from tunnel handshake clientAddress)
    // gatewayIPv6: device IPv6 address (from tunnel handshake serverAddress)
    // mtu: maximum transmission unit (typically 1280 for QUIC tunnel)
    Error Init(const std::string& localIPv6, const std::string& gatewayIPv6, uint32_t mtu);

    // Feed an incoming IPv6 packet (received from QUIC datagram) into lwIP
    void InjectPacket(const uint8_t* data, size_t len);

    // Set callback for outgoing packets (lwIP → QUIC datagram)
    using OutputCallback = std::function<void(const uint8_t* data, size_t len)>;
    void SetOutputCallback(OutputCallback cb) { m_outputCb = std::move(cb); }

    // Create a TCP connection through the tunnel (async)
    // The connection becomes usable after the connect callback fires.
    Error TcpConnect(const std::string& destIPv6, uint16_t port,
                     std::shared_ptr<UserspaceTcpConnection>& out);

    // Poll the lwIP stack - must be called periodically from the forwarding thread
    void Poll();

    // Shutdown the network stack
    void Shutdown();

    bool IsInitialized() const { return m_initialized.load(); }

private:
    // lwIP netif output function - called when lwIP wants to send an IPv6 packet
    static int8_t NetifOutput(struct netif* netif, struct pbuf* p,
                              const ip6_addr_t* ipaddr);

    // lwIP netif init function
    static int8_t NetifInit(struct netif* netif);

    struct netif* m_netif = nullptr;
    OutputCallback m_outputCb;
    std::atomic<bool> m_initialized{false};
    std::vector<std::shared_ptr<UserspaceTcpConnection>> m_connections;
    std::mutex m_connMutex;
};

} // namespace instruments

#endif // INSTRUMENTS_USERSPACE_NETWORK_H
