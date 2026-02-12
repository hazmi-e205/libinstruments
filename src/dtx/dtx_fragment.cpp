#include "dtx_fragment.h"
#include "../util/log.h"

namespace instruments {

static const char* TAG = "DTXFragment";

bool DTXFragmentDecoder::AddFragment(uint32_t identifier, uint16_t fragmentIndex,
                                     uint16_t fragmentCount,
                                     const std::vector<uint8_t>& data) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto& state = m_pending[identifier];

    if (fragmentIndex == 0) {
        // First fragment - initialize state
        state.expectedCount = fragmentCount;
        state.receivedCount = 1;
        // Fragment 0 has no payload data (just the header)
        INST_LOG_TRACE(TAG, "Fragment start: id=%u, count=%u", identifier, fragmentCount);
        return fragmentCount == 1; // Single-fragment message is immediately complete
    }

    // Subsequent fragments carry payload data
    state.fragments[fragmentIndex] = data;
    state.totalSize += data.size();
    state.receivedCount++;

    INST_LOG_TRACE(TAG, "Fragment %u/%u for id=%u, size=%zu",
                  fragmentIndex, state.expectedCount, identifier, data.size());

    return state.receivedCount >= state.expectedCount;
}

std::vector<uint8_t> DTXFragmentDecoder::GetAssembledData(uint32_t identifier) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_pending.find(identifier);
    if (it == m_pending.end()) return {};

    auto& state = it->second;
    std::vector<uint8_t> result;
    result.reserve(state.totalSize);

    // Assemble fragments in order (1, 2, 3, ...)
    for (auto& [idx, fragment] : state.fragments) {
        result.insert(result.end(), fragment.begin(), fragment.end());
    }

    return result;
}

void DTXFragmentDecoder::Remove(uint32_t identifier) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.erase(identifier);
}

void DTXFragmentDecoder::Clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pending.clear();
}

} // namespace instruments
