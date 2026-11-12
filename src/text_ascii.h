#pragma once
// Fold UTF-8 text from web APIs into the ASCII the built-in LVGL fonts can draw.
// The Montserrat fonts carry ASCII only, so any other letter shows as an empty box.
// Typographic punctuation becomes its ASCII form and accented Latin letters lose the
// accent; German and Nordic letters use their usual two-letter spelling (ü -> ue,
// ß -> ss, ø -> oe, å -> aa). Anything else (Cyrillic, Greek, CJK...) is copied as is.
// Header-only so the host tests can use it.
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ASCII for one code point (>= 0x80) into out; returns its length, 0 = no mapping.
inline size_t text_ascii_fold_cp(uint32_t cp, char out[4]) {
    // U+00C0..U+00FF
    static const char *const kLatin1[64] = {
        "A", "A", "A", "A", "Ae", "Aa", "Ae", "C", "E", "E", "E", "E", "I", "I", "I", "I",
        "D", "N", "O", "O", "O",  "O",  "Oe", "x", "Oe", "U", "U", "U", "Ue", "Y", "Th", "ss",
        "a", "a", "a", "a", "ae", "aa", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
        "d", "n", "o", "o", "o",  "o",  "oe", "/", "oe", "u", "u", "u", "ue", "y", "th", "y"};
    // U+0100..U+017F (Latin Extended-A: Polish, Czech, Turkish, Hungarian...), base letters
    static const char kExtA[] =
        "AaAaAaCcCcCcCcDd" "DdEeEeEeEeEeGgGg" "GgGgHhHhIiIiIiIi" "IiIiJjKkkLlLlLlL"
        "lLlNnNnNnnNnOoOo" "OoOoRrRrRrSsSsSs" "SsTtTtTtUuUuUuUu" "UuUuWwYyYZzZzZzs";
    const char *s = nullptr;
    if (cp == 0x00A0) s = " ";                              // no-break space
    else if (cp == 0x00B4) s = "'";                         // acute accent used as an apostrophe
    else if (cp >= 0x00C0 && cp <= 0x00FF) s = kLatin1[cp - 0x00C0];
    else if (cp == 0x0132) s = "IJ";
    else if (cp == 0x0133) s = "ij";
    else if (cp == 0x0152) s = "OE";
    else if (cp == 0x0153) s = "oe";
    else if (cp >= 0x0100 && cp <= 0x017F) { out[0] = kExtA[cp - 0x0100]; return 1; }
    else if (cp >= 0x2010 && cp <= 0x2015) s = "-";         // hyphens and dashes
    else if (cp >= 0x2018 && cp <= 0x201B) s = "'";         // single quotes
    else if (cp >= 0x201C && cp <= 0x201F) s = "\"";        // double quotes
    else if (cp == 0x2026) s = "...";
    if (!s) return 0;
    const size_t n = strlen(s);
    memcpy(out, s, n);
    return n;
}

// Copy in to out (size n, always terminated), folding as above. Never cuts a character
// or a two-letter replacement in half; stops early if the next one does not fit.
inline void text_ascii_fold(const char *in, char *out, size_t n) {
    if (!out || n == 0) return;
    size_t o = 0;
    const uint8_t *p = (const uint8_t *)(in ? in : "");
    while (*p) {
        // decode one UTF-8 sequence
        uint32_t cp = *p;
        size_t len = 1;
        if (cp >= 0xF0 && cp <= 0xF4) { cp &= 0x07; len = 4; }
        else if (cp >= 0xE0 && cp <= 0xEF) { cp &= 0x0F; len = 3; }
        else if (cp >= 0xC2 && cp <= 0xDF) { cp &= 0x1F; len = 2; }
        else if (cp >= 0x80) len = 0;                       // stray continuation / invalid lead
        for (size_t i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) { len = 0; break; }  // truncated sequence
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        char rep[4];
        const char *src;
        size_t rn;
        if (len == 0) { rep[0] = '?'; src = rep; rn = 1; len = 1; }
        else if (cp < 0x80) { src = (const char *)p; rn = 1; }
        else if ((rn = text_ascii_fold_cp(cp, rep)) > 0) src = rep;
        else { src = (const char *)p; rn = len; }           // no ASCII form: keep it
        if (o + rn >= n) break;
        memcpy(out + o, src, rn);
        o += rn;
        p += len;
    }
    out[o] = '\0';
}
