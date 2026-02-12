#include "http2_framer.h"
#include <cstring>

namespace instruments {

static void WriteUint24(uint8_t* out, uint32_t val) {
    out[0] = static_cast<uint8_t>((val >> 16) & 0xFF);
    out[1] = static_cast<uint8_t>((val >> 8) & 0xFF);
    out[2] = static_cast<uint8_t>(val & 0xFF);
}

static void WriteUint32(uint8_t* out, uint32_t val) {
    out[0] = static_cast<uint8_t>((val >> 24) & 0xFF);
    out[1] = static_cast<uint8_t>((val >> 16) & 0xFF);
    out[2] = static_cast<uint8_t>((val >> 8) & 0xFF);
    out[3] = static_cast<uint8_t>(val & 0xFF);
}

static uint32_t ReadUint24(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 16) |
           (static_cast<uint32_t>(data[1]) << 8) |
           static_cast<uint32_t>(data[2]);
}

static uint32_t ReadUint32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
           (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) |
           static_cast<uint32_t>(data[3]);
}

std::vector<uint8_t> Http2Framer::EncodeFrame(const H2Frame& frame) {
    std::vector<uint8_t> result(9 + frame.payload.size());

    // Length (24 bits)
    WriteUint24(result.data(), static_cast<uint32_t>(frame.payload.size()));

    // Type (8 bits)
    result[3] = frame.type;

    // Flags (8 bits)
    result[4] = frame.flags;

    // Stream ID (31 bits, R bit always 0)
    WriteUint32(result.data() + 5, frame.streamId & 0x7FFFFFFF);

    // Payload
    if (!frame.payload.empty()) {
        std::memcpy(result.data() + 9, frame.payload.data(), frame.payload.size());
    }

    return result;
}

size_t Http2Framer::DecodeFrame(const uint8_t* data, size_t length, H2Frame& outFrame) {
    if (length < 9) return 0; // Not enough for frame header

    uint32_t payloadLen = ReadUint24(data);
    size_t totalLen = 9 + payloadLen;

    if (length < totalLen) return 0; // Not enough for full frame

    outFrame.type = data[3];
    outFrame.flags = data[4];
    outFrame.streamId = ReadUint32(data + 5) & 0x7FFFFFFF;

    if (payloadLen > 0) {
        outFrame.payload.assign(data + 9, data + 9 + payloadLen);
    } else {
        outFrame.payload.clear();
    }

    return totalLen;
}

std::vector<uint8_t> Http2Framer::MakeConnectionPreface() {
    // Client magic + initial SETTINGS frame
    std::vector<uint8_t> preface;
    preface.insert(preface.end(), ClientMagic, ClientMagic + ClientMagicLen);

    auto settings = MakeSettingsFrame(false);
    preface.insert(preface.end(), settings.begin(), settings.end());

    return preface;
}

std::vector<uint8_t> Http2Framer::MakeSettingsFrame(bool ack) {
    H2Frame frame;
    frame.type = H2FrameType::Settings;
    frame.flags = ack ? H2Flags::Ack : 0;
    frame.streamId = 0;

    if (!ack) {
        // Send default settings:
        // SETTINGS_MAX_CONCURRENT_STREAMS (0x3) = 100
        // SETTINGS_INITIAL_WINDOW_SIZE (0x4) = 1048576
        // SETTINGS_ENABLE_PUSH (0x2) = 0
        uint8_t settings[] = {
            0x00, 0x03, 0x00, 0x00, 0x00, 0x64, // MAX_CONCURRENT_STREAMS = 100
            0x00, 0x04, 0x00, 0x10, 0x00, 0x00, // INITIAL_WINDOW_SIZE = 1048576
            0x00, 0x02, 0x00, 0x00, 0x00, 0x00, // ENABLE_PUSH = 0
        };
        frame.payload.assign(settings, settings + sizeof(settings));
    }

    return EncodeFrame(frame);
}

std::vector<uint8_t> Http2Framer::MakeWindowUpdateFrame(uint32_t streamId, uint32_t increment) {
    H2Frame frame;
    frame.type = H2FrameType::WindowUpdate;
    frame.streamId = streamId;
    frame.payload.resize(4);
    WriteUint32(frame.payload.data(), increment & 0x7FFFFFFF);
    return EncodeFrame(frame);
}

std::vector<uint8_t> Http2Framer::MakeDataFrame(uint32_t streamId,
                                                  const uint8_t* data, size_t length,
                                                  bool endStream) {
    H2Frame frame;
    frame.type = H2FrameType::Data;
    frame.flags = endStream ? H2Flags::EndStream : 0;
    frame.streamId = streamId;
    if (data && length > 0) {
        frame.payload.assign(data, data + length);
    }
    return EncodeFrame(frame);
}

std::vector<uint8_t> Http2Framer::MakeHeadersFrame(uint32_t streamId,
                                                     const std::vector<std::pair<std::string, std::string>>& headers,
                                                     bool endStream) {
    // Minimal HPACK encoding - we use literal header field without indexing
    // (RFC 7541 Section 6.2.1): 0000xxxx pattern
    // For RSD, we only need a few simple headers, so no need for Huffman or indexing.
    std::vector<uint8_t> hpackData;

    for (const auto& [name, value] : headers) {
        // Literal Header Field without Indexing - New Name (0x00)
        hpackData.push_back(0x00);

        // Name length + name
        hpackData.push_back(static_cast<uint8_t>(name.size()));
        hpackData.insert(hpackData.end(), name.begin(), name.end());

        // Value length + value
        hpackData.push_back(static_cast<uint8_t>(value.size()));
        hpackData.insert(hpackData.end(), value.begin(), value.end());
    }

    H2Frame frame;
    frame.type = H2FrameType::Headers;
    frame.flags = H2Flags::EndHeaders | (endStream ? H2Flags::EndStream : 0);
    frame.streamId = streamId;
    frame.payload = std::move(hpackData);
    return EncodeFrame(frame);
}

} // namespace instruments
