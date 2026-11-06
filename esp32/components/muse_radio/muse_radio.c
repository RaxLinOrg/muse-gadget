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

#include "muse_radio.h"

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
#include "esp_aac_dec.h"
#include "esp_audio_dec_default.h"
#include "minimp3.h"
#include "muse_audio.h"
#include "muse_state.h"

static const char *TAG = "radio";

/*
 * Rax's list, 2026-11-05. Every URL was probed by hand for its status and
 * Content-Type before it went in; plan/radio.md has the table.
 *
 * Four of these are AAC on Triton/revma, which is why the board has an AAC
 * decoder at all, and why the fetch follows redirects: Triton answers 302
 * with a short-lived token URL. 好事聯播網 is HLS, so it arrives as MPEG-TS
 * segments listed by a playlist, with video riding alongside the audio.
 */
static const muse_radio_station_t s_stations[] = {
    { "ICRT",           "英語・多元音樂",   "http://n03.rcs.revma.com/x27mfpztxp3vv" },
    { "中廣流行網",      "流行音樂・談話",   "https://n03.rcs.revma.com/aw9uqyxy2tzuv" },
    { "中廣音樂網",      "音樂・古典",      "https://n03.rcs.revma.com/ndk05tyy2tzuv" },
    { "飛碟聯播網",      "流行・談話節目",   "https://n10.rcs.revma.com/em90w4aeewzuv" },
    { "寶島新聲 FM98.5", "台語・華語流行",   "http://125.227.87.206:8000/FM98.5" },
    { "好事聯播網 港都983", "高雄・在地音樂",  "http://202.39.43.67:1935/live/RA000012/chunklist.m3u8" },
};
#define STATIONS ((int)(sizeof(s_stations) / sizeof(s_stations[0])))

#define UA "muse-gadget/1.0"
#define HTTP_TIMEOUT_MS 8000
#define READ_CHUNK 1460          /* one TCP segment */
#define RING (48 * 1024)         /* ~3 s at 128 kbps */
#define PREBUFFER (20 * 1024)    /* before the first sample goes out */
#define REFILL (12 * 1024)       /* after a stall */
#define WORK (12 * 1024)         /* the decoder's own window on the ring */
#define MP3_HOLD (1441 + 4)      /* the largest frame, plus the next header */
#define AAC_HOLD 1600            /* comfortably over an ADTS frame at these rates */
#define PCM_MAX 4096             /* samples: an HE-AAC frame is 2048 x 2 ch */
#define MAX_HOPS 4
#define PLAYLIST_MAX 3072
#define URL_MAX 320
#define TS_PACKET 188
#define SEG_POLL_MS 3000
#define META_MAX 512             /* of an ICY block; longer titles are cut */
#define TITLE_MAX 96
#define SEND_WAIT_MS 200
#define RECV_WAIT_MS 300

int muse_radio_count(void)
{
    return STATIONS;
}

const muse_radio_station_t *muse_radio_station(int i)
{
    return (i >= 0 && i < STATIONS) ? &s_stations[i] : NULL;
}

/* ------------------------------------------------------------------ shared */

/*
 * One epoch per tune. Both workers compare it on every pass and leave as soon
 * as it moves, which is how stop() and switching station both work: nothing
 * waits for a socket to notice.
 */
static volatile uint32_t s_epoch;
static volatile int s_index = -1;
static volatile muse_radio_state_t s_state;
static const char *volatile s_status = "";

static StreamBufferHandle_t s_ring;
static SemaphoreHandle_t s_lock;
static char s_title[TITLE_MAX];

static TaskHandle_t s_net, s_play;
static volatile bool s_net_done, s_play_done;
static volatile bool s_net_live;   /* the fetch is still on the socket */
static volatile bool s_is_aac;     /* set by the fetch once the headers are in */

muse_radio_state_t muse_radio_state(void)
{
    return s_state;
}

int muse_radio_index(void)
{
    return s_index;
}

const char *muse_radio_status_text(void)
{
    return s_status;
}

void muse_radio_title(char *out, size_t cap)
{
    if (!out || !cap) {
        return;
    }
    out[0] = '\0';
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        snprintf(out, cap, "%s", s_title);
        xSemaphoreGive(s_lock);
    }
}

static void set_title(const char *t)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        snprintf(s_title, sizeof(s_title), "%s", t ? t : "");
        xSemaphoreGive(s_lock);
    }
}

/* --------------------------------------------------------------- the fetch */

/* StreamTitle='...'; inside an ICY metadata block. */
static void parse_meta(const char *meta)
{
    const char *p = strstr(meta, "StreamTitle=");
    if (!p) {
        return;
    }
    p += strlen("StreamTitle=");
    if (*p == '\'' || *p == '"') {
        char quote = *p++;
        const char *end = strchr(p, quote);
        size_t n = end ? (size_t)(end - p) : strlen(p);
        char buf[TITLE_MAX];
        if (n >= sizeof(buf)) {
            n = sizeof(buf) - 1;
        }
        memcpy(buf, p, n);
        buf[n] = '\0';
        ESP_LOGI(TAG, "now playing: %s", buf);
        set_title(buf);
    }
}

/* Hands audio bytes to the decoder, waiting while it is behind. */
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

/* Splits one read into audio and ICY metadata. State rides in the pointers so
 * a block that straddles two reads still comes out whole. */
typedef struct {
    size_t metaint;      /* 0 when the station sends no metadata */
    size_t audio_left;
    int need;            /* -1: next byte is the block's length */
    size_t have;
    char buf[META_MAX];
} icy_t;

static bool demux(icy_t *icy, const uint8_t *p, size_t n, uint32_t epoch)
{
    if (!icy->metaint) {
        return push(p, n, epoch);
    }
    while (n) {
        if (icy->audio_left) {
            size_t take = icy->audio_left < n ? icy->audio_left : n;
            if (!push(p, take, epoch)) {
                return false;
            }
            p += take;
            n -= take;
            icy->audio_left -= take;
            continue;
        }
        if (icy->need < 0) {
            icy->need = (int)(*p++) * 16;
            n--;
            icy->have = 0;
            if (!icy->need) {
                icy->audio_left = icy->metaint;   /* the usual case: no change */
                icy->need = -1;
            }
            continue;
        }
        size_t take = (size_t)icy->need - icy->have;
        if (take > n) {
            take = n;
        }
        /* A title longer than the buffer is cut, but all of it is still
         * skipped: `have` counts the bytes seen, not the bytes kept. */
        if (icy->have < sizeof(icy->buf) - 1) {
            size_t room = sizeof(icy->buf) - 1 - icy->have;
            memcpy(icy->buf + icy->have, p, take < room ? take : room);
        }
        icy->have += take;
        p += take;
        n -= take;
        if (icy->have >= (size_t)icy->need) {
            size_t kept = icy->have < sizeof(icy->buf) - 1 ? icy->have : sizeof(icy->buf) - 1;
            icy->buf[kept] = '\0';
            parse_meta(icy->buf);
            icy->audio_left = icy->metaint;
            icy->need = -1;
        }
    }
    return true;
}

/*
 * esp_http_client only keeps the headers it needs itself, so asking it for
 * `icy-metaint` after the fact comes back empty -- which is how the first
 * build on the board reported `metaint 0` for stations that plainly send it.
 * They have to be caught as they arrive.
 */
#define LOCATION_MAX 256

typedef struct {
    size_t metaint;
    bool typed;
    bool mp3;
    bool aac;
    bool hls;
    char location[LOCATION_MAX];
} hdrs_t;

/* strcasestr is a GNU extension, so: haystack contains needle, ignoring case. */
static bool has_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        if (!strncasecmp(hay, needle, n)) {
            return true;
        }
    }
    return false;
}

static esp_err_t on_header(esp_http_client_event_t *e)
{
    hdrs_t *h = e->user_data;
    if (e->event_id != HTTP_EVENT_ON_HEADER || !h || !e->header_key || !e->header_value) {
        return ESP_OK;
    }
    if (!strcasecmp(e->header_key, "icy-metaint")) {
        long n = strtol(e->header_value, NULL, 10);
        h->metaint = n > 0 ? (size_t)n : 0;
    } else if (!strcasecmp(e->header_key, "Content-Type")) {
        h->typed = true;
        h->mp3 = strstr(e->header_value, "mpeg") || strstr(e->header_value, "mp3");
        h->aac = strstr(e->header_value, "aac") != NULL;
        /* application/vnd.apple.mpegurl, audio/mpegurl, application/x-mpegURL */
        h->hls = has_ci(e->header_value, "mpegurl");
    } else if (!strcasecmp(e->header_key, "Location")) {
        snprintf(h->location, sizeof(h->location), "%s", e->header_value);
    }
    return ESP_OK;
}

/* -------------------------------------------------------------------- HLS */

/*
 * 好事聯播網 publishes HLS rather than a plain stream, so there are two more
 * layers between the socket and the decoder: a playlist that lists segments,
 * and MPEG-TS segments that carry the audio alongside video this board has no
 * use for. Both are small, and what comes out the far end is the same ADTS
 * AAC the Triton stations send directly -- so only the plumbing is new.
 *
 * What this does not do: encryption (#EXT-X-KEY), byte ranges, and PAT/PMT
 * tables split across packets. None appear in the stream this was built for,
 * and each would be a silent wrong answer rather than a crash, so the demuxer
 * says so in the log instead of guessing.
 */
typedef struct {
    uint8_t pkt[TS_PACKET];
    size_t have;
    int pmt_pid;
    int audio_pid;
    bool announced;
} ts_t;

/* The PMT names the elementary streams; we want the audio one and its codec. */
static void ts_read_pmt(ts_t *ts, const uint8_t *sec, size_t len)
{
    if (len < 12) {
        return;
    }
    size_t slen = (size_t)((sec[1] & 0x0f) << 8 | sec[2]);
    size_t info_len = (size_t)((sec[10] & 0x0f) << 8 | sec[11]);
    size_t q = 12 + info_len;
    size_t end = 3 + slen - 4;   /* less the CRC */
    if (end > len) {
        end = len;
    }
    while (q + 5 <= end) {
        int type = sec[q];
        int pid = (sec[q + 1] & 0x1f) << 8 | sec[q + 2];
        size_t es_len = (size_t)((sec[q + 3] & 0x0f) << 8 | sec[q + 4]);
        if (type == 0x0f || type == 0x11) {        /* AAC, ADTS or LATM */
            ts->audio_pid = pid;
            s_is_aac = true;
        } else if (type == 0x03 || type == 0x04) { /* MPEG-1/2 audio */
            ts->audio_pid = pid;
            s_is_aac = false;
        }
        q += 5 + es_len;
    }
    if (ts->audio_pid >= 0 && !ts->announced) {
        ts->announced = true;
        ESP_LOGI(TAG, "HLS: %s on pid %d", s_is_aac ? "AAC" : "MP3", ts->audio_pid);
    }
}

/* One 188-byte packet: tables are read, audio is forwarded, video is dropped. */
static bool ts_packet(ts_t *ts, const uint8_t *p, uint32_t epoch)
{
    if (p[0] != 0x47 || (p[1] & 0x80)) {
        return true;   /* not a packet, or flagged as corrupt: skip it */
    }
    int pid = (p[1] & 0x1f) << 8 | p[2];
    bool pusi = (p[1] & 0x40) != 0;
    int afc = (p[3] >> 4) & 3;
    size_t off = 4;
    if (afc & 2) {                       /* adaptation field */
        off += 1 + p[off];
    }
    if (!(afc & 1) || off >= TS_PACKET) {
        return true;                     /* no payload */
    }
    if (pid == 0 || pid == ts->pmt_pid) {
        size_t q = off;
        if (pusi) {
            q += 1 + p[q];               /* pointer_field */
        }
        if (q + 12 > TS_PACKET) {
            return true;
        }
        if (pid == 0) {
            /* First program in the PAT is the one we take. */
            ts->pmt_pid = (p[q + 10] & 0x1f) << 8 | p[q + 11];
        } else {
            ts_read_pmt(ts, p + q, TS_PACKET - q);
        }
        return true;
    }
    if (pid != ts->audio_pid) {
        return true;                     /* video, metadata, padding */
    }
    if (pusi) {
        /* Step over the PES header to the elementary bytes. */
        if (off + 9 > TS_PACKET || p[off] || p[off + 1] || p[off + 2] != 1) {
            return true;
        }
        off += 9 + p[off + 8];
        if (off >= TS_PACKET) {
            return true;
        }
    }
    return push(p + off, TS_PACKET - off, epoch);
}

/* HTTP reads land wherever they land, so packets are reassembled here. */
static bool ts_feed(ts_t *ts, const uint8_t *data, size_t n, uint32_t epoch)
{
    while (n) {
        if (!ts->have && *data != 0x47) {
            data++;                      /* resync */
            n--;
            continue;
        }
        size_t want = TS_PACKET - ts->have;
        size_t take = want < n ? want : n;
        memcpy(ts->pkt + ts->have, data, take);
        ts->have += take;
        data += take;
        n -= take;
        if (ts->have == TS_PACKET) {
            ts->have = 0;
            if (!ts_packet(ts, ts->pkt, epoch)) {
                return false;
            }
        }
    }
    return true;
}

/* GET a whole small body (a playlist). Returns its length, or -1. */
static int fetch_text(esp_http_client_handle_t c, const char *url, char *out, size_t cap)
{
    int got = 0;
    if (esp_http_client_set_url(c, url) != ESP_OK || esp_http_client_open(c, 0) != ESP_OK) {
        return -1;
    }
    if (esp_http_client_fetch_headers(c) < 0 || esp_http_client_get_status_code(c) != 200) {
        esp_http_client_close(c);
        return -1;
    }
    while ((size_t)got < cap - 1) {
        int n = esp_http_client_read(c, out + got, (int)(cap - 1 - (size_t)got));
        if (n <= 0) {
            break;
        }
        got += n;
    }
    out[got] = '\0';
    esp_http_client_close(c);
    return got;
}

/* Resolves `name` against the playlist URL it came from. */
static void resolve(const char *base, const char *name, char *out, size_t cap)
{
    if (strstr(name, "://")) {
        snprintf(out, cap, "%s", name);
        return;
    }
    const char *slash = strrchr(base, '/');
    size_t n = slash ? (size_t)(slash - base) : strlen(base);
    snprintf(out, cap, "%.*s/%s", (int)n, base, name);
}

/* The nth line of the playlist that is not a comment, and its sequence. */
static const char *playlist_segment(const char *text, int64_t want, int64_t first,
                                    char *name, size_t cap)
{
    int64_t seq = first;
    for (const char *p = text; *p; ) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        while (len && (p[len - 1] == '\r' || p[len - 1] == ' ')) {
            len--;
        }
        if (len && *p != '#') {
            if (seq == want) {
                snprintf(name, cap, "%.*s", (int)len, p);
                return name;
            }
            seq++;
        }
        if (!eol) {
            break;
        }
        p = eol + 1;
    }
    return NULL;
}

static int64_t playlist_first(const char *text)
{
    const char *p = strstr(text, "#EXT-X-MEDIA-SEQUENCE:");
    return p ? strtoll(p + strlen("#EXT-X-MEDIA-SEQUENCE:"), NULL, 10) : 0;
}

/* Counts the segments a playlist lists, so we know where its live edge is. */
static int64_t playlist_count(const char *text)
{
    int64_t n = 0;
    for (const char *p = text; *p; ) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        while (len && (p[len - 1] == '\r' || p[len - 1] == ' ')) {
            len--;
        }
        if (len && *p != '#') {
            n++;
        }
        if (!eol) {
            break;
        }
        p = eol + 1;
    }
    return n;
}

/* Streams one segment through the demuxer. */
static bool fetch_segment(esp_http_client_handle_t c, const char *url, ts_t *ts,
                          uint8_t *chunk, uint32_t epoch)
{
    if (esp_http_client_set_url(c, url) != ESP_OK || esp_http_client_open(c, 0) != ESP_OK) {
        return false;
    }
    bool ok = true;
    if (esp_http_client_fetch_headers(c) < 0 || esp_http_client_get_status_code(c) != 200) {
        ok = false;
    }
    while (ok && epoch == s_epoch) {
        int n = esp_http_client_read(c, (char *)chunk, READ_CHUNK);
        if (n < 0) {
            ok = false;
            break;
        }
        if (!n) {
            break;                       /* segment done */
        }
        if (!ts_feed(ts, chunk, (size_t)n, epoch)) {
            ok = false;                  /* cancelled */
        }
    }
    esp_http_client_close(c);
    return ok;
}

/*
 * Walks the playlist for as long as the station is tuned. Each pass takes the
 * segments it has not played yet; with none, it waits and asks again. The
 * ring's backpressure is what paces this: a segment only downloads as fast as
 * the speaker drains its audio, which is what keeps us at the live edge.
 */
static void hls_loop(esp_http_client_handle_t c, const char *url, uint32_t epoch)
{
    char *text = heap_caps_malloc(PLAYLIST_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *seg_url = heap_caps_malloc(URL_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *list_url = heap_caps_malloc(URL_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *name = heap_caps_malloc(URL_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *chunk = heap_caps_malloc(READ_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ts_t *ts = heap_caps_calloc(1, sizeof(ts_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int64_t next = -1;

    if (!text || !seg_url || !list_url || !name || !chunk || !ts) {
        s_status = "OUT OF MEMORY";
        goto done;
    }
    ts->pmt_pid = -1;
    ts->audio_pid = -1;
    snprintf(list_url, URL_MAX, "%s", url);

    if (fetch_text(c, list_url, text, PLAYLIST_MAX) < 0) {
        s_status = "NO PLAYLIST";
        goto done;
    }
    /* A master playlist lists variants rather than segments; take the first. */
    if (strstr(text, "#EXT-X-STREAM-INF")) {
        if (!playlist_segment(text, 0, 0, name, URL_MAX)) {
            s_status = "EMPTY PLAYLIST";
            goto done;
        }
        resolve(list_url, name, seg_url, URL_MAX);
        snprintf(list_url, URL_MAX, "%s", seg_url);
        ESP_LOGI(TAG, "HLS: variant %s", list_url);
        if (fetch_text(c, list_url, text, PLAYLIST_MAX) < 0) {
            s_status = "NO PLAYLIST";
            goto done;
        }
    }

    while (epoch == s_epoch) {
        int64_t first = playlist_first(text);
        int64_t count = playlist_count(text);
        if (count <= 0) {
            s_status = "EMPTY PLAYLIST";
            goto done;
        }
        if (next < 0 || next < first) {
            /* Join near the live edge, one segment back so the first one is
             * certainly complete; and after a stall, rejoin rather than try to
             * catch up through segments the station has already dropped. */
            next = first + count - 2;
            if (next < first) {
                next = first;
            }
        }
        while (next < first + count && epoch == s_epoch) {
            if (!playlist_segment(text, next, first, name, URL_MAX)) {
                break;
            }
            resolve(list_url, name, seg_url, URL_MAX);
            if (!fetch_segment(c, seg_url, ts, chunk, epoch)) {
                if (epoch != s_epoch) {
                    goto done;
                }
                ESP_LOGW(TAG, "segment %lld failed", (long long)next);
            }
            next++;
        }
        if (epoch != s_epoch) {
            goto done;
        }
        /* Nothing new yet: the station publishes a segment at a time. */
        vTaskDelay(pdMS_TO_TICKS(SEG_POLL_MS));
        if (fetch_text(c, list_url, text, PLAYLIST_MAX) < 0) {
            s_status = "PLAYLIST GONE";
            goto done;
        }
    }
done:
    free(text);
    free(seg_url);
    free(list_url);
    free(name);
    free(chunk);
    free(ts);
}

static void net_task(void *arg)
{
    uint32_t epoch = (uint32_t)(uintptr_t)arg;
    const muse_radio_station_t *st = muse_radio_station(s_index);
    uint8_t *chunk = heap_caps_malloc(READ_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    icy_t *icy = heap_caps_calloc(1, sizeof(icy_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    esp_http_client_config_t cfg = {
        .timeout_ms = HTTP_TIMEOUT_MS,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .disable_auto_redirect = true,
        .event_handler = on_header,
    };
    hdrs_t hdrs = { 0 };
    esp_http_client_handle_t c = NULL;
    /* Relative segment names resolve against the playlist's *final* URL, so
     * the redirect loop keeps it rather than the one in the table. */
    char *final_url = heap_caps_malloc(URL_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int status = 0;

    if (!st || !chunk || !icy || !final_url) {
        s_status = "OUT OF MEMORY";
        goto out;
    }
    snprintf(final_url, URL_MAX, "%s", st->url);
    cfg.url = st->url;
    cfg.user_data = &hdrs;
    c = esp_http_client_init(&cfg);
    if (!c) {
        s_status = "OUT OF MEMORY";
        goto out;
    }
    esp_http_client_set_header(c, "User-Agent", UA);
    esp_http_client_set_header(c, "Icy-MetaData", "1");
    /*
     * Triton (revma), which carries most of the Taiwanese stations, answers
     * the published URL with a 302 to a short-lived token URL on whichever
     * node is nearest -- and it drops from https to http on the way. So the
     * redirect is followed here rather than left to the client: this way the
     * headers from the *final* response are the ones that pick the decoder.
     */
    for (int hop = 0; ; hop++) {
        if (esp_http_client_open(c, 0) != ESP_OK) {
            ESP_LOGW(TAG, "connect failed (internal heap %u free)",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            s_status = "CAN'T REACH STATION";
            goto out;
        }
        if (esp_http_client_fetch_headers(c) < 0) {
            s_status = "NO RESPONSE";
            goto out;
        }
        status = esp_http_client_get_status_code(c);
        if (status < 300 || status > 399) {
            break;
        }
        if (hop >= MAX_HOPS || !hdrs.location[0]) {
            ESP_LOGW(TAG, "redirect %d with nowhere to go", status);
            s_status = "STATION KEEPS REDIRECTING";
            goto out;
        }
        ESP_LOGI(TAG, "%d -> %s", status, hdrs.location);
        esp_http_client_close(c);
        if (esp_http_client_set_url(c, hdrs.location) != ESP_OK) {
            s_status = "BAD REDIRECT";
            goto out;
        }
        snprintf(final_url, URL_MAX, "%s", hdrs.location);
        hdrs.location[0] = '\0';
        hdrs.typed = false;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d", status);
        s_status = status == 404 ? "STATION GONE" : "STATION REFUSED US";
        goto out;
    }
    /* MP3 and AAC are what the board decodes, and HLS carries one of the two
     * inside MPEG-TS. Ogg is not, and would otherwise be silence with no
     * explanation. */
    if (hdrs.typed && !hdrs.mp3 && !hdrs.aac && !hdrs.hls) {
        ESP_LOGW(TAG, "unplayable Content-Type");
        s_status = "UNPLAYABLE STREAM";
        goto out;
    }
    if (hdrs.hls) {
        /* The playlist's body is of no use unread; hls_loop drives the same
         * handle itself, a request at a time. Its codec comes from the PMT,
         * not from these headers. */
        esp_http_client_close(c);
        ESP_LOGI(TAG, "%s: HLS", st->name);
        hls_loop(c, final_url, epoch);
        goto out;
    }
    s_is_aac = hdrs.aac;
    icy->metaint = hdrs.metaint;
    icy->audio_left = icy->metaint;
    icy->need = -1;
    ESP_LOGI(TAG, "%s: connected, %s, metaint %u", st->name, s_is_aac ? "AAC" : "MP3",
             (unsigned)icy->metaint);

    for (;;) {
        if (epoch != s_epoch) {
            goto out;
        }
        int got = esp_http_client_read(c, (char *)chunk, READ_CHUNK);
        if (got <= 0) {
            /* A live stream has no end, so either is a real break. */
            ESP_LOGW(TAG, "stream ended (%d)", got);
            s_status = "STREAM DROPPED";
            goto out;
        }
        if (!demux(icy, chunk, (size_t)got, epoch)) {
            goto out;   /* cancelled */
        }
    }
out:
    if (c) {
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
    }
    free(chunk);
    free(icy);
    free(final_url);
    if (epoch == s_epoch) {
        s_net_live = false;
    }
    s_net_done = true;
    vTaskSuspend(NULL);   /* the next play()/stop() deletes it, freeing its stack */
}

/* ------------------------------------------------------------- the decoder */

typedef struct {
    uint32_t step;
    uint32_t pos;
    int16_t prev;
} resampler_t;

/* A third copy of this (muse_chat_session.cpp and muse_voice_call_core.c have
 * the others); unifying them is noted in plan/radio.md. */
static void resampler_init(resampler_t *r, int in_rate, int out_rate)
{
    r->step = (uint32_t)(((uint64_t)in_rate << 16) / out_rate);
    r->pos = 0;
    r->prev = 0;
}

/* Linear interpolation; state carries across calls. out holds n*out/in + 2. */
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

/* Waits for `want` bytes in the ring. False if the tune was cancelled or the
 * fetch gave up first. */
static bool wait_for(size_t want, uint32_t epoch)
{
    while (epoch == s_epoch) {
        if (xStreamBufferBytesAvailable(s_ring) >= want) {
            return true;
        }
        if (!s_net_live) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return false;
}

static void play_task(void *arg)
{
    uint32_t epoch = (uint32_t)(uintptr_t)arg;
    mp3dec_t dec;
    void *aac = NULL;
    uint8_t *mp3 = heap_caps_malloc(WORK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm = heap_caps_malloc(PCM_MAX * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *out = heap_caps_malloc((PCM_MAX + 8) * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t len = 0;
    int rate = 0;
    bool is_aac = false;
    resampler_t rs = { 0 };

    mp3dec_init(&dec);
    if (!mp3 || !pcm || !out) {
        s_status = "OUT OF MEMORY";
        goto out;
    }
    s_state = MUSE_RADIO_BUFFERING;
    if (!wait_for(PREBUFFER, epoch)) {
        goto out;
    }
    /* The fetch has the headers by now -- nothing reaches the ring before
     * they are in -- so this is where the format is known. */
    is_aac = s_is_aac;
    if (is_aac) {
        esp_aac_dec_cfg_t cfg = ESP_AAC_DEC_CONFIG_DEFAULT();
        cfg.aac_plus_enable = true;   /* these stations are HE-AAC, not plain LC */
        if (esp_aac_dec_open(&cfg, sizeof(cfg), &aac) != ESP_AUDIO_ERR_OK || !aac) {
            ESP_LOGW(TAG, "no AAC decoder");
            s_status = "NO AAC DECODER";
            goto out;
        }
    }
    s_state = MUSE_RADIO_PLAYING;

    while (epoch == s_epoch) {
        size_t room = WORK - len;
        if (room) {
            size_t got = xStreamBufferReceive(s_ring, mp3 + len, room, pdMS_TO_TICKS(RECV_WAIT_MS));
            len += got;
        }
        if (len <= (is_aac ? AAC_HOLD : MP3_HOLD)) {
            /* The link fell behind the speaker. Hold the station on screen and
             * refill rather than dropping back to the list. */
            if (!s_net_live) {
                goto out;
            }
            s_state = MUSE_RADIO_BUFFERING;
            if (!wait_for(REFILL, epoch)) {
                goto out;
            }
            s_state = MUSE_RADIO_PLAYING;
            continue;
        }
        size_t off = 0;
        while (len - off > (is_aac ? AAC_HOLD : MP3_HOLD) && epoch == s_epoch) {
            int samples, hz, channels;
            if (is_aac) {
                esp_audio_dec_in_raw_t raw = { .buffer = mp3 + off, .len = len - off };
                esp_audio_dec_out_frame_t frame = { .buffer = (uint8_t *)pcm,
                                                    .len = PCM_MAX * sizeof(int16_t) };
                esp_audio_dec_info_t info = { 0 };
                esp_audio_err_t err = esp_aac_dec_decode(aac, &raw, &frame, &info);
                if (err == ESP_AUDIO_ERR_DATA_LACK) {
                    break;   /* the rest of this frame is still on the wire */
                }
                /* A decoder that consumed nothing and failed is sitting on
                 * junk; step over a byte so it can find the next sync word
                 * rather than spinning on the same one. */
                off += raw.consumed ? raw.consumed : 1;
                if (err != ESP_AUDIO_ERR_OK) {
                    continue;
                }
                channels = info.channel ? info.channel : 1;
                hz = (int)info.sample_rate;
                samples = (int)(frame.decoded_size / sizeof(int16_t)) / channels;
            } else {
                mp3dec_frame_info_t info;
                samples = mp3dec_decode_frame(&dec, mp3 + off, len - off, pcm, &info);
                if (!info.frame_bytes) {
                    break;   /* needs more bytes to find a header */
                }
                off += info.frame_bytes;
                channels = info.channels;
                hz = info.hz;
            }
            if (samples <= 0) {
                continue;
            }
            if (channels == 2) {
                for (int k = 0; k < samples; k++) {
                    pcm[k] = (pcm[2 * k] + pcm[2 * k + 1]) / 2;
                }
            }
            if (rate != hz) {
                ESP_LOGI(TAG, "stream: %s, %d Hz, %d ch", is_aac ? "AAC" : "MP3", hz, channels);
                rate = hz;
                resampler_init(&rs, hz, MUSE_AUDIO_RATE);
            }
            size_t n = resample(&rs, pcm, samples, out);
            /* Blocking, so the speaker is what paces this whole task. */
            for (size_t i = 0; i < n && epoch == s_epoch; i += MUSE_AUDIO_CHUNK) {
                size_t left = n - i;
                muse_audio_write(out + i, left < MUSE_AUDIO_CHUNK ? left : MUSE_AUDIO_CHUNK);
            }
        }
        if (off) {
            memmove(mp3, mp3 + off, len - off);
            len -= off;
        } else if (len >= WORK) {
            /* A full window with no frame in it is junk, not audio. */
            ESP_LOGW(TAG, "no frame in %u bytes, resyncing", (unsigned)len);
            len = 0;
            if (is_aac) {
                esp_aac_dec_reset(aac);
            } else {
                mp3dec_init(&dec);
            }
        }
    }
out:
    if (aac) {
        esp_aac_dec_close(aac);
    }
    free(mp3);
    free(pcm);
    free(out);
    if (epoch == s_epoch) {
        s_state = s_status[0] ? MUSE_RADIO_FAILED : MUSE_RADIO_IDLE;
        if (s_state == MUSE_RADIO_FAILED) {
            ESP_LOGW(TAG, "stopped: %s", s_status);
        }
    }
    s_play_done = true;
    vTaskSuspend(NULL);
}

/* ------------------------------------------------------------------ the UI */

/* Deletes workers that have run their course, so the next tune can start. */
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

void muse_radio_stop(void)
{
    if (s_index < 0 && !s_net && !s_play) {
        return;
    }
    s_epoch++;            /* both workers leave on their next pass */
    s_index = -1;
    s_state = MUSE_RADIO_IDLE;
    s_status = "";
    set_title("");
    /* Give them the time one blocking wait costs, then take their stacks back. */
    for (int i = 0; i < 40 && (!s_net_done || !s_play_done); i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    reap();
    if (s_ring) {
        xStreamBufferReset(s_ring);
    }
}

bool muse_radio_play(int i)
{
    if (i < 0 || i >= STATIONS) {
        return false;
    }
    muse_radio_stop();
    reap();
    if (s_net || s_play) {
        ESP_LOGW(TAG, "the last tune hasn't let go yet");
        return false;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (!s_ring) {
        s_ring = xStreamBufferCreateWithCaps(RING, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!s_lock || !s_ring) {
        s_status = "OUT OF MEMORY";
        s_state = MUSE_RADIO_FAILED;
        return false;
    }
    s_status = "";
    set_title("");
    s_index = i;
    s_state = MUSE_RADIO_CONNECTING;
    s_is_aac = false;
    s_net_live = true;
    void *epoch = (void *)(uintptr_t)s_epoch;
    /* Stacks in PSRAM: minimp3 wants ~16 KB and internal RAM is down to a few
     * KB free on this board, which plan/radio.md records. */
    if (xTaskCreatePinnedToCoreWithCaps(net_task, "radio_net", 8 * 1024, epoch, 5, &s_net, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_net = NULL;
        s_net_live = false;
        s_index = -1;
        s_status = "NO ROOM FOR THE STREAM";
        s_state = MUSE_RADIO_FAILED;
        return false;
    }
    if (xTaskCreatePinnedToCoreWithCaps(play_task, "radio_play", 32 * 1024, epoch, 5, &s_play, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        s_play = NULL;
        s_status = "NO ROOM FOR THE DECODER";
        s_state = MUSE_RADIO_FAILED;
        muse_radio_stop();
        return false;
    }
    ESP_LOGI(TAG, "tuning %s", s_stations[i].name);
    return true;
}

void muse_radio_tick(void)
{
    reap();
    if (s_index < 0) {
        return;
    }
    /* Muse and the radio would write to the same speaker. The radio is the one
     * that gives way, and a live stream can't be resumed, so it stops. */
    muse_mode_t mode = muse_state_mode(NULL);
    if (mode == MUSE_MODE_LISTENING || mode == MUSE_MODE_THINKING || mode == MUSE_MODE_SPEAKING) {
        ESP_LOGI(TAG, "Muse is talking; stopping the radio");
        muse_radio_stop();
    }
}
