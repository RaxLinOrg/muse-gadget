# Plans

One file per feature. Each file carries the plan, the mockup, what has been
found out, and whatever is currently blocking it — so picking a feature back up
after a week does not mean re-deriving it.

| File | Feature | Status | Where it is |
|---|---|---|---|
| [weather.md](weather.md) | Clock and weather page | working | on `main` |
| [voice.md](voice.md) | Spoken replies through a TTS endpoint | working | on `main`, one step left |
| [pet.md](pet.md) | Food, music and gift buttons | in progress | `feat/digital-pet` |
| [music.md](music.md) | Music on the gadget | blocked | nothing built |

## Conventions

- Copy [TEMPLATE.md](TEMPLATE.md) to `<feature>.md` and add a row above.
- **Status** is one of: `idea`, `mockup only`, `in progress`, `blocked`,
  `working`, `shipped upstream`, `abandoned`. Keep it honest — a plan that
  claims more than was verified is worse than no plan.
- Screenshots and mockups go in `assets/`, named `<feature>-<what>.png`
  (`music-mockup.png`, `weather-nightmode.png`). Reference them relatively,
  e.g. `![Now playing](assets/music-mockup.png)`, which GitHub renders inline.
- The device screen is **466 x 466** round AMOLED. Mockups drawn at that exact
  size are directly comparable to a photo of the panel.
- Real screens beat mockups once a feature exists. Build with
  `MUSE_BENCH=1 tools/muse/board.sh build|flash s3`, then
  `tools/muse/snap.py <port> '>ui=1' out.png 45`. It saves at 3x, so
  `sips -z 466 466` it back down before committing. Give `snap.py` the reset
  wait on the first call only — it resets the board, which throws away
  whatever an earlier call navigated to.
- Record findings as **what was measured**, not what was reasoned. On this board
  reading the source has repeatedly produced confident wrong theories; the
  device's own log line settles things in minutes.
