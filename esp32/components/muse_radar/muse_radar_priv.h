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

/*
 * Weather: one picture of the sky, in two passes into the same buffer. Cloud
 * (NASA GIBS clean infrared) is laid down first and rain (the newest
 * RainViewer frame) painted over it, so a rainy cell always wins the pixel it
 * shares with the cloud it falls out of.
 */
bool muse_radar_rain_fetch(void);

/* How much of the scope each pass covered, in tenths of a percent, and the
 * zoom the rain was cut at. A picture landing is not the same as it having
 * anything in it. */
int  muse_radar_rain_permille(void);
int  muse_radar_cloud_permille(void);
bool muse_radar_cloud_seen(void);
int  muse_radar_rain_zoom(void);

/* Drop the picture: it belongs to the place and zoom it was cut for. */
void muse_radar_rain_invalidate(void);

/*
 * Ships: AIS arrives as a stream rather than an answer to a question, so the
 * poll keeps the socket up and the snapshot reads off whatever has arrived.
 */
void muse_radar_ais_poll(void);
bool muse_radar_ais_snapshot(muse_radar_sweep_t *out);
void muse_radar_ais_stop(void);
bool muse_radar_ais_have_key(void);

/* False once the stream has been tried and would not open, so the page can
 * say that rather than looking for contacts that will never arrive. */
bool muse_radar_ais_linked(void);

/* Adds one contact to the nearest-first list, spreading what it keeps across
 * the range rings once the list is full. Shared by both sources. */
void muse_radar_keep(muse_radar_sweep_t *s, const muse_radar_target_t *t);
