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

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "muse_wifi.h"

static const char *TAG = "radar";

const int MUSE_RADAR_RANGES[MUSE_RADAR_RANGE_COUNT] = { 12, 40, 250 };

#define NVS_NS "muse_radar"

#define HTTP_TIMEOUT_MS 10000
/* libpng's simplified reader is the deep one: the plane and ship paths sit
 * under 4 KB, a weather tile decode overflowed 7 KB. The stack is PSRAM. */
#define TASK_STACK      20480
#define RETRY_EVERY_S   30

/*
 * The response is read a chunk at a time and split into aircraft objects, so
 * a 615 KB New York sweep costs this much memory and not a byte more. A
 * single aircraft object runs to about 500 bytes; 2 KB is generous.
 */
#define CHUNK       2048
#define OBJ_MAX     2048

/* ------------------------------------------------------------- settings */

static int s_range = -1;        /* -1 until NVS is read */
static int s_mode = -1;
static int s_place = -1;

static int nvs_get(const char *key, int dflt, int lo, int hi)
{
    nvs_handle_t h;
    int32_t v = dflt;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, key, &v) != ESP_OK) {
            v = dflt;
        }
        nvs_close(h);
    }
    if (v < lo || v > hi) {
        v = dflt;
    }
    return (int)v;
}

static void nvs_put(const char *key, int v)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, key, (int32_t)v);
        nvs_commit(h);
        nvs_close(h);
    }
}

int muse_radar_range_index(void)
{
    if (s_range < 0) {
        /* 40 NM: busy enough to look alive, small enough to parse. */
        s_range = nvs_get("range", 1, 0, MUSE_RADAR_OFF);
    }
    return s_range;
}

void muse_radar_set_range_index(int i)
{
    if (i < 0 || i > MUSE_RADAR_OFF || i == muse_radar_range_index()) {
        return;
    }
    s_range = i;
    nvs_put("range", i);
    muse_radar_refresh();
}

int muse_radar_range_nm(void)
{
    int i = muse_radar_range_index();
    return i == MUSE_RADAR_OFF ? 0 : MUSE_RADAR_RANGES[i];
}

bool muse_radar_enabled(void)
{
    return muse_radar_range_index() != MUSE_RADAR_OFF;
}

muse_radar_mode_t muse_radar_mode(void)
{
    if (s_mode < 0) {
        s_mode = nvs_get("mode", MUSE_RADAR_PLANES, 0, MUSE_RADAR_MODE_COUNT - 1);
    }
    return (muse_radar_mode_t)s_mode;
}

void muse_radar_set_mode(muse_radar_mode_t m)
{
    if (m < 0 || m >= MUSE_RADAR_MODE_COUNT || m == muse_radar_mode()) {
        return;
    }
    s_mode = m;
    nvs_put("mode", m);
    muse_radar_refresh();
}

const char *muse_radar_mode_name(muse_radar_mode_t m)
{
    switch (m) {
    case MUSE_RADAR_SHIPS:   return "Ships";
    case MUSE_RADAR_WEATHER: return "Weather";
    default:                 return "Planes";
    }
}

int muse_radar_place(void)
{
    if (s_place < 0) {
        s_place = nvs_get("place", 0, 0, MUSE_RADAR_PLACE_COUNT - 1);
    }
    return s_place;
}

void muse_radar_set_place(int i)
{
    if (i < 0 || i >= MUSE_RADAR_PLACE_COUNT || i == muse_radar_place()) {
        return;
    }
    s_place = i;
    nvs_put("place", i);
    muse_radar_refresh();
}

const muse_radar_place_t *muse_radar_place_at(void)
{
    return &MUSE_RADAR_PLACES[muse_radar_place()];
}

int muse_radar_period_s(void)
{
    if (muse_radar_mode() == MUSE_RADAR_WEATHER) {
        return 600;     /* RainViewer publishes a frame every 600 s exactly */
    }
    switch (muse_radar_range_index()) {
    case 0:  return 30;     /*  12 NM: a blip crosses the scope in 3.2 min */
    case 1:  return 60;     /*  40 NM: 10.7 min */
    default: return 300;    /* 250 NM: 67 min */
    }
}

/* ---------------------------------------------------------- the sweep */

static muse_radar_sweep_t s_sweep;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_want = true;
static volatile bool s_visible;
static TaskHandle_t s_task;

void muse_radar_get(muse_radar_sweep_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_sweep;
    portEXIT_CRITICAL(&s_lock);
}

void muse_radar_refresh(void)
{
    s_want = true;
}

void muse_radar_set_visible(bool visible)
{
    if (visible && !s_visible) {
        s_want = true;      /* coming into view is worth a fresh sweep */
    }
    s_visible = visible;
}

/* ------------------------------------------------------ parsing a sweep */

/* Keeps the nearest MUSE_RADAR_MAX_TARGETS, in order, as they stream past. */
void muse_radar_keep(muse_radar_sweep_t *s, const muse_radar_target_t *t)
{
    if (s->n == MUSE_RADAR_MAX_TARGETS && t->dst >= s->t[s->n - 1].dst) {
        return;
    }
    int at = s->n < MUSE_RADAR_MAX_TARGETS ? s->n : MUSE_RADAR_MAX_TARGETS - 1;
    while (at > 0 && s->t[at - 1].dst > t->dst) {
        s->t[at] = s->t[at - 1];
        at--;
    }
    s->t[at] = *t;
    if (s->n < MUSE_RADAR_MAX_TARGETS) {
        s->n++;
    }
}

static float num(const cJSON *o, const char *key, float dflt)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : dflt;
}

static void str(const cJSON *o, const char *key, char *out, size_t cap)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    out[0] = '\0';
    if (!cJSON_IsString(v) || !v->valuestring) {
        return;
    }
    /* The feed space-pads callsigns ("EVA023  "). */
    const char *p = v->valuestring;
    while (*p == ' ') {
        p++;
    }
    size_t n = strlen(p);
    while (n && p[n - 1] == ' ') {
        n--;
    }
    if (n > cap - 1) {
        n = cap - 1;
    }
    memcpy(out, p, n);
    out[n] = '\0';
}

/* One aircraft object from the feed. False when it is not worth showing. */
static bool aircraft(const char *json, size_t len, muse_radar_target_t *t)
{
    cJSON *o = cJSON_ParseWithLength(json, len);
    if (!o) {
        return false;
    }
    bool ok = false;
    const cJSON *dst = cJSON_GetObjectItemCaseSensitive(o, "dst");
    const cJSON *dir = cJSON_GetObjectItemCaseSensitive(o, "dir");
    if (!cJSON_IsNumber(dst) || !cJSON_IsNumber(dir)) {
        goto done;      /* no position relative to us: nothing to draw */
    }
    /*
     * alt_baro is a number in the air and the string "ground" on it. Half the
     * traffic inside 60 nm of a big airport is parked, and drawing it puts a
     * blob on every runway, so the ground goes.
     */
    const cJSON *alt = cJSON_GetObjectItemCaseSensitive(o, "alt_baro");
    if (!cJSON_IsNumber(alt)) {
        goto done;
    }
    memset(t, 0, sizeof(*t));
    t->dst = (float)dst->valuedouble;
    t->dir = (float)dir->valuedouble;
    t->alt = (int32_t)alt->valuedouble;
    t->track = num(o, "track", 0.0f);
    t->speed = num(o, "gs", 0.0f);
    str(o, "flight", t->id, sizeof(t->id));
    if (!t->id[0]) {
        str(o, "r", t->id, sizeof(t->id));      /* registration, failing that */
    }
    str(o, "t", t->type, sizeof(t->type));
    ok = true;
done:
    cJSON_Delete(o);
    return ok;
}

/*
 * Reads the body as it arrives and hands each aircraft object off one at a
 * time. Nothing holds the whole response: the 615 KB New York sweep goes
 * through the same 2 KB as a 3 KB Panama one.
 */
static bool stream_objects(esp_http_client_handle_t c, muse_radar_sweep_t *s)
{
    char *chunk = malloc(CHUNK);
    char *obj = malloc(OBJ_MAX);
    if (!chunk || !obj) {
        free(chunk);
        free(obj);
        ESP_LOGE(TAG, "out of memory");
        return false;
    }
    int depth = 0;          /* brace depth; aircraft objects sit at 1 */
    size_t olen = 0;
    bool in_str = false, esc = false, over = false;
    bool saw_any = false;

    for (;;) {
        int n = esp_http_client_read(c, chunk, CHUNK);
        if (n < 0) {
            break;
        }
        if (n == 0) {
            saw_any = true;     /* a clean end of body */
            break;
        }
        for (int i = 0; i < n; i++) {
            char ch = chunk[i];
            /*
             * The aircraft sit at depth 3: the root object, the "ac" array,
             * then each object inside it. Depth is updated before the
             * character is kept so an object's own braces land in its buffer.
             */
            if (in_str) {
                if (esc) {
                    esc = false;
                } else if (ch == '\\') {
                    esc = true;
                } else if (ch == '"') {
                    in_str = false;
                }
            } else if (ch == '"') {
                in_str = true;
            } else if (ch == '{' || ch == '[') {
                depth++;
                if (depth == 3 && ch == '{') {
                    olen = 0;
                    over = false;
                }
            } else if (ch == '}' || ch == ']') {
                if (depth >= 3 && olen < OBJ_MAX) {
                    obj[olen++] = ch;
                }
                depth--;
                if (depth == 2 && ch == '}') {
                    s->total++;
                    muse_radar_target_t t;
                    if (!over && olen < OBJ_MAX && aircraft(obj, olen, &t)) {
                        muse_radar_keep(s, &t);
                    }
                    olen = 0;
                }
                continue;
            }
            if (depth >= 3) {
                if (olen < OBJ_MAX - 1) {
                    obj[olen++] = ch;
                } else {
                    over = true;
                }
            }
        }
    }
    free(chunk);
    free(obj);
    return saw_any;
}

static bool fetch_planes(muse_radar_sweep_t *s)
{
    const muse_radar_place_t *p = muse_radar_place_at();
    char url[160];
    snprintf(url, sizeof(url), "https://api.adsb.lol/v2/point/%.4f/%.4f/%d",
             (double)p->lat, (double)p->lon, muse_radar_range_nm());

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .user_agent = "muse-gadget-radar/1.0 (ESP32; github.com/RaxLinOrg/muse-gadget)",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return false;
    }
    bool ok = false;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        ESP_LOGW(TAG, "connect failed");
        goto done;
    }
    if (esp_http_client_fetch_headers(c) < 0) {
        ESP_LOGW(TAG, "no headers");
        goto done;
    }
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        /* 429 is the daily quota; it is worth saying so in as many words. */
        ESP_LOGW(TAG, "HTTP %d%s", status,
                 status == 429 ? " -- out of requests for today" : "");
        goto done;
    }
    ok = stream_objects(c, s);
done:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

static void radar_task(void *arg)
{
    (void)arg;
    uint32_t next = 0;
    bool mode_was_ships = false;
    for (;;) {
        uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
        bool due = s_want || up >= next;
        if (due && s_visible && muse_radar_enabled() && muse_wifi_connected()) {
            s_want = false;
            muse_radar_mode_t mode = muse_radar_mode();
            if (mode == MUSE_RADAR_PLANES) {
                muse_radar_sweep_t s = { 0 };
                if (fetch_planes(&s)) {
                    s.valid = true;
                    s.at = up;
                    portENTER_CRITICAL(&s_lock);
                    s_sweep = s;
                    portEXIT_CRITICAL(&s_lock);
                    next = up + (uint32_t)muse_radar_period_s();
                    ESP_LOGI(TAG, "%s: %d of %d aircraft kept",
                             muse_radar_place_at()->name, s.n, s.total);
                } else {
                    portENTER_CRITICAL(&s_lock);
                    s_sweep.stale = true;
                    portEXIT_CRITICAL(&s_lock);
                    next = up + RETRY_EVERY_S;
                }
            } else if (mode == MUSE_RADAR_WEATHER) {
                next = up + (muse_radar_rain_fetch()
                             ? (uint32_t)muse_radar_period_s() : RETRY_EVERY_S);
            } else {
                /* AIS pushes rather than answers, so there is nothing to
                 * time: keep the socket up and read off what has arrived. */
                muse_radar_ais_poll();
                muse_radar_sweep_t s;
                if (muse_radar_ais_snapshot(&s)) {
                    portENTER_CRITICAL(&s_lock);
                    s_sweep = s;
                    portEXIT_CRITICAL(&s_lock);
                }
                next = up + 2;
            }
        }
        if (mode_was_ships && (!s_visible || muse_radar_mode() != MUSE_RADAR_SHIPS)) {
            muse_radar_ais_stop();
            mode_was_ships = false;
        } else if (s_visible && muse_radar_mode() == MUSE_RADAR_SHIPS) {
            mode_was_ships = true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void muse_radar_net_start(void)
{
    if (s_task) {
        return;
    }
    muse_radar_range_index();
    muse_radar_mode();
    muse_radar_place();
    /* cJSON and mbedTLS both want room; PSRAM keeps it off the internal heap. */
    if (xTaskCreateWithCaps(radar_task, "radar", TASK_STACK, NULL, 3, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "could not start the task");
        s_task = NULL;
    }
}
