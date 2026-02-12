#include "../../include/instruments/dtx_connection.h"
#include "dtx_transport.h"
#include "dtx_fragment.h"
#include "../nskeyedarchiver/nsobject.h"
#include "../util/log.h"
#include <cstring>

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
    if (m_connected.load()) return Error::Success;

    // Create the global channel (channel code 0)
    {
        std::lock_guard<std::mutex> lock(m_channelsMutex);
        auto globalCh = std::make_shared<DTXChannel>(this, "_global_", 0);
        m_channels[0] = globalCh;
    }

    m_connected.store(true);

    // Start receive loop
    m_receiveThread = std::thread([this]() {
        ReceiveLoop();
    });

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
        std::lock_guard<std::mutex> lock(m_channelsMutex);
        m_channels.erase(code);
        return nullptr;
    }

    INST_LOG_INFO(TAG, "Channel created: %s (code=%d)", identifier.c_str(), code);
    return channel;
}

void DTXConnection::Disconnect() {
    if (!m_connected.exchange(false)) return;

    INST_LOG_INFO(TAG, "Disconnecting");

    // Cancel all channels
    {
        std::lock_guard<std::mutex> lock(m_channelsMutex);
        for (auto& [code, channel] : m_channels) {
            channel->Cancel();
        }
    }

    // Close transport
    if (m_transport) {
        m_transport->Close();
    }

    // Wait for receive thread
    if (m_receiveThread.joinable()) {
        m_receiveThread.join();
    }

    // Clear channels
    {
        std::lock_guard<std::mutex> lock(m_channelsMutex);
        m_channels.clear();
    }

    if (m_fragmentDecoder) {
        m_fragmentDecoder->Clear();
    }
}

void DTXConnection::SetGlobalMessageHandler(DTXChannel::MessageHandler handler) {
    std::lock_guard<std::mutex> lock(m_globalHandlerMutex);
    m_globalHandler = std::move(handler);
}

Error DTXConnection::SendMessage(std::shared_ptr<DTXMessage> message) {
    if (!m_connected.load() || !m_transport) {
        return Error::ConnectionFailed;
    }
    return m_transport->SendMessage(message);
}

void DTXConnection::SendAck(uint32_t identifier, uint32_t channelCode) {
    auto ack = DTXMessage::CreateAck(identifier, channelCode);
    SendMessage(ack);
}

void DTXConnection::ReceiveLoop() {
    INST_LOG_DEBUG(TAG, "Receive loop started");

    while (m_connected.load()) {
        auto rawData = m_transport->Receive();
        if (rawData.empty()) {
            if (m_connected.load()) {
                INST_LOG_INFO(TAG, "Connection closed by remote");
                m_connected.store(false);
            }
            break;
        }

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

void DTXConnection::DispatchMessage(std::shared_ptr<DTXMessage> message) {
    // Send ACK if the message expects a reply
    if (message->ExpectsReply() &&
        message->MessageType() != DTXMessageType::Ack &&
        message->ConversationIndex() == 0) {
        SendAck(message->Identifier(), message->ChannelCode());
    }

    // Skip ACK messages
    if (message->MessageType() == DTXMessageType::Ack) {
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
