// M3 UI: tileview (radar / list / stats / weather) + tap-to-inspect detail card.
// Pure LVGL, portable. Taps hit-test via radar::hitTest; selection lives in radar.
#include "ui.h"
#include "radar_view.h"
#include "airports.h"
#include "coastline.h"
#include "route.h"
#include "photo.h"
#include "weather.h"
#include "config.h"
#ifdef ARDUINO
#include "touch_cst9217.h"
#endif
#include <lvgl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define UI_GREEN lv_color_hex(0x1DFF86)
#define UI_INK   lv_color_hex(0xEAFFF3)
#define UI_SOFT  lv_color_hex(0x9AFFC8)
#define UI_DIM   lv_color_hex(0x5F7A6C)
#define UI_PANEL lv_color_hex(0x0C160F)
#define UI_EMERG lv_color_hex(0xFF5A3C)
#define UI_MIL   lv_color_hex(0xFFB23C)
#define UI_AIRPORT lv_color_hex(0x8A93A6)
#define WEATHER_AUTO_REFRESH_DELAY_MS 2000

static lv_obj_t *s_tv = nullptr;
static lv_obj_t *s_tileRadar = nullptr, *s_tileList = nullptr, *s_tileStats = nullptr, *s_tileWeather = nullptr;
static lv_obj_t *s_card = nullptr, *s_cardTitle = nullptr, *s_cardL = nullptr, *s_cardR = nullptr;
static lv_obj_t *s_cardRoute = nullptr;
static lv_obj_t *s_photo = nullptr, *s_photoCredit = nullptr;   // aircraft photo above the card
static char s_lastRouteReq[12] = "";
static lv_obj_t *s_hudWifi = nullptr, *s_hudCount = nullptr, *s_hudClock = nullptr, *s_hudBatt = nullptr, *s_hudDate = nullptr;
static lv_obj_t *s_helpHotspot = nullptr;
static lv_obj_t *s_hudBars[4] = { nullptr, nullptr, nullptr, nullptr };   // WiFi signal-strength bars
static lv_obj_t *s_list = nullptr;
static lv_obj_t *s_statsLbl = nullptr;
static lv_obj_t *s_statsNet = nullptr;
static lv_obj_t *s_weatherCanvas = nullptr;
static lv_obj_t *s_weatherAirportLayer = nullptr;
static lv_obj_t *s_weatherLabel = nullptr;
static lv_obj_t *s_weatherLoading = nullptr;
static lv_obj_t *s_weatherRefreshLabel = nullptr;
static lv_obj_t *s_weatherRefreshBtn = nullptr;
static void (*s_weatherRefreshCb)(void) = nullptr;
static lv_timer_t *s_weatherAutoTimer = nullptr;
static bool s_weatherAutoPending = false;
static lv_obj_t *s_lastTile = nullptr;
static bool s_wifiUp = false, s_feedOk = false;
static int s_rssi = -127;
static int s_battPct = -1;
static bool s_battCharging = false, s_battPresent = false;

// --------------------------------------------------------------------- units
// 0 = Aviation (ft, kt, km) · 1 = Metric (m, km/h, km) · 2 = Imperial (ft, mph, mi).
// The feed gives altitude in ft, speed in kt, vertical speed in fpm, distance in km.
static int s_units = 0;
void ui_set_units(int u) { s_units = (u < 0 || u > 2) ? 0 : u; }

static void fmt_alt(char *b, size_t n, float ft, bool gnd) {
    if (gnd)            snprintf(b, n, "GND");
    else if (s_units == 1) snprintf(b, n, "%.0f m",  ft * 0.3048f);
    else                snprintf(b, n, "%.0f ft", ft);
}
static void fmt_spd(char *b, size_t n, float kt) {
    if (kt != kt)          snprintf(b, n, "-");
    else if (s_units == 1) snprintf(b, n, "%.0f km/h", kt * 1.852f);
    else if (s_units == 2) snprintf(b, n, "%.0f mph",  kt * 1.15078f);
    else                   snprintf(b, n, "%.0f kt",   kt);
}
static void fmt_vs(char *b, size_t n, float fpm) {
    if (fpm != fpm)        snprintf(b, n, "-");
    else if (s_units == 1) snprintf(b, n, "%+.1f m/s", fpm * 0.00508f);
    else                   snprintf(b, n, "%+.0f fpm", fpm);
}
static float dist_val(float km)   { return s_units == 2 ? km * 0.621371f : km; }
static const char *dist_unit(void){ return s_units == 2 ? "mi" : "km"; }

// Fold Latin-1 accents / drop any other non-ASCII so the Montserrat font never hits a
// missing glyph (which renders as an empty box). Belt-and-suspenders for card text.
static void fold_ascii(char *s) {
    char *o = s;
    for (unsigned char *p = (unsigned char *)s; *p; ) {
        if (*p < 0x80) { *o++ = (char)*p++; continue; }
        if (*p == 0xC3 && p[1]) {                       // Latin-1 Supplement (U+00C0..U+00FF)
            const unsigned char d = p[1];
            char r;
            if      (d >= 0x80 && d <= 0x85) r = 'A';
            else if (d >= 0xA0 && d <= 0xA5) r = 'a';
            else if (d == 0x87)              r = 'C';
            else if (d == 0xA7)              r = 'c';
            else if (d >= 0x88 && d <= 0x8B) r = 'E';
            else if (d >= 0xA8 && d <= 0xAB) r = 'e';
            else if (d >= 0x8C && d <= 0x8F) r = 'I';
            else if (d >= 0xAC && d <= 0xAF) r = 'i';
            else if (d == 0x91)              r = 'N';
            else if (d == 0xB1)              r = 'n';
            else if (d >= 0x92 && d <= 0x96) r = 'O';
            else if (d >= 0xB2 && d <= 0xB6) r = 'o';
            else if (d >= 0x99 && d <= 0x9C) r = 'U';
            else if (d >= 0xB9 && d <= 0xBC) r = 'u';
            else                             r = '?';
            *o++ = r; p += 2; continue;
        }
        ++p;                                            // skip other multibyte lead + continuation
        while (*p >= 0x80 && *p < 0xC0) ++p;
    }
    *o = 0;
}

// ----------------------------------------------------------------- detail card
static void refresh_card(void) {
    AcInfo in;
    if (!radar::selected(in)) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        if (s_photo)       lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
        if (s_photoCredit) lv_obj_add_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
        s_lastRouteReq[0] = 0;
        return;
    }
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    char title[40];
    if (in.type[0]) snprintf(title, sizeof(title), "%s  %s", in.call[0] ? in.call : "-", in.type);
    else            snprintf(title, sizeof(title), "%s", in.call[0] ? in.call : "-");
    fold_ascii(title);
    lv_label_set_text(s_cardTitle, title);
    lv_obj_set_style_text_color(s_cardTitle, in.emergency ? UI_EMERG : UI_INK, 0);

    char altS[16], vsS[24], spdS[16], sqS[16];
    fmt_alt(altS, sizeof(altS), in.altFt, in.onGround);
    fmt_vs (vsS,  sizeof(vsS),  in.vsFpm);
    fmt_spd(spdS, sizeof(spdS), in.gsKt);
    if (in.squawk < 0)          snprintf(sqS, sizeof(sqS), "-");
    else                        snprintf(sqS, sizeof(sqS), "%04d", in.squawk);

    char left[96], right[96];
    snprintf(left,  sizeof(left),  "ALT  %s\nSPD  %s\nDIST %.1f %s", altS, spdS, dist_val(in.distKm), dist_unit());
    snprintf(right, sizeof(right), "V/S  %s\nHDG  %03.0f\nSQK  %s", vsS, in.bearingDeg, sqS);
    lv_label_set_text(s_cardL, left);
    lv_label_set_text(s_cardR, right);

    // route (origin -> destination), looked up asynchronously by callsign
    if (in.call[0] && strcmp(in.call, s_lastRouteReq) != 0) {
        snprintf(s_lastRouteReq, sizeof(s_lastRouteReq), "%s", in.call);
        route_request(in.call, in.lat, in.lon);
    }
    char rfrom[40], rto[40];
    if (!in.call[0]) {
        lv_label_set_text(s_cardRoute, "Route -");                 // no callsign -> nothing to look up
    } else if (route_get(in.call, rfrom, sizeof(rfrom), rto, sizeof(rto))) {
        char rt[96];
        if (rfrom[0] || rto[0]) snprintf(rt, sizeof(rt), "%s -> %s", rfrom[0] ? rfrom : "?", rto[0] ? rto : "?");
        else                    snprintf(rt, sizeof(rt), "Route unavailable");
        fold_ascii(rt);
        lv_label_set_text(s_cardRoute, rt);
    } else {
        lv_label_set_text(s_cardRoute, "Looking up route...");     // pending: lookup in flight
    }

    // aircraft photo (planespotters), shown above the card when one is available
    if (in.hex[0]) photo_request(in.hex);
    int pw = 0, ph = 0; char pcred[40];
    if (s_photo && in.hex[0] && photo_get(in.hex, &pw, &ph, pcred, sizeof(pcred)) && pw > 0 && ph > 0) {
        int mw, mh;
        lv_color_t *pbuf = photo_buffer(&mw, &mh);
        lv_canvas_set_buffer(s_photo, pbuf, pw, ph, LV_IMG_CF_TRUE_COLOR);
        lv_obj_set_size(s_photo, pw, ph);
        lv_obj_align(s_photo, LV_ALIGN_CENTER, 0, -28 - ph / 2);   // sit lower: fill the band down to the card
        lv_obj_clear_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_photo);
        if (s_photoCredit) {
            char c[52];
            snprintf(c, sizeof(c), "Photo: %s", pcred[0] ? pcred : "planespotters.net");
            fold_ascii(c);
            lv_label_set_text(s_photoCredit, c);
            lv_obj_align_to(s_photoCredit, s_photo, LV_ALIGN_OUT_BOTTOM_MID, 0, 1);
            lv_obj_clear_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
        }
    } else if (s_photo) {
        // No image to show yet: hide the canvas, but use the caption line to tell the
        // user what's happening — "Loading..." while the fetch is in flight, or a quiet
        // "No photo" once it finished without one. Unobtrusive (small, dim) but informative.
        lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);
        if (s_photoCredit) {
            const bool done = in.hex[0] && photo_done(in.hex);
            lv_label_set_text(s_photoCredit, done ? "No photo available" : "Loading photo...");
            lv_obj_align(s_photoCredit, LV_ALIGN_CENTER, 0, -104);   // where the photo would sit
            lv_obj_clear_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// --------------------------------------------------------------------- input
static bool s_longPressed = false;
static bool s_panActive = false;
static bool s_panMoved = false;
static lv_point_t s_panLast;
static int s_rangeIdx = -1;
static float s_rangeKm = RANGE_KM_DEFAULT;   // current display range (km), for the stats view
static void (*s_rangeCb)(float) = nullptr;
static void (*s_panCb)(int dx, int dy, bool done) = nullptr;
static lv_obj_t *s_zoomBtn = nullptr, *s_zoomLbl = nullptr;
static bool s_zoomLongPressed = false;
static lv_obj_t *s_rangeModal = nullptr;
static lv_obj_t *s_rangeBtns[8] = { nullptr };
static bool s_rangeClosing = false;
static lv_obj_t *s_themeModal = nullptr;
static lv_obj_t *s_themeBtns[THEME_COUNT] = { nullptr };
static bool s_themeClosing = false;

void ui_set_range_cb(void (*cb)(float)) { s_rangeCb = cb; }
void ui_set_pan_cb(void (*cb)(int dx, int dy, bool done)) { s_panCb = cb; }

static uint8_t ui_touch_points(void) {
#ifdef ARDUINO
    return touch_points();
#else
    return 1;
#endif
}

static void fmt_range(char *b, size_t n, float km) {
    snprintf(b, n, "%.0f %s", dist_val(km), dist_unit());
}

static void close_range_modal(void) {
    if (!s_rangeModal) return;
    lv_obj_del(s_rangeModal);
    s_rangeModal = nullptr;
    memset(s_rangeBtns, 0, sizeof(s_rangeBtns));
    s_rangeClosing = false;
}

static void range_modal_bg_cb(lv_event_t *e) {
    if (lv_event_get_target(e) == s_rangeModal) close_range_modal();
}

static void range_close_timer_cb(lv_timer_t *t) {
    (void)t;
    close_range_modal();
}

static void style_range_btn(lv_obj_t *btn, bool selected) {
    if (!btn) return;
    lv_obj_set_style_bg_color(btn, selected ? UI_GREEN : lv_color_hex(0x11251A), 0);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) lv_obj_set_style_text_color(lbl, selected ? lv_color_black() : UI_INK, 0);
}

static void range_option_cb(lv_event_t *e) {
    if (s_rangeClosing) return;
    const int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    s_rangeIdx = idx;
    const int n = (int)(sizeof(RANGE_STEPS_KM) / sizeof(RANGE_STEPS_KM[0]));
    for (int i = 0; i < n && i < (int)(sizeof(s_rangeBtns) / sizeof(s_rangeBtns[0])); ++i) {
        style_range_btn(s_rangeBtns[i], i == idx);
    }
    if (s_rangeCb) s_rangeCb(RANGE_STEPS_KM[idx]);
    s_rangeClosing = true;
    lv_timer_t *timer = lv_timer_create(range_close_timer_cb, 220, nullptr);
    lv_timer_set_repeat_count(timer, 1);
}

static void show_range_selector(void) {
    if (s_rangeModal) { close_range_modal(); return; }
    s_rangeClosing = false;

    s_rangeModal = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_rangeModal);
    lv_obj_set_size(s_rangeModal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_rangeModal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_rangeModal, 150, 0);
    lv_obj_add_flag(s_rangeModal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_rangeModal, range_modal_bg_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *panel = lv_obj_create(s_rangeModal);
    lv_obj_set_size(panel, 260, 300);
    lv_obj_center(panel);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_bg_color(panel, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, UI_GREEN, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_opa(panel, 180, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Select range");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, UI_GREEN, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 14);

    const int n = (int)(sizeof(RANGE_STEPS_KM) / sizeof(RANGE_STEPS_KM[0]));
    for (int i = 0; i < n; ++i) {
        lv_obj_t *btn = lv_btn_create(panel);
        if (i < (int)(sizeof(s_rangeBtns) / sizeof(s_rangeBtns[0]))) s_rangeBtns[i] = btn;
        lv_obj_set_size(btn, 90, 42);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, (i % 2) ? 139 : 31, 54 + (i / 2) * 54);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, i == s_rangeIdx ? UI_GREEN : lv_color_hex(0x11251A), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, UI_GREEN, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_opa(btn, 150, 0);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, range_option_cb, LV_EVENT_CLICKED, nullptr);

        char txt[16];
        fmt_range(txt, sizeof(txt), RANGE_STEPS_KM[i]);
        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, i == s_rangeIdx ? lv_color_black() : UI_INK, 0);
        lv_obj_center(lbl);
    }
}

static void close_theme_modal(void) {
    if (!s_themeModal) return;
    lv_obj_del(s_themeModal);
    s_themeModal = nullptr;
    memset(s_themeBtns, 0, sizeof(s_themeBtns));
    s_themeClosing = false;
}

static void theme_modal_bg_cb(lv_event_t *e) {
    if (lv_event_get_target(e) == s_themeModal) close_theme_modal();
}

static void theme_close_timer_cb(lv_timer_t *t) {
    (void)t;
    close_theme_modal();
}

static const char *theme_name(int idx) {
    switch (idx) {
        case THEME_PHOSPHOR: return "Phosphor";
        case THEME_DRAGON:   return "Dragon";
        case THEME_AMBER:    return "Amber";
        case THEME_MILITARY: return "Military";
        default:             return "Theme";
    }
}

static void style_theme_btn(lv_obj_t *btn, bool selected) {
    if (!btn) return;
    lv_obj_set_style_bg_color(btn, selected ? UI_GREEN : lv_color_hex(0x11251A), 0);
    lv_obj_t *lbl = lv_obj_get_child(btn, 0);
    if (lbl) lv_obj_set_style_text_color(lbl, selected ? lv_color_black() : UI_INK, 0);
}

static void theme_option_cb(lv_event_t *e) {
    if (s_themeClosing) return;
    const int idx = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_target(e));
    for (int i = 0; i < THEME_COUNT; ++i) style_theme_btn(s_themeBtns[i], i == idx);
    radar::setTheme(idx);
    s_themeClosing = true;
    lv_timer_t *timer = lv_timer_create(theme_close_timer_cb, 220, nullptr);
    lv_timer_set_repeat_count(timer, 1);
}

static void show_theme_selector(void) {
    if (s_themeModal) { close_theme_modal(); return; }
    s_themeClosing = false;

    s_themeModal = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_themeModal);
    lv_obj_set_size(s_themeModal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_themeModal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_themeModal, 150, 0);
    lv_obj_add_flag(s_themeModal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_themeModal, theme_modal_bg_cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *panel = lv_obj_create(s_themeModal);
    lv_obj_set_size(panel, 300, 260);
    lv_obj_center(panel);
    lv_obj_set_style_radius(panel, 10, 0);
    lv_obj_set_style_bg_color(panel, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, UI_GREEN, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_opa(panel, 180, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "Select theme");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, UI_GREEN, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    const int current = radar::theme();
    for (int i = 0; i < THEME_COUNT; ++i) {
        lv_obj_t *btn = lv_btn_create(panel);
        s_themeBtns[i] = btn;
        lv_obj_set_size(btn, 118, 42);
        lv_obj_align(btn, LV_ALIGN_TOP_LEFT, (i % 2) ? 158 : 24, 64 + (i / 2) * 58);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_bg_color(btn, i == current ? UI_GREEN : lv_color_hex(0x11251A), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, UI_GREEN, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_opa(btn, 150, 0);
        lv_obj_set_user_data(btn, (void *)(intptr_t)i);
        lv_obj_add_event_cb(btn, theme_option_cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, theme_name(i));
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(lbl, i == current ? lv_color_black() : UI_INK, 0);
        lv_obj_center(lbl);
    }
}

static void zoom_cb(lv_event_t *e) {   // opens a precise range selector
    (void)e;
    if (s_zoomLongPressed) { s_zoomLongPressed = false; return; }
    static uint32_t last = 0;
    const uint32_t now = lv_tick_get();
    if (now - last < 250) return;      // debounce repeated/held presses
    last = now;
    show_range_selector();
}

static void zoom_longpress_cb(lv_event_t *e) {
    (void)e;
    s_zoomLongPressed = true;
    show_theme_selector();
}

void ui_set_range_km(float km) {
    s_rangeKm = km;
    if (s_zoomLbl) {
        char b[20];
        char r[14];
        fmt_range(r, sizeof(r), km);
        snprintf(b, sizeof(b), LV_SYMBOL_LIST " %s", r);
        lv_label_set_text(s_zoomLbl, b);
    }
    int best = 0; float bd = 1e9f;                 // sync the cycle index to the shown range
    const int n = (int)(sizeof(RANGE_STEPS_KM) / sizeof(RANGE_STEPS_KM[0]));
    for (int i = 0; i < n; ++i) { float d = km - RANGE_STEPS_KM[i]; if (d < 0) d = -d; if (d < bd) { bd = d; best = i; } }
    s_rangeIdx = best;
}

static void radar_press_cb(lv_event_t *e) { (void)e; s_longPressed = false; }

static void radar_pressing_cb(lv_event_t *e) {
    (void)e;
    if (!s_panCb) return;
    if (ui_touch_points() < 2) {
        if (s_panActive) {
            s_panActive = false;
            if (s_tv) lv_obj_add_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE);
            radar::setAircraftLayerVisible(true);
            s_panCb(0, 0, true);
        }
        return;
    }

    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    if (!s_panActive) {
        s_panActive = true;
        s_panMoved = true;     // suppress aircraft/airport click even if the fingers do not move
        s_longPressed = true;
        s_panLast = p;
        if (s_tv) lv_obj_clear_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE);   // two-finger pan should not swipe tiles
        radar::setAircraftLayerVisible(false);
        return;
    }

    const int dx = p.x - s_panLast.x;
    const int dy = p.y - s_panLast.y;
    if (dx == 0 && dy == 0) return;
    s_panMoved = true;
    s_longPressed = true;   // suppress normal tap/select after a pan gesture
    s_panLast = p;
    s_panCb(dx, dy, false);
}

static void radar_release_cb(lv_event_t *e) {
    (void)e;
    if (!s_panActive) return;
    s_panActive = false;
    if (s_tv) lv_obj_add_flag(s_tv, LV_OBJ_FLAG_SCROLLABLE);
    radar::setAircraftLayerVisible(true);
    if (s_panCb) s_panCb(0, 0, true);
}

static void chart_popup_close_cb(lv_event_t *e) {
    lv_obj_t *modal = (lv_obj_t *)lv_event_get_user_data(e);
    if (modal) lv_obj_del(modal);
}

static void show_help_popup(void) {
    lv_obj_t *modal = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, 150, 0);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *panel = lv_obj_create(modal);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, SCREEN_W - 8, SCREEN_H - 8);
    lv_obj_center(panel);
    lv_obj_set_style_radius(panel, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05100A), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, UI_GREEN, 0);
    lv_obj_set_style_border_opa(panel, 100, 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_clip_corner(panel, true, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "SCREEN HELP");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, UI_GREEN, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    lv_obj_t *body = lv_obj_create(panel);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 330, 270);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 68);
    lv_obj_set_style_pad_left(body, 8, 0);
    lv_obj_set_style_pad_right(body, 8, 0);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_AUTO);

    lv_obj_t *txt = lv_label_create(body);
    lv_label_set_long_mode(txt, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(txt, 300);
    lv_obj_set_style_text_font(txt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(txt, UI_INK, 0);
    lv_label_set_text(txt,
        "Radar screen\n"
        "- Aircraft triangles point along their track.\n"
        "- Triangle color shows altitude. Long-press the center for the aircraft color legend.\n"
        "- Tap an aircraft to open its detail card. Tap empty scope to clear selection.\n"
        "- Tap an airport ICAO label to show airport info.\n\n"
        "Range and map\n"
        "- Tap the range button near the bottom to choose an exact range.\n"
        "- Long-press the range button to choose a theme.\n"
        "- Use two fingers to pan the map center. The new center is saved shortly after you stop.\n\n"
        "Pages\n"
        "- Swipe left from radar for weather.\n"
        "- Keep swiping for aircraft list and stats.\n"
        "- Weather refreshes when opened, and the Refresh button requests a new image.\n\n"
        "Status\n"
        "- Top icons show WiFi/feed, time/date, battery, and special alerts.\n"
        "- EMR means emergency squawk. MIL means military. ALR means mixed/combined alert count.\n\n"
        "Web portal\n"
        "- Use capsuleradar.local or the IP on Stats to configure units, timezone, brightness, airports, ground aircraft, and animations.");
    lv_obj_align(txt, LV_ALIGN_TOP_LEFT, 0, 0);

    lv_obj_t *close = lv_btn_create(panel);
    lv_obj_set_size(close, 112, 40);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -42);
    lv_obj_set_style_bg_color(close, UI_GREEN, 0);
    lv_obj_add_event_cb(close, chart_popup_close_cb, LV_EVENT_CLICKED, modal);
    lv_obj_t *cl = lv_label_create(close);
    lv_label_set_text(cl, "Close");
    lv_obj_set_style_text_color(cl, lv_color_black(), 0);
    lv_obj_center(cl);
}

static void help_hotspot_longpress_cb(lv_event_t *e) {
    (void)e;
    s_longPressed = true;   // suppress any follow-up radar click
    show_help_popup();
}

static void legend_glyph_draw_cb(lv_event_t *e) {
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    const lv_coord_t cx = (lv_coord_t)((a.x1 + a.x2) / 2);
    const lv_coord_t cy = (lv_coord_t)((a.y1 + a.y2) / 2);
    const uint32_t colorHex = (uint32_t)(uintptr_t)lv_obj_get_user_data(obj);

    lv_point_t pts[4] = {
        { cx,     (lv_coord_t)(cy - 8) },
        { (lv_coord_t)(cx + 5), (lv_coord_t)(cy + 4) },
        { cx,     (lv_coord_t)(cy + 7) },
        { (lv_coord_t)(cx - 5), (lv_coord_t)(cy + 4) },
    };
    lv_draw_rect_dsc_t dsc;
    lv_draw_rect_dsc_init(&dsc);
    dsc.bg_color = lv_color_hex(colorHex);
    dsc.bg_opa = LV_OPA_COVER;
    lv_draw_polygon(lv_event_get_draw_ctx(e), &dsc, pts, 4);
}

static lv_obj_t *legend_row(lv_obj_t *parent, int y, uint32_t colorHex, const char *text) {
    lv_obj_t *tri = lv_obj_create(parent);
    lv_obj_remove_style_all(tri);
    lv_obj_set_size(tri, 18, 22);
    lv_obj_set_user_data(tri, (void *)(uintptr_t)colorHex);
    lv_obj_add_event_cb(tri, legend_glyph_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_align(tri, LV_ALIGN_TOP_LEFT, 76, y - 2);

    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, 260);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl, UI_INK, 0);
    lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 102, y);
    return lbl;
}

static void show_legend_popup(void) {
    lv_obj_t *modal = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, 150, 0);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *panel = lv_obj_create(modal);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, SCREEN_W - 8, SCREEN_H - 8);
    lv_obj_center(panel);
    lv_obj_set_style_radius(panel, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05100A), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, UI_GREEN, 0);
    lv_obj_set_style_border_opa(panel, 100, 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_clip_corner(panel, true, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, "AIRCRAFT LEGEND");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, UI_GREEN, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    legend_row(panel, 76,  0x888888, "Gray: on ground");
    legend_row(panel, 102, 0xFF5A3C, "Red: below 3,000 ft");
    legend_row(panel, 128, 0xFFB23C, "Amber: 3,000-10,000 ft");
    legend_row(panel, 154, 0xC8FF3C, "Lime: 10,000-20,000 ft");
    legend_row(panel, 180, 0x39FF14, "Green: 20,000-30,000 ft");
    legend_row(panel, 206, 0x3CE0FF, "Cyan: above 30,000 ft");
    legend_row(panel, 238, 0xFF5A3C, "EMR: emergency squawk");
    legend_row(panel, 264, 0xFFB23C, "MIL: military aircraft");
    legend_row(panel, 290, 0x1DFF86, "Dim trail: recent path, fades out");
    legend_row(panel, 316, 0xEAFFF3, "Triangle points along track");

    lv_obj_t *close = lv_btn_create(panel);
    lv_obj_set_size(close, 112, 40);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -42);
    lv_obj_set_style_bg_color(close, UI_GREEN, 0);
    lv_obj_add_event_cb(close, chart_popup_close_cb, LV_EVENT_CLICKED, modal);
    lv_obj_t *cl = lv_label_create(close);
    lv_label_set_text(cl, "Close");
    lv_obj_set_style_text_color(cl, lv_color_black(), 0);
    lv_obj_center(cl);
}

static void radar_longpress_cb(lv_event_t *e) {
    (void)e;
    if (ui_touch_points() >= 2) return;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const int dx = (int)p.x - SCREEN_CX;
    const int dy = (int)p.y - SCREEN_CY;
    if (dx * dx + dy * dy > 70 * 70) return;
    s_longPressed = true;   // suppress the follow-up click/aircraft selection
    s_panMoved = true;
    show_legend_popup();
}

static void show_airport_chart_popup(const char *icao) {
    lv_obj_t *modal = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(modal, 150, 0);
    lv_obj_add_flag(modal, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *panel = lv_obj_create(modal);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, SCREEN_W - 8, SCREEN_H - 8);
    lv_obj_center(panel);
    lv_obj_set_style_radius(panel, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x05100A), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(panel, UI_GREEN, 0);
    lv_obj_set_style_border_opa(panel, 100, 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_clip_corner(panel, true, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    char name[56] = "", runways[104] = "-", surfaces[40] = "-", freqs[80] = "-";
    airports_info(icao, name, sizeof(name), runways, sizeof(runways),
                  surfaces, sizeof(surfaces), freqs, sizeof(freqs));

    lv_obj_t *title = lv_label_create(panel);
    lv_label_set_text(title, icao);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, UI_GREEN, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 34);

    char txt[420];
    snprintf(txt, sizeof(txt), "%s\n\nRunways\n%s\nSurface\n%s\n\nFreqs\n%s",
             name[0] ? name : "Airport", runways, surfaces, freqs);
    lv_obj_t *lbl = lv_label_create(panel);
    lv_label_set_text(lbl, txt);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl, UI_INK, 0);
    lv_obj_set_width(lbl, SCREEN_W - 120);
    lv_obj_align(lbl, LV_ALIGN_TOP_MID, 0, 88);

    lv_obj_t *close = lv_btn_create(panel);
    lv_obj_set_size(close, 112, 40);
    lv_obj_align(close, LV_ALIGN_BOTTOM_MID, 0, -42);
    lv_obj_set_style_bg_color(close, UI_GREEN, 0);
    lv_obj_add_event_cb(close, chart_popup_close_cb, LV_EVENT_CLICKED, modal);
    lv_obj_t *cl = lv_label_create(close);
    lv_label_set_text(cl, "Close");
    lv_obj_set_style_text_color(cl, lv_color_black(), 0);
    lv_obj_center(cl);
}

static void radar_clicked_cb(lv_event_t *e) {
    (void)e;
    if (s_panMoved) { s_panMoved = false; s_longPressed = false; return; }
    if (s_longPressed) { s_longPressed = false; return; }   // ignore the click after a long-press
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const int acIdx = radar::hitTest(p.x, p.y);
    radar::select(acIdx);   // aircraft hit -> select; miss -> clear
    refresh_card();
    if (acIdx < 0) {
        char icao[5];
        if (airports_hit_test(p.x, p.y, icao, sizeof(icao))) show_airport_chart_popup(icao);
    }
}

static void list_btn_cb(lv_event_t *e) {
    lv_obj_t *b = lv_event_get_target(e);
    const int idx = (int)(intptr_t)lv_obj_get_user_data(b);
    radar::select(idx);
    refresh_card();
    lv_obj_set_tile_id(s_tv, 0, 0, LV_ANIM_ON);   // jump back to the radar
}

// ----------------------------------------------------------------- list/stats
void ui_set_status(bool wifiUp, bool feedOk, int rssi, const char *clock) {
    s_wifiUp = wifiUp;
    s_feedOk = feedOk;
    s_rssi = rssi;
    // bar count from RSSI (dBm): the weaker the signal, the fewer lit bars
    int level;
    if      (!wifiUp)     level = 0;
    else if (rssi >= -55) level = 4;   // excellent
    else if (rssi >= -67) level = 3;   // good
    else if (rssi >= -75) level = 2;   // ok
    else                  level = 1;   // weak (connected but marginal)
    // colour: red = no WiFi, amber = connected but feed stale (no fresh data), white = healthy
    const lv_color_t col = !wifiUp ? UI_EMERG : (feedOk ? UI_INK : lv_color_hex(0xFFB23C));
    for (int i = 0; i < 4; ++i) {
        if (!s_hudBars[i]) continue;
        lv_obj_set_style_bg_color(s_hudBars[i], col, 0);
        lv_obj_set_style_bg_opa(s_hudBars[i], (i < level) ? LV_OPA_COVER : 45, 0);
    }
    if (s_hudClock && clock) lv_label_set_text(s_hudClock, clock);
}

void ui_set_battery(int pct, bool charging, bool present) {
    s_battPct = pct;
    s_battCharging = charging;
    s_battPresent = present;
    if (!s_hudBatt) return;
    if (!present || pct < 0) { lv_label_set_text(s_hudBatt, ""); return; }   // USB-only -> hide
    const char *sym = pct > 80 ? LV_SYMBOL_BATTERY_FULL :
                      pct > 55 ? LV_SYMBOL_BATTERY_3 :
                      pct > 35 ? LV_SYMBOL_BATTERY_2 :
                      pct > 12 ? LV_SYMBOL_BATTERY_1 : LV_SYMBOL_BATTERY_EMPTY;
    char buf[24];
    snprintf(buf, sizeof(buf), "%s%s%d", charging ? LV_SYMBOL_CHARGE : "", sym, pct);
    lv_label_set_text(s_hudBatt, buf);
    lv_obj_set_style_text_color(s_hudBatt, (pct <= 15 && !charging) ? UI_EMERG : UI_INK, 0);
}

void ui_set_date(const char *date) {
    if (s_hudDate && date) lv_label_set_text(s_hudDate, date);
}

void ui_set_netinfo(const char *line) {
    if (s_statsNet && line) lv_label_set_text(s_statsNet, line);
}

void ui_set_weather_refresh_cb(void (*cb)(void)) { s_weatherRefreshCb = cb; }

static void update_weather_refresh_label(void) {
    if (!s_weatherRefreshLabel) return;
    char txt[32] = "Last Refresh: --:--";
    const time_t now = time(nullptr);
    if (now > 0) {
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(txt, sizeof(txt), "Last Refresh: %H:%M", &tmv);
    }
    lv_label_set_text(s_weatherRefreshLabel, txt);
}

static void request_weather_refresh(void) {
    s_weatherAutoPending = false;
    if (s_weatherRefreshCb) s_weatherRefreshCb();
    update_weather_refresh_label();
    ui_weather_updated();
}

static void weather_auto_refresh_cb(lv_timer_t *timer) {
    if (s_weatherAutoTimer == timer) s_weatherAutoTimer = nullptr;
    lv_timer_del(timer);
    if (!s_tv || lv_tileview_get_tile_act(s_tv) != s_tileWeather) return;
    request_weather_refresh();
}

static void cancel_weather_auto_refresh(void) {
    s_weatherAutoPending = false;
    if (!s_weatherAutoTimer) return;
    lv_timer_del(s_weatherAutoTimer);
    s_weatherAutoTimer = nullptr;
}

static void schedule_weather_auto_refresh(void) {
    cancel_weather_auto_refresh();
    s_weatherAutoPending = true;
    if (s_weatherLoading) {
        lv_label_set_text(s_weatherLoading, "Weather refresh pending...");
        lv_obj_clear_flag(s_weatherLoading, LV_OBJ_FLAG_HIDDEN);
    }
    s_weatherAutoTimer = lv_timer_create(weather_auto_refresh_cb, WEATHER_AUTO_REFRESH_DELAY_MS, nullptr);
}

static void weather_refresh_cb(lv_event_t *) {
    cancel_weather_auto_refresh();
    request_weather_refresh();
}

static void weather_airports_draw_cb(lv_event_t *e) {
    airports_draw_weather(lv_event_get_draw_ctx(e), UI_AIRPORT, 150);
}

void ui_weather_updated(void) {
    if (!s_weatherCanvas || !s_weatherLabel) return;
    int w = 0, h = 0, maxW = 0, maxH = 0;
    char label[48] = "";
    char status[72] = "";
    const bool ready = weather_get(&w, &h, label, sizeof(label), status, sizeof(status));
    lv_label_set_text(s_weatherLabel, label[0] ? label : (status[0] ? status : "Global weather radar"));
    if (ready && w > 0 && h > 0) {
        lv_color_t *buf = weather_buffer(&maxW, &maxH);
        lv_canvas_set_buffer(s_weatherCanvas, buf, w, h, LV_IMG_CF_TRUE_COLOR);
        lv_obj_clear_flag(s_weatherCanvas, LV_OBJ_FLAG_HIDDEN);
        if (s_weatherLoading) lv_obj_add_flag(s_weatherLoading, LV_OBJ_FLAG_HIDDEN);
        if (s_weatherAirportLayer) {
            WeatherView view;
            if (weather_view(&view)) {
                const float displayPx = (float)((w < h) ? w : h);
                airports_project_weather(view.lat, view.lon, view.zoom,
                                         SCREEN_CX, SCREEN_CY, displayPx, 512.0f);
                lv_obj_clear_flag(s_weatherAirportLayer, LV_OBJ_FLAG_HIDDEN);
                lv_obj_invalidate(s_weatherAirportLayer);
            } else {
                lv_obj_add_flag(s_weatherAirportLayer, LV_OBJ_FLAG_HIDDEN);
            }
        }
    } else {
        lv_obj_add_flag(s_weatherCanvas, LV_OBJ_FLAG_HIDDEN);
        if (s_weatherAirportLayer) lv_obj_add_flag(s_weatherAirportLayer, LV_OBJ_FLAG_HIDDEN);
        if (s_weatherLoading) {
            lv_label_set_text(s_weatherLoading,
                              s_weatherAutoPending ? "Weather refresh pending..." :
                              (status[0] ? status : "Loading weather radar..."));
            lv_obj_clear_flag(s_weatherLoading, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// Rebuild the scrollable contact list. Costly (deletes+recreates LVGL buttons), so we
// only call it when the list tile is actually visible — not on every 2 s poll.
static void build_list(void) {
    if (!s_list) return;
    lv_obj_clean(s_list);
    const int n = radar::count();
    for (int i = 0; i < n; ++i) {
        AcInfo in;
        radar::info(i, in);
        char altS[16], txt[64];
        fmt_alt(altS, sizeof(altS), in.altFt, in.onGround);
        snprintf(txt, sizeof(txt), "%-8.8s  %-8s %4.1f %s",
                 in.call[0] ? in.call : in.hex, altS, dist_val(in.distKm), dist_unit());
        lv_obj_t *b = lv_list_add_btn(s_list, NULL, txt);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(b, in.emergency ? UI_EMERG : UI_SOFT, 0);
        lv_obj_set_style_text_font(b, &lv_font_montserrat_16, 0);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, list_btn_cb, LV_EVENT_CLICKED, NULL);
    }
}

static void build_stats(void) {
    if (!s_statsLbl) return;
    const int n = radar::count();
    const int inRange = radar::countInRange();
    const int emg = radar::emergencyCountInRange();
    const int mil = radar::militaryCountInRange();
    float nearest = 1e9f, highest = -1e9f, fastest = -1.0f;
    char nearestCall[12] = "-";
    char highestCall[12] = "-";
    char fastestCall[12] = "-";
    int airborne = 0, ground = 0;
    for (int i = 0; i < n; ++i) {
        AcInfo in;
        radar::info(i, in);
        if (in.onGround) ground++;
        else airborne++;
        if (in.distKm < nearest) { nearest = in.distKm; snprintf(nearestCall, sizeof(nearestCall), "%s", in.call[0] ? in.call : in.hex); }
        if (!in.onGround && in.altFt > highest) {
            highest = in.altFt;
            snprintf(highestCall, sizeof(highestCall), "%s", in.call[0] ? in.call : in.hex);
        }
        if (in.gsKt == in.gsKt && in.gsKt > fastest) {
            fastest = in.gsKt;
            snprintf(fastestCall, sizeof(fastestCall), "%s", in.call[0] ? in.call : in.hex);
        }
    }
    char altH[16], fastS[16], battS[32], wifiS[32];
    if (highest > -1e8f) fmt_alt(altH, sizeof(altH), highest, false);
    else snprintf(altH, sizeof(altH), "-");
    if (fastest >= 0.0f) fmt_spd(fastS, sizeof(fastS), fastest);
    else snprintf(fastS, sizeof(fastS), "-");
    if (!s_battPresent || s_battPct < 0) snprintf(battS, sizeof(battS), "USB / no battery");
    else snprintf(battS, sizeof(battS), "%d%% %s", s_battPct, s_battCharging ? "charging" : "battery");
    if (!s_wifiUp) snprintf(wifiS, sizeof(wifiS), "WiFi down");
    else snprintf(wifiS, sizeof(wifiS), "WiFi %d dBm", s_rssi);

    char st[420];
    snprintf(st, sizeof(st),
             "SCOPE\n"
             "Range %.0f %s   In range %d\n"
             "Shown %d   Air %d   Gnd %d\n\n"
             "ALERTS\n"
             "Emergency %d   Military %d\n\n"
             "EXTREMES\n"
             "Nearest %s   %.1f %s\n"
             "Highest %s   %s\n"
             "Fastest %s   %s\n\n"
             "SYSTEM\n"
             "%s   Feed %s\n"
             "%s",
             dist_val(s_rangeKm), dist_unit(), inRange,
             n, airborne, ground,
             emg, mil,
             n ? nearestCall : "-", dist_val(n ? nearest : 0.0f), dist_unit(),
             (highest > -1e8f) ? highestCall : "-", altH,
             (fastest >= 0.0f) ? fastestCall : "-", fastS,
             wifiS, s_feedOk ? "OK" : "stale",
             battS);
    lv_label_set_text(s_statsLbl, st);
}

// Rebuild whichever secondary tile is currently on screen (called on poll and on swipe).
static void refresh_active_tile(void) {
    if (!s_tv) return;
    lv_obj_t *act = lv_tileview_get_tile_act(s_tv);
    if (act == s_tileList)  build_list();
    else if (act == s_tileStats) build_stats();
    else if (act == s_tileWeather) ui_weather_updated();
}

static void tile_changed_cb(lv_event_t *) {
    if (!s_tv) return;
    lv_obj_t *act = lv_tileview_get_tile_act(s_tv);
    const bool enteredWeather = (act == s_tileWeather && s_lastTile != s_tileWeather);
    const bool leftWeather = (act != s_tileWeather && s_lastTile == s_tileWeather);
    s_lastTile = act;
    if (enteredWeather) schedule_weather_auto_refresh();
    else if (leftWeather) cancel_weather_auto_refresh();
    refresh_active_tile();
}

void ui_on_data_updated(void) {
    refresh_card();
    if (s_hudCount) {
        const int emr = radar::emergencyCountInRange();
        const int mil = radar::militaryCountInRange();
        const int alerts = radar::alertCountInRange();
        if (alerts > 0) {
            char cbuf[16];
            const lv_color_t col = emr > 0 ? UI_EMERG : UI_MIL;
            if (emr > 0 && mil > 0) snprintf(cbuf, sizeof(cbuf), "ALR %d", alerts);
            else if (emr > 0)      snprintf(cbuf, sizeof(cbuf), "EMR %d", emr);
            else                   snprintf(cbuf, sizeof(cbuf), "MIL %d", mil);
            lv_label_set_text(s_hudCount, cbuf);
            lv_obj_set_style_text_color(s_hudCount, col, 0);
            lv_obj_set_style_border_color(s_hudCount, col, 0);
            lv_obj_clear_flag(s_hudCount, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(s_hudCount, "");
            lv_obj_add_flag(s_hudCount, LV_OBJ_FLAG_HIDDEN);
        }
    }
    refresh_active_tile();   // only the visible tile pays the rebuild cost
}

// ------------------------------------------------------------------- building
static lv_obj_t *make_tile_title(lv_obj_t *tile, const char *txt) {
    lv_obj_t *l = lv_label_create(tile);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, UI_GREEN, 0);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 22);
    return l;
}

// A full-screen round panel that clips its content to the circle (for list/stats views).
static lv_obj_t *make_round_panel(lv_obj_t *parent) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, 462, 462);
    lv_obj_center(p);
    lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x05100A), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, UI_GREEN, 0);
    lv_obj_set_style_border_opa(p, 50, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_clip_corner(p, true, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static void build_card(void) {
    s_card = lv_obj_create(s_tileRadar);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, 300, 118);
    lv_obj_align(s_card, LV_ALIGN_CENTER, 0, 66);
    lv_obj_set_style_bg_color(s_card, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(s_card, 235, 0);
    lv_obj_set_style_radius(s_card, 14, 0);
    lv_obj_set_style_border_color(s_card, UI_GREEN, 0);
    lv_obj_set_style_border_opa(s_card, 90, 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_pad_all(s_card, 12, 0);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);   // consume taps (don't deselect)
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    s_cardTitle = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_cardTitle, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_cardTitle, UI_INK, 0);
    lv_obj_align(s_cardTitle, LV_ALIGN_TOP_LEFT, 0, 0);

    s_cardL = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_cardL, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_cardL, UI_SOFT, 0);
    lv_obj_align(s_cardL, LV_ALIGN_TOP_LEFT, 0, 26);

    s_cardR = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_cardR, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_cardR, UI_SOFT, 0);
    lv_obj_align(s_cardR, LV_ALIGN_TOP_LEFT, 150, 26);

    s_cardRoute = lv_label_create(s_card);
    lv_obj_set_style_text_font(s_cardRoute, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_cardRoute, UI_GREEN, 0);
    lv_obj_align(s_cardRoute, LV_ALIGN_TOP_LEFT, 0, 76);

    // aircraft photo + credit, floating above the card (hidden until one loads)
    s_photo = lv_canvas_create(s_tileRadar);
    lv_obj_set_style_radius(s_photo, 6, 0);
    lv_obj_set_style_clip_corner(s_photo, true, 0);
    lv_obj_set_style_border_color(s_photo, UI_GREEN, 0);
    lv_obj_set_style_border_opa(s_photo, 170, 0);
    lv_obj_set_style_border_width(s_photo, 1, 0);
    lv_obj_add_flag(s_photo, LV_OBJ_FLAG_HIDDEN);

    s_photoCredit = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_photoCredit, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_photoCredit, UI_DIM, 0);
    lv_label_set_text(s_photoCredit, "");
    lv_obj_add_flag(s_photoCredit, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_view(int idx) {
    if (s_tv && idx >= 0 && idx <= 3) lv_obj_set_tile_id(s_tv, (uint32_t)idx, 0, LV_ANIM_OFF);
}

// ------------------------------------------------------------------- splash
static lv_obj_t *s_splash = nullptr;
static void splash_fade_cb(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }
static void splash_del_cb(lv_anim_t *a) {
    lv_obj_t *obj = (lv_obj_t *)a->var;
    if (obj) lv_obj_del(obj);
    if (s_splash == obj) s_splash = nullptr;
}

void ui_splash_hide(void) {
    if (!s_splash) return;
    lv_obj_t *cont = s_splash;
    s_splash = nullptr;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, cont);
    lv_anim_set_exec_cb(&a, splash_fade_cb);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_time(&a, 600);
    lv_anim_set_ready_cb(&a, splash_del_cb);
    lv_anim_start(&a);
}

void ui_splash_show(void) {
    if (s_splash) return;
    lv_obj_t *cont = lv_obj_create(lv_layer_top());
    s_splash = cont;
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, SCREEN_W, SCREEN_H);
    lv_obj_center(cont);
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    // concentric rings
    const lv_coord_t dia[3] = { 210, 142, 78 };
    const lv_opa_t   op[3]  = { 90, 120, 160 };
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *r = lv_obj_create(cont);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, dia[i], dia[i]);
        lv_obj_align(r, LV_ALIGN_CENTER, 0, -8);
        lv_obj_set_style_radius(r, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_color(r, UI_GREEN, 0);
        lv_obj_set_style_border_opa(r, op[i], 0);
        lv_obj_set_style_border_width(r, 2, 0);
        lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    }
    // rotating sweep
    lv_obj_t *sweep = lv_spinner_create(cont, 1400, 55);
    lv_obj_set_size(sweep, 210, 210);
    lv_obj_align(sweep, LV_ALIGN_CENTER, 0, -8);
    lv_obj_set_style_arc_opa(sweep, 0, LV_PART_MAIN);
    lv_obj_set_style_arc_color(sweep, UI_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(sweep, 4, LV_PART_INDICATOR);

    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text(title, "CAPSULE RADAR");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, UI_GREEN, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 118);

    lv_obj_t *sub = lv_label_create(cont);
    lv_label_set_text(sub, "Live ADS-B radar");
    lv_obj_set_style_text_font(sub, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(sub, UI_SOFT, 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 150);
}

void ui_create(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    ui_splash_show();   // show as soon as the LVGL screen exists; build the radar UI behind it

    s_tv = lv_tileview_create(scr);
    lv_obj_set_size(s_tv, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_tv, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);

    s_tileRadar = lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT);
    s_tileWeather = lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_HOR);
    s_tileList  = lv_tileview_add_tile(s_tv, 2, 0, LV_DIR_HOR);
    s_tileStats = lv_tileview_add_tile(s_tv, 3, 0, LV_DIR_LEFT);
    // Rebuild the secondary views with the latest data the moment they slide into view
    // (between polls they'd otherwise show whatever was there when last visible).
    lv_obj_add_event_cb(s_tv, tile_changed_cb, LV_EVENT_VALUE_CHANGED, nullptr);

    // --- radar tile ---
    lv_obj_clear_flag(s_tileRadar, LV_OBJ_FLAG_SCROLLABLE);
    radar::init(s_tileRadar);
    radar::setRangeLabelVisible(false);                     // the zoom button shows the range instead
    lv_obj_add_flag(s_tileRadar, LV_OBJ_FLAG_CLICKABLE);     // receive taps (planes/empty)
    lv_obj_add_event_cb(s_tileRadar, radar_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_pressing_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_release_cb, LV_EVENT_RELEASED, NULL);
    build_card();

    // on-screen range button: tap selects range, long-press cycles theme.
    s_zoomBtn = lv_btn_create(s_tileRadar);
    lv_obj_set_size(s_zoomBtn, 104, 36);
    lv_obj_align(s_zoomBtn, LV_ALIGN_BOTTOM_MID, 0, -66);
    lv_obj_set_style_radius(s_zoomBtn, 18, 0);
    lv_obj_set_style_bg_color(s_zoomBtn, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(s_zoomBtn, 225, 0);
    lv_obj_set_style_border_color(s_zoomBtn, UI_GREEN, 0);
    lv_obj_set_style_border_width(s_zoomBtn, 1, 0);
    lv_obj_set_style_border_opa(s_zoomBtn, 170, 0);
    lv_obj_clear_flag(s_zoomBtn, LV_OBJ_FLAG_SCROLL_CHAIN);  // tapping it must not swipe the tileview
    lv_obj_add_event_cb(s_zoomBtn, zoom_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_zoomBtn, zoom_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
    s_zoomLbl = lv_label_create(s_zoomBtn);
    lv_label_set_text(s_zoomLbl, LV_SYMBOL_LOOP " 30 km");
    lv_obj_set_style_text_font(s_zoomLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_zoomLbl, UI_GREEN, 0);
    lv_obj_center(s_zoomLbl);

    // top status HUD (wifi / aircraft count / clock); white reads on both themes.
    // WiFi is a 4-bar signal meter: bar count = RSSI strength, colour = feed health.
    s_hudWifi = lv_obj_create(s_tileRadar);
    lv_obj_remove_style_all(s_hudWifi);
    lv_obj_set_size(s_hudWifi, 21, 14);
    lv_obj_align(s_hudWifi, LV_ALIGN_TOP_MID, -72, 52);
    lv_obj_clear_flag(s_hudWifi, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 4; ++i) {
        s_hudBars[i] = lv_obj_create(s_hudWifi);
        lv_obj_remove_style_all(s_hudBars[i]);
        lv_obj_set_size(s_hudBars[i], 3, (lv_coord_t)(4 + i * 3));   // 4, 7, 10, 13 px tall
        lv_obj_align(s_hudBars[i], LV_ALIGN_BOTTOM_LEFT, (lv_coord_t)(i * 5), 0);
        lv_obj_set_style_radius(s_hudBars[i], 1, 0);
        lv_obj_set_style_bg_color(s_hudBars[i], UI_INK, 0);
        lv_obj_set_style_bg_opa(s_hudBars[i], LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_hudBars[i], LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    }

    s_hudCount = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudCount, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_hudCount, UI_EMERG, 0);
    lv_obj_set_style_bg_color(s_hudCount, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_hudCount, 180, 0);
    lv_obj_set_style_border_color(s_hudCount, UI_EMERG, 0);
    lv_obj_set_style_border_width(s_hudCount, 1, 0);
    lv_obj_set_style_border_opa(s_hudCount, 210, 0);
    lv_obj_set_style_radius(s_hudCount, 5, 0);
    lv_obj_set_style_pad_left(s_hudCount, 6, 0);
    lv_obj_set_style_pad_right(s_hudCount, 6, 0);
    lv_obj_set_style_pad_top(s_hudCount, 2, 0);
    lv_obj_set_style_pad_bottom(s_hudCount, 2, 0);
    lv_label_set_text(s_hudCount, "");
    lv_obj_add_flag(s_hudCount, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_hudCount, LV_ALIGN_TOP_MID, 0, 82);

    s_hudClock = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudClock, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hudClock, UI_INK, 0);
    lv_label_set_text(s_hudClock, "--:--");
    lv_obj_align(s_hudClock, LV_ALIGN_TOP_MID, 0, 44);

    s_hudBatt = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudBatt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hudBatt, UI_INK, 0);
    lv_label_set_text(s_hudBatt, "");
    lv_obj_align(s_hudBatt, LV_ALIGN_TOP_MID, 72, 52);

    s_hudDate = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudDate, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hudDate, UI_INK, 0);
    lv_label_set_text(s_hudDate, "");
    lv_obj_align(s_hudDate, LV_ALIGN_TOP_MID, 0, 62);

    s_helpHotspot = lv_obj_create(s_tileRadar);
    lv_obj_remove_style_all(s_helpHotspot);
    lv_obj_set_size(s_helpHotspot, 190, 74);
    lv_obj_align(s_helpHotspot, LV_ALIGN_TOP_MID, 0, 24);
    lv_obj_add_flag(s_helpHotspot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_helpHotspot, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_add_event_cb(s_helpHotspot, help_hotspot_longpress_cb, LV_EVENT_LONG_PRESSED, nullptr);

    // --- list tile (circular panel, clipped to the round screen) ---
    lv_obj_t *lp = make_round_panel(s_tileList);
    make_tile_title(lp, "AIRCRAFT");
    s_list = lv_list_create(lp);
    lv_obj_set_size(s_list, 300, 372);
    lv_obj_align(s_list, LV_ALIGN_CENTER, 0, 22);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_row(s_list, 2, 0);

    // --- stats tile (circular panel) ---
    lv_obj_t *sp = make_round_panel(s_tileStats);
    make_tile_title(sp, "STATS");
    s_statsLbl = lv_label_create(sp);
    lv_obj_set_width(s_statsLbl, 330);
    lv_label_set_long_mode(s_statsLbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_statsLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_statsLbl, UI_SOFT, 0);
    lv_obj_set_style_text_align(s_statsLbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_statsLbl, "SCOPE\nNo data yet");
    lv_obj_align(s_statsLbl, LV_ALIGN_CENTER, 0, -10);

    // footer: where to reach the configuration page (IP / hostname / setup AP)
    s_statsNet = lv_label_create(sp);
    lv_obj_set_width(s_statsNet, 320);
    lv_obj_set_style_text_font(s_statsNet, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_statsNet, UI_GREEN, 0);
    lv_obj_set_style_text_align(s_statsNet, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_statsNet, "");
    lv_obj_align(s_statsNet, LV_ALIGN_CENTER, 0, 132);

    // --- weather tile (static global radar image, manually refreshed) ---
    lv_obj_t *wp = lv_obj_create(s_tileWeather);
    lv_obj_remove_style_all(wp);
    lv_obj_set_size(wp, SCREEN_W, SCREEN_H);
    lv_obj_center(wp);
    lv_obj_set_style_radius(wp, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(wp, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wp, LV_OPA_COVER, 0);
    lv_obj_set_style_clip_corner(wp, true, 0);
    lv_obj_clear_flag(wp, LV_OBJ_FLAG_SCROLLABLE);

    s_weatherCanvas = lv_canvas_create(wp);
    lv_obj_set_size(s_weatherCanvas, SCREEN_W, SCREEN_H);
    lv_obj_center(s_weatherCanvas);
    lv_obj_set_style_radius(s_weatherCanvas, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(s_weatherCanvas, true, 0);
    lv_obj_set_style_border_width(s_weatherCanvas, 0, 0);
    lv_obj_add_flag(s_weatherCanvas, LV_OBJ_FLAG_HIDDEN);

    s_weatherAirportLayer = lv_obj_create(wp);
    lv_obj_remove_style_all(s_weatherAirportLayer);
    lv_obj_set_size(s_weatherAirportLayer, SCREEN_W, SCREEN_H);
    lv_obj_center(s_weatherAirportLayer);
    lv_obj_clear_flag(s_weatherAirportLayer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_weatherAirportLayer, weather_airports_draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
    lv_obj_add_flag(s_weatherAirportLayer, LV_OBJ_FLAG_HIDDEN);

    s_weatherLabel = lv_label_create(wp);
    lv_obj_set_width(s_weatherLabel, 350);
    lv_obj_set_style_text_font(s_weatherLabel, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_weatherLabel, UI_INK, 0);
    lv_obj_set_style_text_align(s_weatherLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_weatherLabel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_weatherLabel, 180, 0);
    lv_obj_set_style_radius(s_weatherLabel, 6, 0);
    lv_obj_set_style_pad_top(s_weatherLabel, 3, 0);
    lv_obj_set_style_pad_bottom(s_weatherLabel, 3, 0);
    lv_label_set_text(s_weatherLabel, "Global weather radar");
    lv_obj_align(s_weatherLabel, LV_ALIGN_TOP_MID, 0, 48);

    s_weatherLoading = lv_label_create(wp);
    lv_obj_set_width(s_weatherLoading, 300);
    lv_obj_set_style_text_font(s_weatherLoading, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_weatherLoading, UI_GREEN, 0);
    lv_obj_set_style_text_align(s_weatherLoading, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_weatherLoading, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_weatherLoading, 185, 0);
    lv_obj_set_style_radius(s_weatherLoading, 8, 0);
    lv_obj_set_style_pad_all(s_weatherLoading, 8, 0);
    lv_label_set_text(s_weatherLoading, "Tap Refresh");
    lv_obj_align(s_weatherLoading, LV_ALIGN_CENTER, 0, -2);

    s_weatherRefreshLabel = lv_label_create(wp);
    lv_obj_set_width(s_weatherRefreshLabel, 260);
    lv_obj_set_style_text_font(s_weatherRefreshLabel, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_weatherRefreshLabel, UI_INK, 0);
    lv_obj_set_style_text_align(s_weatherRefreshLabel, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_color(s_weatherRefreshLabel, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_weatherRefreshLabel, 150, 0);
    lv_obj_set_style_radius(s_weatherRefreshLabel, 5, 0);
    lv_obj_set_style_pad_top(s_weatherRefreshLabel, 2, 0);
    lv_obj_set_style_pad_bottom(s_weatherRefreshLabel, 2, 0);
    lv_label_set_text(s_weatherRefreshLabel, "Last Refresh: --:--");
    lv_obj_align(s_weatherRefreshLabel, LV_ALIGN_BOTTOM_MID, 0, -72);

    s_weatherRefreshBtn = lv_btn_create(wp);
    lv_obj_set_size(s_weatherRefreshBtn, 116, 34);
    lv_obj_align(s_weatherRefreshBtn, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_set_style_radius(s_weatherRefreshBtn, 17, 0);
    lv_obj_set_style_bg_color(s_weatherRefreshBtn, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(s_weatherRefreshBtn, 225, 0);
    lv_obj_set_style_border_color(s_weatherRefreshBtn, UI_GREEN, 0);
    lv_obj_set_style_border_width(s_weatherRefreshBtn, 1, 0);
    lv_obj_set_style_border_opa(s_weatherRefreshBtn, 170, 0);
    lv_obj_add_event_cb(s_weatherRefreshBtn, weather_refresh_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *wr = lv_label_create(s_weatherRefreshBtn);
    lv_label_set_text(wr, LV_SYMBOL_LOOP " Refresh");
    lv_obj_set_style_text_font(wr, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(wr, UI_GREEN, 0);
    lv_obj_center(wr);

    lv_obj_set_tile_id(s_tv, 0, 0, LV_ANIM_OFF);
}
