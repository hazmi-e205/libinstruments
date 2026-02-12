#ifndef INSTRUMENTS_DTX_PRIMITIVE_DICT_H
#define INSTRUMENTS_DTX_PRIMITIVE_DICT_H

#include "../nskeyedarchiver/nsobject.h"
#include <cstdint>
#include <vector>

namespace instruments {

// DTX PrimitiveDictionary - binary format for auxiliary data in DTX messages.
// Each entry is: type(4 bytes LE) + length(4 bytes LE) + data(length bytes)
//
// Type codes:
//   0x01 = string (NSKeyedArchiver-encoded)
//   0x02 = byte array (NSKeyedArchiver-encoded)
//   0x03 = uint32
//   0x06 = int64
//   0x0A = null
//
// The full auxiliary section has a 16-byte header:
//   bufferSize(4) + unknown(4) + auxSize(4) + unknown(4)
// followed by the primitive dictionary entries.

namespace PrimitiveDictType {
    constexpr uint32_t String = 0x01;
    constexpr uint32_t ByteArray = 0x02;
    constexpr uint32_t UInt32 = 0x03;
    constexpr uint32_t Int64 = 0x06;
    constexpr uint32_t Null = 0x0A;
}

class DTXPrimitiveDict {
public:
    // Encode a list of NSObject values into auxiliary binary format
    // (includes the 16-byte auxiliary header)
    static std::vector<uint8_t> Encode(const std::vector<NSObject>& items);

    // Decode auxiliary binary data into a list of NSObject values.
    // Input should include the 16-byte auxiliary header.
    static std::vector<NSObject> Decode(const uint8_t* data, size_t length);
    static std::vector<NSObject> Decode(const std::vector<uint8_t>& data);

    // Encode a single NSObject into a primitive dictionary entry (without header)
    static std::vector<uint8_t> EncodeEntry(const NSObject& item);

    // Decode entries only (no 16-byte header)
    static std::vector<NSObject> DecodeEntries(const uint8_t* data, size_t length);
};

} // namespace instruments

#endif // INSTRUMENTS_DTX_PRIMITIVE_DICT_H
