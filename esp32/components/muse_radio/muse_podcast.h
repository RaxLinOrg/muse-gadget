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

#include "muse_podcast_rss.h"

/*
 * Podcasts: the SHOWS half of the radio tile.
 *
 * Where the radio is a live stream with no beginning or end, an episode is a
 * file -- which is the whole difference, and where all of the extra machinery
 * comes from. A file has a length, so there is a progress bar; a position, so
 * PAUSE means something; and a resume point worth keeping, so one byte offset
 * per show lives in NVS. All seven of the shows' CDNs answered a ranged GET
 * with 206 (plan/radio.md has the probe), so seeking and resuming are exact
 * byte offsets rather than a guess run through a bitrate.
 *
 * Every enclosure on this list is MP3, so this takes the minimp3 path the
 * radio already had and none of the AAC or MPEG-TS plumbing.
 *
 * Three calls into here block on the network and must not run on the LVGL
 * task: load(), play() and seek() all return at once, having handed the work
 * to a task of their own.
 */

typedef struct {
    const char *name;   /* the list row */
    const char *feed;   /* an RSS URL; the enclosures must be audio/mpeg */
} muse_podcast_show_t;

typedef enum {
    MUSE_FEED_IDLE = 0,
    MUSE_FEED_FETCHING,
    MUSE_FEED_READY,
    MUSE_FEED_FAILED,
} muse_feed_state_t;

typedef enum {
    MUSE_EP_IDLE = 0,
    MUSE_EP_CONNECTING,
    MUSE_EP_BUFFERING,
    MUSE_EP_PLAYING,
    MUSE_EP_PAUSED,
    MUSE_EP_FINISHED,   /* played to the end; the resume point is cleared */
    MUSE_EP_FAILED,
} muse_ep_state_t;

int muse_podcast_show_count(void);
const muse_podcast_show_t *muse_podcast_show(int i);

/* ------------------------------------------------------------- the episodes */

/* Fetches show `i`'s feed far enough to read its newest few episodes, on a
 * task of its own. Poll feed_state(); the list is valid once it is READY. */
bool muse_podcast_load(int i);
muse_feed_state_t muse_podcast_feed_state(void);
int muse_podcast_feed_show(void);           /* which show was asked for, -1 if none */
const char *muse_podcast_feed_status(void); /* one line for the screen when it fails */
int muse_podcast_episode_count(void);
const muse_rss_item_t *muse_podcast_episode(int i);

/* Where this show was left off, as a fraction 0..1000 of the episode, or -1
 * when there is no resume point for episode `i` of the loaded show. Cheap, so
 * the list can mark the episode you were in the middle of. */
int muse_podcast_resume_mark(int i);

/* ------------------------------------------------------------ the playback */

/* Plays episode `i` of the loaded show, from its saved offset if that offset
 * belongs to this episode. */
bool muse_podcast_play(int i);
void muse_podcast_stop(void);               /* saves the resume point */
void muse_podcast_pause(bool paused);
bool muse_podcast_is_paused(void);

/* Jumps by whole seconds, converted to bytes through the file's average
 * bitrate -- so on a variable-bitrate episode a 30 s step is approximately
 * 30 s. Reopens the socket, which costs a re-buffer. */
void muse_podcast_seek(int delta_secs);

muse_ep_state_t muse_podcast_state(void);
int muse_podcast_playing_show(void);        /* -1 when idle */
int muse_podcast_playing_episode(void);
const char *muse_podcast_status_text(void);

/* The playing episode's title, kept here rather than read back out of the
 * episode list -- loading another show's feed replaces that list while this
 * one is still playing. */
const char *muse_podcast_playing_title(void);

int muse_podcast_position_secs(void);
int muse_podcast_duration_secs(void);

/* ~1 Hz from the page, as the radio's. Gives the speaker back to Muse. */
void muse_podcast_tick(void);
