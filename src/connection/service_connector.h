#ifndef INSTRUMENTS_SERVICE_CONNECTOR_H
#define INSTRUMENTS_SERVICE_CONNECTOR_H

#include "../../include/instruments/types.h"
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <string>
#include <vector>

namespace instruments {

// ServiceConnector - helper for starting lockdown services on an iOS device.
// Handles version-specific service name selection and SSL configuration.
class ServiceConnector {
public:
    // Determine the correct instrument service name based on iOS version
    static std::string GetInstrumentServiceName(IOSProtocol protocol);

    // Determine the testmanagerd service name
    static std::string GetTestManagerServiceName(IOSProtocol protocol);

    // Start a lockdown service by name.
    // Tries each service name in order until one succeeds.
    static Error StartService(idevice_t device,
                              const std::vector<std::string>& serviceNames,
                              lockdownd_service_descriptor_t* outService);

    // Start a lockdown service by name (single name)
    static Error StartService(idevice_t device,
                              const std::string& serviceName,
                              lockdownd_service_descriptor_t* outService);

    // Start the instruments service (auto-detects version)
    static Error StartInstrumentService(idevice_t device,
                                        lockdownd_service_descriptor_t* outService,
                                        IOSProtocol* outProtocol = nullptr);

    // Connect to a service and get a raw connection
    static Error ConnectToService(idevice_t device,
                                  lockdownd_service_descriptor_t service,
                                  idevice_connection_t* outConnection);

    // Detect the iOS version from the device and return the protocol level
    static IOSProtocol DetectProtocol(idevice_t device);

    // Get the iOS version string (e.g., "16.4.1")
    static std::string GetIOSVersion(idevice_t device);

    // Parse version string into major/minor/patch
    static void ParseVersion(const std::string& version,
                            int& major, int& minor, int& patch);

    // Check if SSL handshake-only is needed for a service
    static bool NeedsSSLHandshakeOnly(const std::string& serviceName);
};

} // namespace instruments

#endif // INSTRUMENTS_SERVICE_CONNECTOR_H
