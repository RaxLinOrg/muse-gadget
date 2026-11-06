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
#include <stddef.h>

/*
 * Internet radio: an MP3 stream over HTTPS, decoded to the speaker's 16 kHz
 * mono. The station list is compiled in -- plan/vm-config.md has why it isn't
 * on the user's VM.
 *
 * A live stream never ends, which is the one real difference from the TTS
 * fetch this borrows from: there is no "complete" to wait for, so the only
 * ways out are stop(), a read error, or the user talking to Muse.
 */

typedef struct {
    const char *name;    /* the list row */
    const char *genre;   /* the caption under it */
    const char *url;     /* must answer 200 audio/mpeg; see plan/radio.md */
} muse_radio_station_t;

typedef enum {
    MUSE_RADIO_IDLE = 0,
    MUSE_RADIO_CONNECTING,   /* TLS and headers */
    MUSE_RADIO_BUFFERING,    /* filling, or refilling after a stall */
    MUSE_RADIO_PLAYING,
    MUSE_RADIO_FAILED,
} muse_radio_state_t;

int muse_radio_count(void);
const muse_radio_station_t *muse_radio_station(int i);

/* Tunes station `i`, dropping whatever was playing. Call from the UI task:
 * it creates the two worker tasks and returns at once. */
bool muse_radio_play(int i);
void muse_radio_stop(void);

muse_radio_state_t muse_radio_state(void);
int muse_radio_index(void);   /* the tuned station, -1 when idle */

/* The station's ICY now-playing title, "" when it sends none. */
void muse_radio_title(char *out, size_t cap);

/* One line for the player: why it failed, or what it is waiting for. */
const char *muse_radio_status_text(void);

/* ~1 Hz from the page. Drops the stream when Muse starts listening or
 * talking, since both would write to the same speaker. */
void muse_radio_tick(void);
