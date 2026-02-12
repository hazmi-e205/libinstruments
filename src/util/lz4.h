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

    // Decompress with known output size
    static bool Decompress(const uint8_t* src, size_t srcSize,
                           uint8_t* dst, size_t dstCapacity,
                           size_t& outSize);
};

} // namespace instruments

#endif // INSTRUMENTS_LZ4_H
