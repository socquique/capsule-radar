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
#include "config.h"          // board header: I/O-expander address + GPS reset bit (1.75)
#include <Arduino.h>
#include <Wire.h>
#include <TinyGPS++.h>

#define GPS_ADDR_W      0x50
#define GPS_ADDR_R      0x54
#define GPS_ADDR_DW     0x58       // data-write endpoint: command text goes here (see gps_write)
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
// Stuck-module recovery. A wedged LC76G (queue overflow, #24) streams nothing until it is
// restarted, and an OTA/soft reboot never power-cycles it (its VCC is the board's main 3V3
// rail). On the 1.75 its RESET pin is on the TCA9554 expander, so we can restart it ourselves:
// once at every boot, and again whenever no NMEA has arrived for GPS_STALL_MS — which also
// covers "Use GPS" being switched back on after the module sat undrained and overflowed.
#define GPS_STALL_MS        30000  // no NMEA bytes for this long -> hardware reset
#define GPS_RESET_GAP_MS    60000  // at most one runtime reset a minute (a dead module can't loop us)
#define GPS_RESET_PULSE_MS  100    // RESET held low
#define GPS_BOOT_WAIT_MS    1500   // boot: wait up to this for the module to ACK again after reset
#define GPS_SETTLE_AFTER_MS 1000   // runtime: skip drains this long after a reset (module booting)
#define GPS_CFG_RETRY_MS    10000  // re-send the sentence config if not all acked by then
#define GPS_CFG_TRIES       3      // per reset. Keep writes rare: reportedly ~88 cumulative command
                                   //   writes can corrupt the module's parser (landracer/opendash)
#define GPS_CFG_MAX_PER_BOOT 10    // hard cap across ALL resets: a module that keeps re-wedging gets
                                   //   one reset a minute and one config write each, which would
                                   //   otherwise grind toward that limit over a long session
#define GPS_CFG_N           4      // commands in GPS_CFG_BODIES

// TinyGPS++ only reads GGA + RMC. GSV (per-satellite detail, several sentences per
// constellation every second), GSA, GLL and VTG are dropped by the parser anyway, yet they are
// most of the I2C volume — the bursts that overflow the module's queue (#24) and stretch each
// blocking drain. Turn them off (Quectel $PAIR062,<type>,0: 1 GLL, 2 GSA, 3 GSV, 5 VTG). Held in
// the module's backup RAM, so re-sent after every reset rather than saved to flash.
static const char *const GPS_CFG_BODIES[GPS_CFG_N] = {
    "PAIR062,1,0", "PAIR062,2,0", "PAIR062,3,0", "PAIR062,5,0",
};

static const uint8_t QLEN[8] = { 0x08, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00 };  // "how much is queued?"
static const uint8_t QFREE[8] = { 0x04, 0x00, 0x51, 0xAA, 0x04, 0x00, 0x00, 0x00 }; // "how much room to write?"

static bool        s_present = false;
static TinyGPSPlus s_gps;
static uint8_t     s_deadDrains = 0;   // consecutive drains where the module never answered
static uint32_t    s_lastDataMs = 0;   // last drain that returned NMEA bytes
static uint32_t    s_lastResetMs = 0;
static uint32_t    s_settleUntil = 0;  // no drains before this (module booting after a reset)
static uint16_t    s_resets = 0;       // hardware resets since boot (diagnostics)
static bool        s_needCfg = false;  // sentence config still to be sent/acked since the last reset
static uint8_t     s_cfgTries = 0;
static uint8_t     s_cfgWritesBoot = 0; // sentence-config writes since boot (see GPS_CFG_MAX_PER_BOOT)
static uint32_t    s_cfgSentMs = 0;
static uint8_t     s_cfgAcks = 0;      // "$PAIR001,062,0" (success) replies seen since the last send
static uint8_t     s_ackPos = 0;       // matcher state for those replies in the NMEA stream

// ---------- hardware reset via the TCA9554 (1.75 board) ----------
#ifdef I2C_ADDR_EXPANDER
static bool exp_read(uint8_t reg, uint8_t *v) {
    Wire.beginTransmission(I2C_ADDR_EXPANDER); Wire.write(reg);
    if (Wire.endTransmission() != 0) return false;
    if (Wire.requestFrom((uint8_t)I2C_ADDR_EXPANDER, (uint8_t)1) != 1) return false;
    *v = Wire.read();
    return true;
}
static bool exp_write(uint8_t reg, uint8_t v) {
    Wire.beginTransmission(I2C_ADDR_EXPANDER); Wire.write(reg); Wire.write(v);
    return Wire.endTransmission() == 0;
}
// Pulse the LC76G's RESET low. Read-modify-write so only P7 changes: the other EXIO pins are
// interrupt/status inputs. Released by returning P7 to an input (its power-on state; the
// module's own pull-up then holds RESET high). Returns false when there is no expander.
static bool gps_hw_reset() {
    const uint8_t bit = (uint8_t)(1u << EXPANDER_GPS_RST_BIT);
    uint8_t out, cfg;
    if (!exp_read(0x01, &out) || !exp_read(0x03, &cfg)) return false;   // 0x01 output, 0x03 config (1 = input)
    if (!exp_write(0x01, out & ~bit)) return false;   // latch LOW first so the switch to output can't glitch HIGH
    if (!exp_write(0x03, cfg & ~bit)) return false;   // P7 output -> RESET asserted
    // Self-check via the input register (0x00), which reports the real pin level even for an
    // output: LOW while asserted proves the writes reached P7 and it drives the line; HIGH after
    // release proves the module's pull-up took it back. Logged every reset.
    uint8_t in = 0xFF;
    const bool lowOk = exp_read(0x00, &in) && !(in & bit);
    delay(GPS_RESET_PULSE_MS);
    bool released = false;                            // never leave RESET asserted: that would kill GPS
    for (int t = 0; t < 3 && !released; ++t) released = exp_write(0x03, cfg | bit);
    delay(2);
    in = 0x00;
    const bool highOk = released && exp_read(0x00, &in) && (in & bit);
    Serial.printf("[gps] reset pulse: P7 low=%s released=%s\n", lowOk ? "ok" : "FAIL", highOk ? "ok" : "FAIL");
    return released;
}
#else
static bool gps_hw_reset() { return false; }   // no reset line on this board
#endif

// After a reset: fresh stall timer, forget the dead-drain backoff, and re-send the sentence config.
static void gps_after_reset(uint32_t now) {
    ++s_resets;
    s_lastResetMs = now;
    s_lastDataMs  = now;
    s_deadDrains  = 0;
    s_needCfg     = true;
    s_cfgTries    = 0;
    s_cfgAcks     = 0;
}

// Spot "$PAIR001,062,0" replies (command 062 accepted) in the byte stream.
static void gps_scan_ack(char c) {
    static const char PFX[] = "$PAIR001,062,";
    if (s_ackPos == sizeof(PFX) - 1) {
        if (c == '0' && s_cfgAcks < 255) ++s_cfgAcks;
        s_ackPos = 0;
    } else if (c == PFX[s_ackPos]) {
        ++s_ackPos;
    } else {
        s_ackPos = (c == '$') ? 1 : 0;
    }
}

// Send raw command text to the module (Quectel I2C write, same settle rules as the read path):
// ask for free RX space at 0x50/0x54, announce the length at 0x50, then write the bytes to 0x58.
static bool gps_write(const uint8_t *data, uint32_t len) {
    Wire.beginTransmission(GPS_ADDR_W); Wire.write(QFREE, sizeof(QFREE));
    if (Wire.endTransmission() != 0) return false;
    delay(GPS_SETTLE_MS);
    if (Wire.requestFrom((uint8_t)GPS_ADDR_R, (uint8_t)4) != 4) return false;
    uint32_t room = (uint32_t)Wire.read();
    room |= (uint32_t)Wire.read() << 8;
    room |= (uint32_t)Wire.read() << 16;
    room |= (uint32_t)Wire.read() << 24;
    if (room != 0 && room < len) {                   // 0 has been seen from modules that still accept the write
        Serial.printf("[gps] cmd: only %lu bytes free, need %lu\n", (unsigned long)room, (unsigned long)len);
        return false;
    }
    const uint8_t cw[8] = { 0x00, 0x10, 0x53, 0xAA,
                            (uint8_t)len, (uint8_t)(len >> 8), (uint8_t)(len >> 16), (uint8_t)(len >> 24) };
    delay(GPS_SETTLE_MS);
    Wire.beginTransmission(GPS_ADDR_W); Wire.write(cw, sizeof(cw));
    if (Wire.endTransmission() != 0) return false;
    delay(GPS_SETTLE_MS);
    Wire.beginTransmission(GPS_ADDR_DW); Wire.write(data, len);
    return Wire.endTransmission() == 0;
}

// All config commands in ONE write (the module parses its RX stream like a UART), so each
// attempt costs a single command-write sequence.
static void gps_send_config(uint32_t now) {
    char buf[128];
    size_t n = 0;
    for (int i = 0; i < GPS_CFG_N; ++i) {
        uint8_t cs = 0;
        for (const char *p = GPS_CFG_BODIES[i]; *p; ++p) cs ^= (uint8_t)*p;
        n += snprintf(buf + n, sizeof(buf) - n, "$%s*%02X\r\n", GPS_CFG_BODIES[i], cs);
    }
    ++s_cfgTries;
    s_cfgSentMs = now;
    s_cfgAcks = 0;
    const bool ok = gps_write((const uint8_t *)buf, (uint32_t)n);
    Serial.printf("[gps] sentence config (GGA+RMC only) sent: %s, try %u/%u\n",
                  ok ? "ok" : "FAILED", (unsigned)s_cfgTries, (unsigned)GPS_CFG_TRIES);
}

// Un-stick the module: other shared-bus traffic (LVGL polls the touch IC continuously
// between our calls) can leave the LC76G mid-response and unable to answer at 0x50; a
// stray 1-byte read at 0x54/0x58 kicks it back to normal (Quectel quirk). 1-byte reads,
// no extra RAM. Costs up to two I2C timeouts against a dead module, so callers only run
// it when the quick path has actually failed.
static void gps_unstick() {
    Wire.requestFrom((uint8_t)GPS_ADDR_R, (uint8_t)1); while (Wire.available()) Wire.read();
    Wire.requestFrom((uint8_t)0x58,        (uint8_t)1); while (Wire.available()) Wire.read();
}

// Waits inside the read path. The LC76G needs the I2C bus to itself for the whole drain
// (other bus traffic between steps makes it stop answering), so during these waits only
// the idle hook runs: the display's sweep compositor, which uses QSPI and never touches
// I2C. Without a hook this is a plain delay(), as before.
static void (*s_idleHook)() = nullptr;
void gps_set_idle_hook(void (*hook)()) { s_idleHook = hook; }
static void gps_settle(uint32_t ms) {
    if (!s_idleHook) { delay(ms); return; }
    const uint32_t t0 = millis();
    while (millis() - t0 < ms) { s_idleHook(); delay(1); }
}

// Ask how many bytes are queued. Retries, since prior shared-bus traffic can briefly
// leave the module unable to answer the first attempt. Returns true when the module
// answered (avail may legitimately be 0), false when it never did.
static bool gps_query_len(uint32_t *avail, int tries) {
    for (int t = 0; t < tries; ++t) {
        Wire.beginTransmission(GPS_ADDR_W); Wire.write(QLEN, sizeof(QLEN));
        if (Wire.endTransmission() != 0) { delay(15); continue; }
        gps_settle(GPS_SETTLE_MS);
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
    gps_settle(GPS_SETTLE_MS);                         // <-- without this gap the cmd write gets NACKed
    Wire.beginTransmission(GPS_ADDR_W); Wire.write(cmd, sizeof(cmd));
    if (Wire.endTransmission() != 0) return 0;

    // 3) read straight into the NMEA parser (no Wire.setBufferSize here — see file header)
    gps_settle(GPS_SETTLE_MS);
    const int got = Wire.requestFrom((uint8_t)GPS_ADDR_R, (size_t)want);
    for (int i = 0; i < got; ++i) {
        const char c = (char)Wire.read();
        gps_scan_ack(c);
        s_gps.encode(c);
    }
    return got;
}

static bool gps_ack() {   // does anything answer at 0x50? (only the -G variant's LC76G does)
    Wire.beginTransmission(GPS_ADDR_W);
    Wire.write(QLEN, sizeof(QLEN));
    return Wire.endTransmission() == 0;
}

bool gps_begin() {
    // probe with the default buffer first; only bump it on a real -G board (and only once,
    // here at boot before TLS comes up) so non-GPS boards keep their small internal footprint.
    s_present = gps_ack();
    // Restart the module on every boot: one wedged before an OTA/soft reboot may still ACK but
    // never stream again, or stop ACKing entirely. Then wait for it to come back up. Boards
    // without the expander skip this; a 1.75 without GPS pays GPS_BOOT_WAIT_MS once.
    const uint32_t t0 = millis();
    if (gps_hw_reset()) {
        do { delay(50); s_present = gps_ack(); } while (!s_present && millis() - t0 < GPS_BOOT_WAIT_MS);
        if (s_present) {
            gps_after_reset(millis());
            s_settleUntil = millis() + GPS_SETTLE_AFTER_MS;
            Serial.printf("[gps] reset via expander; module answered after %lu ms\n",
                          (unsigned long)(millis() - t0));
        }
    }
    if (s_present) Wire.setBufferSize(GPS_BUF);       // sized ONCE; never resized at runtime
    s_lastDataMs = millis();                          // stall timer starts now
    Serial.printf("[gps] LC76G %s\n", s_present ? "detected on I2C" : "not present");
    return s_present;
}

bool gps_present() { return s_present; }

void gps_poll() {
    if (!s_present) return;
    static uint32_t last = 0;
    const uint32_t now = millis();
    // Same cadence with or without a fix: the module never stops emitting NMEA, so WE must
    // never stop draining it, or its queue overflows and it wedges until power-cycled (#24).
    // Exception: a module that is not answering AT ALL has nothing to overflow — hammering
    // it just blocks this core (and the config web page) on I2C timeouts, so back off.
    const uint32_t interval = (s_deadDrains >= GPS_DEAD_TO_BACKOFF) ? GPS_POLL_DEAD_MS : GPS_POLL_MS;
    if (last != 0 && now - last < interval) return;
    if ((int32_t)(now - s_settleUntil) < 0) return;              // still booting after a reset
    last = now;

    // Stuck module: nothing for GPS_STALL_MS (answering or not) -> restart it via its RESET line.
    if (now - s_lastDataMs > GPS_STALL_MS && now - s_lastResetMs > GPS_RESET_GAP_MS) {
        Serial.printf("[gps] no NMEA for %lu s: resetting the module\n",
                      (unsigned long)((now - s_lastDataMs) / 1000));
        if (gps_hw_reset()) {
            gps_after_reset(now);
            s_settleUntil = now + GPS_RESET_PULSE_MS + GPS_SETTLE_AFTER_MS;
            return;
        }
        s_lastResetMs = now;   // no reset line: don't retry (or log) on every poll
    }

    Wire.setClock(GPS_I2C_HZ); Wire.setTimeOut(GPS_TIMEOUT_MS);   // LC76G read path: 100 kHz + tolerate clock-stretch
    const int got = gps_drain(s_deadDrains > 0);
    if (got > 0) s_lastDataMs = now;
    // Trim the output once the module is streaming again (bytes arriving = it's up and parsing).
    if (s_needCfg) {
        if (s_cfgAcks >= GPS_CFG_N) {
            s_needCfg = false;
            Serial.printf("[gps] sentence config acked (%u/%u)\n", (unsigned)s_cfgAcks, (unsigned)GPS_CFG_N);
        } else if (got > 0 && (s_cfgTries == 0 || now - s_cfgSentMs > GPS_CFG_RETRY_MS)) {
            if (s_cfgWritesBoot >= GPS_CFG_MAX_PER_BOOT) {
                s_needCfg = false;     // the module still works, it just keeps its default (chattier) output
                Serial.printf("[gps] sentence config: per-boot cap of %u writes reached, not sending\n",
                              (unsigned)GPS_CFG_MAX_PER_BOOT);
            } else if (s_cfgTries < GPS_CFG_TRIES) {
                ++s_cfgWritesBoot;
                gps_send_config(now);
            } else {
                s_needCfg = false;
                Serial.printf("[gps] sentence config: only %u/%u acked, giving up\n",
                              (unsigned)s_cfgAcks, (unsigned)GPS_CFG_N);
            }
        }
    }
    Wire.setTimeOut(BUS_TIMEOUT_MS); Wire.setClock(GPS_BUS_HZ);   // hand the shared bus back to touch/IMU/RTC/PMIC
    if (got < 0) { if (s_deadDrains < 255) ++s_deadDrains; }
    else s_deadDrains = 0;

    // Diagnostic ladder (every ~8 s): chars=0 -> no NMEA arriving; sent>0 but fix=0 -> valid
    // data, just no satellite lock yet (needs clear sky / a few min on a cold start).
    static uint32_t lg = 0;
    if (now - lg > 8000) { lg = now;
        Serial.printf("[gps] chars=%lu sent=%lu csErr=%lu sats=%d fix=%d resets=%u\n",
                      (unsigned long)s_gps.charsProcessed(), (unsigned long)s_gps.sentencesWithFix(),
                      (unsigned long)s_gps.failedChecksum(), (int)s_gps.satellites.value(),
                      (int)gps_has_fix(), (unsigned)s_resets);
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

float gps_course_deg() {
    return (s_gps.course.isValid() && s_gps.course.age() < GPS_FIX_TTL_MS)
               ? (float)s_gps.course.deg() : NAN;
}

float gps_speed_kmh() {
    return (s_gps.speed.isValid() && s_gps.speed.age() < GPS_FIX_TTL_MS)
               ? (float)s_gps.speed.kmph() : NAN;
}
