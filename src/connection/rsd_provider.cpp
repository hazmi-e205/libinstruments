#include "rsd_provider.h"
#include "http2_framer.h"
#include "../util/log.h"
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

namespace instruments {

static const char* TAG = "RSDProvider";

RSDProvider::RSDProvider() = default;
RSDProvider::~RSDProvider() = default;

Error RSDProvider::Connect(const std::string& tunnelAddress, uint16_t rsdPort) {
    INST_LOG_WARN(TAG, "Direct RSD handshake requires UserspaceNetwork. "
                 "Use Connect(address, port, network) instead.");
    return Error::NotSupported;
}

Error RSDProvider::Connect(const std::string& tunnelAddress, uint16_t rsdPort,
                           UserspaceNetwork* network) {
    if (!network || !network->IsInitialized()) {
        INST_LOG_ERROR(TAG, "UserspaceNetwork not initialized");
        return Error::InvalidArgument;
    }

    INST_LOG_INFO(TAG, "Connecting to RSD at [%s]:%u", tunnelAddress.c_str(), rsdPort);

    // Step 1: Create TCP connection through the tunnel
    std::shared_ptr<UserspaceTcpConnection> tcpConn;
    Error err = network->TcpConnect(tunnelAddress, rsdPort, tcpConn);
    if (err != Error::Success) {
        INST_LOG_ERROR(TAG, "Failed to create TCP connection to RSD");
        return err;
    }

    // Wait for connection to establish (lwIP TCP handshake)
    std::mutex connMutex;
    std::condition_variable connCv;
    bool connected = false;
    bool connError = false;

    tcpConn->SetErrorCallback([&](Error) {
        std::lock_guard<std::mutex> lock(connMutex);
        connError = true;
        connCv.notify_one();
    });

    // Poll the network until connected or timeout
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!tcpConn->IsConnected() && !connError) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "TCP connection to RSD timed out");
            return Error::Timeout;
        }
        network->Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (connError || !tcpConn->IsConnected()) {
        INST_LOG_ERROR(TAG, "TCP connection to RSD failed");
        return Error::ConnectionFailed;
    }

    INST_LOG_INFO(TAG, "TCP connected to RSD");
    m_tcpConn = tcpConn;

    // Step 2: Accumulate received data
    std::mutex recvMutex;
    std::vector<uint8_t> recvBuffer;
    bool recvDone = false;

    tcpConn->SetRecvCallback([&](const uint8_t* data, size_t length) {
        std::lock_guard<std::mutex> lock(recvMutex);
        recvBuffer.insert(recvBuffer.end(), data, data + length);
    });

    tcpConn->SetErrorCallback([&](Error) {
        std::lock_guard<std::mutex> lock(recvMutex);
        recvDone = true;
    });

    // Step 3: Send HTTP/2 connection preface
    auto preface = Http2Framer::MakeConnectionPreface();
    err = tcpConn->Send(preface.data(), preface.size());
    if (err != Error::Success) {
        INST_LOG_ERROR(TAG, "Failed to send HTTP/2 preface");
        return err;
    }

    // Also send a window update for connection-level flow control
    auto windowUpdate = Http2Framer::MakeWindowUpdateFrame(0, 1048576);
    tcpConn->Send(windowUpdate.data(), windowUpdate.size());

    INST_LOG_DEBUG(TAG, "Sent HTTP/2 connection preface");

    // Step 4: Wait for server SETTINGS
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool gotServerSettings = false;

    while (!gotServerSettings && !recvDone) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "Timeout waiting for server SETTINGS");
            return Error::Timeout;
        }

        network->Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Try to decode frames
        std::lock_guard<std::mutex> lock(recvMutex);
        size_t offset = 0;
        while (offset < recvBuffer.size()) {
            H2Frame frame;
            size_t consumed = Http2Framer::DecodeFrame(
                recvBuffer.data() + offset, recvBuffer.size() - offset, frame);
            if (consumed == 0) break;
            offset += consumed;

            if (frame.type == H2FrameType::Settings && !(frame.flags & H2Flags::Ack)) {
                INST_LOG_DEBUG(TAG, "Received server SETTINGS");
                // Send SETTINGS ACK
                auto ack = Http2Framer::MakeSettingsFrame(true);
                tcpConn->Send(ack.data(), ack.size());
                gotServerSettings = true;
            }
        }
        if (offset > 0) {
            recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + offset);
        }
    }

    if (!gotServerSettings) {
        INST_LOG_ERROR(TAG, "Failed to receive server SETTINGS");
        return Error::ProtocolError;
    }

    // Step 5: Send XPC InitHandshake on stream 1 (client-initiated bidi)
    XPCMessage initMsg;
    initMsg.flags = XPCFlags::AlwaysSet | XPCFlags::InitHandshake;
    initMsg.messageId = 1;

    auto xpcData = initMsg.Encode();

    // Send as HTTP/2 DATA frame on stream 1
    // First send empty HEADERS to open the stream
    auto headers = Http2Framer::MakeHeadersFrame(1, {}, false);
    tcpConn->Send(headers.data(), headers.size());

    auto dataFrame = Http2Framer::MakeDataFrame(1, xpcData.data(), xpcData.size(), false);
    err = tcpConn->Send(dataFrame.data(), dataFrame.size());
    if (err != Error::Success) {
        INST_LOG_ERROR(TAG, "Failed to send XPC InitHandshake");
        return err;
    }

    INST_LOG_DEBUG(TAG, "Sent XPC InitHandshake on stream 1");

    // Step 6: Receive the XPC response (service discovery)
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    bool gotXPCResponse = false;

    while (!gotXPCResponse && !recvDone) {
        if (std::chrono::steady_clock::now() > deadline) {
            INST_LOG_ERROR(TAG, "Timeout waiting for XPC response");
            return Error::Timeout;
        }

        network->Poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

        // Try to decode frames
        std::lock_guard<std::mutex> lock(recvMutex);
        size_t offset = 0;
        while (offset < recvBuffer.size()) {
            H2Frame frame;
            size_t consumed = Http2Framer::DecodeFrame(
                recvBuffer.data() + offset, recvBuffer.size() - offset, frame);
            if (consumed == 0) break;
            offset += consumed;

            if (frame.type == H2FrameType::Data && !frame.payload.empty()) {
                // Try to decode as XPC message
                XPCMessage xpcResp;
                if (XPCMessage::Decode(frame.payload, xpcResp)) {
                    INST_LOG_INFO(TAG, "Received XPC response (flags=0x%x msgId=%llu)",
                                 xpcResp.flags, (unsigned long long)xpcResp.messageId);

                    if (xpcResp.body.IsDict()) {
                        ParseServiceResponse(xpcResp.body);
                        gotXPCResponse = true;
                    }
                }
            } else if (frame.type == H2FrameType::Settings) {
                if (!(frame.flags & H2Flags::Ack)) {
                    auto ack = Http2Framer::MakeSettingsFrame(true);
                    tcpConn->Send(ack.data(), ack.size());
                }
            } else if (frame.type == H2FrameType::WindowUpdate) {
                // Acknowledge window updates silently
            }
        }
        if (offset > 0) {
            recvBuffer.erase(recvBuffer.begin(), recvBuffer.begin() + offset);
        }
    }

    if (!gotXPCResponse) {
        INST_LOG_ERROR(TAG, "Failed to receive XPC service discovery response");
        return Error::ProtocolError;
    }

    INST_LOG_INFO(TAG, "RSD handshake complete: UDID=%s, %zu services discovered",
                 m_udid.c_str(), m_services.size());

    return Error::Success;
}

void RSDProvider::ParseServiceResponse(const NSObject& body) {
    // The XPC response body is a dictionary containing:
    // "Properties" → dict with device info including "UniqueDeviceID"
    // "Services" → dict mapping service name → {"Port": <int>}

    if (body.HasKey("Properties")) {
        const auto& props = body["Properties"];
        if (props.IsDict() && props.HasKey("UniqueDeviceID")) {
            m_udid = props["UniqueDeviceID"].AsString();
            INST_LOG_INFO(TAG, "Device UDID: %s", m_udid.c_str());
        }
    }

    if (body.HasKey("Services")) {
        const auto& services = body["Services"];
        if (services.IsDict()) {
            for (const auto& [name, value] : services.AsDict()) {
                RSDServiceEntry entry;
                entry.name = name;
                if (value.IsDict() && value.HasKey("Port")) {
                    entry.port = static_cast<uint16_t>(value["Port"].AsInt64());
                }
                if (entry.port > 0) {
                    m_services[name] = entry;
                    INST_LOG_DEBUG(TAG, "  Service: %s -> port %u", name.c_str(), entry.port);
                }
            }
        }
    }
}

uint16_t RSDProvider::FindServicePort(const std::string& serviceName) const {
    auto it = m_services.find(serviceName);
    if (it != m_services.end()) {
        return it->second.port;
    }
    return 0;
}

uint16_t RSDProvider::FindServicePortWithShim(const std::string& serviceName) const {
    // First try exact match
    uint16_t port = FindServicePort(serviceName);
    if (port > 0) return port;

    // Try with .shim.remote suffix (iOS 17+ shim services)
    std::string shimName = serviceName + ".shim.remote";
    return FindServicePort(shimName);
}

} // namespace instruments
