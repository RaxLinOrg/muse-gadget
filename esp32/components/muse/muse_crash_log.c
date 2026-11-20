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


#include "muse_crash_log.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_log_write.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

void muse_crash_log_verbose(bool on)
{
    esp_log_level_set("*", on ? ESP_LOG_DEBUG : (esp_log_level_t)CONFIG_LOG_DEFAULT_LEVEL);
    if (on) {
        /* Pinned back down: spi_master logs twice per bus transaction from
         * inside lcd_send, which both drowns everything else and overflowed
         * that task's stack when it was let through. Nothing about the panel
         * bus is worth a crash to see. */
        esp_log_level_set("spi_master", ESP_LOG_INFO);
    }
}

#if CONFIG_MUSE_CRASH_LOG

#define RING_BYTES (CONFIG_MUSE_CRASH_LOG_KB * 1024)
/* Small on purpose: this buffer sits on the stack of whichever task is
 * logging, and with DEBUG turned on spi_master logs from inside lcd_send,
 * whose stack is tight enough that 256 bytes here overflowed it (measured
 * 2026-11-19: "***ERROR*** A stack overflow in task lcd_send"). */
#define RING_LINE_MAX   128
#define RING_MAGIC 0x474f4c4du   /* "MLOG" little-endian: what to grep a dump for */

/* One named struct in .bss, not a heap buffer: a core dump stores .bss whole,
 * and `print s_ring` wants a symbol with the head and length beside the bytes,
 * since the bytes on their own do not say where the oldest one is. */
static struct {
    uint32_t magic;
    uint32_t head;          /* offset of the oldest byte */
    uint32_t len;           /* bytes in use, up to RING_BYTES */
    char buf[RING_BYTES];
} s_ring = { .magic = RING_MAGIC };

/* A spinlock, not a mutex: this runs inside whichever task is logging,
 * including ones that must not block, and the critical section is two
 * memcpys of at most RING_LINE_MAX bytes. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_next = vprintf;
static bool s_installed;

static void push(const char *s, size_t n)
{
    if (n == 0) {
        return;
    }
    if (n > RING_BYTES) {       /* keep the end of an over-long line */
        s += n - RING_BYTES;
        n = RING_BYTES;
    }
    portENTER_CRITICAL(&s_mux);
    size_t tail = (s_ring.head + s_ring.len) % RING_BYTES;
    size_t first = RING_BYTES - tail;
    if (first > n) {
        first = n;
    }
    memcpy(s_ring.buf + tail, s, first);
    if (first < n) {
        memcpy(s_ring.buf, s + first, n - first);
    }
    if (s_ring.len + n <= RING_BYTES) {
        s_ring.len += n;
    } else {
        s_ring.head = (tail + n) % RING_BYTES;   /* wrote over the oldest */
        s_ring.len = RING_BYTES;
    }
    portEXIT_CRITICAL(&s_mux);
}

/* The console sink still sees the original arguments, so what goes out of the
 * port is never the truncated copy; only the ring's own line is clipped. */
static int hook(const char *fmt, va_list ap)
{
    va_list mine;
    va_copy(mine, ap);
    char line[RING_LINE_MAX];
    int n = vsnprintf(line, sizeof(line), fmt, mine);
    va_end(mine);
    if (n > 0) {
        push(line, (size_t)n < sizeof(line) - 1 ? (size_t)n : sizeof(line) - 1);
    }
    return s_next ? s_next(fmt, ap) : 0;
}

void muse_crash_log_init(void)
{
    if (s_installed) {
        return;
    }
    s_ring.magic = RING_MAGIC;
    s_ring.head = 0;
    s_ring.len = 0;
    vprintf_like_t prev = esp_log_set_vprintf(hook);
    if (prev && prev != hook) {
        s_next = prev;
    }
    s_installed = true;
}

void muse_crash_log_dump(void)
{
    /* Written with fwrite rather than the log, because by the time this is
     * called the thing being diagnosed may be whatever makes logging block. */
    portENTER_CRITICAL(&s_mux);
    uint32_t head = s_ring.head;
    uint32_t len = s_ring.len;
    portEXIT_CRITICAL(&s_mux);

    printf("\n@crashlog.begin %u\n", (unsigned)len);
    size_t first = RING_BYTES - head;
    if (first > len) {
        first = len;
    }
    fwrite(s_ring.buf + head, 1, first, stdout);
    if (first < len) {
        fwrite(s_ring.buf, 1, len - first, stdout);
    }
    printf("\n@crashlog.end\n");
    fflush(stdout);
}

#else   /* the ring is off; the Debug log switch above still works */

void muse_crash_log_init(void) {}
void muse_crash_log_dump(void) {}

#endif
