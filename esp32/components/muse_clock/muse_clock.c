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

#include "muse_clock.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "sdkconfig.h"

#include "muse_board.h"
#include "muse_clock_cities.h"
#include "muse_clock_icons.h"
#include "muse_page.h"
#include "muse_settings_ui.h"
#include "muse_weather.h"
#include "muse_wifi.h"

static const char *TAG = "clock";

/* A Kconfig bool that is off is undefined, not zero. */
#ifdef CONFIG_MUSE_CLOCK_24H
#define CLOCK_24H 1
#else
#define CLOCK_24H 0
#endif

LV_FONT_DECLARE(muse_font_clock_88)

/* The palette muse_ui.c uses, so the page looks like the rest of the UI. */
#define COLOR_DIM     0x8b84a8
#define COLOR_CAPTION 0xd8d2ff
#define COLOR_ACCENT  0xa77dff
#define COLOR_LIT     0xf2efff
#define COLOR_PANEL   0x140f22
#define COLOR_HI      0xff8a3d
#define COLOR_LO      0x7bb8ff

/* 16x16 art at 1:1 for the day tiles, doubled for today's. */
#define ICON_SMALL 1
#define ICON_BIG   2

/* Layout for the 466 px round panel, as offsets from the centre. A compact
 * panel gets the same column with the forecast tiles left off. */
#define Y_WEEKDAY (-172)
#define Y_DATE    (-150)
#define Y_TIME    (-66)
#define Y_NOW     (0)
#define Y_SKY     (34)
#define Y_RULE    (54)
#define Y_TILES   (110)
#define Y_FOOT    (182)

#define TILE_W 124
#define TILE_H 86

static lv_obj_t *s_weekday, *s_date, *s_time, *s_ampm;
static lv_obj_t *s_now_icon, *s_now_temp, *s_now_hi, *s_now_lo, *s_sky, *s_foot;
static lv_obj_t *s_day_name[MUSE_WEATHER_DAYS - 1];
static lv_obj_t *s_day_icon[MUSE_WEATHER_DAYS - 1];
static lv_obj_t *s_day_hi[MUSE_WEATHER_DAYS - 1];
static lv_obj_t *s_day_lo[MUSE_WEATHER_DAYS - 1];

/* One ARGB8888 image per sky, built once and shared by every widget that
 * shows that sky at that scale. */
static lv_image_dsc_t s_icon[MUSE_SKY_COUNT][ICON_BIG + 1];

static bool s_sntp_started;
static int32_t s_offset = INT32_MIN;    /* seconds east of UTC, once known */
static char s_shown_time[8];
static char s_shown_date[24];
static muse_sky_t s_shown_sky[MUSE_WEATHER_DAYS];
static bool s_shown_valid;

bool muse_clock_enabled(void)
{
    return true;    /* the component is only compiled in when the page is on */
}

/* ---------------------------------------------------------------- icons */

static uint32_t art_color(char c)
{
    switch (c) {
    case 'y': return MUSE_SKY_COLOR_SUN;
    case 'w': return MUSE_SKY_COLOR_CLOUD;
    case 'g': return MUSE_SKY_COLOR_SHADE;
    case 'b': return MUSE_SKY_COLOR_RAIN;
    case 's': return MUSE_SKY_COLOR_SNOW;
    case 'f': return MUSE_SKY_COLOR_BOLT;
    default:  return 0;     /* transparent */
    }
}

static const char *const *art_of(muse_sky_t sky)
{
    switch (sky) {
    case MUSE_SKY_CLEAR:  return MUSE_SKY_ART_CLEAR;
    case MUSE_SKY_PARTLY: return MUSE_SKY_ART_PARTLY;
    case MUSE_SKY_CLOUD:  return MUSE_SKY_ART_CLOUD;
    case MUSE_SKY_FOG:    return MUSE_SKY_ART_FOG;
    case MUSE_SKY_SNOW:   return MUSE_SKY_ART_SNOW;
    case MUSE_SKY_STORM:  return MUSE_SKY_ART_STORM;
    default:              return MUSE_SKY_ART_RAIN;
    }
}

/* The string art, scaled up by whole pixels into an ARGB8888 image. */
static const lv_image_dsc_t *icon(muse_sky_t sky, int scale)
{
    if (sky >= MUSE_SKY_COUNT || scale < 1 || scale > ICON_BIG) {
        return NULL;
    }
    lv_image_dsc_t *dsc = &s_icon[sky][scale];
    if (dsc->data) {
        return dsc;
    }
    int w = MUSE_SKY_ART_W * scale, h = MUSE_SKY_ART_H * scale;
    uint8_t *buf = lv_malloc_zeroed((size_t)w * h * 4);
    if (!buf) {
        ESP_LOGW(TAG, "no room for the %d px icon", w);
        return NULL;
    }
    const char *const *art = art_of(sky);
    for (int r = 0; r < MUSE_SKY_ART_H; r++) {
        for (int c = 0; c < MUSE_SKY_ART_W; c++) {
            uint32_t rgb = art_color(art[r][c]);
            if (!rgb) {
                continue;
            }
            for (int y = 0; y < scale; y++) {
                for (int x = 0; x < scale; x++) {
                    uint8_t *q = buf + (((size_t)(r * scale + y)) * w + c * scale + x) * 4;
                    q[0] = (uint8_t)(rgb & 0xff);           /* B */
                    q[1] = (uint8_t)((rgb >> 8) & 0xff);    /* G */
                    q[2] = (uint8_t)((rgb >> 16) & 0xff);   /* R */
                    q[3] = 0xff;                            /* A */
                }
            }
        }
    }
    dsc->header.cf = LV_COLOR_FORMAT_ARGB8888;
    dsc->header.w = (uint32_t)w;
    dsc->header.h = (uint32_t)h;
    dsc->header.stride = (uint32_t)w * 4;
    dsc->data_size = (uint32_t)w * h * 4;
    dsc->data = buf;
    return dsc;
}

/* ----------------------------------------------------------------- time */

static void start_sntp(void)
{
    if (s_sntp_started) {
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_MUSE_CLOCK_NTP_SERVER);
    /* The firmware brings Wi-Fi up and down as it likes; resync on each
     * reconnect rather than only at the first one. */
    cfg.start = true;
    cfg.server_from_dhcp = false;
    cfg.renew_servers_after_new_IP = true;
    cfg.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    if (esp_netif_sntp_init(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP did not start");
        return;
    }
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP started against %s", CONFIG_MUSE_CLOCK_NTP_SERVER);
}

/* Local time, from the UTC clock plus the forecast's own offset. Returns
 * false until both halves are known. */
static bool local_now(struct tm *out)
{
    if (s_offset == INT32_MIN) {
        return false;
    }
    time_t t = time(NULL);
    if (t < 1700000000) {
        return false;       /* SNTP has not answered yet */
    }
    t += s_offset;
    gmtime_r(&t, out);
    return true;
}

/* ----------------------------------------------------------------- build */

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t color,
                       int16_t dx, int16_t dy)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    lv_obj_align(l, LV_ALIGN_CENTER, dx, dy);
    return l;
}

static void build_day_tile(lv_obj_t *tile, int i, int16_t dx)
{
    lv_obj_t *box = lv_obj_create(tile);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, TILE_W, TILE_H);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(COLOR_PANEL), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_align(box, LV_ALIGN_CENTER, dx, Y_TILES);

    s_day_name[i] = label(box, &lv_font_unscii_8, COLOR_DIM, 0, -30);
    lv_obj_set_style_text_letter_space(s_day_name[i], 2, 0);

    s_day_icon[i] = lv_image_create(box);
    lv_obj_align(s_day_icon[i], LV_ALIGN_CENTER, 0, -4);

    /* Montserrat, not unscii: the pixel face has no degree sign. */
    s_day_hi[i] = label(box, &lv_font_montserrat_16, COLOR_HI, -27, 28);
    s_day_lo[i] = label(box, &lv_font_montserrat_16, COLOR_LO, 27, 28);
}

static void build(lv_obj_t *tile)
{
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    bool big = muse_board->width >= 400 && muse_board->height >= 400;

    s_weekday = label(tile, &lv_font_unscii_16, COLOR_ACCENT, 0, big ? Y_WEEKDAY : -70);
    lv_obj_set_style_text_letter_space(s_weekday, 3, 0);
    s_date = label(tile, &lv_font_unscii_8, COLOR_DIM, 0, big ? Y_DATE : -52);
    lv_obj_set_style_text_letter_space(s_date, 1, 0);

    /* The one glyph set worth its flash: digits and a colon at 88 px. */
    s_time = label(tile, big ? &muse_font_clock_88 : &lv_font_montserrat_28,
                   COLOR_LIT, 0, big ? Y_TIME : -14);
    s_ampm = label(tile, &lv_font_unscii_16, COLOR_DIM, 0, big ? Y_TIME + 62 : 10);

    s_now_icon = lv_image_create(tile);
    lv_obj_align(s_now_icon, LV_ALIGN_CENTER, big ? -72 : -58, big ? Y_NOW : 34);
    s_now_temp = label(tile, big ? &lv_font_montserrat_28 : &lv_font_montserrat_20,
                       COLOR_LIT, big ? 8 : -8, big ? Y_NOW : 34);
    s_now_hi = label(tile, &lv_font_montserrat_16, COLOR_HI, big ? 88 : 44, big ? Y_NOW - 11 : 26);
    s_now_lo = label(tile, &lv_font_montserrat_16, COLOR_LO, big ? 88 : 44, big ? Y_NOW + 11 : 44);

    s_sky = label(tile, &lv_font_unscii_8, COLOR_DIM, 0, big ? Y_SKY : 62);
    lv_obj_set_style_text_letter_space(s_sky, 1, 0);

    if (big) {
        lv_obj_t *rule = lv_obj_create(tile);
        lv_obj_remove_style_all(rule);
        lv_obj_set_size(rule, 230, 2);
        lv_obj_set_style_bg_color(rule, lv_color_hex(0x1d1733), 0);
        lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
        lv_obj_align(rule, LV_ALIGN_CENTER, 0, Y_RULE);

        for (int i = 0; i < MUSE_WEATHER_DAYS - 1; i++) {
            build_day_tile(tile, i, (int16_t)(i == 0 ? -71 : 71));
        }
        s_foot = label(tile, &lv_font_unscii_8, COLOR_DIM, 0, Y_FOOT);
    }

    /* Nothing else starts these: the page owns its clock and its forecast. */
    muse_weather_start();
    ESP_LOGI(TAG, "page built for %s", muse_clock_city_name());
}

/* ------------------------------------------------------------------ tick */

static const char *const WEEKDAY[7] = {
    "SUNDAY", "MONDAY", "TUESDAY", "WEDNESDAY", "THURSDAY", "FRIDAY", "SATURDAY",
};
static const char *const MONTH[12] = {
    "JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC",
};

static void set_icon(lv_obj_t *obj, muse_sky_t sky, int scale)
{
    const lv_image_dsc_t *d = icon(sky, scale);
    if (d) {
        lv_image_set_src(obj, d);
    }
}

static void show_clock(void)
{
    struct tm tm;
    if (!local_now(&tm)) {
        if (strcmp(s_shown_time, "--:--")) {
            lv_label_set_text(s_time, "--:--");
            lv_label_set_text(s_weekday, muse_wifi_connected() ? "SYNCING" : "NO WI-FI");
            lv_label_set_text(s_date, "");
            lv_label_set_text(s_ampm, "");
            snprintf(s_shown_time, sizeof(s_shown_time), "--:--");
            s_shown_date[0] = '\0';
        }
        return;
    }
    char hhmm[8];
    int hour = tm.tm_hour;
    const char *ampm = "";
    if (!CLOCK_24H) {
        ampm = hour < 12 ? "AM" : "PM";
        hour = hour % 12;
        if (hour == 0) {
            hour = 12;
        }
    }
    /* 24-hour time is written with the leading zero; 12-hour time is not. */
    snprintf(hhmm, sizeof(hhmm), CLOCK_24H ? "%02d:%02d" : "%d:%02d", hour, tm.tm_min);
    if (strcmp(hhmm, s_shown_time)) {
        lv_label_set_text(s_time, hhmm);
        lv_label_set_text(s_ampm, ampm);
        snprintf(s_shown_time, sizeof(s_shown_time), "%s", hhmm);
    }
    char date[24];
    snprintf(date, sizeof(date), "%d %s %d", tm.tm_mday, MONTH[tm.tm_mon], 1900 + tm.tm_year);
    if (strcmp(date, s_shown_date)) {
        lv_label_set_text(s_weekday, WEEKDAY[tm.tm_wday % 7]);
        lv_label_set_text(s_date, date);
        snprintf(s_shown_date, sizeof(s_shown_date), "%s", date);
        /* A new local day moved the forecast's "today" along. */
        muse_weather_refresh();
    }
}

/* ------------------------------------------------------- settings page */

/* Cleared when the page is freed, which happens on the way back. */
static lv_obj_t *s_city_check[MUSE_CITY_COUNT];

static void on_city_page_deleted(lv_event_t *e)
{
    (void)e;
    memset(s_city_check, 0, sizeof(s_city_check));
}

static void mark_city(void)
{
    int chosen = muse_clock_city();
    for (int i = 0; i < MUSE_CITY_COUNT; i++) {
        if (s_city_check[i]) {
            lv_label_set_text(s_city_check[i], i == chosen ? LV_SYMBOL_OK : "");
        }
    }
}

static void on_city(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    muse_clock_set_city(i);
    /* The new place is in another timezone, so the old offset is wrong until
     * its forecast lands. Showing nothing beats showing the wrong hour. */
    s_offset = INT32_MIN;
    s_shown_valid = false;
    s_shown_time[0] = '\0';
    s_shown_date[0] = '\0';
    mark_city();
}

static void build_settings(lv_obj_t *tile)
{
    lv_obj_t *list;
    lv_obj_t *p = muse_settings_ui_page(tile, "WEATHER", &list);
    lv_obj_add_event_cb(p, on_city_page_deleted, LV_EVENT_DELETE, NULL);
    for (int i = 0; i < MUSE_CITY_COUNT; i++) {
        muse_settings_ui_row(list, NULL, MUSE_CITIES[i].name, &s_city_check[i],
                             on_city, (void *)(intptr_t)i);
        lv_obj_set_style_text_color(s_city_check[i], lv_color_hex(COLOR_ACCENT), 0);
    }
    muse_settings_ui_note(list,
                          "The clock follows the city: its timezone comes back "
                          "with the forecast. The last entry is the one from "
                          "menuconfig.");
    mark_city();
}

static void settings_value(char *out, size_t cap)
{
    snprintf(out, cap, "%s", muse_clock_city_name());
}

/* ------------------------------------------------------------ weather */

static void show_weather(void)
{
    muse_weather_t w;
    muse_weather_get(&w);
    if (w.utc_offset != 0 || w.valid) {
        s_offset = w.valid ? w.utc_offset : s_offset;
    }
    if (!w.valid) {
        if (s_shown_valid) {
            return;     /* keep the last forecast on screen rather than blanking it */
        }
        lv_label_set_text(s_sky, muse_wifi_connected() ? "LOOKING UP THE FORECAST" : "");
        return;
    }
    const char *deg = muse_weather_fahrenheit() ? "°F" : "°C";
    char buf[24];

    if (!s_shown_valid || w.sky != s_shown_sky[0]) {
        set_icon(s_now_icon, w.sky, ICON_BIG);
        lv_label_set_text(s_sky, muse_weather_sky_name(w.sky));
    }
    snprintf(buf, sizeof(buf), "%d%s", w.temp, deg);
    lv_label_set_text(s_now_temp, buf);
    snprintf(buf, sizeof(buf), "%d°", w.hi[0]);
    lv_label_set_text(s_now_hi, buf);
    snprintf(buf, sizeof(buf), "%d°", w.lo[0]);
    lv_label_set_text(s_now_lo, buf);

    if (s_foot) {
        struct tm tm;
        if (local_now(&tm)) {
            snprintf(buf, sizeof(buf), "%s %02d:%02d", muse_clock_city_name(),
                     tm.tm_hour, tm.tm_min);
        } else {
            snprintf(buf, sizeof(buf), "%s", muse_clock_city_name());
        }
        lv_label_set_text(s_foot, buf);
    }

    /* Tomorrow and the day after, named from the local date. */
    for (int i = 0; i < MUSE_WEATHER_DAYS - 1; i++) {
        if (!s_day_name[i]) {
            break;
        }
        struct tm tm;
        if (local_now(&tm)) {
            const char *name = WEEKDAY[(tm.tm_wday + 1 + i) % 7];
            char abbr[4] = { name[0], name[1], name[2], '\0' };
            lv_label_set_text(s_day_name[i], abbr);
        }
        if (!s_shown_valid || w.day_sky[i + 1] != s_shown_sky[i + 1]) {
            set_icon(s_day_icon[i], w.day_sky[i + 1], ICON_SMALL);
        }
        snprintf(buf, sizeof(buf), "%d°", w.hi[i + 1]);
        lv_label_set_text(s_day_hi[i], buf);
        snprintf(buf, sizeof(buf), "%d°", w.lo[i + 1]);
        lv_label_set_text(s_day_lo[i], buf);
    }

    for (int i = 0; i < MUSE_WEATHER_DAYS; i++) {
        s_shown_sky[i] = i == 0 ? w.sky : w.day_sky[i];
    }
    s_shown_valid = true;
}

static void tick(bool visible)
{
    if (muse_wifi_connected()) {
        start_sntp();
    }
    /* Off screen, the forecast still has to land somewhere; only the drawing
     * waits for the page to come into view. */
    if (!visible) {
        muse_weather_t w;
        muse_weather_get(&w);
        if (w.valid) {
            s_offset = w.utc_offset;
        }
        return;
    }
    show_weather();
    show_clock();
}

static const muse_page_t clock_page = {
    .name = "clock",
    .build = build,
    .tick = tick,
    .settings_title = "Weather",
    .settings_icon = LV_SYMBOL_GPS,
    .settings_build = build_settings,
    .settings_value = settings_value,
};
MUSE_PAGE(clock_page);
