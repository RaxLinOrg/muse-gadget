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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Settings kept as a file on the user's VM, so Muse can read and change them
 * in conversation and the gadget picks the change up.
 *
 * The transport is Link's own Noise session: POST /fs/read and POST /fs/write
 * on the VM daemon, the routes AGENTS.md lists under "Requests to Muse's VM
 * from your code". A path is relative to the user's home on the VM.
 *
 * The firmware still carries the defaults. The VM is where they go to be
 * edited, not where they come from the first time: muse_vm_config_load()
 * seeds the file from the firmware's own defaults when the VM has none, so a
 * fresh VM ends up with something to reset back to.
 *
 * Every call here BLOCKS on a network round trip, so call them from a task of
 * your own: never from the LVGL task, and never from the session task (which
 * is where the reply callback runs, so doing it there would deadlock).
 */

/* The most a settings file may be. Response frames have to fit the session's
 * inbound buffer, and /fs/read escapes the content, so this stays well under
 * the 1024-byte read AGENTS.md asks for. */
#define MUSE_VM_CONFIG_MAX 768

typedef enum {
    MUSE_VM_CONFIG_OK = 0,
    MUSE_VM_CONFIG_MISSING,   /* the VM has no such file */
    MUSE_VM_CONFIG_OFFLINE,   /* no session to the VM */
    MUSE_VM_CONFIG_FAILED,    /* the request or the reply did not work out */
    MUSE_VM_CONFIG_TOO_BIG,   /* the file is longer than MUSE_VM_CONFIG_MAX */
} muse_vm_config_err_t;

/* Reads `path` into `out`. OK, MISSING, OFFLINE, FAILED or TOO_BIG. */
muse_vm_config_err_t muse_vm_config_read(const char *path, char *out, size_t cap);

/* Writes `text` to `path`, replacing whatever is there. OK, OFFLINE or FAILED. */
muse_vm_config_err_t muse_vm_config_write(const char *path, const char *text);

/*
 * Reads `path`; if the VM has no such file, writes `defaults` there and
 * returns those instead. Point 1 of the design: the firmware's defaults are
 * the fallback AND the seed, so the VM always has a copy to go back to.
 * `seeded` (may be NULL) says which of the two happened.
 */
muse_vm_config_err_t muse_vm_config_load(const char *path, const char *defaults,
                                         char *out, size_t cap, bool *seeded);

/*
 * The raw reply to one POST, for the bench: the daemon's own JSON, before
 * this file tries to understand it. Returns the HTTP status, 0 if the
 * session is down, or negative on a transport error.
 */
int muse_vm_config_post_raw(const char *route, const char *body, char *out, size_t cap);

/*
 * Bench round trip, from the serial console: reads a test file, seeds it from
 * a default if the VM has none, reads it back, and prints each step with the
 * daemon's own reply. Runs on a task of its own and returns at once. This is
 * what proves the route works before anything depends on it.
 */
void muse_vm_config_selftest(void);

#ifdef __cplusplus
}
#endif
