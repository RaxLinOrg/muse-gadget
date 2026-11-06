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

#include "muse_podcast.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "minimp3.h"
#include "muse_audio.h"
#include "muse_radio.h"
#include "muse_state.h"
#include "nvs.h"

static const char *TAG = "podcast";

/*
 * Rax's seven, 2026-11-06. Every feed URL was resolved through the iTunes
 * lookup API and then fetched, and every enclosure probed with a ranged GET;
 * plan/radio.md has the table. "All-In" is the Chamath / Jason / Sacks /
 * Friedberg show -- two other podcasts carry the name.
 */
static const muse_podcast_show_t s_shows[] = {
    { "All-In", "https://rss.libsyn.com/shows/254861/destinations/1928300.xml" },
    { "跳脫 Do 式圈", "https://feeds.soundon.fm/podcasts/22505944-fec2-4417-b277-649ce5d3a491.xml" },
    { "博音", "https://feed.firstory.me/rss/user/ckyjmnkp0166d0830od1kznfj" },
    { "游庭皓的財經皓角",
      "https://feeds.soundcloud.com/users/soundcloud:users:735679489/sounds.rss" },
    { "Gooaye 股癌", "https://feeds.soundon.fm/podcasts/954689a5-3096-43a4-a80b-7810b219cef3.xml" },
    { "台灣通勤第一品牌", "https://anchor.fm/s/1ea77470/podcast/rss" },
    { "法客電台", "https://feed.firstory.me/rss/user/ckudnw7fn4tqg0870axzgirva" },
};
#define SHOWS ((int)(sizeof(s_shows) / sizeof(s_shows[0])))

#define UA "muse-gadget/1.0"
#define HTTP_TIMEOUT_MS 10000
#define READ_CHUNK 1460
#define RING (48 * 1024)
#define PREBUFFER (20 * 1024)
#define REFILL (12 * 1024)
#define WORK (12 * 1024)
#define MP3_HOLD (1441 + 4)
#define PCM_MAX 2304                 /* an MP3 frame is at most 1152 x 2 ch */
/* The feeds are 1.4-8.2 MB and the first four items land within 12-51 KB, so
 * this is a ceiling against a feed that buries its items, not a target. */
#define FEED_MAX_BYTES (192 * 1024)
#define NET_RETRIES 3
#define MAX_HOPS 4
#define SEND_WAIT_MS 200
#define RECV_WAIT_MS 300
#define WANT_EPISODES 4
#define NVS_NS "podcast"
/* Below this much played, or this close to the end, there is nothing worth
 * coming back to -- and resuming four seconds from the end is worse than not
 * resuming at all. */
#define RESUME_MIN_SECS 30
#define RESUME_TAIL_SECS 45

int muse_podcast_show_count(void)
{
    return SHOWS;
}

const muse_podcast_show_t *muse_podcast_show(int i)
{
    return (i >= 0 && i < SHOWS) ? &s_shows[i] : NULL;
}

/* ------------------------------------------------------------------ shared */

static volatile muse_feed_state_t s_feed_state;
static volatile int s_feed_show = -1;
static const char *volatile s_feed_status = "";
static muse_rss_t *s_rss;              /* the parse, in PSRAM */
static volatile int s_ep_count;
static TaskHandle_t s_feed_task;
static volatile bool s_feed_done;

/* One epoch per start, as the radio's: both workers leave as soon as it
 * moves, so stop(), seek() and switching episode all cost nothing. */
static volatile uint32_t s_epoch;
static volatile muse_ep_state_t s_state;
static const char *volatile s_status = "";
static volatile int s_play_show = -1, s_play_ep = -1;
static char s_play_title[MUSE_RSS_TITLE_MAX];
static char *s_url;                    /* the episode's enclosure, in PSRAM */
static volatile bool s_paused;
static volatile bool s_auto_paused;    /* ... because Muse took the speaker */
static volatile int s_dur;             /* <itunes:duration>, seconds */

/* Byte positions in the whole file. The fetch runs ahead of the decoder by
 * whatever is sitting in the ring, so the resume point and the progress bar
 * both come from s_play_off, which is where the speaker actually is. */
static volatile int64_t s_total;       /* 0 until the server says */
static volatile int64_t s_fetch_off;
static volatile int64_t s_play_off;
static volatile bool s_eof;            /* the fetch reached the end of the file */

static StreamBufferHandle_t s_ring;
static TaskHandle_t s_net, s_play;
static volatile bool s_net_done, s_play_done, s_net_live;

/* The resume point of the loaded show, read once when its feed arrives so the
 * episode list can mark a row without going back to flash every second. */
typedef struct {
    uint32_t hash;
    uint32_t off;
    uint32_t total;
} resume_t;
static resume_t s_resume;
static bool s_resume_valid;

muse_feed_state_t muse_podcast_feed_state(void)
{
    return s_feed_state;
}

int muse_podcast_feed_show(void)
{
    return s_feed_show;
}

const char *muse_podcast_feed_status(void)
{
    return s_feed_status;
}

int muse_podcast_episode_count(void)
{
    return s_feed_state == MUSE_FEED_READY ? s_ep_count : 0;
}

const muse_rss_item_t *muse_podcast_episode(int i)
{
    if (s_feed_state != MUSE_FEED_READY || !s_rss) {
        return NULL;
    }
    return muse_rss_item(s_rss, i);
}

muse_ep_state_t muse_podcast_state(void)
{
    return s_state;
}

int muse_podcast_playing_show(void)
{
    return s_play_show;
}

int muse_podcast_playing_episode(void)
{
    return s_play_ep;
}

const char *muse_podcast_status_text(void)
{
    return s_status;
}

const char *muse_podcast_playing_title(void)
{
    return s_play_title;
}

bool muse_podcast_is_paused(void)
{
    return s_paused;
}

/*
 * Bytes to seconds through the file's average bitrate. Exact for the constant
 * bitrate MP3 these feeds serve, approximate for anything variable -- which
 * is why the progress bar is drawn from the byte fraction and only the clock
 * readouts go through here.
 */
static int secs_at(int64_t bytes)
{
    if (s_total <= 0 || s_dur <= 0) {
        return 0;
    }
    if (bytes < 0) {
        bytes = 0;
    }
    return (int)((bytes * (int64_t)s_dur) / s_total);
}

int muse_podcast_position_secs(void)
{
    return secs_at(s_play_off);
}

int muse_podcast_duration_secs(void)
{
    return s_dur;
}

/* --------------------------------------------------------------- the resume */

/*
 * An episode is identified by its enclosure URL with the query string cut
 * off: SoundOn and Firstory regenerate a `?timestamp=` or `?v=` on every feed
 * build, so hashing the whole URL would throw the resume point away roughly
 * daily. The path alone carries the episode's own id on all seven feeds.
 */
static uint32_t url_hash(const char *url)
{
    uint32_t h = 2166136261u;
    for (const char *p = url; *p && *p != '?'; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    return h;
}

static void resume_key(int show, char *out, size_t cap)
{
    snprintf(out, cap, "s%d", show);
}

static bool resume_read(int show, resume_t *out)
{
    char key[8];
    nvs_handle_t h;
    bool got = false;
    resume_key(show, key, sizeof(key));
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(*out);
        got = nvs_get_blob(h, key, out, &len) == ESP_OK && len == sizeof(*out);
        nvs_close(h);
    }
    return got;
}

static void resume_write(int show, const resume_t *r)
{
    char key[8];
    nvs_handle_t h;
    resume_key(show, key, sizeof(key));
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    if (r) {
        nvs_set_blob(h, key, r, sizeof(*r));
    } else {
        nvs_erase_key(h, key);
    }
    nvs_commit(h);
    nvs_close(h);
}

/*
 * Called when playback stops for any reason, including the end of the file.
 * It reads s_play_off, which the decoder owns -- fine while that task is
 * winding down (it has stopped moving) and fine from a pause, because an
 * episode is at most 123 MB and so the high word of that 64-bit counter is
 * always zero: there is nothing for a non-atomic read to tear.
 */
static void resume_save(void)
{
    if (s_play_show < 0 || !s_url) {
        return;
    }
    int pos = secs_at(s_play_off);
    int left = s_dur > 0 ? s_dur - pos : 0;
    if (s_eof || (s_dur > 0 && left <= RESUME_TAIL_SECS) || pos < RESUME_MIN_SECS) {
        /* Finished, near enough to finished, or barely begun. */
        resume_write(s_play_show, NULL);
        if (s_resume_valid && s_feed_show == s_play_show) {
            s_resume_valid = false;
        }
        return;
    }
    resume_t r = { .hash = url_hash(s_url),
                   .off = (uint32_t)s_play_off,
                   .total = (uint32_t)s_total };
    resume_write(s_play_show, &r);
    if (s_feed_show == s_play_show) {
        s_resume = r;
        s_resume_valid = true;
    }
    ESP_LOGI(TAG, "resume saved: %s at %d s", s_shows[s_play_show].name, pos);
}

int muse_podcast_resume_mark(int i)
{
    const muse_rss_item_t *ep = muse_podcast_episode(i);
    if (!ep || !s_resume_valid || !s_resume.total) {
        return -1;
    }
    if (url_hash(ep->url) != s_resume.hash) {
        return -1;
    }
    return (int)(((uint64_t)s_resume.off * 1000) / s_resume.total);
}

/* ---------------------------------------------------------------- the feed */

static esp_err_t ignore_headers(esp_http_client_event_t *e)
{
    (void)e;
    return ESP_OK;
}

static void feed_task(void *arg)
{
    int show = (int)(intptr_t)arg;
    char *chunk = heap_caps_malloc(READ_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_http_client_config_t cfg = {
        .url = s_shows[show].feed,
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .event_handler = ignore_headers,
    };
    esp_http_client_handle_t c = NULL;
    int total = 0;

    if (!chunk || !s_rss) {
        s_feed_status = "OUT OF MEMORY";
        goto fail;
    }
    muse_rss_init(s_rss, WANT_EPISODES);
    c = esp_http_client_init(&cfg);
    if (!c) {
        s_feed_status = "OUT OF MEMORY";
        goto fail;
    }
    esp_http_client_set_header(c, "User-Agent", UA);
    /* The feed is plain XML over one GET, and nothing here needs the final
     * response's own headers, so the client may follow redirects itself --
     * which is the one place this differs from the radio's fetch. */
    if (esp_http_client_open(c, 0) != ESP_OK || esp_http_client_fetch_headers(c) < 0) {
        s_feed_status = "CAN'T REACH THE FEED";
        goto fail;
    }
    if (esp_http_client_get_status_code(c) != 200) {
        ESP_LOGW(TAG, "feed HTTP %d", esp_http_client_get_status_code(c));
        s_feed_status = "THE FEED SAID NO";
        goto fail;
    }
    for (;;) {
        if (show != s_feed_show) {
            goto out;            /* another show was asked for */
        }
        int n = esp_http_client_read(c, chunk, READ_CHUNK);
        if (n < 0) {
            s_feed_status = "THE FEED CUT OUT";
            goto fail;
        }
        if (!n) {
            break;               /* the whole feed, with fewer items than asked */
        }
        total += n;
        if (!muse_rss_feed(s_rss, chunk, (size_t)n)) {
            break;               /* got what we came for */
        }
        if (total > FEED_MAX_BYTES) {
            ESP_LOGW(TAG, "%d KB with no episodes in it", total / 1024);
            break;
        }
    }
    s_ep_count = muse_rss_count(s_rss);
    /* An item with no enclosure cannot be played, so it is not an episode. */
    while (s_ep_count > 0 && !muse_rss_item(s_rss, s_ep_count - 1)->url[0]) {
        s_ep_count--;
    }
    if (!s_ep_count) {
        s_feed_status = "NO EPISODES IN THE FEED";
        goto fail;
    }
    ESP_LOGI(TAG, "%s: %d episodes in %d KB", s_shows[show].name, s_ep_count, total / 1024);
    s_resume_valid = resume_read(show, &s_resume);
    s_feed_state = MUSE_FEED_READY;
    goto out;
fail:
    if (show == s_feed_show) {
        s_feed_state = MUSE_FEED_FAILED;
    }
out:
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    free(chunk);
    s_feed_done = true;
    vTaskSuspend(NULL);
}

bool muse_podcast_load(int i)
{
    if (i < 0 || i >= SHOWS) {
        return false;
    }
    if (s_feed_task && s_feed_done) {
        vTaskDeleteWithCaps(s_feed_task);
        s_feed_task = NULL;
        s_feed_done = false;
    }
    if (s_feed_task) {
        /* One fetch at a time: the parse has one home. Moving s_feed_show is
         * what tells the one in flight that nobody wants its answer. */
        ESP_LOGW(TAG, "a feed fetch is still running");
        return false;
    }
    if (!s_rss) {
        s_rss = heap_caps_malloc(sizeof(*s_rss), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_rss) {
        s_feed_status = "OUT OF MEMORY";
        s_feed_state = MUSE_FEED_FAILED;
        return false;
    }
    s_ep_count = 0;
    s_resume_valid = false;
    s_feed_status = "";
    s_feed_show = i;
    s_feed_state = MUSE_FEED_FETCHING;
    if (xTaskCreatePinnedToCoreWithCaps(feed_task, "pod_feed", 8 * 1024, (void *)(intptr_t)i, 5,
                                        &s_feed_task, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        s_feed_task = NULL;
        s_feed_status = "NO ROOM FOR THE FETCH";
        s_feed_state = MUSE_FEED_FAILED;
        return false;
    }
    return true;
}

/* ------------------------------------------------------------- the playback */

typedef struct {
    int64_t range_start;
    int64_t range_total;
    int64_t length;
    char *location;          /* MUSE_RSS_URL_MAX; a final one ran to 622 chars */
} ranged_t;

/* Content-Range tells us where the server actually started and how long the
 * file is. A 200 means it ignored the Range header, so the body starts at 0
 * however much we asked to skip -- which has to be noticed, or the position
 * would be wrong for the whole episode. */
static esp_err_t on_header(esp_http_client_event_t *e)
{
    ranged_t *r = e->user_data;
    if (e->event_id != HTTP_EVENT_ON_HEADER || !r || !e->header_key || !e->header_value) {
        return ESP_OK;
    }
    if (!strcasecmp(e->header_key, "Content-Range")) {
        const char *p = e->header_value;
        while (*p && (*p < '0' || *p > '9')) {
            p++;
        }
        r->range_start = strtoll(p, (char **)&p, 10);
        const char *slash = strchr(p, '/');
        r->range_total = slash ? strtoll(slash + 1, NULL, 10) : 0;
    } else if (!strcasecmp(e->header_key, "Content-Length")) {
        r->length = strtoll(e->header_value, NULL, 10);
    } else if (!strcasecmp(e->header_key, "Location") && r->location) {
        snprintf(r->location, MUSE_RSS_URL_MAX, "%s", e->header_value);
    }
    return ESP_OK;
}

static bool push(const uint8_t *data, size_t n, uint32_t epoch)
{
    while (n) {
        if (epoch != s_epoch) {
            return false;
        }
        size_t sent = xStreamBufferSend(s_ring, data, n, pdMS_TO_TICKS(SEND_WAIT_MS));
        data += sent;
        n -= sent;
    }
    return true;
}

static void net_task(void *arg)
{
    uint32_t epoch = (uint32_t)(uintptr_t)arg;
    uint8_t *chunk = heap_caps_malloc(READ_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *loc = heap_caps_malloc(MUSE_RSS_URL_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char range[48];
    int tries = 0;

    if (!chunk || !loc || !s_url) {
        s_status = "OUT OF MEMORY";
        goto out;
    }
    while (epoch == s_epoch) {
        ranged_t hdr = { .location = loc };
        loc[0] = '\0';
        esp_http_client_config_t cfg = {
            .url = s_url,
            .timeout_ms = HTTP_TIMEOUT_MS,
            .buffer_size = 2048,
            .buffer_size_tx = 1024,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = on_header,
            .user_data = &hdr,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (!c) {
            s_status = "OUT OF MEMORY";
            goto out;
        }
        esp_http_client_set_header(c, "User-Agent", UA);
        if (s_fetch_off > 0) {
            snprintf(range, sizeof(range), "bytes=%lld-", (long long)s_fetch_off);
            esp_http_client_set_header(c, "Range", range);
        }
        /*
         * Every enclosure on this list answers 302 -- a podtrac or SoundOn
         * tracking hop, sometimes two -- and esp_http_client only follows
         * redirects inside perform(), not when the body is read by hand. So
         * the hops are walked here, as the radio's fetch does. The Range
         * header is set on the handle and survives set_url(), which is what
         * makes a resume survive the redirect.
         */
        bool opened = false;
        int status = 0;
        for (int hop = 0;; hop++) {
            if (esp_http_client_open(c, 0) != ESP_OK ||
                esp_http_client_fetch_headers(c) < 0) {
                status = 0;
                break;
            }
            status = esp_http_client_get_status_code(c);
            if (status < 300 || status > 399) {
                opened = true;
                break;
            }
            if (hop >= MAX_HOPS || !loc[0]) {
                ESP_LOGW(TAG, "redirect %d with nowhere to go", status);
                status = 0;
                break;
            }
            esp_http_client_close(c);
            if (esp_http_client_set_url(c, loc) != ESP_OK) {
                status = 0;
                break;
            }
            loc[0] = '\0';
            hdr.range_start = hdr.range_total = hdr.length = 0;
        }
        if (opened && (status == 200 || status == 206)) {
            int64_t at = status == 206 ? hdr.range_start : 0;
            if (status == 200 && s_fetch_off > 0) {
                /* It served the whole file anyway: start over from the top
                 * rather than play from the wrong place. */
                ESP_LOGW(TAG, "no range support after all; from the start");
            }
            if (!s_total) {
                s_total = hdr.range_total ? hdr.range_total
                                          : (at == 0 && hdr.length > 0 ? hdr.length : 0);
            }
            s_fetch_off = at;
            ESP_LOGI(TAG, "episode: HTTP %d from %lld of %lld", status, (long long)at,
                     (long long)s_total);
            tries = 0;
            while (epoch == s_epoch) {
                int got = esp_http_client_read(c, (char *)chunk, READ_CHUNK);
                if (got < 0) {
                    ESP_LOGW(TAG, "read failed at %lld", (long long)s_fetch_off);
                    break;
                }
                if (!got) {
                    if (s_total && s_fetch_off >= s_total) {
                        s_eof = true;
                    }
                    break;
                }
                if (!push(chunk, (size_t)got, epoch)) {
                    esp_http_client_close(c);
                    esp_http_client_cleanup(c);
                    goto out;
                }
                s_fetch_off += got;
            }
        } else {
            ESP_LOGW(TAG, "episode HTTP %d", status);
            if (status == 404) {
                s_status = "THE EPISODE IS GONE";
                esp_http_client_close(c);
                esp_http_client_cleanup(c);
                goto out;
            }
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        if (s_eof || epoch != s_epoch) {
            goto out;
        }
        /* An episode is a file, so an early end is a broken connection rather
         * than the end of the show -- and a pause long enough for the server
         * to hang up lands here too. Pick up where the fetch stopped. */
        if (++tries > NET_RETRIES) {
            s_status = s_fetch_off > 0 ? "THE DOWNLOAD KEPT DROPPING" : "CAN'T REACH THE EPISODE";
            goto out;
        }
        ESP_LOGW(TAG, "reconnecting at %lld (try %d)", (long long)s_fetch_off, tries);
        for (int i = 0; i < 20 && epoch == s_epoch; i++) {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
out:
    free(chunk);
    free(loc);
    if (epoch == s_epoch) {
        s_net_live = false;
    }
    s_net_done = true;
    vTaskSuspend(NULL);
}

/* ------------------------------------------------------------- the decoder */

typedef struct {
    uint32_t step;
    uint32_t pos;
    int16_t prev;
} resampler_t;

/* The fourth copy of this in the firmware; plan/radio.md notes unifying them. */
static void resampler_init(resampler_t *r, int in_rate, int out_rate)
{
    r->step = (uint32_t)(((uint64_t)in_rate << 16) / out_rate);
    r->pos = 0;
    r->prev = 0;
}

static size_t resample(resampler_t *r, const int16_t *in, size_t n, int16_t *out)
{
    size_t o = 0;
    if (!n) {
        return 0;
    }
    while ((r->pos >> 16) < n) {
        size_t i = r->pos >> 16;
        int32_t a = i ? in[i - 1] : r->prev;
        int32_t b = in[i];
        out[o++] = (int16_t)(a + (int32_t)(((int64_t)(b - a) * (int64_t)(r->pos & 0xffff)) >> 16));
        r->pos += r->step;
    }
    r->pos -= n << 16;
    r->prev = in[n - 1];
    return o;
}

static bool wait_for(size_t want, uint32_t epoch)
{
    while (epoch == s_epoch) {
        if (xStreamBufferBytesAvailable(s_ring) >= want) {
            return true;
        }
        if (!s_net_live) {
            /* Everything the fetch is ever going to send is already here. */
            return xStreamBufferBytesAvailable(s_ring) > MP3_HOLD;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

static void play_task(void *arg)
{
    uint32_t epoch = (uint32_t)(uintptr_t)arg;
    mp3dec_t dec;
    uint8_t *buf = heap_caps_malloc(WORK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm = heap_caps_malloc(PCM_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *out = heap_caps_malloc((PCM_MAX + 8) * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t len = 0;
    int rate = 0;
    resampler_t rs = { 0 };

    mp3dec_init(&dec);
    if (!buf || !pcm || !out) {
        s_status = "OUT OF MEMORY";
        goto out;
    }
    s_state = MUSE_EP_BUFFERING;
    if (!wait_for(PREBUFFER, epoch)) {
        goto out;
    }
    /* The headers are in by now -- nothing reaches the ring before them -- so
     * this is where the file's real starting offset is known. */
    s_play_off = s_fetch_off - (int64_t)xStreamBufferBytesAvailable(s_ring);
    if (s_play_off < 0) {
        s_play_off = 0;
    }
    s_state = MUSE_EP_PLAYING;

    while (epoch == s_epoch) {
        if (s_paused) {
            /* Nothing is written to the speaker and nothing is drained, so the
             * fetch stalls on a full ring of its own accord. */
            s_state = MUSE_EP_PAUSED;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (s_state == MUSE_EP_PAUSED) {
            s_state = MUSE_EP_PLAYING;
        }
        size_t room = WORK - len;
        if (room) {
            size_t got = xStreamBufferReceive(s_ring, buf + len, room, pdMS_TO_TICKS(RECV_WAIT_MS));
            len += got;
        }
        if (len <= MP3_HOLD) {
            if (!s_net_live) {
                break;   /* the file is done, or the fetch gave up */
            }
            s_state = MUSE_EP_BUFFERING;
            if (!wait_for(REFILL, epoch)) {
                break;
            }
            s_state = MUSE_EP_PLAYING;
            continue;
        }
        size_t off = 0;
        while (len - off > MP3_HOLD && epoch == s_epoch && !s_paused) {
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(&dec, buf + off, len - off, pcm, &info);
            if (!info.frame_bytes) {
                break;
            }
            off += info.frame_bytes;
            if (samples <= 0) {
                continue;
            }
            if (info.channels == 2) {
                for (int k = 0; k < samples; k++) {
                    pcm[k] = (pcm[2 * k] + pcm[2 * k + 1]) / 2;
                }
            }
            if (rate != info.hz) {
                ESP_LOGI(TAG, "episode: MP3, %d Hz, %d ch", info.hz, info.channels);
                rate = info.hz;
                resampler_init(&rs, info.hz, MUSE_AUDIO_RATE);
            }
            size_t n = resample(&rs, pcm, (size_t)samples, out);
            for (size_t i = 0; i < n && epoch == s_epoch; i += MUSE_AUDIO_CHUNK) {
                size_t left = n - i;
                muse_audio_write(out + i, left < MUSE_AUDIO_CHUNK ? left : MUSE_AUDIO_CHUNK);
            }
        }
        if (off) {
            memmove(buf, buf + off, len - off);
            len -= off;
            s_play_off += (int64_t)off;
        } else if (len >= WORK) {
            ESP_LOGW(TAG, "no frame in %u bytes, resyncing", (unsigned)len);
            s_play_off += (int64_t)len;
            len = 0;
            mp3dec_init(&dec);
        }
    }
out:
    free(buf);
    free(pcm);
    free(out);
    if (epoch == s_epoch) {
        if (s_eof && xStreamBufferBytesAvailable(s_ring) == 0) {
            s_play_off = s_total;
            s_state = MUSE_EP_FINISHED;
        } else {
            s_state = s_status[0] ? MUSE_EP_FAILED : MUSE_EP_IDLE;
        }
        /* The page is on the LVGL task and must not write flash, so the one
         * place the resume point can be saved is here, as playback ends. */
        resume_save();
        if (s_state == MUSE_EP_FAILED) {
            ESP_LOGW(TAG, "stopped: %s", s_status);
        }
    }
    s_play_done = true;
    vTaskSuspend(NULL);
}

/* ------------------------------------------------------------------ the UI */

static void reap(void)
{
    if (s_net && s_net_done) {
        vTaskDeleteWithCaps(s_net);
        s_net = NULL;
        s_net_done = false;
    }
    if (s_play && s_play_done) {
        vTaskDeleteWithCaps(s_play);
        s_play = NULL;
        s_play_done = false;
    }
}

/* Takes the workers down without touching what is being played, so that
 * start_at() can put them back somewhere else in the same file. */
static void teardown(void)
{
    s_epoch++;
    for (int i = 0; i < 40 && (!s_net_done || !s_play_done); i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    reap();
    if (s_ring) {
        xStreamBufferReset(s_ring);
    }
    s_net_live = false;
}

void muse_podcast_stop(void)
{
    if (s_play_show < 0 && !s_net && !s_play) {
        return;
    }
    /*
     * The workers' own exit path saves nothing here: teardown() has already
     * moved the epoch, which is exactly what tells them to leave, so the save
     * has to happen on this side. It must come after teardown(), when the
     * decoder has stopped advancing the position, and before the episode is
     * cleared, because that is what names the thing being saved.
     */
    teardown();
    resume_save();
    s_play_show = -1;
    s_play_ep = -1;
    s_paused = false;
    s_auto_paused = false;
    s_state = MUSE_EP_IDLE;
    s_status = "";
    s_play_title[0] = '\0';
}

static bool start_at(int64_t off)
{
    if (!s_ring) {
        s_ring = xStreamBufferCreateWithCaps(RING, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_ring || !s_url) {
        s_status = "OUT OF MEMORY";
        s_state = MUSE_EP_FAILED;
        return false;
    }
    s_eof = false;
    s_fetch_off = off < 0 ? 0 : off;
    s_play_off = s_fetch_off;
    s_state = MUSE_EP_CONNECTING;
    s_net_live = true;
    void *epoch = (void *)(uintptr_t)s_epoch;
    if (xTaskCreatePinnedToCoreWithCaps(net_task, "pod_net", 8 * 1024, epoch, 5, &s_net, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_net = NULL;
        s_net_live = false;
        s_status = "NO ROOM FOR THE DOWNLOAD";
        s_state = MUSE_EP_FAILED;
        return false;
    }
    if (xTaskCreatePinnedToCoreWithCaps(play_task, "pod_play", 32 * 1024, epoch, 5, &s_play, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_play = NULL;
        s_status = "NO ROOM FOR THE DECODER";
        s_state = MUSE_EP_FAILED;
        teardown();
        return false;
    }
    return true;
}

bool muse_podcast_play(int i)
{
    const muse_rss_item_t *ep = muse_podcast_episode(i);
    if (!ep || !ep->url[0] || s_feed_show < 0) {
        return false;
    }
    /* One speaker, so the radio gives way. muse_radio_play() does the same to
     * this side, which is the whole of the arrangement between them. */
    muse_radio_stop();
    muse_podcast_stop();
    reap();
    if (s_net || s_play) {
        ESP_LOGW(TAG, "the last episode hasn't let go yet");
        return false;
    }
    if (!s_url) {
        s_url = heap_caps_malloc(MUSE_RSS_URL_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_url) {
        s_status = "OUT OF MEMORY";
        s_state = MUSE_EP_FAILED;
        return false;
    }
    snprintf(s_url, MUSE_RSS_URL_MAX, "%s", ep->url);
    snprintf(s_play_title, sizeof(s_play_title), "%s", ep->title);
    s_play_show = s_feed_show;
    s_play_ep = i;
    s_dur = ep->secs;
    s_total = 0;
    s_status = "";
    s_paused = false;
    s_auto_paused = false;

    /* Where you left off, if the stored offset belongs to this episode. */
    int64_t at = 0;
    resume_t r;
    if (resume_read(s_play_show, &r) && r.hash == url_hash(ep->url) && r.off) {
        at = r.off;
        s_total = r.total;
        ESP_LOGI(TAG, "resuming at %d s", secs_at(at));
    }
    ESP_LOGI(TAG, "playing %s: %s", s_shows[s_play_show].name, ep->title);
    return start_at(at);
}

void muse_podcast_pause(bool paused)
{
    if (s_play_show < 0) {
        return;
    }
    s_paused = paused;
    s_auto_paused = false;
    if (paused) {
        /* A deliberate pause is the one moment the user is most likely to put
         * the gadget down for good, so the place is written to flash now
         * rather than waiting for a stop that may never come. */
        resume_save();
    }
}

void muse_podcast_seek(int delta_secs)
{
    if (s_play_show < 0 || s_total <= 0 || s_dur <= 0) {
        return;
    }
    int64_t bps = s_total / s_dur;
    if (bps <= 0) {
        return;
    }
    int64_t at = s_play_off + (int64_t)delta_secs * bps;
    if (at < 0) {
        at = 0;
    }
    if (at > s_total - bps) {
        at = s_total - bps;   /* a second short of the end, not past it */
    }
    if (at < 0) {
        at = 0;
    }
    bool was_paused = s_paused;
    /* No save here: the position is about to change, and a flash write on
     * every tap of -30 would be a lot of wear for a number nobody reads. */
    teardown();
    s_paused = was_paused;
    ESP_LOGI(TAG, "seek %+d s -> %d s", delta_secs, secs_at(at));
    start_at(at);
}

void muse_podcast_tick(void)
{
    reap();
    if (s_feed_task && s_feed_done) {
        vTaskDeleteWithCaps(s_feed_task);
        s_feed_task = NULL;
        s_feed_done = false;
    }
    if (s_play_show < 0) {
        return;
    }
    /*
     * Muse and the podcast would write to the same speaker. Unlike the radio,
     * an episode can be picked up exactly where it was interrupted, so this
     * pauses instead of stopping -- and lets go again once Muse is done,
     * which is only right for a pause nobody asked for.
     */
    muse_mode_t mode = muse_state_mode(NULL);
    bool muse_busy = mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_THINKING ||
                     mode == MUSE_MODE_SPEAKING;
    if (muse_busy && !s_paused) {
        ESP_LOGI(TAG, "Muse is talking; pausing the episode");
        s_paused = true;
        s_auto_paused = true;
    } else if (!muse_busy && s_auto_paused) {
        s_auto_paused = false;
        s_paused = false;
    }
}
