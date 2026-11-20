# A frozen screen that leaves something behind

**Status:** working
**Branch:** `feat/ui-hang-diagnosis`
**Last updated:** 2026-11-19

## Goal

When the screen freezes, be able to find out why. Today it cannot be done at
all, and that is not an oversight in one place but three of them lining up.

## What happened

Rax's gadget froze on 2026-11-19 — the screen stopped and swiping did nothing.
It was plugged into the laptop to look at. **Opening the serial port reset the
board**, and the state was gone before a single byte could be read:

```
@power {"boot":"usb","uptime":2,...}
```

`ESP_RST_USB`. That is this hardware, not a mistake in the capture: the comment
in `muse_battery.c` already says *"Plugging back in resets some boards once
their port opens."*

Nothing had been kept, and nothing could have been:

- **`CONFIG_ESP_INT_WDT=n` and `CONFIG_ESP_TASK_WDT_EN=n`** — both off, so a
  wedged task hangs forever rather than resetting. `sdkconfig.defaults` gives
  the reason: PSRAM cache stalls can outlast the int-WDT's 300 ms window.
- **No coredump partition**, so a panic stored nothing either.
- **The RTC record that does survive a reset is rewritten at the start of every
  boot**, and by then there had been three.

A freeze was, by construction, undiagnosable after the fact.

## The task watchdog would not have caught it

Worth stating plainly, because it is the obvious first answer and it is wrong.
The reason given for disabling the watchdogs is about the **interrupt**
watchdog, whose window is 300 ms; the **task** watchdog is a different
mechanism with a 5 s default, which a cache stall will not trip. The two look
conflated.

But turning it back on still would not have caught this one. The task watchdog
watches the **idle** tasks, and a UI task blocked on a mutex leaves idle running
perfectly happily. It catches a task that spins, not a task that waits — and
waiting is what a deadlocked screen does.

## What was built

Three pieces, each useless without the others.

1. **A coredump partition**, 64 KB, in `partitions_muse.csv` (`0x822000`) and
   `partitions_muse_8mb.csv` (`0x7E2000`). Both sit after the app slots and the
   product data, so nothing above them moves and no existing board's layout
   shifts. There was room to spare: the 16 MB table ends at 8.13 MB.
2. **`CONFIG_MUSE_UI_WATCHDOG`** (`muse_ui_watchdog.c`), which watches the one
   thing that actually stops — the LVGL frame timer. `frame_tick()` bumps a
   counter; a priority-1 task checks it every second and panics after
   `CONFIG_MUSE_UI_WATCHDOG_S` seconds without movement. Default off; on for
   `muse-waveshare-s3-175c` at 10 s.
3. **`>ui.hang=<secs>`**, a bench-only console verb that blocks the UI task, so
   the watchdog can be proved to fire without waiting for a real freeze.

The watchdog holds off while LVGL is paused on purpose — the screen asleep on
battery, or the camera view open. A board that reboots itself every time the
screen sleeps would not be a diagnosis, it would be a new bug.

## Findings

![It fires, and it names the task that was stuck](assets/ui-watchdog.png)

- **The whole chain works end to end** — *(how: `>ui.hang=20` on the board with
  the limit at 10 s)*. The watchdog fired at exactly 10 s, panicked, wrote the
  dump, rebooted, and the next boot found the partition and verified the
  checksum. Back up in under two seconds.
- **The dump names the stuck task and the line it stopped on** — *(how:
  `idf.py coredump-info` over USB)*. 25 tasks, every one with a stack. The
  crashed task is `ui_wdt`; thread 12 is `lvgl`, stopped at
  `frame_tick ... muse_ui.c:2140`. In a real deadlock that frame is whatever
  the UI task was waiting on.
- **Reading the dump back needs `--parttable-off 0x10000`** — *(how: the first
  attempt failed)*. This project moved the partition table off the IDF default
  at `0x8000` (`sdkconfig.defaults` says why: a Secure Boot V2 bootloader grows
  past the `0x6000` ceiling). `esp_coredump` does not know that and reads the
  wrong place. `idf.py coredump-info` has no such flag, so with the dump still
  in flash it is the `esp_coredump` module directly:

  ```
  python -m esp_coredump --chip esp32s3 --port /dev/cu.usbmodem1101 \
      info_corefile --parttable-off 0x10000 --save-core core.elf \
      build-muse-waveshare-s3-175c/muse-gadget.elf
  ```

  and then `idf.py -B <build> coredump-info --core core.elf` for the symbolised
  output, which is where the task names come from.
- **Flash cost is about 66 KB** — *(how: measured)*. The bench image went from
  3,739,648 to 3,805,184 bytes, 3.63 MB of the 4 MB slot.

## Still open

- **The original freeze is still unexplained**, and this does not explain it —
  it only means the next one will explain itself. There are around thirty
  unbounded `xSemaphoreTake(..., portMAX_DELAY)` waits across the `muse`
  components; any one held by a stuck task, while the UI task waits for the
  LVGL lock, gives exactly the symptom.
- **The radio and podcast pages were checked and cleared** — *(how: read)*.
  `teardown()` is bounded to 40 × 25 ms, `reap()` only deletes workers that
  have already signalled done, and `muse_radio` has no `portMAX_DELAY` at all.
  That page can stall the screen for about a second; it cannot freeze it.
- **The watchdog is off for every other board.** It panics, which is a
  behaviour change, so the boards it has not been tried on do not get it by
  default.
- **Nothing reads the dump on its own.** It waits in flash until someone asks.
  Posting it to Hatch on the next boot would be the obvious next step.

## References

- Files: `esp32/components/muse/muse_ui_watchdog.{c,h}`,
  `esp32/components/muse/muse_ui.c` (`frame_tick`),
  `esp32/components/muse/muse_input.c` (`>ui.hang=`),
  `esp32/partitions_muse.csv`, `esp32/partitions_muse_8mb.csv`,
  `esp32/devices/sdkconfig.muse`,
  `esp32/devices/sdkconfig.muse-waveshare-s3-175c`
