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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "muse_radar_places.h"

/*
 * What the radar page shows, and where it gets it.
 *
 * Settings live in this component's own NVS namespace rather than in
 * muse_settings, so nothing upstream owns has to change. The page's tile and
 * its worker task both read them.
 */

typedef enum {
    MUSE_RADAR_PLANES = 0,
    MUSE_RADAR_SHIPS,
    MUSE_RADAR_WEATHER,
    MUSE_RADAR_MODE_COUNT,
} muse_radar_mode_t;

/*
 * Ranges, widest last, with "off" one past the end. Off is not a mode the
 * page can be in: muse_radar_enabled() answers false and the page never
 * joins the tileview (muse_page_t.enabled).
 */
#define MUSE_RADAR_RANGE_COUNT 3
#define MUSE_RADAR_OFF         MUSE_RADAR_RANGE_COUNT

extern const int MUSE_RADAR_RANGES[MUSE_RADAR_RANGE_COUNT];   /* 12, 40, 250 */

int  muse_radar_range_index(void);      /* 0..2, or MUSE_RADAR_OFF */
void muse_radar_set_range_index(int i);
int  muse_radar_range_nm(void);         /* 0 when off */
bool muse_radar_enabled(void);

muse_radar_mode_t muse_radar_mode(void);
void muse_radar_set_mode(muse_radar_mode_t m);
const char *muse_radar_mode_name(muse_radar_mode_t m);

int  muse_radar_place(void);            /* index into MUSE_RADAR_PLACES */
void muse_radar_set_place(int i);
const muse_radar_place_t *muse_radar_place_at(void);

/*
 * How often the worker refreshes, in seconds, for the range now chosen.
 * Planes follow the range -- a blip has to move a sensible number of pixels
 * between frames, not jump the width of the scope -- while weather is pinned
 * to the ten minutes RainViewer publishes at. See plan/radar.md.
 */
int muse_radar_period_s(void);

/* One contact. Polar, because that is how the plane feed arrives. */
typedef struct {
    float dst;          /* nautical miles from the centre */
    float dir;          /* bearing to it, degrees true */
    float track;        /* where it is heading, degrees true */
    float speed;        /* knots over the ground */
    int32_t alt;        /* feet; 0 when on the ground or unknown */
    char id[10];        /* callsign, registration or ship name */
    char type[6];       /* ICAO type, when the feed gives one */
} muse_radar_target_t;

/*
 * The cap exists because New York inside 250 nm is 1,199 aircraft in a 615 KB
 * body. The nearest ones are the interesting ones, so the parser keeps those
 * and counts the rest.
 */
#define MUSE_RADAR_MAX_TARGETS 80

typedef struct {
    muse_radar_target_t t[MUSE_RADAR_MAX_TARGETS];
    int n;              /* how many are in t[] */
    int total;          /* how many the source actually had */
    bool valid;
    bool stale;         /* last fetch failed; what is here is the old sweep */
    uint32_t at;        /* seconds since boot when it landed */
} muse_radar_sweep_t;

void muse_radar_net_start(void);
void muse_radar_get(muse_radar_sweep_t *out);

/*
 * The page tells the worker whether anyone is looking. Nothing is fetched
 * while the answer is false, which is what keeps a 500-a-day quota from
 * mattering, and coming back into view fetches at once rather than waiting
 * out the period.
 */
void muse_radar_set_visible(bool visible);
void muse_radar_refresh(void);

/*
 * Weather is a picture rather than a list of contacts: one RainViewer tile,
 * reprojected into the scope. The buffer is RGB565 at MUSE_RADAR_TILE_PX,
 * which is the whole round display: the scope is the page.
 * square, or NULL until one has landed. Caller must not free it.
 */
#define MUSE_RADAR_TILE_PX 466
const uint16_t *muse_radar_rain(void);
bool muse_radar_rain_fresh(void);
