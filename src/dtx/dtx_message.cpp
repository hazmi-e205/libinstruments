#include "../../include/instruments/dtx_message.h"
#include "dtx_primitive_dict.h"
#include "../nskeyedarchiver/nskeyedarchiver.h"
#include "../nskeyedarchiver/nskeyedunarchiver.h"
#include "../util/lz4.h"
#include "../util/log.h"
#include <atomic>
#include <cstring>
#include <fstream>
#include <sstream>

namespace instruments {

static const char* TAG = "DTXMessage";
static std::atomic<uint32_t> g_lz4DumpCounter{0};

static uint32_t ReadLE32(const uint8_t* p);

static void DumpLZ4Payload(const uint8_t* data, size_t len) {
    const uint32_t id = ++g_lz4DumpCounter;
    if (id > 3 || data == nullptr || len == 0) {
        return;
    }

    char filename[64] = {0};
    std::snprintf(filename, sizeof(filename), "sysmontap_raw_%04u.bin", id);
    std::ofstream out(filename, std::ios::binary);
    if (!out.is_open()) {
        INST_LOG_WARN(TAG, "Failed to open dump file: %s", filename);
        return;
    }
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    out.close();
    INST_LOG_INFO(TAG, "Wrote LZ4 raw payload dump: %s (%zu bytes)", filename, len);
}

static std::vector<uint8_t> TryDecodeBV4Container(const uint8_t* data, size_t len) {
    std::vector<uint8_t> out;
    if (data == nullptr || len < 8) {
        return out;
    }

    auto readBE32Local = [](const uint8_t* p) -> uint32_t {
        return (static_cast<uint32_t>(p[0]) << 24)
             | (static_cast<uint32_t>(p[1]) << 16)
             | (static_cast<uint32_t>(p[2]) << 8)
             | static_cast<uint32_t>(p[3]);
    };

    struct ChunkRef {
        bool compressed;
        uint32_t u;
        const uint8_t* data;
        uint32_t c;
    };

    std::vector<ChunkRef> sequence;
    std::vector<uint8_t> compressedAgg;
    std::vector<uint32_t> compU;
    size_t pos = 0;

    // First chunk: [u32 uncompressed][u32 compressed][compressed bytes]
    uint32_t u0 = ReadLE32(data + pos);
    uint32_t c0 = ReadLE32(data + pos + 4);
    pos += 8;
    if (c0 == 0 || pos + c0 > len) {
        return out;
    }
    sequence.push_back({true, u0, data + pos, c0});
    compU.push_back(u0);
    compressedAgg.insert(compressedAgg.end(), data + pos, data + pos + c0);
    pos += c0;

    while (pos + 4 <= len) {
        uint32_t tag = readBE32Local(data + pos);
        if (tag == 0x62763424) { // "bv4$"
            break;
        }
        if (tag == 0x62763431) { // "bv41" compressed chunk
            if (pos + 12 > len) {
                return {};
            }
            uint32_t u = ReadLE32(data + pos + 4);
            uint32_t c = ReadLE32(data + pos + 8);
            pos += 12;
            if (c == 0 || pos + c > len) {
                return {};
            }
            sequence.push_back({true, u, data + pos, c});
            compU.push_back(u);
            compressedAgg.insert(compressedAgg.end(), data + pos, data + pos + c);
            pos += c;
            continue;
        }
        if (tag == 0x6276342D) { // "bv4-" uncompressed chunk
            if (pos + 8 > len) {
                return {};
            }
            uint32_t u = ReadLE32(data + pos + 4);
            pos += 8;
            if (u == 0 || pos + u > len) {
                return {};
            }
            sequence.push_back({false, u, data + pos, u});
            pos += u;
            continue;
        }
        // Unknown tag
        break;
    }
    if (sequence.empty()) {
        return {};
    }

    // First try: decompress each chunk individually with a streaming dictionary.
    size_t totalOut = 0;
    for (const auto& ch : sequence) {
        totalOut += ch.compressed ? ch.u : ch.c;
    }
    out.reserve(totalOut);

    bool ok = true;
    for (const auto& ch : sequence) {
        if (ch.compressed) {
            const uint8_t* dict = nullptr;
            size_t dictSize = 0;
            if (!out.empty()) {
                dictSize = out.size() > 65536 ? 65536 : out.size();
                dict = out.data() + (out.size() - dictSize);
            }
            auto dec = LZ4::DecompressWithDict(ch.data, ch.c, ch.u, dict, dictSize);
            if (dec.empty()) {
                dec = LZ4::DecompressFrame(ch.data, ch.c, ch.u);
            }
            if (dec.empty()) {
                ok = false;
                break;
            }
            out.insert(out.end(), dec.begin(), dec.end());
        } else {
            out.insert(out.end(), ch.data, ch.data + ch.c);
        }
    }
    if (ok && !out.empty()) {
        return out;
    }

    // Fallback: aggregate all compressed chunks into one stream and decompress once.
    if (compressedAgg.empty()) {
        return {};
    }
    uint64_t totalU64 = 0;
    for (auto u : compU) totalU64 += u;
    size_t totalU = static_cast<size_t>(totalU64);

    auto decAll = LZ4::Decompress(compressedAgg.data(), compressedAgg.size(), totalU);
    if (decAll.empty()) {
        decAll = LZ4::DecompressFrame(compressedAgg.data(), compressedAgg.size(), totalU);
    }
    if (decAll.empty()) {
        return {};
    }

    out.clear();
    size_t decPos = 0;
    for (const auto& ch : sequence) {
        if (ch.compressed) {
            size_t take = ch.u;
            if (decPos + take > decAll.size()) {
                take = decAll.size() > decPos ? (decAll.size() - decPos) : 0;
            }
            if (take == 0) break;
            out.insert(out.end(), decAll.begin() + decPos, decAll.begin() + decPos + take);
            decPos += take;
        } else {
            out.insert(out.end(), ch.data, ch.data + ch.c);
        }
    }

    if (out.empty()) {
        return {};
    }
    return out;
}

// Little-endian read/write helpers
static void WriteLE16(uint8_t* p, uint16_t val) {
    p[0] = val & 0xFF;
    p[1] = (val >> 8) & 0xFF;
}

static void WriteLE32(uint8_t* p, uint32_t val) {
    p[0] = val & 0xFF;
    p[1] = (val >> 8) & 0xFF;
    p[2] = (val >> 16) & 0xFF;
    p[3] = (val >> 24) & 0xFF;
}

static void WriteBE32(uint8_t* p, uint32_t val) {
    p[0] = (val >> 24) & 0xFF;
    p[1] = (val >> 16) & 0xFF;
    p[2] = (val >> 8) & 0xFF;
    p[3] = val & 0xFF;
}

static uint16_t ReadLE16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

static uint32_t ReadLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

DTXMessage::DTXMessage() = default;

std::shared_ptr<DTXMessage> DTXMessage::Create() {
    return std::make_shared<DTXMessage>();
}

std::shared_ptr<DTXMessage> DTXMessage::CreateWithSelector(const std::string& selector) {
    auto msg = std::make_shared<DTXMessage>();
    msg->SetMessageType(DTXMessageType::MethodInvocation);
    msg->SetExpectsReply(true);

    // Payload is the selector string, NSKeyedArchiver-encoded
    msg->SetPayload(NSObject(selector));
    return msg;
}

std::shared_ptr<DTXMessage> DTXMessage::CreateAck(uint32_t identifier, uint32_t channelCode, uint32_t conversationIndex) {
    auto msg = std::make_shared<DTXMessage>();
    msg->SetMessageType(DTXMessageType::Ack);
    msg->SetIdentifier(identifier);
    msg->SetChannelCode(channelCode);
    // Per go-ios: ACK uses ConversationIndex + 1
    msg->SetConversationIndex(conversationIndex + 1);
    msg->SetExpectsReply(false);
    return msg;
}

void DTXMessage::SetPayload(const NSObject& obj) {
    m_payload = NSKeyedArchiver::Archive(obj);
}

std::shared_ptr<NSObject> DTXMessage::PayloadObject() const {
    if (m_payload.empty()) return nullptr;
    auto obj = std::make_shared<NSObject>(
        NSKeyedUnarchiver::Unarchive(m_payload));
    if (obj->IsNull()) return nullptr;
    return obj;
}

std::string DTXMessage::Selector() const {
    auto obj = PayloadObject();
    if (obj && obj->IsString()) {
        return obj->AsString();
    }
    return "";
}

void DTXMessage::AppendAuxiliary(const NSObject& value) {
    m_auxItems.push_back(value);
    // Re-encode all auxiliary items
    m_auxiliary = DTXPrimitiveDict::Encode(m_auxItems);
}

std::vector<NSObject> DTXMessage::AuxiliaryObjects() const {
    if (m_auxiliary.empty()) return {};
    return DTXPrimitiveDict::Decode(m_auxiliary);
}

bool DTXMessage::ParseHeader(const uint8_t* data, size_t length, DTXMessageHeader& outHeader) {
    if (length < DTXProtocol::HeaderLength) return false;

    outHeader.magic = ReadLE32(data);
    if (outHeader.magic != DTXProtocol::Magic) {
        // Check big-endian
        uint32_t beMagic = (static_cast<uint32_t>(data[0]) << 24)
                         | (static_cast<uint32_t>(data[1]) << 16)
                         | (static_cast<uint32_t>(data[2]) << 8)
                         | static_cast<uint32_t>(data[3]);
        if (beMagic != DTXProtocol::Magic) {
            INST_LOG_ERROR(TAG, "Invalid DTX magic: 0x%08X", outHeader.magic);
            return false;
        }
        // Use big-endian magic value
        outHeader.magic = beMagic;
    }

    outHeader.headerLength = ReadLE32(data + 4);
    outHeader.fragmentIndex = ReadLE16(data + 8);
    outHeader.fragmentCount = ReadLE16(data + 10);
    outHeader.messageLength = ReadLE32(data + 12);
    outHeader.identifier = ReadLE32(data + 16);
    outHeader.conversationIndex = ReadLE32(data + 20);
    outHeader.channelCode = ReadLE32(data + 24);
    outHeader.expectsReply = ReadLE32(data + 28);

    return true;
}

std::vector<std::vector<uint8_t>> DTXMessage::Encode() const {
    size_t auxLen = m_auxiliary.size();
    size_t payloadLen = m_payload.size();
    size_t auxLenWithHeader = auxLen > 0 ? auxLen + DTXProtocol::PayloadHeaderLength : 0;
    size_t totalPayloadLen = auxLenWithHeader + payloadLen;
    // ACK messages still include a 16-byte payload header (go-ios behavior)
    bool hasPayload = (totalPayloadLen > 0 ||
                       MessageType() == DTXMessageType::Ack);

    std::vector<uint8_t> payloadSection;
    if (hasPayload) {
        // Payload header (16 bytes) + auxiliary + payload
        payloadSection.resize(DTXProtocol::PayloadHeaderLength);
        INST_LOG_INFO(TAG, "Encoding message: messageType=0x%04X (raw), expectsReply=%d, auxLen=%zu, totalLen=%zu",
                     m_payloadHeader.messageType,
                     m_header.expectsReply,
                     auxLenWithHeader,
                     totalPayloadLen);
        WriteLE32(payloadSection.data(), m_payloadHeader.messageType);
        WriteLE32(payloadSection.data() + 4, static_cast<uint32_t>(auxLenWithHeader));
        WriteLE32(payloadSection.data() + 8, static_cast<uint32_t>(totalPayloadLen));
        WriteLE32(payloadSection.data() + 12, m_payloadHeader.flags);

        if (auxLen > 0) {
            // 16-byte auxiliary header: magic (0x1F0) + aux size
            const uint64_t magic = 0x1F0;
            const uint64_t auxSize = static_cast<uint64_t>(auxLen);
            uint8_t auxHeader[DTXProtocol::PayloadHeaderLength];
            for (int i = 0; i < 8; i++) {
                auxHeader[i] = static_cast<uint8_t>((magic >> (i * 8)) & 0xFF);
            }
            for (int i = 0; i < 8; i++) {
                auxHeader[8 + i] = static_cast<uint8_t>((auxSize >> (i * 8)) & 0xFF);
            }
            payloadSection.insert(payloadSection.end(), auxHeader, auxHeader + DTXProtocol::PayloadHeaderLength);
            payloadSection.insert(payloadSection.end(), m_auxiliary.begin(), m_auxiliary.end());
        }
        payloadSection.insert(payloadSection.end(), m_payload.begin(), m_payload.end());
    }
    // ACK messages: no payload section (messageLength = 0)

    // Build complete message: header + optional payload section
    std::vector<uint8_t> message(DTXProtocol::HeaderLength);
    // Magic is written big-endian (matches go-ios fixtures)
    WriteBE32(message.data(), DTXProtocol::Magic);
    WriteLE32(message.data() + 4, DTXProtocol::HeaderLength);
    WriteLE16(message.data() + 8, 0);  // fragmentIndex
    WriteLE16(message.data() + 10, 1); // fragmentCount
    WriteLE32(message.data() + 12, static_cast<uint32_t>(payloadSection.size()));
    WriteLE32(message.data() + 16, m_header.identifier);
    WriteLE32(message.data() + 20, m_header.conversationIndex);
    WriteLE32(message.data() + 24, m_header.channelCode);
    WriteLE32(message.data() + 28, m_header.expectsReply);

    message.insert(message.end(), payloadSection.begin(), payloadSection.end());

    // For now, return as a single fragment
    // TODO: Fragment if message exceeds transport buffer size
    return {message};
}

std::shared_ptr<DTXMessage> DTXMessage::Decode(const DTXMessageHeader& header,
                                                const uint8_t* data, size_t length) {
    auto msg = std::make_shared<DTXMessage>();
    msg->m_header = header;

    if (length == 0) {
        // ACK or header-only message
        return msg;
    }

    auto parsePayloadSection = [&](const uint8_t* buf, size_t len) -> bool {
        if (len < DTXProtocol::PayloadHeaderLength) {
            return false;
        }

        DTXPayloadHeader ph;
        ph.messageType = ReadLE32(buf);
        ph.auxiliaryLength = ReadLE32(buf + 4);
        ph.totalPayloadLength = ReadLE32(buf + 8);
        ph.flags = ReadLE32(buf + 12);

        const size_t remaining = len - DTXProtocol::PayloadHeaderLength;
        if (ph.totalPayloadLength > remaining) {
            return false;
        }
        if (ph.auxiliaryLength > ph.totalPayloadLength) {
            return false;
        }
        if (ph.messageType == 0 || ph.messageType == static_cast<uint32_t>(DTXMessageType::LZ4Compressed)) {
            return false;
        }

        msg->m_payloadHeader = ph;

        const uint8_t* payloadStart = buf + DTXProtocol::PayloadHeaderLength;
        size_t auxLen = ph.auxiliaryLength;
        if (auxLen > 0 && auxLen <= remaining) {
            if (auxLen > DTXProtocol::PayloadHeaderLength) {
                const uint8_t* auxStart = payloadStart + DTXProtocol::PayloadHeaderLength;
                size_t auxDataLen = auxLen - DTXProtocol::PayloadHeaderLength;
                msg->m_auxiliary.assign(auxStart, auxStart + auxDataLen);
            }
        }

        size_t payloadDataOffset = auxLen;
        size_t payloadDataLen = remaining > payloadDataOffset ? remaining - payloadDataOffset : 0;
        if (payloadDataLen > 0) {
            msg->m_payload.assign(payloadStart + payloadDataOffset,
                                 payloadStart + payloadDataOffset + payloadDataLen);
        }

        return true;
    };

    auto tryBplistFallback = [&](const uint8_t* scan, size_t scanLen, uint32_t msgType,
                                 const char* label) -> bool {
        if (!scan || scanLen < 8) {
            return false;
        }

        const uint8_t magic[] = { 'b','p','l','i','s','t' };
        const uint8_t* found = nullptr;
        for (size_t i = 0; i + sizeof(magic) <= scanLen; i++) {
            if (std::memcmp(scan + i, magic, sizeof(magic)) == 0) {
                found = scan + i;
                break;
            }
        }
        if (!found) {
            return false;
        }

        auto readBE64 = [](const uint8_t* p) -> uint64_t {
            uint64_t v = 0;
            for (int i = 0; i < 8; i++) {
                v = (v << 8) | static_cast<uint64_t>(p[i]);
            }
            return v;
        };

        auto findNextPlist = [&](const uint8_t* start, size_t maxLen) -> const uint8_t* {
            const uint8_t magic2[] = { 'b','p','l','i','s','t' };
            for (size_t i = sizeof(magic2); i + sizeof(magic2) <= maxLen; i++) {
                if (std::memcmp(start + i, magic2, sizeof(magic2)) == 0) {
                    return start + i;
                }
            }
            return nullptr;
        };

        auto findPlistLength = [&](const uint8_t* start, size_t maxLen) -> size_t {
            if (maxLen < 32) return 0;
            for (size_t end = maxLen; end >= 32; end--) {
                const uint8_t* trailer = start + end - 32;
                uint8_t offsetIntSize = trailer[6];
                uint8_t objectRefSize = trailer[7];
                if (offsetIntSize == 0 || offsetIntSize > 8) continue;
                if (objectRefSize == 0 || objectRefSize > 8) continue;

                uint64_t numObjects = readBE64(trailer + 8);
                uint64_t topObject = readBE64(trailer + 16);
                uint64_t offsetTableOffset = readBE64(trailer + 24);

                if (numObjects == 0 || numObjects > 0xFFFFFFFFULL) continue;
                if (topObject >= numObjects) continue;
                if (offsetTableOffset >= (end - 32)) continue;
                if (offsetTableOffset + (numObjects * offsetIntSize) > (end - 32)) continue;

                return end;
            }
            return 0;
        };

        const size_t maxLen = static_cast<size_t>((scan + scanLen) - found);
        size_t plistLen = 0;
        size_t nextOffset = 0;

        // If multiple bplists are concatenated, end at the next magic.
        if (const uint8_t* next = findNextPlist(found, maxLen)) {
            nextOffset = static_cast<size_t>(next - found);
            plistLen = nextOffset;
        }
        if (plistLen == 0) {
            plistLen = findPlistLength(found, maxLen);
        }
        if (plistLen == 0) {
            plistLen = maxLen;
        }

        INST_LOG_INFO(TAG, "Bplist fallback (%s): foundOffset=%zu nextOffset=%zu plistLen=%zu maxLen=%zu",
                      label ? label : "unknown",
                      static_cast<size_t>(found - scan),
                      nextOffset, plistLen, maxLen);
        if (plistLen >= 8) {
            char head[3 * 9] = {0};
            for (size_t i = 0; i < 8; i++) {
                std::snprintf(head + i * 3, sizeof(head) - i * 3, "%02X ", found[i]);
            }
            INST_LOG_INFO(TAG, "Bplist header (%s): %s", label ? label : "unknown", head);
        }

        std::vector<uint8_t> raw(found, found + plistLen);
        msg->m_payloadHeader.messageType = msgType;
        msg->m_payloadHeader.auxiliaryLength = 0;
        msg->m_payloadHeader.totalPayloadLength = static_cast<uint32_t>(raw.size());
        msg->m_payloadHeader.flags = 0;
        msg->m_payload = std::move(raw);
        return true;
    };

    // Parse payload header
    if (length < DTXProtocol::PayloadHeaderLength) {
        INST_LOG_WARN(TAG, "Payload too small: %zu bytes", length);
        return msg;
    }

    msg->m_payloadHeader.messageType = ReadLE32(data);
    msg->m_payloadHeader.auxiliaryLength = ReadLE32(data + 4);
    msg->m_payloadHeader.totalPayloadLength = ReadLE32(data + 8);
    msg->m_payloadHeader.flags = ReadLE32(data + 12);

    const uint8_t* payloadStart = data + DTXProtocol::PayloadHeaderLength;
    size_t remaining = length - DTXProtocol::PayloadHeaderLength;

    // Handle LZ4 compressed messages
    if (msg->MessageType() == DTXMessageType::LZ4Compressed) {
        if (remaining < 8) {
            INST_LOG_WARN(TAG, "LZ4 compressed message too small");
            return msg;
        }
        // First 4 bytes: decompressed message type
        uint32_t origType = ReadLE32(payloadStart);
        // Next 4 bytes: decompressed size
        uint32_t decompSize = ReadLE32(payloadStart + 4);
        if (decompSize == 0 || decompSize > (128u * 1024u * 1024u)) {
            uint32_t beType = (static_cast<uint32_t>(payloadStart[0]) << 24)
                            | (static_cast<uint32_t>(payloadStart[1]) << 16)
                            | (static_cast<uint32_t>(payloadStart[2]) << 8)
                            | static_cast<uint32_t>(payloadStart[3]);
            uint32_t beSize = (static_cast<uint32_t>(payloadStart[4]) << 24)
                            | (static_cast<uint32_t>(payloadStart[5]) << 16)
                            | (static_cast<uint32_t>(payloadStart[6]) << 8)
                            | static_cast<uint32_t>(payloadStart[7]);
            origType = beType;
            decompSize = beSize;
        }

        size_t maxOut = decompSize;
        if (maxOut == 0 || maxOut > (128u * 1024u * 1024u)) {
            maxOut = 64u * 1024u * 1024u;
        }

        auto decompressed = LZ4::Decompress(payloadStart + 8, remaining - 8, maxOut);
        if (decompressed.empty()) {
            // Try LZ4 frame format
            decompressed = LZ4::DecompressFrame(payloadStart + 8, remaining - 8, maxOut);
        }
        bool usedBv4 = false;
        if (decompressed.empty()) {
            // Try custom "bv4" container used by instruments sysmontap
            decompressed = TryDecodeBV4Container(payloadStart + 8, remaining - 8);
            if (!decompressed.empty()) {
                usedBv4 = true;
            }
        }
        if (decompressed.empty()) {
            // Dump first bytes to identify frame/block format
            char hex[3 * 17] = {0};
            const size_t dumpLen = remaining - 8 < 16 ? (remaining - 8) : 16;
            for (size_t i = 0; i < dumpLen; i++) {
                std::snprintf(hex + i * 3, sizeof(hex) - i * 3, "%02X ", payloadStart[8 + i]);
            }
            INST_LOG_ERROR(TAG, "LZ4 decompression failed (origType=0x%08X, decompSize=%u, first=%s)",
                          origType, decompSize, hex);

            DumpLZ4Payload(payloadStart + 8, remaining - 8);

            if (tryBplistFallback(payloadStart + 8, remaining - 8, origType, "lz4-raw")) {
                return msg;
            }

            return msg;
        }

        msg->m_payloadHeader.messageType = origType;
        if (usedBv4) {
            INST_LOG_INFO(TAG, "Decoded bv4 container: %zu bytes", decompressed.size());
        }

        // If decompressed data includes a payload header, parse it.
        if (parsePayloadSection(decompressed.data(), decompressed.size())) {
            return msg;
        }

        if (tryBplistFallback(decompressed.data(), decompressed.size(), origType,
                              usedBv4 ? "bv4" : "lz4-decompressed")) {
            return msg;
        }

        // Fallback: treat decompressed data as aux+payload (no payload header)
        size_t auxLen = msg->m_payloadHeader.auxiliaryLength;
        if (auxLen > 0 && auxLen <= decompressed.size()) {
            msg->m_auxiliary.assign(decompressed.begin(), decompressed.begin() + auxLen);
        }
        size_t payloadDataLen = decompressed.size() > auxLen ? decompressed.size() - auxLen : 0;
        if (payloadDataLen > 0) {
            msg->m_payload.assign(decompressed.begin() + auxLen, decompressed.end());
        }

        return msg;
    }

    // Non-compressed: extract auxiliary and payload
    parsePayloadSection(data, length);

    return msg;
}

std::string DTXMessage::Dump() const {
    std::ostringstream ss;
    ss << "DTXMessage{";
    ss << "id=" << m_header.identifier;
    ss << ", ch=" << m_header.channelCode;
    ss << ", conv=" << m_header.conversationIndex;
    ss << ", type=" << m_payloadHeader.messageType;
    ss << ", reply=" << (ExpectsReply() ? "yes" : "no");

    auto sel = Selector();
    if (!sel.empty()) {
        ss << ", selector=\"" << sel << "\"";
    }

    auto auxObjs = AuxiliaryObjects();
    if (!auxObjs.empty()) {
        ss << ", aux=[" << auxObjs.size() << " items]";
    }

    auto payload = PayloadObject();
    if (payload) {
        auto json = payload->ToJson();
        if (json.size() > 200) json = json.substr(0, 200) + "...";
        ss << ", payload=" << json;
    }

    ss << "}";
    return ss.str();
}

} // namespace instruments
