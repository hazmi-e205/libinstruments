#ifndef INSTRUMENTS_TUNNEL_MANAGER_H
#define INSTRUMENTS_TUNNEL_MANAGER_H

#include "types.h"
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace instruments {

// TunnelManager - manages tunnel lifecycle for iOS 17+ devices.
//
// For iOS 17+, a tunnel is required to communicate with the device.
// The tunnel is established over QUIC and provides an IPv6 network
// interface for service connections.
//
// There are two modes:
// 1. Manual: Create tunnels for specific devices
// 2. Auto: Automatically discover and tunnel all connected devices
//
// For iOS 16 and below, no tunnel is needed (direct USB via usbmuxd).
//
// If the built-in QUIC tunnel is not available (msquic not linked),
// the TunnelManager can work with external tunnel providers:
// - pymobiledevice3: `python3 -m pymobiledevice3 remote start-tunnel`
// - go-ios: `ios tunnel start`
// These external tools create the tunnel and output the address/port.
class TunnelManager {
public:
    TunnelManager();
    ~TunnelManager();

    // Start a tunnel for a specific device
    Error StartTunnel(const std::string& udid, TunnelInfo& outInfo);

    // Register an externally-created tunnel
    void RegisterExternalTunnel(const std::string& udid,
                               const std::string& address, uint16_t rsdPort);

    // Stop a tunnel for a device
    void StopTunnel(const std::string& udid);

    // Auto-tunnel: periodically discover devices and create tunnels
    Error StartAutoTunnel();
    void StopAutoTunnel();
    bool IsAutoTunnelRunning() const { return m_autoTunnelRunning.load(); }

    // Get active tunnels
    std::vector<TunnelInfo> GetActiveTunnels() const;

    // Find tunnel for a device
    bool FindTunnel(const std::string& udid, TunnelInfo& outInfo) const;

    // Check if a device needs tunneling based on its iOS version
    static bool NeedsTunnel(const std::string& iosVersion);
    static bool NeedsTunnel(int majorVersion);

    // Configure userspace TUN mode (no root required)
    void SetUserspaceTUN(bool enable) { m_useUserspaceTUN = enable; }
    bool IsUserspaceTUN() const { return m_useUserspaceTUN; }

private:
    void AutoTunnelLoop();

    mutable std::mutex m_mutex;
    std::map<std::string, TunnelInfo> m_tunnels;

    std::atomic<bool> m_autoTunnelRunning{false};
    std::thread m_autoTunnelThread;
    bool m_useUserspaceTUN = false;
};

} // namespace instruments

#endif // INSTRUMENTS_TUNNEL_MANAGER_H
