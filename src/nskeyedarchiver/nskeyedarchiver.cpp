#include "nskeyedarchiver.h"
#include <plist/plist.h>
#include <cassert>

namespace instruments {

// Helper class to build the $objects array and track UIDs
class ArchiverContext {
public:
    ArchiverContext() {
        m_objects = plist_new_array();
        // Index 0 is always "$null"
        plist_array_append_item(m_objects, plist_new_string("$null"));
    }

    ~ArchiverContext() {
        if (m_objects) {
            plist_free(m_objects);
            m_objects = nullptr;
        }
    }

    // Add an object to the $objects array, return its UID
    uint64_t AddObject(plist_t obj) {
        uint64_t uid = plist_array_get_size(m_objects);
        plist_array_append_item(m_objects, obj);
        return uid;
    }

    // Encode an NSObject, returning its UID in the $objects array
    uint64_t Encode(const NSObject& obj) {
        switch (obj.GetType()) {
            case NSObject::Type::Null:
                return 0; // "$null" is always at index 0

            case NSObject::Type::Bool:
                return AddObject(plist_new_bool(obj.AsBool()));

            case NSObject::Type::Int32:
            case NSObject::Type::Int64:
                return AddObject(plist_new_int(obj.AsInt64()));

            case NSObject::Type::UInt64:
                return AddObject(plist_new_uint(obj.AsUInt64()));

            case NSObject::Type::Float32:
                return AddObject(plist_new_real(obj.AsFloat()));

            case NSObject::Type::Float64:
                return AddObject(plist_new_real(obj.AsDouble()));

            case NSObject::Type::String:
                return AddObject(plist_new_string(obj.AsString().c_str()));

            case NSObject::Type::Data:
                return AddObject(plist_new_data(
                    reinterpret_cast<const char*>(obj.AsData().data()),
                    obj.AsData().size()));

            case NSObject::Type::Array:
                return EncodeArray(obj);

            case NSObject::Type::Set:
                return EncodeSet(obj);

            case NSObject::Type::Dictionary:
                return EncodeDict(obj);
        }
        return 0;
    }

    // Encode with explicit class info
    uint64_t EncodeWithClass(const NSObject& obj,
                             const std::string& className,
                             const std::vector<std::string>& hierarchy) {
        NSObject copy = obj;
        copy.SetClassName(className);
        copy.SetClassHierarchy(hierarchy);
        return Encode(copy);
    }

    plist_t GetObjects() { return m_objects; }

private:
    plist_t m_objects;

    // Create a $class entry and return its UID
    uint64_t AddClass(const std::string& className,
                      const std::vector<std::string>& hierarchy) {
        plist_t classDict = plist_new_dict();

        // $classname
        plist_dict_set_item(classDict, "$classname",
                           plist_new_string(className.c_str()));

        // $classes array
        plist_t classes = plist_new_array();
        for (const auto& cls : hierarchy) {
            plist_array_append_item(classes, plist_new_string(cls.c_str()));
        }
        plist_dict_set_item(classDict, "$classes", classes);

        return AddObject(classDict);
    }

    uint64_t EncodeArray(const NSObject& obj) {
        const auto& items = obj.AsArray();

        // Encode all items first to get their UIDs
        std::vector<uint64_t> itemUids;
        for (const auto& item : items) {
            itemUids.push_back(Encode(item));
        }

        // Create the array container dict
        plist_t arrayDict = plist_new_dict();

        // NS.objects - array of UIDs
        plist_t nsObjects = plist_new_array();
        for (uint64_t uid : itemUids) {
            plist_array_append_item(nsObjects, plist_new_uid(uid));
        }
        plist_dict_set_item(arrayDict, "NS.objects", nsObjects);

        // Determine class
        std::string className = obj.ClassName().empty() ? "NSArray" : obj.ClassName();
        std::vector<std::string> hierarchy = obj.ClassHierarchy();
        if (hierarchy.empty()) {
            hierarchy = {"NSArray", "NSObject"};
        }

        // Add class reference
        uint64_t classUid = AddClass(className, hierarchy);
        plist_dict_set_item(arrayDict, "$class", plist_new_uid(classUid));

        return AddObject(arrayDict);
    }

    uint64_t EncodeSet(const NSObject& obj) {
        const auto& items = obj.AsArray();

        std::vector<uint64_t> itemUids;
        for (const auto& item : items) {
            itemUids.push_back(Encode(item));
        }

        plist_t setDict = plist_new_dict();

        plist_t nsObjects = plist_new_array();
        for (uint64_t uid : itemUids) {
            plist_array_append_item(nsObjects, plist_new_uid(uid));
        }
        plist_dict_set_item(setDict, "NS.objects", nsObjects);

        std::string className = obj.ClassName().empty() ? "NSSet" : obj.ClassName();
        std::vector<std::string> hierarchy = obj.ClassHierarchy();
        if (hierarchy.empty()) {
            hierarchy = {"NSSet", "NSObject"};
        }

        uint64_t classUid = AddClass(className, hierarchy);
        plist_dict_set_item(setDict, "$class", plist_new_uid(classUid));

        return AddObject(setDict);
    }

    uint64_t EncodeDict(const NSObject& obj) {
        const auto& dict = obj.AsDict();

        // Encode keys and values
        std::vector<uint64_t> keyUids, valUids;
        for (const auto& [key, val] : dict) {
            keyUids.push_back(Encode(NSObject(key)));
            valUids.push_back(Encode(val));
        }

        plist_t dictNode = plist_new_dict();

        // NS.keys
        plist_t nsKeys = plist_new_array();
        for (uint64_t uid : keyUids) {
            plist_array_append_item(nsKeys, plist_new_uid(uid));
        }
        plist_dict_set_item(dictNode, "NS.keys", nsKeys);

        // NS.objects
        plist_t nsObjects = plist_new_array();
        for (uint64_t uid : valUids) {
            plist_array_append_item(nsObjects, plist_new_uid(uid));
        }
        plist_dict_set_item(dictNode, "NS.objects", nsObjects);

        std::string className = obj.ClassName().empty() ? "NSDictionary" : obj.ClassName();
        std::vector<std::string> hierarchy = obj.ClassHierarchy();
        if (hierarchy.empty()) {
            hierarchy = {"NSDictionary", "NSObject"};
        }

        uint64_t classUid = AddClass(className, hierarchy);
        plist_dict_set_item(dictNode, "$class", plist_new_uid(classUid));

        return AddObject(dictNode);
    }
};

std::vector<uint8_t> NSKeyedArchiver::Archive(const NSObject& root) {
    std::string className = root.ClassName();
    std::vector<std::string> hierarchy = root.ClassHierarchy();

    if (!className.empty()) {
        return Archive(root, className, hierarchy);
    }

    // Infer class from type
    switch (root.GetType()) {
        case NSObject::Type::Dictionary:
            return Archive(root, "NSDictionary", {"NSDictionary", "NSObject"});
        case NSObject::Type::Array:
            return Archive(root, "NSArray", {"NSArray", "NSObject"});
        case NSObject::Type::Set:
            return Archive(root, "NSSet", {"NSSet", "NSObject"});
        case NSObject::Type::String:
            return Archive(root, "NSString", {"NSString", "NSObject"});
        case NSObject::Type::Data:
            return Archive(root, "NSData", {"NSData", "NSObject"});
        default:
            break;
    }

    // For primitives, encode directly without class wrapper
    ArchiverContext ctx;
    uint64_t rootUid = ctx.Encode(root);

    // Build the archive plist
    plist_t archive = plist_new_dict();
    plist_dict_set_item(archive, "$archiver",
                       plist_new_string("NSKeyedArchiver"));
    plist_dict_set_item(archive, "$version",
                       plist_new_uint(100000));

    plist_t top = plist_new_dict();
    plist_dict_set_item(top, "root", plist_new_uid(rootUid));
    plist_dict_set_item(archive, "$top", top);
    plist_dict_set_item(archive, "$objects", plist_copy(ctx.GetObjects()));

    // Serialize to binary plist
    char* binData = nullptr;
    uint32_t binLen = 0;
    plist_to_bin(archive, &binData, &binLen);
    plist_free(archive);

    std::vector<uint8_t> result;
    if (binData && binLen > 0) {
        result.assign(reinterpret_cast<uint8_t*>(binData),
                     reinterpret_cast<uint8_t*>(binData) + binLen);
        plist_mem_free(binData);
    }
    return result;
}

std::vector<uint8_t> NSKeyedArchiver::Archive(const NSObject& root,
                                              const std::string& className,
                                              const std::vector<std::string>& classHierarchy) {
    ArchiverContext ctx;
    uint64_t rootUid = ctx.EncodeWithClass(root, className, classHierarchy);

    plist_t archive = plist_new_dict();
    plist_dict_set_item(archive, "$archiver",
                       plist_new_string("NSKeyedArchiver"));
    plist_dict_set_item(archive, "$version",
                       plist_new_uint(100000));

    plist_t top = plist_new_dict();
    plist_dict_set_item(top, "root", plist_new_uid(rootUid));
    plist_dict_set_item(archive, "$top", top);
    plist_dict_set_item(archive, "$objects", plist_copy(ctx.GetObjects()));

    char* binData = nullptr;
    uint32_t binLen = 0;
    plist_to_bin(archive, &binData, &binLen);
    plist_free(archive);

    std::vector<uint8_t> result;
    if (binData && binLen > 0) {
        result.assign(reinterpret_cast<uint8_t*>(binData),
                     reinterpret_cast<uint8_t*>(binData) + binLen);
        plist_mem_free(binData);
    }
    return result;
}

} // namespace instruments
