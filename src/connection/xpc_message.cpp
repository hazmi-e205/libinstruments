#include "xpc_message.h"
#include "../util/log.h"
#include <plist/plist.h>
#include <cstring>

namespace instruments {

static const char* TAG = "XPCMessage";

// Convert NSObject to plist_t (recursive)
static plist_t NSObjectToPlist(const NSObject& obj) {
    switch (obj.GetType()) {
        case NSObject::Type::Null:
            return plist_new_string("$null");
        case NSObject::Type::Bool:
            return plist_new_bool(obj.AsBool());
        case NSObject::Type::Int32:
        case NSObject::Type::Int64:
            return plist_new_int(obj.AsInt64());
        case NSObject::Type::UInt64:
            return plist_new_uint(obj.AsUInt64());
        case NSObject::Type::Float32:
        case NSObject::Type::Float64:
            return plist_new_real(obj.AsDouble());
        case NSObject::Type::String:
            return plist_new_string(obj.AsString().c_str());
        case NSObject::Type::Data:
            return plist_new_data(
                reinterpret_cast<const char*>(obj.AsData().data()),
                obj.AsData().size());
        case NSObject::Type::Array:
        case NSObject::Type::Set: {
            plist_t arr = plist_new_array();
            for (const auto& item : obj.AsArray()) {
                plist_array_append_item(arr, NSObjectToPlist(item));
            }
            return arr;
        }
        case NSObject::Type::Dictionary: {
            plist_t dict = plist_new_dict();
            for (const auto& [key, val] : obj.AsDict()) {
                plist_dict_set_item(dict, key.c_str(), NSObjectToPlist(val));
            }
            return dict;
        }
    }
    return plist_new_string("$null");
}

// Convert plist_t to NSObject (recursive)
static NSObject PlistToNSObject(plist_t node) {
    if (!node) return NSObject::Null();

    plist_type type = plist_get_node_type(node);
    switch (type) {
        case PLIST_BOOLEAN: {
            uint8_t val = 0;
            plist_get_bool_val(node, &val);
            return NSObject(static_cast<bool>(val));
        }
        case PLIST_INT: {
            int64_t val = 0;
            plist_get_int_val(node, &val);
            return NSObject(val);
        }
        case PLIST_REAL: {
            double val = 0.0;
            plist_get_real_val(node, &val);
            return NSObject(val);
        }
        case PLIST_STRING: {
            char* val = nullptr;
            plist_get_string_val(node, &val);
            if (val) {
                NSObject obj{std::string(val)};
                plist_mem_free(val);
                return obj;
            }
            return NSObject(std::string(""));
        }
        case PLIST_DATA: {
            char* val = nullptr;
            uint64_t len = 0;
            plist_get_data_val(node, &val, &len);
            if (val && len > 0) {
                std::vector<uint8_t> data(
                    reinterpret_cast<uint8_t*>(val),
                    reinterpret_cast<uint8_t*>(val) + len);
                plist_mem_free(val);
                return NSObject(std::move(data));
            }
            if (val) plist_mem_free(val);
            return NSObject(std::vector<uint8_t>{});
        }
        case PLIST_ARRAY: {
            NSObject::ArrayType arr;
            uint32_t count = plist_array_get_size(node);
            for (uint32_t i = 0; i < count; i++) {
                arr.push_back(PlistToNSObject(plist_array_get_item(node, i)));
            }
            return NSObject(std::move(arr));
        }
        case PLIST_DICT: {
            NSObject::DictType dict;
            plist_dict_iter iter = nullptr;
            plist_dict_new_iter(node, &iter);
            if (iter) {
                char* key = nullptr;
                plist_t val = nullptr;
                while (true) {
                    plist_dict_next_item(node, iter, &key, &val);
                    if (!key) break;
                    dict[key] = PlistToNSObject(val);
                    plist_mem_free(key);
                    key = nullptr;
                }
                free(iter);
            }
            return NSObject(std::move(dict));
        }
        default:
            return NSObject::Null();
    }
}

// XPC wire format:
// - 4 bytes: flags (LE)
// - 8 bytes: message ID (LE)
// - 8 bytes: body length (LE)
// - N bytes: body (binary plist)

std::vector<uint8_t> XPCMessage::Encode() const {
    std::vector<uint8_t> result;

    // Encode body to binary plist
    std::vector<uint8_t> bodyData;
    if (!body.IsNull()) {
        plist_t plistBody = NSObjectToPlist(body);

        char* binData = nullptr;
        uint32_t binLen = 0;
        plist_to_bin(plistBody, &binData, &binLen);
        plist_free(plistBody);

        if (binData && binLen > 0) {
            bodyData.assign(reinterpret_cast<uint8_t*>(binData),
                          reinterpret_cast<uint8_t*>(binData) + binLen);
            plist_mem_free(binData);
        }
    }

    // Write header
    result.resize(20 + bodyData.size());

    // Flags (4 bytes LE)
    result[0] = flags & 0xFF;
    result[1] = (flags >> 8) & 0xFF;
    result[2] = (flags >> 16) & 0xFF;
    result[3] = (flags >> 24) & 0xFF;

    // Message ID (8 bytes LE)
    for (int i = 0; i < 8; i++) {
        result[4 + i] = (messageId >> (i * 8)) & 0xFF;
    }

    // Body length (8 bytes LE)
    uint64_t bodyLen = bodyData.size();
    for (int i = 0; i < 8; i++) {
        result[12 + i] = (bodyLen >> (i * 8)) & 0xFF;
    }

    // Body
    if (!bodyData.empty()) {
        std::memcpy(result.data() + 20, bodyData.data(), bodyData.size());
    }

    return result;
}

bool XPCMessage::Decode(const uint8_t* data, size_t length, XPCMessage& outMsg) {
    if (length < 20) {
        INST_LOG_WARN(TAG, "XPC message too small: %zu bytes", length);
        return false;
    }

    // Flags
    outMsg.flags = static_cast<uint32_t>(data[0])
                 | (static_cast<uint32_t>(data[1]) << 8)
                 | (static_cast<uint32_t>(data[2]) << 16)
                 | (static_cast<uint32_t>(data[3]) << 24);

    // Message ID
    outMsg.messageId = 0;
    for (int i = 0; i < 8; i++) {
        outMsg.messageId |= (static_cast<uint64_t>(data[4 + i]) << (i * 8));
    }

    // Body length
    uint64_t bodyLen = 0;
    for (int i = 0; i < 8; i++) {
        bodyLen |= (static_cast<uint64_t>(data[12 + i]) << (i * 8));
    }

    if (20 + bodyLen > length) {
        INST_LOG_WARN(TAG, "XPC body extends beyond message: %llu + 20 > %zu",
                     (unsigned long long)bodyLen, length);
        return false;
    }

    // Parse body
    if (bodyLen > 0) {
        plist_t plistBody = nullptr;
        plist_from_bin(reinterpret_cast<const char*>(data + 20),
                      static_cast<uint32_t>(bodyLen), &plistBody);

        if (plistBody) {
            outMsg.body = PlistToNSObject(plistBody);
            plist_free(plistBody);
        }
    }

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
