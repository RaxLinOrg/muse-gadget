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

/* Today and the two days after it, from Open-Meteo. No API key exists to
 * leak: the forecast endpoint is open, and the only configuration is where
 * you are. */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MUSE_WEATHER_DAYS 3

/* WMO 4677 codes, grouped down to what a 16 px icon can actually say apart. */
typedef enum {
    MUSE_SKY_CLEAR = 0,
    MUSE_SKY_PARTLY,
    MUSE_SKY_CLOUD,
    MUSE_SKY_FOG,
    MUSE_SKY_RAIN,
    MUSE_SKY_SNOW,
    MUSE_SKY_STORM,
    MUSE_SKY_COUNT,
} muse_sky_t;

typedef struct {
    bool valid;
    int temp;                       /* now, rounded, in the configured unit */
    muse_sky_t sky;                 /* now */
    int hi[MUSE_WEATHER_DAYS];      /* [0] is today */
    int lo[MUSE_WEATHER_DAYS];
    muse_sky_t day_sky[MUSE_WEATHER_DAYS];
    int32_t utc_offset;             /* seconds, as the forecast's own location reports it */
    uint32_t fetched_at;            /* uptime seconds of the last good answer */
} muse_weather_t;

/* Starts the task that keeps the forecast fresh. Safe to call before Wi-Fi
 * is up: it waits, then retries on its own. */
void muse_weather_start(void);

/* The last good answer, copied out. valid is false until the first one
 * lands. Safe from any task. */
void muse_weather_get(muse_weather_t *out);

/* Ask for a fetch now, rather than at the next interval. */
void muse_weather_refresh(void);

/*
 * Point it somewhere else. The stored forecast is dropped straight away, so
 * the page shows the new place looking something up rather than the old
 * place's numbers under a new name, and a fetch starts at once. Both strings
 * must outlive the call (the city table is static).
 */
void muse_weather_set_place(const char *lat, const char *lon);

/* Which city the settings page last chose, as an index into MUSE_CITIES.
 * Reads NVS on the first call. */
int muse_clock_city(void);
void muse_clock_set_city(int index);
const char *muse_clock_city_name(void);

/* "Partly cloudy" and the like, for the line under the temperature. */
const char *muse_weather_sky_name(muse_sky_t sky);

/* true where the configured unit is Fahrenheit, for the degree suffix. */
bool muse_weather_fahrenheit(void);

#ifdef __cplusplus
}
#endif
