#pragma once

#include <string>

struct DisplayState;  // defined in tools/display_state.h

// RecoveryStrategy -- abstract interface for observer startup recovery.
//
// Each exchange protocol provides its own implementation:
//   - NullRecovery:  no-op, observer starts with an empty book (default)
//   - CmeRecovery:   join snapshot multicast, decode SnapshotFullRefreshOrderBook53
//   - IceRecovery:   TCP connect to snapshot server, decode SnapshotOrder messages
class RecoveryStrategy {
public:
    virtual ~RecoveryStrategy() = default;

    // Called once at startup. Blocks until initial book state is recovered.
    // Populates the provided DisplayState with the recovered book.
    virtual void recover(const std::string& instrument, DisplayState& ds) = 0;
};
