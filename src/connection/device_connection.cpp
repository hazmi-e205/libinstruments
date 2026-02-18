#include "../../include/instruments/device_connection.h"
#include "service_connector.h"
#include "../util/log.h"

namespace instruments {

static const char* TAG = "DeviceConnection";

DeviceConnection::DeviceConnection() = default;

DeviceConnection::~DeviceConnection() {
    if (m_lockdown && m_ownsLockdown) {
        lockdownd_client_free(m_lockdown);
        m_lockdown = nullptr;
    }
    if (m_device && m_ownsDevice) {
        idevice_free(m_device);
        m_device = nullptr;
    }
}

std::shared_ptr<DeviceConnection> DeviceConnection::FromUDID(const std::string& udid) {
    auto conn = std::shared_ptr<DeviceConnection>(new DeviceConnection());

    idevice_error_t err = idevice_new_with_options(
        &conn->m_device, udid.c_str(),
        static_cast<idevice_options>(IDEVICE_LOOKUP_USBMUX | IDEVICE_LOOKUP_NETWORK));

    if (err != IDEVICE_E_SUCCESS) {
        INST_LOG_ERROR(TAG, "Failed to create device for UDID %s: error %d",
                      udid.c_str(), err);
        return nullptr;
    }

    conn->m_ownsDevice = true;
    conn->m_iosVersion = ServiceConnector::GetIOSVersion(conn->m_device);
    conn->m_protocol = ServiceConnector::DetectProtocol(conn->m_device);

    INST_LOG_INFO(TAG, "Connected to %s (iOS %s, protocol=%d)",
                 udid.c_str(), conn->m_iosVersion.c_str(),
                 static_cast<int>(conn->m_protocol));

    return conn;
}

std::shared_ptr<DeviceConnection> DeviceConnection::FromDevice(idevice_t device) {
    if (!device) return nullptr;

    auto conn = std::shared_ptr<DeviceConnection>(new DeviceConnection());
    conn->m_device = device;
    conn->m_ownsDevice = false;
    conn->m_iosVersion = ServiceConnector::GetIOSVersion(device);
    conn->m_protocol = ServiceConnector::DetectProtocol(device);

    INST_LOG_INFO(TAG, "Using existing device (iOS %s, protocol=%d)",
                 conn->m_iosVersion.c_str(), static_cast<int>(conn->m_protocol));

    return conn;
}

std::shared_ptr<DeviceConnection> DeviceConnection::FromDevice(idevice_t device, lockdownd_client_t lockdown) {
    if (!device) return nullptr;

    auto conn = std::shared_ptr<DeviceConnection>(new DeviceConnection());
    conn->m_device = device;
    conn->m_ownsDevice = false;
    conn->m_lockdown = lockdown;
    conn->m_ownsLockdown = false;
    conn->m_iosVersion = ServiceConnector::GetIOSVersion(device, lockdown);
    conn->m_protocol = ServiceConnector::DetectProtocol(device, lockdown);

    INST_LOG_INFO(TAG, "Using existing device with lockdown (iOS %s, protocol=%d)",
                 conn->m_iosVersion.c_str(), static_cast<int>(conn->m_protocol));

    return conn;
}

DeviceInfo DeviceConnection::GetDeviceInfo() {
    if (m_deviceInfoLoaded) return m_deviceInfo;

    m_deviceInfo.version = m_iosVersion;
    m_deviceInfo.protocol = m_protocol;

    ServiceConnector::ParseVersion(m_iosVersion,
                                   m_deviceInfo.versionMajor,
                                   m_deviceInfo.versionMinor,
                                   m_deviceInfo.versionPatch);

    // Get UDID and device name
    if (m_device) {
        lockdownd_client_t lockdown = m_lockdown;
        bool owns_lockdown = false;

        // Create temporary lockdown if not already provided
        // NOTE: For remote devices, caller should provide lockdown client
        if (!lockdown) {
            lockdownd_error_t lerr = lockdownd_client_new_with_handshake(m_device, &lockdown, "libinstruments");
            if (lerr != LOCKDOWN_E_SUCCESS) {
                lockdown = nullptr;
            }
            owns_lockdown = true;
        }

        if (lockdown) {
            // UDID
            char* udid = nullptr;
            idevice_get_udid(m_device, &udid);
            if (udid) {
                m_deviceInfo.udid = udid;
                free(udid);
            }

            // Device name
            plist_t nameNode = nullptr;
            if (lockdownd_get_value(lockdown, nullptr, "DeviceName", &nameNode) == LOCKDOWN_E_SUCCESS && nameNode) {
                char* name = nullptr;
                plist_get_string_val(nameNode, &name);
                if (name) {
                    m_deviceInfo.name = name;
                    plist_mem_free(name);
                }
                plist_free(nameNode);
            }

            if (owns_lockdown) {
                lockdownd_client_free(lockdown);
            }
        }
    }

    m_deviceInfoLoaded = true;
    return m_deviceInfo;
}

std::unique_ptr<DTXConnection> DeviceConnection::CreateInstrumentConnection() {
    lockdownd_service_descriptor_t service = nullptr;
    IOSProtocol protocol;
    Error err = ServiceConnector::StartInstrumentService(m_device, &service, &protocol, m_lockdown);
    if (err != Error::Success || !service) {
        if (m_protocol == IOSProtocol::RSD) {
            INST_LOG_ERROR(TAG, "Failed to start instrument service — iOS 17+ requires a tunnel connection (QUIC or remote usbmux proxy)");
        } else {
            INST_LOG_ERROR(TAG, "Failed to start instrument service");
        }
        return nullptr;
    }

    std::string serviceName = ServiceConnector::GetInstrumentServiceName(protocol);
    bool sslHandshakeOnly = ServiceConnector::NeedsSSLHandshakeOnly(serviceName) && service->ssl_enabled;

    INST_LOG_DEBUG(TAG, "Creating DTX connection: service=%s, sslHandshakeOnly=%d, ssl_enabled=%d",
                  serviceName.c_str(), sslHandshakeOnly ? 1 : 0, service->ssl_enabled ? 1 : 0);

    auto conn = DTXConnection::Create(m_device, service, sslHandshakeOnly);
    lockdownd_service_descriptor_free(service);

    if (!conn) {
        INST_LOG_ERROR(TAG, "Failed to create DTX connection");
        return nullptr;
    }

    Error connectErr = conn->Connect();
    if (connectErr != Error::Success) {
        INST_LOG_ERROR(TAG, "Failed to connect DTX: %s", ErrorToString(connectErr));
        return nullptr;
    }

    return conn;
}

std::unique_ptr<DTXConnection> DeviceConnection::CreateServiceConnection(const std::string& serviceName) {
    lockdownd_service_descriptor_t service = nullptr;
    Error err = ServiceConnector::StartService(m_device, serviceName, &service, m_lockdown);
    if (err != Error::Success || !service) {
        return nullptr;
    }

    bool sslHandshakeOnly = ServiceConnector::NeedsSSLHandshakeOnly(serviceName) && service->ssl_enabled;
    auto conn = DTXConnection::Create(m_device, service, sslHandshakeOnly);
    lockdownd_service_descriptor_free(service);

    if (!conn) return nullptr;

    Error connectErr = conn->Connect();
    if (connectErr != Error::Success) return nullptr;

    return conn;
}

Error DeviceConnection::StartService(const std::string& serviceId,
                                     lockdownd_service_descriptor_t* outService) {
    return ServiceConnector::StartService(m_device, serviceId, outService, m_lockdown);
}

} // namespace instruments
