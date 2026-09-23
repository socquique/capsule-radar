// LC76G GNSS over the shared I2C bus (Quectel I2C-NMEA protocol).
// Verified on the ESP32-S3-Touch-AMOLED-1.75-G. What this protocol needed (figured out from
// Waveshare's RP2350 `04_GPS_I2C` demo + on-hardware probing):
//   - a ~100 ms settle between EVERY step — crucially BETWEEN reading the queued length and
//     writing the read-data command, or the module NACKs that write (endTransmission == 2);
//   - the read-data command echoes the byte count you then read;
//   - the whole transaction must run uninterrupted, so it's one blocking call (nothing else
//     on core 1 runs meanwhile);
//   - 100 kHz + a longer I2C timeout, because the module clock-stretches while preparing data.
// MEMORY: the Wire RX buffer is sized ONCE at begin and never resized afterwards — calling
// Wire.setBufferSize() at runtime fragments the internal heap and kills the mbedTLS handshake
// (the ADS-B HTTPS feed). So we cap each read to one bufferful and let the rest come next poll.
#include "gps.h"
#include <Arduino.h>
#include <Wire.h>
#include <TinyGPS++.h>

#define GPS_ADDR_W      0x50
#define GPS_ADDR_R      0x54
#define GPS_SETTLE_MS   100        // settle between each protocol step (LC76G is fussy)
// Read size + cadence reworked after issue #24 (@xatarsixnine-stack, field-verified): backing
// off to 60 s polls once fixed let the module's internal NMEA queue overflow, and a wedged
// LC76G stops answering until a full power cycle. The fix is to KEEP draining after the fix
// and to drain big enough to always outrun the NMEA output (multi-GNSS GSV bursts exceed
// 1 KB/s). 4 KB every 2 s is 4x that worst case. The bigger Wire RX buffer is still sized
// exactly once, in gps_begin() at boot BEFORE TLS comes up, and only on -G boards — the
// runtime-resize heap-fragmentation gotcha in the file header still stands.
#define GPS_READ_MAX    4096       // max bytes drained per transaction (issue #24; was 720)
#define GPS_BUF         4160       // fixed Wire RX buffer (>= GPS_READ_MAX); set once, never resized
#define GPS_POLL_MS     2000       // continuous drain cadence, fix or no fix — same blocking
                                   //   hitch users already had pre-fix, now just permanent
// The drain runs on the SAME core that serves the config web page, and a module that is
// absent, antenna-less or wedged makes every drain walk its slowest path (I2C timeouts,
// retries) — several BLOCKED seconds out of every two, which reads as "the web page will
// not save while GPS is on" (field report, v1.4.3). So: after a few drains in a row where
// the module never even answers, back off hard until one works. A healthy module keeps the
// full 2 s cadence and the #24 overflow fix intact.
#define GPS_POLL_DEAD_MS      30000   // cadence while the module is not answering at all
#define GPS_DEAD_TO_BACKOFF   3       // consecutive dead drains before backing off
#define GPS_FIX_TTL_MS  70000      // how long a fix stays "valid" without fresh sentences
#define GPS_I2C_HZ      100000     // the read protocol is unreliable at the 400 kHz bus default
#define GPS_BUS_HZ      400000     // restore the shared bus to this after each GPS transaction
#define GPS_TIMEOUT_MS  1000       // the module clock-stretches while preparing data; 50 ms (default) times out
#define BUS_TIMEOUT_MS  50         // restore the snappy default for touch/IMU/etc.

static const uint8_t QLEN[8] = { 0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00 };  // "how much is queued?"

static bool        s_present = false;
static TinyGPSPlus s_gps;

// Un-stick the module: other shared-bus traffic (LVGL polls the touch IC continuously
// between our calls) can leave the LC76G mid-response and unable to answer at 0x50; a
// stray 1-byte read at 0x54/0x58 kicks it back to normal (Quectel quirk). 1-byte reads,
// no extra RAM. Costs up to two I2C timeouts against a dead module, so callers only run
// it when the quick path has actually failed.
static void gps_unstick() {
    Wire.requestFrom((uint8_t)GPS_ADDR_R, (uint8_t)1); while (Wire.available()) Wire.read();
    Wire.requestFrom((uint8_t)0x58,        (uint8_t)1); while (Wire.available()) Wire.read();
}

// Ask how many bytes are queued. Retries, since prior shared-bus traffic can briefly
// leave the module unable to answer the first attempt. Returns true when the module
// answered (avail may legitimately be 0), false when it never did.
static bool gps_query_len(uint32_t *avail, int tries) {
    for (int t = 0; t < tries; ++t) {
        Wire.beginTransmission(GPS_ADDR_W); Wire.write(QLEN, sizeof(QLEN));
        if (Wire.endTransmission() != 0) { delay(15); continue; }
        delay(GPS_SETTLE_MS);
        if (Wire.requestFrom((uint8_t)GPS_ADDR_R, (uint8_t)4) == 4) {
            uint32_t a  = (uint32_t)Wire.read();
            a |= (uint32_t)Wire.read() << 8;
            a |= (uint32_t)Wire.read() << 16;
            a |= (uint32_t)Wire.read() << 24;
            *avail = a;
            return true;
        }
        delay(15);
    }
    return false;
}

// One blocking, uninterrupted drain of up to GPS_READ_MAX bytes into the parser.
// Returns bytes read, 0 when the module answered but had nothing usable queued, or -1
// when it never answered (caller escalates toward the dead-module backoff). Caller has
// set 100 kHz + long timeout. `recovering` = the previous drain got no answer: lead with
// the un-stick reads and be more patient; the healthy path skips them and only falls
// back to them if the quick length query fails.
static int gps_drain(bool recovering) {
    uint32_t avail = 0;
    bool answered = false;
    if (recovering) {
        gps_unstick();
        answered = gps_query_len(&avail, 6);
    } else {
        answered = gps_query_len(&avail, 3);
        if (!answered) { gps_unstick(); answered = gps_query_len(&avail, 3); }
    }
    if (!answered) return -1;
    if (avail == 0 || avail > 200000) return 0;       // nothing queued / garbage length

    // 2) ask for at most one bufferful; the command echoes the count we will read.
    const uint32_t want = avail < GPS_READ_MAX ? avail : GPS_READ_MAX;
    const uint8_t cmd[8] = { 0x00, 0x20, 0x51, 0xAA,
                             (uint8_t)want, (uint8_t)(want >> 8),
                             (uint8_t)(want >> 16), (uint8_t)(want >> 24) };
    delay(GPS_SETTLE_MS);                              // <-- without this gap the cmd write gets NACKed
    Wire.beginTransmission(GPS_ADDR_W); Wire.write(cmd, sizeof(cmd));
    if (Wire.endTransmission() != 0) return 0;

    // 3) read straight into the NMEA parser (no Wire.setBufferSize here — see file header)
    delay(GPS_SETTLE_MS);
    const int got = Wire.requestFrom((uint8_t)GPS_ADDR_R, (size_t)want);
    for (int i = 0; i < got; ++i) s_gps.encode((char)Wire.read());
    return got;
}

bool gps_begin() {
    // probe with the default buffer first; only bump it on a real -G board (and only once,
    // here at boot before TLS comes up) so non-GPS boards keep their small internal footprint.
    Wire.beginTransmission(GPS_ADDR_W);
    Wire.write(QLEN, sizeof(QLEN));
    s_present = (Wire.endTransmission() == 0);        // 0x50 ACKs only on the -G variant
    if (s_present) Wire.setBufferSize(GPS_BUF);       // sized ONCE; never resized at runtime
    Serial.printf("[gps] LC76G %s\n", s_present ? "detected on I2C" : "not present");
    return s_present;
}

bool gps_present() { return s_present; }

void gps_poll() {
    if (!s_present) return;
    static uint32_t last = 0;
    static uint8_t  deadDrains = 0;   // consecutive drains where the module never answered
    const uint32_t now = millis();
    // Same cadence with or without a fix: the module never stops emitting NMEA, so WE must
    // never stop draining it, or its queue overflows and it wedges until power-cycled (#24).
    // Exception: a module that is not answering AT ALL has nothing to overflow — hammering
    // it just blocks this core (and the config web page) on I2C timeouts, so back off.
    const uint32_t interval = (deadDrains >= GPS_DEAD_TO_BACKOFF) ? GPS_POLL_DEAD_MS : GPS_POLL_MS;
    if (last != 0 && now - last < interval) return;
    last = now;

    Wire.setClock(GPS_I2C_HZ); Wire.setTimeOut(GPS_TIMEOUT_MS);   // LC76G read path: 100 kHz + tolerate clock-stretch
    const int got = gps_drain(deadDrains > 0);
    Wire.setTimeOut(BUS_TIMEOUT_MS); Wire.setClock(GPS_BUS_HZ);   // hand the shared bus back to touch/IMU/RTC/PMIC
    if (got < 0) { if (deadDrains < 255) ++deadDrains; }
    else deadDrains = 0;

    // Diagnostic ladder (every ~8 s): chars=0 -> no NMEA arriving; sent>0 but fix=0 -> valid
    // data, just no satellite lock yet (needs clear sky / a few min on a cold start).
    static uint32_t lg = 0;
    if (now - lg > 8000) { lg = now;
        Serial.printf("[gps] chars=%lu sent=%lu csErr=%lu sats=%d fix=%d\n",
                      (unsigned long)s_gps.charsProcessed(), (unsigned long)s_gps.sentencesWithFix(),
                      (unsigned long)s_gps.failedChecksum(), (int)s_gps.satellites.value(),
                      (int)gps_has_fix());
    }
}

bool gps_has_fix() {
    return s_gps.location.isValid() && s_gps.location.age() < GPS_FIX_TTL_MS;
}

bool gps_location(double *lat, double *lon) {
    if (!gps_has_fix()) return false;
    if (lat) *lat = s_gps.location.lat();
    if (lon) *lon = s_gps.location.lng();
    return true;
}

int gps_satellites() {
    return s_gps.satellites.isValid() ? (int)s_gps.satellites.value() : 0;
}

float gps_altitude_m() {
    return s_gps.altitude.isValid() ? (float)s_gps.altitude.meters() : NAN;
}
