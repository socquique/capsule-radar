// Host-side test for the ADS-B JSON parse path. Contributed by @geoffg28.
//
// Question under test: does the production ReliableJsonStream actually survive the failure
// mode that produced DeserializationError::IncompleteInput on the device, and what timeout
// does it need to do so? Runs against the real ArduinoJson v7 with the real production
// field filter.
//
// The mock reproduces the reported device behaviour: WiFiClientSecure hands over one TLS
// record's worth of bytes and then returns a transient negative read while the transfer is
// still very much alive.
//
// The answer below is "yes, but only with an explicit setTimeout()": Arduino Stream's
// default is 1000 ms, so any stall longer than that still dropped the poll (case 3).
// That finding is why fetchFrom() sets the wrapper's timeout to the HTTP client's budget.
//
// RECORDED CAVEAT (measured by @geoffg28 on his fork): parsing through an Arduino Stream
// costs ArduinoJson one readBytes() call PER BYTE (see ArduinoStreamReader.hpp), each going
// through Stream::timedRead() -- a millis() call plus two virtual dispatches. At ~25 KB per
// poll every 2 s that measured as a ~10% render frame-rate drop on the device
// (7.79 -> 7.02 fps, +14 ms per frame, three 180 s runs). A bulk-read-into-PSRAM-then-parse
// path avoids that cost; if feed CPU ever matters here, that is the direction to take.

#include <Arduino.h>
#include <ArduinoJson.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

unsigned long g_virtualMs = 0;

// ---------------------------------------------------------------------------
// ReliableJsonStream — copied verbatim from src/adsb_client.cpp.
// The runner script diffs this against the source to catch drift.
// ---------------------------------------------------------------------------
class ReliableJsonStream : public Stream {
public:
    explicit ReliableJsonStream(Stream& source) : _source(source) {}
    int available() override { return _source.available(); }
    int read() override {
        const int value = _source.read();
        if (value >= 0) ++_bytesRead;
        return value;
    }
    int peek() override { return _source.peek(); }
    void flush() override { _source.flush(); }
    size_t write(uint8_t) override { return 0; }
    size_t bytesRead() const { return _bytesRead; }

private:
    Stream& _source;
    size_t _bytesRead = 0;
};

// ---------------------------------------------------------------------------
// A socket that stalls mid-document, the way the TLS client does on the device.
// ---------------------------------------------------------------------------
class StallingSocket : public Stream {
public:
    // stallEveryNBytes: how far it gets between stalls. stallMs: how long each stall lasts.
    StallingSocket(std::string body, size_t stallEveryNBytes, unsigned long stallMs)
        : _body(std::move(body)), _every(stallEveryNBytes), _stallMs(stallMs) {}

    int available() override {
        if (_pos >= _body.size()) return 0;
        return _stalling() ? 0 : (int)(_body.size() - _pos);
    }

    int read() override {
        if (++_reads > 100000000) { printf("\nABORT: runaway read loop\n"); exit(2); }
        // Genuine end of body. The clock must still advance here, otherwise Stream::timedRead()
        // spins forever waiting for a timeout that can never arrive on a frozen virtual clock.
        if (_pos >= _body.size()) { g_virtualMs += 5; return -1; }
        if (_stalling()) {
            g_virtualMs += 5;                          // time passes while we wait for the peer
            ++_transientMisses;
            return -1;                                 // transient: more data IS coming
        }
        const unsigned char c = (unsigned char)_body[_pos++];
        if (_every && _pos % _every == 0 && _pos < _body.size()) _stallUntil = g_virtualMs + _stallMs;
        return (int)c;
    }

    int peek() override { return _pos < _body.size() && !_stalling() ? (unsigned char)_body[_pos] : -1; }
    size_t write(uint8_t) override { return 0; }
    bool connected() const { return _pos < _body.size(); }
    size_t transientMisses() const { return _transientMisses; }
    size_t delivered() const { return _pos; }

private:
    bool _stalling() const { return g_virtualMs < _stallUntil; }

    std::string   _body;
    size_t        _pos = 0;
    size_t        _every;
    unsigned long _stallMs;
    unsigned long _stallUntil = 0;
    size_t        _transientMisses = 0;
    long long     _reads = 0;
};

// Models NetworkClient's readBytes(), which upstream identified as treating a transient
// negative read as end-of-input. This is the "before the fix" consumer.
class NaiveClientView : public Stream {
public:
    explicit NaiveClientView(Stream& source) : _source(source) {}
    int available() override { return _source.available(); }
    int read() override { const int v = _source.read(); if (v >= 0) ++_bytesRead; return v; }
    int peek() override { return _source.peek(); }
    size_t write(uint8_t) override { return 0; }
    size_t bytesRead() const { return _bytesRead; }

    // The bug: bail out on the first negative read instead of retrying until the timeout.
    size_t readBytes(char *buffer, size_t length) override {
        size_t count = 0;
        while (count < length) {
            const int c = read();
            if (c < 0) break;
            *buffer++ = (char)c;
            ++count;
        }
        return count;
    }

private:
    Stream& _source;
    size_t  _bytesRead = 0;
};

// ---------------------------------------------------------------------------
static std::string makeFeed(int aircraft) {
    std::string s = "{\"ac\":[";
    char buf[512];
    for (int i = 0; i < aircraft; ++i) {
        snprintf(buf, sizeof(buf),
                 "%s{\"hex\":\"40%04x\",\"flight\":\"BAW%03d  \",\"t\":\"A32%d\","
                 "\"lat\":%.4f,\"lon\":%.4f,\"alt_baro\":%d,\"track\":%.1f,"
                 "\"gs\":%.1f,\"baro_rate\":%d,\"squawk\":\"%04d\",\"seen_pos\":0.%d,"
                 "\"dbFlags\":%d,\"ignored_field\":\"padding padding padding\"}",
                 i ? "," : "", i, i % 1000, i % 10,
                 38.84 + i * 0.001, 0.10 + i * 0.001, 1000 + i * 100,
                 (float)((i * 7) % 360), 250.0f + i, (i % 2) ? 640 : -640,
                 (1200 + i) % 7777, i % 10, (i % 5 == 0) ? 1 : 0);
        s += buf;
    }
    s += "],\"total\":";
    snprintf(buf, sizeof(buf), "%d}", aircraft);
    s += buf;
    return s;
}

static void buildFilter(JsonDocument& filter) {
    const char* keys[] = { "ac", "aircraft" };
    const char* flds[] = { "hex", "flight", "t", "lat", "lon", "alt_baro",
                           "track", "true_heading", "gs", "baro_rate",
                           "squawk", "seen_pos", "dbFlags" };
    for (const char* k : keys)
        for (const char* f : flds)
            filter[k][0][f] = true;
}

struct Result { DeserializationError err; size_t parsed; size_t read; size_t misses; };

template <typename MakeView>
static Result run(const std::string& body, size_t stallEvery, unsigned long stallMs,
                  unsigned long timeoutMs, MakeView makeView) {
    g_virtualMs = 0;
    StallingSocket sock(body, stallEvery, stallMs);
    auto view = makeView(sock);
    view.setTimeout(timeoutMs);

    JsonDocument filter; buildFilter(filter);
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, view, DeserializationOption::Filter(filter));
    const size_t parsed = err ? 0 : doc["ac"].as<JsonArrayConst>().size();
    return { err, parsed, view.bytesRead(), sock.transientMisses() };
}

static int failures = 0;
static void check(bool cond, const char* what) {
    printf("  %s  %s\n", cond ? "[PASS]" : "[FAIL]", what);
    if (!cond) ++failures;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);   // unbuffered: keep output if a case ever hangs
    const int AIRCRAFT = 120;
    const std::string body = makeFeed(AIRCRAFT);
    printf("Feed: %d aircraft, %zu bytes\n\n", AIRCRAFT, body.size());

    // 1. Reproduce the original bug: a consumer that gives up on a transient negative read.
    printf("1. Naive readBytes() (models NetworkClient) — 200ms stalls every 512 bytes\n");
    Result naive = run(body, 512, 200, 15000,
                       [](Stream& s) { return NaiveClientView(s); });
    printf("     error=%-16s parsed=%zu/%d read=%zu of %zu bytes\n",
           naive.err.c_str(), naive.parsed, AIRCRAFT, naive.read, body.size());
    check(naive.err == DeserializationError::IncompleteInput,
          "reproduces IncompleteInput (this is the bug being fixed)");

    // 2. Same stream through ReliableJsonStream with a matched timeout.
    printf("\n2. ReliableJsonStream, setTimeout(15000) — same 200ms stalls\n");
    Result fixed = run(body, 512, 200, 15000,
                       [](Stream& s) { return ReliableJsonStream(s); });
    printf("     error=%-16s parsed=%zu/%d read=%zu of %zu bytes, %zu transient misses absorbed\n",
           fixed.err.c_str(), fixed.parsed, AIRCRAFT, fixed.read, body.size(), fixed.misses);
    check(!fixed.err, "parses cleanly through the stalls");
    check(fixed.parsed == (size_t)AIRCRAFT, "recovers every aircraft");
    check(fixed.misses > 0, "the stalls really did fire (test is not vacuous)");

    // 3. The default Arduino Stream timeout is 1000ms. A longer stall must break it —
    //    this is why fetchFrom() must call setTimeout() on the wrapper (it now does).
    printf("\n3. ReliableJsonStream at Arduino's DEFAULT 1000ms timeout — one 2500ms stall\n");
    Result deflt = run(body, 4096, 2500, 1000,
                       [](Stream& s) { return ReliableJsonStream(s); });
    printf("     error=%-16s parsed=%zu/%d read=%zu of %zu bytes\n",
           deflt.err.c_str(), deflt.parsed, AIRCRAFT, deflt.read, body.size());
    check(deflt.err == DeserializationError::IncompleteInput,
          "default 1000ms timeout FAILS on a 2.5s stall (why setTimeout is required)");

    // 4. Same 2500ms stall, timeout raised to the client's 15s budget.
    printf("\n4. ReliableJsonStream, setTimeout(15000) — same 2500ms stall\n");
    Result raised = run(body, 4096, 2500, 15000,
                        [](Stream& s) { return ReliableJsonStream(s); });
    printf("     error=%-16s parsed=%zu/%d read=%zu of %zu bytes\n",
           raised.err.c_str(), raised.parsed, AIRCRAFT, raised.read, body.size());
    check(!raised.err, "survives the 2.5s stall once the timeout matches the client");
    check(raised.parsed == (size_t)AIRCRAFT, "recovers every aircraft");

    // 5. A genuinely dead peer must still terminate rather than hang forever.
    printf("\n5. Truncated body (peer died mid-document), setTimeout(15000)\n");
    std::string truncated = body.substr(0, body.size() / 2);
    Result dead = run(truncated, 0, 0, 15000,
                      [](Stream& s) { return ReliableJsonStream(s); });
    printf("     error=%-16s read=%zu of %zu bytes\n", dead.err.c_str(), dead.read, truncated.size());
    check(dead.err == DeserializationError::IncompleteInput, "fails fast, does not hang");

    printf("\n%s (%d failing checks)\n", failures ? "RESULT: FAILURES" : "RESULT: ALL CHECKS PASSED", failures);
    return failures ? 1 : 0;
}
