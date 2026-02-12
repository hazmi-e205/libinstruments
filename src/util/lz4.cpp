#include "lz4.h"
#include <cstring>

namespace instruments {

// Minimal LZ4 block decompression (LZ4_decompress_safe equivalent)
// Based on LZ4 block format specification:
// https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md

bool LZ4::Decompress(const uint8_t* src, size_t srcSize,
                     uint8_t* dst, size_t dstCapacity,
                     size_t& outSize) {
    if (!src || !dst || srcSize == 0 || dstCapacity == 0) {
        outSize = 0;
        return false;
    }

    const uint8_t* ip = src;
    const uint8_t* const iEnd = src + srcSize;
    uint8_t* op = dst;
    uint8_t* const oEnd = dst + dstCapacity;

    while (ip < iEnd) {
        // Read token
        uint8_t token = *ip++;

        // Literal length
        size_t literalLen = (token >> 4) & 0x0F;
        if (literalLen == 15) {
            uint8_t s;
            do {
                if (ip >= iEnd) return false;
                s = *ip++;
                literalLen += s;
            } while (s == 255);
        }

        // Copy literals
        if (literalLen > 0) {
            if (ip + literalLen > iEnd || op + literalLen > oEnd) return false;
            std::memcpy(op, ip, literalLen);
            ip += literalLen;
            op += literalLen;
        }

        // Check for end of input (last sequence has no match)
        if (ip >= iEnd) break;

        // Read offset (2 bytes, little-endian)
        if (ip + 2 > iEnd) return false;
        size_t offset = ip[0] | (ip[1] << 8);
        ip += 2;
        if (offset == 0) return false;

        uint8_t* match = op - offset;
        if (match < dst) return false;

        // Match length
        size_t matchLen = (token & 0x0F) + 4;  // minmatch = 4
        if (matchLen == 19) { // 15 + 4
            uint8_t s;
            do {
                if (ip >= iEnd) return false;
                s = *ip++;
                matchLen += s;
            } while (s == 255);
        }

        // Copy match (may overlap)
        if (op + matchLen > oEnd) return false;
        for (size_t i = 0; i < matchLen; i++) {
            op[i] = match[i];
        }
        op += matchLen;
    }

    outSize = static_cast<size_t>(op - dst);
    return true;
}

std::vector<uint8_t> LZ4::Decompress(const uint8_t* src, size_t srcSize,
                                     size_t maxDecompressedSize) {
    std::vector<uint8_t> result(maxDecompressedSize);
    size_t outSize = 0;
    if (!Decompress(src, srcSize, result.data(), maxDecompressedSize, outSize)) {
        return {};
    }
    result.resize(outSize);
    return result;
}

} // namespace instruments
