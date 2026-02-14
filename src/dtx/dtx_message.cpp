#include "../../include/instruments/dtx_message.h"
#include "dtx_primitive_dict.h"
#include "../nskeyedarchiver/nskeyedarchiver.h"
#include "../nskeyedarchiver/nskeyedunarchiver.h"
#include "../util/lz4.h"
#include "../util/log.h"
#include <cstring>
#include <sstream>

namespace instruments {

static const char* TAG = "DTXMessage";

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

std::shared_ptr<DTXMessage> DTXMessage::CreateAck(uint32_t identifier, uint32_t channelCode) {
    auto msg = std::make_shared<DTXMessage>();
    msg->SetMessageType(DTXMessageType::Ack);
    msg->SetIdentifier(identifier);
    msg->SetChannelCode(channelCode);
    msg->SetConversationIndex(identifier);
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
    bool hasPayload = (totalPayloadLen > 0 ||
                       MessageType() != DTXMessageType::Ack);

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

        auto decompressed = LZ4::Decompress(payloadStart + 8, remaining - 8, decompSize);
        if (decompressed.empty()) {
            INST_LOG_ERROR(TAG, "LZ4 decompression failed");
            return msg;
        }

        msg->m_payloadHeader.messageType = origType;

        // Re-parse the decompressed data
        size_t auxLen = msg->m_payloadHeader.auxiliaryLength;
        if (auxLen > 0 && auxLen <= decompressed.size()) {
            msg->m_auxiliary.assign(decompressed.begin(), decompressed.begin() + auxLen);
        }

        size_t payloadDataLen = decompressed.size() - auxLen;
        if (payloadDataLen > 0) {
            msg->m_payload.assign(decompressed.begin() + auxLen, decompressed.end());
        }

        return msg;
    }

    // Non-compressed: extract auxiliary and payload
    size_t auxLen = msg->m_payloadHeader.auxiliaryLength;
    if (auxLen > 0 && auxLen <= remaining) {
        if (auxLen <= DTXProtocol::PayloadHeaderLength) {
            INST_LOG_WARN(TAG, "Auxiliary length too small: %zu bytes", auxLen);
        } else {
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
