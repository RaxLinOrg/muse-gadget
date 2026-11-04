# Plans

One file per feature. Each file carries the plan, the mockup, what has been
found out, and whatever is currently blocking it — so picking a feature back up
after a week does not mean re-deriving it.

| File | Feature | Status | Where it is |
|---|---|---|---|
| [weather.md](weather.md) | Clock and weather page | working | on `main` |
| [voice.md](voice.md) | Spoken replies through a TTS endpoint | working | on `main`, one step left |
| [pet.md](pet.md) | Food, music and gift buttons | working | on `main` |
| [music.md](music.md) | Music on the gadget | blocked | nothing built |

## Conventions

- Copy [TEMPLATE.md](TEMPLATE.md) to `<feature>.md` and add a row above.
- **Status** is one of: `idea`, `mockup only`, `in progress`, `blocked`,
  `working`, `shipped upstream`, `abandoned`. Keep it honest — a plan that
  claims more than was verified is worse than no plan.
- **Every design decision gets a picture beside it.** Not a link to a mockup
  somewhere — a committed image. That means both the thing as built *and* the
  options that were looked at and rejected, because a sentence saying three
  layouts were considered is far less use later than the picture of them. If a
  decision was made in a session off a screenshot or a mockup, that screenshot
  or mockup belongs in this folder.
- Images go in `assets/`, named `<feature>-<what>.png` (`music-mockup.png`,
  `weather-nightmode.png`). Reference them relatively, e.g.
  `![Now playing](assets/music-mockup.png)`, which GitHub renders inline — and
  check it does by looking at the file on GitHub. An image that was never
  pushed renders as broken alt text, which is how this convention got written.
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
