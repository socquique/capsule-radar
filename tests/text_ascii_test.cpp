#include "text_ascii.h"

#include <assert.h>
#include <string.h>

static bool folds(const char *in, const char *want, size_t n = 40) {
    char out[64];
    text_ascii_fold(in, out, n);
    return strcmp(out, want) == 0;
}

int main() {
    // ASCII passes through unchanged.
    assert(folds("John Smith", "John Smith"));
    assert(folds("", ""));
    assert(folds(nullptr, ""));

    // Typographic punctuation.
    assert(folds("O’Brien", "O'Brien"));
    assert(folds("“Ace” – test…", "\"Ace\" - test..."));
    assert(folds("a b", "a b"));

    // Accented Latin letters; German/Nordic two-letter spellings.
    assert(folds("Jürgen Müller", "Juergen Mueller"));
    assert(folds("Großmann", "Grossmann"));
    assert(folds("André Muñoz", "Andre Munoz"));
    assert(folds("Søren Åse", "Soeren Aase"));
    assert(folds("Łukasz Żak, Šťastný", "Lukasz Zak, Stastny"));
    assert(folds("Œuvre", "OEuvre"));

    // No ASCII form: copied unchanged (still a valid UTF-8 sequence).
    assert(folds("Иван", "Иван"));

    // Invalid bytes become '?', and a truncated sequence does not swallow the next byte.
    assert(folds("a\x80" "b", "a?b"));
    assert(folds("a\xC3" "b", "a?b"));

    // The output is always terminated and never cuts a replacement or a character in half.
    assert(folds("abcü", "abc", 5));       // "ue" does not fit in the last 1 byte
    assert(folds("abcü", "abcue", 6));
    assert(folds("abИ", "ab", 4));         // 2-byte Cyrillic letter does not fit
    char tiny[1] = {'x'};
    text_ascii_fold("abc", tiny, 1);
    assert(tiny[0] == '\0');
    text_ascii_fold("abc", nullptr, 0);         // no crash
}
