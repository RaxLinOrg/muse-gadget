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
#include "minimp3.h"
#include "muse_audio.h"
#include "muse_state.h"

static const char *TAG = "radio";

/*
 * Every URL here was checked by hand for a 200 and an `audio/mpeg`
 * Content-Type, because there is no AAC or Ogg decoder on the board and a
 * station that answers either would be silence with no explanation.
 *
 * Rax asked for five Taiwanese stations on 2026-11-05. Only this one is MP3:
 * ICRT, 中廣流行網, 中廣音樂網 and 飛碟 are all AAC on Triton/revma, and
 * 好事聯播網 is HLS. They are reachable and they send ICY metadata -- they
 * need a decoder, not a different URL. plan/radio.md has the measurement.
 */
static const muse_radio_station_t s_stations[] = {
    { "寶島新聲 FM98.5", "台語・華語流行", "http://125.227.87.206:8000/FM98.5" },
    { "BBC WORLD SERVICE", "NEWS - ENGLISH", "https://stream.live.vc.bbcmedia.co.uk/bbc_world_service" },
    { "RADIO PARADISE",    "ECLECTIC MIX",   "https://stream.radioparadise.com/mp3-128" },
    { "FRANCE INTER",      "TALK - FRANCAIS", "https://icecast.radiofrance.fr/franceinter-midfi.mp3" },
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
typedef struct {
    size_t metaint;
    bool typed;
    bool mp3;
} hdrs_t;

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
    }
    return ESP_OK;
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
    int status = 0;

    if (!st || !chunk || !icy) {
        s_status = "OUT OF MEMORY";
        goto out;
    }
    cfg.url = st->url;
    cfg.user_data = &hdrs;
    c = esp_http_client_init(&cfg);
    if (!c) {
        s_status = "OUT OF MEMORY";
        goto out;
    }
    esp_http_client_set_header(c, "User-Agent", UA);
    esp_http_client_set_header(c, "Icy-MetaData", "1");
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
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP %d", status);
        s_status = status == 404 ? "STATION GONE" : "STATION REFUSED US";
        goto out;
    }
    /* No AAC or Ogg decoder on this board, so anything but MP3 would be
     * silence with no explanation. Refuse it instead. */
    if (hdrs.typed && !hdrs.mp3) {
        ESP_LOGW(TAG, "Content-Type is not MP3");
        s_status = "NOT AN MP3 STREAM";
        goto out;
    }
    icy->metaint = hdrs.metaint;
    icy->audio_left = icy->metaint;
    icy->need = -1;
    ESP_LOGI(TAG, "%s: connected, metaint %u", st->name, (unsigned)icy->metaint);

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
    uint8_t *mp3 = heap_caps_malloc(WORK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *pcm = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *out = heap_caps_malloc((MINIMP3_MAX_SAMPLES_PER_FRAME + 8) * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t len = 0;
    int rate = 0;
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
    s_state = MUSE_RADIO_PLAYING;

    while (epoch == s_epoch) {
        size_t room = WORK - len;
        if (room) {
            size_t got = xStreamBufferReceive(s_ring, mp3 + len, room, pdMS_TO_TICKS(RECV_WAIT_MS));
            len += got;
        }
        if (len <= MP3_HOLD) {
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
        while (len - off > MP3_HOLD && epoch == s_epoch) {
            mp3dec_frame_info_t info;
            int samples = mp3dec_decode_frame(&dec, mp3 + off, len - off, pcm, &info);
            if (!info.frame_bytes) {
                break;   /* needs more bytes to find a header */
            }
            off += info.frame_bytes;
            if (!samples) {
                continue;
            }
            if (info.channels == 2) {
                for (int k = 0; k < samples; k++) {
                    pcm[k] = (pcm[2 * k] + pcm[2 * k + 1]) / 2;
                }
            }
            if (rate != info.hz) {
                ESP_LOGI(TAG, "stream: %d Hz, %d ch, %d kbps", info.hz, info.channels, info.bitrate_kbps);
                rate = info.hz;
                resampler_init(&rs, info.hz, MUSE_AUDIO_RATE);
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
            ESP_LOGW(TAG, "no MP3 frame in %u bytes, resyncing", (unsigned)len);
            len = 0;
            mp3dec_init(&dec);
        }
    }
out:
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
