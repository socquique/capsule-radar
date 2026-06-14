// Native (Mac/Linux) LVGL simulator for Capsule Radar.
// Runs the SAME LVGL UI (include/lv_conf.h + ui_boot) in an SDL2 window — no
// hardware, no Arduino_GFX. Only compiled for the `native` PlatformIO env.
//
//   pio run -e native            # build
//   pio run -e native -t exec    # build + run (or run .pio/build/native/program)
//
// Note: the real panel is a 466x466 *round* AMOLED; this square window shows the
// full buffer, so the corners (hidden on the device) are visible here.
#include <SDL.h>
#include <lvgl.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "radar_view.h"
#include "ui.h"
#include "route.h"
#include "aircraft.h"
#include <vector>
#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SIM_W SCREEN_W   // 466
#define SIM_H SCREEN_H   // 466

static SDL_Window   *s_win = NULL;
static SDL_Renderer *s_ren = NULL;
static SDL_Texture  *s_tex = NULL;

// LVGL -> SDL texture. Accumulate dirty areas, present once per refresh.
static void sdl_flush(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *px) {
    const int w = area->x2 - area->x1 + 1;
    const int h = area->y2 - area->y1 + 1;
    SDL_Rect r = { area->x1, area->y1, w, h };
    SDL_UpdateTexture(s_tex, &r, px, w * (int)sizeof(lv_color_t));
    if (lv_disp_flush_is_last(drv)) {
        SDL_RenderClear(s_ren);
        SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
        SDL_RenderPresent(s_ren);
    }
    lv_disp_flush_ready(drv);
}

// Mouse acts as the touch input device (left button = press).
static void sdl_mouse_read(lv_indev_drv_t *drv, lv_indev_data_t *data) {
    (void)drv;
    int x, y;
    Uint32 btn = SDL_GetMouseState(&x, &y);
    data->point.x = x;
    data->point.y = y;
    data->state = (btn & SDL_BUTTON(SDL_BUTTON_LEFT)) ? LV_INDEV_STATE_PRESSED
                                                      : LV_INDEV_STATE_RELEASED;
}

// ---- mock ADS-B data (sim only): 6 aircraft near Dénia that drift along track --
static std::vector<Aircraft> g_mockAcs;
static std::vector<Aircraft> g_mockInit;
static RadarSettings g_set;

static Aircraft mk(const char *call, const char *hex, double distKm, double brgDeg,
                   float altFt, float track, float gsKt, int sq) {
    Aircraft a;
    snprintf(a.flight, sizeof(a.flight), "%s", call ? call : "");
    snprintf(a.hex, sizeof(a.hex), "%s", hex ? hex : "");
    const double br = brgDeg * M_PI / 180.0;
    const double latR = HOME_LAT_DEFAULT * M_PI / 180.0;
    a.lat = HOME_LAT_DEFAULT + (distKm * cos(br)) / 111.0;
    a.lon = HOME_LON_DEFAULT + (distKm * sin(br)) / (111.0 * cos(latR));
    a.altBaro = altFt;
    a.onGround = false;
    a.track = track;
    a.gs = gsKt;
    a.squawk = sq;
    return a;
}

static void sim_range_cb(float km) { g_set.rangeKm = km; radar::update(g_mockAcs, g_set); ui_set_range_km(km); }

static void mock_init() {
    g_set.homeLat = HOME_LAT_DEFAULT;
    g_set.homeLon = HOME_LON_DEFAULT;
    g_set.rangeKm = RANGE_KM_DEFAULT;
    g_set.rotationDeg = 0.0;
    g_mockAcs.clear();
    // in-range (< 50 km)
    g_mockAcs.push_back(mk("RESCUE51", "306006",  3.0, 170.0,  1200,  20,  42, 7700));
    g_mockAcs.push_back(mk("IBE3174",  "301001",  5.0,  60.0,  6200, 250, 286, 4655));
    g_mockAcs.push_back(mk("EC-ABC",   "305005",  7.0, 200.0,  2800,  70,  96, 7000));
    g_mockAcs.push_back(mk("VLG28PK",  "303003",  8.0,   0.0, 34000, 180, 448, 1000));
    g_mockAcs.push_back(mk("RYR4521",  "302002", 11.0, 135.0, 11025, 225, 412, 3421));
    // out of range (> 50 km) -> shown as rim arrows pointing their way
    g_mockAcs.push_back(mk("AFR1234",  "401001", 55.0,  30.0, 37000, 210, 470, 1000));
    g_mockAcs.push_back(mk("DLH88X",   "402002", 62.0, 300.0, 39000, 120, 455, 2000));
    g_mockAcs.push_back(mk("BAW777",   "403003", 75.0, 200.0, 41000,  20, 480, 3000));
    g_mockAcs.push_back(mk("UAE9",     "404004", 90.0, 110.0, 38000, 290, 490, 4000));
    g_mockInit = g_mockAcs;
}

static void mock_step(double dt) {
    const double latR = HOME_LAT_DEFAULT * M_PI / 180.0;
    for (size_t i = 0; i < g_mockAcs.size(); ++i) {
        Aircraft &a = g_mockAcs[i];
        const double stepKm = (double)a.gs * 1.852 * (dt / 3600.0);   // kt -> km in dt s
        const double br = a.track * M_PI / 180.0;
        a.lat += (stepKm * cos(br)) / 111.0;
        a.lon += (stepKm * sin(br)) / (111.0 * cos(a.lat * M_PI / 180.0));
        const double dLat = (a.lat - HOME_LAT_DEFAULT) * 111.0;
        const double dLon = (a.lon - HOME_LON_DEFAULT) * 111.0 * cos(latR);
        if (sqrt(dLat * dLat + dLon * dLon) > RANGE_KM_DEFAULT * 1.04) {
            a.lat = g_mockInit[i].lat;     // respawn so the scene stays populated
            a.lon = g_mockInit[i].lon;
        }
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    setvbuf(stdout, NULL, _IOLBF, 0);  // line-buffered: logs appear even when piped to a file
    const char *shotPath = (argc >= 3 && strcmp(argv[1], "--shot") == 0) ? argv[2] : NULL;
    const char *gifPath  = (argc >= 3 && strcmp(argv[1], "--gif")  == 0) ? argv[2] : NULL;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("[sim] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    s_win = SDL_CreateWindow("Capsule Radar (sim)",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             SIM_W, SIM_H, SDL_WINDOW_ALLOW_HIGHDPI);
    s_ren = SDL_CreateRenderer(s_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_win || !s_ren) {
        printf("[sim] window/renderer creation failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_RenderSetLogicalSize(s_ren, SIM_W, SIM_H);
    s_tex = SDL_CreateTexture(s_ren, SDL_PIXELFORMAT_RGB565,
                              SDL_TEXTUREACCESS_STREAMING, SIM_W, SIM_H);
    printf("[sim] SDL video driver: %s\n", SDL_GetCurrentVideoDriver());

    lv_init();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf1[SIM_W * 100];
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, SIM_W * 100);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.draw_buf = &draw_buf;
    disp_drv.flush_cb = sdl_flush;
    disp_drv.hor_res  = SIM_W;
    disp_drv.ver_res  = SIM_H;
    lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = sdl_mouse_read;
    lv_indev_drv_register(&indev_drv);

    ui_create();
    ui_set_range_cb(sim_range_cb);   // on-screen zoom button
    ui_set_range_km(RANGE_KM_DEFAULT);
    mock_init();
    radar::update(g_mockAcs, g_set);
    ui_on_data_updated();
    printf("[sim] Capsule Radar simulator running (%dx%d) with 6 mock aircraft.\n", SIM_W, SIM_H);

    Uint32 last = SDL_GetTicks();
    Uint32 lastData = last;
    const Uint32 start = last;
    bool run = true;
    while (run) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) run = false;
            else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_t) radar::cycleTheme();  // T = switch theme
        }
        Uint32 now = SDL_GetTicks();
        lv_tick_inc(now - last);
        last = now;
        if (now - lastData >= 1000) {       // simulate a 1 Hz ADS-B poll
            lastData = now;
            mock_step(1.0);
            radar::update(g_mockAcs, g_set);
            ui_on_data_updated();
            char clk[8];
            snprintf(clk, sizeof(clk), "14:%02d", (int)((now / 1000) % 60));  // mock clock
            ui_set_status(true, true, -58, clk);   // mock: connected, fresh, strong signal
            ui_set_battery(78, false, true);   // mock battery
            ui_set_date("08 Jun 2026");        // mock date
            ui_set_netinfo("Configure at\ncapsuleradar.local\n192.168.1.42");  // mock net info
        }
        // fulfil route lookups with a mock (the sim has no network)
        char wc[12];
        if (route_pending(wc, sizeof(wc))) {
            static const char *cities[] = { "Madrid", "London", "Paris", "Berlin",
                                            "Rome", "Lisbon", "Amsterdam", "Dublin" };
            int h = 0;
            for (const char *p = wc; *p; ++p) h += (unsigned char)*p;
            route_store(wc, cities[h % 8], cities[(h / 2 + 3) % 8]);
        }
        lv_timer_handler();

        // headless screenshot mode: grab the boot splash early (before it fades)
        static bool splashSaved = false;
        if (shotPath && !splashSaved && now - start > 900) {
            splashSaved = true;
            lv_refr_now(NULL);
            SDL_RenderClear(s_ren); SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
            int ow, oh; SDL_GetRendererOutputSize(s_ren, &ow, &oh);
            SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
            if (surf) {
                SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
                char path[300]; snprintf(path, sizeof(path), "%s-splash.bmp", shotPath);
                SDL_SaveBMP(surf, path); SDL_FreeSurface(surf);
                printf("[sim] saved %s\n", path);
            }
        }

        // animated GIF capture (--gif <prefix>): grab frames after the splash fades
        static Uint32 lastGif = 0;
        static int gifFrame = 0;
        if (gifPath && now - start > 3000 && now - lastGif >= 55) {
            lastGif = now;
            if (gifFrame >= 60) { run = false; }
            else {
                SDL_RenderClear(s_ren); SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
                int ow, oh; SDL_GetRendererOutputSize(s_ren, &ow, &oh);
                SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
                if (surf) {
                    SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
                    char path[300]; snprintf(path, sizeof(path), "%s-%03d.bmp", gifPath, gifFrame);
                    SDL_SaveBMP(surf, path); SDL_FreeSurface(surf);
                }
                gifFrame++;
            }
        }

        // headless screenshot mode (--shot <prefix>): settle, then grab all 3 views
        if (shotPath && now - start > 4000) {
            for (int k = 0; k < 150; ++k) {              // fast-forward to build up the flow map
                mock_step(1.0);
                radar::update(g_mockAcs, g_set);
            }
            radar::select(0);                            // select an aircraft so the card shows
            ui_on_data_updated();
            { char wc[12]; if (route_pending(wc, sizeof(wc))) route_store(wc, "Madrid", "London"); }
            ui_on_data_updated();                        // pick up the mock route for the card
            int ow, oh;
            SDL_GetRendererOutputSize(s_ren, &ow, &oh);
            struct Shot { const char *name; int view; int theme; };
            const Shot shots[6] = {
                { "radar",  0, THEME_PHOSPHOR },
                { "dragon", 0, THEME_DRAGON   },
                { "amber",  0, THEME_AMBER    },
                { "military",0, THEME_MILITARY },
                { "list",   2, THEME_PHOSPHOR },
                { "stats",  3, THEME_PHOSPHOR },
            };
            for (int v = 0; v < 6; ++v) {
                radar::setTheme(shots[v].theme);
                ui_show_view(shots[v].view);
                lv_refr_now(NULL);                       // force the view into the buffer
                SDL_RenderClear(s_ren);
                SDL_RenderCopy(s_ren, s_tex, NULL, NULL);
                SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
                if (surf) {
                    SDL_RenderReadPixels(s_ren, NULL, SDL_PIXELFORMAT_ARGB8888, surf->pixels, surf->pitch);
                    char path[300];
                    snprintf(path, sizeof(path), "%s-%s.bmp", shotPath, shots[v].name);
                    SDL_SaveBMP(surf, path);
                    SDL_FreeSurface(surf);
                    printf("[sim] saved %s\n", path);
                }
            }
            radar::setTheme(THEME_PHOSPHOR);
            run = false;
        }
        SDL_Delay(5);
    }

    SDL_DestroyTexture(s_tex);
    SDL_DestroyRenderer(s_ren);
    SDL_DestroyWindow(s_win);
    SDL_Quit();
    return 0;
}
