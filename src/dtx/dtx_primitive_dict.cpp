#include "dtx_primitive_dict.h"
#include "../nskeyedarchiver/nskeyedarchiver.h"
#include "../nskeyedarchiver/nskeyedunarchiver.h"
#include "../util/log.h"
#include <cstring>

namespace instruments {

static const char* TAG = "DTXPrimDict";

// Write a little-endian uint32
static void WriteLE32(std::vector<uint8_t>& buf, uint32_t val) {
    buf.push_back(static_cast<uint8_t>(val & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
}

// Write a little-endian uint64 (pymobiledevice3 uses unsigned for type 6)
static void WriteLE64(std::vector<uint8_t>& buf, uint64_t val) {
    for (int i = 0; i < 8; i++) {
        buf.push_back(static_cast<uint8_t>((val >> (i * 8)) & 0xFF));
    }
}

// Read a little-endian uint32
static uint32_t ReadLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

// Read a little-endian uint64 (pymobiledevice3 uses unsigned Int64ul for type 6)
static uint64_t ReadLE64(const uint8_t* p) {
    uint64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val |= (static_cast<uint64_t>(p[i]) << (i * 8));
    }
    return val;
}

std::vector<uint8_t> DTXPrimitiveDict::EncodeEntry(const NSObject& item) {
    std::vector<uint8_t> entry;

    // Write the "_empty_dictionary" marker (0x0A) before each entry to match pymobiledevice3/go-ios format
    WriteLE32(entry, 0x0A);

    switch (item.GetType()) {
        case NSObject::Type::Null: {
            WriteLE32(entry, PrimitiveDictType::Null);
            break;
        }
        case NSObject::Type::Int32: {
            WriteLE32(entry, PrimitiveDictType::UInt32);
            WriteLE32(entry, static_cast<uint32_t>(item.AsInt32()));
            break;
        }
        case NSObject::Type::UInt64: {
            // UInt64 encoded as int64 type (0x06) in primitive dict
            WriteLE32(entry, PrimitiveDictType::Int64);
            WriteLE64(entry, item.AsUInt64());
            break;
        }
        case NSObject::Type::Int64: {
            WriteLE32(entry, PrimitiveDictType::Int64);
            WriteLE64(entry, static_cast<uint64_t>(item.AsInt64()));
            break;
        }
        default: {
            // Everything else gets NSKeyedArchiver-encoded as a byte array
            std::vector<uint8_t> archived = NSKeyedArchiver::Archive(item);
            WriteLE32(entry, PrimitiveDictType::ByteArray);
            WriteLE32(entry, static_cast<uint32_t>(archived.size()));
            entry.insert(entry.end(), archived.begin(), archived.end());
            break;
        }
    }

    return entry;
}

std::vector<uint8_t> DTXPrimitiveDict::Encode(const std::vector<NSObject>& items) {
    // Encode entries only. The 16-byte auxiliary header is added at the message layer.
    std::vector<uint8_t> entries;
    for (const auto& item : items) {
        auto entry = EncodeEntry(item);
        entries.insert(entries.end(), entry.begin(), entry.end());
    }
    return entries;
}

std::vector<NSObject> DTXPrimitiveDict::DecodeEntries(const uint8_t* data, size_t length) {
    std::vector<NSObject> result;
    size_t offset = 0;

    while (offset + 8 <= length) {
        // Read the "_empty_dictionary" marker (4 bytes) that pymobiledevice3 uses
        uint32_t marker = ReadLE32(data + offset);
        uint32_t type = ReadLE32(data + offset + 4);
        offset += 8;

        INST_LOG_DEBUG(TAG, "Decode entry: marker=0x%x, type=0x%x", marker, type);

        switch (type) {
            case PrimitiveDictType::Null:
                result.push_back(NSObject::Null());
                break;

            case PrimitiveDictType::UInt32: {
                if (offset + 4 > length) {
                    INST_LOG_WARN(TAG, "Truncated uint32 entry at offset %zu", offset);
                    return result;
                }
                result.push_back(NSObject(static_cast<int32_t>(ReadLE32(data + offset))));
                offset += 4;
                break;
            }

            case PrimitiveDictType::Int64: {
                if (offset + 8 > length) {
                    INST_LOG_WARN(TAG, "Truncated int64 entry at offset %zu", offset);
                    return result;
                }
                // Type 6 is uint64 per pymobiledevice3 Int64ul
                result.push_back(NSObject(static_cast<uint64_t>(ReadLE64(data + offset))));
                offset += 8;
                break;
            }

            case PrimitiveDictType::String:
            case PrimitiveDictType::ByteArray: {
                if (offset + 4 > length) {
                    INST_LOG_WARN(TAG, "Truncated length entry at offset %zu", offset);
                    return result;
                }
                uint32_t entryLen = ReadLE32(data + offset);
                offset += 4;
                if (offset + entryLen > length) {
                    INST_LOG_WARN(TAG, "Truncated data entry at offset %zu, len=%u",
                                 offset, entryLen);
                    return result;
                }
                if (entryLen > 0) {
                    NSObject obj = NSKeyedUnarchiver::Unarchive(data + offset, entryLen);
                    result.push_back(std::move(obj));
                } else {
                    result.push_back(NSObject::Null());
                }
                offset += entryLen;
                break;
            }

            default:
                INST_LOG_WARN(TAG, "Unknown primitive dict type: 0x%x", type);
                result.push_back(NSObject::Null());
                return result;
        }
    }

    return result;
}

std::vector<NSObject> DTXPrimitiveDict::Decode(const uint8_t* data, size_t length) {
    // Data already excludes the 16-byte auxiliary header.
    if (length == 0) {
        return {};
    }
    return DecodeEntries(data, length);
}

std::vector<NSObject> DTXPrimitiveDict::Decode(const std::vector<uint8_t>& data) {
    return Decode(data.data(), data.size());
}

} // namespace instruments
