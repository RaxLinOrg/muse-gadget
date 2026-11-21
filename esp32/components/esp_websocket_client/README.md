# esp_websocket_client (forked)

A copy of `espressif/esp_websocket_client` **1.2.3**
(esp-protocols commit `1750a3fda9c99a9545379f89c6bb49a86615cfaa`), vendored here
so that its task stack can come from PSRAM. Being a local component, it
overrides the managed dependency declared in `main/idf_component.yml` and
`components/muse_radar/idf_component.yml`.

## Why

Upstream starts the client with `xTaskCreate()`, and ESP-IDF only ever takes a
*dynamic* task stack from internal RAM — `CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM`
applies to `xTaskCreateStatic()` only. The AIS feed behind the radar page's
Ships mode starts late in bring-up, by which time the largest free contiguous
internal block on this board is around 4 KB, so the 6 KB stack can never be had
and the client never opens. Measured, not guessed: see `plan/radar.md`.

## The change

Every edit is marked `muse fork` in `esp_websocket_client.c`; there are no other
differences from upstream. One 8 KB PSRAM stack and one internal `StaticTask_t`
are kept in file scope and lent to a single client at a time via
`xTaskCreateStatic()`. A second concurrent client falls back to upstream's
`xTaskCreate()`.

Because the idle task does not free a static task's buffers, the static task
must not delete itself. It parks on `vTaskSuspend()` after setting
`STOPPED_BIT`, and `ws_slot_reap()` deletes it from the task that asked it to
stop — always another task, since `stop()` and `close()` refuse to run on the
websocket task. Deleting another task frees its TCB synchronously, so the slot
is free as soon as `ws_slot_reap()` returns. The one path where the task does
delete itself, `esp_websocket_client_destroy_on_exit()`, retires the slot
instead of reusing it.

## Updating

Re-copy the upstream version, then re-apply the `muse fork` blocks. Diffing
against the component-manager cache
(`~/Library/Caches/Espressif/ComponentManager/.../espressif__esp_websocket_client_*`)
shows exactly what the fork adds.
