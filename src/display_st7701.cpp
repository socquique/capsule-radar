// BOARD_LCD21 display bring-up: ST7701 480x480 IPS over the ESP32-S3 RGB parallel
// peripheral (esp_lcd_panel_rgb) + LVGL v8. Implements the same display:: API as the
// AMOLED build (display.cpp), so main.cpp / ui / radar_view are board-agnostic.
//
// The RGB pin map, timings, and the ST7701 init sequence are lifted verbatim from
// Waveshare's own Arduino demo for this board (LVGL_Arduino / Display_ST7701). The
// panel's chip-select + reset and the touch reset are on a TCA9554 I2C expander.
#include "config.h"
#if defined(BOARD_LCD21)

#include "display.h"
#include "radar_view.h"
#include "ui.h"
#include "touch_cst820.h"
#include "touch_queue.h"
#include <math.h>
#include "tca9554.h"

#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <esp_heap_caps.h>
#include "driver/spi_master.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"

// --- state -------------------------------------------------------------------
static esp_lcd_panel_handle_t s_panel = nullptr;
static spi_device_handle_t    s_spi   = nullptr;

static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_indev_drv;
static lv_color_t        *s_buf1   = nullptr;   // full-screen LVGL buffers (PSRAM)
static lv_color_t        *s_buf2   = nullptr;

// Two framebuffers. LVGL refreshes only what changed (partial refresh) at 0/90/180/270:
// those areas, and the sweep compositor's boxes, are copied straight into the framebuffer
// on screen (panel_push). At other angles LVGL renders whole frames, which are written
// (rotated) into the framebuffer NOT on screen and page-flipped (flush_cb): draw_bitmap
// only flips when it is given one of the panel's own framebuffers; any other buffer is
// copied into the one on screen (tearing). The bounce-buffer refill switches to the new
// buffer when it finishes the frame it is reading (on_bounce_frame_finish); only then is
// the old one free to draw into. A whole frame overwrites every pixel, so the buffer that
// becomes visible is always complete and the two never need to be kept in step.
static void             *s_fb[2]  = {nullptr, nullptr};
static int               s_back   = 1;          // panel starts on fbs[0]
static SemaphoreHandle_t s_flipped = nullptr;
static volatile bool     s_flipPending = false;   // set after draw_bitmap: the next switch is ours
static lv_color_t       *s_rotBuf = nullptr;   // partial refresh: one rotated area (PSRAM)
#define DRAW_BUF_DIV     4    // partial refresh renders in 1/N-screen strips, so the sweep
                              // ticks (flush_cb) between strips of a long LVGL frame
#define TICK_WORK_MS     30   // external sweep check-ins tick at most this often (sweepTick)

static bool IRAM_ATTR on_frame_switched(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t *, void *) {
    if (!s_flipPending) return false;
    s_flipPending = false;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flipped, &woken);
    return woken == pdTRUE;
}

static volatile uint32_t s_frameCount = 0;
uint32_t display_frames() { return s_frameCount; }

static uint16_t s_rot = 0; // clockwise degrees
static float s_rotCos = 1.0f, s_rotSin = 0.0f;
static TouchQueue s_touchQueue;
static bool s_ready = false;                    // LVGL up? (guards loop/rotation even if the panel failed)

// --- ST7701 3-wire SPI init (bit-banged DC via cmd/addr; CS on TCA9554 EXIO3) --
static void st7701_cmd(uint8_t cmd) {
    spi_transaction_t t = {};
    t.cmd = 0;          // DC = 0 -> command
    t.addr = cmd;
    spi_device_transmit(s_spi, &t);
}
static void st7701_dat(uint8_t data) {
    spi_transaction_t t = {};
    t.cmd = 1;          // DC = 1 -> data
    t.addr = data;
    spi_device_transmit(s_spi, &t);
}

static void st7701_init_sequence() {
    // Vendor init table (Waveshare Display_ST7701.cpp), unchanged.
    st7701_cmd(0xFF); st7701_dat(0x77); st7701_dat(0x01); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x10);
    st7701_cmd(0xC0); st7701_dat(0x3B); st7701_dat(0x00);
    st7701_cmd(0xC1); st7701_dat(0x0B); st7701_dat(0x02);
    st7701_cmd(0xC2); st7701_dat(0x07); st7701_dat(0x02);
    st7701_cmd(0xCC); st7701_dat(0x10);
    st7701_cmd(0xCD); st7701_dat(0x08);
    st7701_cmd(0xB0); st7701_dat(0x00); st7701_dat(0x11); st7701_dat(0x16); st7701_dat(0x0e); st7701_dat(0x11);
                      st7701_dat(0x06); st7701_dat(0x05); st7701_dat(0x09); st7701_dat(0x08); st7701_dat(0x21);
                      st7701_dat(0x06); st7701_dat(0x13); st7701_dat(0x10); st7701_dat(0x29); st7701_dat(0x31); st7701_dat(0x18);
    st7701_cmd(0xB1); st7701_dat(0x00); st7701_dat(0x11); st7701_dat(0x16); st7701_dat(0x0e); st7701_dat(0x11);
                      st7701_dat(0x07); st7701_dat(0x05); st7701_dat(0x09); st7701_dat(0x09); st7701_dat(0x21);
                      st7701_dat(0x05); st7701_dat(0x13); st7701_dat(0x11); st7701_dat(0x2a); st7701_dat(0x31); st7701_dat(0x18);
    st7701_cmd(0xFF); st7701_dat(0x77); st7701_dat(0x01); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x11);
    st7701_cmd(0xB0); st7701_dat(0x6d);
    st7701_cmd(0xB1); st7701_dat(0x37);
    st7701_cmd(0xB2); st7701_dat(0x81);
    st7701_cmd(0xB3); st7701_dat(0x80);
    st7701_cmd(0xB5); st7701_dat(0x43);
    st7701_cmd(0xB7); st7701_dat(0x85);
    st7701_cmd(0xB8); st7701_dat(0x20);
    st7701_cmd(0xC1); st7701_dat(0x78);
    st7701_cmd(0xC2); st7701_dat(0x78);
    st7701_cmd(0xD0); st7701_dat(0x88);
    st7701_cmd(0xE0); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x02);
    st7701_cmd(0xE1); st7701_dat(0x03); st7701_dat(0xA0); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x04);
                      st7701_dat(0xA0); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x20); st7701_dat(0x20);
    st7701_cmd(0xE2); for (int i = 0; i < 13; ++i) st7701_dat(0x00);
    st7701_cmd(0xE3); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x11); st7701_dat(0x00);
    st7701_cmd(0xE4); st7701_dat(0x22); st7701_dat(0x00);
    st7701_cmd(0xE5); st7701_dat(0x05); st7701_dat(0xEC); st7701_dat(0xA0); st7701_dat(0xA0); st7701_dat(0x07);
                      st7701_dat(0xEE); st7701_dat(0xA0); st7701_dat(0xA0); st7701_dat(0x00); st7701_dat(0x00);
                      st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x00);
    st7701_cmd(0xE6); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x11); st7701_dat(0x00);
    st7701_cmd(0xE7); st7701_dat(0x22); st7701_dat(0x00);
    st7701_cmd(0xE8); st7701_dat(0x06); st7701_dat(0xED); st7701_dat(0xA0); st7701_dat(0xA0); st7701_dat(0x08);
                      st7701_dat(0xEF); st7701_dat(0xA0); st7701_dat(0xA0); st7701_dat(0x00); st7701_dat(0x00);
                      st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x00);
    st7701_cmd(0xEB); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x40); st7701_dat(0x40); st7701_dat(0x00);
                      st7701_dat(0x00); st7701_dat(0x00);
    st7701_cmd(0xED); st7701_dat(0xFF); st7701_dat(0xFF); st7701_dat(0xFF); st7701_dat(0xBA); st7701_dat(0x0A);
                      st7701_dat(0xBF); st7701_dat(0x45); st7701_dat(0xFF); st7701_dat(0xFF); st7701_dat(0x54);
                      st7701_dat(0xFB); st7701_dat(0xA0); st7701_dat(0xAB); st7701_dat(0xFF); st7701_dat(0xFF); st7701_dat(0xFF);
    st7701_cmd(0xEF); st7701_dat(0x10); st7701_dat(0x0D); st7701_dat(0x04); st7701_dat(0x08); st7701_dat(0x3F); st7701_dat(0x1F);
    st7701_cmd(0xFF); st7701_dat(0x77); st7701_dat(0x01); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x13);
    st7701_cmd(0xEF); st7701_dat(0x08);
    st7701_cmd(0xFF); st7701_dat(0x77); st7701_dat(0x01); st7701_dat(0x00); st7701_dat(0x00); st7701_dat(0x00);
    st7701_cmd(0x36); st7701_dat(0x00);
    st7701_cmd(0x3A); st7701_dat(0x66);
    st7701_cmd(0x11);                          // sleep out
    delay(480);
    st7701_cmd(0x20);                          // display inversion off
    delay(120);
    st7701_cmd(0x29);                          // display on
}

// Push the ST7701 init over 3-wire SPI (cmd=1 DC-bit, addr=8 payload bits), CS via EXIO3.
static void st7701_begin() {
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = PIN_LCD_MOSI;
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = PIN_LCD_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 64;
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t devcfg = {};
    devcfg.command_bits = 1;                   // DC bit
    devcfg.address_bits = 8;                   // payload
    devcfg.mode = 0;
    devcfg.clock_speed_hz = 40 * 1000 * 1000;
    devcfg.spics_io_num = -1;                  // CS is toggled via the TCA9554 expander
    devcfg.queue_size = 1;
    spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi);

    tca9554_set(EXIO_LCD_CS, 0);               // assert CS low for the whole init
    delay(10);
    st7701_init_sequence();
    tca9554_set(EXIO_LCD_CS, 1);               // release CS (RGB refresh is DE-driven, no CS)
    delay(10);
}

static bool rgb_panel_begin() {
    esp_lcd_rgb_panel_config_t cfg = {};
    cfg.clk_src = LCD_CLK_SRC_DEFAULT;
    cfg.timings.pclk_hz = RGB_PCLK_HZ;
    cfg.timings.h_res = SCREEN_W;
    cfg.timings.v_res = SCREEN_H;
    cfg.timings.hsync_pulse_width = RGB_HPW;
    cfg.timings.hsync_back_porch  = RGB_HBP;
    cfg.timings.hsync_front_porch = RGB_HFP;
    cfg.timings.vsync_pulse_width = RGB_VPW;
    cfg.timings.vsync_back_porch  = RGB_VBP;
    cfg.timings.vsync_front_porch = RGB_VFP;
    cfg.timings.flags.pclk_active_neg = false;
    cfg.data_width = 16;
    cfg.bits_per_pixel = 16;
    cfg.num_fbs = 2;                        // 2 framebuffers = double-buffered (tear-free page flip).
    // Bounce buffers: 2 x 10 lines in internal RAM (2 x 9.6 KB). The DMA streams from them
    // and an ISR refills each from the PSRAM framebuffer as it empties (~0.46 ms per 10 lines
    // at 12 MHz). Streaming straight from PSRAM, the DMA fell behind whenever the CPU or WiFi
    // was busy in PSRAM and the picture shifted sideways (#49), more with more aircraft.
    // The refill reads PSRAM, which is unreachable while a WiFi/NVS flash op has the cache
    // off ("Cache disabled..." with A4=0x2580=9600=this buffer), so the RGB/GDMA ISRs are
    // NOT IRAM-safe (platformio.ini): they are held off during the flash op instead.
    cfg.bounce_buffer_size_px = 10 * SCREEN_W;
    cfg.hsync_gpio_num = RGB_HSYNC;
    cfg.vsync_gpio_num = RGB_VSYNC;
    cfg.de_gpio_num    = RGB_DE;
    cfg.pclk_gpio_num  = RGB_PCLK;
    cfg.disp_gpio_num  = -1;
    for (int i = 0; i < 16; ++i) cfg.data_gpio_nums[i] = RGB_DATA_PINS[i];
    cfg.flags.fb_in_psram = true;
    // NB: do NOT also set flags.double_fb here — with num_fbs already 2, newer ESP-IDF
    // rejects the combination (ESP_ERR_INVALID_ARG), which left s_panel null -> crash loop.

    // Don't ESP_ERROR_CHECK (that abort()s -> silent reboot loop). Log and report instead.
    esp_err_t err;
    if ((err = esp_lcd_new_rgb_panel(&cfg, &s_panel)) != ESP_OK) {
        Serial.printf("[display] esp_lcd_new_rgb_panel: %s\n", esp_err_to_name(err)); return false;
    }
    if ((err = esp_lcd_panel_reset(s_panel)) != ESP_OK) {
        Serial.printf("[display] rgb panel reset: %s\n", esp_err_to_name(err)); return false;
    }
    if ((err = esp_lcd_panel_init(s_panel)) != ESP_OK) {
        Serial.printf("[display] rgb panel init: %s\n", esp_err_to_name(err)); return false;
    }
    if ((err = esp_lcd_rgb_panel_get_frame_buffer(s_panel, 2, &s_fb[0], &s_fb[1])) != ESP_OK) {
        Serial.printf("[display] rgb frame buffers: %s\n", esp_err_to_name(err)); return false;
    }
    s_flipped = xSemaphoreCreateBinary();
    esp_lcd_rgb_panel_event_callbacks_t cbs = {};
    cbs.on_bounce_frame_finish = on_frame_switched;
    if (!s_flipped || (err = esp_lcd_rgb_panel_register_event_callbacks(s_panel, &cbs, nullptr)) != ESP_OK) {
        Serial.printf("[display] rgb flip callback: %s\n", s_flipped ? esp_err_to_name(err) : "no semaphore");
        return false;
    }
    Serial.println("[display] RGB panel initialised");
    return true;
}

// Partial updates go straight into the framebuffer on screen. A small box (a moved glyph,
// the sweep wedge) written while the panel scans it shows half-updated for at most one
// panel frame, which is not visible; the AMOLED panel, too, shows its memory while it is
// written. Whole frames are page-flipped instead (flush_cb).
static void panel_push(int16_t x, int16_t y, uint16_t *pixels, uint16_t w, uint16_t h) {
    if (!s_fb[0]) return;
    uint16_t *fb = (uint16_t *)s_fb[s_back ^ 1];
    for (int row = 0; row < h; ++row)
        memcpy(fb + (size_t)(y + row) * SCREEN_W + x, pixels + (size_t)row * w, (size_t)w * 2);
}

// --- sweep compositor ----------------------------------------------------------------
// The AMOLED compositor (display.cpp, from the "sweep compositor" block through
// compose_step; idea and blend from ijord's 2.8C fork, upstream issue #14), unchanged
// except for its output: a "push" here is a copy into the framebuffer on screen (see
// panel_push above) instead of a QSPI window write.
// LVGL renders the scope WITHOUT the sweep. Every block pushed to the panel is also kept
// in s_phys (physical orientation, so any rotation works). Each sweep tick re-pushes only
// the box under the old + new wedge: s_phys plus a per-pixel angular fade. LVGL output
// that lands under the wedge between ticks is blended the same way before it is pushed.
// The idle clock's moving parts (radar_view's ClockFrame) are drawn here too: the lead
// line becomes the second hand, and the hour/minute hands and the centre cap are line
// segments with boxes of their own, so they move at the sweep's rate, not LVGL's.
// Pixels are LVGL's RGB565 (LV_COLOR_16_SWAP 0); a compile-time check is below.
#define ANG_UNITS      32768                     // 360 degrees in bearing-map units
#define ANG_OUTSIDE    0xFFFF                    // bearing-map value outside the scope disc
#define TRAIL_U        ((int)(SWEEP_TRAIL_DEG / 360.0f * ANG_UNITS))
#define FADE_SHIFT     3
#define FADE_N         ((TRAIL_U >> FADE_SHIFT) + 2)
#define COMP_ROWS      32                        // rows per push from the scratch buffer
#define SWEEP_MASKS    5                         // ui_sweep_masks(): card, photo, credit, zoom button, hold hint
static_assert(LV_COLOR_16_SWAP == 0, "compositor assumes native-order RGB565");

struct Rect { int x1, y1, x2, y2; bool valid; };
static uint16_t *s_phys    = nullptr;   // last pushed LVGL content, physical orientation (PSRAM)
static uint16_t *s_angMap  = nullptr;   // per-pixel physical bearing, 0 = up, clockwise (PSRAM)
static uint16_t *s_compBuf = nullptr;   // push scratch, SCREEN_W x COMP_ROWS (PSRAM)
static uint8_t   s_fadeLut[FADE_N];
static uint16_t  s_swRing = 0x07E0, s_swLead = 0xFFFF;
static bool      s_swWanted  = false;   // radar_view: sweep on and the theme has one
static bool      s_swVisible = false;   // UI: radar tile shown, not mid-swipe
static bool      s_sweepWas = false, s_pulseWas = false;   // wanted states at the last tick
static int       s_angU      = 0;       // sweep line angle on the panel (bearing-map units)
static float     s_pcx = 0, s_pcy = 0;  // scope centre, physical pixels
// A line from the scope centre along (ux, uy), from t0 to t1 px (t0 < 0: a tail past the
// centre), hw px half-width, round ends. The sweep's lead line, the clock's hands and its
// cap (a zero-length segment: a disc) are all drawn as one of these.
struct Seg { float ux, uy, t0, t1, hw; uint16_t col; uint32_t opa; Rect box; };
static bool      s_trailOn   = false;   // the sweep trail is on the panel (s_trailBox, at s_angU)
static Rect      s_trailBox  = {};
static uint32_t  s_trailMul  = 256;     // trail opacity, /256: dimmer behind the clock's second hand
static bool      s_leadOn    = false;   // the lead line (the clock's second hand) is on the panel
static Seg       s_lead      = {};
static Rect      s_wedge     = {};      // trail + lead boxes: what each sweep tick re-pushes
// Idle clock parts below and above the second hand: two segments each (outline + hand,
// cap + dot), with a box of their own so a part is re-pushed only when it changes.
enum { PART_HOUR, PART_MIN, PART_CAP, PARTS };
struct Part { bool on; Seg seg[2]; Rect box; float deg, size; };   // deg/size: change detection
static Part      s_part[PARTS] = {};
// Objects the sweep must not paint over: the logical rounded rectangle (exact shape) and
// its physical bounding box (cheap first test).
struct SweepMask { Rect box; float x1, y1, x2, y2, r; };
static SweepMask s_mask[SWEEP_MASKS];
static int       s_maskN     = 0;
static uint32_t  s_lastComposeMs = 0;
static uint32_t  s_swPhase = 0;         // sweep angle in ANG_UNITS << 8 (sub-unit precision)
#define SWEEP_MAX_STEP_MS (2 * SWEEP_FRAME_MS)   // after a stall, resume instead of jumping ahead
// Centre ripple (the expanding ring at home), drawn here too so it moves at the sweep's rate.
static bool      s_pulseWanted = false; // radar_view: the theme has one
static bool      s_pulseOn   = false;   // a ring is currently on the panel
static uint16_t  s_pulseInk  = 0xFFFF;
static uint32_t  s_pulseMs   = 0;       // position in the expansion, 0..PULSE_PERIOD_MS
static float     s_pulseR    = 0;       // outer radius of the ring on the panel
static uint32_t  s_pulseOpa  = 0;
static Rect      s_pulseBox  = {};      // physical box of the ring currently on the panel


static inline void rect_grow(Rect &r, const Rect &o) {
    if (!o.valid) return;
    if (!r.valid) { r = o; return; }
    r.x1 = min(r.x1, o.x1); r.y1 = min(r.y1, o.y1);
    r.x2 = max(r.x2, o.x2); r.y2 = max(r.y2, o.y2);
}

// Logical point -> physical, the same mapping the flush paths use (rotation about the
// panel centre, (W-1)/2).
static inline void log_to_phys(float lx, float ly, float *px, float *py) {
    const float c = (SCREEN_W - 1) * 0.5f;
    const float rx = lx - c, ry = ly - c;
    *px = c + s_rotCos * rx - s_rotSin * ry;
    *py = c + s_rotSin * rx + s_rotCos * ry;
}

static void sweep_build_map() {
    if (!s_angMap) return;
    float pcx, pcy;
    log_to_phys((float)SCREEN_CX, (float)SCREEN_CY, &pcx, &pcy);
    s_pcx = pcx; s_pcy = pcy;
    const float rMax2 = (float)(RADAR_R_OUTER_PX + 2) * (float)(RADAR_R_OUTER_PX + 2);
    for (int y = 0; y < SCREEN_H; ++y) {
        uint16_t *row = s_angMap + (size_t)y * SCREEN_W;
        const float dy = (float)y - pcy;
        for (int x = 0; x < SCREEN_W; ++x) {
            const float dx = (float)x - pcx;
            if (dx * dx + dy * dy > rMax2) { row[x] = ANG_OUTSIDE; continue; }
            float a = atan2f(dx, -dy);                       // 0 = up, clockwise
            if (a < 0) a += 2.0f * (float)M_PI;
            int u = (int)(a * ((float)ANG_UNITS / (2.0f * (float)M_PI)));
            row[x] = (uint16_t)(u & (ANG_UNITS - 1));
        }
    }
}

static void sweep_build_lut() {
    for (int i = 0; i < FADE_N; ++i) {
        const float delta = (float)((i << FADE_SHIFT) + (1 << (FADE_SHIFT - 1)));
        float frac = 1.0f - delta / (float)TRAIL_U;          // 1 at the lead edge -> 0 at the tail
        if (frac < 0) frac = 0;
        s_fadeLut[i] = (uint8_t)(frac * frac * (float)SWEEP_TRAIL_OPA);
    }
}

// Blend `tint` over `dst` at `opa` (0..255), ordered-dithered back to RGB565 so the
// fade has no visible banding (ijord's blend565_dither, with a /256 approximation).
static const uint8_t BAYER4[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };
static inline uint16_t blend565(uint16_t dst, uint16_t tint, uint32_t opa, uint32_t d) {
    const uint32_t ia = 256 - opa;
    uint32_t dr = (dst >> 11) & 0x1F, dg = (dst >> 5) & 0x3F, db = dst & 0x1F;
    uint32_t tr = (tint >> 11) & 0x1F, tg = (tint >> 5) & 0x3F, tb = tint & 0x1F;
    dr = (dr << 3) | (dr >> 2); dg = (dg << 2) | (dg >> 4); db = (db << 3) | (db >> 2);
    tr = (tr << 3) | (tr >> 2); tg = (tg << 2) | (tg >> 4); tb = (tb << 3) | (tb >> 2);
    uint32_t r = ((tr * opa + dr * ia) >> 8) + (d >> 1);
    uint32_t g = ((tg * opa + dg * ia) >> 8) + (d >> 2);
    uint32_t b = ((tb * opa + db * ia) >> 8) + (d >> 1);
    r >>= 3; g >>= 2; b >>= 3;
    if (r > 31) r = 31;
    if (g > 63) g = 63;
    if (b > 31) b = 31;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

// Is physical pixel (x,y) inside the mask's rounded rectangle? Tested in logical
// coordinates (inverse rotation), so the shape is exact at any display angle.
static bool mask_hit(const SweepMask &m, int x, int y) {
    const float c = (SCREEN_W - 1) * 0.5f;
    const float rx = (float)x - c, ry = (float)y - c;
    const float lx = c + s_rotCos * rx + s_rotSin * ry;
    const float ly = c - s_rotSin * rx + s_rotCos * ry;
    const float cx = lx < m.x1 + m.r ? m.x1 + m.r : (lx > m.x2 - m.r ? m.x2 - m.r : lx);
    const float cy = ly < m.y1 + m.r ? m.y1 + m.r : (ly > m.y2 - m.r ? m.y2 - m.r : ly);
    const float dx = lx - cx, dy = ly - cy, rr = m.r + 0.5f;    // +0.5: keep the anti-aliased edge too
    return dx * dx + dy * dy <= rr * rr;
}

// The masks that cover physical row y, and whether one of them covers pixel (x, y).
static int row_masks(int y, const SweepMask **out) {
    int n = 0;
    for (int i = 0; i < s_maskN; ++i)
        if (y >= s_mask[i].box.y1 && y <= s_mask[i].box.y2) out[n++] = &s_mask[i];
    return n;
}
static inline bool masked(const SweepMask *const *m, int n, int x, int y) {
    for (int i = 0; i < n; ++i)
        if (x >= m[i]->box.x1 && x <= m[i]->box.x2 && mask_hit(*m[i], x, y)) return true;
    return false;
}

// Blend the sweep trail (the angular fade behind the lead at s_angU) into `buf`, which
// holds physical pixels starting at (bx,by) with row stride `stride`, limited to `r`.
static void trail_blend(uint16_t *buf, int bx, int by, int stride, Rect r) {
    for (int y = r.y1; y <= r.y2; ++y) {
        const SweepMask *rowMask[SWEEP_MASKS];
        const int mn = row_masks(y, rowMask);
        const uint16_t *am = s_angMap + (size_t)y * SCREEN_W;
        uint16_t *row = buf + (size_t)(y - by) * stride - bx;
        const uint8_t *bay = &BAYER4[(y & 3) << 2];
        for (int x = r.x1; x <= r.x2; ++x) {
            const uint16_t m = am[x];
            if (m == ANG_OUTSIDE) continue;
            const int delta = (s_angU - (int)m) & (ANG_UNITS - 1);
            if (delta >= TRAIL_U) continue;
            if (mn && masked(rowMask, mn, x, y)) continue;
            const uint32_t opa = ((uint32_t)s_fadeLut[delta >> FADE_SHIFT] * s_trailMul) >> 8;
            if (opa >= 2) row[x] = blend565(row[x], s_swRing, opa, bay[x & 3]);
        }
    }
}

// Blend segment `s` (same buffer layout), limited to `r`. Drawn by distance to the segment
// (anti-aliased, round ends), not as an angular slice: a slice is under a pixel wide near
// the centre, which made the lead look dotted there and shimmer as it turned.
static void seg_blend(uint16_t *buf, int bx, int by, int stride, Rect r, const Seg &s) {
    const float reach = s.hw + 0.5f;
    for (int y = r.y1; y <= r.y2; ++y) {
        // Columns of this row within `reach` of the segment's line: (x - cx)*uy - fy*ux = +-reach.
        const float fy = (float)y - s_pcy;
        int x1 = r.x1, x2 = r.x2;
        if (fabsf(s.uy) > 1e-4f) {
            float a = s_pcx + (fy * s.ux - reach) / s.uy, b = s_pcx + (fy * s.ux + reach) / s.uy;
            if (a > b) { const float t = a; a = b; b = t; }
            x1 = max(x1, (int)floorf(a)); x2 = min(x2, (int)ceilf(b));
        } else if (fabsf(fy * s.ux) >= reach) {
            continue;                                            // horizontal line, this row is off it
        }
        if (x1 > x2) continue;
        const SweepMask *rowMask[SWEEP_MASKS];
        const int mn = row_masks(y, rowMask);
        uint16_t *row = buf + (size_t)(y - by) * stride - bx;
        const uint8_t *bay = &BAYER4[(y & 3) << 2];
        for (int x = x1; x <= x2; ++x) {
            // distance to the segment (clamping gives the round ends)
            const float fx = (float)x - s_pcx;
            float t = fx * s.ux + fy * s.uy;
            t = t < s.t0 ? s.t0 : (t > s.t1 ? s.t1 : t);
            const float ex = fx - t * s.ux, ey = fy - t * s.uy;
            const float cov = reach - sqrtf(ex * ex + ey * ey);
            if (cov <= 0) continue;
            if (mn && masked(rowMask, mn, x, y)) continue;
            const uint32_t opa = (uint32_t)((cov > 1 ? 1 : cov) * (float)s.opa);
            if (opa >= 2) row[x] = blend565(row[x], s.col, opa, bay[x & 3]);
        }
    }
}

// Blend the centre ring into `buf` (same layout as trail_blend), limited to `r`. It is
// centred on the panel centre, which every rotation leaves in place, as LVGL centred it.
static void pulse_blend(uint16_t *buf, int bx, int by, int stride, Rect r) {
    const float c = (SCREEN_W - 1) * 0.5f;
    const float ro = s_pulseR, ri = ro - PULSE_W;
    for (int y = r.y1; y <= r.y2; ++y) {
        uint16_t *row = buf + (size_t)(y - by) * stride - bx;
        const uint8_t *bay = &BAYER4[(y & 3) << 2];
        const float fy = (float)y - c;
        // Only the columns within the ring's outer edge and outside its hole need the
        // per-pixel distance: two spans per row, found with two square roots.
        const float o2 = (ro + 0.5f) * (ro + 0.5f) - fy * fy;
        if (o2 <= 0) continue;
        const float ow = sqrtf(o2);
        const float i2 = ri > 0.5f ? (ri - 0.5f) * (ri - 0.5f) - fy * fy : 0;
        const float iw = i2 > 0 ? sqrtf(i2) : -1;
        const int xa = max(r.x1, (int)floorf(c - ow)), xd = min(r.x2, (int)ceilf(c + ow));
        const int xb = iw < 0 ? xd : (int)ceilf(c - iw), xc = iw < 0 ? xd + 1 : (int)floorf(c + iw);
        for (int x = xa; x <= xd; ++x) {
            if (x > xb && x < xc) { x = xc - 1; continue; }   // jump over the hole
            const float fx = (float)x - c;
            const float d = sqrtf(fx * fx + fy * fy);
            float cov = min(d - ri, ro - d) + 0.5f;      // anti-aliased inner and outer edges
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            if (d < 4.5f) cov *= max(d - 3.5f, 0.0f);    // stay under the centre dot (7 px)
            const uint32_t opa = (uint32_t)(cov * (float)s_pulseOpa);
            if (opa < 2) continue;
            bool masked = false;
            for (int i = 0; i < s_maskN && !masked; ++i) {
                const Rect &b = s_mask[i].box;
                masked = x >= b.x1 && x <= b.x2 && y >= b.y1 && y <= b.y2 && mask_hit(s_mask[i], x, y);
            }
            if (!masked) row[x] = blend565(row[x], s_pulseInk, opa, bay[x & 3]);
        }
    }
}

static Rect pulse_box(float ro) {
    const int c = (SCREEN_W - 1) / 2, e = (int)ceilf(ro) + 2;
    Rect r = { (c - e) & ~1, (c - e) & ~1, (c + e + 1) | 1, (c + e + 1) | 1, true };
    r.x1 = max(r.x1, 0); r.y1 = max(r.y1, 0);
    r.x2 = min(r.x2, SCREEN_W - 1); r.y2 = min(r.y2, SCREEN_H - 1);
    return r;
}

// Physical box of the wedge at angle `u`, padded. The even start / odd end is kept from
// display.cpp (the CO5300 needs it); the RGB panel doesn't, and it costs at most a pixel.
static Rect wedge_box(int u) {
    float pcx, pcy;
    log_to_phys((float)SCREEN_CX, (float)SCREEN_CY, &pcx, &pcy);
    float minX = pcx, maxX = pcx, minY = pcy, maxY = pcy;
    const float toRad = 2.0f * (float)M_PI / (float)ANG_UNITS;
    for (int i = 0; i <= 12; ++i) {
        const float a = (float)(u - TRAIL_U * i / 12) * toRad;   // the 3 px pad covers the lead's far half
        const float x = pcx + (RADAR_R_OUTER_PX + 2) * sinf(a);
        const float y = pcy - (RADAR_R_OUTER_PX + 2) * cosf(a);
        minX = min(minX, x); maxX = max(maxX, x);
        minY = min(minY, y); maxY = max(maxY, y);
    }
    Rect r = { ((int)minX - 3) & ~1, ((int)minY - 3) & ~1, ((int)maxX + 3) | 1, ((int)maxY + 3) | 1, true };
    r.x1 = max(r.x1, 0); r.y1 = max(r.y1, 0);
    r.x2 = min(r.x2, SCREEN_W - 1); r.y2 = min(r.y2, SCREEN_H - 1);
    return r;
}

static inline Rect rect_and(const Rect &a, const Rect &b) {
    Rect r = { max(a.x1, b.x1), max(a.y1, b.y1), min(a.x2, b.x2), min(a.y2, b.y2), a.valid && b.valid };
    if (r.x1 > r.x2 || r.y1 > r.y2) r.valid = false;
    return r;
}

// Physical box of segment `s`, padded and aligned like the wedge's.
static Rect seg_box(const Seg &s) {
    const float ax = s_pcx + s.t0 * s.ux, ay = s_pcy + s.t0 * s.uy;
    const float bx = s_pcx + s.t1 * s.ux, by = s_pcy + s.t1 * s.uy;
    const float e = s.hw + 2.0f;
    Rect r = { (int)floorf(min(ax, bx) - e) & ~1, (int)floorf(min(ay, by) - e) & ~1,
               (int)ceilf(max(ax, bx) + e) | 1, (int)ceilf(max(ay, by) + e) | 1, true };
    r.x1 = max(r.x1, 0); r.y1 = max(r.y1, 0);
    r.x2 = min(r.x2, SCREEN_W - 1); r.y2 = min(r.y2, SCREEN_H - 1);
    if (r.x1 > r.x2 || r.y1 > r.y2) r.valid = false;
    return r;
}

// A segment at logical angle `deg` (0 = up on the LVGL screen, clockwise): the display
// rotation is added here, as for the sweep, so it lines up with the LVGL dial.
static Seg make_seg(float deg, float t0, float t1, float width, uint16_t col, uint32_t opa) {
    const float a = (deg + (float)s_rot) * ((float)M_PI / 180.0f);
    Seg s = { sinf(a), -cosf(a), t0, t1, width * 0.5f, col, opa, {} };
    s.box = seg_box(s);
    return s;
}

// The clock's hour hand, minute hand and centre cap for frame `f` (all off without one).
static void clock_parts(const ClockFrame *f, Part out[PARTS]) {
    const float R = (float)RADAR_R_OUTER_PX;
    for (int i = 0; i < PARTS; ++i) out[i] = Part{};
    if (!f) return;
    if (f->grow > 0.001f) {
        const float deg[2] = { f->hourDeg, f->minDeg };
        const float len[2] = { R * CLOCK_HOUR_LEN * f->grow, R * CLOCK_MIN_LEN * f->grow };
        const int   w[2]   = { CLOCK_HOUR_W, CLOCK_MIN_W };
        for (int k = 0; k < 2; ++k) {
            Part &p = out[PART_HOUR + k];
            p.on = true;
            p.deg = deg[k];
            p.size = len[k];
            p.seg[0] = make_seg(deg[k], 0, len[k], (float)(w[k] + CLOCK_OUTLINE_W), 0x0000, CLOCK_OUTLINE_OPA);
            p.seg[1] = make_seg(deg[k], 0, len[k], (float)w[k], s_pulseInk, 255);
            p.box = p.seg[0].box;                      // the outline is the wider one
        }
    }
    if (f->capR > 0) {
        Part &p = out[PART_CAP];
        p.on = true;
        p.size = f->capR + f->capDot;                  // either changing means a re-push
        p.seg[0] = make_seg(0, 0, 0, 2.0f * f->capR, s_pulseInk, 255);
        p.seg[1] = make_seg(0, 0, 0, 2.0f * CLOCK_CAP_DOT_R, s_swLead, (uint32_t)lroundf(f->capDot * 255.0f));
        p.box = p.seg[0].box;
    }
}

// Re-push only parts that visibly changed: on/off, colour (sweepConfig), 0.1 degree of
// turn, or a quarter pixel.
static bool part_changed(const Part &a, const Part &b) {
    if (a.on != b.on) return true;
    if (!a.on) return false;
    if (a.seg[0].col != b.seg[0].col || a.seg[1].col != b.seg[1].col) return true;
    return fabsf(a.deg - b.deg) >= 0.1f || fabsf(a.size - b.size) >= 0.25f;
}

// Blend every overlay over `buf` (physical pixels from (bx,by), row stride `stride`) within
// `r`, bottom to top: sweep trail, hour hand, minute hand, lead line / second hand, cap,
// centre ripple.
static void overlays_blend(uint16_t *buf, int bx, int by, int stride, const Rect &r) {
    Rect b;
    if (s_trailOn && (b = rect_and(r, s_trailBox)).valid) trail_blend(buf, bx, by, stride, b);
    for (int i = PART_HOUR; i <= PART_MIN; ++i)
        if (s_part[i].on)
            for (const Seg &s : s_part[i].seg)
                if ((b = rect_and(r, s.box)).valid) seg_blend(buf, bx, by, stride, b, s);
    if (s_leadOn && (b = rect_and(r, s_lead.box)).valid) seg_blend(buf, bx, by, stride, b, s_lead);
    if (s_part[PART_CAP].on)
        for (const Seg &s : s_part[PART_CAP].seg)
            if ((b = rect_and(r, s.box)).valid) seg_blend(buf, bx, by, stride, b, s);
    if (s_pulseOn && (b = rect_and(r, s_pulseBox)).valid) pulse_blend(buf, bx, by, stride, b);
}

// Every panel write goes through here: remember it in s_phys, blend the overlays into the
// part that lies under them, then push.
static void draw_block(int16_t x, int16_t y, lv_color_t *pixels, uint16_t w, uint16_t h) {
    uint16_t *px = (uint16_t *)pixels;
    if (s_phys) {
        for (int row = 0; row < h; ++row)
            memcpy(s_phys + (size_t)(y + row) * SCREEN_W + x, px + (size_t)row * w, (size_t)w * 2);
        overlays_blend(px, x, y, w, Rect{ x, y, x + w - 1, y + h - 1, true });
    }
    panel_push(x, y, px, w, h);
}

// Re-push a physical box from s_phys, with the overlays blended in.
static void push_region(Rect reg) {
    if (!reg.valid) return;
    const int w = reg.x2 - reg.x1 + 1;
    for (int y = reg.y1; y <= reg.y2; y += COMP_ROWS) {
        int rows = min(COMP_ROWS, reg.y2 - y + 1);
        for (int r = 0; r < rows; ++r)
            memcpy(s_compBuf + (size_t)r * w, s_phys + (size_t)(y + r) * SCREEN_W + reg.x1, (size_t)w * 2);
        overlays_blend(s_compBuf, reg.x1, y, w, Rect{ reg.x1, y, reg.x2, y + rows - 1, true });
        panel_push((int16_t)reg.x1, (int16_t)y, s_compBuf, (uint16_t)w, (uint16_t)rows);
    }
}

static void sweep_poll_ui() {
    s_swVisible = ui_sweep_visible();
    int16_t m[SWEEP_MASKS][5];
    const int n = ui_sweep_masks(m, SWEEP_MASKS);
    s_maskN = 0;
    for (int i = 0; i < n; ++i) {              // logical box -> physical bounding box
        const float xs[4] = { (float)m[i][0], (float)m[i][2], (float)m[i][2], (float)m[i][0] };
        const float ys[4] = { (float)m[i][1], (float)m[i][1], (float)m[i][3], (float)m[i][3] };
        float x1 = 1e9f, y1 = 1e9f, x2 = -1e9f, y2 = -1e9f;
        for (int k = 0; k < 4; ++k) {
            float px, py;
            log_to_phys(xs[k], ys[k], &px, &py);
            x1 = min(x1, px); y1 = min(y1, py); x2 = max(x2, px); y2 = max(y2, py);
        }
        float rad = (float)m[i][4];                  // LVGL caps the radius at half the short side
        rad = min(rad, (float)(m[i][2] - m[i][0] + 1) * 0.5f);
        rad = min(rad, (float)(m[i][3] - m[i][1] + 1) * 0.5f);
        s_mask[s_maskN++] = { { (int)floorf(x1) - 1, (int)floorf(y1) - 1, (int)ceilf(x2) + 1, (int)ceilf(y2) + 1, true },
                              (float)m[i][0], (float)m[i][1], (float)m[i][2], (float)m[i][3], rad };
    }
}

static bool sweep_ready() { return s_phys && s_angMap && s_compBuf; }

static void compose_step() {
    if (!sweep_ready()) return;
    const bool sweepOn = s_swWanted && s_swVisible;
    const bool pulseOn = s_pulseWanted && s_swVisible;
    const uint32_t now = millis();
    // A tick every SWEEP_FRAME_MS, or at once when the sweep or the ripple is switched. The
    // clock is only looked at on ticks: it changes the picture at most that often anyway.
    const bool switched = sweepOn != s_sweepWas || pulseOn != s_pulseWas;
    if (!switched && now - s_lastComposeMs < SWEEP_FRAME_MS) return;
    s_sweepWas = sweepOn;
    s_pulseWas = pulseOn;
    ClockFrame cf;
    const bool clk = s_swVisible && radar::clockFrame(cf);
    bool anyOn = s_trailOn || s_leadOn || s_pulseOn;
    for (const Part &p : s_part) anyOn = anyOn || p.on;
    if (!sweepOn && !pulseOn && !clk && !anyOn) return;   // nothing to draw, nothing to clear
    // Advance by elapsed time, but never more than SWEEP_MAX_STEP_MS per tick: a blocked
    // loop (GPS read, full repaint) then pauses the sweep briefly instead of making it jump.
    uint32_t dt = anyOn ? now - s_lastComposeMs : 0;
    if (dt > SWEEP_MAX_STEP_MS) dt = SWEEP_MAX_STEP_MS;
    s_lastComposeMs = now;

    // The sweep line. On the clock it is the second hand, and the free-running phase follows
    // it so the radar sweep goes on from the hand afterwards; otherwise it turns on its own.
    // The clock also draws it with the radar sweep off (Orb, or switched off): no trail then.
    const bool trailOn = sweepOn && (!clk || cf.trailOpa > 0);
    const bool leadOn  = clk ? cf.leadLen > 0.001f : sweepOn;
    if (clk)
        s_swPhase = (uint32_t)(clock_wrap360(cf.leadDeg) * ((float)(ANG_UNITS * 256) / 360.0f)) & (ANG_UNITS * 256 - 1);
    else if (sweepOn)
        s_swPhase = (s_swPhase + (uint32_t)(((uint64_t)dt * ANG_UNITS * 256) / SWEEP_COMP_PERIOD_MS)) & (ANG_UNITS * 256 - 1);
    const int u = (int)((s_swPhase >> 8) + ((uint32_t)s_rot * ANG_UNITS) / 360) & (ANG_UNITS - 1);

    // Old + new boxes, pushed separately: their union would be mostly empty.
    Rect regW = (s_trailOn || s_leadOn) ? s_wedge : Rect{};
    Rect nw = {};
    if (trailOn || leadOn) {
        const float R = (float)RADAR_R_OUTER_PX;
        const float a = (float)u * (2.0f * (float)M_PI / (float)ANG_UNITS);   // 0 = up, clockwise, as in the bearing map
        s_lead = Seg{ sinf(a), -cosf(a), clk ? -R * cf.leadTail : 0.0f, clk ? R * cf.leadLen : R,
                      (clk ? cf.leadW : SWEEP_LEAD_W) * 0.5f, s_swLead, SWEEP_LEAD_OPA, {} };
        s_lead.box = seg_box(s_lead);
        s_angU = u;
        s_trailBox = trailOn ? wedge_box(u) : Rect{};
        nw = s_trailBox;
        if (leadOn) rect_grow(nw, s_lead.box);
    }
    rect_grow(regW, nw);
    s_wedge = nw;
    s_trailOn = trailOn;
    s_leadOn = leadOn;
    s_trailMul = clk ? cf.trailOpa : 256;

    // The clock's hands and cap: each re-pushed (old + new box) only when it visibly changed.
    Part np[PARTS];
    clock_parts(clk ? &cf : nullptr, np);
    Rect regC[PARTS];
    for (int i = 0; i < PARTS; ++i) {
        regC[i] = Rect{};
        if (!part_changed(s_part[i], np[i])) continue;
        if (s_part[i].on) regC[i] = s_part[i].box;
        if (np[i].on) rect_grow(regC[i], np[i].box);
        s_part[i] = np[i];
    }

    const bool pOn = pulseOn && !clk;                  // the cap sits where the ripple would be
    Rect regP = s_pulseOn ? s_pulseBox : Rect{};
    if (pOn) {
        s_pulseMs = (s_pulseMs + dt) % PULSE_PERIOD_MS;
        const float f = (float)s_pulseMs / (float)PULSE_PERIOD_MS;
        s_pulseR = (PULSE_D_MIN + f * (PULSE_D_MAX - PULSE_D_MIN)) * 0.5f;
        s_pulseOpa = (uint32_t)((1.0f - f) * PULSE_OPA);
        s_pulseBox = pulse_box(s_pulseR);
        rect_grow(regP, s_pulseBox);
    } else {
        s_pulseBox.valid = false;
    }
    s_pulseOn = pOn;
    push_region(regW);                         // old wedge area restored, new wedge drawn
    for (const Rect &r : regC) push_region(r); // the same for each clock part that changed
    push_region(regP);                         // and for the ring
}

// Partial refresh: one LVGL area, rotated for 90/180/270 (other angles render whole frames,
// see setRotation), into s_phys and the framebuffer on screen with the sweep blended in.
static void flush_area(const lv_area_t *a, lv_color_t *px) {
    const int lw = a->x2 - a->x1 + 1, lh = a->y2 - a->y1 + 1;
    lv_color_t *out = px;
    int x1 = a->x1, y1 = a->y1, pw = lw, ph = lh;
    if (s_rot == 90) {                      // logical (x, y) -> physical (H-1-y, x)
        x1 = SCREEN_H - 1 - a->y2; y1 = a->x1; pw = lh; ph = lw;
        for (int y = 0; y < lh; ++y)
            for (int x = 0; x < lw; ++x)
                s_rotBuf[x * pw + (lh - 1 - y)] = px[y * lw + x];
        out = s_rotBuf;
    } else if (s_rot == 180) {              // (x, y) -> (W-1-x, H-1-y)
        x1 = SCREEN_W - 1 - a->x2; y1 = SCREEN_H - 1 - a->y2;
        for (int i = 0, j = lw * lh - 1; i < j; ++i, --j) { lv_color_t t = px[i]; px[i] = px[j]; px[j] = t; }
    } else if (s_rot == 270) {              // (x, y) -> (y, W-1-x)
        x1 = a->y1; y1 = SCREEN_W - 1 - a->x2; pw = lh; ph = lw;
        for (int y = 0; y < lh; ++y)
            for (int x = 0; x < lw; ++x)
                s_rotBuf[(lw - 1 - x) * pw + y] = px[y * lw + x];
        out = s_rotBuf;
    }
    draw_block((int16_t)x1, (int16_t)y1, out, (uint16_t)pw, (uint16_t)ph);
}

// Partial refresh draws in strips; whole frames need the whole buffer. Both buffers are
// allocated full size once, so a rotation change can switch between the two.
static void set_draw_buf_size() {
    const uint32_t px = (uint32_t)SCREEN_W * SCREEN_H;
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, s_disp_drv.full_refresh ? px : px / DRAW_BUF_DIV);
}

// --- LVGL flush ----------------------------------------------------------------------
static void touch_sample(bool force);
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    if (!s_panel || !s_fb[0]) {                 // headless (panel bring-up failed)
        if (lv_disp_flush_is_last(drv)) s_frameCount++;
        lv_disp_flush_ready(drv);
        return;
    }
    // Let the sweep tick between strips of a long LVGL frame (a full repaint takes ~200 ms).
    // Safe mid-frame: strips already flushed are in s_phys and on the panel; the rest of
    // s_phys still matches what the panel shows, so the composite is never wrong.
    compose_step();
    // Sample the touch panel too: LVGL reads it only between frames (see touch_queue.h).
    touch_sample(false);
    if (!drv->full_refresh) {
        flush_area(area, px);
        if (lv_disp_flush_is_last(drv)) s_frameCount++;
        lv_disp_flush_ready(drv);
        return;
    }
    // Whole frame: only angles other than 0/90/180/270 come here (full_refresh is set only
    // for them; 0/90/180/270 use partial refresh above). Rotate it into the framebuffer NOT
    // on screen and flip to it.
    lv_color_t *out = (lv_color_t *)s_fb[s_back];
    const int N = SCREEN_W * SCREEN_H;
    // Inverse-map output pixels around the centre; round panel corners are black.
    // A whole frame arrives in one flush, so keep sampling the touch panel while it is
    // rotated, or a quick tap that starts and ends in here never reaches LVGL.
    const float c = (SCREEN_W - 1) * 0.5f;
    for (int y = 0; y < SCREEN_H; ++y) {
        if ((y & 15) == 0) touch_sample(false);
        for (int x = 0; x < SCREEN_W; ++x) {
            const float dx = x - c, dy = y - c;
            const int sx = (int)lroundf(c + s_rotCos * dx + s_rotSin * dy);
            const int sy = (int)lroundf(c - s_rotSin * dx + s_rotCos * dy);
            out[y * SCREEN_W + x].full =
                sx >= 0 && sx < SCREEN_W && sy >= 0 && sy < SCREEN_H
                ? px[sy * SCREEN_W + sx].full : 0;
        }
    }
    // Keep the compositor's copy, and blend the overlays in, as draw_block does for an area.
    if (s_phys) {
        memcpy(s_phys, out, N * sizeof(lv_color_t));
        overlays_blend((uint16_t *)out, 0, 0, SCREEN_W, Rect{ 0, 0, SCREEN_W - 1, SCREEN_H - 1, true });
    }
    touch_sample(false);
    // Flip to the frame just written (no copy: it is one of the panel's framebuffers), then
    // wait until the refill has switched to it, so the other buffer is free for the next
    // frame. A flash op holds the refill ISR off; after 100 ms carry on (worst case one torn
    // frame) rather than stall the UI.
    xSemaphoreTake(s_flipped, 0);               // drop a signal left by an earlier timeout
    esp_lcd_panel_draw_bitmap(s_panel, 0, 0, SCREEN_W, SCREEN_H, out);
    s_flipPending = true;                       // only a switch after this point picks up `out`
    xSemaphoreTake(s_flipped, pdMS_TO_TICKS(100));
    touch_sample(false);
    s_back ^= 1;
    s_frameCount++;
    lv_disp_flush_ready(drv);
}

// CST820 touch -> TouchQueue -> LVGL pointer, undoing the display rotation (mirror of
// display.cpp). With the sweep compositor busy, LVGL renders only a few frames a second
// and reads touch only between them, so a quick tap could fall between two reads.
static bool     s_touchOk = false;              // touch_begin() succeeded
static uint32_t s_touchLastMs = 0;              // when the panel was last sampled
static bool     s_touchReplaying = false;       // LVGL is calling back for the queued samples

// Read the panel into the queue: always from the read callback (force), and at most every
// TOUCH_SAMPLE_MS between flush strips. Both run on the LVGL task.
static void touch_sample(bool force) {
    if (!s_touchOk) return;
    const uint32_t now = millis();
    if (!force && now - s_touchLastMs < TOUCH_SAMPLE_MS) return;
    s_touchLastMs = now;
    uint16_t x = 0, y = 0;
    const TouchRead state = touch_read(&x, &y);
    const float c = (SCREEN_W - 1) * 0.5f;
    const float dx = x - c, dy = y - c;
    const int16_t lx = (int16_t)constrain(lroundf(c + s_rotCos * dx + s_rotSin * dy), 0L, (long)SCREEN_W - 1);
    const int16_t ly = (int16_t)constrain(lroundf(c - s_rotSin * dx + s_rotCos * dy), 0L, (long)SCREEN_H - 1);
    s_touchQueue.add(state == TOUCH_DOWN ? TouchQueue::DOWN :
                     state == TOUCH_UP ? TouchQueue::UP : TouchQueue::NODATA,
                     lx, ly, now);
    // A press held back from LVGL (wake-only) is still a touch: it restarts LVGL's
    // inactivity time, which wakes the screen and keeps it lit while the finger rests.
    if (state == TOUCH_DOWN && s_touchQueue.holding()) lv_disp_trig_activity(NULL);
}

static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    // Take the current state once per LVGL read, not again for each queued sample LVGL
    // asks for: a finger that keeps moving would otherwise keep the queue from emptying.
    if (!s_touchReplaying) touch_sample(true);
    TouchQueue::Sample sample;
    if (!s_touchQueue.next(sample)) sample = s_touchQueue.delivered();
    data->point.x = sample.x;
    data->point.y = sample.y;
    data->state = sample.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = !s_touchQueue.empty();
    s_touchReplaying = data->continue_reading;
}

namespace display {

bool begin() {
    Serial.println("[display] ST7701 480x480 RGB bring-up...");

    Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 400000);   // shared bus: expander + touch + IMU + RTC
    tca9554_init();
    // EXIO8 low at init (matches the Waveshare demo). tca9554_init() drives every expander
    // pin as an output, and they power on high; EXIO8 high leaves the board's onboard
    // buzzer/aux line asserted (constant tone), so pull it low immediately.
    tca9554_set(EXIO_AUX, 0);

    // Backlight PWM (start dim; main.cpp applies the saved brightness right after).
    ledcAttach(PIN_LCD_BL, LCD_BL_PWM_FREQ, LCD_BL_PWM_BITS);
    setBrightness(BRIGHTNESS_DEFAULT);

    tca9554_set(EXIO_LCD_RST, 0);                   // ST7701 hardware reset (EXIO1)
    delay(10);
    tca9554_set(EXIO_LCD_RST, 1);
    delay(50);

    st7701_begin();       // push the ST7701 register init over 3-wire SPI
    // If the RGB panel fails, keep going with a null panel: flush becomes a no-op, but LVGL,
    // touch, WiFi, the web server and (critically) USB-CDC serial all still come up so the
    // failure is diagnosable instead of a silent reboot loop.
    const bool panelOk = rgb_panel_begin();
    if (!panelOk) Serial.println("[display] RGB panel bring-up FAILED — continuing headless for diagnostics");
    else          Serial.println("[display] panel up; init LVGL...");

    lv_init();

    // Full-screen LVGL draw buffers in PSRAM (whole frames need them; partial refresh uses
    // 1/DRAW_BUF_DIV of each, see set_draw_buf_size), plus one area's rotation scratch.
    const size_t px = (size_t)SCREEN_W * SCREEN_H;
    s_buf1   = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    s_buf2   = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    s_rotBuf = (lv_color_t *)heap_caps_malloc(px / DRAW_BUF_DIV * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!s_buf1 || !s_buf2 || !s_rotBuf) {
        Serial.println("[display] LVGL PSRAM buffer alloc FAILED");
        return false;
    }
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, px);   // resized by set_draw_buf_size() below
    Serial.printf("[display] LVGL draw buffers OK (%u KB free PSRAM)\n", (unsigned)(ESP.getFreePsram() / 1024));
    // Sweep compositor buffers (PSRAM; ~0.95 MB), as in display.cpp. Without them the
    // sweep stays in LVGL.
    s_phys    = (uint16_t *)heap_caps_calloc(px, 2, MALLOC_CAP_SPIRAM);
    s_angMap  = (uint16_t *)heap_caps_malloc(px * 2, MALLOC_CAP_SPIRAM);
    s_compBuf = (uint16_t *)heap_caps_malloc((size_t)SCREEN_W * COMP_ROWS * 2, MALLOC_CAP_SPIRAM);
    if (sweep_ready()) {
        const uint32_t t0 = millis();
        sweep_build_map();
        sweep_build_lut();
        Serial.printf("[display] sweep compositor ready (map %lu ms, %u KB free PSRAM)\n",
                      (unsigned long)(millis() - t0), (unsigned)(ESP.getFreePsram() / 1024));
    } else {
        heap_caps_free(s_phys); heap_caps_free(s_angMap); heap_caps_free(s_compBuf);
        s_phys = s_angMap = s_compBuf = nullptr;
        Serial.println("[display] WARNING: compositor buffers failed; sweep stays in LVGL");
    }

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res      = SCREEN_W;
    s_disp_drv.ver_res      = SCREEN_H;
    s_disp_drv.flush_cb     = flush_cb;
    s_disp_drv.draw_buf     = &s_draw_buf;
    s_disp_drv.full_refresh = (s_rot % 90) ? 1 : 0;   // partial refresh unless the angle needs whole frames
    set_draw_buf_size();
    lv_disp_drv_register(&s_disp_drv);

    if (touch_begin()) {
        s_touchOk = true;
        lv_indev_drv_init(&s_indev_drv);
        s_indev_drv.type = LV_INDEV_TYPE_POINTER;
        s_indev_drv.read_cb = touch_read_cb;
        lv_indev_drv_register(&s_indev_drv);
        Serial.println("[display] CST820 touch registered");
    }

    ui_create();
    s_ready = true;
    Serial.printf("[display] LVGL ready (panel %s)\n", panelOk ? "OK" : "FAILED — screen will stay blank");
    return panelOk;
}

void loop() {
    if (!s_ready) return;
    if (sweep_ready()) sweep_poll_ui();
    lv_timer_handler();
    compose_step();
}

// 0..255 -> PWM duty. Backlight is active-high on this board.
void setBrightness(uint8_t v) {
    const uint32_t maxDuty = (1u << LCD_BL_PWM_BITS) - 1u;
    ledcWrite(PIN_LCD_BL, (uint32_t)v * maxDuty / 255u);
    s_touchQueue.setWakeOnly(v == 0);   // a dark panel takes a touch only as a wake-up (as display.cpp)
}

void setRotation(uint16_t degrees) {
    s_rot = degrees % 360;
    const float radians = s_rot * ((float)M_PI / 180.0f);
    s_rotCos = cosf(radians);
    s_rotSin = sinf(radians);
    if (!s_ready) return;
    s_disp_drv.full_refresh = (s_rot % 90) ? 1 : 0;
    set_draw_buf_size();
    if (sweep_ready()) {                   // the compositor's copy must match the panel (as display.cpp)
        memset(s_phys, 0, (size_t)SCREEN_W * SCREEN_H * 2);
        s_trailOn = s_leadOn = false;
        s_wedge.valid = s_trailBox.valid = false;
        for (Part &p : s_part) p.on = false;   // redrawn on the next tick in the new orientation
        s_pulseOn = false;
        s_pulseBox.valid = false;
        sweep_build_map();                 // the scope centre moves by a fraction of a pixel
    }
    lv_obj_t *scr = lv_scr_act();
    if (scr) lv_obj_invalidate(scr);       // full repaint in the new orientation
}
uint16_t rotation() { return s_rot; }

void sweepConfig(uint16_t ring565, uint16_t lead565, uint16_t ink565, bool sweep, bool pulse) {
    s_swRing = ring565;
    s_swLead = lead565;
    s_swWanted = sweep;
    s_pulseInk = ink565;
    s_pulseWanted = pulse;
}
// Check-ins from outside the driver (radar_view's update loop, the GPS idle hook). A sweep
// tick costs about as long here as the gap to the next one, so honouring every check-in
// left radar_view's update ~1 ms of its own work per tick (up to ~1.8 s per poll at ~38
// aircraft). Tick only after TICK_WORK_MS since the last check-in tick: during an update
// the sweep steps ~2 frames at a time (under SWEEP_MAX_STEP_MS, so no slow-down) and the
// update runs many times faster. flush_cb's and loop()'s own ticks are not capped.
void sweepTick() {
    static uint32_t doneMs = 0;
    if (millis() - doneMs < TICK_WORK_MS) return;
    compose_step();
    doneMs = millis();
}
bool sweepAvailable() { return sweep_ready(); }
float sweepAngleDeg() { return (float)(s_swPhase >> 8) * (360.0f / (float)ANG_UNITS); }

uint32_t inactiveMs() { return s_ready ? lv_disp_get_inactive_time(NULL) : 0; }

} // namespace display

#endif  // BOARD_LCD21
