#pragma once

#include <stdint.h>

// Prevent a single valid-but-empty feed response from clearing the radar.
// Poll failures never reach this gate, so the last rendered snapshot is also
// retained while the network is unavailable.
class AircraftSnapshotGate {
public:
    bool shouldPublish(bool hasAircraft, uint32_t nowMs, uint32_t emptyGraceMs) {
        if (hasAircraft) {
            _emptyPending = false;
            return true;
        }

        if (!_emptyPending) {
            _emptyPending = true;
            _emptySinceMs = nowMs;
        }
        return (uint32_t)(nowMs - _emptySinceMs) >= emptyGraceMs;
    }

private:
    bool _emptyPending = false;
    uint32_t _emptySinceMs = 0;
};
