#include "snapshot_gate.h"

#include <assert.h>
#include <stdint.h>

int main() {
    AircraftSnapshotGate gate;

    assert(gate.shouldPublish(true, 1000, 15000));
    assert(!gate.shouldPublish(false, 2000, 15000));
    assert(!gate.shouldPublish(false, 16999, 15000));
    assert(gate.shouldPublish(false, 17000, 15000));

    // A recovered non-empty snapshot resets the empty-response grace period.
    assert(gate.shouldPublish(true, 18000, 15000));
    assert(!gate.shouldPublish(false, 19000, 15000));
    assert(!gate.shouldPublish(false, 20000, 15000));

    // Unsigned subtraction keeps expiry correct across millis() rollover.
    AircraftSnapshotGate rolloverGate;
    assert(!rolloverGate.shouldPublish(false, UINT32_MAX - 5, 10));
    assert(!rolloverGate.shouldPublish(false, 3, 10));
    assert(rolloverGate.shouldPublish(false, 4, 10));
}
