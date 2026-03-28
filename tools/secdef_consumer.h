#pragma once

#include "tools/instrument_info.h"

#include <string>
#include <unordered_map>

namespace exchange {

// SecdefConsumer -- abstract interface for security definition discovery.
//
// Implementations join a multicast channel, decode exchange-specific secdef
// messages, and produce an exchange-agnostic InstrumentInfo map.
//
// Usage:
//   auto consumer = CmeSecdefConsumer("239.0.31.3", 14312);
//   auto instruments = consumer.discover(35);
//   // instruments["ES"] -> InstrumentInfo{...}
class SecdefConsumer {
public:
    virtual ~SecdefConsumer() = default;

    // Block until all instrument definitions are received (or timeout).
    // Returns the discovered instrument map keyed by symbol.
    //
    // The 35-second default allows one full secdef cycle (30s) plus margin.
    // Returns an empty map on timeout with no instruments discovered.
    virtual std::unordered_map<std::string, InstrumentInfo>
        discover(int timeout_secs = 35) = 0;
};

}  // namespace exchange
