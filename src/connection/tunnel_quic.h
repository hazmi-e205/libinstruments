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

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>

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

// QUICTunnel - establishes a userspace tunnel to an iOS 17+ device.
//
// Supports three transport modes:
//   Wi-Fi (UDP):    Connect(address, port)              - picoquic over UDP socket
//   USB CDTunnel:   ConnectViaCoreDeviceProxy(device)   - CDTunnel JSON + raw IPv6 TCP (iOS 17.4+, 18+/26+)
//   USB QUIC:       ConnectViaUSB(device)               - picoquic over framed lockdown stream (iOS 17.x only)
//
// After connect, CreateTunnelSocket(destIPv6, port) returns an OS socket fd
// bridged through the lwIP TCP stack to the device. The fd is a normal connected
// socket usable by DTXTransport, RSDProvider, etc.
//
// Preferred USB path for iOS 17.4+ and all iOS 18+/26+:
//   ConnectViaCoreDeviceProxy() — uses com.apple.internal.devicecompute.CoreDeviceProxy
//   lockdown service (standard usbmuxd, works with iTunes). No admin required.
//   Sends CDTunnel JSON handshake, then pumps raw IPv6 packets over the SSL TCP connection.
class QUICTunnel {
public:
    QUICTunnel();
    ~QUICTunnel();

    // Connect to the device's tunnel port over Wi-Fi/network (UDP)
    Error Connect(const std::string& address, uint16_t tunnelPort);

    // Connect via USB using CoreDeviceProxy CDTunnel (iOS 17.4+, iOS 18+/26+).
    // Starts "com.apple.internal.devicecompute.CoreDeviceProxy" lockdown service,
    // performs CDTunnel JSON handshake, then pumps raw IPv6 packets over SSL TCP.
    // Works with iTunes (no Apple Devices app required). No admin privileges needed.
    Error ConnectViaCoreDeviceProxy(idevice_t device);

    // Connect via USB using the CoreDevice QUIC tunnel service (iOS 17.x only).
    // Starts "com.apple.internal.dt.coredevice.free.tunnelservice" lockdown service,
    // runs QUIC over the framed TCP stream.
    Error ConnectViaUSB(idevice_t device);

    // Create a TCP socket to [destIPv6]:port through the tunnel.
    // Returns a pre-connected OS socket fd (caller owns it, must close).
    // Returns -1 on error. Thread-safe.
    int CreateTunnelSocket(const std::string& destIPv6, uint16_t port);

    // Get tunnel parameters (valid after Connect/ConnectViaUSB succeeds)
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

    // USB stream state (always present)
    bool m_isUsb = false;        // true for QUIC-over-framed-stream path (iOS 17.x)
    bool m_isCDTunnel = false;   // true for CDTunnel path (iOS 17.4+, iOS 18+/26+)
    idevice_connection_t m_idevConn = nullptr;
    std::vector<uint8_t> m_streamRecvBuf;   // partial packet buffer for USB QUIC framing
    std::vector<uint8_t> m_cdRecvBuf;       // partial IPv6 packet buffer for CDTunnel

    // CDTunnel output queue: lwIP -> device send (locked by m_cdOutputMutex)
    std::mutex m_cdOutputMutex;
    std::vector<std::vector<uint8_t>> m_cdOutputQueue;

    // Fake sockaddr for USB path (picoquic needs addresses even over stream)
    struct sockaddr_storage m_fakeLocalAddr;
    struct sockaddr_storage m_fakeRemoteAddr;

    // Task queue: SubmitToLoop() allows any thread to schedule work on
    // the ForwardLoop thread (required for lwIP thread safety)
    std::mutex m_taskMutex;
    std::vector<std::function<void()>> m_pendingTasks;
    void SubmitToLoop(std::function<void()> fn);
    void DrainTasks();  // called from ForwardLoop

    // Socket bridges: OS socket pair <-> lwIP TCP connection
    struct SocketBridge {
        int externalFd = -1;  // fds[0]: returned to caller
        int internalFd = -1;  // fds[1]: owned by ForwardLoop
        std::shared_ptr<UserspaceTcpConnection> lwipConn;
        std::atomic<bool> connected{false};
    };
    std::mutex m_bridgeMutex;
    std::vector<std::shared_ptr<SocketBridge>> m_bridges;
    void DrainBridges();  // called from ForwardLoop

#ifdef INSTRUMENTS_HAS_QUIC
    picoquic_quic_t* m_quic = nullptr;
    picoquic_cnx_t* m_cnx = nullptr;
    int m_udpSocket = -1;  // UDP mode only; -1 for USB mode

    // Datagram queue for outgoing packets (lwIP -> QUIC)
    std::mutex m_dgMutex;
    std::vector<std::vector<uint8_t>> m_outgoingDatagrams;

    // QUIC callback
    static int QuicCallback(picoquic_cnx_t* cnx, uint64_t stream_id,
                            uint8_t* bytes, size_t length,
                            picoquic_call_back_event_t fin_or_event,
                            void* callback_ctx, void* stream_ctx);

    // CDTunnel handshake: exchange parameters over CDTunnel JSON protocol
    Error PerformCDTunnelHandshake();

    // Handshake: exchange parameters on stream 0 (QUIC path)
    Error PerformHandshake();

    // Forwarding loop (runs in m_forwardThread)
    void ForwardLoop();

    // Create and bind UDP socket (Wi-Fi mode)
    Error CreateUdpSocket(const struct sockaddr* addr, socklen_t addrLen);

    // USB stream framing helpers (iOS 17.x lockdown stream: 4B BE length prefix)
    Error SendFramed(const uint8_t* buf, size_t len);
    int RecvFramedPartial(uint8_t* buf, size_t maxLen);  // returns pkt len or 0

    // Create OS socket pair for CreateTunnelSocket (platform-specific)
    static int MakeLoopbackPair(int fds[2]);
#endif
};

} // namespace instruments

#endif // INSTRUMENTS_TUNNEL_QUIC_H
