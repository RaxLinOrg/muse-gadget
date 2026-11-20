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

#ifdef __cplusplus
extern "C" {
#endif

/* The last few KB of console log, kept in internal RAM so that a core dump
 * carries it.
 *
 * The core dump from muse_ui_watchdog gives every task's backtrace, which says
 * where the firmware stopped but nothing about what it was doing on the way
 * there. The words are in the console log, and the console log exists only for
 * as long as somebody is capturing the port -- which, for a gadget in a
 * pocket, is never.
 *
 * So the log is mirrored into a fixed ring in .bss. With
 * CONFIG_ESP_COREDUMP_CAPTURE_DRAM the dump stores .bss whole, so the ring
 * comes back with the backtraces, days later, over USB. It has to be internal
 * RAM: the dump explicitly skips anything in PSRAM, which is also why
 * main/diagnostic_log.c's 16 KB PSRAM ring is no use here (it is never
 * installed in this firmware either, and it keeps only "link.*" lines).
 *
 * ESP_EARLY_LOGx and anything logged from an interrupt go straight out of
 * esp_rom_printf and are not seen here. */
void muse_crash_log_init(void);

/* Print the ring, oldest line first, between @crashlog.begin and
 * @crashlog.end. For the moment just before a deliberate panic, when the
 * serial port may still have somebody on the other end. */
void muse_crash_log_dump(void);

/* The Debug log switch in Settings. On raises every tag to DEBUG, off puts it
 * back to the level the image was built with. What this actually reveals is
 * limited by CONFIG_LOG_MAXIMUM_LEVEL at build time: statements above it were
 * never compiled in and no runtime switch brings them back. */
void muse_crash_log_verbose(bool on);

#ifdef __cplusplus
}
#endif
