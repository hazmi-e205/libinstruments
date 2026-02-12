#ifndef INSTRUMENTS_DTX_FRAGMENT_H
#define INSTRUMENTS_DTX_FRAGMENT_H

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

namespace instruments {

// DTX Fragment decoder for reassembling fragmented messages.
//
// DTX messages can be fragmented when they exceed the transport buffer size.
// Fragment structure:
// - Fragment 0: 32-byte header only (no payload)
// - Fragment 1..N: contain the actual payload data
// - The first fragment's header contains fragmentCount and total message length
// - Subsequent fragments are concatenated in order

class DTXFragmentDecoder {
public:
    // Add a fragment. Returns true when the message is complete.
    // identifier: message identifier
    // fragmentIndex: 0-based fragment index
    // fragmentCount: total number of fragments
    // data: fragment payload data (empty for fragment 0)
    bool AddFragment(uint32_t identifier, uint16_t fragmentIndex,
                     uint16_t fragmentCount, const std::vector<uint8_t>& data);

    // Get the assembled message data (only valid after AddFragment returns true)
    std::vector<uint8_t> GetAssembledData(uint32_t identifier);

    // Remove assembled data for an identifier
    void Remove(uint32_t identifier);

    // Clear all pending fragments
    void Clear();

private:
    struct FragmentState {
        uint16_t expectedCount = 0;
        uint16_t receivedCount = 0;
        size_t totalSize = 0;
        std::map<uint16_t, std::vector<uint8_t>> fragments;
    };

    std::mutex m_mutex;
    std::map<uint32_t, FragmentState> m_pending;
};

} // namespace instruments

#endif // INSTRUMENTS_DTX_FRAGMENT_H
