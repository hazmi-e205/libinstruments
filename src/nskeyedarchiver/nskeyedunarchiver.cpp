#include "nskeyedunarchiver.h"
#include "../util/log.h"
#include <plist/plist.h>

namespace instruments {

static const char* TAG = "NSKeyedUnarchiver";

// Helper to resolve a UID reference in the $objects array
static plist_t ResolveUID(plist_t objects, plist_t uidNode) {
    if (!uidNode || plist_get_node_type(uidNode) != PLIST_UID) return nullptr;
    uint64_t uid = 0;
    plist_get_uid_val(uidNode, &uid);
    return plist_array_get_item(objects, static_cast<uint32_t>(uid));
}

// Forward declaration
static NSObject DecodeObject(plist_t objects, plist_t node);

// Decode a plist node that might be a UID reference
static NSObject DecodeValue(plist_t objects, plist_t node) {
    if (!node) return NSObject::Null();

    if (plist_get_node_type(node) == PLIST_UID) {
        plist_t resolved = ResolveUID(objects, node);
        return DecodeObject(objects, resolved);
    }

    return DecodeObject(objects, node);
}

// Get class name from a $class UID reference
static std::string GetClassName(plist_t objects, plist_t containerDict) {
    plist_t classUid = plist_dict_get_item(containerDict, "$class");
    if (!classUid) return "";

    plist_t classDict = ResolveUID(objects, classUid);
    if (!classDict) return "";

    plist_t classNameNode = plist_dict_get_item(classDict, "$classname");
    if (!classNameNode) return "";

    char* name = nullptr;
    plist_get_string_val(classNameNode, &name);
    std::string result = name ? name : "";
    plist_mem_free(name);
    return result;
}

// Decode an NSArray/NSMutableArray from keyed archiver format
static NSObject DecodeNSArray(plist_t objects, plist_t node) {
    plist_t nsObjects = plist_dict_get_item(node, "NS.objects");
    if (!nsObjects) return NSObject(NSObject::ArrayType{});

    NSObject::ArrayType items;
    uint32_t count = plist_array_get_size(nsObjects);
    for (uint32_t i = 0; i < count; i++) {
        plist_t item = plist_array_get_item(nsObjects, i);
        items.push_back(DecodeValue(objects, item));
    }
    return NSObject(std::move(items));
}

// Decode an NSSet/NSMutableSet
static NSObject DecodeNSSet(plist_t objects, plist_t node) {
    plist_t nsObjects = plist_dict_get_item(node, "NS.objects");
    if (!nsObjects) return NSObject::Set({});

    NSObject::ArrayType items;
    uint32_t count = plist_array_get_size(nsObjects);
    for (uint32_t i = 0; i < count; i++) {
        plist_t item = plist_array_get_item(nsObjects, i);
        items.push_back(DecodeValue(objects, item));
    }
    return NSObject::Set(std::move(items));
}

// Decode an NSDictionary/NSMutableDictionary
static NSObject DecodeNSDictionary(plist_t objects, plist_t node) {
    plist_t nsKeys = plist_dict_get_item(node, "NS.keys");
    plist_t nsValues = plist_dict_get_item(node, "NS.objects");
    if (!nsKeys || !nsValues) return NSObject(NSObject::DictType{});

    uint32_t keyCount = plist_array_get_size(nsKeys);
    uint32_t valCount = plist_array_get_size(nsValues);
    uint32_t count = keyCount < valCount ? keyCount : valCount;

    NSObject::DictType dict;
    for (uint32_t i = 0; i < count; i++) {
        plist_t keyNode = plist_array_get_item(nsKeys, i);
        plist_t valNode = plist_array_get_item(nsValues, i);

        NSObject key = DecodeValue(objects, keyNode);
        NSObject val = DecodeValue(objects, valNode);

        // Dictionary keys should be strings
        std::string keyStr;
        if (key.IsString()) {
            keyStr = key.AsString();
        } else {
            keyStr = key.ToJson();
        }
        dict[keyStr] = std::move(val);
    }
    return NSObject(std::move(dict));
}

// Decode NSMutableData / NSData
static NSObject DecodeNSData(plist_t objects, plist_t node) {
    plist_t nsData = plist_dict_get_item(node, "NS.data");
    if (!nsData) return NSObject(std::vector<uint8_t>{});

    if (plist_get_node_type(nsData) == PLIST_UID) {
        plist_t resolved = ResolveUID(objects, nsData);
        if (resolved && plist_get_node_type(resolved) == PLIST_DATA) {
            char* data = nullptr;
            uint64_t len = 0;
            plist_get_data_val(resolved, &data, &len);
            std::vector<uint8_t> result;
            if (data && len > 0) {
                result.assign(reinterpret_cast<uint8_t*>(data),
                             reinterpret_cast<uint8_t*>(data) + len);
                plist_mem_free(data);
            }
            return NSObject(std::move(result));
        }
    }

    if (plist_get_node_type(nsData) == PLIST_DATA) {
        char* data = nullptr;
        uint64_t len = 0;
        plist_get_data_val(nsData, &data, &len);
        std::vector<uint8_t> result;
        if (data && len > 0) {
            result.assign(reinterpret_cast<uint8_t*>(data),
                         reinterpret_cast<uint8_t*>(data) + len);
            plist_mem_free(data);
        }
        return NSObject(std::move(result));
    }

    return NSObject(std::vector<uint8_t>{});
}

// Decode NSMutableString / NSString
static NSObject DecodeNSString(plist_t objects, plist_t node) {
    plist_t nsString = plist_dict_get_item(node, "NS.string");
    if (!nsString) return NSObject(std::string(""));

    if (plist_get_node_type(nsString) == PLIST_UID) {
        plist_t resolved = ResolveUID(objects, nsString);
        if (resolved && plist_get_node_type(resolved) == PLIST_STRING) {
            char* str = nullptr;
            plist_get_string_val(resolved, &str);
            std::string result = str ? str : "";
            plist_mem_free(str);
            return NSObject(std::move(result));
        }
    }

    if (plist_get_node_type(nsString) == PLIST_STRING) {
        char* str = nullptr;
        plist_get_string_val(nsString, &str);
        std::string result = str ? str : "";
        plist_mem_free(str);
        return NSObject(std::move(result));
    }

    return NSObject(std::string(""));
}

// Decode a raw plist node (not a keyed archiver container)
static NSObject DecodePrimitive(plist_t node) {
    if (!node) return NSObject::Null();

    switch (plist_get_node_type(node)) {
        case PLIST_BOOLEAN: {
            uint8_t val = 0;
            plist_get_bool_val(node, &val);
            return NSObject(static_cast<bool>(val));
        }
        case PLIST_INT: {
            // Note: PLIST_UINT is #defined as PLIST_INT in newer libplist.
            // Use plist_get_uint_val to handle both signed and unsigned correctly.
            uint64_t uval = 0;
            plist_get_uint_val(node, &uval);
            // If the high bit is set, treat as unsigned; otherwise as signed
            if (uval > static_cast<uint64_t>(INT64_MAX)) {
                return NSObject(uval);
            }
            return NSObject(static_cast<int64_t>(uval));
        }
        case PLIST_REAL: {
            double val = 0.0;
            plist_get_real_val(node, &val);
            return NSObject(val);
        }
        case PLIST_STRING: {
            char* str = nullptr;
            plist_get_string_val(node, &str);
            std::string result = str ? str : "";
            plist_mem_free(str);
            return NSObject(std::move(result));
        }
        case PLIST_DATA: {
            char* data = nullptr;
            uint64_t len = 0;
            plist_get_data_val(node, &data, &len);
            std::vector<uint8_t> result;
            if (data && len > 0) {
                result.assign(reinterpret_cast<uint8_t*>(data),
                             reinterpret_cast<uint8_t*>(data) + len);
                plist_mem_free(data);
            }
            return NSObject(std::move(result));
        }
        case PLIST_ARRAY: {
            NSObject::ArrayType items;
            uint32_t count = plist_array_get_size(node);
            for (uint32_t i = 0; i < count; i++) {
                items.push_back(DecodePrimitive(plist_array_get_item(node, i)));
            }
            return NSObject(std::move(items));
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
                    dict[key] = DecodePrimitive(val);
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

// Main decode function - handles both keyed-archiver containers and plain plist nodes
static NSObject DecodeObject(plist_t objects, plist_t node) {
    if (!node) return NSObject::Null();

    plist_type type = plist_get_node_type(node);

    // If it's a plain string "$null", return null
    if (type == PLIST_STRING) {
        char* str = nullptr;
        plist_get_string_val(node, &str);
        if (str) {
            std::string s(str);
            plist_mem_free(str);
            if (s == "$null") return NSObject::Null();
            return NSObject(std::move(s));
        }
        return NSObject::Null();
    }

    // If it's not a dict, decode as primitive
    if (type != PLIST_DICT) {
        return DecodePrimitive(node);
    }

    // It's a dict - check if it has a $class key (keyed archiver container)
    std::string className = GetClassName(objects, node);
    if (className.empty()) {
        // Plain dictionary (not a keyed archiver object)
        return DecodePrimitive(node);
    }

    // Dispatch by class name
    if (className == "NSArray" || className == "NSMutableArray") {
        return DecodeNSArray(objects, node);
    }
    if (className == "NSSet" || className == "NSMutableSet") {
        return DecodeNSSet(objects, node);
    }
    if (className == "NSDictionary" || className == "NSMutableDictionary") {
        return DecodeNSDictionary(objects, node);
    }
    if (className == "NSData" || className == "NSMutableData") {
        return DecodeNSData(objects, node);
    }
    if (className == "NSString" || className == "NSMutableString") {
        return DecodeNSString(objects, node);
    }
    if (className == "NSValue" || className == "NSNumber") {
        // NSNumber is often stored with NS.intval, NS.dblval, etc.
        plist_t intVal = plist_dict_get_item(node, "NS.intval");
        if (intVal) return DecodeValue(objects, intVal);
        plist_t dblVal = plist_dict_get_item(node, "NS.dblval");
        if (dblVal) return DecodeValue(objects, dblVal);
        return NSObject::Null();
    }
    if (className == "NSDate") {
        plist_t timeVal = plist_dict_get_item(node, "NS.time");
        if (timeVal) return DecodeValue(objects, timeVal);
        return NSObject(0.0);
    }
    if (className == "NSUUID") {
        plist_t uuidBytes = plist_dict_get_item(node, "NS.uuidbytes");
        if (uuidBytes) return DecodeValue(objects, uuidBytes);
        return NSObject(std::vector<uint8_t>{});
    }
    if (className == "NSError" || className == "NSException") {
        // Decode as dictionary with relevant fields
        NSObject::DictType result;
        result["$class"] = NSObject(className);

        plist_t domain = plist_dict_get_item(node, "NSDomain");
        if (domain) result["domain"] = DecodeValue(objects, domain);

        plist_t code = plist_dict_get_item(node, "NSCode");
        if (code) result["code"] = DecodeValue(objects, code);

        plist_t userInfo = plist_dict_get_item(node, "NSUserInfo");
        if (userInfo) result["userInfo"] = DecodeValue(objects, userInfo);

        return NSObject(std::move(result));
    }
    if (className == "NSURL") {
        plist_t relative = plist_dict_get_item(node, "NS.relative");
        if (relative) return DecodeValue(objects, relative);
        return NSObject(std::string(""));
    }
    if (className == "DTTapMessage" || className == "DTSysmonTapMessage") {
        // DTTapMessage contains plistBytes
        plist_t plistBytes = plist_dict_get_item(node, "DTTapMessagePlist");
        if (plistBytes) {
            NSObject data = DecodeValue(objects, plistBytes);
            if (data.IsData() && !data.AsData().empty()) {
                // Parse the inner plist
                plist_t inner = nullptr;
                plist_from_bin(reinterpret_cast<const char*>(data.AsData().data()),
                              data.AsData().size(), &inner);
                if (inner) {
                    NSObject result = DecodePrimitive(inner);
                    plist_free(inner);
                    return result;
                }
            }
        }
        return NSObject::Null();
    }
    if (className == "XCTCapabilities") {
        // Decode as dict with capabilitiesDictionary
        plist_t capsDict = plist_dict_get_item(node, "capabilities-dictionary");
        if (capsDict) return DecodeValue(objects, capsDict);
        // Fallthrough to generic decode
    }

    // Unknown class - decode all keys as a dictionary
    INST_LOG_DEBUG(TAG, "Unknown class: %s, decoding as dict", className.c_str());
    NSObject::DictType result;
    result["$class"] = NSObject(className);

    plist_dict_iter iter = nullptr;
    plist_dict_new_iter(node, &iter);
    if (iter) {
        char* key = nullptr;
        plist_t val = nullptr;
        while (true) {
            plist_dict_next_item(node, iter, &key, &val);
            if (!key) break;
            std::string keyStr(key);
            plist_mem_free(key);
            key = nullptr;
            if (keyStr == "$class") continue; // skip $class
            result[keyStr] = DecodeValue(objects, val);
        }
        free(iter);
    }
    return NSObject(std::move(result));
}

NSObject NSKeyedUnarchiver::Unarchive(const uint8_t* data, size_t length) {
    if (!data || length == 0) return NSObject::Null();

    // Parse binary plist
    plist_t root = nullptr;
    plist_from_bin(reinterpret_cast<const char*>(data), static_cast<uint32_t>(length), &root);
    if (!root) {
        // Try XML format
        plist_from_xml(reinterpret_cast<const char*>(data), static_cast<uint32_t>(length), &root);
    }
    if (!root) {
        INST_LOG_ERROR(TAG, "Failed to parse plist data (%zu bytes)", length);
        return NSObject::Null();
    }

    // Check if this is a keyed archiver plist
    plist_t archiver = plist_dict_get_item(root, "$archiver");
    if (!archiver) {
        // Not a keyed archiver - decode as plain plist
        NSObject result = DecodePrimitive(root);
        plist_free(root);
        return result;
    }

    plist_t objects = plist_dict_get_item(root, "$objects");
    plist_t top = plist_dict_get_item(root, "$top");
    if (!objects || !top) {
        INST_LOG_ERROR(TAG, "Invalid keyed archiver format: missing $objects or $top");
        plist_free(root);
        return NSObject::Null();
    }

    // Get the root UID from $top
    // $top can have "root" or "$0", "$1", etc.
    plist_t rootUid = plist_dict_get_item(top, "root");
    if (!rootUid) {
        // Try $0
        rootUid = plist_dict_get_item(top, "$0");
    }

    NSObject result;
    if (rootUid) {
        result = DecodeValue(objects, rootUid);
    } else {
        // Multiple top-level objects
        NSObject::ArrayType items;
        plist_dict_iter iter = nullptr;
        plist_dict_new_iter(top, &iter);
        if (iter) {
            char* key = nullptr;
            plist_t val = nullptr;
            while (true) {
                plist_dict_next_item(top, iter, &key, &val);
                if (!key) break;
                items.push_back(DecodeValue(objects, val));
                plist_mem_free(key);
                key = nullptr;
            }
            free(iter);
        }
        if (items.size() == 1) {
            result = std::move(items[0]);
        } else {
            result = NSObject(std::move(items));
        }
    }

    plist_free(root);
    return result;
}

NSObject NSKeyedUnarchiver::Unarchive(const std::vector<uint8_t>& data) {
    return Unarchive(data.data(), data.size());
}

} // namespace instruments
