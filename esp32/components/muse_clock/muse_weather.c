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

#include "muse_weather.h"

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
#include "sdkconfig.h"

#include "muse_wifi.h"

static const char *TAG = "weather";

/* A Kconfig bool that is off is undefined, not zero. */
#ifdef CONFIG_MUSE_CLOCK_FAHRENHEIT
#define CLOCK_FAHRENHEIT 1
#else
#define CLOCK_FAHRENHEIT 0
#endif


/* Open-Meteo answers this in a few KB. 8 KB holds three days with room to
 * spare; anything longer than the cap is a sign the shape changed. */
#define BODY_MAX        8192
#define HTTP_TIMEOUT_MS 8000
#define FETCH_EVERY_S   (20 * 60)
#define RETRY_EVERY_S   60
#define TASK_STACK      6144

static muse_weather_t s_now;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_want_fetch = true;
static TaskHandle_t s_task;

bool muse_weather_fahrenheit(void)
{
    return CLOCK_FAHRENHEIT != 0;
}

void muse_weather_get(muse_weather_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_now;
    portEXIT_CRITICAL(&s_lock);
}

void muse_weather_refresh(void)
{
    s_want_fetch = true;
}

const char *muse_weather_sky_name(muse_sky_t sky)
{
    switch (sky) {
    case MUSE_SKY_CLEAR:  return "CLEAR";
    case MUSE_SKY_PARTLY: return "PARTLY CLOUDY";
    case MUSE_SKY_CLOUD:  return "CLOUDY";
    case MUSE_SKY_FOG:    return "FOG";
    case MUSE_SKY_RAIN:   return "RAIN";
    case MUSE_SKY_SNOW:   return "SNOW";
    case MUSE_SKY_STORM:  return "THUNDERSTORMS";
    default:              return "";
    }
}

/* WMO 4677, as Open-Meteo documents the subset it returns. */
static muse_sky_t sky_of(int code)
{
    switch (code) {
    case 0:  return MUSE_SKY_CLEAR;
    case 1:
    case 2:  return MUSE_SKY_PARTLY;
    case 3:  return MUSE_SKY_CLOUD;
    case 45:
    case 48: return MUSE_SKY_FOG;
    case 71: case 73: case 75: case 77:
    case 85: case 86: return MUSE_SKY_SNOW;
    case 95: case 96: case 99: return MUSE_SKY_STORM;
    default: return MUSE_SKY_RAIN;   /* 51-67 drizzle and rain, 80-82 showers */
    }
}

static int round_num(const cJSON *item, bool *ok)
{
    if (!cJSON_IsNumber(item)) {
        *ok = false;
        return 0;
    }
    return (int)lroundf((float)item->valuedouble);
}

/* An array of numbers under daily[], rounded into out[0..MUSE_WEATHER_DAYS). */
static bool day_array(const cJSON *daily, const char *key, int *out)
{
    const cJSON *arr = cJSON_GetObjectItem(daily, key);
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < MUSE_WEATHER_DAYS) {
        ESP_LOGW(TAG, "daily.%s missing or short", key);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < MUSE_WEATHER_DAYS; i++) {
        out[i] = round_num(cJSON_GetArrayItem(arr, i), &ok);
    }
    return ok;
}

static bool parse(const char *body, size_t len, muse_weather_t *out)
{
    cJSON *root = cJSON_ParseWithLength(body, len);
    if (!root) {
        ESP_LOGW(TAG, "body is not JSON");
        return false;
    }
    bool ok = true;
    const cJSON *cur = cJSON_GetObjectItem(root, "current");
    const cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (!cJSON_IsObject(cur) || !cJSON_IsObject(daily)) {
        /* Open-Meteo reports its own errors as {"error":true,"reason":...}. */
        const cJSON *why = cJSON_GetObjectItem(root, "reason");
        ESP_LOGW(TAG, "no forecast: %s",
                 cJSON_IsString(why) ? why->valuestring : "unexpected shape");
        cJSON_Delete(root);
        return false;
    }
    out->temp = round_num(cJSON_GetObjectItem(cur, "temperature_2m"), &ok);
    out->sky = sky_of(round_num(cJSON_GetObjectItem(cur, "weather_code"), &ok));

    int codes[MUSE_WEATHER_DAYS];
    ok = day_array(daily, "temperature_2m_max", out->hi) && ok;
    ok = day_array(daily, "temperature_2m_min", out->lo) && ok;
    ok = day_array(daily, "weather_code", codes) && ok;
    for (int i = 0; i < MUSE_WEATHER_DAYS; i++) {
        out->day_sky[i] = sky_of(codes[i]);
    }

    /* timezone=auto, so this is the forecast location's offset: the clock
     * takes its timezone from here and nothing has to be configured twice. */
    const cJSON *off = cJSON_GetObjectItem(root, "utc_offset_seconds");
    out->utc_offset = cJSON_IsNumber(off) ? (int32_t)off->valuedouble : 0;

    cJSON_Delete(root);
    if (!ok) {
        ESP_LOGW(TAG, "a field was missing or not a number");
    }
    return ok;
}

/* Blocking GET into buf. Returns the body length, or -1. */
static int fetch(char *buf, size_t cap)
{
    char url[320];
    snprintf(url, sizeof(url),
             "https://api.open-meteo.com/v1/forecast"
             "?latitude=%s&longitude=%s"
             "&current=temperature_2m,weather_code"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min"
             "&timezone=auto&forecast_days=%d%s",
             CONFIG_MUSE_CLOCK_LATITUDE, CONFIG_MUSE_CLOCK_LONGITUDE,
             MUSE_WEATHER_DAYS,
             CLOCK_FAHRENHEIT ? "&temperature_unit=fahrenheit" : "");

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        return -1;
    }
    int got = -1;
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "connect: %s", esp_err_to_name(err));
        goto done;
    }
    if (esp_http_client_fetch_headers(c) < 0) {
        ESP_LOGW(TAG, "no headers");
        goto done;
    }
    int status = esp_http_client_get_status_code(c);
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d", status);
        goto done;
    }
    size_t len = 0;
    while (len < cap - 1) {
        int n = esp_http_client_read(c, buf + len, (int)(cap - 1 - len));
        if (n < 0) {
            ESP_LOGW(TAG, "read failed after %u bytes", (unsigned)len);
            goto done;
        }
        if (n == 0) {
            break;          /* the body ended */
        }
        len += (size_t)n;
    }
    buf[len] = '\0';
    got = (int)len;
done:
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return got;
}

static void weather_task(void *arg)
{
    (void)arg;
    char *buf = malloc(BODY_MAX);
    if (!buf) {
        ESP_LOGE(TAG, "out of memory");
        s_task = NULL;
        vTaskDelete(NULL);
        return;
    }
    uint32_t next = 0;
    for (;;) {
        uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
        bool due = s_want_fetch || up >= next;
        if (due && muse_wifi_connected()) {
            s_want_fetch = false;
            int n = fetch(buf, BODY_MAX);
            muse_weather_t w = { 0 };
            if (n > 0 && parse(buf, (size_t)n, &w)) {
                w.valid = true;
                w.fetched_at = up;
                portENTER_CRITICAL(&s_lock);
                s_now = w;
                portEXIT_CRITICAL(&s_lock);
                next = up + FETCH_EVERY_S;
                ESP_LOGI(TAG, "%d deg now, today %d/%d, offset %ld s",
                         w.temp, w.hi[0], w.lo[0], (long)w.utc_offset);
            } else {
                next = up + RETRY_EVERY_S;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void muse_weather_start(void)
{
    if (s_task) {
        return;
    }
    /* The JSON parse and mbedTLS both want room; PSRAM keeps it off the
     * internal heap, the way the TTS worker does. */
    if (xTaskCreateWithCaps(weather_task, "weather", TASK_STACK, NULL, 3, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "could not start the task");
        s_task = NULL;
    }
}
