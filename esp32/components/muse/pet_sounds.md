# The pet buttons' sounds

Three clips, embedded in the image by `EMBED_FILES` in `CMakeLists.txt` and
played by `muse_pet_sound.c`.

| File | Button | Source | Licence |
|---|---|---|---|
| `pet_food.mp3` | Food | *Eating Chips* by 49603404 | Pixabay Content License |
| `pet_dance.mp3` | Music | *Ringtone 027* by Universfield | Pixabay Content License |
| `pet_thanks.mp3` | Gift | *Thank you.* by kittenstrike1 | Pixabay Content License |

The Pixabay Content License covers use in a product and asks for no
attribution. These credits are here because the authors deserve them, not
because the licence demands it.

## How they were cut

Each one was trimmed, levelled to about −18 LUFS so none of them jumps out
against a spoken reply, and encoded **16 kHz mono at 32 kbps**. 30 KB in all.

```
# food: drop 0.4 s of lead silence, take 2 s, compress so the crunches carry
ffmpeg -ss 0.40 -t 2.0 -i src.mp3 \
  -af "afade=t=out:st=1.9:d=0.1,acompressor=threshold=-30dB:ratio=4:attack=5:release=120" \
  -ac 1 -ar 16000 -b:a 32k pet_food.mp3

# music: the first 2 s, played twice
ffmpeg -i src.mp3 -filter_complex \
  "[0:a]atrim=0:2,asetpts=N/SR/TB,afade=t=out:st=1.94:d=0.06[a];[a]asplit=2[a1][a2];[a1][a2]concat=n=2:v=0:a=1" \
  -ac 1 -ar 16000 -b:a 32k pet_dance.mp3
```

## Why 16 kHz

`MUSE_AUDIO_RATE` is 16000, so a clip at that rate needs no resampling and
`muse_pet_sound.c` carries no resampler. The decoder checks the rate of every
frame and refuses a clip that is not 16 kHz, because the alternative — playing
it at the wrong pitch — is the kind of wrong nobody notices for a week.

**Re-encode at 16 kHz mono or the clip will not play.** The log says which one
and what rate it found.

## Lengths

The reaction lasts as long as its clip: `react_secs()` in `muse_state.c` holds
2.3 s for food, 4.3 s for music and 1.8 s for the gift. Change a clip's length
and change that number with it, or the motion and the sound stop at different
times.
