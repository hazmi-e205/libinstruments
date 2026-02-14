#ifndef INSTRUMENTS_DTX_CHANNEL_H
#define INSTRUMENTS_DTX_CHANNEL_H

#include "dtx_message.h"
#include "types.h"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

namespace instruments {

class DTXConnection;

// DTXChannel - represents a named channel within a DTX connection.
// Channels are created via DTXConnection::MakeChannelWithIdentifier().
// The global channel (code 0) is automatically created for each connection.
class DTXChannel : public std::enable_shared_from_this<DTXChannel> {
public:
    using MessageHandler = std::function<void(std::shared_ptr<DTXMessage>)>;

    DTXChannel(DTXConnection* connection, const std::string& identifier, int32_t channelCode);
    ~DTXChannel();

    // Synchronous method call - sends message and blocks until response
    std::shared_ptr<DTXMessage> SendMessageSync(
        std::shared_ptr<DTXMessage> message,
        int timeoutMs = DTXProtocol::DefaultTimeoutMs);

    // Asynchronous method call - fire-and-forget
    void SendMessageAsync(std::shared_ptr<DTXMessage> message);

    // Register handler for unsolicited incoming messages (streaming data)
    void SetMessageHandler(MessageHandler handler);

    // Register handler for specific method names
    void SetMethodHandler(const std::string& methodName, MessageHandler handler);

    // Cancel channel (stops waiting, closes)
    void Cancel();

    // Synchronize message identifier to avoid collisions
    // Per pymobiledevice3: update counter when device sends message with higher ID
    void SyncIdentifier(uint32_t receivedId);

    // Channel info
    const std::string& Identifier() const { return m_identifier; }
    int32_t ChannelCode() const { return m_channelCode; }
    bool IsCancelled() const { return m_cancelled.load(); }

    // Called by DTXConnection when a message arrives for this channel
    void DispatchMessage(std::shared_ptr<DTXMessage> message);

private:
    // Get next message identifier for this channel
    uint32_t NextIdentifier();

    DTXConnection* m_connection;
    std::string m_identifier;
    int32_t m_channelCode;
    std::atomic<uint32_t> m_nextIdentifier{1};
    std::atomic<bool> m_cancelled{false};

    // Message handler for streaming/unsolicited messages
    MessageHandler m_messageHandler;
    std::mutex m_handlerMutex;

    // Method-specific handlers
    std::map<std::string, MessageHandler> m_methodHandlers;
    std::mutex m_methodHandlerMutex;

    // Response waiters for synchronous calls
    struct ResponseWaiter {
        std::mutex mutex;
        std::condition_variable cv;
        std::shared_ptr<DTXMessage> response;
        bool ready = false;
    };
    std::map<uint32_t, std::shared_ptr<ResponseWaiter>> m_waiters;
    std::mutex m_waitersMutex;
};

} // namespace instruments

#endif // INSTRUMENTS_DTX_CHANNEL_H
