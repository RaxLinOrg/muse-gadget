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

#include "lvgl.h"

/*
 * The settings tile (swipe left from Muse): Wi-Fi, Hatch, Bluetooth, Sound,
 * Sleep and Power pages. Runs entirely in the LVGL task; hardware state is
 * polled from the owning modules.
 */

void muse_settings_ui_build(lv_obj_t *tile);

/* Call periodically from the LVGL task; `visible` = settings tile is showing. */
void muse_settings_ui_tick(bool visible);

/* True when a sub-page is open (the tileview must not steal horizontal swipes). */
bool muse_settings_ui_in_subpage(void);

/*
 * For the settings page a registered swipe page brings with it (muse_page.h).
 * Its settings_build is handed a tile; these give it the same chrome and rows
 * as the firmware's own pages, back button included, so it does not have to
 * copy the layout or find its own way back to the list.
 *
 * Everything built here is freed when the user goes back, so hold no pointers
 * into it across calls without clearing them from an LV_EVENT_DELETE handler.
 */
lv_obj_t *muse_settings_ui_page(lv_obj_t *tile, const char *title, lv_obj_t **list_out);
lv_obj_t *muse_settings_ui_row(lv_obj_t *list, const char *icon, const char *text,
                               lv_obj_t **value_out, lv_event_cb_t cb, void *user);
void muse_settings_ui_note(lv_obj_t *list, const char *text);

