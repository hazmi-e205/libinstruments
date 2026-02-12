#ifndef INSTRUMENTS_NSKEYEDUNARCHIVER_H
#define INSTRUMENTS_NSKEYEDUNARCHIVER_H

#include "nsobject.h"
#include <cstdint>
#include <vector>

namespace instruments {

// NSKeyedUnarchiver - Decodes Apple's NSKeyedArchiver binary plist format
// back into NSObject values.
class NSKeyedUnarchiver {
public:
    // Unarchive from binary plist data
    static NSObject Unarchive(const std::vector<uint8_t>& data);
    static NSObject Unarchive(const uint8_t* data, size_t length);
};

} // namespace instruments

#endif // INSTRUMENTS_NSKEYEDUNARCHIVER_H
