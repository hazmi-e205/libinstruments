#include "dtx_transport.h"
#include "../util/log.h"
#include <cstring>

namespace instruments {

static const char* TAG = "DTXTransport";

DTXTransport::DTXTransport(idevice_connection_t connection, bool sslHandshakeOnly)
    : m_connection(connection)
    , m_ownsConnection(false)
    , m_connected(connection != nullptr)
{
    if (sslHandshakeOnly && m_connection) {
        // Enable SSL for handshake, then disable
        idevice_connection_enable_ssl(m_connection);
        idevice_connection_disable_ssl(m_connection);
        INST_LOG_DEBUG(TAG, "SSL handshake-only completed");
    }
}

DTXTransport::DTXTransport(idevice_t device, lockdownd_service_descriptor_t service,
                           bool sslHandshakeOnly)
    : m_ownsConnection(true)
    , m_connected(false)
{
    if (!device || !service) {
        INST_LOG_ERROR(TAG, "Invalid device or service descriptor");
        return;
    }

    idevice_error_t err = idevice_connect(device, service->port, &m_connection);
    if (err != IDEVICE_E_SUCCESS) {
        INST_LOG_ERROR(TAG, "Failed to connect to service port %u: error %d",
                      service->port, err);
        m_connection = nullptr;
        return;
    }

    m_connected = true;

    if (sslHandshakeOnly || service->ssl_enabled) {
        idevice_connection_enable_ssl(m_connection);
        if (sslHandshakeOnly) {
            idevice_connection_disable_ssl(m_connection);
            INST_LOG_INFO(TAG, "SSL mode: handshake-only (auth then plaintext)");
        } else {
            INST_LOG_INFO(TAG, "SSL mode: full encryption");
        }
    } else {
        INST_LOG_INFO(TAG, "SSL mode: none");
    }
}

DTXTransport::~DTXTransport() {
    Close();
}

void DTXTransport::Close() {
    m_connected = false;
    // Ensure no concurrent Send/Receive is in-flight while closing the connection
    {
        std::scoped_lock<std::mutex, std::mutex> lock(m_recvMutex, m_sendMutex);
        if (m_connection && m_ownsConnection) {
            idevice_disconnect(m_connection);
        }
        m_connection = nullptr;
    }
}

bool DTXTransport::ReadExact(uint8_t* buffer, size_t length) {
    if (!m_connection || !m_connected) return false;

    size_t totalRead = 0;
    m_lastReadTimeout = false;
    m_lastReadError = 0;
    m_lastReadBytes = 0;
    while (totalRead < length) {
        if (!m_connected) {
            return false;
        }
        uint32_t bytesRead = 0;
        idevice_error_t err = idevice_connection_receive_timeout(
            m_connection,
            reinterpret_cast<char*>(buffer + totalRead),
            static_cast<uint32_t>(length - totalRead),
            &bytesRead,
            30000 // 30 second timeout
        );

        if (bytesRead > 0) {
            // Even if the transport reports an error, keep any bytes we did receive
            if (err != IDEVICE_E_SUCCESS) {
                INST_LOG_DEBUG(TAG, "Partial read with error=%d, read=%u", err, bytesRead);
            }
            totalRead += bytesRead;
            continue;
        }

        if (err != IDEVICE_E_SUCCESS || bytesRead == 0) {
            m_lastReadError = static_cast<int>(err);
            m_lastReadBytes = bytesRead;
            if (err == IDEVICE_E_TIMEOUT) {
                INST_LOG_WARN(TAG, "Read timeout after %zu/%zu bytes", totalRead, length);
                m_lastReadTimeout = true;
                continue;
            }
            if (err == IDEVICE_E_SSL_ERROR && bytesRead == 0) {
                // Treat SSL read with no data as a non-fatal timeout-like condition
                INST_LOG_DEBUG(TAG, "SSL read returned no data (err=%d) after %zu/%zu bytes",
                               err, totalRead, length);
                m_lastReadTimeout = true;
                continue;
            }
            if (err == IDEVICE_E_SUCCESS && bytesRead == 0) {
                // Treat as no data available (some platforms report 0 bytes without error)
                INST_LOG_DEBUG(TAG, "Read returned 0 bytes with success after %zu/%zu bytes", totalRead, length);
                m_lastReadTimeout = true;
                continue;
            }
            INST_LOG_DEBUG(TAG, "Read failed: error=%d, read=%u", err, bytesRead);
            m_lastReadTimeout = false;
            m_connected = false;
            return false;
        }
    }

    m_lastReadTimeout = false;
    m_lastReadError = 0;
    m_lastReadBytes = 0;
    return true;
}

std::vector<uint8_t> DTXTransport::Receive() {
    std::lock_guard<std::mutex> lock(m_recvMutex);

    // Read the 32-byte header, resyncing if needed
    uint8_t headerBuf[DTXProtocol::HeaderLength];
    if (!ReadExact(headerBuf, DTXProtocol::HeaderLength)) {
        return {};
    }

    auto isMagic = [](const uint8_t* p) {
        // Accept both big-endian and little-endian magic
        return (p[0] == 0x79 && p[1] == 0x5B && p[2] == 0x3D && p[3] == 0x1F) ||
               (p[0] == 0x1F && p[1] == 0x3D && p[2] == 0x5B && p[3] == 0x79);
    };

    if (!isMagic(headerBuf)) {
        // Attempt to resync by sliding window until magic is found
        const size_t maxScan = 1024 * 1024; // 1MB
        size_t scanned = 0;
        while (scanned < maxScan) {
            // shift left by 1 byte
            std::memmove(headerBuf, headerBuf + 1, DTXProtocol::HeaderLength - 1);
            if (!ReadExact(headerBuf + DTXProtocol::HeaderLength - 1, 1)) {
                return {};
            }
            scanned++;
            if (isMagic(headerBuf)) {
                INST_LOG_WARN(TAG, "Resynced DTX stream after %zu bytes", scanned);
                break;
            }
        }
        if (scanned >= maxScan && !isMagic(headerBuf)) {
            INST_LOG_ERROR(TAG, "Failed to resync DTX stream (scanned %zu bytes)", scanned);
            return {};
        }
    }

    DTXMessageHeader header;
    if (!DTXMessage::ParseHeader(headerBuf, DTXProtocol::HeaderLength, header)) {
        INST_LOG_ERROR(TAG, "Failed to parse DTX header");
        return {};
    }

    // If this is the first fragment, it is header-only (no payload data)
    if (header.fragmentCount > 1 && header.fragmentIndex == 0) {
        std::vector<uint8_t> result(DTXProtocol::HeaderLength);
        std::memcpy(result.data(), headerBuf, DTXProtocol::HeaderLength);
        return result;
    }

    // Allocate buffer for header + message data
    size_t totalSize = DTXProtocol::HeaderLength + header.messageLength;
    std::vector<uint8_t> result(totalSize);
    std::memcpy(result.data(), headerBuf, DTXProtocol::HeaderLength);

    // Read the remaining message data
    if (header.messageLength > 0) {
        if (!ReadExact(result.data() + DTXProtocol::HeaderLength, header.messageLength)) {
            INST_LOG_ERROR(TAG, "Failed to read message body (%u bytes)", header.messageLength);
            return {};
        }
    }

    INST_LOG_TRACE(TAG, "Received message: id=%u, ch=%u, len=%u, frag=%u/%u",
                  header.identifier, header.channelCode, header.messageLength,
                  header.fragmentIndex, header.fragmentCount);

    return result;
}

Error DTXTransport::Send(const uint8_t* data, size_t length) {
    std::lock_guard<std::mutex> lock(m_sendMutex);

    if (!m_connection || !m_connected) {
        return Error::ConnectionFailed;
    }

    size_t totalSent = 0;
    while (totalSent < length) {
        uint32_t bytesSent = 0;
        idevice_error_t err = idevice_connection_send(
            m_connection,
            reinterpret_cast<const char*>(data + totalSent),
            static_cast<uint32_t>(length - totalSent),
            &bytesSent
        );

        if (err != IDEVICE_E_SUCCESS) {
            INST_LOG_ERROR(TAG, "Send failed: error=%d", err);
            m_connected = false;
            return Error::ConnectionFailed;
        }

        totalSent += bytesSent;
    }

    return Error::Success;
}

Error DTXTransport::Send(const std::vector<uint8_t>& data) {
    return Send(data.data(), data.size());
}

Error DTXTransport::SendMessage(const std::shared_ptr<DTXMessage>& message) {
    auto fragments = message->Encode();
    for (const auto& fragment : fragments) {
        Error err = Send(fragment);
        if (err != Error::Success) {
            return err;
        }
    }
    return Error::Success;
}

} // namespace instruments
