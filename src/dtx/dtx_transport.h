#ifndef INSTRUMENTS_DTX_TRANSPORT_H
#define INSTRUMENTS_DTX_TRANSPORT_H

#include "../../include/instruments/dtx_message.h"
#include "../../include/instruments/types.h"
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// Undefine Windows API macros that collide with method names
#ifdef SendMessage
#undef SendMessage
#endif

namespace instruments {

// DTXTransport - low-level transport for sending and receiving raw DTX
// message frames over an idevice_connection_t or a raw TCP socket.
//
// Handles:
// - Reading complete DTX messages (header + payload)
// - Writing DTX messages to the connection
// - SSL handshake-only mode for certain services (idevice mode only)
// - Raw TCP socket mode for iOS 17+ external tunnel connections (no SSL)
class DTXTransport {
public:
    // Create transport from an existing idevice connection.
    // If sslHandshakeOnly is true, performs SSL handshake then disables SSL
    // (used by instruments remoteserver and similar services).
    DTXTransport(idevice_connection_t connection, bool sslHandshakeOnly = false);

    // Create transport from idevice_t by starting the instrument service
    // and connecting to it.
    DTXTransport(idevice_t device, lockdownd_service_descriptor_t service,
                 bool sslHandshakeOnly = false);

    // Create transport from a pre-connected raw TCP socket fd.
    // Takes ownership of the socket; closes it on destruction.
    // Used for iOS 17+ external tunnel connections (no SSL needed).
    // On Windows, pass (int)socket where socket is a SOCKET handle.
    explicit DTXTransport(int socketFd);

    // Connect via TCP to address:port and create a transport.
    // For iOS 17+ external tunnel connections (no SSL).
    // address: IPv4 or IPv6 address string
    // Returns nullptr on failure.
    static std::unique_ptr<DTXTransport> ConnectTCP(const std::string& address, uint16_t port);

    ~DTXTransport();

    // Non-copyable
    DTXTransport(const DTXTransport&) = delete;
    DTXTransport& operator=(const DTXTransport&) = delete;

    // Read one complete DTX message (blocking).
    // Returns the 32-byte header + all data after it.
    // Returns empty vector on error or disconnect.
    std::vector<uint8_t> Receive();

    // Send raw bytes
    Error Send(const uint8_t* data, size_t length);
    Error Send(const std::vector<uint8_t>& data);

    // Send a DTX message (handles fragmentation)
    Error SendMessage(const std::shared_ptr<DTXMessage>& message);

    // Check if transport is still connected
    bool IsConnected() const { return m_connected; }
    bool WasLastReadTimeout() const { return m_lastReadTimeout; }
    int LastReadError() const { return m_lastReadError; }
    uint32_t LastReadBytes() const { return m_lastReadBytes; }

    // Close the transport
    void Close();

private:
    // Read exactly n bytes from the connection
    bool ReadExact(uint8_t* buffer, size_t length);

    idevice_connection_t m_connection = nullptr;
    bool m_ownsConnection = false;
    bool m_connected = false;
    bool m_lastReadTimeout = false;
    int m_lastReadError = 0;
    uint32_t m_lastReadBytes = 0;
    std::mutex m_sendMutex;
    std::mutex m_recvMutex;

    // Raw TCP socket mode (iOS 17+ external tunnel, value -1 = not in use)
    int m_socketFd = -1;
};

} // namespace instruments

#endif // INSTRUMENTS_DTX_TRANSPORT_H
