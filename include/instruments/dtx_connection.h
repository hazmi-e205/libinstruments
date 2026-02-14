#ifndef INSTRUMENTS_DTX_CONNECTION_H
#define INSTRUMENTS_DTX_CONNECTION_H

#include "dtx_channel.h"
#include "dtx_message.h"
#include "types.h"
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>

namespace instruments {

class DTXTransport;
class DTXFragmentDecoder;

// DTXConnection - manages a DTX protocol connection to an iOS device.
// Handles channel management, message routing, and the receive loop.
//
// Usage:
//   auto conn = DTXConnection::Create(device, service, sslHandshakeOnly);
//   conn->Connect();
//   auto channel = conn->MakeChannelWithIdentifier("com.apple...service");
//   auto msg = DTXMessage::CreateWithSelector("methodName");
//   auto response = channel->SendMessageSync(msg);
class DTXConnection {
public:
    // Create from an idevice connection
    static std::unique_ptr<DTXConnection> Create(
        idevice_connection_t connection, bool sslHandshakeOnly = false);

    // Create by connecting to a service on a device
    static std::unique_ptr<DTXConnection> Create(
        idevice_t device, lockdownd_service_descriptor_t service,
        bool sslHandshakeOnly = false);

    ~DTXConnection();

    // Non-copyable
    DTXConnection(const DTXConnection&) = delete;
    DTXConnection& operator=(const DTXConnection&) = delete;

    // Start the connection (begins the receive loop)
    Error Connect();

    // Create a named channel. The channel is requested from the device
    // via the global channel's _requestChannelWithCode:identifier: method.
    std::shared_ptr<DTXChannel> MakeChannelWithIdentifier(const std::string& identifier);

    // Get the global channel (channel code 0)
    std::shared_ptr<DTXChannel> GlobalChannel();

    // Close the connection
    void Disconnect();

    // Check if connected
    bool IsConnected() const { return m_connected.load(); }

    // Set handler for messages on unregistered channels
    void SetGlobalMessageHandler(DTXChannel::MessageHandler handler);

    // Send a message via the transport (called by DTXChannel)
    Error SendMessage(std::shared_ptr<DTXMessage> message);

private:
    explicit DTXConnection(std::unique_ptr<DTXTransport> transport);

    // Perform DTX protocol handshake (send client capabilities)
    Error PerformHandshake();

    // Receive loop running on background thread
    void ReceiveLoop();

    // Dispatch a received message to the appropriate channel
    void DispatchMessage(std::shared_ptr<DTXMessage> message);

    // Send an ACK for a received message
    void SendAck(uint32_t identifier, uint32_t channelCode);

    std::unique_ptr<DTXTransport> m_transport;
    std::atomic<bool> m_connected{false};
    std::thread m_receiveThread;

    // Channel management
    std::map<int32_t, std::shared_ptr<DTXChannel>> m_channels;
    std::mutex m_channelsMutex;
    std::atomic<int32_t> m_nextChannelCode{1};

    // Fragment assembly
    std::unique_ptr<DTXFragmentDecoder> m_fragmentDecoder;

    // Global message handler
    DTXChannel::MessageHandler m_globalHandler;
    std::mutex m_globalHandlerMutex;

    // Handshake synchronization
    std::mutex m_handshakeMutex;
    std::condition_variable m_handshakeCV;
    std::atomic<bool> m_handshakeReceived{false};
};

} // namespace instruments

#endif // INSTRUMENTS_DTX_CONNECTION_H
