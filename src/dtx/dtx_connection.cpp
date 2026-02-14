#include "../../include/instruments/dtx_connection.h"
#include "dtx_transport.h"
#include "dtx_fragment.h"
#include "../nskeyedarchiver/nsobject.h"
#include "../util/log.h"
#include <cstring>
#include <libimobiledevice/libimobiledevice.h>
#include <thread>

namespace instruments {

static const char* TAG = "DTXConnection";

DTXConnection::DTXConnection(std::unique_ptr<DTXTransport> transport)
    : m_transport(std::move(transport))
    , m_fragmentDecoder(std::make_unique<DTXFragmentDecoder>())
{
}

DTXConnection::~DTXConnection() {
    Disconnect();
}

std::unique_ptr<DTXConnection> DTXConnection::Create(
    idevice_connection_t connection, bool sslHandshakeOnly) {
    auto transport = std::make_unique<DTXTransport>(connection, sslHandshakeOnly);
    if (!transport->IsConnected()) return nullptr;
    return std::unique_ptr<DTXConnection>(new DTXConnection(std::move(transport)));
}

std::unique_ptr<DTXConnection> DTXConnection::Create(
    idevice_t device, lockdownd_service_descriptor_t service,
    bool sslHandshakeOnly) {
    auto transport = std::make_unique<DTXTransport>(device, service, sslHandshakeOnly);
    if (!transport->IsConnected()) return nullptr;
    return std::unique_ptr<DTXConnection>(new DTXConnection(std::move(transport)));
}

Error DTXConnection::Connect() {
    INST_LOG_INFO(TAG, "=== DTXConnection::Connect() ENTRY - BUILD TIMESTAMP: " __DATE__ " " __TIME__ " ===");

    if (m_connected.load()) {
        INST_LOG_INFO(TAG, "Already connected, returning");
        return Error::Success;
    }

    // Create the global channel (channel code 0)
    INST_LOG_INFO(TAG, "Creating global channel");
    {
        std::lock_guard<std::mutex> lock(m_channelsMutex);
        auto globalCh = std::make_shared<DTXChannel>(this, "_global_", 0);
        m_channels[0] = globalCh;
    }

    m_connected.store(true);
    INST_LOG_INFO(TAG, "Connection flag set to true");

    // Start receive loop
    INST_LOG_INFO(TAG, "Starting receive thread");
    m_receiveThread = std::thread([this]() {
        ReceiveLoop();
    });

    // Perform DTX handshake: send client capabilities and wait for device response
    // Based on pymobiledevice3: send _notifyOfPublishedCapabilities, then recv_plist()
    // The device expects a bidirectional handshake before accepting channel requests
    INST_LOG_INFO(TAG, "Performing DTX handshake");
    Error handshakeErr = PerformHandshake();
    if (handshakeErr != Error::Success) {
        INST_LOG_ERROR(TAG, "DTX handshake failed with error %d", static_cast<int>(handshakeErr));
        Disconnect();
        return handshakeErr;
    }

    INST_LOG_INFO(TAG, "Connected");
    return Error::Success;
}

std::shared_ptr<DTXChannel> DTXConnection::GlobalChannel() {
    std::lock_guard<std::mutex> lock(m_channelsMutex);
    auto it = m_channels.find(0);
    return (it != m_channels.end()) ? it->second : nullptr;
}

std::shared_ptr<DTXChannel> DTXConnection::MakeChannelWithIdentifier(const std::string& identifier) {
    if (!m_connected.load()) {
        INST_LOG_ERROR(TAG, "Not connected");
        return nullptr;
    }

    int32_t code = m_nextChannelCode.fetch_add(1);

    // Create the channel object
    auto channel = std::make_shared<DTXChannel>(this, identifier, code);
    {
        std::lock_guard<std::mutex> lock(m_channelsMutex);
        m_channels[code] = channel;
    }

    // Request the channel from the device via the global channel
    auto globalCh = GlobalChannel();
    if (!globalCh) {
        INST_LOG_ERROR(TAG, "No global channel");
        return nullptr;
    }

    auto requestMsg = DTXMessage::CreateWithSelector("_requestChannelWithCode:identifier:");
    requestMsg->AppendAuxiliary(NSObject(static_cast<int32_t>(code)));
    requestMsg->AppendAuxiliary(NSObject(identifier));

    auto response = globalCh->SendMessageSync(requestMsg);
    if (!response) {
        INST_LOG_ERROR(TAG, "Failed to request channel: %s", identifier.c_str());
        INST_LOG_ERROR(TAG, "Connection status: %d, Transport: %s",
                      m_connected.load() ? 1 : 0,
                      m_transport ? "valid" : "null");
        std::lock_guard<std::mutex> lock(m_channelsMutex);
        m_channels.erase(code);
        return nullptr;
    }

    INST_LOG_INFO(TAG, "Channel created: %s (code=%d)", identifier.c_str(), code);
    return channel;
}

void DTXConnection::Disconnect() {
    bool wasConnected = m_connected.exchange(false);

    if (wasConnected) {
        INST_LOG_INFO(TAG, "Disconnecting");

        // Cancel all channels
        {
            std::lock_guard<std::mutex> lock(m_channelsMutex);
            for (auto& [code, channel] : m_channels) {
                channel->Cancel();
            }
        }

        // Close transport to unblock receive thread
        if (m_transport) {
            m_transport->Close();
        }
    }

    // Always join the receive thread — even if the receive loop already
    // set m_connected=false (remote closure).  Without this, the
    // std::thread destructor calls std::terminate() on a joinable thread.
    if (m_receiveThread.joinable()) {
        m_receiveThread.join();
    }

    if (wasConnected) {
        // Clear channels
        {
            std::lock_guard<std::mutex> lock(m_channelsMutex);
            m_channels.clear();
        }

        if (m_fragmentDecoder) {
            m_fragmentDecoder->Clear();
        }
    }
}

void DTXConnection::SetGlobalMessageHandler(DTXChannel::MessageHandler handler) {
    std::lock_guard<std::mutex> lock(m_globalHandlerMutex);
    m_globalHandler = std::move(handler);
}

void DTXConnection::AddGlobalMessageHandler(DTXChannel::MessageHandler handler) {
    std::lock_guard<std::mutex> lock(m_globalHandlerMutex);
    if (!m_globalHandler) {
        m_globalHandler = std::move(handler);
        return;
    }
    auto existing = m_globalHandler;
    m_globalHandler = [existing, handler](std::shared_ptr<DTXMessage> msg) {
        if (existing) existing(msg);
        if (handler) handler(msg);
    };
}

Error DTXConnection::SendMessage(std::shared_ptr<DTXMessage> message) {
    if (!m_connected.load() || !m_transport) {
        return Error::ConnectionFailed;
    }
    return m_transport->SendMessage(message);
}

void DTXConnection::SendAck(uint32_t identifier, uint32_t channelCode, uint32_t conversationIndex) {
    INST_LOG_INFO(TAG, "Creating ACK message for id=%u, ch=%u", identifier, channelCode);
    auto ack = DTXMessage::CreateAck(identifier, channelCode, conversationIndex);
    Error err = SendMessage(ack);
    if (err == Error::Success) {
        INST_LOG_INFO(TAG, "ACK sent successfully");
    } else {
        INST_LOG_ERROR(TAG, "Failed to send ACK: error %d", static_cast<int>(err));
    }
}

void DTXConnection::ReceiveLoop() {
    INST_LOG_INFO(TAG, "*** RECEIVE LOOP STARTED ***");

    auto lastWaitLog = std::chrono::steady_clock::now() - std::chrono::seconds(10);

    while (m_connected.load()) {
        auto now = std::chrono::steady_clock::now();
        if (now - lastWaitLog > std::chrono::seconds(2)) {
            INST_LOG_INFO(TAG, "Waiting for data from transport...");
            lastWaitLog = now;
        }
        auto rawData = m_transport->Receive();
        if (rawData.empty()) {
            if (m_transport) {
                const int lastErr = m_transport->LastReadError();
                const uint32_t lastBytes = m_transport->LastReadBytes();
                const bool sslNoData = (lastErr == IDEVICE_E_SSL_ERROR && lastBytes == 0);
                const bool successNoData = (lastErr == IDEVICE_E_SUCCESS && lastBytes == 0);
                if (m_transport->WasLastReadTimeout() || sslNoData || successNoData) {
                    // No data yet, keep waiting (avoid tight loop)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
            }
            if (m_connected.load()) {
                INST_LOG_INFO(TAG,
                              "*** Connection closed by remote (received empty data) err=%d, bytes=%u ***",
                              m_transport ? m_transport->LastReadError() : -1,
                              m_transport ? m_transport->LastReadBytes() : 0);
                m_connected.store(false);
            }
            break;
        }
        INST_LOG_INFO(TAG, "Received %zu bytes from transport", rawData.size());

        // Parse header
        DTXMessageHeader header;
        if (!DTXMessage::ParseHeader(rawData.data(), rawData.size(), header)) {
            INST_LOG_ERROR(TAG, "Failed to parse received message header");
            continue;
        }

        const uint8_t* payloadData = rawData.data() + DTXProtocol::HeaderLength;
        size_t payloadSize = rawData.size() - DTXProtocol::HeaderLength;

        // Handle fragmented messages
        if (header.fragmentCount > 1) {
            std::vector<uint8_t> fragPayload(payloadData, payloadData + payloadSize);
            bool complete = m_fragmentDecoder->AddFragment(
                header.identifier, header.fragmentIndex,
                header.fragmentCount, fragPayload);

            if (!complete) continue;

            // Assemble the complete message
            auto assembled = m_fragmentDecoder->GetAssembledData(header.identifier);
            m_fragmentDecoder->Remove(header.identifier);

            auto message = DTXMessage::Decode(header, assembled.data(), assembled.size());
            if (message) {
                DispatchMessage(message);
            }
            continue;
        }

        // Non-fragmented message
        auto message = DTXMessage::Decode(header, payloadData, payloadSize);
        if (message) {
            DispatchMessage(message);
        }
    }

    INST_LOG_DEBUG(TAG, "Receive loop ended");
}

Error DTXConnection::PerformHandshake() {
    INST_LOG_INFO(TAG, ">>> PerformHandshake() ENTRY <<<");

    auto globalCh = GlobalChannel();
    if (!globalCh) {
        INST_LOG_ERROR(TAG, "No global channel!");
        return Error::ConnectionFailed;
    }

    // Reset handshake flag
    {
        std::lock_guard<std::mutex> lock(m_handshakeMutex);
        m_handshakeReceived.store(false);
        INST_LOG_INFO(TAG, "Handshake flag reset to false");
    }

    // Build client capabilities
    // Use DTXBlockCompression=2 to match go-ios / sonic-gidevice behavior
    // Use uint64 to match go-ios NSNumber unsigned encoding
    NSObject::DictType caps;
    caps["com.apple.private.DTXBlockCompression"] = NSObject(static_cast<uint64_t>(2));
    caps["com.apple.private.DTXConnection"] = NSObject(static_cast<uint64_t>(1));

    NSObject capsObj(std::move(caps));
    capsObj.SetClassName("NSMutableDictionary");
    capsObj.SetClassHierarchy({"NSMutableDictionary", "NSDictionary", "NSObject"});

    auto msg = DTXMessage::CreateWithSelector("_notifyOfPublishedCapabilities:");
    msg->AppendAuxiliary(capsObj);
    // Handshake is a bidirectional exchange, not a request-response - no ACK expected
    msg->SetExpectsReply(false);

    INST_LOG_INFO(TAG, "Sending client capabilities");

    // Debug: log the raw auxiliary data
    auto auxData = msg->RawAuxiliary();
    INST_LOG_INFO(TAG, "Auxiliary data size: %zu bytes", auxData.size());
    if (auxData.size() > 0 && auxData.size() <= 500) {
        std::string hexDump;
        for (size_t i = 0; i < auxData.size(); i++) {
            char buf[4];
            snprintf(buf, sizeof(buf), "%02X ", auxData[i]);
            hexDump += buf;
            if ((i + 1) % 16 == 0) hexDump += "\n";
        }
        INST_LOG_INFO(TAG, "Auxiliary hex dump:\n%s", hexDump.c_str());
    }

    globalCh->SendMessageAsync(msg);

    // Wait for device's capabilities message
    INST_LOG_INFO(TAG, "Waiting for device capabilities (flag=%d)...", m_handshakeReceived.load() ? 1 : 0);
    std::unique_lock<std::mutex> lock(m_handshakeMutex);
    INST_LOG_INFO(TAG, "Lock acquired, flag=%d", m_handshakeReceived.load() ? 1 : 0);

    bool received = m_handshakeCV.wait_for(lock, std::chrono::seconds(5),
        [this]() {
            bool flag = m_handshakeReceived.load();
            INST_LOG_DEBUG(TAG, "CV predicate: flag=%d", flag ? 1 : 0);
            return flag;
        });

    INST_LOG_INFO(TAG, "Wait done: received=%d, flag=%d", received ? 1 : 0, m_handshakeReceived.load() ? 1 : 0);

    if (!received) {
        INST_LOG_ERROR(TAG, "Handshake timeout");
        return Error::Timeout;
    }

    INST_LOG_INFO(TAG, "Handshake complete");
    return Error::Success;
}

void DTXConnection::DispatchMessage(std::shared_ptr<DTXMessage> message) {
    INST_LOG_INFO(TAG, "*** RECEIVED MESSAGE: ch=%d, id=%u, conv=%u, type=%u, selector=%s, ExpectsReply=%d ***",
                  static_cast<int32_t>(message->ChannelCode()),
                  message->Identifier(),
                  message->ConversationIndex(),
                  static_cast<uint32_t>(message->MessageType()),
                  message->Selector().c_str(),
                  message->ExpectsReply() ? 1 : 0);

    // Update channel's identifier counter to avoid ID collisions
    // Per pymobiledevice3: if device sends message with id >= our counter, update our counter
    // This prevents us from reusing identifiers that the device has used
    if (message->ConversationIndex() == 0) {
        int32_t channelCode = static_cast<int32_t>(message->ChannelCode());
        std::shared_ptr<DTXChannel> channel;
        {
            std::lock_guard<std::mutex> lock(m_channelsMutex);
            auto it = m_channels.find(channelCode);
            if (it != m_channels.end()) {
                channel = it->second;
            }
        }
        if (channel) {
            channel->SyncIdentifier(message->Identifier());
        }
    }

    // Check if this is the device's capabilities message (handshake)
    std::string selector = message->Selector();
    bool isHandshake = (selector == "_notifyOfPublishedCapabilities:" &&
                        message->ChannelCode() == 0 &&
                        message->ConversationIndex() == 0);

    INST_LOG_INFO(TAG, "isHandshake check: selector='%s', ch=%d, conv=%d -> %s",
                  selector.c_str(),
                  static_cast<int32_t>(message->ChannelCode()),
                  message->ConversationIndex(),
                  isHandshake ? "TRUE" : "FALSE");

    if (isHandshake) {
        INST_LOG_INFO(TAG, "Received device capabilities (handshake)");
        // Log the device's capabilities for debugging
        auto auxObjects = message->AuxiliaryObjects();
        if (!auxObjects.empty() && auxObjects[0].IsDict()) {
            INST_LOG_INFO(TAG, "Device capabilities dict has %zu entries", auxObjects[0].AsDict().size());
            for (const auto& [key, value] : auxObjects[0].AsDict()) {
                INST_LOG_INFO(TAG, "  %s = %s", key.c_str(),
                    value.IsInt() ? std::to_string(value.ToNumber()).c_str() : "...");
            }
        }
        INST_LOG_INFO(TAG, "Finished logging capabilities, about to check ACK logic");
    }

    // Send ACK only if the message explicitly expects a reply
    // IMPORTANT: Check ACK BEFORE returning early for handshake!
    // The device might require an ACK even for the handshake message.
    if (message->MessageType() != DTXMessageType::Ack &&
        message->ConversationIndex() == 0 &&
        message->ExpectsReply()) {
        INST_LOG_INFO(TAG, "Sending ACK for message id=%u",
                      message->Identifier());
        SendAck(message->Identifier(), message->ChannelCode(), message->ConversationIndex());
    } else {
        INST_LOG_INFO(TAG, "NOT sending ACK (ExpectsReply=%d, MsgType=%u, ConvIdx=%u)",
                      message->ExpectsReply() ? 1 : 0,
                      static_cast<uint32_t>(message->MessageType()),
                      message->ConversationIndex());
    }

    // Now handle handshake - signal CV and return early (after ACK check)
    INST_LOG_INFO(TAG, "About to check isHandshake again: %s", isHandshake ? "TRUE" : "FALSE");
    if (isHandshake) {
        INST_LOG_INFO(TAG, "Signaling handshake CV");
        std::lock_guard<std::mutex> lock(m_handshakeMutex);
        m_handshakeReceived.store(true);
        INST_LOG_INFO(TAG, "Flag set to true, about to notify");
        m_handshakeCV.notify_one();
        INST_LOG_INFO(TAG, "CV notified, returning from DispatchMessage");
        // Return early - don't dispatch handshake message to channel
        return;
    }

    int32_t channelCode = static_cast<int32_t>(message->ChannelCode());

    INST_LOG_TRACE(TAG, "Dispatch: ch=%d, id=%u, conv=%u, type=%u",
                  channelCode, message->Identifier(),
                  message->ConversationIndex(),
                  static_cast<uint32_t>(message->MessageType()));

    // Find the channel
    std::shared_ptr<DTXChannel> channel;
    {
        std::lock_guard<std::mutex> lock(m_channelsMutex);
        auto it = m_channels.find(channelCode);
        if (it != m_channels.end()) {
            channel = it->second;
        }
    }

    if (channel) {
        channel->DispatchMessage(message);
    } else {
        // No channel found - use global handler
        DTXChannel::MessageHandler handler;
        {
            std::lock_guard<std::mutex> lock(m_globalHandlerMutex);
            handler = m_globalHandler;
        }
        if (handler) {
            handler(message);
        } else {
            INST_LOG_DEBUG(TAG, "No handler for channel code %d", channelCode);
        }
    }
}

} // namespace instruments
