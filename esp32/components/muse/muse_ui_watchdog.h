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

/*
 * A watchdog for the one task the ordinary ones cannot see.
 *
 * CONFIG_ESP_INT_WDT and CONFIG_ESP_TASK_WDT_EN are both off on these boards
 * (sdkconfig.defaults says why: PSRAM cache stalls can outlast the int-WDT's
 * 300 ms window). Even with the task watchdog on it would not catch this: it
 * watches the idle tasks, and a UI task blocked on a mutex leaves idle running
 * quite happily. So a deadlocked screen freezes and the board sits there, lit,
 * with nothing on the serial port and nothing in flash -- which is exactly how
 * one was found on 2026-11-19, hours after the fact, with no way left to tell
 * what had stopped.
 *
 * This watches the LVGL frame timer instead, which is the thing that actually
 * stops. beat() is called from it; a low-priority task checks that the count
 * is still moving and panics when it is not, so the core dump names every
 * task's backtrace including whatever the UI task was waiting on.
 */

/* From the LVGL frame timer, on the UI task. Cheap enough to call every frame. */
void muse_ui_watchdog_beat(void);

/* Once, after the frame timer exists. Does nothing without
 * CONFIG_MUSE_UI_WATCHDOG. */
void muse_ui_watchdog_start(void);
