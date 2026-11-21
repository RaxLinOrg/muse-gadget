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

static void radar_reset(void);

void muse_radar_set_range_index(int i)
{
    if (i < 0 || i > MUSE_RADAR_OFF || i == muse_radar_range_index()) {
        return;
    }
    s_range = i;
    nvs_put("range", i);
    /* Not just a refetch: the old sweep was gathered for the old range, so
     * every blip in it is at the wrong radius until the new one lands. */
    radar_reset();
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
    radar_reset();
}

const char *muse_radar_mode_name(muse_radar_mode_t m)
{
    switch (m) {
    case MUSE_RADAR_SHIPS:   return "Ships";
    case MUSE_RADAR_WEATHER: return "Weather";
    case MUSE_RADAR_CLOUDS:  return "Clouds";
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
    radar_reset();
}

const muse_radar_place_t *muse_radar_place_at(void)
{
    return &MUSE_RADAR_PLACES[muse_radar_place()];
}

int muse_radar_period_s(void)
{
    if (muse_radar_mode_is_layer(muse_radar_mode())) {
        /* RainViewer publishes a frame every 600 s exactly, and GIBS serves
         * the geostationary discs on the same ten-minute cadence. */
        return 600;
    }
    switch (muse_radar_range_index()) {
    case 0:  return 30;     /*  12 NM: a blip crosses the scope in 3.2 min */
    case 1:  return 60;     /*  40 NM: 10.7 min */
    default: return 300;    /* 250 NM: 67 min */
    }
}

/* ---------------------------------------------------------- the sweep */

/*
 * How many contacts the current range keeps. 80 fills a 12 or 40 NM scope;
 * 250 NM covers four hundred times the area of 12 and looked bare at the same
 * number, so it gets the whole buffer.
 */
int muse_radar_cap(void)
{
    return muse_radar_range_index() == MUSE_RADAR_RANGE_COUNT - 1
           ? MUSE_RADAR_MAX_TARGETS : 80;
}

bool muse_radar_sweep_init(muse_radar_sweep_t *s)
{
    memset(s, 0, sizeof(*s));
    s->t = heap_caps_malloc(sizeof(muse_radar_target_t) * MUSE_RADAR_MAX_TARGETS,
                            MALLOC_CAP_SPIRAM);
    return s->t != NULL;
}

/* The published sweep and the worker's scratch. Publishing swaps the two
 * pointers under the lock, so the spinlock is held for a handful of stores
 * rather than a 10.8 KB copy. */
static muse_radar_sweep_t s_sweep;
static muse_radar_sweep_t s_work;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_want = true;
static volatile bool s_visible;
static TaskHandle_t s_task;

/*
 * The caller hands in its own sweep and gets the published one back, leaving
 * its previous contacts behind for the next publish to fill. Three buffers
 * exist in all -- the worker's, the published one, and the page's -- and no
 * contact is ever copied.
 */
void muse_radar_get(muse_radar_sweep_t *out)
{
    if (!out->t || !s_sweep.t) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_sweep.seq != out->seq) {
        /* Something new: take it, and leave our old contacts behind for the
         * next publish to fill. Nothing new: the caller keeps what it has,
         * which is what stops a reader that polls faster than the worker
         * fetches from blanking the scope between sweeps. */
        muse_radar_target_t *mine = out->t;
        *out = s_sweep;
        s_sweep.t = mine;
    }
    portEXIT_CRITICAL(&s_lock);
}

/* Empty a sweep without losing the contact array it owns. */
static void sweep_clear(muse_radar_sweep_t *s)
{
    muse_radar_target_t *keep = s->t;
    memset(s, 0, sizeof(*s));
    s->t = keep;
}

/* Hand the worker's scratch to the readers and take back whatever buffer the
 * published sweep was holding. The lock covers a few stores, never contacts. */
static void publish(void)
{
    portENTER_CRITICAL(&s_lock);
    muse_radar_target_t *theirs = s_sweep.t;
    uint32_t seq = s_sweep.seq + 1;
    s_sweep = s_work;
    s_sweep.seq = seq;
    s_work.t = theirs;
    portEXIT_CRITICAL(&s_lock);
}

void muse_radar_refresh(void)
{
    s_want = true;
}

/*
 * Throws the last sweep away as well as asking for a new one. A change of
 * mode or place makes the contacts on screen wrong rather than merely old:
 * aircraft kept being drawn over the rain until the first weather tile
 * landed, because the sweep behind them was still the one planes left.
 */
static void radar_reset(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t seq = s_sweep.seq + 1;
    sweep_clear(&s_sweep);
    s_sweep.seq = seq;
    portEXIT_CRITICAL(&s_lock);
    /* The rain picture was projected for one place at one zoom, and a setting
     * change invalidates both. Dropping it is what stops the last city's
     * weather sitting under the new city's rings until the next tile lands. */
    muse_radar_rain_invalidate();
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
/*
 * Which range ring a contact falls in. Keeping the nearest 80 looked right at
 * 12 NM and wrong at 250: a busy place has 80 aircraft within 30 miles, so the
 * cap filled up long before the edge and the outer two thirds of the scope was
 * always empty. Contacts are spread across the rings instead, each ring
 * holding a share of the cap and lending what it does not use to the others.
 *
 * The range is read here rather than passed in because the worker is the only
 * caller and is always working at the current range; a range change mid-sweep
 * costs one badly distributed sweep and nothing else.
 */
#define KEEP_RINGS   4

static int ring_of(float dst)
{
    int range = muse_radar_range_nm();
    if (range <= 0) {
        return 0;
    }
    int r = (int)(dst / (float)range * KEEP_RINGS);
    return r < 0 ? 0 : (r >= KEEP_RINGS ? KEEP_RINGS - 1 : r);
}

void muse_radar_keep(muse_radar_sweep_t *s, const muse_radar_target_t *t)
{
    const int cap = muse_radar_cap();
    const int quota = cap / KEEP_RINGS;
    if (!s->t) {
        return;
    }
    if (s->n < cap) {
        int at = s->n;
        while (at > 0 && s->t[at - 1].dst > t->dst) {
            s->t[at] = s->t[at - 1];
            at--;
        }
        s->t[at] = *t;
        s->n++;
        return;
    }

    /* Full: someone has to go, and it should be whichever ring is most over
     * its share rather than simply the farthest contact on the scope. */
    int count[KEEP_RINGS] = { 0 };
    for (int i = 0; i < s->n; i++) {
        count[ring_of(s->t[i].dst)]++;
    }
    int mine = ring_of(t->dst), fat = 0;
    for (int r = 1; r < KEEP_RINGS; r++) {
        if (count[r] > count[fat]) {
            fat = r;
        }
    }
    if (count[mine] >= quota && count[mine] >= count[fat]) {
        return;         /* this ring has had its share; leave the list alone */
    }

    /* Drop the farthest contact of the fattest ring: within a ring the near
     * ones are the interesting ones, which is the old rule, ring by ring. */
    int drop = -1;
    for (int i = s->n - 1; i >= 0; i--) {
        if (ring_of(s->t[i].dst) == fat) {
            drop = i;
            break;
        }
    }
    if (drop < 0) {
        return;
    }
    memmove(&s->t[drop], &s->t[drop + 1],
            (size_t)(s->n - 1 - drop) * sizeof(s->t[0]));
    s->n--;

    int at = s->n;
    while (at > 0 && s->t[at - 1].dst > t->dst) {
        s->t[at] = s->t[at - 1];
        at--;
    }
    s->t[at] = *t;
    s->n++;
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
                sweep_clear(&s_work);
                if (fetch_planes(&s_work)) {
                    s_work.valid = true;
                    s_work.at = up;
                    int n = s_work.n, total = s_work.total;
                    publish();
                    next = up + (uint32_t)muse_radar_period_s();
                    ESP_LOGI(TAG, "%s: %d of %d aircraft kept, cap %d",
                             muse_radar_place_at()->name, n, total,
                             muse_radar_cap());
                } else {
                    portENTER_CRITICAL(&s_lock);
                    s_sweep.stale = true;
                    s_sweep.seq++;      /* the page has to see this */
                    portEXIT_CRITICAL(&s_lock);
                    next = up + RETRY_EVERY_S;
                }
            } else if (muse_radar_mode_is_layer(mode)) {
                bool got = mode == MUSE_RADAR_CLOUDS ? muse_radar_cloud_fetch()
                                                     : muse_radar_rain_fetch();
                next = up + (got ? (uint32_t)muse_radar_period_s()
                                 : RETRY_EVERY_S);
            } else {
                /* AIS pushes rather than answers, so there is nothing to
                 * time: keep the socket up and read off what has arrived. */
                muse_radar_ais_poll();
                sweep_clear(&s_work);
                if (muse_radar_ais_snapshot(&s_work)) {
                    s_work.at = up;
                    publish();
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
    /* The published sweep and the worker's scratch. Without both there is
     * nowhere to put contacts, so the page stays empty rather than crashing. */
    if (!muse_radar_sweep_init(&s_sweep) || !muse_radar_sweep_init(&s_work)) {
        ESP_LOGE(TAG, "no PSRAM for %d contacts", MUSE_RADAR_MAX_TARGETS);
        return;
    }
    /* cJSON and mbedTLS both want room; PSRAM keeps it off the internal heap. */
    if (xTaskCreateWithCaps(radar_task, "radar", TASK_STACK, NULL, 3, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "could not start the task");
        s_task = NULL;
    }
}
