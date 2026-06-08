// M3 UI: tileview (radar / list / stats) + tap-to-inspect detail card.
// Pure LVGL, portable. Taps hit-test via radar::hitTest; selection lives in radar.
#include "ui.h"
#include "radar_view.h"
#include "route.h"
#include "config.h"
#include <lvgl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define UI_GREEN lv_color_hex(0x1DFF86)
#define UI_INK   lv_color_hex(0xEAFFF3)
#define UI_SOFT  lv_color_hex(0x9AFFC8)
#define UI_DIM   lv_color_hex(0x5F7A6C)
#define UI_PANEL lv_color_hex(0x0C160F)
#define UI_EMERG lv_color_hex(0xFF5A3C)

static lv_obj_t *s_tv = nullptr;
static lv_obj_t *s_tileRadar = nullptr, *s_tileList = nullptr, *s_tileStats = nullptr;
static lv_obj_t *s_card = nullptr, *s_cardTitle = nullptr, *s_cardL = nullptr, *s_cardR = nullptr;
static lv_obj_t *s_cardRoute = nullptr;
static char s_lastRouteReq[12] = "";
static lv_obj_t *s_hudWifi = nullptr, *s_hudCount = nullptr, *s_hudClock = nullptr, *s_hudBatt = nullptr, *s_hudDate = nullptr;
static lv_obj_t *s_list = nullptr;
static lv_obj_t *s_statsLbl = nullptr;
static lv_obj_t *s_statsNet = nullptr;

// ----------------------------------------------------------------- detail card
static void refresh_card(void) {
    AcInfo in;
    if (!radar::selected(in)) {
        lv_obj_add_flag(s_card, LV_OBJ_FLAG_HIDDEN);
        s_lastRouteReq[0] = 0;
        return;
    }
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_HIDDEN);

    char title[40];
    if (in.type[0]) snprintf(title, sizeof(title), "%s  %s", in.call[0] ? in.call : "-", in.type);
    else            snprintf(title, sizeof(title), "%s", in.call[0] ? in.call : "-");
    lv_label_set_text(s_cardTitle, title);
    lv_obj_set_style_text_color(s_cardTitle, in.emergency ? UI_EMERG : UI_INK, 0);

    char altS[16], vsS[16], spdS[16], sqS[16];
    if (in.onGround)            snprintf(altS, sizeof(altS), "GND");
    else                        snprintf(altS, sizeof(altS), "%.0f ft", in.altFt);
    if (in.vsFpm != in.vsFpm)   snprintf(vsS, sizeof(vsS), "-");
    else                        snprintf(vsS, sizeof(vsS), "%+.0f", in.vsFpm);
    if (in.gsKt != in.gsKt)     snprintf(spdS, sizeof(spdS), "-");
    else                        snprintf(spdS, sizeof(spdS), "%.0f kt", in.gsKt);
    if (in.squawk < 0)          snprintf(sqS, sizeof(sqS), "-");
    else                        snprintf(sqS, sizeof(sqS), "%04d", in.squawk);

    char left[96], right[96];
    snprintf(left,  sizeof(left),  "ALT  %s\nSPD  %s\nDIST %.1f km", altS, spdS, in.distKm);
    snprintf(right, sizeof(right), "V/S  %s\nHDG  %03.0f\nSQK  %s", vsS, in.bearingDeg, sqS);
    lv_label_set_text(s_cardL, left);
    lv_label_set_text(s_cardR, right);

    // route (origin -> destination), looked up asynchronously by callsign
    if (in.call[0] && strcmp(in.call, s_lastRouteReq) != 0) {
        snprintf(s_lastRouteReq, sizeof(s_lastRouteReq), "%s", in.call);
        route_request(in.call);
    }
    char rfrom[40], rto[40];
    if (in.call[0] && route_get(in.call, rfrom, sizeof(rfrom), rto, sizeof(rto))) {
        char rt[96];
        if (rfrom[0] || rto[0]) snprintf(rt, sizeof(rt), "%s -> %s", rfrom[0] ? rfrom : "?", rto[0] ? rto : "?");
        else                    snprintf(rt, sizeof(rt), "route n/a");
        lv_label_set_text(s_cardRoute, rt);
    } else {
        lv_label_set_text(s_cardRoute, "...");
    }
}

// --------------------------------------------------------------------- input
static bool s_longPressed = false;
static int s_rangeIdx = -1;
static void (*s_rangeCb)(float) = nullptr;
static lv_obj_t *s_zoomBtn = nullptr, *s_zoomLbl = nullptr;

void ui_set_range_cb(void (*cb)(float)) { s_rangeCb = cb; }

static void zoom_cb(lv_event_t *e) {   // on-screen range control: reliable single tap
    (void)e;
    if (!s_rangeCb) return;
    const int n = (int)(sizeof(RANGE_STEPS_KM) / sizeof(RANGE_STEPS_KM[0]));
    s_rangeIdx = (s_rangeIdx + 1) % n;
    s_rangeCb(RANGE_STEPS_KM[s_rangeIdx]);
}

void ui_set_range_km(float km) {
    if (s_zoomLbl) {
        char b[16];
        snprintf(b, sizeof(b), LV_SYMBOL_LOOP " %.0f km", (double)km);
        lv_label_set_text(s_zoomLbl, b);
    }
    int best = 0; float bd = 1e9f;                 // sync the cycle index to the shown range
    const int n = (int)(sizeof(RANGE_STEPS_KM) / sizeof(RANGE_STEPS_KM[0]));
    for (int i = 0; i < n; ++i) { float d = km - RANGE_STEPS_KM[i]; if (d < 0) d = -d; if (d < bd) { bd = d; best = i; } }
    s_rangeIdx = best;
}

static void radar_press_cb(lv_event_t *e) { (void)e; s_longPressed = false; }

static void radar_longpress_cb(lv_event_t *e) {   // long-press cycles the visual theme
    (void)e;
    radar::cycleTheme();
    s_longPressed = true;
}

static void radar_clicked_cb(lv_event_t *e) {
    (void)e;
    if (s_longPressed) { s_longPressed = false; return; }   // ignore the click after a long-press
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    radar::select(radar::hitTest(p.x, p.y));   // hit -> select; miss -> clear
    refresh_card();
}

static void list_btn_cb(lv_event_t *e) {
    lv_obj_t *b = lv_event_get_target(e);
    const int idx = (int)(intptr_t)lv_obj_get_user_data(b);
    radar::select(idx);
    refresh_card();
    lv_obj_set_tile_id(s_tv, 0, 0, LV_ANIM_ON);   // jump back to the radar
}

// ----------------------------------------------------------------- list/stats
void ui_set_status(bool wifiUp, bool feedOk, const char *clock) {
    if (s_hudWifi) {
        const lv_color_t c = !wifiUp ? UI_EMERG : (feedOk ? UI_INK : lv_color_hex(0xFFB23C));
        lv_obj_set_style_text_color(s_hudWifi, c, 0);   // red = no wifi, amber = feed failing
    }
    if (s_hudClock && clock) lv_label_set_text(s_hudClock, clock);
}

void ui_set_battery(int pct, bool charging, bool present) {
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

void ui_on_data_updated(void) {
    refresh_card();
    if (s_hudCount) {
        char cbuf[8];
        snprintf(cbuf, sizeof(cbuf), "%d", radar::countInRange());
        lv_label_set_text(s_hudCount, cbuf);
    }

    // list view
    lv_obj_clean(s_list);
    const int n = radar::count();
    for (int i = 0; i < n; ++i) {
        AcInfo in;
        radar::info(i, in);
        char txt[56];
        snprintf(txt, sizeof(txt), "%-8s  %5.0f ft  %4.1f km",
                 in.call[0] ? in.call : in.hex, in.onGround ? 0.0f : in.altFt, in.distKm);
        lv_obj_t *b = lv_list_add_btn(s_list, NULL, txt);
        lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(b, in.emergency ? UI_EMERG : UI_SOFT, 0);
        lv_obj_set_style_text_font(b, &lv_font_montserrat_16, 0);
        lv_obj_set_user_data(b, (void *)(intptr_t)i);
        lv_obj_add_event_cb(b, list_btn_cb, LV_EVENT_CLICKED, NULL);
    }

    // stats view
    int emg = 0;
    float nearest = 1e9f, highest = -1e9f;
    char nearestCall[12] = "-";
    for (int i = 0; i < n; ++i) {
        AcInfo in;
        radar::info(i, in);
        if (in.emergency) emg++;
        if (in.distKm < nearest) { nearest = in.distKm; snprintf(nearestCall, sizeof(nearestCall), "%s", in.call[0] ? in.call : in.hex); }
        if (!in.onGround && in.altFt > highest) highest = in.altFt;
    }
    char st[200];
    snprintf(st, sizeof(st),
             "Aircraft   %d\n"
             "Emergency  %d\n"
             "Nearest    %s\n"
             "           %.1f km\n"
             "Highest    %.0f ft\n"
             "Range      %.0f km",
             n, emg, n ? nearestCall : "-", n ? nearest : 0.0f,
             (highest > -1e8f) ? highest : 0.0f, (double)RANGE_KM_DEFAULT);
    lv_label_set_text(s_statsLbl, st);
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
}

void ui_show_view(int idx) {
    if (s_tv && idx >= 0 && idx <= 2) lv_obj_set_tile_id(s_tv, (uint32_t)idx, 0, LV_ANIM_OFF);
}

// ------------------------------------------------------------------- splash
static void splash_fade_cb(void *obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0); }
static void splash_del_cb(lv_anim_t *a) { lv_obj_del((lv_obj_t *)a->var); }

static void splash_dismiss_cb(lv_timer_t *t) {
    lv_obj_t *cont = (lv_obj_t *)t->user_data;
    lv_timer_del(t);
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
    lv_obj_t *cont = lv_obj_create(lv_layer_top());
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

    lv_timer_t *t = lv_timer_create(splash_dismiss_cb, 2200, cont);   // hold, then fade out
    lv_timer_set_repeat_count(t, 1);
}

void ui_create(void) {
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_tv = lv_tileview_create(scr);
    lv_obj_set_size(s_tv, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_tv, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);

    s_tileRadar = lv_tileview_add_tile(s_tv, 0, 0, LV_DIR_RIGHT);
    s_tileList  = lv_tileview_add_tile(s_tv, 1, 0, LV_DIR_HOR);
    s_tileStats = lv_tileview_add_tile(s_tv, 2, 0, LV_DIR_LEFT);

    // --- radar tile ---
    lv_obj_clear_flag(s_tileRadar, LV_OBJ_FLAG_SCROLLABLE);
    radar::init(s_tileRadar);
    radar::setRangeLabelVisible(false);                     // the zoom button shows the range instead
    lv_obj_add_flag(s_tileRadar, LV_OBJ_FLAG_CLICKABLE);     // receive taps (planes/empty)
    lv_obj_add_event_cb(s_tileRadar, radar_clicked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_tileRadar, radar_longpress_cb, LV_EVENT_LONG_PRESSED, NULL);
    build_card();

    // on-screen range/zoom button (reliable single tap; bottom, above the 'S' marker)
    s_zoomBtn = lv_btn_create(s_tileRadar);
    lv_obj_set_size(s_zoomBtn, 104, 36);
    lv_obj_align(s_zoomBtn, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_set_style_radius(s_zoomBtn, 18, 0);
    lv_obj_set_style_bg_color(s_zoomBtn, UI_PANEL, 0);
    lv_obj_set_style_bg_opa(s_zoomBtn, 225, 0);
    lv_obj_set_style_border_color(s_zoomBtn, UI_GREEN, 0);
    lv_obj_set_style_border_width(s_zoomBtn, 1, 0);
    lv_obj_set_style_border_opa(s_zoomBtn, 170, 0);
    lv_obj_clear_flag(s_zoomBtn, LV_OBJ_FLAG_SCROLL_CHAIN);  // tapping it must not swipe the tileview
    lv_obj_add_event_cb(s_zoomBtn, zoom_cb, LV_EVENT_CLICKED, NULL);
    s_zoomLbl = lv_label_create(s_zoomBtn);
    lv_label_set_text(s_zoomLbl, LV_SYMBOL_LOOP " 30 km");
    lv_obj_set_style_text_font(s_zoomLbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_zoomLbl, UI_GREEN, 0);
    lv_obj_center(s_zoomLbl);

    // top status HUD (wifi / aircraft count / clock); white reads on both themes
    s_hudWifi = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudWifi, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hudWifi, UI_INK, 0);
    lv_label_set_text(s_hudWifi, LV_SYMBOL_WIFI);
    lv_obj_align(s_hudWifi, LV_ALIGN_TOP_MID, -92, 50);

    s_hudCount = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudCount, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hudCount, UI_INK, 0);
    lv_label_set_text(s_hudCount, "0");
    lv_obj_align(s_hudCount, LV_ALIGN_TOP_MID, -34, 50);

    s_hudClock = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudClock, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hudClock, UI_INK, 0);
    lv_label_set_text(s_hudClock, "--:--");
    lv_obj_align(s_hudClock, LV_ALIGN_TOP_MID, 30, 50);

    s_hudBatt = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudBatt, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_hudBatt, UI_INK, 0);
    lv_label_set_text(s_hudBatt, "");
    lv_obj_align(s_hudBatt, LV_ALIGN_TOP_MID, 92, 50);

    s_hudDate = lv_label_create(s_tileRadar);
    lv_obj_set_style_text_font(s_hudDate, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(s_hudDate, UI_INK, 0);
    lv_obj_set_style_text_opa(s_hudDate, 140, 0);
    lv_label_set_text(s_hudDate, "");
    lv_obj_align(s_hudDate, LV_ALIGN_TOP_MID, 0, 70);

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
    lv_obj_set_style_text_font(s_statsLbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_statsLbl, UI_SOFT, 0);
    lv_label_set_text(s_statsLbl, "Aircraft   0");
    lv_obj_align(s_statsLbl, LV_ALIGN_CENTER, 0, -16);

    // footer: where to reach the configuration page (IP / hostname / setup AP)
    s_statsNet = lv_label_create(sp);
    lv_obj_set_width(s_statsNet, 320);
    lv_obj_set_style_text_font(s_statsNet, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_statsNet, UI_GREEN, 0);
    lv_obj_set_style_text_align(s_statsNet, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_statsNet, "");
    lv_obj_align(s_statsNet, LV_ALIGN_CENTER, 0, 132);

    lv_obj_set_tile_id(s_tv, 0, 0, LV_ANIM_OFF);

    ui_splash_show();   // branded boot splash on top (auto-fades)
}
