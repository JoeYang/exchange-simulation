#pragma once
#include "exchange-core/types.h"
#include "exchange-core/matching_engine.h"
#include <string>

namespace exchange {

using InstrumentId = uint32_t;

struct InstrumentConfig {
    InstrumentId id{0};
    std::string symbol;
    EngineConfig engine_config;
};

}  // namespace exchange
