#include "../include/instruments/instruments.h"
#include "util/log.h"

namespace instruments {

Instruments::Instruments(std::shared_ptr<DeviceConnection> connection)
    : m_connection(std::move(connection))
{
}

Instruments::~Instruments() {
    // Stop all running services
    if (m_fps) m_fps->Stop();
    if (m_performance) m_performance->Stop();
    if (m_xctest) m_xctest->Stop();
    if (m_wda) m_wda->Stop();
    if (m_ports) m_ports->StopAll();
}

std::shared_ptr<Instruments> Instruments::Create(const std::string& udid) {
    auto connection = DeviceConnection::FromUDID(udid);
    if (!connection) return nullptr;
    return std::shared_ptr<Instruments>(new Instruments(std::move(connection)));
}

std::shared_ptr<Instruments> Instruments::Create(idevice_t device) {
    auto connection = DeviceConnection::FromDevice(device);
    if (!connection) return nullptr;
    return std::shared_ptr<Instruments>(new Instruments(std::move(connection)));
}

std::shared_ptr<Instruments> Instruments::Create(idevice_t device, lockdownd_client_t lockdown) {
    auto connection = DeviceConnection::FromDevice(device, lockdown);
    if (!connection) return nullptr;
    return std::shared_ptr<Instruments>(new Instruments(std::move(connection)));
}

ProcessService& Instruments::Process() {
    if (!m_process) {
        m_process = std::make_unique<ProcessService>(m_connection);
    }
    return *m_process;
}

PerformanceService& Instruments::Performance() {
    if (!m_performance) {
        m_performance = std::make_unique<PerformanceService>(m_connection);
    }
    return *m_performance;
}

FPSService& Instruments::FPS() {
    if (!m_fps) {
        m_fps = std::make_unique<FPSService>(m_connection);
    }
    return *m_fps;
}

XCTestService& Instruments::XCTest() {
    if (!m_xctest) {
        m_xctest = std::make_unique<XCTestService>(m_connection);
    }
    return *m_xctest;
}

WDAService& Instruments::WDA() {
    if (!m_wda) {
        m_wda = std::make_unique<WDAService>(m_connection);
    }
    return *m_wda;
}

PortForwarder& Instruments::Ports() {
    if (!m_ports) {
        m_ports = std::make_unique<PortForwarder>(m_connection);
    }
    return *m_ports;
}

void Instruments::SetLogLevel(LogLevel level) {
    Log::SetLevel(level);
}

} // namespace instruments
