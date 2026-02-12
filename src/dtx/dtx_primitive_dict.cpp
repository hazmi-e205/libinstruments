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

// Write a little-endian int64
static void WriteLE64(std::vector<uint8_t>& buf, int64_t val) {
    uint64_t u = static_cast<uint64_t>(val);
    for (int i = 0; i < 8; i++) {
        buf.push_back(static_cast<uint8_t>((u >> (i * 8)) & 0xFF));
    }
}

// Read a little-endian uint32
static uint32_t ReadLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0])
         | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16)
         | (static_cast<uint32_t>(p[3]) << 24);
}

// Read a little-endian int64
static int64_t ReadLE64(const uint8_t* p) {
    uint64_t val = 0;
    for (int i = 0; i < 8; i++) {
        val |= (static_cast<uint64_t>(p[i]) << (i * 8));
    }
    return static_cast<int64_t>(val);
}

std::vector<uint8_t> DTXPrimitiveDict::EncodeEntry(const NSObject& item) {
    std::vector<uint8_t> entry;

    switch (item.GetType()) {
        case NSObject::Type::Null: {
            WriteLE32(entry, PrimitiveDictType::Null);
            WriteLE32(entry, 0);
            break;
        }
        case NSObject::Type::Int32: {
            WriteLE32(entry, PrimitiveDictType::UInt32);
            WriteLE32(entry, 4);
            WriteLE32(entry, static_cast<uint32_t>(item.AsInt32()));
            break;
        }
        case NSObject::Type::UInt64: {
            // UInt64 encoded as int64 in primitive dict
            WriteLE32(entry, PrimitiveDictType::Int64);
            WriteLE32(entry, 8);
            WriteLE64(entry, static_cast<int64_t>(item.AsUInt64()));
            break;
        }
        case NSObject::Type::Int64: {
            WriteLE32(entry, PrimitiveDictType::Int64);
            WriteLE32(entry, 8);
            WriteLE64(entry, item.AsInt64());
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
    // First, encode all entries
    std::vector<uint8_t> entries;
    for (const auto& item : items) {
        auto entry = EncodeEntry(item);
        entries.insert(entries.end(), entry.begin(), entry.end());
    }

    // Build the full auxiliary section with 16-byte header
    std::vector<uint8_t> result;
    uint32_t bufSize = static_cast<uint32_t>(entries.size() + 16);
    uint32_t auxSize = static_cast<uint32_t>(entries.size());

    WriteLE32(result, bufSize);     // total buffer size
    WriteLE32(result, 0);           // unknown (always 0)
    WriteLE32(result, auxSize);     // auxiliary data size
    WriteLE32(result, 0);           // unknown (always 0)

    result.insert(result.end(), entries.begin(), entries.end());
    return result;
}

std::vector<NSObject> DTXPrimitiveDict::DecodeEntries(const uint8_t* data, size_t length) {
    std::vector<NSObject> result;
    size_t offset = 0;

    while (offset + 8 <= length) {
        uint32_t type = ReadLE32(data + offset);
        uint32_t entryLen = ReadLE32(data + offset + 4);
        offset += 8;

        if (offset + entryLen > length) {
            INST_LOG_WARN(TAG, "Truncated entry at offset %zu, type=0x%x, len=%u",
                         offset - 8, type, entryLen);
            break;
        }

        switch (type) {
            case PrimitiveDictType::Null:
                result.push_back(NSObject::Null());
                break;

            case PrimitiveDictType::UInt32:
                if (entryLen >= 4) {
                    result.push_back(NSObject(static_cast<int32_t>(ReadLE32(data + offset))));
                }
                break;

            case PrimitiveDictType::Int64:
                if (entryLen >= 8) {
                    result.push_back(NSObject(ReadLE64(data + offset)));
                }
                break;

            case PrimitiveDictType::String:
            case PrimitiveDictType::ByteArray:
                if (entryLen > 0) {
                    NSObject obj = NSKeyedUnarchiver::Unarchive(data + offset, entryLen);
                    result.push_back(std::move(obj));
                } else {
                    result.push_back(NSObject::Null());
                }
                break;

            default:
                INST_LOG_WARN(TAG, "Unknown primitive dict type: 0x%x", type);
                result.push_back(NSObject::Null());
                break;
        }

        offset += entryLen;
    }

    return result;
}

std::vector<NSObject> DTXPrimitiveDict::Decode(const uint8_t* data, size_t length) {
    if (length < 16) {
        INST_LOG_WARN(TAG, "Auxiliary data too small: %zu bytes", length);
        return {};
    }

    // Parse 16-byte header
    // uint32_t bufSize = ReadLE32(data);
    // uint32_t unknown1 = ReadLE32(data + 4);
    uint32_t auxSize = ReadLE32(data + 8);
    // uint32_t unknown2 = ReadLE32(data + 12);

    if (auxSize > length - 16) {
        INST_LOG_WARN(TAG, "Auxiliary size %u exceeds available data %zu", auxSize, length - 16);
        auxSize = static_cast<uint32_t>(length - 16);
    }

    return DecodeEntries(data + 16, auxSize);
}

std::vector<NSObject> DTXPrimitiveDict::Decode(const std::vector<uint8_t>& data) {
    return Decode(data.data(), data.size());
}

} // namespace instruments
