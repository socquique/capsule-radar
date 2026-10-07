// Strict web-form number parsing + range snapping (src/web_input.h). Guards the /save
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

    // --- range snapping onto RANGE_STEPS_KM ---
    assert(snapRangeKm(30.0f) == 30.0f);       // already a step: unchanged
    assert(snapRangeKm(24.0f) == 20.0f);
    assert(snapRangeKm(26.0f) == 30.0f);
    assert(snapRangeKm(15.0f) == 10.0f);       // equidistant tie -> lower step
    assert(snapRangeKm(0.0f) == 10.0f);        // stored 0 must never reach projectToScreen
    // For an absurd input the only contract is "lands on SOME step" (float ULPs make
    // 1e9-100 and 1e9-150 tie), never a div-by-zero 0.
    const float absurd = snapRangeKm(1e9f);
    bool absurdIsStep = false;
    for (float s : RANGE_STEPS_KM) if (s == absurd) absurdIsStep = true;
    assert(absurdIsStep);
    return 0;
}
