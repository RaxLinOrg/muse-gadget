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

#include "muse_pet_sound.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "minimp3.h"

#include "muse_audio.h"

static const char *TAG = "pet_sound";

/* The longest clip is the four-bar sting; the cap is a little over it. */
#define MAX_SECS 5

typedef struct {
    const uint8_t *start;
    const uint8_t *end;
} clip_t;

/* EMBED_FILES in CMakeLists.txt puts each clip in the image under these. */
extern const uint8_t pet_food_start[] asm("_binary_pet_food_mp3_start");
extern const uint8_t pet_food_end[] asm("_binary_pet_food_mp3_end");
extern const uint8_t pet_dance_start[] asm("_binary_pet_dance_mp3_start");
extern const uint8_t pet_dance_end[] asm("_binary_pet_dance_mp3_end");
extern const uint8_t pet_thanks_start[] asm("_binary_pet_thanks_mp3_start");
extern const uint8_t pet_thanks_end[] asm("_binary_pet_thanks_mp3_end");

static clip_t clip_for(muse_reaction_t r)
{
    switch (r) {
    case MUSE_REACT_CARROT:
        return (clip_t){ pet_food_start, pet_food_end };
    case MUSE_REACT_NOTE:
        return (clip_t){ pet_dance_start, pet_dance_end };
    case MUSE_REACT_GIFT:
        return (clip_t){ pet_thanks_start, pet_thanks_end };
    default:
        return (clip_t){ NULL, NULL };   /* a plain pet makes no noise */
    }
}

/* -1 when there is nothing to play; otherwise the reaction asked for. */
static volatile int s_want = -1;
static volatile bool s_playing;

void muse_pet_sound_play(muse_reaction_t r)
{
    if (s_want >= 0 || s_playing) {
        return;   /* one at a time: a second press rides the first clip out */
    }
    if (!clip_for(r).start) {
        return;
    }
    s_want = (int)r;
}

bool muse_pet_sound_busy(void)
{
    return s_want >= 0 || s_playing;
}

/* ------------------------------------------------------------------ decode */

typedef struct {
    TaskHandle_t caller;
    clip_t clip;
    int16_t *pcm;
    size_t n;
} job_t;

static void decode_body(void *arg);

static size_t decode(clip_t c, int16_t **pcm_out)
{
    size_t len = (size_t)(c.end - c.start);
    mp3dec_t *dec = heap_caps_malloc(sizeof(*dec), MALLOC_CAP_SPIRAM);
    int16_t *frame = heap_caps_malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    size_t cap = (size_t)MUSE_AUDIO_RATE * MAX_SECS;
    int16_t *out = heap_caps_malloc(cap * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!dec || !frame || !out) {
        free(dec);
        free(frame);
        free(out);
        ESP_LOGW(TAG, "no room to decode");
        return 0;
    }
    mp3dec_init(dec);
    size_t off = 0, n = 0;
    while (off < len) {
        mp3dec_frame_info_t info;
        int samples = mp3dec_decode_frame(dec, c.start + off, len - off, frame, &info);
        if (!info.frame_bytes) {
            break;
        }
        off += info.frame_bytes;
        if (!samples) {
            continue;
        }
        /*
         * The clips ship at the speaker's own rate so there is no resampler
         * here. A clip at any other rate would come out at the wrong pitch,
         * which is a quiet kind of wrong, so refuse it loudly instead.
         */
        if (info.hz != MUSE_AUDIO_RATE) {
            ESP_LOGE(TAG, "clip is %d Hz, not %d: re-encode it", info.hz, MUSE_AUDIO_RATE);
            n = 0;
            break;
        }
        if (info.channels == 2) {
            for (int k = 0; k < samples; k++) {
                frame[k] = (int16_t)((frame[2 * k] + frame[2 * k + 1]) / 2);
            }
        }
        if (n + (size_t)samples > cap) {
            break;
        }
        memcpy(out + n, frame, (size_t)samples * sizeof(int16_t));
        n += (size_t)samples;
    }
    free(dec);
    free(frame);
    if (!n) {
        free(out);
        return 0;
    }
    *pcm_out = out;
    return n;
}

/* minimp3 wants ~16 KB of stack, more than the voice task has, so it decodes
 * on a task of its own with a PSRAM stack -- as the MP3 self-test does. */
static size_t decode_off_task(clip_t c, int16_t **pcm_out)
{
    job_t job = { xTaskGetCurrentTaskHandle(), c, NULL, 0 };
    TaskHandle_t task;
    if (xTaskCreatePinnedToCoreWithCaps(decode_body, "pet_mp3", 32 * 1024, &job, 5, &task, 0,
                                        MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGW(TAG, "no room for the decode task");
        return 0;
    }
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    vTaskDeleteWithCaps(task);
    *pcm_out = job.pcm;
    return job.n;
}

static void decode_body(void *arg)
{
    job_t *job = (job_t *)arg;
    job->n = decode(job->clip, &job->pcm);
    xTaskNotifyGive(job->caller);
    vTaskSuspend(NULL);   /* the caller deletes it, which frees the PSRAM stack */
}

/* ----------------------------------------------------------------- playback */

void muse_pet_sound_pump(void)
{
    int want = s_want;
    if (want < 0) {
        return;
    }
    s_want = -1;
    s_playing = true;
    int16_t *pcm = NULL;
    size_t n = decode_off_task(clip_for((muse_reaction_t)want), &pcm);
    ESP_LOGI(TAG, "reaction %d: %u samples, %.2f s", want, (unsigned)n, (double)n / MUSE_AUDIO_RATE);
    for (size_t i = 0; i < n; i += MUSE_AUDIO_CHUNK) {
        size_t left = n - i;
        muse_audio_write(pcm + i, left < MUSE_AUDIO_CHUNK ? left : MUSE_AUDIO_CHUNK);
    }
    free(pcm);
    s_playing = false;
}
