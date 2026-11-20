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
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

static const char *TAG = "radar.ais";

/*
 * Ships are not a question with an answer: aisstream.io pushes positions as
 * vessels transmit them, which under the AIS standard is every 2-10 seconds
 * for anything moving. So this keeps a socket open while the page is on
 * screen, files each report into a small table, and lets the page read the
 * table whenever it redraws.
 *
 * Two things to know, both from aisstream's own documentation:
 *   - The subscription has to arrive within 3 seconds of the socket opening,
 *     and re-sending one replaces it rather than adding to it.
 *   - Since September 2026 uncompressed connections are subject to per-user
 *     bandwidth limits. ESP-IDF's websocket client does not do
 *     permessage-deflate, so a busy box may have messages dropped. A strait
 *     is small enough that this has not bitten in practice; the Atlantic
 *     would be another matter.
 */

#ifndef CONFIG_MUSE_RADAR_AIS_KEY
#define CONFIG_MUSE_RADAR_AIS_KEY ""
#endif

#define AIS_URL     "wss://stream.aisstream.io/v0/stream"
#define SHIPS_MAX   96
#define FORGET_S    600     /* a vessel unheard for ten minutes has gone */
#define RX_BUF      4096

typedef struct {
    uint32_t mmsi;
    float lat, lon;
    float sog, cog;
    uint32_t seen;          /* seconds since boot */
    char name[10];
} ship_t;

static ship_t s_ship[SHIPS_MAX];
static int s_ships;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static esp_websocket_client_handle_t s_ws;
static int s_sub_place = -1;    /* which place the live subscription covers */
static char *s_rx;
static size_t s_rx_len;

bool muse_radar_ais_have_key(void)
{
    return CONFIG_MUSE_RADAR_AIS_KEY[0] != '\0';
}

/* --------------------------------------------------------- the ship table */

static void remember(uint32_t mmsi, float lat, float lon, float sog, float cog,
                     const char *name)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    portENTER_CRITICAL(&s_lock);
    int at = -1;
    for (int i = 0; i < s_ships; i++) {
        if (s_ship[i].mmsi == mmsi) {
            at = i;
            break;
        }
    }
    if (at < 0) {
        if (s_ships < SHIPS_MAX) {
            at = s_ships++;
        } else {
            /* Replace whichever has been quiet longest. */
            at = 0;
            for (int i = 1; i < s_ships; i++) {
                if (s_ship[i].seen < s_ship[at].seen) {
                    at = i;
                }
            }
        }
        memset(&s_ship[at], 0, sizeof(s_ship[at]));
    }
    s_ship[at].mmsi = mmsi;
    s_ship[at].lat = lat;
    s_ship[at].lon = lon;
    s_ship[at].sog = sog;
    s_ship[at].cog = cog;
    s_ship[at].seen = now;
    if (name && name[0] && !s_ship[at].name[0]) {
        size_t n = strlen(name);
        while (n && name[n - 1] == ' ') {
            n--;
        }
        if (n > sizeof(s_ship[at].name) - 1) {
            n = sizeof(s_ship[at].name) - 1;
        }
        memcpy(s_ship[at].name, name, n);
        s_ship[at].name[n] = '\0';
    }
    portEXIT_CRITICAL(&s_lock);
}

/* One frame from the stream. Anything that is not a position is ignored. */
static void on_message(const char *json, size_t len)
{
    cJSON *o = cJSON_ParseWithLength(json, len);
    if (!o) {
        return;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(o, "MessageType");
    if (!cJSON_IsString(type) || strcmp(type->valuestring, "PositionReport")) {
        goto done;
    }
    const cJSON *meta = cJSON_GetObjectItemCaseSensitive(o, "MetaData");
    const cJSON *msg = cJSON_GetObjectItemCaseSensitive(o, "Message");
    const cJSON *pr = msg ? cJSON_GetObjectItemCaseSensitive(msg, "PositionReport") : NULL;
    if (!cJSON_IsObject(meta) || !cJSON_IsObject(pr)) {
        goto done;
    }
    const cJSON *mmsi = cJSON_GetObjectItemCaseSensitive(meta, "MMSI");
    const cJSON *lat = cJSON_GetObjectItemCaseSensitive(pr, "Latitude");
    const cJSON *lon = cJSON_GetObjectItemCaseSensitive(pr, "Longitude");
    if (!cJSON_IsNumber(lat) || !cJSON_IsNumber(lon)) {
        goto done;
    }
    const cJSON *sog = cJSON_GetObjectItemCaseSensitive(pr, "Sog");
    const cJSON *cog = cJSON_GetObjectItemCaseSensitive(pr, "Cog");
    const cJSON *hdg = cJSON_GetObjectItemCaseSensitive(pr, "TrueHeading");
    const cJSON *name = cJSON_GetObjectItemCaseSensitive(meta, "ShipName");
    /* Course over ground is what the hull is actually doing; true heading is
     * often 511, meaning "not available". */
    float course = cJSON_IsNumber(cog) ? (float)cog->valuedouble : 0.0f;
    if (cJSON_IsNumber(hdg) && hdg->valuedouble < 360.0) {
        course = (float)hdg->valuedouble;
    }
    remember(cJSON_IsNumber(mmsi) ? (uint32_t)mmsi->valuedouble : 0,
             (float)lat->valuedouble, (float)lon->valuedouble,
             cJSON_IsNumber(sog) ? (float)sog->valuedouble : 0.0f, course,
             cJSON_IsString(name) ? name->valuestring : NULL);
done:
    cJSON_Delete(o);
}

/* ------------------------------------------------------------- the socket */

static void subscribe(void)
{
    const muse_radar_place_t *p = muse_radar_place_at();
    int range = muse_radar_range_nm();
    if (range <= 0) {
        return;
    }
    double dlat = range / 60.0;
    double dlon = dlat / cos(p->lat * M_PI / 180.0);
    char sub[320];
    int n = snprintf(sub, sizeof(sub),
                     "{\"APIKey\":\"%s\",\"BoundingBoxes\":[[[%.4f,%.4f],[%.4f,%.4f]]],"
                     "\"FilterMessageTypes\":[\"PositionReport\"]}",
                     CONFIG_MUSE_RADAR_AIS_KEY,
                     p->lat + dlat, p->lon - dlon,
                     p->lat - dlat, p->lon + dlon);
    if (n > 0 && esp_websocket_client_send_text(s_ws, sub, n, pdMS_TO_TICKS(2000)) >= 0) {
        s_sub_place = muse_radar_place();
        ESP_LOGI(TAG, "subscribed to %s, %d nm", p->name, range);
    }
}

static void on_ws(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_websocket_event_data_t *e = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        /* aisstream wants the subscription inside three seconds. */
        subscribe();
        break;
    case WEBSOCKET_EVENT_DATA:
        if (e->op_code == 0x8) {        /* close */
            break;
        }
        if (!s_rx) {
            break;
        }
        /* Frames can arrive split; reassemble on the offset the client gives. */
        if (e->payload_offset == 0) {
            s_rx_len = 0;
        }
        if (s_rx_len + e->data_len < RX_BUF) {
            memcpy(s_rx + s_rx_len, e->data_ptr, e->data_len);
            s_rx_len += e->data_len;
            if (s_rx_len >= (size_t)e->payload_len) {
                on_message(s_rx, s_rx_len);
                s_rx_len = 0;
            }
        } else {
            s_rx_len = 0;       /* longer than any position report; drop it */
        }
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_ERROR:
        s_sub_place = -1;
        break;
    default:
        break;
    }
}

void muse_radar_ais_stop(void)
{
    if (s_ws) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
    }
    s_sub_place = -1;
    portENTER_CRITICAL(&s_lock);
    s_ships = 0;
    portEXIT_CRITICAL(&s_lock);
}

void muse_radar_ais_poll(void)
{
    if (!muse_radar_ais_have_key()) {
        return;
    }
    if (!s_rx) {
        s_rx = malloc(RX_BUF);
        if (!s_rx) {
            ESP_LOGE(TAG, "out of memory");
            return;
        }
    }
    if (!s_ws) {
        esp_websocket_client_config_t cfg = {
            .uri = AIS_URL,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .reconnect_timeout_ms = 10000,
            .network_timeout_ms = 10000,
            .buffer_size = RX_BUF,
            .task_stack = 6144,
        };
        s_ws = esp_websocket_client_init(&cfg);
        if (!s_ws) {
            return;
        }
        esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, on_ws, NULL);
        if (esp_websocket_client_start(s_ws) != ESP_OK) {
            ESP_LOGW(TAG, "could not open the stream");
            esp_websocket_client_destroy(s_ws);
            s_ws = NULL;
        }
        return;
    }
    /* The box follows the place, so a change of place needs a new one. */
    if (esp_websocket_client_is_connected(s_ws) && s_sub_place != muse_radar_place()) {
        subscribe();
    }
}

bool muse_radar_ais_snapshot(muse_radar_sweep_t *out)
{
    const muse_radar_place_t *p = muse_radar_place_at();
    float coslat = cosf(p->lat * (float)M_PI / 180.0f);
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    memset(out, 0, sizeof(*out));

    portENTER_CRITICAL(&s_lock);
    int n = s_ships;
    for (int i = 0; i < n; i++) {
        ship_t s = s_ship[i];
        portEXIT_CRITICAL(&s_lock);
        if (now - s.seen <= FORGET_S && s.mmsi) {
            float dy = (s.lat - p->lat) * 60.0f;
            float dx = (s.lon - p->lon) * 60.0f * coslat;
            muse_radar_target_t t;
            memset(&t, 0, sizeof(t));
            t.dst = sqrtf(dx * dx + dy * dy);
            t.dir = atan2f(dx, dy) * 180.0f / (float)M_PI;
            if (t.dir < 0.0f) {
                t.dir += 360.0f;
            }
            t.track = s.cog;
            t.speed = s.sog;
            out->total++;
            if (s.name[0]) {
                snprintf(t.id, sizeof(t.id), "%s", s.name);
            } else {
                snprintf(t.id, sizeof(t.id), "%lu", (unsigned long)s.mmsi);
            }
            muse_radar_keep(out, &t);
        }
        portENTER_CRITICAL(&s_lock);
    }
    portEXIT_CRITICAL(&s_lock);
    out->valid = true;
    out->at = now;
    return true;
}
