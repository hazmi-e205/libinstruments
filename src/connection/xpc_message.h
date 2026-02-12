#ifndef INSTRUMENTS_XPC_MESSAGE_H
#define INSTRUMENTS_XPC_MESSAGE_H

#include "../nskeyedarchiver/nsobject.h"
#include <cstdint>
#include <string>
#include <vector>

namespace instruments {

// XPC Message flags (from go-ios xpc protocol)
namespace XPCFlags {
    constexpr uint32_t AlwaysSet = 0x00000001;
    constexpr uint32_t Data = 0x00000002;
    constexpr uint32_t InitHandshake = 0x00000400;
    constexpr uint32_t Ping = 0x00000100;
    constexpr uint32_t FilePath = 0x00000200;
}

// XPC Message - used for iOS 17+ communication over HTTP/2.
// This is the wire format for CoreDevice/RSD protocol.
struct XPCMessage {
    uint32_t flags = 0;
    uint64_t messageId = 0;
    NSObject body;

    // Encode to binary
    std::vector<uint8_t> Encode() const;

    // Decode from binary
    static bool Decode(const uint8_t* data, size_t length, XPCMessage& outMsg);
    static bool Decode(const std::vector<uint8_t>& data, XPCMessage& outMsg);
};

// XPC-based service request for iOS 17+ AppService
struct XPCServiceRequest {
    std::string featureIdentifier;
    NSObject payload;

    // Encode as XPC message body
    NSObject ToBody() const;
};

// XPC-based service response
struct XPCServiceResponse {
    NSObject output;
    std::string errorDomain;
    int64_t errorCode = 0;
    std::string errorDescription;

    // Parse from XPC message body
    static XPCServiceResponse FromBody(const NSObject& body);
    bool HasError() const { return !errorDomain.empty(); }
};

} // namespace instruments

#endif // INSTRUMENTS_XPC_MESSAGE_H
