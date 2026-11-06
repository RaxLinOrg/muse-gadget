# Internet radio and podcasts

**Status:** mockup only
**Branch:** TBD — nothing built
**Last updated:** 2026-11-05

## Goal

A third page: internet radio and podcasts, both on one tile. This is
**route 2** of the four in [music.md](music.md), and picking it answers the
licensing question that blocked music — plain MP3 over HTTPS, no account, no
service to get permission from.

## What it looks like

Drawn at the real 466 x 466 in the firmware's palette, so these sit beside a
photo of the panel. Nothing is built; these are mockups.

![The list, both halves of the toggle](assets/radio-page.png)

![The two players](assets/radio-players.png)

## Decisions

Taken 2026-11-05, from the mockup
(<https://claude.ai/artifact/9iSrMEuRm4vByKvzvWne5W>).

![The two decisions, and what was not taken](assets/radio-decisions.png)

- **A toggle at the top, not one list and not two tiles.** RADIO and SHOWS
  swap the list under them. Five rows fit between the toggle and the page
  dots, so neither side scrolls — a combined list would scroll immediately.
  Two tiles would cost five dots and two of the four page slots.
- **The toggle does not persist.** `build()` leaves it on RADIO every time,
  so the page is the same page whenever you swipe to it and SHOWS is one tap
  away. Costs nothing and no NVS write, and it answers the one real objection
  to the layout — coming back to a page you left on the other half.
- **The station and feed URLs are compiled in.** A table beside the page.
  Changing one means a reflash, which for three or four stations is a fair
  trade, and it is the only option with nothing that can be unreachable at
  the moment you press play.
- **Live radio gets no scrub bar and no track skip.** The side buttons change
  *station*; the middle one is stop rather than pause, because a live stream
  cannot resume where you left it. A podcast is a fixed thing, so it gets
  position, duration, ±30 s and a resume point.

## Findings

- **The speaker picks the content.** The audio path is hardwired to 16 kHz
  mono (`MUSE_AUDIO_RATE`) into one small driver, so music sounds like
  speakerphone whatever feeds it — but talk radio and podcasts are exactly
  what 16 kHz mono is. This is the first audio feature whose content matches
  the speaker, which is a better argument for it than being the cheapest
  route — *(how: `muse_audio.h` and the board file, recorded in music.md)*.
- **Settings on the user's VM were tried first, and are blocked.** Rax's idea
  was to ask Muse to change the station list in conversation, which is the
  best fit of the three and the one the SDK is shaped for. The firmware was
  written and run: `POST /fs/read` and `POST /fs/write` both return **403
  `route not allowed for this credential's scopes`**, while `GET
  /chat/history` on the same session returns 200. So the transport works and
  those two routes specifically are refused. [vm-config.md](vm-config.md) has
  the measurement and the code is on `feat/vm-config` — *(how: a bench build
  on the board, over a live session)*.
- **Rebuffering is not an edge case.** A 16 kHz stream over a weak link
  stalls; this board logged `rssi -86` and three join attempts on an ordinary
  evening. The stalled state is drawn because it will be seen often: the
  station stays on screen and refills rather than dropping back to the list.
- **Podcasts cost much more than radio.** Radio is a URL. A podcast is an RSS
  feed to parse (XML on an ESP32), an episode list to hold, and a resume
  position per show in NVS. Same tile, but not the same size of job.

## Open questions

- **Radio first, podcasts second?** The toggle makes this natural: SHOWS can
  be an empty state until the RSS side exists. Not yet agreed.
- **Where does the page sit in the tile order?** It would be the second
  registered page, after the clock, making four dots.
- **ICY metadata.** Shoutcast and Icecast streams carry a now-playing title
  in-band. The "NOW PLAYING" line in the mockup assumes it; whether the
  chosen stations actually send it has not been checked.
- **Internal RAM is tight.** The heartbeat showed `min=4K` free internal heap.
  PSRAM is fine (2.7 MB free) and the stream buffer belongs there, but
  anything new in internal RAM needs watching.

## References

- [music.md](music.md) — the four routes; this is route 2
- [vm-config.md](vm-config.md) — why the list is compiled in rather than
  edited by Muse
- [voice.md](voice.md) — the MP3 decode path this reuses: HTTPS fetch on a
  worker task, cross-signed cert verification, minimp3, a stream buffer whose
  backpressure is already demonstrated. A radio stream never ends, which is
  the one real difference.
- `esp32/components/muse/muse_page.h` — `MUSE_PAGE`, the tile contract
- `esp32/components/muse_clock/muse_clock.c` — the one registered page today,
  and the pattern to follow
- `esp32/components/muse/muse_audio.h` — `MUSE_AUDIO_RATE`, the 16 kHz ceiling
