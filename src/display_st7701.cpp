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
static lv_color_t        *s_rotBuf = nullptr;   // full-screen scratch for 90/180/270 (PSRAM)

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
    // NO bounce buffer — permanently. Its refill ISR memcpy's from the PSRAM framebuffer, and
    // PSRAM is unreachable while the cache is disabled during a WiFi/NVS flash op, so the copy
    // faults ("Cache disabled..." with A4=0x2580=9600=this buffer). Making the ISR IRAM-safe
    // can't help — the DATA it reads lives in PSRAM. Drift is instead controlled by a modest
    // pixel clock (RGB_PCLK_HZ) so direct-from-PSRAM DMA comfortably keeps up, plus the
    // per-VSYNC transmission restart (CONFIG_LCD_RGB_RESTART_IN_VSYNC) which resyncs each frame.
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
    Serial.println("[display] RGB panel initialised");
    return true;
}

// --- LVGL flush: full frame each time (full_refresh=1), rotated into the panel FB -----
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    (void)area;
    lv_color_t *out = px;
    const int N = SCREEN_W * SCREEN_H;
    if (s_rot && s_rotBuf) {
        switch (s_rot) {
            case 90:  // 90° CW
                for (int j = 0; j < SCREEN_H; ++j)
                    for (int i = 0; i < SCREEN_W; ++i)
                        s_rotBuf[i * SCREEN_H + (SCREEN_H - 1 - j)] = px[j * SCREEN_W + i];
                break;
            case 180:  // 180°
                for (int k = 0; k < N; ++k) s_rotBuf[k] = px[N - 1 - k];
                break;
            case 270:  // 270° CW
                for (int j = 0; j < SCREEN_H; ++j)
                    for (int i = 0; i < SCREEN_W; ++i)
                        s_rotBuf[(SCREEN_W - 1 - i) * SCREEN_H + j] = px[j * SCREEN_W + i];
                break;
        }
        if (s_rot % 90) {
            // Inverse-map output pixels around the centre; round panel corners are black.
            const float c = (SCREEN_W - 1) * 0.5f;
            for (int y = 0; y < SCREEN_H; ++y) {
                for (int x = 0; x < SCREEN_W; ++x) {
                    const float dx = x - c, dy = y - c;
                    const int sx = (int)lroundf(c + s_rotCos * dx + s_rotSin * dy);
                    const int sy = (int)lroundf(c - s_rotSin * dx + s_rotCos * dy);
                    s_rotBuf[y * SCREEN_W + x].full =
                        sx >= 0 && sx < SCREEN_W && sy >= 0 && sy < SCREEN_H
                        ? px[sy * SCREEN_W + sx].full : 0;
                }
            }
        }
        out = s_rotBuf;
    }
    // Whole-screen blit; the second framebuffer makes this a clean page flip (no tearing).
    if (s_panel) esp_lcd_panel_draw_bitmap(s_panel, 0, 0, SCREEN_W, SCREEN_H, out);
    s_frameCount++;
    lv_disp_flush_ready(drv);
}

// CST820 touch -> LVGL pointer, undoing the display rotation (mirror of display.cpp).
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    uint16_t x = 0, y = 0;
    const TouchRead state = touch_read(&x, &y);
    const float c = (SCREEN_W - 1) * 0.5f;
    const float dx = x - c, dy = y - c;
    const int16_t lx = (int16_t)constrain(lroundf(c + s_rotCos * dx + s_rotSin * dy), 0L, (long)SCREEN_W - 1);
    const int16_t ly = (int16_t)constrain(lroundf(c - s_rotSin * dx + s_rotCos * dy), 0L, (long)SCREEN_H - 1);
    s_touchQueue.add(state == TOUCH_DOWN ? TouchQueue::DOWN :
                     state == TOUCH_UP ? TouchQueue::UP : TouchQueue::NODATA,
                     lx, ly, millis());
    TouchQueue::Sample sample;
    if (!s_touchQueue.next(sample)) sample = s_touchQueue.delivered();
    data->point.x = sample.x;
    data->point.y = sample.y;
    data->state = sample.pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    data->continue_reading = !s_touchQueue.empty();
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

    // Full-screen LVGL draw buffers in PSRAM. full_refresh=1 -> LVGL redraws the whole
    // frame each refresh, so the flush always gets a complete frame to (optionally) rotate.
    const size_t px = (size_t)SCREEN_W * SCREEN_H;
    s_buf1   = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    s_buf2   = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    s_rotBuf = (lv_color_t *)heap_caps_malloc(px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!s_buf1 || !s_buf2 || !s_rotBuf) {
        Serial.println("[display] LVGL PSRAM buffer alloc FAILED");
        return false;
    }
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, px);
    Serial.printf("[display] LVGL draw buffers OK (%u KB free PSRAM)\n", (unsigned)(ESP.getFreePsram() / 1024));

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res      = SCREEN_W;
    s_disp_drv.ver_res      = SCREEN_H;
    s_disp_drv.flush_cb     = flush_cb;
    s_disp_drv.draw_buf     = &s_draw_buf;
    s_disp_drv.full_refresh = 1;
    lv_disp_drv_register(&s_disp_drv);

    if (touch_begin()) {
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

void loop() { if (s_ready) lv_timer_handler(); }

// 0..255 -> PWM duty. Backlight is active-high on this board.
void setBrightness(uint8_t v) {
    const uint32_t maxDuty = (1u << LCD_BL_PWM_BITS) - 1u;
    ledcWrite(PIN_LCD_BL, (uint32_t)v * maxDuty / 255u);
}

void setRotation(uint16_t degrees) {
    s_rot = degrees % 360;
    const float radians = s_rot * ((float)M_PI / 180.0f);
    s_rotCos = cosf(radians);
    s_rotSin = sinf(radians);
    if (!s_ready) return;
    lv_obj_t *scr = lv_scr_act();
    if (scr) lv_obj_invalidate(scr);
}
uint16_t rotation() { return s_rot; }

// The QSPI compositor writes panel GRAM. RGB refresh scans a framebuffer;
// leave sweep/pulse ownership with the shared LVGL renderer on this panel.
bool sweepAvailable() { return false; }
void sweepConfig(uint16_t, uint16_t, uint16_t, bool, bool) {}
void sweepTick() {}
float sweepAngleDeg() { return 0.0f; }   // never asked: sweepAvailable() is false here

uint32_t inactiveMs() { return s_ready ? lv_disp_get_inactive_time(NULL) : 0; }

} // namespace display

#endif  // BOARD_LCD21
