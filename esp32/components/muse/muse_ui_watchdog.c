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

#include "muse_ui_watchdog.h"

#include <stdatomic.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "muse_state.h"
#if CONFIG_MUSE_VIDEO
#include "muse_video.h"
#endif

static atomic_uint s_beats;

void muse_ui_watchdog_beat(void)
{
    atomic_fetch_add_explicit(&s_beats, 1u, memory_order_relaxed);
}

#if CONFIG_MUSE_UI_WATCHDOG

static const char *TAG = "ui_wdt";

#define CHECK_MS 1000
#define LIMIT_S  CONFIG_MUSE_UI_WATCHDOG_S

/*
 * The frame timer stops for reasons that are not faults, and firing then would
 * be worse than not firing at all -- a board that reboots itself every time the
 * screen sleeps is not a diagnosis, it is a new bug. Both of these stop LVGL on
 * purpose:
 *
 *  - the screen asleep on battery (muse_input.c's update_power() -> the board's
 *    display_pause(true)). Its full condition is narrower than this, so asking
 *    only whether the device is asleep errs towards staying quiet;
 *  - the phone camera, which pauses LVGL for as long as the view is open.
 */
static bool ui_should_be_running(void)
{
    if (muse_state_asleep()) {
        return false;
    }
#if CONFIG_MUSE_VIDEO
    if (muse_video_is_open()) {
        return false;
    }
#endif
    return true;
}

static void watch(void *arg)
{
    (void)arg;
    unsigned last = atomic_load(&s_beats);
    int stalled_ms = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CHECK_MS));

        unsigned now = atomic_load(&s_beats);
        if (now != last || !ui_should_be_running()) {
            last = now;
            stalled_ms = 0;
            continue;
        }
        stalled_ms += CHECK_MS;
        if (stalled_ms < LIMIT_S * 1000) {
            continue;
        }
        /*
         * Said before the panic, because this line is the one thing that
         * survives on a board nobody was capturing: it names the fault even
         * when the core dump cannot be read back.
         */
        ESP_LOGE(TAG, "UI task has not drawn a frame in %d s -- panicking so the "
                      "core dump keeps every task's backtrace", LIMIT_S);
        /*
         * abort() here, not from the UI task, which is the point: that task is
         * stuck and cannot run anything. The dump records all tasks, so the UI
         * task's stack -- and whatever it is blocked on -- is in it, under this
         * task's name as the one that crashed.
         */
        abort();
    }
}

void muse_ui_watchdog_start(void)
{
    static bool started;

    if (started) {
        return;
    }
    started = true;
    /* Lowest priority above idle, and a stack that only has to hold a log
     * line: it must never be the reason something else misses its deadline. */
    if (xTaskCreate(watch, "ui_wdt", 2560, NULL, 1, NULL) != pdPASS) {
        ESP_LOGW(TAG, "no task: a frozen UI will not be caught");
        started = false;
        return;
    }
    ESP_LOGI(TAG, "watching the frame timer, %d s to panic", LIMIT_S);
}

#else  /* !CONFIG_MUSE_UI_WATCHDOG */

void muse_ui_watchdog_start(void)
{
}

#endif
