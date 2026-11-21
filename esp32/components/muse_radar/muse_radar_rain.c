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

#include "muse_radar.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "png.h"

static const char *TAG = "radar.rain";

/*
 * Weather is a picture, not a list of contacts: one RainViewer radar tile,
 * decoded, recoloured and reprojected into the scope.
 *
 * Two measured facts shape this (plan/radar.md):
 *   - RainViewer publishes a frame every 600 s exactly, so there is nothing
 *     to gain from asking more often.
 *   - Zoom 8 and above answers with a "Zoom Level Not Supported" placeholder
 *     rather than data, so 7 is as tight as it goes and the 12 NM range is a
 *     magnified z=7 tile rather than more detail.
 *
 * A 2x2 block of tiles is fetched because the scope's centre lands anywhere
 * within its tile, and a single tile would miss whichever way it straddles.
 * Only one tile is decoded at a time, so the cost is one 256 px RGBA image.
 */
#define TILE_PX   256
#define ZOOM_MAX  7
#define ZOOM_MIN  2
#define PNG_MAX   (64 * 1024)
#define HTTP_TIMEOUT_MS 10000

#define PX  MUSE_RADAR_TILE_PX

static uint16_t *s_rain;        /* PX*PX RGB565, 0 where it is dry */
static bool s_fresh;
static int s_wet;               /* scope pixels with rain in them */
static int s_zoom;              /* the zoom the current picture was cut at */

const uint16_t *muse_radar_rain(void)
{
    return s_fresh ? s_rain : NULL;
}

bool muse_radar_rain_fresh(void)
{
    return s_fresh;
}

/*
 * How much of the scope has rain on it, in tenths of a percent. A picture
 * landing and a picture having anything in it are different questions, and
 * the card used to answer the first while claiming to answer the second: dry
 * sky over the whole state still read "RAIN".
 */
int muse_radar_rain_permille(void)
{
    if (!s_fresh) {
        return 0;
    }
    /* Against the scope's area, not the square the pixels live in. */
    int area = (int)(3.14159f * (PX / 2) * (PX / 2));
    return area > 0 ? (int)((long long)s_wet * 1000 / area) : 0;
}

int muse_radar_rain_zoom(void)
{
    return s_zoom;
}

/* A setting changed: the picture belongs to the place and zoom it was cut
 * for, so it stops counting as current until the next one lands. */
void muse_radar_rain_invalidate(void)
{
    s_fresh = false;
    s_wet = 0;
}

/* --------------------------------------------------------------- fetch */

/* Blocking GET into buf; returns the length or -1. */
static int get(const char *url, uint8_t *buf, size_t cap)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
        .user_agent = "muse-gadget-radar/1.0 (ESP32; github.com/RaxLinOrg/muse-gadget)",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return -1;
    }
    int got = -1;
    if (esp_http_client_open(c, 0) != ESP_OK) {
        goto done;
    }
    if (esp_http_client_fetch_headers(c) < 0) {
        goto done;
    }
    if (esp_http_client_get_status_code(c) != 200) {
        ESP_LOGW(TAG, "HTTP %d", esp_http_client_get_status_code(c));
        goto done;
    }
    size_t len = 0;
    while (len < cap) {
        int n = esp_http_client_read(c, (char *)buf + len, (int)(cap - len));
        if (n < 0) {
            goto done;
        }
        if (n == 0) {
            break;
        }
        len += (size_t)n;
    }
    got = (int)len;
done:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return got;
}

/* The newest radar frame's path, e.g. "/v2/radar/b88904175c6c". */
static bool latest_path(char *out, size_t cap)
{
    uint8_t *buf = malloc(2048);
    if (!buf) {
        return false;
    }
    bool ok = false;
    int n = get("https://api.rainviewer.com/public/weather-maps.json", buf, 2047);
    if (n <= 0) {
        goto done;
    }
    buf[n] = '\0';
    /*
     * The index is small and its shape is fixed; the last "path" in it is the
     * newest past frame. Scanning for it beats parsing the whole document.
     */
    const char *p = (const char *)buf, *last = NULL;
    while ((p = strstr(p, "\"path\":\"")) != NULL) {
        p += 8;
        last = p;
    }
    if (!last) {
        goto done;
    }
    const char *end = strchr(last, '"');
    if (!end || (size_t)(end - last) >= cap) {
        goto done;
    }
    memcpy(out, last, (size_t)(end - last));
    out[end - last] = '\0';
    ok = true;
done:
    free(buf);
    return ok;
}

/* --------------------------------------------------------------- colour */

/*
 * RainViewer's own palette is a blue ramp. The page recolours it to the
 * green/yellow/red a radar is read in -- which costs nothing, since the
 * pixels are being walked anyway.
 */
static uint16_t ramp(float s)
{
    static const float stop[] = { 0.00f, 0.25f, 0.45f, 0.62f, 0.78f, 0.90f, 1.00f };
    static const uint8_t col[][3] = {
        { 10, 90, 40 }, { 40, 190, 70 }, { 150, 225, 55 }, { 245, 220, 60 },
        { 245, 150, 45 }, { 230, 60, 45 }, { 190, 30, 90 },
    };
    if (s < 0.0f) {
        s = 0.0f;
    }
    if (s > 1.0f) {
        s = 1.0f;
    }
    for (int i = 0; i < 6; i++) {
        if (s <= stop[i + 1]) {
            float f = (s - stop[i]) / (stop[i + 1] - stop[i]);
            int r = (int)(col[i][0] + f * (col[i + 1][0] - col[i][0]));
            int g = (int)(col[i][1] + f * (col[i + 1][1] - col[i][1]));
            int b = (int)(col[i][2] + f * (col[i + 1][2] - col[i][2]));
            return (uint16_t)(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
        }
    }
    return 0;
}

/* --------------------------------------------------- tiles and projection */

static double merc_y(double lat_deg)
{
    double s = sin(lat_deg * M_PI / 180.0);
    return 0.5 - log((1.0 + s) / (1.0 - s)) / (4.0 * M_PI);
}

static int pick_zoom(double lat, int range_nm)
{
    /* A 2x2 block has to span the whole scope: 2 * tile_nm >= 2 * range. */
    double want = 360.0 * 60.0 * cos(lat * M_PI / 180.0) / (double)range_nm;
    int z = (int)floor(log2(want));
    if (z > ZOOM_MAX) {
        z = ZOOM_MAX;
    }
    if (z < ZOOM_MIN) {
        z = ZOOM_MIN;
    }
    return z;
}

/* Decode one tile and drop whatever of it lands inside the scope. */
static bool blit_tile(const uint8_t *png, size_t len, int tx, int ty, int z,
                      double lat0, double lon0, int range_nm)
{
    png_image img;
    memset(&img, 0, sizeof(img));
    img.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&img, png, len)) {
        ESP_LOGW(TAG, "not a PNG: %s", img.message);
        return false;
    }
    img.format = PNG_FORMAT_RGBA;
    size_t need = PNG_IMAGE_SIZE(img);
    uint8_t *rgba = heap_caps_malloc(need, MALLOC_CAP_SPIRAM);
    if (!rgba) {
        png_image_free(&img);
        ESP_LOGE(TAG, "no room to decode %ux%u", (unsigned)img.width, (unsigned)img.height);
        return false;
    }
    if (!png_image_finish_read(&img, NULL, rgba, 0, NULL)) {
        ESP_LOGW(TAG, "decode failed: %s", img.message);
        free(rgba);
        return false;
    }
    int w = (int)img.width, h = (int)img.height;

    double n = pow(2.0, z);
    double world = (double)w * n;           /* the tile's own pixel size */
    double ox = (double)tx * w, oy = (double)ty * h;
    double coslat = cos(lat0 * M_PI / 180.0);
    int ctr = PX / 2, rad = PX / 2 - 2;

    for (int y = 0; y < PX; y++) {
        for (int x = 0; x < PX; x++) {
            int dx = x - ctr, dy = y - ctr;
            if (dx * dx + dy * dy > rad * rad) {
                continue;
            }
            double nx = (double)dx / rad * range_nm;
            double ny = -(double)dy / rad * range_nm;
            double lat = lat0 + ny / 60.0;
            double lon = lon0 + nx / (60.0 * coslat);
            double px_x = (lon + 180.0) / 360.0 * world - ox;
            double px_y = merc_y(lat) * world - oy;
            if (px_x < 0 || px_x >= w || px_y < 0 || px_y >= h) {
                continue;       /* belongs to one of the other three tiles */
            }
            const uint8_t *q = rgba + (((size_t)(int)px_y) * w + (int)px_x) * 4;
            int a = q[3];
            if (a < 30) {
                continue;
            }
            int mx = q[0] > q[1] ? (q[0] > q[2] ? q[0] : q[2]) : (q[1] > q[2] ? q[1] : q[2]);
            int mn = q[0] < q[1] ? (q[0] < q[2] ? q[0] : q[2]) : (q[1] < q[2] ? q[1] : q[2]);
            float light = mx / 255.0f;
            float sat = (mx - mn) / 255.0f;
            float s = (a / 255.0f) * (0.30f * (1.0f - light) + 0.95f * sat);
            if (s < 0.04f) {
                continue;
            }
            s_rain[y * PX + x] = ramp(s);
            s_wet++;
        }
    }
    free(rgba);
    png_image_free(&img);
    return true;
}

bool muse_radar_rain_fetch(void)
{
    if (!s_rain) {
        s_rain = heap_caps_calloc((size_t)PX * PX, 2, MALLOC_CAP_SPIRAM);
        if (!s_rain) {
            ESP_LOGE(TAG, "no room for the rain layer");
            return false;
        }
    }
    char path[64];
    if (!latest_path(path, sizeof(path))) {
        ESP_LOGW(TAG, "no frame index");
        return false;
    }
    const muse_radar_place_t *p = muse_radar_place_at();
    int range = muse_radar_range_nm();
    if (range <= 0) {
        return false;
    }
    int z = pick_zoom(p->lat, range);
    double n = pow(2.0, z);
    double fx = ((double)p->lon + 180.0) / 360.0 * n;
    double fy = merc_y(p->lat) * n;
    int cx = (int)floor(fx), cy = (int)floor(fy);
    /* Take the 2x2 the centre sits nearest the corner of. */
    int x0 = (fx - cx) < 0.5 ? cx - 1 : cx;
    int y0 = (fy - cy) < 0.5 ? cy - 1 : cy;

    uint8_t *png = heap_caps_malloc(PNG_MAX, MALLOC_CAP_SPIRAM);
    if (!png) {
        return false;
    }
    memset(s_rain, 0, (size_t)PX * PX * 2);
    s_wet = 0;
    s_zoom = z;
    int wrap = (int)n;
    int ok = 0;
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            int tx = ((x0 + i) % wrap + wrap) % wrap;
            int ty = y0 + j;
            if (ty < 0 || ty >= wrap) {
                continue;
            }
            char url[160];
            snprintf(url, sizeof(url),
                     "https://tilecache.rainviewer.com%s/%d/%d/%d/%d/4/1_1.png",
                     path, TILE_PX, z, tx, ty);
            int len = get(url, png, PNG_MAX);
            /*
             * An empty tile is a few hundred bytes and decodes to nothing,
             * which is the right answer for dry sky; only a failed request is
             * worth counting against us.
             */
            if (len > 0 && blit_tile(png, (size_t)len, tx, ty, z,
                                     p->lat, p->lon, range)) {
                ok++;
            }
        }
    }
    free(png);
    s_fresh = ok > 0;
    ESP_LOGI(TAG, "%s: z=%d, %d of 4 tiles, %d wet px, stack left %u",
             p->name, z, ok, s_wet, (unsigned)uxTaskGetStackHighWaterMark(NULL));
    return s_fresh;
}
