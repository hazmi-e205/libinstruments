#ifndef INSTRUMENTS_DTX_MESSAGE_H
#define INSTRUMENTS_DTX_MESSAGE_H

#include "types.h"
#include "../../src/nskeyedarchiver/nsobject.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace instruments {

// DTX message type codes
enum class DTXMessageType : uint32_t {
    Ack = 0x0,
    Unknown1 = 0x1,
    MethodInvocation = 0x2,
    ResponseWithPayload = 0x3,
    Error = 0x4,
    LZ4Compressed = 0x0707,
};

// DTX message header (32 bytes, little-endian)
struct DTXMessageHeader {
    uint32_t magic = DTXProtocol::Magic;
    uint32_t headerLength = DTXProtocol::HeaderLength;
    uint16_t fragmentIndex = 0;
    uint16_t fragmentCount = 1;
    uint32_t messageLength = 0;     // length after header
    uint32_t identifier = 0;
    uint32_t conversationIndex = 0;
    uint32_t channelCode = 0;
    uint32_t expectsReply = 0;
};

// DTX payload header (16 bytes, little-endian)
struct DTXPayloadHeader {
    uint32_t messageType = 0;
    uint32_t auxiliaryLength = 0;   // includes 16-byte aux header if present
    uint32_t totalPayloadLength = 0;
    uint32_t flags = 0;
};

// DTX message - represents a complete DTX protocol message
class DTXMessage {
public:
    DTXMessage();

    // Factory methods
    static std::shared_ptr<DTXMessage> Create();
    static std::shared_ptr<DTXMessage> CreateWithSelector(const std::string& selector);
    static std::shared_ptr<DTXMessage> CreateAck(uint32_t identifier, uint32_t channelCode);

    // Header fields
    uint32_t Identifier() const { return m_header.identifier; }
    void SetIdentifier(uint32_t id) { m_header.identifier = id; }

    uint32_t ChannelCode() const { return m_header.channelCode; }
    void SetChannelCode(uint32_t code) { m_header.channelCode = code; }

    uint32_t ConversationIndex() const { return m_header.conversationIndex; }
    void SetConversationIndex(uint32_t idx) { m_header.conversationIndex = idx; }

    bool ExpectsReply() const { return m_header.expectsReply != 0; }
    void SetExpectsReply(bool v) { m_header.expectsReply = v ? 1 : 0; }

    uint16_t FragmentIndex() const { return m_header.fragmentIndex; }
    uint16_t FragmentCount() const { return m_header.fragmentCount; }

    DTXMessageType MessageType() const { return static_cast<DTXMessageType>(m_payloadHeader.messageType); }
    void SetMessageType(DTXMessageType type) { m_payloadHeader.messageType = static_cast<uint32_t>(type); }

    // Payload - the selector or return value (NSKeyedArchiver encoded)
    void SetPayload(const NSObject& obj);
    std::shared_ptr<NSObject> PayloadObject() const;
    const std::vector<uint8_t>& RawPayload() const { return m_payload; }

    // Selector convenience
    std::string Selector() const;

    // Auxiliary data - method arguments
    void AppendAuxiliary(const NSObject& value);
    std::vector<NSObject> AuxiliaryObjects() const;
    const std::vector<uint8_t>& RawAuxiliary() const { return m_auxiliary; }

    // Encoding/Decoding
    // Encode to wire format (may produce multiple fragments)
    std::vector<std::vector<uint8_t>> Encode() const;

    // Decode from wire data (after fragment reassembly)
    // data should contain payload header + payload + auxiliary
    static std::shared_ptr<DTXMessage> Decode(const DTXMessageHeader& header,
                                              const uint8_t* data, size_t length);

    // Parse just the header from raw bytes
    static bool ParseHeader(const uint8_t* data, size_t length, DTXMessageHeader& outHeader);

    // Debug dump
    std::string Dump() const;

private:
    DTXMessageHeader m_header;
    DTXPayloadHeader m_payloadHeader;

    std::vector<uint8_t> m_payload;     // NSKeyedArchiver-encoded payload
    std::vector<uint8_t> m_auxiliary;   // PrimitiveDictionary-encoded auxiliary
    std::vector<NSObject> m_auxItems;   // Decoded auxiliary items (for encoding)
};

} // namespace instruments

#endif // INSTRUMENTS_DTX_MESSAGE_H
