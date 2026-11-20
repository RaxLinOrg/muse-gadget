/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "muse_radar_priv.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "muse_page.h"
#include "muse_settings_ui.h"
#include "muse_wifi.h"

static const char *TAG = "radar";

/*
 * The scope is rasterised into an RGB565 image of its own rather than drawn
 * with LVGL widgets: a sweep is a few hundred lines and triangles, and one
 * image costs one object instead of eighty. Text stays as real labels on top,
 * so the font work is LVGL's.
 */
#define PX   MUSE_RADAR_TILE_PX     /* 304 */
#define CTR  (PX / 2)
#define RAD  (PX / 2 - 2)

/* Phosphor green, because this page is pretending to be an instrument. */
#define C_BG     rgb(1, 18, 10)
#define C_RING   rgb(19, 92, 60)
#define C_GRID   rgb(10, 52, 34)
#define C_COAST  rgb(26, 122, 82)
#define C_LOW    rgb(234, 255, 233)
#define C_MID    rgb(77, 255, 159)
#define C_HIGH   rgb(31, 160, 100)
#define C_SEL    rgb(255, 255, 255)
#define C_SHIP   rgb(120, 205, 255)

#define TEXT_DIM  0x0d6b43
#define TEXT_MID  0x1fd47a
#define TEXT_LIT  0x7ef0b8
#define TEXT_WARN 0xe0a85f

static uint16_t *s_px;
static lv_image_dsc_t s_dsc;
static lv_obj_t *s_img, *s_title, *s_sub, *s_card, *s_card_id, *s_card_sub;
static lv_obj_t *s_card_a, *s_tag[3];
static uint32_t s_drawn_at = UINT32_MAX;
static int s_drawn_n = -1;

static inline uint16_t rgb(int r, int g, int b)
{
    return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

/* ------------------------------------------------------- rasteriser */

static inline void px(int x, int y, uint16_t c)
{
    if ((unsigned)x < PX && (unsigned)y < PX) {
        s_px[y * PX + x] = c;
    }
}

static void line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        px(x0, y0, c);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

static void ring(int cx, int cy, int r, uint16_t c)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        px(cx + x, cy + y, c); px(cx + y, cy + x, c);
        px(cx - y, cy + x, c); px(cx - x, cy + y, c);
        px(cx - x, cy - y, c); px(cx - y, cy - x, c);
        px(cx + y, cy - x, c); px(cx + x, cy - y, c);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

static void disc(int cx, int cy, int r, uint16_t c)
{
    for (int y = -r; y <= r; y++) {
        int w = (int)(sqrtf((float)(r * r - y * y)) + 0.5f);
        for (int x = -w; x <= w; x++) {
            px(cx + x, cy + y, c);
        }
    }
}

/* A filled triangle, which is every blip on the screen. */
static void tri(float ax, float ay, float bx, float by, float cx2, float cy2, uint16_t c)
{
    int lo_y = (int)floorf(fminf(ay, fminf(by, cy2)));
    int hi_y = (int)ceilf(fmaxf(ay, fmaxf(by, cy2)));
    for (int y = lo_y; y <= hi_y; y++) {
        float lo = 1e9f, hi = -1e9f;
        float xs[3] = { ax, bx, cx2 }, ys[3] = { ay, by, cy2 };
        for (int i = 0; i < 3; i++) {
            int j = (i + 1) % 3;
            float y0 = ys[i], y1 = ys[j];
            if ((y + 0.5f < y0) == (y + 0.5f < y1)) {
                continue;
            }
            float t = (y + 0.5f - y0) / (y1 - y0);
            float x = xs[i] + t * (xs[j] - xs[i]);
            lo = fminf(lo, x);
            hi = fmaxf(hi, x);
        }
        if (hi < lo) {
            continue;
        }
        for (int x = (int)lo; x <= (int)hi; x++) {
            px(x, y, c);
        }
    }
}

/* ------------------------------------------------------------ the scope */

static void polar(float dst, float dir, float range, float *x, float *y)
{
    float r = dst / range * RAD;
    float t = dir * (float)M_PI / 180.0f;
    *x = CTR + r * sinf(t);
    *y = CTR - r * cosf(t);
}

static void draw_coast(float range)
{
    const muse_radar_place_t *p = muse_radar_place_at();
    float scale = RAD / range;
    float coslat = cosf(p->lat * (float)M_PI / 180.0f);
    int px0 = 0, py0 = 0;
    bool have = false;
    for (int i = 0; i < p->coast_len; i++) {
        int16_t dlat = p->coast[i].dlat, dlon = p->coast[i].dlon;
        if (dlat == MUSE_RADAR_BREAK) {
            have = false;
            continue;
        }
        /* Milli-degrees to nautical miles to pixels. Equirectangular is
         * plenty here: the error over 250 nm is smaller than a blip. */
        int x = CTR + (int)(dlon / 1000.0f * 60.0f * coslat * scale);
        int y = CTR - (int)(dlat / 1000.0f * 60.0f * scale);
        if (have) {
            int dx = x - CTR, dy = y - CTR, ex = px0 - CTR, ey = py0 - CTR;
            if (dx * dx + dy * dy <= RAD * RAD && ex * ex + ey * ey <= RAD * RAD) {
                line(px0, py0, x, y, C_COAST);
            }
        }
        px0 = x;
        py0 = y;
        have = true;
    }
}

static uint16_t alt_colour(int32_t ft)
{
    if (ft < 5000) {
        return C_LOW;
    }
    return ft < 20000 ? C_MID : C_HIGH;
}

static void draw_blip(float x, float y, float heading, float size, uint16_t c)
{
    float t = heading * (float)M_PI / 180.0f;
    float pts[3][2];
    const float ang[3] = { 0.0f, 140.0f, 220.0f };
    const float len[3] = { size, size * 0.62f, size * 0.62f };
    for (int i = 0; i < 3; i++) {
        float a = t + ang[i] * (float)M_PI / 180.0f;
        pts[i][0] = x + len[i] * sinf(a);
        pts[i][1] = y - len[i] * cosf(a);
    }
    tri(pts[0][0], pts[0][1], pts[1][0], pts[1][1], pts[2][0], pts[2][1], c);
}

static void draw_scope(const muse_radar_sweep_t *s)
{
    float range = (float)muse_radar_range_nm();
    if (range <= 0.0f) {
        return;
    }
    memset(s_px, 0, (size_t)PX * PX * 2);
    disc(CTR, CTR, RAD, C_BG);

    if (muse_radar_mode() == MUSE_RADAR_WEATHER) {
        const uint16_t *rain = muse_radar_rain();
        if (rain) {
            for (int y = 0; y < PX; y++) {
                for (int x = 0; x < PX; x++) {
                    int dx = x - CTR, dy = y - CTR;
                    if (dx * dx + dy * dy > RAD * RAD) {
                        continue;
                    }
                    uint16_t v = rain[y * PX + x];
                    if (v) {
                        s_px[y * PX + x] = v;
                    }
                }
            }
        }
    }

    draw_coast(range);

    for (int i = 1; i <= 3; i++) {
        ring(CTR, CTR, RAD * i / 3, C_RING);
    }
    line(CTR - RAD, CTR, CTR + RAD, CTR, C_GRID);
    line(CTR, CTR - RAD, CTR, CTR + RAD, C_GRID);
    /* A bearing scale, the way a ship's radar has one. */
    for (int deg = 0; deg < 360; deg += 10) {
        float t = deg * (float)M_PI / 180.0f;
        int len = deg % 30 == 0 ? 7 : 4;
        line((int)(CTR + (RAD - len) * sinf(t)), (int)(CTR - (RAD - len) * cosf(t)),
             (int)(CTR + RAD * sinf(t)), (int)(CTR - RAD * cosf(t)),
             deg % 30 == 0 ? C_MID : C_RING);
    }

    bool ships = muse_radar_mode() == MUSE_RADAR_SHIPS;
    for (int i = 0; i < s->n; i++) {
        const muse_radar_target_t *t = &s->t[i];
        float x, y;
        polar(t->dst, t->dir, range, &x, &y);
        if (ships) {
            /* A contact and the way it is pointing, ARPA style. */
            float a = t->track * (float)M_PI / 180.0f;
            float v = t->speed / 6.0f / range * RAD;    /* ten minutes of it */
            line((int)x, (int)y, (int)(x + v * sinf(a)), (int)(y - v * cosf(a)), C_RING);
            draw_blip(x, y, t->track, 6.0f, C_SHIP);
        } else {
            draw_blip(x, y, t->track, 6.0f, alt_colour(t->alt));
        }
    }
    if (s->n > 0) {
        float x, y;
        polar(s->t[0].dst, s->t[0].dir, range, &x, &y);
        ring((int)x, (int)y, 12, C_SEL);
    }
    ring(CTR, CTR, 7, C_MID);
    disc(CTR, CTR, 2, C_MID);

    lv_obj_invalidate(s_img);
}

/* --------------------------------------------------------------- the page */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t colour,
                       lv_align_t align, int x, int y)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, align, x, y);
    return l;
}

static void build(lv_obj_t *tile)
{
    lv_obj_set_style_bg_color(tile, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);

    s_px = heap_caps_malloc((size_t)PX * PX * 2, MALLOC_CAP_SPIRAM);
    if (!s_px) {
        ESP_LOGE(TAG, "no room for the %d px scope", PX);
        label(tile, muse_page_caption_font(), TEXT_WARN, LV_ALIGN_CENTER, 0, 0);
        return;
    }
    memset(s_px, 0, (size_t)PX * PX * 2);
    s_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_dsc.header.w = PX;
    s_dsc.header.h = PX;
    s_dsc.header.stride = PX * 2;
    s_dsc.data_size = (uint32_t)PX * PX * 2;
    s_dsc.data = (const uint8_t *)s_px;

    s_img = lv_image_create(tile);
    lv_image_set_src(s_img, &s_dsc);
    lv_obj_align(s_img, LV_ALIGN_CENTER, 0, 6);

    const lv_font_t *cap = muse_page_caption_font();
    s_title = label(tile, cap, TEXT_MID, LV_ALIGN_TOP_MID, 0, 18);
    lv_label_set_text(s_title, "RADAR");
    s_sub = label(tile, cap, TEXT_DIM, LV_ALIGN_TOP_MID, 0, 40);

    s_card = lv_obj_create(tile);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, 286, 48);
    lv_obj_align(s_card, LV_ALIGN_BOTTOM_MID, 0, -26);
    lv_obj_set_style_bg_color(s_card, lv_color_hex(0x06180f), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_card, 14, 0);
    lv_obj_set_style_border_width(s_card, 1, 0);
    lv_obj_set_style_border_color(s_card, lv_color_hex(0x135c3c), 0);
    lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);

    s_card_id  = label(s_card, cap, 0xeaffe9, LV_ALIGN_TOP_LEFT, 12, 6);
    s_card_sub = label(s_card, cap, TEXT_DIM, LV_ALIGN_BOTTOM_LEFT, 12, -6);
    s_card_a   = label(s_card, cap, TEXT_MID, LV_ALIGN_TOP_RIGHT, -12, 6);

    for (int i = 0; i < 3; i++) {
        s_tag[i] = label(tile, cap, TEXT_DIM, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_add_flag(s_tag[i], LV_OBJ_FLAG_HIDDEN);
    }
    muse_radar_net_start();
}

/* The three labels that sit on the scope itself, on the nearest contacts
 * that are far enough apart not to overprint one another. */
static void place_tags(const muse_radar_sweep_t *s, float range)
{
    int used = 0;
    float ux[3] = { 0 }, uy[3] = { 0 };
    for (int i = 0; i < s->n && used < 3; i++) {
        if (!s->t[i].id[0]) {
            continue;
        }
        float x, y;
        polar(s->t[i].dst, s->t[i].dir, range, &x, &y);
        float dx = x - CTR, dy = y - CTR;
        if (sqrtf(dx * dx + dy * dy) < RAD * 0.30f) {
            continue;       /* too close to the middle to label cleanly */
        }
        bool clear = true;
        for (int k = 0; k < used; k++) {
            if (fabsf(x - ux[k]) < 56.0f && fabsf(y - uy[k]) < 24.0f) {
                clear = false;
                break;
            }
        }
        if (!clear) {
            continue;
        }
        ux[used] = x;
        uy[used] = y;
        lv_label_set_text(s_tag[used], s->t[i].id);
        lv_obj_clear_flag(s_tag[used], LV_OBJ_FLAG_HIDDEN);
        /* The image is centred on the tile with a 6 px drop. */
        lv_obj_align(s_tag[used], LV_ALIGN_CENTER,
                     (int)(x - CTR) + 28, (int)(y - CTR) + 6 - 10);
        used++;
    }
    for (int i = used; i < 3; i++) {
        lv_obj_add_flag(s_tag[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_card(const muse_radar_sweep_t *s)
{
    char buf[40];
    if (muse_radar_mode() == MUSE_RADAR_WEATHER) {
        lv_label_set_text(s_card_id, muse_radar_rain() ? "RAIN" : "NO PICTURE YET");
        lv_label_set_text(s_card_sub, "RainViewer");
        lv_label_set_text(s_card_a, "");
        return;
    }
    if (muse_radar_mode() == MUSE_RADAR_SHIPS && !muse_radar_ais_have_key()) {
        lv_label_set_text(s_card_id, "NO AIS KEY");
        lv_label_set_text(s_card_sub, "aisstream.io key");
        lv_label_set_text(s_card_a, "");
        return;
    }
    if (s->n == 0) {
        lv_label_set_text(s_card_id, s->valid ? "NOTHING IN RANGE" : "LOOKING...");
        lv_label_set_text(s_card_sub, "");
        lv_label_set_text(s_card_a, "");
        return;
    }
    const muse_radar_target_t *t = &s->t[0];
    lv_label_set_text(s_card_id, t->id[0] ? t->id : "-");
    if (muse_radar_mode() == MUSE_RADAR_SHIPS) {
        snprintf(buf, sizeof(buf), "%.1f kn", (double)t->speed);
    } else {
        snprintf(buf, sizeof(buf), "%ld ft", (long)t->alt);
    }
    lv_label_set_text(s_card_a, buf);
    /* One bottom line, left-aligned: a right-aligned second label collided
     * with the type on the left once speed and range were both spelled out. */
    snprintf(buf, sizeof(buf), "%s%s%.0fkt %.1fnm",
             t->type[0] ? t->type : "", t->type[0] ? " " : "",
             (double)t->speed, (double)t->dst);
    lv_label_set_text(s_card_sub, buf);
}

static void tick(bool visible)
{
    muse_radar_set_visible(visible);
    if (!visible || !s_px) {
        return;
    }
    char sub[48];
    snprintf(sub, sizeof(sub), "%s  %d NM  %s",
             muse_radar_mode_name(muse_radar_mode()),
             muse_radar_range_nm(), muse_radar_place_at()->name);
    lv_label_set_text(s_sub, sub);

    muse_radar_sweep_t s;
    muse_radar_get(&s);
    bool weather = muse_radar_mode() == MUSE_RADAR_WEATHER;
    if (s.at != s_drawn_at || s.n != s_drawn_n || weather) {
        draw_scope(&s);
        if (!weather) {
            place_tags(&s, (float)muse_radar_range_nm());
        } else {
            for (int i = 0; i < 3; i++) {
                lv_obj_add_flag(s_tag[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        s_drawn_at = s.at;
        s_drawn_n = s.n;
    }
    show_card(&s);
    if (!muse_wifi_connected()) {
        lv_label_set_text(s_card_id, "NO WI-FI");
    }
}

/* ------------------------------------------------------- settings page */

static lv_obj_t *s_range_val, *s_mode_val;
static lv_obj_t *s_place_check[MUSE_RADAR_PLACE_COUNT];

static void on_settings_deleted(lv_event_t *e)
{
    (void)e;
    s_range_val = NULL;
    s_mode_val = NULL;
    memset(s_place_check, 0, sizeof(s_place_check));
}

static void show_values(void)
{
    if (s_range_val) {
        int i = muse_radar_range_index();
        if (i == MUSE_RADAR_OFF) {
            lv_label_set_text(s_range_val, "Off");
        } else {
            char buf[12];
            snprintf(buf, sizeof(buf), "%d NM", MUSE_RADAR_RANGES[i]);
            lv_label_set_text(s_range_val, buf);
        }
    }
    if (s_mode_val) {
        lv_label_set_text(s_mode_val, muse_radar_mode_name(muse_radar_mode()));
    }
    int chosen = muse_radar_place();
    for (int i = 0; i < MUSE_RADAR_PLACE_COUNT; i++) {
        if (s_place_check[i]) {
            lv_label_set_text(s_place_check[i], i == chosen ? LV_SYMBOL_OK : "");
        }
    }
}

static void on_range(lv_event_t *e)
{
    (void)e;
    /* 12, 40, 250, Off, and round again. */
    muse_radar_set_range_index((muse_radar_range_index() + 1) % (MUSE_RADAR_OFF + 1));
    show_values();
}

static void on_mode(lv_event_t *e)
{
    (void)e;
    muse_radar_set_mode((muse_radar_mode() + 1) % MUSE_RADAR_MODE_COUNT);
    show_values();
}

static void on_place(lv_event_t *e)
{
    muse_radar_set_place((int)(intptr_t)lv_event_get_user_data(e));
    show_values();
}

static void build_settings(lv_obj_t *tile)
{
    lv_obj_t *list;
    lv_obj_t *p = muse_settings_ui_page(tile, "RADAR", &list);
    lv_obj_add_event_cb(p, on_settings_deleted, LV_EVENT_DELETE, NULL);

    muse_settings_ui_row(list, NULL, "Range", &s_range_val, on_range, NULL);
    muse_settings_ui_row(list, NULL, "Show", &s_mode_val, on_mode, NULL);
    muse_settings_ui_note(list,
                          "Range cycles 12, 40, 250 NM and Off. Off takes the "
                          "page out of the swipe list altogether, so nothing "
                          "is fetched.");
    for (int i = 0; i < MUSE_RADAR_PLACE_COUNT; i++) {
        muse_settings_ui_row(list, NULL, MUSE_RADAR_PLACES[i].name,
                             &s_place_check[i], on_place, (void *)(intptr_t)i);
    }
    muse_settings_ui_note(list,
                          "Planes come from volunteer ADS-B receivers, so East "
                          "Asia shows a fraction of what is really flying. "
                          "Panama is for ships rather than aircraft.");
    show_values();
}

static void settings_value(char *out, size_t cap)
{
    if (!muse_radar_enabled()) {
        snprintf(out, cap, "Off");
        return;
    }
    snprintf(out, cap, "%d NM  %s", muse_radar_range_nm(),
             muse_radar_mode_name(muse_radar_mode()));
}

/*
 * ">page=radar:weather", ":ships", ":planes", ":12" / ":40" / ":250" / ":off",
 * ":place=3" — the settings rows need a finger, and a board on a bench has
 * none. Runs on the console task; the setters only touch NVS and the worker,
 * never LVGL, and tick() redraws from the sweep as usual.
 */
static bool bench(const char *arg)
{
    if (!strcmp(arg, "planes")) {
        muse_radar_set_mode(MUSE_RADAR_PLANES);
    } else if (!strcmp(arg, "ships")) {
        muse_radar_set_mode(MUSE_RADAR_SHIPS);
    } else if (!strcmp(arg, "weather")) {
        muse_radar_set_mode(MUSE_RADAR_WEATHER);
    } else if (!strcmp(arg, "off")) {
        muse_radar_set_range_index(MUSE_RADAR_OFF);
    } else if (!strncmp(arg, "place=", 6)) {
        int i = atoi(arg + 6);
        if (i < 0 || i >= MUSE_RADAR_PLACE_COUNT) {
            return false;
        }
        muse_radar_set_place(i);
    } else {
        int nm = atoi(arg), hit = -1;
        for (int i = 0; i < 3; i++) {
            if (MUSE_RADAR_RANGES[i] == nm) {
                hit = i;
            }
        }
        if (hit < 0) {
            return false;
        }
        muse_radar_set_range_index(hit);
    }
    printf("@radar %d %d nm %s\n", (int)muse_radar_mode(),
           muse_radar_range_nm(), muse_radar_place_at()->name);
    return true;
}

static const muse_page_t radar_page = {
    .name = "radar",
    .build = build,
    .tick = tick,
    .enabled = muse_radar_enabled,
    .settings_title = "Radar",
    .settings_icon = LV_SYMBOL_GPS,
    .settings_build = build_settings,
    .settings_value = settings_value,
    .bench = bench,
};
MUSE_PAGE(radar_page);
