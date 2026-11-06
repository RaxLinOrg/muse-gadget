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

#include "muse_vm_config.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "muse_link.h"

static const char *TAG = "muse_vmcfg";

/* A round trip over the session, including the daemon touching the file. */
#define REQ_TIMEOUT_MS 8000
#define REQ_POLL_MS 20
/* The reply is one small JSON object; this is the escaped content plus slack. */
#define REPLY_MAX 2048

/*
 * One request at a time. The reply arrives on the session task, which must
 * not block, so it only copies into this and sets done; the calling task
 * polls. A second caller waits here rather than racing the first's buffer.
 */
static SemaphoreHandle_t s_lock;
static struct {
    char *buf;
    size_t cap;
    size_t len;
    int status;
    bool done;
    bool overflow;
} s_reply;

static void lock_init(void)
{
    /* Both callers of this come from ordinary tasks, so a late init is safe;
     * the first one through creates it. */
    static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    taskENTER_CRITICAL(&mux);
    bool make = s_lock == NULL;
    taskEXIT_CRITICAL(&mux);
    if (make) {
        SemaphoreHandle_t sem = xSemaphoreCreateMutex();
        taskENTER_CRITICAL(&mux);
        if (s_lock == NULL) {
            s_lock = sem;
            sem = NULL;
        }
        taskEXIT_CRITICAL(&mux);
        if (sem) {
            vSemaphoreDelete(sem);
        }
    }
}

/* On the session task: copy what fits, note if it did not, and return. */
static void on_reply(void *ctx, int status, const uint8_t *data, size_t len, bool end)
{
    (void)ctx;
    if (status > 0) {
        s_reply.status = status;
    } else if (status < 0) {
        /* -1 the stream is gone, MUSE_LINK_REQ_TOO_LARGE the session died. */
        s_reply.status = status;
        s_reply.done = true;
        return;
    }
    if (data && len) {
        size_t room = s_reply.cap > s_reply.len + 1 ? s_reply.cap - s_reply.len - 1 : 0;
        if (len > room) {
            s_reply.overflow = true;
            len = room;
        }
        if (len) {
            memcpy(s_reply.buf + s_reply.len, data, len);
            s_reply.len += len;
            s_reply.buf[s_reply.len] = '\0';
        }
    }
    if (end) {
        s_reply.done = true;
    }
}

/* Blocking request to `route`. body may be NULL for a GET. Returns the HTTP
 * status, 0 if the session is down, or negative on a transport error. */
static int req_raw(const char *verb, const char *route, const char *body,
                   char *out, size_t cap)
{
    if (!verb || !route || !out || cap == 0) {
        return -1;
    }
    out[0] = '\0';
    if (!muse_link_req_ready()) {
        return 0;
    }
    lock_init();
    if (!s_lock) {
        return -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);

    s_reply.buf = out;
    s_reply.cap = cap;
    s_reply.len = 0;
    s_reply.status = 0;
    s_reply.done = false;
    s_reply.overflow = false;

    const char *headers[] = { "Content-Type", "application/json", NULL };
    /* end_body is false only when a body follows on the same stream. */
    int64_t id = muse_link_req_open(verb, route, body ? headers : NULL, body ? false : true,
                                    on_reply, NULL);
    int rc;
    if (!id) {
        rc = 0;
    } else if (body && !muse_link_req_send(id, body, strlen(body), true, 1000)) {
        muse_link_req_cancel(id);
        rc = -1;
    } else {
        int64_t deadline = esp_timer_get_time() + (int64_t)REQ_TIMEOUT_MS * 1000;
        while (!s_reply.done && esp_timer_get_time() < deadline) {
            vTaskDelay(pdMS_TO_TICKS(REQ_POLL_MS));
        }
        if (!s_reply.done) {
            muse_link_req_cancel(id);
            ESP_LOGW(TAG, "%s timed out after %d ms", route, REQ_TIMEOUT_MS);
            rc = -1;
        } else if (s_reply.overflow) {
            ESP_LOGW(TAG, "%s reply longer than %u bytes", route, (unsigned)cap);
            rc = -1;
        } else {
            rc = s_reply.status;
        }
    }

    s_reply.buf = NULL;
    xSemaphoreGive(s_lock);
    return rc;
}

int muse_vm_config_post_raw(const char *route, const char *body, char *out, size_t cap)
{
    return req_raw("POST", route, body, out, cap);
}

/* The daemon's error shape is not documented, so treat any JSON that carries
 * an "error" or a false "ok" as a refusal and log what it said once. */
static bool reply_is_error(const cJSON *root, const char **why)
{
    const cJSON *err = cJSON_GetObjectItem(root, "error");
    if (cJSON_IsString(err) && err->valuestring) {
        *why = err->valuestring;
        return true;
    }
    const cJSON *ok = cJSON_GetObjectItem(root, "ok");
    if (cJSON_IsBool(ok) && !cJSON_IsTrue(ok)) {
        *why = "ok:false";
        return true;
    }
    return false;
}

/* strcasestr is a GNU extension; this is the little of it that is needed. */
static bool has_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        if (strncasecmp(hay, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

/* "no such file" has to be told apart from a real failure, and the daemon
 * says it in prose rather than a code. */
static bool says_missing(int status, const char *why)
{
    if (status == 404) {
        return true;
    }
    if (!why) {
        return false;
    }
    return has_ci(why, "no such file") || has_ci(why, "not found") ||
           has_ci(why, "enoent") || has_ci(why, "does not exist");
}

muse_vm_config_err_t muse_vm_config_read(const char *path, char *out, size_t cap)
{
    if (!path || !out || cap == 0) {
        return MUSE_VM_CONFIG_FAILED;
    }
    out[0] = '\0';

    char body[256];
    cJSON *req = cJSON_CreateObject();
    if (!req) {
        return MUSE_VM_CONFIG_FAILED;
    }
    cJSON_AddStringToObject(req, "path", path);
    cJSON_AddNumberToObject(req, "offset", 0);
    cJSON_AddNumberToObject(req, "len", MUSE_VM_CONFIG_MAX);
    bool printed = cJSON_PrintPreallocated(req, body, sizeof(body), false);
    cJSON_Delete(req);
    if (!printed) {
        return MUSE_VM_CONFIG_FAILED;
    }

    char *reply = malloc(REPLY_MAX);
    if (!reply) {
        return MUSE_VM_CONFIG_FAILED;
    }
    int status = muse_vm_config_post_raw("/fs/read", body, reply, REPLY_MAX);
    muse_vm_config_err_t rc = MUSE_VM_CONFIG_FAILED;

    if (status == 0) {
        rc = MUSE_VM_CONFIG_OFFLINE;
    } else if (status == MUSE_LINK_REQ_TOO_LARGE) {
        rc = MUSE_VM_CONFIG_TOO_BIG;
    } else if (status > 0) {
        cJSON *root = cJSON_Parse(reply);
        if (!root) {
            ESP_LOGW(TAG, "read %s: HTTP %d, reply not JSON", path, status);
        } else {
            const char *why = NULL;
            if (reply_is_error(root, &why) || status >= 400) {
                rc = says_missing(status, why) ? MUSE_VM_CONFIG_MISSING : MUSE_VM_CONFIG_FAILED;
                if (rc == MUSE_VM_CONFIG_FAILED) {
                    ESP_LOGW(TAG, "read %s: HTTP %d, %s", path, status, why ? why : "refused");
                }
            } else {
                /* The documented reply carries the content; the field name is
                 * the daemon's, so take the first of the ones it could be. */
                const cJSON *text = cJSON_GetObjectItem(root, "text");
                if (!cJSON_IsString(text)) {
                    text = cJSON_GetObjectItem(root, "content");
                }
                if (!cJSON_IsString(text)) {
                    text = cJSON_GetObjectItem(root, "data");
                }
                if (cJSON_IsString(text) && text->valuestring) {
                    size_t n = strlen(text->valuestring);
                    if (n >= cap) {
                        rc = MUSE_VM_CONFIG_TOO_BIG;
                    } else {
                        memcpy(out, text->valuestring, n + 1);
                        rc = MUSE_VM_CONFIG_OK;
                    }
                } else {
                    ESP_LOGW(TAG, "read %s: HTTP %d, no text in reply", path, status);
                }
            }
            cJSON_Delete(root);
        }
    }
    free(reply);
    return rc;
}

muse_vm_config_err_t muse_vm_config_write(const char *path, const char *text)
{
    if (!path || !text) {
        return MUSE_VM_CONFIG_FAILED;
    }
    size_t n = strlen(text);
    if (n > MUSE_VM_CONFIG_MAX) {
        return MUSE_VM_CONFIG_TOO_BIG;
    }

    cJSON *req = cJSON_CreateObject();
    if (!req) {
        return MUSE_VM_CONFIG_FAILED;
    }
    cJSON_AddStringToObject(req, "path", path);
    cJSON_AddStringToObject(req, "text", text);
    /* All three are required every time, says AGENTS.md. */
    cJSON_AddBoolToObject(req, "overwrite", true);
    cJSON_AddBoolToObject(req, "append", false);
    cJSON_AddBoolToObject(req, "create_parent", true);
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return MUSE_VM_CONFIG_FAILED;
    }

    char reply[512];
    int status = muse_vm_config_post_raw("/fs/write", body, reply, sizeof(reply));
    free(body);

    if (status == 0) {
        return MUSE_VM_CONFIG_OFFLINE;
    }
    if (status < 0) {
        return MUSE_VM_CONFIG_FAILED;
    }
    cJSON *root = cJSON_Parse(reply);
    const char *why = NULL;
    bool bad = status >= 400;
    if (root) {
        bad = bad || reply_is_error(root, &why);
        cJSON_Delete(root);
    }
    if (bad) {
        ESP_LOGW(TAG, "write %s: HTTP %d, %s", path, status, why ? why : "refused");
        return MUSE_VM_CONFIG_FAILED;
    }
    return MUSE_VM_CONFIG_OK;
}

static const char *err_name(muse_vm_config_err_t e)
{
    switch (e) {
    case MUSE_VM_CONFIG_OK: return "OK";
    case MUSE_VM_CONFIG_MISSING: return "MISSING";
    case MUSE_VM_CONFIG_OFFLINE: return "OFFLINE";
    case MUSE_VM_CONFIG_TOO_BIG: return "TOO_BIG";
    default: return "FAILED";
    }
}

#define SELFTEST_PATH "muse-gadget/selftest.json"
#define SELFTEST_DEFAULTS "{\"hello\":\"from the firmware\"}"

static void selftest_task(void *arg)
{
    (void)arg;
    printf("@vmcfg.begin ready=%d path=%s\n", muse_link_req_ready() ? 1 : 0, SELFTEST_PATH);

    /* The raw replies first: the daemon's field names are its own, and this
     * is the only way to find out what they are — or that it refuses us. The
     * three probes together say whether a refusal is about /fs or about
     * daemon requests altogether. */
    char *raw = malloc(REPLY_MAX);
    if (raw) {
        char body[192];
        snprintf(body, sizeof(body), "{\"path\":\"%s\",\"offset\":0,\"len\":%d}",
                 SELFTEST_PATH, MUSE_VM_CONFIG_MAX);
        int st = req_raw("POST", "/fs/read", body, raw, REPLY_MAX);
        printf("@vmcfg.probe route=/fs/read status=%d reply=%s\n", st, raw);

        snprintf(body, sizeof(body),
                 "{\"path\":\"%s\",\"text\":\"probe\",\"overwrite\":true,"
                 "\"append\":false,\"create_parent\":true}", SELFTEST_PATH);
        st = req_raw("POST", "/fs/write", body, raw, REPLY_MAX);
        printf("@vmcfg.probe route=/fs/write status=%d reply=%s\n", st, raw);

        /* Documented as reachable by a gadget's credentials too: if this one
         * works and /fs does not, the gap is the fs scope, not the surface. */
        st = req_raw("GET", "/chat/history?limit=1", NULL, raw, REPLY_MAX);
        printf("@vmcfg.probe route=/chat/history status=%d reply=%.200s\n", st, raw);
        free(raw);
    }

    char text[MUSE_VM_CONFIG_MAX];
    bool seeded = false;
    muse_vm_config_err_t rc = muse_vm_config_load(SELFTEST_PATH, SELFTEST_DEFAULTS,
                                                  text, sizeof(text), &seeded);
    printf("@vmcfg.load rc=%s seeded=%d text=%s\n", err_name(rc), seeded ? 1 : 0,
           rc == MUSE_VM_CONFIG_OK ? text : "");

    /* Read it back on its own. After a seed this is the half that proves the
     * write landed on the VM rather than only in RAM. */
    text[0] = '\0';
    muse_vm_config_err_t back = muse_vm_config_read(SELFTEST_PATH, text, sizeof(text));
    printf("@vmcfg.verify rc=%s text=%s\n", err_name(back),
           back == MUSE_VM_CONFIG_OK ? text : "");

    printf("@vmcfg.done\n");
    fflush(stdout);
    vTaskDelete(NULL);
}

void muse_vm_config_selftest(void)
{
    /* Its own task: the calls block on the network, and the console's must not. */
    if (xTaskCreate(selftest_task, "vmcfg", 5120, NULL, 4, NULL) != pdPASS) {
        printf("@vmcfg.done error=no_task\n");
        fflush(stdout);
    }
}

muse_vm_config_err_t muse_vm_config_load(const char *path, const char *defaults,
                                         char *out, size_t cap, bool *seeded)
{
    if (seeded) {
        *seeded = false;
    }
    muse_vm_config_err_t rc = muse_vm_config_read(path, out, cap);
    if (rc != MUSE_VM_CONFIG_MISSING || !defaults) {
        return rc;
    }

    /* Nothing on the VM yet. Put the firmware's defaults there, so Muse has
     * something to edit and the user has something to reset back to. A failed
     * seed is not a failed load: the caller still gets the defaults. */
    muse_vm_config_err_t wrote = muse_vm_config_write(path, defaults);
    if (wrote != MUSE_VM_CONFIG_OK) {
        ESP_LOGW(TAG, "could not seed %s; using defaults in RAM only", path);
    } else {
        ESP_LOGI(TAG, "seeded %s from the firmware's defaults", path);
        if (seeded) {
            *seeded = true;
        }
    }
    size_t n = strlen(defaults);
    if (n >= cap) {
        return MUSE_VM_CONFIG_TOO_BIG;
    }
    memcpy(out, defaults, n + 1);
    return MUSE_VM_CONFIG_OK;
}
