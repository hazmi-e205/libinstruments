#ifndef INSTRUMENTS_NSKEYEDARCHIVER_H
#define INSTRUMENTS_NSKEYEDARCHIVER_H

#include "nsobject.h"
#include <cstdint>
#include <vector>

namespace instruments {

// NSKeyedArchiver - Encodes NSObject values into Apple's NSKeyedArchiver
// binary plist format. This is used by DTX protocol for method selectors
// and auxiliary arguments.
//
// Format:
// {
//   "$archiver": "NSKeyedArchiver",
//   "$version": 100000,
//   "$top": { "root": UID(1) },
//   "$objects": [ "$null", ... ]
// }
class NSKeyedArchiver {
public:
    // Archive a single value to binary plist
    static std::vector<uint8_t> Archive(const NSObject& root);

    // Archive with explicit class name and hierarchy
    // e.g., Archive(dictObj, "NSMutableDictionary", {"NSMutableDictionary", "NSDictionary", "NSObject"})
    static std::vector<uint8_t> Archive(const NSObject& root,
                                        const std::string& className,
                                        const std::vector<std::string>& classHierarchy);
};

} // namespace instruments

#endif // INSTRUMENTS_NSKEYEDARCHIVER_H
