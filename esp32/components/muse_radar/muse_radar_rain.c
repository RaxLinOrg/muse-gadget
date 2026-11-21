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
#include "esp_timer.h"
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
#define CLOUD_ZOOM_MAX 6
#define ZOOM_MIN  2
/*
 * A rain tile is mostly empty and compresses to a few KB. An infrared tile is
 * a photograph of the whole sky and does not: New York at z=5 is 97 KB, and
 * at 64 KB it arrived truncated and libpng said "read beyond end of data".
 */
#define PNG_MAX   (192 * 1024)
#define HTTP_TIMEOUT_MS 10000

#define PX  MUSE_RADAR_TILE_PX

static uint16_t *s_rain;        /* PX*PX RGB565, 0 where the sky is empty */
static uint8_t  *s_grey;        /* clouds pass: raw infrared, 0 = no data */
static bool s_fresh;
static int s_wet;               /* scope pixels with rain in them */
static int s_cloud;             /* scope pixels with cloud in them */
static bool s_cloud_seen;       /* a satellite answered for this place at all */
static int s_zoom;              /* the zoom the rain pass was cut at */
/*
 * Screen pixel to web-mercator tile coordinate, worked out once a pass.
 *
 * Measured, this was the whole cost of a weather fetch: 30 s of the ~35 s a
 * pass took, against 0.3 s to pull the bytes down. The old inner loop ran
 * merc_y() -- a double-precision sin() and log(), both software on this chip
 * -- plus two divisions for every one of the ~167,000 pixels inside the
 * scope, once per tile, four tiles a pass. 1.3 million of them a cycle.
 *
 * None of it depended on the tile, and the mapping separates: the column a
 * screen pixel lands in depends only on its x, the row only on its y. So it
 * is two arrays of 466 floats, filled once, and the inner loop becomes two
 * lookups and a subtraction.
 */
static float *s_mapx, *s_mapy;  /* in tile units: whole part is the tile */

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
static int permille(int px)
{
    if (!s_fresh) {
        return 0;
    }
    /* Against the scope's area, not the square the pixels live in. */
    int area = (int)(3.14159f * (PX / 2) * (PX / 2));
    return area > 0 ? (int)((long long)px * 1000 / area) : 0;
}

int muse_radar_rain_permille(void)
{
    return permille(s_wet);
}

int muse_radar_cloud_permille(void)
{
    return permille(s_cloud);
}

/*
 * Whether any infrared reached the scope. Nothing painted can mean clear sky
 * or it can mean no satellite sees this place -- the Indian Ocean has no GIBS
 * disc at all -- and the card must not pick one on the reader's behalf.
 */
bool muse_radar_cloud_seen(void)
{
    return s_cloud_seen;
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
    s_cloud = 0;
    s_cloud_seen = false;
}

/* --------------------------------------------------------------- fetch */

/* Blocking GET into buf; returns the length or -1. */
/* Where a fetch's time actually goes, summed over a pass. */
static int64_t s_t_open, s_t_read, s_t_blit;

/*
 * The four tiles of a pass come from one host, so they share one client.
 * Measured, a fresh TLS session costs 3.5 s on this chip and the pass opened
 * four of them: 14 s of RainViewer's 15 s pass was handshakes.
 *
 * Note that keep_alive_enable alone does not help here. Reading IDF's
 * esp_http_client.c, esp_http_client_close() calls esp_transport_close(), and
 * esp_http_client_open() sets the state to CONNECTING and then connects
 * unconditionally; the socket is only held across requests by
 * esp_http_client_perform(), which this does not use. What does carry over is
 * the TLS session ticket, which open() looks for and resumes from -- hence
 * save_client_session here and CONFIG_ESP_TLS_CLIENT_SESSION_TICKETS in
 * sdkconfig.defaults. The first tile pays the full handshake and the rest
 * resume.
 */
static esp_http_client_handle_t open_client(const char *url)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .save_client_session = true,
        .user_agent = "muse-gadget-radar/1.0 (ESP32; github.com/RaxLinOrg/muse-gadget)",
    };
    return esp_http_client_init(&cfg);
}

static int get(esp_http_client_handle_t c, const char *url,
               uint8_t *buf, size_t cap)
{
    int64_t t0 = esp_timer_get_time();
    if (!c) {
        return -1;
    }
    int got = -1;
    if (esp_http_client_set_url(c, url) != ESP_OK) {
        return -1;
    }
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
    int64_t t1 = esp_timer_get_time();
    s_t_open += t1 - t0;
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
    s_t_read += esp_timer_get_time() - t1;
done:
    /* Closed but not destroyed: close() parks the session so the next
     * set_url + open on the same host reuses it. */
    esp_http_client_close(c);
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
    const char *url = "https://api.rainviewer.com/public/weather-maps.json";
    esp_http_client_handle_t c = open_client(url);
    int n = get(c, url, buf, 2047);
    if (c) {
        esp_http_client_cleanup(c);
    }
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

/*
 * Clouds are a different quantity and get a different palette. GIBS serves
 * clean infrared as greyscale, where white is cold -- which is to say high
 * cloud top -- and dark is warm ground. Everything below CLOUD_FLOOR is
 * treated as clear sky and left transparent, so the coastline still shows
 * through; what is left is ramped cool, so a glance never mistakes cloud for
 * rain on the same green scope.
 */
/*
 * Cloud is whatever is colder than the ground under it, and no fixed
 * greyscale value says where that is. Clean infrared is a brightness
 * temperature picture, so the clear-sky baseline moves with the local
 * surface: measured inside the scope, the median pixel is 0.36 over Panama,
 * 0.42 over the Taiwan Strait and 0.53 over New York. A single cut-off is
 * therefore wrong twice over -- 0.38 lit the entire scope at every place
 * (167,601 of 167,650 pixels, a solid wash with the coastline buried), and
 * 0.62 lit almost nothing anywhere (0 px at three places out of four).
 *
 * So the floor is found per picture: the median of the pixels actually inside
 * the scope, plus a margin. What is left is the cold tail, which is the
 * cloud. Over the Bay Area at 40 NM that correctly comes to nearly nothing --
 * the scope's whole brightness range was 0.42 to 0.57 -- while the same place
 * at 250 NM reaches 0.77 and shows real cloud.
 */
#define CLOUD_MARGIN  0.10f

static uint16_t cloud_ramp(float s)
{
    static const float stop[] = { 0.00f, 0.35f, 0.70f, 1.00f };
    static const uint8_t col[][3] = {
        { 28, 58, 78 }, { 86, 118, 148 }, { 168, 194, 214 }, { 245, 250, 255 },
    };
    if (s < 0.0f) {
        s = 0.0f;
    }
    if (s > 1.0f) {
        s = 1.0f;
    }
    for (int i = 0; i < 3; i++) {
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

/*
 * Which geostationary satellite can see a place. GIBS carries three clean-IR
 * discs and no Meteosat, so the Indian Ocean falls between them: Hormuz sits
 * on Himawari's western limb and usually comes back empty, which the page
 * says rather than pretending the sky is clear.
 */
static const char *cloud_layer(double lon)
{
    if (lon >= 40.0 || lon <= -150.0) {
        return "Himawari_AHI_Band13_Clean_Infrared";
    }
    if (lon <= -100.0) {
        return "GOES-West_ABI_Band13_Clean_Infrared";
    }
    return "GOES-East_ABI_Band13_Clean_Infrared";
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
static bool blit_tile(const uint8_t *png, size_t len, int tx, int ty,
                      bool clouds)
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

    int ctr = PX / 2, rad = PX / 2 - 2;

    for (int y = 0; y < PX; y++) {
        float fy = (s_mapy[y] - (float)ty) * (float)h;
        if (fy < 0.0f || fy >= (float)h) {
            continue;           /* this row belongs to another tile */
        }
        int py = (int)fy;
        int dy = y - ctr;
        int span = rad * rad - dy * dy;
        if (span < 0) {
            continue;
        }
        for (int x = 0; x < PX; x++) {
            int dx = x - ctr;
            if (dx * dx > span) {
                continue;
            }
            float fx = (s_mapx[x] - (float)tx) * (float)w;
            if (fx < 0.0f || fx >= (float)w) {
                continue;       /* belongs to one of the other three tiles */
            }
            const uint8_t *q = rgba + ((size_t)py * w + (int)fx) * 4;
            int a = q[3];
            if (a < 30) {
                continue;
            }
            if (clouds) {
                /* Greyscale: one channel is the whole signal. Keep it raw;
                 * the floor cannot be known until every tile has landed. */
                if (s_grey) {
                    s_grey[y * PX + x] = (uint8_t)(q[0] < 255 ? q[0] + 1 : 255);
                }
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

/*
 * One fetch for both pictures. The difference is where the tiles come from
 * and how their pixels are read:
 *
 *   rain   -- RainViewer, whose index has to be fetched first for the newest
 *             frame's path, max zoom 7.
 *   clouds -- NASA GIBS clean infrared, where "default" as the time means the
 *             newest frame, so there is no index call at all. The tile matrix
 *             set tops out at zoom 6, and the path puts row before column.
 *
 * RainViewer discontinued its own satellite layer on 2026-01-01, which is why
 * clouds come from somewhere else entirely.
 */
static bool fetch_layer(bool clouds, bool keep)
{
    if (!s_rain) {
        s_rain = heap_caps_calloc((size_t)PX * PX, 2, MALLOC_CAP_SPIRAM);
        if (!s_rain) {
            ESP_LOGE(TAG, "no room for the rain layer");
            return false;
        }
    }
    char path[64];
    if (!clouds && !latest_path(path, sizeof(path))) {
        ESP_LOGW(TAG, "no frame index");
        return false;
    }
    const muse_radar_place_t *p = muse_radar_place_at();
    int range = muse_radar_range_nm();
    if (range <= 0) {
        return false;
    }
    int z = pick_zoom(p->lat, range);
    if (clouds && z > CLOUD_ZOOM_MAX) {
        z = CLOUD_ZOOM_MAX;     /* GoogleMapsCompatible_Level6 stops here */
    }
    double n = pow(2.0, z);
    if (!s_mapx) {
        s_mapx = heap_caps_malloc(PX * sizeof(float), MALLOC_CAP_SPIRAM);
        s_mapy = heap_caps_malloc(PX * sizeof(float), MALLOC_CAP_SPIRAM);
        if (!s_mapx || !s_mapy) {
            ESP_LOGE(TAG, "no room for the projection table");
            return false;
        }
    }
    {
        int ctr = PX / 2, rad = PX / 2 - 2;
        double coslat = cos(p->lat * M_PI / 180.0);
        for (int i = 0; i < PX; i++) {
            double d = (double)(i - ctr) / rad * range;
            double lon = p->lon + d / (60.0 * coslat);
            double lat = p->lat - d / 60.0;     /* screen y runs south */
            s_mapx[i] = (float)((lon + 180.0) / 360.0 * n);
            s_mapy[i] = (float)(merc_y(lat) * n);
        }
    }
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
    /* Opened on the first tile's URL and kept for the other three. */
    esp_http_client_handle_t http = NULL;
    if (!keep) {
        memset(s_rain, 0, (size_t)PX * PX * 2);
    }
    if (clouds) {
        s_cloud = 0;
    } else {
        s_wet = 0;
        s_zoom = z;
    }
    s_t_open = s_t_read = s_t_blit = 0;
    if (clouds) {
        /* Freed before this call returns; 217 KB of PSRAM for the time it
         * takes to decide where the cloud starts. */
        s_grey = heap_caps_calloc((size_t)PX * PX, 1, MALLOC_CAP_SPIRAM);
        if (!s_grey) {
            ESP_LOGE(TAG, "no room for the infrared scratch");
            free(png);
            return false;
        }
    }
    int wrap = (int)n;
    int ok = 0;
    for (int j = 0; j < 2; j++) {
        for (int i = 0; i < 2; i++) {
            int tx = ((x0 + i) % wrap + wrap) % wrap;
            int ty = y0 + j;
            if (ty < 0 || ty >= wrap) {
                continue;
            }
            char url[256];
            if (clouds) {
                snprintf(url, sizeof(url),
                         "https://gibs.earthdata.nasa.gov/wmts/epsg3857/best/%s"
                         "/default/default/GoogleMapsCompatible_Level6/%d/%d/%d.png",
                         cloud_layer(p->lon), z, ty, tx);
            } else {
                snprintf(url, sizeof(url),
                         "https://tilecache.rainviewer.com%s/%d/%d/%d/%d/4/1_1.png",
                         path, TILE_PX, z, tx, ty);
            }
            if (!http) {
                http = open_client(url);
            }
            int64_t tb = esp_timer_get_time();
            int len = get(http, url, png, PNG_MAX);
            tb = esp_timer_get_time() - tb;
            /*
             * An empty tile is a few hundred bytes and decodes to nothing,
             * which is the right answer for dry sky; only a failed request is
             * worth counting against us.
             */
            int64_t td = esp_timer_get_time();
            if (len > 0 && blit_tile(png, (size_t)len, tx, ty, clouds)) {
                ok++;
            }
            s_t_blit += esp_timer_get_time() - td;
            (void)tb;
        }
    }
    free(png);
    if (http) {
        esp_http_client_cleanup(http);
    }
    if (clouds && s_grey) {
        /* The floor is the median of what is actually on the scope, plus a
         * margin; everything above it is the cold tail. */
        int hist[256] = { 0 }, seen = 0;
        for (int i = 0; i < PX * PX; i++) {
            if (s_grey[i]) {
                hist[s_grey[i] - 1]++;
                seen++;
            }
        }
        s_cloud_seen = seen > 0;
        int med = 0;
        for (int i = 0, run = 0; i < 256; i++) {
            run += hist[i];
            if (run * 2 >= seen) {
                med = i;
                break;
            }
        }
        float floor_v = med / 255.0f + CLOUD_MARGIN;
        if (floor_v > 0.92f) {
            floor_v = 0.92f;
        }
        float span = 1.0f - floor_v;
        for (int i = 0; i < PX * PX; i++) {
            if (!s_grey[i]) {
                continue;
            }
            float b = (s_grey[i] - 1) / 255.0f;
            if (b <= floor_v) {
                continue;
            }
            s_rain[i] = cloud_ramp((b - floor_v) / span);
            s_cloud++;
        }
        ESP_LOGI(TAG, "%s clouds: median %.2f, floor %.2f, %d of %d px",
                 p->name, (double)(med / 255.0f), (double)floor_v, s_cloud, seen);
        heap_caps_free(s_grey);
        s_grey = NULL;
    }
    ESP_LOGI(TAG, "%s %s: z=%d, %d of 4 tiles, %d px, stack left %u",
             p->name, clouds ? "clouds" : "rain", z, ok,
             clouds ? s_cloud : s_wet,
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    ESP_LOGI(TAG, "%s %s timing: connect %lld ms, body %lld ms, decode %lld ms",
             p->name, clouds ? "clouds" : "rain",
             s_t_open / 1000, s_t_read / 1000, s_t_blit / 1000);
    return ok > 0;
}

/*
 * Cloud first, then rain over the top of it, into the one buffer. They are
 * two measurements of the same sky and separating them into two modes only
 * made the user press the icon more: rain without its cloud reads as an empty
 * scope most days, and cloud without its rain cannot say where the weather
 * actually is.
 *
 * Either pass may come back with nothing and the other still be worth
 * drawing -- the Indian Ocean has no infrared disc at all, and most of the
 * time most places have no rain -- so the picture counts as current if either
 * one landed, and the card reports the two coverages separately.
 */
bool muse_radar_rain_fetch(void)
{
    if (!s_rain) {
        s_rain = heap_caps_calloc((size_t)PX * PX, 2, MALLOC_CAP_SPIRAM);
        if (!s_rain) {
            ESP_LOGE(TAG, "no room for the weather layer");
            return false;
        }
    }
    memset(s_rain, 0, (size_t)PX * PX * 2);
    s_wet = 0;
    s_cloud = 0;
    s_cloud_seen = false;
    bool clouds = fetch_layer(true, true);
    bool rain = fetch_layer(false, true);
    s_fresh = clouds || rain;
    return s_fresh;
}
