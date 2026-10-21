// Strict web-form number parsing + range repair (src/web_input.h). Guards the /save
// handler: String::toDouble() read "38,84" as 38 and "" as 0 — both inside the valid
// range, both saved, then the device restarted centred on the wrong point.
#include "web_input.h"

#include <assert.h>

int main() {
    double v;

    // --- accepted: full-string decimals, '.' or ',' separator ---
    assert(parseDoubleStrict("38.8409", &v) && v == 38.8409);
    assert(parseDoubleStrict("-0.1059", &v) && v == -0.1059);
    assert(parseDoubleStrict("38,84", &v) && v == 38.84);   // Spanish decimal comma
    assert(parseDoubleStrict("-,25", &v) && v == -0.25);
    assert(parseDoubleStrict("30", &v) && v == 30.0);
    assert(parseDoubleStrict("+5", &v) && v == 5.0);
    assert(parseDoubleStrict("0", &v) && v == 0.0);
    assert(parseDoubleStrict(".5", &v) && v == 0.5);
    assert(parseDoubleStrict("5.", &v) && v == 5.0);

    // --- rejected: everything String::toDouble() used to accept ---
    assert(!parseDoubleStrict("", &v));        // empty: was 0, in range, saved
    assert(!parseDoubleStrict("38", &v) == false);  // sanity: plain integer still fine
    assert(!parseDoubleStrict("abc", &v));
    assert(!parseDoubleStrict("38abc", &v));   // trailing garbage: was 38, saved
    assert(!parseDoubleStrict("3.8.4", &v));   // second separator
    assert(!parseDoubleStrict("1,2,3", &v));
    assert(!parseDoubleStrict("38 84", &v));   // embedded space
    assert(!parseDoubleStrict(" 38", &v));     // leading space
    assert(parseDoubleStrict("38", &v) && v == 38.0);  // plain integer still fine
    assert(!parseDoubleStrict("inf", &v));
    assert(!parseDoubleStrict("nan", &v));
    assert(!parseDoubleStrict("--3", &v));
    assert(!parseDoubleStrict("+", &v));
    assert(!parseDoubleStrict(".", &v));
    assert(!parseDoubleStrict(nullptr, &v));

    // failure must leave the caller's variable untouched
    double keep = 42.0;
    assert(!parseDoubleStrict("", &keep) && keep == 42.0);

    // --- range repair: only values that can't work change ---
    assert(repairRangeKm(30.0f) == 30.0f);     // a preset: unchanged
    assert(repairRangeKm(15.0f) == 15.0f);     // web-page presets the zoom ladder lacks stay
    assert(repairRangeKm(25.0f) == 25.0f);
    assert(repairRangeKm(20.0f) == 20.0f);     // a zoom-button step the web list lacks stays
    assert(repairRangeKm(42.0f) == 42.0f);     // any in-range value stays (nothing snaps)
    assert(repairRangeKm(150.0f) == 150.0f);   // the top of the web list
    assert(repairRangeKm(250.0f) == RANGE_KM_MAX);   // the old web page's 250 km -> 150
    assert(repairRangeKm(1e9f) == RANGE_KM_MAX);
    assert(repairRangeKm(5.0f) == RANGE_KM_MIN);
    assert(repairRangeKm(0.0f) == RANGE_KM_DEFAULT);  // 0 must never reach projectToScreen
    assert(repairRangeKm(-30.0f) == RANGE_KM_DEFAULT);
    assert(repairRangeKm(0.0f / 0.0f) == RANGE_KM_DEFAULT);   // NaN (a corrupt NVS float)
    assert(repairRangeKm(1.0f / 0.0f) == RANGE_KM_DEFAULT);   // +inf
    assert(repairRangeKm(-1.0f / 0.0f) == RANGE_KM_DEFAULT);  // -inf
    // The web list must stay inside the repair bounds, or the page would offer a value
    // that /save immediately changes.
    for (float s : WEB_RANGE_KM) assert(repairRangeKm(s) == s);
    for (float s : RANGE_STEPS_KM) assert(repairRangeKm(s) == s);
    return 0;
}
