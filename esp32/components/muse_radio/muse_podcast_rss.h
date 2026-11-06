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
 * A streaming reader for the first few <item> elements of a podcast feed.
 *
 * The feeds on this gadget's list are 1.4 to 8.2 MB of XML and the board has
 * about 2.7 MB of PSRAM free, so a feed can never be held whole -- and there
 * is no need to, because RSS items are newest-first. Bytes go in as they come
 * off the socket, and once the wanted number of items is in, feed() says so
 * and the caller closes the connection. Measured against the seven feeds, the
 * first four items arrive within 12 to 51 KB.
 *
 * It is not an XML parser. It walks tags, and inside an <item> it keeps the
 * FIRST title, pubDate, duration and enclosure URL it sees, ignoring every
 * other element's text. That "first wins" rule is what makes it safe to read
 * straight past a <description> full of escaped HTML without tracking where
 * that element ends: in all seven feeds the title precedes the description, so
 * a stray tag inside the prose arrives too late to be mistaken for the real
 * thing. A feed that put its description first would show that description's
 * first tag as the title -- a wrong line on the screen, not a crash.
 *
 * No ESP-IDF here on purpose: it compiles on the host, which is how it was
 * tested against all seven real feeds before it went near the board.
 */

#define MUSE_RSS_TITLE_MAX 192
#define MUSE_RSS_URL_MAX   768   /* a final enclosure URL ran to 622 chars */
#define MUSE_RSS_ITEMS_MAX 4

typedef struct {
    char title[MUSE_RSS_TITLE_MAX];
    char url[MUSE_RSS_URL_MAX];
    int secs;        /* <itunes:duration>, 0 when the feed gives none */
    int month, day;  /* from <pubDate>, 0 when it cannot be read */
} muse_rss_item_t;

/* The caller owns this; it is about 4 KB, so it belongs in PSRAM. */
typedef struct {
    muse_rss_item_t items[MUSE_RSS_ITEMS_MAX];
    muse_rss_item_t cur;
    int count;
    int want;
    bool in_item;
    bool done;

    int state;
    char tag[48];
    size_t tag_len;
    char attrs[MUSE_RSS_URL_MAX + 128];
    size_t attrs_len;
    char ent[12];
    size_t ent_len;
    char tmp[64];      /* a pubDate or a duration, while it is being read */
    int match;         /* how much of a terminator (-->, ]]>) has matched */

    char *dst;         /* the field being filled, NULL while discarding */
    size_t dst_cap;
    size_t dst_len;
} muse_rss_t;

/* `want` is clamped to 1..MUSE_RSS_ITEMS_MAX. */
void muse_rss_init(muse_rss_t *r, int want);

/* Takes the next n bytes. Returns false once it has all the items it wants
 * and the rest of the feed is of no use. */
bool muse_rss_feed(muse_rss_t *r, const char *data, size_t n);

int muse_rss_count(const muse_rss_t *r);
const muse_rss_item_t *muse_rss_item(const muse_rss_t *r, int i);
