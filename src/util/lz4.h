#ifndef INSTRUMENTS_LZ4_H
#define INSTRUMENTS_LZ4_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace instruments {

// Minimal LZ4 block decompression for DTX compressed messages.
// Only implements LZ4_decompress_safe equivalent.
class LZ4 {
public:
    // Decompress LZ4 block format.
    // Returns decompressed data, or empty vector on failure.
    static std::vector<uint8_t> Decompress(const uint8_t* src, size_t srcSize,
                                           size_t maxDecompressedSize);

    // Decompress LZ4 frame format (magic 0x184D2204).
    // Returns decompressed data, or empty vector on failure.
    static std::vector<uint8_t> DecompressFrame(const uint8_t* src, size_t srcSize,
                                                size_t maxDecompressedSize);
    // Decompress LZ4 block with a dictionary (streaming decode).
    // dict may be null if dictSize is 0.
    static std::vector<uint8_t> DecompressWithDict(const uint8_t* src, size_t srcSize,
                                                   size_t maxDecompressedSize,
                                                   const uint8_t* dict, size_t dictSize);

    // Decompress with known output size
    static bool Decompress(const uint8_t* src, size_t srcSize,
                           uint8_t* dst, size_t dstCapacity,
                           size_t& outSize);
    static bool DecompressWithDict(const uint8_t* src, size_t srcSize,
                                   uint8_t* dst, size_t dstCapacity,
                                   size_t& outSize,
                                   const uint8_t* dict, size_t dictSize);
};

} // namespace instruments

#endif // INSTRUMENTS_LZ4_H
