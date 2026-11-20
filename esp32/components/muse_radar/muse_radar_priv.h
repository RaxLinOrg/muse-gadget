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

#include "muse_radar.h"

/* Between the component's own files; not for anyone else. */

/* Weather: fetch the newest RainViewer frame into the rain layer. */
bool muse_radar_rain_fetch(void);

/*
 * Ships: AIS arrives as a stream rather than an answer to a question, so the
 * poll keeps the socket up and the snapshot reads off whatever has arrived.
 */
void muse_radar_ais_poll(void);
bool muse_radar_ais_snapshot(muse_radar_sweep_t *out);
void muse_radar_ais_stop(void);
bool muse_radar_ais_have_key(void);

/* Drops the nearest-first target list in place. Shared by both sources. */
void muse_radar_keep(muse_radar_sweep_t *s, const muse_radar_target_t *t);
