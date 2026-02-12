#include "service_connector.h"
#include "../util/log.h"
#include <cstring>
#include <cstdlib>

namespace instruments {

static const char* TAG = "ServiceConnector";

// Services that use SSL handshake-only mode (handshake then plaintext)
static const char* s_sslHandshakeOnlyServices[] = {
    "com.apple.instruments.remoteserver",
    "com.apple.accessibility.axAuditDaemon.remoteserver",
    "com.apple.testmanagerd.lockdown",
    "com.apple.debugserver",
    nullptr,
};

std::string ServiceConnector::GetInstrumentServiceName(IOSProtocol protocol) {
    switch (protocol) {
        case IOSProtocol::Legacy:  return ServiceName::InstrumentsPre14;
        case IOSProtocol::Modern:  return ServiceName::Instruments14To16;
        case IOSProtocol::RSD:     return ServiceName::Instruments17Plus;
    }
    return ServiceName::Instruments14To16;
}

std::string ServiceConnector::GetTestManagerServiceName(IOSProtocol protocol) {
    switch (protocol) {
        case IOSProtocol::Legacy:  return ServiceName::TestManagerD;
        case IOSProtocol::Modern:  return ServiceName::TestManagerDSecure;
        case IOSProtocol::RSD:     return ServiceName::TestManagerDSecure;
    }
    return ServiceName::TestManagerD;
}

Error ServiceConnector::StartService(idevice_t device,
                                     const std::vector<std::string>& serviceNames,
                                     lockdownd_service_descriptor_t* outService) {
    if (!device || serviceNames.empty() || !outService) {
        return Error::InvalidArgument;
    }

    lockdownd_client_t lockdown = nullptr;
    lockdownd_error_t lerr = lockdownd_client_new_with_handshake(device, &lockdown, "libinstruments");
    if (lerr != LOCKDOWN_E_SUCCESS) {
        INST_LOG_ERROR(TAG, "Failed to create lockdown client: error %d", lerr);
        return Error::ConnectionFailed;
    }

    Error result = Error::ServiceStartFailed;
    for (const auto& name : serviceNames) {
        lockdownd_service_descriptor_t service = nullptr;
        lerr = lockdownd_start_service(lockdown, name.c_str(), &service);
        if (lerr == LOCKDOWN_E_SUCCESS && service) {
            *outService = service;
            INST_LOG_INFO(TAG, "Started service: %s (port=%u, ssl=%d)",
                         name.c_str(), service->port, service->ssl_enabled);
            result = Error::Success;
            break;
        }
        INST_LOG_DEBUG(TAG, "Failed to start service %s: error %d", name.c_str(), lerr);
    }

    lockdownd_client_free(lockdown);
    return result;
}

Error ServiceConnector::StartService(idevice_t device,
                                     const std::string& serviceName,
                                     lockdownd_service_descriptor_t* outService) {
    return StartService(device, std::vector<std::string>{serviceName}, outService);
}

Error ServiceConnector::StartInstrumentService(idevice_t device,
                                                lockdownd_service_descriptor_t* outService,
                                                IOSProtocol* outProtocol) {
    IOSProtocol protocol = DetectProtocol(device);
    if (outProtocol) *outProtocol = protocol;

    // Try version-specific service names in order
    std::vector<std::string> serviceNames;
    switch (protocol) {
        case IOSProtocol::RSD:
            serviceNames.push_back(ServiceName::Instruments17Plus);
            serviceNames.push_back(ServiceName::Instruments14To16);
            break;
        case IOSProtocol::Modern:
            serviceNames.push_back(ServiceName::Instruments14To16);
            serviceNames.push_back(ServiceName::InstrumentsPre14);
            break;
        case IOSProtocol::Legacy:
            serviceNames.push_back(ServiceName::InstrumentsPre14);
            break;
    }

    return StartService(device, serviceNames, outService);
}

Error ServiceConnector::ConnectToService(idevice_t device,
                                         lockdownd_service_descriptor_t service,
                                         idevice_connection_t* outConnection) {
    if (!device || !service || !outConnection) {
        return Error::InvalidArgument;
    }

    idevice_error_t err = idevice_connect(device, service->port, outConnection);
    if (err != IDEVICE_E_SUCCESS) {
        INST_LOG_ERROR(TAG, "Failed to connect to port %u: error %d", service->port, err);
        return Error::ConnectionFailed;
    }

    return Error::Success;
}

IOSProtocol ServiceConnector::DetectProtocol(idevice_t device) {
    std::string version = GetIOSVersion(device);
    if (version.empty()) return IOSProtocol::Modern;

    int major = 0, minor = 0, patch = 0;
    ParseVersion(version, major, minor, patch);

    if (major >= 17) return IOSProtocol::RSD;
    if (major >= 14) return IOSProtocol::Modern;
    return IOSProtocol::Legacy;
}

std::string ServiceConnector::GetIOSVersion(idevice_t device) {
    lockdownd_client_t lockdown = nullptr;
    lockdownd_error_t err = lockdownd_client_new_with_handshake(device, &lockdown, "libinstruments");
    if (err != LOCKDOWN_E_SUCCESS) return "";

    plist_t verNode = nullptr;
    err = lockdownd_get_value(lockdown, nullptr, "ProductVersion", &verNode);
    lockdownd_client_free(lockdown);

    if (err != LOCKDOWN_E_SUCCESS || !verNode) return "";

    char* verStr = nullptr;
    plist_get_string_val(verNode, &verStr);
    plist_free(verNode);

    std::string result;
    if (verStr) {
        result = verStr;
        plist_mem_free(verStr);
    }
    return result;
}

void ServiceConnector::ParseVersion(const std::string& version,
                                    int& major, int& minor, int& patch) {
    major = minor = patch = 0;
    if (version.empty()) return;

    // Parse "major.minor.patch"
    size_t pos1 = version.find('.');
    if (pos1 == std::string::npos) {
        major = std::atoi(version.c_str());
        return;
    }
    major = std::atoi(version.substr(0, pos1).c_str());

    size_t pos2 = version.find('.', pos1 + 1);
    if (pos2 == std::string::npos) {
        minor = std::atoi(version.substr(pos1 + 1).c_str());
        return;
    }
    minor = std::atoi(version.substr(pos1 + 1, pos2 - pos1 - 1).c_str());
    patch = std::atoi(version.substr(pos2 + 1).c_str());
}

bool ServiceConnector::NeedsSSLHandshakeOnly(const std::string& serviceName) {
    for (int i = 0; s_sslHandshakeOnlyServices[i] != nullptr; i++) {
        if (serviceName == s_sslHandshakeOnlyServices[i]) {
            return true;
        }
    }
    return false;
}

} // namespace instruments
