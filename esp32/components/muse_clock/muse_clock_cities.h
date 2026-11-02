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

/*
 * The cities the settings page offers. Add your own: the name is what the
 * page shows, and the coordinates are decimal degrees, north and east
 * positive. Nothing else has to change -- the timezone comes back with the
 * forecast, so a new row needs no further configuration.
 *
 * The last entry is always the one from Kconfig, so somewhere that isn't on
 * this list can still be reached without editing the file.
 */

#pragma once

#include "sdkconfig.h"

typedef struct {
    const char *name;
    const char *lat;    /* decimal degrees as text: it goes straight into the URL */
    const char *lon;
} muse_city_t;

static const muse_city_t MUSE_CITIES[] = {
    { "San Jose",      "37.3382",  "-121.8863" },
    { "San Francisco", "37.7749",  "-122.4194" },
    { "Los Angeles",   "34.0522",  "-118.2437" },
    { "Seattle",       "47.6062",  "-122.3321" },
    { "New York",      "40.7128",  "-74.0060"  },
    { "Taipei",        "25.0330",  "121.5654"  },
    { "Kaohsiung",     "22.6273",  "120.3014"  },
    { "Tokyo",         "35.6762",  "139.6503"  },
    { "Singapore",     "1.3521",   "103.8198"  },
    { "London",        "51.5074",  "-0.1278"   },
    /* Keep last: whatever menuconfig was set to. */
    { CONFIG_MUSE_CLOCK_PLACE, CONFIG_MUSE_CLOCK_LATITUDE, CONFIG_MUSE_CLOCK_LONGITUDE },
};

#define MUSE_CITY_COUNT ((int)(sizeof(MUSE_CITIES) / sizeof(MUSE_CITIES[0])))
