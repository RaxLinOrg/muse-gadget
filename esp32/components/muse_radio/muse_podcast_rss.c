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

#include "muse_podcast_rss.h"

#include <stdlib.h>
#include <string.h>

/* Where the walk is. Everything a chunk boundary could fall inside -- a tag
 * name, an attribute list, an entity, a CDATA terminator -- is held in the
 * struct, so a feed read 1 byte at a time gives the same answer as one read
 * whole. The host test does exactly that comparison. */
enum {
    S_TEXT,     /* between tags */
    S_TAG,      /* just took '<' */
    S_NAME,     /* taking an opening tag's name */
    S_ATTRS,    /* taking the raw attribute text after that name */
    S_CLOSE,    /* taking a closing tag's name */
    S_BANG,     /* after "<!", before a comment, CDATA or doctype is known */
    S_COMMENT,  /* skipping to "-->" */
    S_CDATA,    /* copying to "]]>" */
    S_ENTITY,   /* taking "&...;" */
    S_PI,       /* skipping a <?...?> or <!DOCTYPE...> to its '>' */
};

void muse_rss_init(muse_rss_t *r, int want)
{
    memset(r, 0, sizeof(*r));
    r->want = want < 1 ? 1 : (want > MUSE_RSS_ITEMS_MAX ? MUSE_RSS_ITEMS_MAX : want);
    r->state = S_TEXT;
}

int muse_rss_count(const muse_rss_t *r)
{
    return r->count;
}

const muse_rss_item_t *muse_rss_item(const muse_rss_t *r, int i)
{
    return (i >= 0 && i < r->count) ? &r->items[i] : NULL;
}

/* ------------------------------------------------------------------ fields */

static void put(muse_rss_t *r, char c)
{
    if (r->dst && r->dst_len + 1 < r->dst_cap) {
        r->dst[r->dst_len++] = c;
        r->dst[r->dst_len] = '\0';
    }
}

static void capture(muse_rss_t *r, char *dst, size_t cap)
{
    r->dst = dst;
    r->dst_cap = cap;
    r->dst_len = 0;
    dst[0] = '\0';
}

/* Seconds, written either as a plain count or as [[HH:]MM:]SS. */
static int parse_duration(const char *s)
{
    int parts[3] = { 0, 0, 0 };
    int n = 0;
    while (*s && n < 3) {
        while (*s == ' ') {
            s++;
        }
        parts[n++] = (int)strtol(s, (char **)&s, 10);
        if (*s != ':') {
            break;
        }
        s++;
    }
    if (n == 1) {
        return parts[0];
    }
    if (n == 2) {
        return parts[0] * 60 + parts[1];
    }
    return parts[0] * 3600 + parts[1] * 60 + parts[2];
}

/* RFC 822-ish: "Wed, 04 Nov 2026 06:36:50 GMT". Only the day and month are
 * wanted -- the list shows 11/04 -- and the day-of-week prefix is optional. */
static void parse_pubdate(const char *s, int *month, int *day)
{
    static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *comma = strchr(s, ',');
    if (comma) {
        s = comma + 1;
    }
    while (*s == ' ') {
        s++;
    }
    int d = (int)strtol(s, (char **)&s, 10);
    while (*s == ' ') {
        s++;
    }
    if (d < 1 || d > 31 || strlen(s) < 3) {
        return;
    }
    for (int m = 0; m < 12; m++) {
        if (!strncmp(s, months + m * 3, 3)) {
            *month = m + 1;
            *day = d;
            return;
        }
    }
}

/* The url="..." of an <enclosure>, wherever in the attribute list it sits --
 * SoundCloud puts type= first, which is why this cannot just take the first
 * quoted string. */
static void take_enclosure_url(muse_rss_t *r)
{
    const char *p = r->attrs;
    while ((p = strstr(p, "url")) != NULL) {
        const char *q = p + 3;
        /* A real attribute, not the tail of "redirect-url" or similar. */
        bool starts = p == r->attrs || p[-1] == ' ' || p[-1] == '\t' ||
                      p[-1] == '\n' || p[-1] == '\r';
        while (*q == ' ') {
            q++;
        }
        if (starts && *q == '=') {
            q++;
            while (*q == ' ') {
                q++;
            }
            if (*q == '"' || *q == '\'') {
                char quote = *q++;
                const char *end = strchr(q, quote);
                size_t n = end ? (size_t)(end - q) : strlen(q);
                if (n >= sizeof(r->cur.url)) {
                    n = sizeof(r->cur.url) - 1;
                }
                memcpy(r->cur.url, q, n);
                r->cur.url[n] = '\0';
                return;
            }
        }
        p += 3;
    }
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    size_t lead = 0;
    while (s[lead] == ' ' || s[lead] == '\n' || s[lead] == '\r' || s[lead] == '\t') {
        lead++;
    }
    if (lead) {
        memmove(s, s + lead, n - lead + 1);
    }
}

/* ------------------------------------------------------------------ entities */

static void put_utf8(muse_rss_t *r, unsigned long cp)
{
    if (cp < 0x80) {
        put(r, (char)cp);
    } else if (cp < 0x800) {
        put(r, (char)(0xc0 | (cp >> 6)));
        put(r, (char)(0x80 | (cp & 0x3f)));
    } else if (cp < 0x10000) {
        put(r, (char)(0xe0 | (cp >> 12)));
        put(r, (char)(0x80 | ((cp >> 6) & 0x3f)));
        put(r, (char)(0x80 | (cp & 0x3f)));
    } else {
        put(r, (char)(0xf0 | (cp >> 18)));
        put(r, (char)(0x80 | ((cp >> 12) & 0x3f)));
        put(r, (char)(0x80 | ((cp >> 6) & 0x3f)));
        put(r, (char)(0x80 | (cp & 0x3f)));
    }
}

static void flush_entity(muse_rss_t *r, bool terminated)
{
    r->ent[r->ent_len] = '\0';
    if (!terminated) {
        /* A bare '&' in the text, which plenty of feeds have. Keep it. */
        put(r, '&');
        for (size_t i = 0; i < r->ent_len; i++) {
            put(r, r->ent[i]);
        }
    } else if (r->ent[0] == '#') {
        unsigned long cp = (r->ent[1] == 'x' || r->ent[1] == 'X')
                               ? strtoul(r->ent + 2, NULL, 16)
                               : strtoul(r->ent + 1, NULL, 10);
        if (cp) {
            put_utf8(r, cp);
        }
    } else if (!strcmp(r->ent, "amp")) {
        put(r, '&');
    } else if (!strcmp(r->ent, "lt")) {
        put(r, '<');
    } else if (!strcmp(r->ent, "gt")) {
        put(r, '>');
    } else if (!strcmp(r->ent, "quot")) {
        put(r, '"');
    } else if (!strcmp(r->ent, "apos")) {
        put(r, '\'');
    } else if (!strcmp(r->ent, "nbsp")) {
        put(r, ' ');
    }
    /* Anything else is dropped rather than shown as its source text. */
    r->ent_len = 0;
}

/* --------------------------------------------------------------- dispatch */

static bool is_name_char(char c)
{
    return !(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '>' || c == '/');
}

/* An opening tag, name complete. Attributes may still follow. */
static void open_tag(muse_rss_t *r)
{
    r->tag[r->tag_len] = '\0';
    r->dst = NULL;
    if (!strcmp(r->tag, "item")) {
        memset(&r->cur, 0, sizeof(r->cur));
        r->in_item = true;
        return;
    }
    if (!r->in_item) {
        return;
    }
    /* First one wins, which is what keeps a <description> full of escaped
     * markup from overwriting a title that was already read. */
    if ((!strcmp(r->tag, "title") || !strcmp(r->tag, "itunes:title")) && !r->cur.title[0]) {
        capture(r, r->cur.title, sizeof(r->cur.title));
    } else if (!strcmp(r->tag, "pubDate") || !strcmp(r->tag, "itunes:duration")) {
        capture(r, r->tmp, sizeof(r->tmp));
    }
}

/* The '>' that ends an opening tag, with whatever attributes it carried. */
static void end_open_tag(muse_rss_t *r)
{
    r->attrs[r->attrs_len < sizeof(r->attrs) ? r->attrs_len : sizeof(r->attrs) - 1] = '\0';
    if (r->in_item && !strcmp(r->tag, "enclosure") && !r->cur.url[0]) {
        take_enclosure_url(r);
    }
    r->attrs_len = 0;
}

static void close_tag(muse_rss_t *r)
{
    r->tag[r->tag_len] = '\0';
    if (!strcmp(r->tag, "item")) {
        if (r->in_item) {
            trim(r->cur.title);
            if (r->count < MUSE_RSS_ITEMS_MAX) {
                r->items[r->count++] = r->cur;
            }
            r->in_item = false;
            if (r->count >= r->want) {
                r->done = true;
            }
        }
    } else if (r->in_item && r->dst == r->tmp) {
        trim(r->tmp);
        if (!strcmp(r->tag, "pubDate")) {
            parse_pubdate(r->tmp, &r->cur.month, &r->cur.day);
        } else if (!strcmp(r->tag, "itunes:duration")) {
            r->cur.secs = parse_duration(r->tmp);
        }
    }
    r->dst = NULL;
}

/* -------------------------------------------------------------------- feed */

bool muse_rss_feed(muse_rss_t *r, const char *data, size_t n)
{
    for (size_t i = 0; i < n && !r->done; i++) {
        char c = data[i];
    redo:
        switch (r->state) {
        case S_TEXT:
            if (c == '<') {
                r->state = S_TAG;
            } else if (c == '&' && r->dst) {
                r->ent_len = 0;
                r->state = S_ENTITY;
            } else {
                put(r, c);
            }
            break;

        case S_TAG:
            r->tag_len = 0;
            r->attrs_len = 0;
            if (c == '/') {
                r->state = S_CLOSE;
            } else if (c == '!') {
                r->state = S_BANG;
            } else if (c == '?') {
                r->state = S_PI;
            } else if (is_name_char(c)) {
                r->tag[r->tag_len++] = c;
                r->state = S_NAME;
            } else {
                r->state = S_TEXT;   /* a stray '<' in the prose */
            }
            break;

        case S_NAME:
            if (is_name_char(c)) {
                if (r->tag_len < sizeof(r->tag) - 1) {
                    r->tag[r->tag_len++] = c;
                }
            } else if (c == '>') {
                open_tag(r);
                end_open_tag(r);
                r->state = S_TEXT;
            } else {
                open_tag(r);
                r->state = S_ATTRS;
                if (c == '/') {
                    r->attrs[r->attrs_len++] = c;
                }
            }
            break;

        case S_ATTRS:
            if (c == '>') {
                end_open_tag(r);
                r->state = S_TEXT;
            } else if (r->attrs_len < sizeof(r->attrs) - 1) {
                r->attrs[r->attrs_len++] = c;
            }
            break;

        case S_CLOSE:
            if (is_name_char(c)) {
                if (r->tag_len < sizeof(r->tag) - 1) {
                    r->tag[r->tag_len++] = c;
                }
            } else if (c == '>') {
                close_tag(r);
                r->state = S_TEXT;
            }
            break;

        case S_BANG:
            if (r->tag_len < sizeof(r->tag) - 1) {
                r->tag[r->tag_len++] = c;
            }
            r->tag[r->tag_len] = '\0';
            if (r->tag_len == 2 && !strcmp(r->tag, "--")) {
                r->match = 0;
                r->state = S_COMMENT;
            } else if (r->tag_len == 7 && !strcmp(r->tag, "[CDATA[")) {
                r->match = 0;
                r->state = S_CDATA;
            } else if (!strncmp(r->tag, "[CDATA[", r->tag_len) || !strncmp(r->tag, "--", r->tag_len)) {
                /* still could be either; keep taking characters */
            } else {
                r->state = S_PI;     /* <!DOCTYPE ...> and friends */
            }
            break;

        case S_COMMENT:
            if (c == '-') {
                if (r->match < 2) {
                    r->match++;
                }
            } else if (c == '>' && r->match == 2) {
                r->match = 0;
                r->state = S_TEXT;
            } else {
                r->match = 0;
            }
            break;

        case S_CDATA:
            /* Literal: no entities in here, so the only thing to watch for is
             * the terminator, and "]]]>" must still end it. */
            if (c == ']') {
                if (r->match < 2) {
                    r->match++;
                } else {
                    put(r, ']');
                }
            } else if (c == '>' && r->match == 2) {
                r->match = 0;
                r->state = S_TEXT;
            } else {
                for (int k = 0; k < r->match; k++) {
                    put(r, ']');
                }
                r->match = 0;
                put(r, c);
            }
            break;

        case S_ENTITY:
            if (c == ';') {
                flush_entity(r, true);
                r->state = S_TEXT;
            } else if (r->ent_len >= sizeof(r->ent) - 1 || c == '<' || c == '&' || c == ' ') {
                flush_entity(r, false);
                r->state = S_TEXT;
                /* That character was not part of the entity, so it has to be
                 * read again as text -- and not by stepping `i` back, which
                 * underflows when the entity broke at a chunk boundary. */
                goto redo;
            } else {
                r->ent[r->ent_len++] = c;
            }
            break;

        case S_PI:
            if (c == '>') {
                r->state = S_TEXT;
            }
            break;

        default:
            r->state = S_TEXT;
            break;
        }
    }
    return !r->done;
}
