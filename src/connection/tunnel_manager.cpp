#include "../../include/instruments/tunnel_manager.h"
#include "tunnel_quic.h"
#include "rsd_provider.h"
#include "service_connector.h"
#include "../util/log.h"
#include <libimobiledevice/libimobiledevice.h>
#include <chrono>
#include <memory>

namespace instruments {

static const char* TAG = "TunnelManager";

// Stored tunnel state (kept alive for the tunnel lifetime)
struct ActiveTunnel {
    std::unique_ptr<QUICTunnel> quicTunnel;
    std::unique_ptr<RSDProvider> rsdProvider;
    TunnelInfo info;
};

// Static storage for active tunnels (associated with TunnelManager)
static std::map<std::string, std::shared_ptr<ActiveTunnel>> s_activeTunnels;
static std::mutex s_tunnelStoreMutex;

TunnelManager::TunnelManager() = default;

TunnelManager::~TunnelManager() {
    StopAutoTunnel();

    // Clean up all active tunnels
    std::lock_guard<std::mutex> lock(s_tunnelStoreMutex);
    s_activeTunnels.clear();
}

bool TunnelManager::NeedsTunnel(const std::string& iosVersion) {
    int major = 0, minor = 0, patch = 0;
    ServiceConnector::ParseVersion(iosVersion, major, minor, patch);
    return NeedsTunnel(major);
}

bool TunnelManager::NeedsTunnel(int majorVersion) {
    return majorVersion >= 17;
}

Error TunnelManager::StartTunnel(const std::string& udid, TunnelInfo& outInfo) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // Check if tunnel already exists
    auto it = m_tunnels.find(udid);
    if (it != m_tunnels.end()) {
        outInfo = it->second;
        return Error::Success;
    }

    INST_LOG_INFO(TAG, "Starting tunnel for device %s", udid.c_str());

    // First we need to discover the device and get its tunnel port
    idevice_t device = nullptr;
    idevice_error_t err = idevice_new_with_options(
        &device, udid.c_str(),
        static_cast<idevice_options>(IDEVICE_LOOKUP_USBMUX));

    if (err != IDEVICE_E_SUCCESS) {
        INST_LOG_ERROR(TAG, "Failed to find device %s: error %d", udid.c_str(), err);
        return Error::DeviceNotFound;
    }

    idevice_free(device);

    // Create QUIC tunnel
    auto tunnel = std::make_unique<QUICTunnel>();

    // For iOS 17+, the tunnel port is typically discovered via lockdownd
    // or manual pairing. For now we try the default tunnel port.
    // In practice, the caller should provide the tunnel address/port.
    Error result = tunnel->Connect("", 0);

    if (result != Error::Success) {
#ifdef INSTRUMENTS_HAS_QUIC
        INST_LOG_WARN(TAG, "QUIC tunnel failed: %s", ErrorToString(result));
        INST_LOG_WARN(TAG, "Ensure device is paired and tunnel address/port is known.");
#else
        INST_LOG_WARN(TAG, "Built-in QUIC tunnel not available (picoquic not linked).");
#endif
        INST_LOG_WARN(TAG, "Use RegisterExternalTunnel() or start a tunnel externally:");
        INST_LOG_WARN(TAG, "  pymobiledevice3: python3 -m pymobiledevice3 remote start-tunnel");
        INST_LOG_WARN(TAG, "  go-ios: ios tunnel start --udid=%s", udid.c_str());
        return Error::TunnelFailed;
    }

    // Perform RSD service discovery
    auto rsd = std::make_unique<RSDProvider>();
    result = rsd->Connect(tunnel->ServerAddress(), tunnel->ServerRSDPort(),
                          tunnel->GetNetwork());

    if (result != Error::Success) {
        INST_LOG_ERROR(TAG, "RSD service discovery failed: %s", ErrorToString(result));
        tunnel->Close();
        return Error::TunnelFailed;
    }

    INST_LOG_INFO(TAG, "Tunnel established for %s: %s:%u (%zu services)",
                 udid.c_str(), tunnel->ServerAddress().c_str(),
                 tunnel->ServerRSDPort(), rsd->GetServices().size());

    // Store tunnel info
    outInfo.udid = udid;
    outInfo.address = tunnel->ServerAddress();
    outInfo.rsdPort = tunnel->ServerRSDPort();
    m_tunnels[udid] = outInfo;

    // Keep the tunnel objects alive
    auto activeTunnel = std::make_shared<ActiveTunnel>();
    activeTunnel->quicTunnel = std::move(tunnel);
    activeTunnel->rsdProvider = std::move(rsd);
    activeTunnel->info = outInfo;

    std::lock_guard<std::mutex> storeLock(s_tunnelStoreMutex);
    s_activeTunnels[udid] = activeTunnel;

    return Error::Success;
}

void TunnelManager::RegisterExternalTunnel(const std::string& udid,
                                           const std::string& address,
                                           uint16_t rsdPort) {
    std::lock_guard<std::mutex> lock(m_mutex);

    TunnelInfo info;
    info.udid = udid;
    info.address = address;
    info.rsdPort = rsdPort;
    m_tunnels[udid] = info;

    INST_LOG_INFO(TAG, "Registered external tunnel for %s: %s:%u",
                 udid.c_str(), address.c_str(), rsdPort);
}

void TunnelManager::StopTunnel(const std::string& udid) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_tunnels.erase(udid);

    // Clean up the active tunnel
    std::lock_guard<std::mutex> storeLock(s_tunnelStoreMutex);
    s_activeTunnels.erase(udid);

    INST_LOG_INFO(TAG, "Stopped tunnel for %s", udid.c_str());
}

Error TunnelManager::StartAutoTunnel() {
    if (m_autoTunnelRunning.exchange(true)) {
        return Error::Success; // Already running
    }

    INST_LOG_INFO(TAG, "Starting auto-tunnel");

    m_autoTunnelThread = std::thread([this]() {
        AutoTunnelLoop();
    });

    return Error::Success;
}

void TunnelManager::StopAutoTunnel() {
    if (!m_autoTunnelRunning.exchange(false)) return;

    INST_LOG_INFO(TAG, "Stopping auto-tunnel");

    if (m_autoTunnelThread.joinable()) {
        m_autoTunnelThread.join();
    }
}

void TunnelManager::AutoTunnelLoop() {
    while (m_autoTunnelRunning.load()) {
        // Discover connected devices
        idevice_info_t* devices = nullptr;
        int count = 0;

        if (idevice_get_device_list_extended(&devices, &count) == IDEVICE_E_SUCCESS && devices) {
            for (int i = 0; i < count; i++) {
                std::string udid = devices[i]->udid;

                // Check if tunnel already exists
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    if (m_tunnels.count(udid) > 0) continue;
                }

                // Check if device needs tunneling
                idevice_t device = nullptr;
                if (idevice_new_with_options(&device, udid.c_str(),
                    static_cast<idevice_options>(IDEVICE_LOOKUP_USBMUX)) == IDEVICE_E_SUCCESS) {

                    std::string version = ServiceConnector::GetIOSVersion(device);
                    if (NeedsTunnel(version)) {
                        TunnelInfo info;
                        StartTunnel(udid, info);
                    }
                    idevice_free(device);
                }
            }
            idevice_device_list_extended_free(devices);
        }

        // Wait before next scan
        for (int i = 0; i < 50 && m_autoTunnelRunning.load(); i++) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

std::vector<TunnelInfo> TunnelManager::GetActiveTunnels() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<TunnelInfo> result;
    for (const auto& [udid, info] : m_tunnels) {
        result.push_back(info);
    }
    return result;
}

bool TunnelManager::FindTunnel(const std::string& udid, TunnelInfo& outInfo) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tunnels.find(udid);
    if (it != m_tunnels.end()) {
        outInfo = it->second;
        return true;
    }
    return false;
}

} // namespace instruments
