#include "../../include/instruments/tunnel_manager.h"
#include "tunnel_quic.h"
#include "rsd_provider.h"
#include "service_connector.h"
#include "../util/log.h"
#include <libimobiledevice/libimobiledevice.h>
#include <chrono>
#include <memory>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#endif

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

    // Return cached tunnel if already active
    auto it = m_tunnels.find(udid);
    if (it != m_tunnels.end()) {
        outInfo = it->second;
        return Error::Success;
    }

    INST_LOG_INFO(TAG, "StartTunnel: device %s", udid.c_str());

    // ── Step 1: Reach the device via USB ────────────────────────────────────
    idevice_t device = nullptr;
    idevice_error_t ierr = idevice_new_with_options(
        &device, udid.c_str(),
        static_cast<idevice_options>(IDEVICE_LOOKUP_USBMUX));

    if (ierr != IDEVICE_E_SUCCESS || !device) {
        INST_LOG_ERROR(TAG, "StartTunnel: device %s not found via USB (error %d)",
                      udid.c_str(), ierr);
        return Error::DeviceNotFound;
    }

    // ── Step 2: Check iOS version ────────────────────────────────────────────
    std::string version = ServiceConnector::GetIOSVersion(device);
    if (!NeedsTunnel(version)) {
        idevice_free(device);
        INST_LOG_INFO(TAG, "StartTunnel: device %s (iOS %s) does not need a tunnel",
                     udid.c_str(), version.c_str());
        return Error::NotSupported;
    }

    // ── Step 3: Try USB RSD (idevice_connect to port 58783 via usbmuxd) ─────
    // Reference: go-ios connects directly to RSD port 58783 for USB iOS 17+.
    // This works because usbmuxd forwards any TCP port to the device.
    RSDProvider rsd;
    Error rsdErr = rsd.ConnectViaIDevice(device, RSDProvider::DefaultPort);

    if (rsdErr == Error::Success && !rsd.GetServices().empty()) {
        INST_LOG_INFO(TAG, "StartTunnel: USB RSD succeeded for %s (iOS %s) — %zu services",
                     udid.c_str(), version.c_str(), rsd.GetServices().size());

        outInfo.udid = udid;
        outInfo.address = "";           // empty = USB direct; use Instruments::Create()
        outInfo.rsdPort = RSDProvider::DefaultPort;
        outInfo.isUsbDirect = true;
        m_tunnels[udid] = outInfo;

        auto activeTunnel = std::make_shared<ActiveTunnel>();
        activeTunnel->info = outInfo;
        // No QUICTunnel/RSDProvider to keep alive for USB path —
        // each Instruments::Create() call rediscovers via idevice_connect.

        std::lock_guard<std::mutex> storeLock(s_tunnelStoreMutex);
        s_activeTunnels[udid] = activeTunnel;

        idevice_free(device);
        return Error::Success;
    }

    idevice_free(device);
    INST_LOG_WARN(TAG, "StartTunnel: USB RSD failed for %s (iOS %s): %s",
                 udid.c_str(), version.c_str(), ErrorToString(rsdErr));

    // ── Step 4: Try built-in USB QUIC tunnel (iOS 18+ CoreDevice tunnel) ─────
#ifdef INSTRUMENTS_HAS_QUIC
    INST_LOG_INFO(TAG, "StartTunnel: trying USB CoreDevice QUIC tunnel for %s...", udid.c_str());

    // Re-open device for QUIC tunnel
    idevice_t device2 = nullptr;
    if (idevice_new_with_options(&device2, udid.c_str(),
            static_cast<idevice_options>(IDEVICE_LOOKUP_USBMUX)) == IDEVICE_E_SUCCESS) {

        auto quicTunnel = std::make_unique<QUICTunnel>();
        Error quicErr = quicTunnel->ConnectViaUSB(device2);
        idevice_free(device2);  // always free here; ConnectViaUSB does NOT take ownership
        device2 = nullptr;

        if (quicErr == Error::Success) {
            // RSD handshake through the tunnel
            int rsdFd = quicTunnel->CreateTunnelSocket(
                quicTunnel->ServerAddress(), quicTunnel->ServerRSDPort());
            if (rsdFd >= 0) {
                RSDProvider rsd2;
                Error rsdErr2 = rsd2.ConnectViaFd(rsdFd);
#ifdef _WIN32
                closesocket(static_cast<SOCKET>(rsdFd));
#else
                ::close(rsdFd);
#endif
                if (rsdErr2 == Error::Success && !rsd2.GetServices().empty()) {
                    INST_LOG_INFO(TAG, "StartTunnel: USB QUIC tunnel succeeded for %s — %zu services",
                                 udid.c_str(), rsd2.GetServices().size());

                    outInfo.udid = udid;
                    outInfo.address = quicTunnel->ServerAddress();
                    outInfo.rsdPort = quicTunnel->ServerRSDPort();
                    outInfo.isUsbDirect = false;
                    m_tunnels[udid] = outInfo;

                    auto activeTunnel = std::make_shared<ActiveTunnel>();
                    activeTunnel->quicTunnel = std::move(quicTunnel);
                    activeTunnel->info = outInfo;

                    std::lock_guard<std::mutex> storeLock(s_tunnelStoreMutex);
                    s_activeTunnels[udid] = activeTunnel;

                    return Error::Success;
                }
            }
        } else {
            INST_LOG_WARN(TAG, "StartTunnel: USB QUIC tunnel failed for %s: %s",
                         udid.c_str(), ErrorToString(quicErr));
        }
    }
#endif // INSTRUMENTS_HAS_QUIC

    // ── Step 5: Suggest external tunnel ──────────────────────────────────────
#ifndef INSTRUMENTS_HAS_QUIC
    INST_LOG_WARN(TAG, "StartTunnel: built-in QUIC tunnel not available (build without INSTRUMENTS_HAS_QUIC).");
#endif
    INST_LOG_WARN(TAG, "StartTunnel: no automatic tunnel available for %s. "
                 "Use RegisterExternalTunnel() with an external tool:", udid.c_str());
    INST_LOG_WARN(TAG, "  pymobiledevice3: python3 -m pymobiledevice3 remote start-tunnel");
    INST_LOG_WARN(TAG, "  go-ios: ios tunnel start --udid=%s", udid.c_str());

    return Error::TunnelFailed;
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
