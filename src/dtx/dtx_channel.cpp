#include "../../include/instruments/dtx_channel.h"
#include "../../include/instruments/dtx_connection.h"
#include "../util/log.h"
#include <chrono>
#include <fstream>

namespace instruments {

static const char* TAG = "DTXChannel";

DTXChannel::DTXChannel(DTXConnection* connection, const std::string& identifier, int32_t channelCode)
    : m_connection(connection)
    , m_identifier(identifier)
    , m_channelCode(channelCode)
{
}

DTXChannel::~DTXChannel() {
    Cancel();
}

uint32_t DTXChannel::NextIdentifier() {
    return m_nextIdentifier.fetch_add(1);
}

std::shared_ptr<DTXMessage> DTXChannel::SendMessageSync(
    std::shared_ptr<DTXMessage> message, int timeoutMs) {

    if (m_cancelled.load()) {
        INST_LOG_WARN(TAG, "Channel %s is cancelled", m_identifier.c_str());
        return nullptr;
    }

    uint32_t msgId = NextIdentifier();
    message->SetIdentifier(msgId);
    message->SetChannelCode(m_channelCode);
    message->SetExpectsReply(true);

    // Create waiter before sending
    auto waiter = std::make_shared<ResponseWaiter>();
    {
        std::lock_guard<std::mutex> lock(m_waitersMutex);
        m_waiters[msgId] = waiter;
    }

    INST_LOG_INFO(TAG, "[%s] SendSync id=%u: %s",
                  m_identifier.c_str(), msgId, message->Dump().c_str());
    if (m_identifier == "_global_" && message->Selector() == "_requestChannelWithCode:identifier:") {
        const auto& aux = message->RawAuxiliary();
        const auto& payload = message->RawPayload();
        INST_LOG_INFO(TAG, "[%s] RequestChannel sizes: aux=%zu, payload=%zu",
                      m_identifier.c_str(), aux.size(), payload.size());
        if (aux.size() >= 16) {
            std::string hexDump;
            size_t dumpLen = aux.size() < 64 ? aux.size() : 64;
            for (size_t i = 0; i < dumpLen; i++) {
                char buf[4];
                snprintf(buf, sizeof(buf), "%02X ", aux[i]);
                hexDump += buf;
                if ((i + 1) % 16 == 0) hexDump += "\n";
            }
            INST_LOG_INFO(TAG, "[%s] RequestChannel aux head:\n%s", m_identifier.c_str(), hexDump.c_str());
        }

        // Dump full encoded message for byte-level comparison with reference fixtures
#if defined(INSTRUMENTS_ENABLE_LOGGING)
        auto frames = message->Encode();
        if (!frames.empty()) {
            const char* dumpPath = "request_channel_last.bin";
            std::ofstream out(dumpPath, std::ios::binary | std::ios::trunc);
            if (out.is_open()) {
                out.write(reinterpret_cast<const char*>(frames[0].data()),
                          static_cast<std::streamsize>(frames[0].size()));
                out.close();
                INST_LOG_INFO(TAG, "[%s] RequestChannel dump written: %s (%zu bytes)",
                              m_identifier.c_str(), dumpPath, frames[0].size());
            } else {
                INST_LOG_WARN(TAG, "[%s] Failed to write RequestChannel dump: %s",
                              m_identifier.c_str(), dumpPath);
            }
        }
#endif
    }

    // Send the message
    Error err = m_connection->SendMessage(message);
    if (err != Error::Success) {
        std::lock_guard<std::mutex> lock(m_waitersMutex);
        m_waiters.erase(msgId);
        INST_LOG_ERROR(TAG, "Failed to send message: %s", ErrorToString(err));
        return nullptr;
    }
    INST_LOG_INFO(TAG, "[%s] Message sent, waiting for response (timeout=%dms)...",
                  m_identifier.c_str(), timeoutMs);

    // Wait for response
    {
        std::unique_lock<std::mutex> lock(waiter->mutex);
        bool gotResponse = waiter->cv.wait_for(lock,
            std::chrono::milliseconds(timeoutMs),
            [&]{ return waiter->ready || m_cancelled.load(); });

        if (!gotResponse || m_cancelled.load()) {
            std::lock_guard<std::mutex> wlock(m_waitersMutex);
            m_waiters.erase(msgId);
            if (m_cancelled.load()) {
                INST_LOG_ERROR(TAG, "Channel cancelled while waiting for response to id=%u", msgId);
            } else {
                INST_LOG_ERROR(TAG, "Timeout waiting for response to id=%u on %s (waited %dms)",
                             msgId, m_identifier.c_str(), timeoutMs);
            }
            return nullptr;
        }
    }

    // Clean up waiter
    {
        std::lock_guard<std::mutex> lock(m_waitersMutex);
        m_waiters.erase(msgId);
    }

    INST_LOG_INFO(TAG, "[%s] Got response for id=%u", m_identifier.c_str(), msgId);
    return waiter->response;
}

void DTXChannel::SendMessageAsync(std::shared_ptr<DTXMessage> message) {
    if (m_cancelled.load()) {
        INST_LOG_WARN(TAG, "[%s] SendAsync called on cancelled channel", m_identifier.c_str());
        return;
    }

    uint32_t msgId = NextIdentifier();
    message->SetIdentifier(msgId);
    message->SetChannelCode(m_channelCode);
    message->SetExpectsReply(false);

    INST_LOG_INFO(TAG, "[%s] SendAsync id=%u, selector=%s",
                  m_identifier.c_str(), msgId, message->Selector().c_str());

    m_connection->SendMessage(message);
}

void DTXChannel::SetMessageHandler(MessageHandler handler) {
    std::lock_guard<std::mutex> lock(m_handlerMutex);
    m_messageHandler = std::move(handler);
}

void DTXChannel::SetMethodHandler(const std::string& methodName, MessageHandler handler) {
    std::lock_guard<std::mutex> lock(m_methodHandlerMutex);
    m_methodHandlers[methodName] = std::move(handler);
}

void DTXChannel::Cancel() {
    if (m_cancelled.exchange(true)) return;

    INST_LOG_DEBUG(TAG, "Cancelling channel %s (code=%d)", m_identifier.c_str(), m_channelCode);

    // Wake up all waiters
    {
        std::lock_guard<std::mutex> lock(m_waitersMutex);
        for (auto& [id, waiter] : m_waiters) {
            std::lock_guard<std::mutex> wlock(waiter->mutex);
            waiter->ready = true;
            waiter->cv.notify_all();
        }
    }
}

void DTXChannel::SyncIdentifier(uint32_t receivedId) {
    uint32_t current = m_nextIdentifier.load();
    if (receivedId >= current) {
        m_nextIdentifier.store(receivedId + 1);
        INST_LOG_DEBUG(TAG, "[%s] Synced identifier: %u -> %u (received %u)",
                      m_identifier.c_str(), current, receivedId + 1, receivedId);
    }
}

void DTXChannel::DispatchMessage(std::shared_ptr<DTXMessage> message) {
    if (m_cancelled.load()) return;

    uint32_t convIdx = message->ConversationIndex();

    // Check if this is a response to a pending synchronous call.
    // Response messages have ConversationIndex > 0 and are keyed by message Identifier (go-ios behavior).
    if (convIdx > 0) {
        std::lock_guard<std::mutex> lock(m_waitersMutex);
        auto it = m_waiters.find(message->Identifier());
        if (it != m_waiters.end()) {
            auto& waiter = it->second;
            std::lock_guard<std::mutex> wlock(waiter->mutex);
            waiter->response = message;
            waiter->ready = true;
            waiter->cv.notify_all();
            return;
        }
    }

    // Check for method-specific handlers
    std::string selector = message->Selector();
    if (!selector.empty()) {
        std::lock_guard<std::mutex> lock(m_methodHandlerMutex);
        auto it = m_methodHandlers.find(selector);
        if (it != m_methodHandlers.end()) {
            it->second(message);
            return;
        }
    }

    // Fall through to generic message handler
    MessageHandler handler;
    {
        std::lock_guard<std::mutex> lock(m_handlerMutex);
        handler = m_messageHandler;
    }

    if (handler) {
        handler(message);
    } else {
        INST_LOG_TRACE(TAG, "[%s] Unhandled message: %s",
                      m_identifier.c_str(), message->Dump().c_str());
    }
}

} // namespace instruments
