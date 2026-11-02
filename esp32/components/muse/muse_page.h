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
 * Extra swipe pages, so a feature can live in its own component.
 *
 * The UI owns two tiles of its own: the character at the left and settings at
 * the right. A registered page takes a tile between them, in registration
 * order, and the dots along the bottom edge grow to match. Register none and
 * the tileview is exactly what it was: face, then settings, one swipe apart.
 *
 * A page component never calls into the UI to place itself. It declares a
 * muse_page_t and hands it over with MUSE_PAGE, which registers it from a
 * constructor, before app_main and so before the screen is built. For that
 * constructor to survive the link, the component must register itself with
 * WHOLE_ARCHIVE, since nothing in the firmware references it by name:
 *
 *     static const muse_page_t clock_page = {
 *         .name = "clock", .build = build, .tick = tick,
 *     };
 *     MUSE_PAGE(clock_page);
 *
 * build() gets an empty tile, once, while the screen is built; the LVGL lock
 * is already held. tick() runs about once a second from the UI task and says
 * whether the page is the visible one, so a page can hold off on redrawing
 * what nobody is looking at. Both run in the LVGL task: keep network and
 * other blocking work on a task of the page's own.
 */

#pragma once

#include <stdbool.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Tiles between the face and settings. Each one costs a swipe to reach
 * settings, so this is deliberately small. */
#define MUSE_PAGES_MAX 4

typedef struct {
    const char *name;               /* for the log line that lists the pages */
    void (*build)(lv_obj_t *tile);  /* the page's widgets; required */
    void (*tick)(bool visible);     /* ~1 Hz from the UI task; may be NULL */
    /* Checked once, before the tile is made: false leaves the page out of the
     * tileview entirely rather than hiding an empty tile. May be NULL. */
    bool (*enabled)(void);

    /*
     * An optional page of the page's own in settings, listed under the rows
     * the firmware owns. settings_title turns it on; without it the other
     * three are ignored.
     *
     * settings_build is handed a fresh tile each time the row is tapped, and
     * what it builds is freed on the way back, so it must not keep pointers
     * between calls without clearing them. settings_value writes the greyed
     * text on the right of the row and is called about once a second while
     * settings is on screen; leave it NULL for no value.
     */
    const char *settings_title;
    const char *settings_icon;      /* an LV_SYMBOL_*; may be NULL */
    void (*settings_build)(lv_obj_t *tile);
    void (*settings_value)(char *out, size_t cap);
} muse_page_t;

/*
 * Takes a page for the next build of the screen. Called from a constructor,
 * so: no locks, no logging, no allocation. Pages registered after the screen
 * is built are ignored, and anything past MUSE_PAGES_MAX is dropped with a
 * warning once the log is up.
 */
void muse_page_register(const muse_page_t *page);

/*
 * The pages that got a tile, for the settings list to walk. Valid once the
 * screen is built; before that the count is zero.
 */
int muse_page_count(void);
const muse_page_t *muse_page_get(int i);

#define MUSE_PAGE(page)                                                 \
    __attribute__((constructor)) static void page##_muse_page_reg(void) \
    {                                                                   \
        muse_page_register(&(page));                                    \
    }

#ifdef __cplusplus
}
#endif
