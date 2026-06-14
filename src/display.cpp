// M0 bring-up: CO5300 466x466 AMOLED via Arduino_GFX (QSPI) + LVGL.
// Pins come from config.h (confirmed against the Waveshare board definition and a
// working Arduino_GFX port for this exact panel). The panel runs off the always-on
// DC1 rail, so it lights up without configuring the AXP2101 PMIC.
// The actual UI is built by ui_boot_create() (shared with the native SDL sim).
#include "display.h"
#include "config.h"
#include "radar_view.h"
#include "ui.h"
#include "touch_cst9217.h"
#include "app_log.h"

#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <esp_heap_caps.h>

// --- Arduino_GFX panel -------------------------------------------------------
// Typed as Arduino_CO5300* (not Arduino_GFX*) so setBrightness() — declared on
// Arduino_OLED, not the GFX base — is reachable.
static Arduino_DataBus *s_bus = nullptr;
static Arduino_CO5300  *s_gfx = nullptr;

// --- LVGL plumbing -----------------------------------------------------------
#define LVGL_BUF_LINES 20    // partial draw-buffer height (lines); kept in fast internal RAM for TLS headroom
static lv_disp_draw_buf_t s_draw_buf;
static lv_disp_drv_t      s_disp_drv;
static lv_indev_drv_t     s_indev_drv;
static lv_color_t        *s_buf1 = nullptr;
static lv_color_t        *s_buf2 = nullptr;

static volatile uint32_t s_frameCount = 0;   // rendered frames (last-flush), for FPS measurement
uint32_t display_frames() { return s_frameCount; }

// LVGL -> panel. Push the rendered area straight to the CO5300.
static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    const uint32_t w = (area->x2 - area->x1 + 1);
    const uint32_t h = (area->y2 - area->y1 + 1);
#if (LV_COLOR_16_SWAP != 0)
    s_gfx->draw16bitBeRGBBitmap(area->x1, area->y1, (uint16_t *)px, w, h);
#else
    s_gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)px, w, h);
#endif
    if (lv_disp_flush_is_last(drv)) s_frameCount++;
    lv_disp_flush_ready(drv);
}

// CO5300 (QSPI) requires 2-pixel-aligned flush windows: even start, odd end.
// Without this, partial-area updates (e.g. the radar sweep) tear / ghost / flicker.
static void rounder_cb(lv_disp_drv_t *drv, lv_area_t *area) {
    (void)drv;
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

// CST9217 touch -> LVGL pointer. LVGL keeps the last point on release.
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    uint16_t x, y;
    if (touch_read(&x, &y)) {
        data->point.x = (lv_coord_t)x;
        data->point.y = (lv_coord_t)y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

namespace display {

bool begin() {
    app_log_println("[display] init CO5300 QSPI...");
    s_bus = new Arduino_ESP32QSPI(PIN_LCD_CS, PIN_LCD_SCLK,
                                  PIN_LCD_D0, PIN_LCD_D1, PIN_LCD_D2, PIN_LCD_D3);
    s_gfx = new Arduino_CO5300(s_bus, PIN_LCD_RST, 0 /*rotation*/,
                               SCREEN_W, SCREEN_H,
                               LCD_COL_OFFSET, LCD_ROW_OFFSET, 0, 0);
    if (!s_gfx->begin(LCD_QSPI_HZ)) {
        app_log_println("[display] gfx->begin() FAILED");
        return false;
    }
    s_gfx->fillScreen(RGB565_BLACK);
    s_gfx->setBrightness(BRIGHTNESS_DEFAULT);
    app_log_println("[display] panel up; init LVGL...");

    lv_init();

    // Draw scratch in INTERNAL DMA RAM: rendering anti-aliased graphics into PSRAM is
    // slow (that, not QSPI bandwidth, was the bottleneck). Keep the active buffer in fast
    // internal SRAM; single partial buffer to stay within the internal-RAM budget.
    const size_t buf_px = (size_t)SCREEN_W * LVGL_BUF_LINES;
    s_buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_buf2 = nullptr;
    if (!s_buf1) {
        app_log_println("[display] internal draw buffer failed; falling back to PSRAM");
        s_buf1 = (lv_color_t *)heap_caps_malloc(buf_px * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    }
    lv_disp_draw_buf_init(&s_draw_buf, s_buf1, s_buf2, buf_px);

    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res  = SCREEN_W;
    s_disp_drv.ver_res  = SCREEN_H;
    s_disp_drv.flush_cb = flush_cb;
    s_disp_drv.rounder_cb = rounder_cb;     // CO5300 needs 2-px-aligned windows
    s_disp_drv.draw_buf = &s_draw_buf;
    lv_disp_drv_register(&s_disp_drv);

    // Touch input (CST9217) -> LVGL pointer indev (drives tap-to-inspect + swipe).
    if (touch_begin()) {
        lv_indev_drv_init(&s_indev_drv);
        s_indev_drv.type = LV_INDEV_TYPE_POINTER;
        s_indev_drv.read_cb = touch_read_cb;
        lv_indev_drv_register(&s_indev_drv);
        app_log_println("[display] CST9217 touch registered");
    }

    app_log_printf("[display] PSRAM free: %u KB\n", (unsigned)(ESP.getFreePsram() / 1024));
    ui_create();                   // M3: radar/list/stats views + tap-to-inspect
    app_log_println("[display] LVGL ready");
    return true;
}

void loop() { lv_timer_handler(); }

void setBrightness(uint8_t v) { if (s_gfx) s_gfx->setBrightness(v); }

uint32_t inactiveMs() { return lv_disp_get_inactive_time(NULL); }

} // namespace display
