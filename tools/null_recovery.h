#pragma once

#include "tools/recovery_strategy.h"

// NullRecovery -- trivial recovery strategy that does nothing.
//
// Preserves the existing observer behavior: start with an empty book and
// build it up from incremental updates. Used as the default when no
// --recovery flag is specified.
class NullRecovery : public RecoveryStrategy {
public:
    void recover(const std::string& /*instrument*/,
                 DisplayState& /*ds*/) override {}
};
