#include "xpc_message.h"
#include "../util/log.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace instruments {

static const char* TAG = "XPCMessage";

namespace {
constexpr uint32_t kWrapperMagic = 0x29b00b92;
constexpr uint32_t kObjectMagic = 0x42133742;
constexpr uint32_t kBodyVersion = 0x00000005;

enum class XpcType : uint32_t {
    Null       = 0x00001000,
    Bool       = 0x00002000,
    Int64      = 0x00003000,
    UInt64     = 0x00004000,
    Double     = 0x00005000,
    Date       = 0x00007000,
    Data       = 0x00008000,
    String     = 0x00009000,
    Uuid       = 0x0000a000,
    Array      = 0x0000e000,
    Dictionary = 0x0000f000,
    FileTransfer = 0x0001a000,
};

static size_t Pad4(size_t n) {
    return (4 - (n % 4)) % 4;
}

static void WriteU32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

static void WriteU64(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

static bool ReadU32(const uint8_t*& p, const uint8_t* end, uint32_t& v) {
    if (static_cast<size_t>(end - p) < 4) return false;
    v = static_cast<uint32_t>(p[0])
      | (static_cast<uint32_t>(p[1]) << 8)
      | (static_cast<uint32_t>(p[2]) << 16)
      | (static_cast<uint32_t>(p[3]) << 24);
    p += 4;
    return true;
}

static bool ReadU64(const uint8_t*& p, const uint8_t* end, uint64_t& v) {
    if (static_cast<size_t>(end - p) < 8) return false;
    v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (static_cast<uint64_t>(p[i]) << (8 * i));
    }
    p += 8;
    return true;
}

static bool EncodeObject(const NSObject& obj, std::vector<uint8_t>& out);

static bool EncodeStringPayload(const std::string& s, std::vector<uint8_t>& out) {
    WriteU32(out, static_cast<uint32_t>(XpcType::String));
    const uint32_t len = static_cast<uint32_t>(s.size() + 1);
    WriteU32(out, len);
    out.insert(out.end(), s.begin(), s.end());
    out.push_back(0);
    out.insert(out.end(), Pad4(len), 0);
    return true;
}

static bool EncodeDataPayload(const std::vector<uint8_t>& d, std::vector<uint8_t>& out) {
    WriteU32(out, static_cast<uint32_t>(XpcType::Data));
    WriteU32(out, static_cast<uint32_t>(d.size()));
    out.insert(out.end(), d.begin(), d.end());
    out.insert(out.end(), Pad4(d.size()), 0);
    return true;
}

static bool EncodeArrayPayload(const NSObject::ArrayType& arr, std::vector<uint8_t>& out) {
    std::vector<uint8_t> payload;
    for (const auto& item : arr) {
        if (!EncodeObject(item, payload)) return false;
    }
    WriteU32(out, static_cast<uint32_t>(XpcType::Array));
    WriteU32(out, static_cast<uint32_t>(payload.size()));
    WriteU32(out, static_cast<uint32_t>(arr.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

static bool EncodeDictPayload(const NSObject::DictType& dict, std::vector<uint8_t>& out) {
    std::vector<uint8_t> payload;
    WriteU32(payload, static_cast<uint32_t>(dict.size()));
    for (const auto& kv : dict) {
        const std::string& key = kv.first;
        payload.insert(payload.end(), key.begin(), key.end());
        payload.push_back(0);
        payload.insert(payload.end(), Pad4(key.size() + 1), 0);
        if (!EncodeObject(kv.second, payload)) return false;
    }
    WriteU32(out, static_cast<uint32_t>(XpcType::Dictionary));
    WriteU32(out, static_cast<uint32_t>(payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return true;
}

static bool EncodeObject(const NSObject& obj, std::vector<uint8_t>& out) {
    switch (obj.GetType()) {
        case NSObject::Type::Null:
            WriteU32(out, static_cast<uint32_t>(XpcType::Null));
            return true;
        case NSObject::Type::Bool:
            WriteU32(out, static_cast<uint32_t>(XpcType::Bool));
            out.push_back(obj.AsBool() ? 1 : 0);
            out.push_back(0); out.push_back(0); out.push_back(0);
            return true;
        case NSObject::Type::Int32:
        case NSObject::Type::Int64:
            WriteU32(out, static_cast<uint32_t>(XpcType::Int64));
            WriteU64(out, static_cast<uint64_t>(obj.AsInt64()));
            return true;
        case NSObject::Type::UInt64:
            WriteU32(out, static_cast<uint32_t>(XpcType::UInt64));
            WriteU64(out, obj.AsUInt64());
            return true;
        case NSObject::Type::Float32:
        case NSObject::Type::Float64: {
            WriteU32(out, static_cast<uint32_t>(XpcType::Double));
            uint64_t bits = 0;
            double d = obj.AsDouble();
            std::memcpy(&bits, &d, sizeof(bits));
            WriteU64(out, bits);
            return true;
        }
        case NSObject::Type::String:
            return EncodeStringPayload(obj.AsString(), out);
        case NSObject::Type::Data:
            return EncodeDataPayload(obj.AsData(), out);
        case NSObject::Type::Array:
        case NSObject::Type::Set:
            return EncodeArrayPayload(obj.AsArray(), out);
        case NSObject::Type::Dictionary:
            return EncodeDictPayload(obj.AsDict(), out);
    }
    return false;
}

static bool DecodeObject(const uint8_t*& p, const uint8_t* end, NSObject& out);

static bool DecodeString(const uint8_t*& p, const uint8_t* end, NSObject& out) {
    uint32_t len = 0;
    if (!ReadU32(p, end, len)) return false;
    if (static_cast<size_t>(end - p) < len) return false;
    std::string s(reinterpret_cast<const char*>(p), reinterpret_cast<const char*>(p + len));
    while (!s.empty() && s.back() == '\0') s.pop_back();
    p += len;
    size_t pad = Pad4(len);
    if (static_cast<size_t>(end - p) < pad) return false;
    p += pad;
    out = NSObject(std::move(s));
    return true;
}

static bool DecodeData(const uint8_t*& p, const uint8_t* end, NSObject& out) {
    uint32_t len = 0;
    if (!ReadU32(p, end, len)) return false;
    if (static_cast<size_t>(end - p) < len) return false;
    std::vector<uint8_t> data(p, p + len);
    p += len;
    size_t pad = Pad4(len);
    if (static_cast<size_t>(end - p) < pad) return false;
    p += pad;
    out = NSObject(std::move(data));
    return true;
}

static bool DecodeArray(const uint8_t*& p, const uint8_t* end, NSObject& out) {
    uint32_t payloadLen = 0;
    uint32_t numEntries = 0;
    if (!ReadU32(p, end, payloadLen)) return false;
    if (!ReadU32(p, end, numEntries)) return false;
    if (static_cast<size_t>(end - p) < payloadLen) return false;
    const uint8_t* payloadEnd = p + payloadLen;

    NSObject::ArrayType arr;
    arr.reserve(numEntries);
    for (uint32_t i = 0; i < numEntries; i++) {
        NSObject item = NSObject::Null();
        if (!DecodeObject(p, payloadEnd, item)) return false;
        arr.push_back(std::move(item));
    }
    p = payloadEnd;
    out = NSObject(std::move(arr));
    return true;
}

static bool DecodeDictionary(const uint8_t*& p, const uint8_t* end, NSObject& out) {
    uint32_t payloadLen = 0;
    uint32_t numEntries = 0;
    if (!ReadU32(p, end, payloadLen)) return false;
    if (static_cast<size_t>(end - p) < payloadLen) return false;
    const uint8_t* payloadEnd = p + payloadLen;
    if (!ReadU32(p, payloadEnd, numEntries)) return false;

    NSObject::DictType dict;
    for (uint32_t i = 0; i < numEntries; i++) {
        const uint8_t* keyStart = p;
        while (p < payloadEnd && *p != 0) p++;
        if (p >= payloadEnd) return false;
        std::string key(reinterpret_cast<const char*>(keyStart), reinterpret_cast<const char*>(p));
        p++; // skip null
        size_t keyRawLen = key.size() + 1;
        size_t pad = Pad4(keyRawLen);
        if (static_cast<size_t>(payloadEnd - p) < pad) return false;
        p += pad;

        NSObject val = NSObject::Null();
        if (!DecodeObject(p, payloadEnd, val)) return false;
        dict[key] = std::move(val);
    }
    p = payloadEnd;
    out = NSObject(std::move(dict));
    return true;
}

static std::string FormatUuid(const uint8_t* b16) {
    char s[37] = {0};
    std::snprintf(
        s, sizeof(s),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        b16[0], b16[1], b16[2], b16[3], b16[4], b16[5], b16[6], b16[7],
        b16[8], b16[9], b16[10], b16[11], b16[12], b16[13], b16[14], b16[15]);
    return std::string(s);
}

static bool DecodeObject(const uint8_t*& p, const uint8_t* end, NSObject& out) {
    uint32_t typeU32 = 0;
    if (!ReadU32(p, end, typeU32)) return false;
    XpcType type = static_cast<XpcType>(typeU32);

    switch (type) {
        case XpcType::Null:
            out = NSObject::Null();
            return true;
        case XpcType::Bool: {
            if (static_cast<size_t>(end - p) < 4) return false;
            bool b = (p[0] != 0);
            p += 4;
            out = NSObject(b);
            return true;
        }
        case XpcType::Int64: {
            uint64_t u = 0;
            if (!ReadU64(p, end, u)) return false;
            out = NSObject(static_cast<int64_t>(u));
            return true;
        }
        case XpcType::UInt64: {
            uint64_t u = 0;
            if (!ReadU64(p, end, u)) return false;
            out = NSObject(u);
            return true;
        }
        case XpcType::Double: {
            uint64_t bits = 0;
            if (!ReadU64(p, end, bits)) return false;
            double d = 0.0;
            std::memcpy(&d, &bits, sizeof(d));
            out = NSObject(d);
            return true;
        }
        case XpcType::Date: {
            uint64_t u = 0;
            if (!ReadU64(p, end, u)) return false;
            out = NSObject(static_cast<int64_t>(u));
            return true;
        }
        case XpcType::String:
            return DecodeString(p, end, out);
        case XpcType::Data:
            return DecodeData(p, end, out);
        case XpcType::Uuid: {
            if (static_cast<size_t>(end - p) < 16) return false;
            const std::string uuidText = FormatUuid(p);
            p += 16;
            out = NSObject(uuidText);
            return true;
        }
        case XpcType::Array:
            return DecodeArray(p, end, out);
        case XpcType::Dictionary:
            return DecodeDictionary(p, end, out);
        case XpcType::FileTransfer: {
            uint64_t msgId = 0;
            if (!ReadU64(p, end, msgId)) return false;
            NSObject nested = NSObject::Null();
            if (!DecodeObject(p, end, nested)) return false;
            NSObject::DictType ft;
            ft["MsgId"] = NSObject(msgId);
            ft["Value"] = std::move(nested);
            out = NSObject(std::move(ft));
            return true;
        }
        default:
            INST_LOG_DEBUG(TAG, "Unsupported XPC object type: 0x%08x", typeU32);
            return false;
    }
}
} // namespace

std::vector<uint8_t> XPCMessage::Encode() const {
    std::vector<uint8_t> out;
    out.reserve(64);

    WriteU32(out, kWrapperMagic);
    WriteU32(out, flags);

    std::vector<uint8_t> bodyPayload;
    if (!body.IsNull()) {
        if (!EncodeObject(body, bodyPayload)) {
            INST_LOG_WARN(TAG, "Failed to encode XPC body object");
            bodyPayload.clear();
        }
    }

    uint64_t bodyLen = bodyPayload.empty() ? 0 : static_cast<uint64_t>(8 + bodyPayload.size());
    WriteU64(out, bodyLen);
    WriteU64(out, messageId);

    if (!bodyPayload.empty()) {
        WriteU32(out, kObjectMagic);
        WriteU32(out, kBodyVersion);
        out.insert(out.end(), bodyPayload.begin(), bodyPayload.end());
    }

    return out;
}

bool XPCMessage::Decode(const uint8_t* data, size_t length, XPCMessage& outMsg) {
    if (!data || length < 24) {
        INST_LOG_WARN(TAG, "XPC message too small: %zu bytes", length);
        return false;
    }

    const uint8_t* p = data;
    const uint8_t* end = data + length;

    uint32_t magic = 0;
    if (!ReadU32(p, end, magic) || magic != kWrapperMagic) {
        INST_LOG_DEBUG(TAG, "XPC wrapper magic mismatch: 0x%08x", magic);
        return false;
    }

    if (!ReadU32(p, end, outMsg.flags)) return false;

    uint64_t bodyLen = 0;
    if (!ReadU64(p, end, bodyLen)) return false;
    if (!ReadU64(p, end, outMsg.messageId)) return false;

    outMsg.body = NSObject::Null();
    if (bodyLen == 0) return true;

    if (static_cast<uint64_t>(end - p) < bodyLen || bodyLen < 8) {
        INST_LOG_WARN(TAG, "XPC body length invalid: %llu (available=%zu)",
                     static_cast<unsigned long long>(bodyLen), static_cast<size_t>(end - p));
        return false;
    }

    const uint8_t* bodyEnd = p + static_cast<size_t>(bodyLen);
    uint32_t objMagic = 0;
    uint32_t version = 0;
    if (!ReadU32(p, bodyEnd, objMagic) || !ReadU32(p, bodyEnd, version)) return false;
    if (objMagic != kObjectMagic || version != kBodyVersion) {
        INST_LOG_DEBUG(TAG, "XPC object header mismatch: magic=0x%08x version=0x%08x", objMagic, version);
        return false;
    }

    NSObject bodyObj = NSObject::Null();
    if (!DecodeObject(p, bodyEnd, bodyObj)) {
        uint32_t maybeType = 0;
        if (static_cast<size_t>(bodyEnd - p) >= 4) {
            const uint8_t* t = p;
            maybeType = static_cast<uint32_t>(t[0])
                      | (static_cast<uint32_t>(t[1]) << 8)
                      | (static_cast<uint32_t>(t[2]) << 16)
                      | (static_cast<uint32_t>(t[3]) << 24);
        }
        INST_LOG_WARN(TAG, "Failed to decode XPC body object (nextType=0x%08x, remaining=%zu)",
                      maybeType, static_cast<size_t>(bodyEnd - p));
        return false;
    }
    outMsg.body = std::move(bodyObj);
    return true;
}

bool XPCMessage::Decode(const std::vector<uint8_t>& data, XPCMessage& outMsg) {
    return Decode(data.data(), data.size(), outMsg);
}

NSObject XPCServiceRequest::ToBody() const {
    NSObject::DictType body;
    body["CoreDevice.featureIdentifier"] = NSObject(featureIdentifier);
    body["CoreDevice.action"] = NSObject::MakeDict({});
    body["CoreDevice.input"] = payload;
    return NSObject(std::move(body));
}

XPCServiceResponse XPCServiceResponse::FromBody(const NSObject& body) {
    XPCServiceResponse resp;

    if (body.IsDict()) {
        if (body.HasKey("CoreDevice.output")) {
            resp.output = body["CoreDevice.output"];
        }
        if (body.HasKey("CoreDevice.error")) {
            const auto& error = body["CoreDevice.error"];
            if (error.IsDict()) {
                if (error.HasKey("NSLocalizedDescription")) {
                    resp.errorDescription = error["NSLocalizedDescription"].AsString();
                }
                if (error.HasKey("domain")) {
                    resp.errorDomain = error["domain"].AsString();
                }
                if (error.HasKey("code")) {
                    resp.errorCode = error["code"].AsInt64();
                }
            }
        }
    }

    return resp;
}

} // namespace instruments
