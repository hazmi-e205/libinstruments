#ifndef INSTRUMENTS_HTTP2_FRAMER_H
#define INSTRUMENTS_HTTP2_FRAMER_H

#include <cstdint>
#include <string>
#include <vector>

namespace instruments {

// Minimal HTTP/2 framing for RSD protocol (iOS 17+).
// This is NOT a full HTTP/2 implementation - only the frame types needed
// for the XPC handshake over RSD service discovery.
//
// Frame format (RFC 7540 Section 4.1):
// +-----------------------------------------------+
// |                 Length (24)                    |
// +---------------+---------------+---------------+
// |   Type (8)    |   Flags (8)   |
// +-+-------------+---------------+-------------------------------+
// |R|                 Stream Identifier (31)                      |
// +---------------------------------------------------------------+
// |                   Frame Payload (0...)                        |
// +---------------------------------------------------------------+

namespace H2FrameType {
    constexpr uint8_t Data = 0x0;
    constexpr uint8_t Headers = 0x1;
    constexpr uint8_t Settings = 0x4;
    constexpr uint8_t WindowUpdate = 0x8;
    constexpr uint8_t GoAway = 0x7;
}

namespace H2Flags {
    constexpr uint8_t EndStream = 0x01;
    constexpr uint8_t Ack = 0x01;       // For SETTINGS
    constexpr uint8_t EndHeaders = 0x04;
    constexpr uint8_t Padded = 0x08;
}

struct H2Frame {
    uint8_t type = 0;
    uint8_t flags = 0;
    uint32_t streamId = 0;
    std::vector<uint8_t> payload;
};

class Http2Framer {
public:
    // HTTP/2 connection preface (client magic + SETTINGS)
    static std::vector<uint8_t> MakeConnectionPreface();

    // Encode a frame to wire format
    static std::vector<uint8_t> EncodeFrame(const H2Frame& frame);

    // Encode specific frame types
    static std::vector<uint8_t> MakeSettingsFrame(bool ack = false);
    static std::vector<uint8_t> MakeWindowUpdateFrame(uint32_t streamId, uint32_t increment);
    static std::vector<uint8_t> MakeDataFrame(uint32_t streamId,
                                               const uint8_t* data, size_t length,
                                               bool endStream = false);
    static std::vector<uint8_t> MakeHeadersFrame(uint32_t streamId,
                                                  const std::vector<std::pair<std::string, std::string>>& headers,
                                                  bool endStream = false);

    // Decode frames from wire data.
    // Returns number of bytes consumed, 0 if not enough data.
    static size_t DecodeFrame(const uint8_t* data, size_t length, H2Frame& outFrame);

    // Connection preface magic string (24 bytes)
    static constexpr const char* ClientMagic = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    static constexpr size_t ClientMagicLen = 24;
};

} // namespace instruments

#endif // INSTRUMENTS_HTTP2_FRAMER_H
