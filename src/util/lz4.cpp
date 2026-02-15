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

bool LZ4::DecompressWithDict(const uint8_t* src, size_t srcSize,
                             uint8_t* dst, size_t dstCapacity,
                             size_t& outSize,
                             const uint8_t* dict, size_t dictSize) {
    if (!src || !dst || srcSize == 0 || dstCapacity == 0) {
        outSize = 0;
        return false;
    }

    const uint8_t* ip = src;
    const uint8_t* const iEnd = src + srcSize;
    uint8_t* op = dst;
    uint8_t* const oEnd = dst + dstCapacity;

    while (ip < iEnd) {
        uint8_t token = *ip++;

        size_t literalLen = (token >> 4) & 0x0F;
        if (literalLen == 15) {
            uint8_t s;
            do {
                if (ip >= iEnd) return false;
                s = *ip++;
                literalLen += s;
            } while (s == 255);
        }

        if (literalLen > 0) {
            if (ip + literalLen > iEnd || op + literalLen > oEnd) return false;
            std::memcpy(op, ip, literalLen);
            ip += literalLen;
            op += literalLen;
        }

        if (ip >= iEnd) break;

        if (ip + 2 > iEnd) return false;
        size_t offset = ip[0] | (ip[1] << 8);
        ip += 2;
        if (offset == 0) return false;

        size_t produced = static_cast<size_t>(op - dst);
        size_t available = dictSize + produced;
        if (offset > available) return false;

        size_t matchLen = (token & 0x0F) + 4;
        if (matchLen == 19) {
            uint8_t s;
            do {
                if (ip >= iEnd) return false;
                s = *ip++;
                matchLen += s;
            } while (s == 255);
        }

        if (op + matchLen > oEnd) return false;

        // Copy match with optional dictionary support
        size_t matchPosCombined = dictSize + produced - offset;
        for (size_t i = 0; i < matchLen; i++) {
            size_t srcPos = matchPosCombined + i;
            if (srcPos < dictSize) {
                op[i] = dict[srcPos];
            } else {
                op[i] = dst[srcPos - dictSize];
            }
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

std::vector<uint8_t> LZ4::DecompressWithDict(const uint8_t* src, size_t srcSize,
                                             size_t maxDecompressedSize,
                                             const uint8_t* dict, size_t dictSize) {
    std::vector<uint8_t> result(maxDecompressedSize);
    size_t outSize = 0;
    if (!DecompressWithDict(src, srcSize, result.data(), maxDecompressedSize, outSize, dict, dictSize)) {
        return {};
    }
    result.resize(outSize);
    return result;
}

std::vector<uint8_t> LZ4::DecompressFrame(const uint8_t* src, size_t srcSize,
                                          size_t maxDecompressedSize) {
    if (!src || srcSize < 7) return {};

    auto readLE32 = [](const uint8_t* p) -> uint32_t {
        return static_cast<uint32_t>(p[0])
             | (static_cast<uint32_t>(p[1]) << 8)
             | (static_cast<uint32_t>(p[2]) << 16)
             | (static_cast<uint32_t>(p[3]) << 24);
    };

    const uint32_t magic = readLE32(src);
    if (magic != 0x184D2204) {
        return {};
    }

    size_t pos = 4;
    uint8_t flg = src[pos++];
    uint8_t bd = src[pos++];

    const uint8_t version = (flg >> 6) & 0x03;
    if (version != 0x01) {
        return {};
    }

    const bool hasContentSize = (flg & 0x08) != 0;
    const bool hasDictId = (flg & 0x01) != 0;

    uint64_t contentSize = 0;
    if (hasContentSize) {
        if (pos + 8 > srcSize) return {};
        contentSize = 0;
        for (int i = 0; i < 8; i++) {
            contentSize |= (static_cast<uint64_t>(src[pos + i]) << (8 * i));
        }
        pos += 8;
    }
    if (hasDictId) {
        if (pos + 4 > srcSize) return {};
        pos += 4;
    }

    // Skip header checksum
    if (pos + 1 > srcSize) return {};
    pos += 1;

    size_t blockMaxSize = 1 << 16; // default 64KB
    switch ((bd >> 4) & 0x07) {
        case 4: blockMaxSize = 1 << 16; break; // 64KB
        case 5: blockMaxSize = 1 << 18; break; // 256KB
        case 6: blockMaxSize = 1 << 20; break; // 1MB
        case 7: blockMaxSize = 1 << 22; break; // 4MB
        default: break;
    }

    size_t outputLimit = maxDecompressedSize;
    if (contentSize > 0 && contentSize < outputLimit) {
        outputLimit = static_cast<size_t>(contentSize);
    }

    std::vector<uint8_t> out;
    out.reserve(outputLimit > 0 ? outputLimit : 1024);

    while (pos + 4 <= srcSize) {
        uint32_t blockSize = readLE32(src + pos);
        pos += 4;
        if (blockSize == 0) break; // end of frame

        const bool uncompressed = (blockSize & 0x80000000u) != 0;
        blockSize &= 0x7FFFFFFFu;

        if (pos + blockSize > srcSize) return {};

        if (uncompressed) {
            if (out.size() + blockSize > outputLimit && outputLimit > 0) return {};
            out.insert(out.end(), src + pos, src + pos + blockSize);
            pos += blockSize;
            continue;
        }

        std::vector<uint8_t> blockOut(blockMaxSize);
        size_t blockOutSize = 0;
        if (!Decompress(src + pos, blockSize, blockOut.data(), blockOut.size(), blockOutSize)) {
            return {};
        }
        if (out.size() + blockOutSize > outputLimit && outputLimit > 0) return {};
        out.insert(out.end(), blockOut.begin(), blockOut.begin() + blockOutSize);
        pos += blockSize;
    }

    return out;
}

} // namespace instruments
