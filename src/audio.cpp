// ES8311 codec "ping" generator. See audio.h for the core/bus discipline.
//
// Uses the IDF I2S driver directly (the Arduino ESP_I2S wrapper hit an IRAM-safe
// GDMA/interrupt mismatch in the precompiled libs: "Register tx callback failed").
// The ES8311 register init below is the canonical DAC-playback sequence; if the
// speaker stays silent, cross-check it against the Waveshare 08_ES8311 demo — only
// that table is board-specific, the rest is independent.
#include "audio.h"
#include "config.h"
#include <Arduino.h>
#include <Wire.h>
#include "driver/i2s.h"
#include "esp_heap_caps.h"
#include <math.h>

#define ES8311_ADDR   0x18
#define SR            16000          // playback sample rate (a beep; pitch-tolerant)
#define I2S_PORT      I2S_NUM_0

static bool s_ok = false;
static int16_t *s_buf = nullptr;     // tone scratch in PSRAM (keeps internal RAM free for TLS)
static const size_t S_BUF_LEN = SR / 2 * 2;   // up to 500 ms, stereo interleaved
static volatile int  s_vol = 60;     // 0..100
static volatile bool s_muted = false;
static volatile int  s_cue = -1;
static SemaphoreHandle_t s_sem = nullptr;

static void es_write(uint8_t reg, uint8_t val) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    Wire.write(val);
    Wire.endTransmission();
}
static uint8_t es_read(uint8_t reg) {
    Wire.beginTransmission(ES8311_ADDR);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return 0;
    if (Wire.requestFrom((int)ES8311_ADDR, 1) != 1) return 0;
    return Wire.read();
}

static void es_update(uint8_t reg, uint8_t andmask, uint8_t ormask) {
    es_write(reg, (uint8_t)((es_read(reg) & andmask) | ormask));
}

// ES8311 init replicated faithfully from the vendor esp_codec_dev driver for this
// exact board: DAC playback, I2S slave, external MCLK = 256*fs, fs = 16 kHz, 16-bit,
// internal reference (ADCL+DACR). Mirrors es8311_open + config_sample + config_fmt +
// set_bits + start + volume/unmute.
static void es8311_init() {
    // --- open() ---
    es_write(0x0D, 0xFA);                 // power up system
    es_write(0x44, 0x08);                 // (written twice: ES8311 first-write quirk)
    es_write(0x44, 0x08);
    es_write(0x01, 0x30);
    es_write(0x02, 0x00);
    es_write(0x03, 0x10);
    es_write(0x16, 0x24);
    es_write(0x04, 0x10);
    es_write(0x05, 0x00);
    es_write(0x0B, 0x00);
    es_write(0x0C, 0x00);
    es_write(0x10, 0x1F);
    es_write(0x11, 0x7F);
    es_write(0x00, 0x80);                 // reset csm/clock, slave mode
    es_write(0x00, 0x80);                 // slave: bit6=0
    es_write(0x01, 0xBF);                 // clk src = SCLK/BCLK-derived (no external MCLK needed)
    es_update(0x06, (uint8_t)~0x20, 0x00); // SCLK not inverted
    es_write(0x13, 0x10);
    es_write(0x1B, 0x0A);
    es_write(0x1C, 0x6A);
    es_write(0x44, 0x58);                 // internal reference (ADCL + DACR) -> drives DAC

    // --- config_sample(): MCLK 4.096 MHz / 16 kHz coeff {pre=1,mult=1,adc=1,dac=1,osr 0x10/0x20,lrck 0xFF,bclk 4} ---
    es_write(0x02, 0x18);                 // pre_div=1, pre_multi=8 (BCLK*8 = DIG_MCLK, use_mclk=false)
    es_write(0x05, 0x00);                 // adc_div=1, dac_div=1
    es_write(0x03, 0x10);                 // fs_mode=0, adc_osr=0x10
    es_write(0x04, 0x20);                 // dac_osr=0x20
    es_update(0x07, 0xC0, 0x00);          // lrck_h=0
    es_write(0x08, 0xFF);                 // lrck_l=0xFF
    es_update(0x06, 0xE0, 0x03);          // bclk_div=4 (preserves SCLK-invert bit)

    // --- config_fmt(NORMAL) + set_bits(16) -> SDP in/out = standard I2S, 16-bit ---
    es_write(0x09, 0x0C);
    es_write(0x0A, 0x0C);

    // --- start() (DAC, slave) ---
    es_write(0x00, 0x80);
    es_write(0x01, 0xBF);                 // keep BCLK-derived clock
    es_write(0x09, 0x0C);                 // DAC iface enabled (bit6=0)
    es_write(0x0A, 0x0C);
    es_write(0x17, 0xBF);
    es_write(0x0E, 0x02);
    es_write(0x12, 0x00);                 // enable DAC
    es_write(0x14, 0x1A);
    es_write(0x0D, 0x01);
    es_write(0x15, 0x40);
    es_write(0x37, 0x08);
    es_write(0x45, 0x00);

    // --- volume + unmute ---
    es_write(0x32, 0xBF);                 // DAC volume ~0 dB
    es_update(0x31, 0x9F, 0x00);          // unmute DAC
}

static bool i2s_setup() {
    i2s_config_t cfg = {};
    cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
    cfg.sample_rate = SR;
    cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
    cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;       // stereo
    cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
    cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    cfg.dma_buf_count = 4;
    cfg.dma_buf_len = 256;
    cfg.use_apll = false;
    cfg.tx_desc_auto_clear = true;
    cfg.fixed_mclk = 0;
    cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;
    if (i2s_driver_install(I2S_PORT, &cfg, 0, nullptr) != ESP_OK) return false;

    i2s_pin_config_t pins = {};
    pins.mck_io_num   = PIN_I2S_MCLK;
    pins.bck_io_num   = PIN_I2S_BCLK;
    pins.ws_io_num    = PIN_I2S_LRCLK;
    pins.data_out_num = PIN_I2S_DOUT;
    pins.data_in_num  = I2S_PIN_NO_CHANGE;
    if (i2s_set_pin(I2S_PORT, &pins) != ESP_OK) { i2s_driver_uninstall(I2S_PORT); return false; }
    i2s_zero_dma_buffer(I2S_PORT);
    return true;
}

// Synthesize one beep (freq Hz, ms) with a short fade in/out, into a stereo buffer.
static size_t gen_beep(int16_t *buf, size_t cap, float freq, int ms, float amp) {
    const size_t n = (size_t)((long)SR * ms / 1000);
    const size_t fade = SR / 200;                 // ~5 ms ramps (anti-click)
    size_t i = 0;
    for (; i < n && (i * 2 + 1) < cap; ++i) {
        float env = 1.0f;
        if (i < fade)            env = (float)i / fade;
        else if (i > n - fade)   env = (float)(n - i) / fade;
        const int16_t s = (int16_t)(amp * env * sinf(2.0f * (float)M_PI * freq * i / SR));
        buf[i * 2] = s; buf[i * 2 + 1] = s;       // L = R
    }
    return i * 2;                                  // samples written (stereo interleaved)
}

static void play_cue(int cue) {
    if (!s_ok || !s_buf || (s_muted && cue != 2) || s_vol <= 0) return;
    int16_t *buf = s_buf;
    const float amp = (s_vol / 100.0f) * 17000.0f;
    digitalWrite(PIN_AUDIO_PA, HIGH);              // enable speaker amp
    delay(8);                                      // let the amp power up
    size_t bw;
    if (cue == 2) {                                // self-test: ~2 s continuous tone, PA held
        size_t ns = gen_beep(buf, S_BUF_LEN, 1000.0f, 480, amp);
        for (int k = 0; k < 4; ++k) i2s_write(I2S_PORT, buf, ns * 2, &bw, portMAX_DELAY);
    } else if (cue == AUDIO_ALERT) {
        for (int k = 0; k < 2; ++k) {
            size_t ns = gen_beep(buf, S_BUF_LEN, 1320.0f, 80, amp);
            i2s_write(I2S_PORT, buf, ns * 2, &bw, portMAX_DELAY);
            delay(40);
        }
    } else {
        size_t ns = gen_beep(buf, S_BUF_LEN, 880.0f, 110, amp * 0.8f);
        i2s_write(I2S_PORT, buf, ns * 2, &bw, portMAX_DELAY);
    }
    i2s_zero_dma_buffer(I2S_PORT);
    delay(6);
    digitalWrite(PIN_AUDIO_PA, LOW);               // mute amp between pings (saves power, kills hiss)
}

static void audio_task(void *) {
    for (;;) {
        if (xSemaphoreTake(s_sem, portMAX_DELAY) == pdTRUE) play_cue(s_cue);
    }
}

bool audio_begin() {
    pinMode(PIN_AUDIO_PA, OUTPUT);
    digitalWrite(PIN_AUDIO_PA, LOW);

    const uint8_t id1 = es_read(0xFD), id2 = es_read(0xFE);   // expect 0x83, 0x11
    if (id1 != 0x83) {
        Serial.printf("[audio] ES8311 not found (id=0x%02X 0x%02X)\n", id1, id2);
        s_ok = false;
        return false;
    }
    es8311_init();

    const uint8_t dr[] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x09,0x0A,0x0D,0x0E,0x12,0x14,0x31,0x32,0x37,0x44};
    Serial.print("[audio] ES8311 regs:");
    for (uint8_t r : dr) Serial.printf(" %02X=%02X", r, es_read(r));
    Serial.println();

    if (!i2s_setup()) {
        Serial.println("[audio] I2S init failed");
        s_ok = false;
        return false;
    }
    s_buf = (int16_t *)heap_caps_malloc(S_BUF_LEN * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        Serial.println("[audio] tone buffer alloc failed");
        s_ok = false;
        return false;
    }
    s_sem = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 1, nullptr, 0);  // I2S only -> core 0
    s_ok = true;
    Serial.println("[audio] ES8311 ready");
    return true;
}

bool audio_present() { return s_ok; }
void audio_set_volume(int pct) { s_vol = constrain(pct, 0, 100); }
void audio_set_muted(bool m) { s_muted = m; }

void audio_play(AudioCue cue) {
    if (!s_ok || s_muted) return;
    s_cue = (int)cue;
    if (s_sem) xSemaphoreGive(s_sem);
}

void audio_selftest() {   // ~2 s continuous tone, ignores mute, PA held on
    if (!s_ok) return;
    s_cue = 2;
    if (s_sem) xSemaphoreGive(s_sem);
}
