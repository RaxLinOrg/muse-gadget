# Music / now playing

**Status:** blocked
**Branch:** TBD
**Last updated:** 2026-11-01

## Goal

Play music on Jolly. The original shape was a Spotify page: what is playing,
and a playlist list to pick from.

## Mockup

Drawn at the real 466 x 466 in the firmware's own palette. Nothing is built.

![Now playing](assets/music-mockup.png)

![Playlist picker](assets/music-playlists.png)

## Findings

- **No major streaming service will license audio to a device like this.**
  Checked one at a time, and they fail for different reasons, which is why
  swapping services does not help:
  - **Spotify Web API** exposes playback *state* and *remote control* of a
    device already running a Spotify client. It hands out no audio stream.
    (Its 30-second preview URLs were withdrawn in the February 2026 changes.)
  - **Spotify eSDK**, which does stream, is a commercial program: you apply as
    a hardware company, sign an agreement, and ship certified hardware. Not
    available to an individual project.
  - **YouTube / YouTube Music** have no audio API at any tier. A Premium
    subscription changes nothing about this; there is no supported endpoint
    that returns an audio stream, and the tools that produce one work by
    scraping, against the terms of service.
  - **Apple Music** has MusicKit, which also only streams through Apple's own
    client on Apple platforms. There is no bare-metal path.
- **A paid subscription does not help.** Every one of these grants a *listening*
  right through the provider's own client. None grants a decoding right to
  third-party firmware, so holding Premium on both Spotify and YouTube does not
  unlock anything here.
- **The ESP32-S3 cannot be a Bluetooth speaker.** It has BLE only — no
  Bluetooth Classic radio, therefore no A2DP. So the fallback of "let the phone
  stream to the gadget" is closed on this chip, not merely unimplemented —
  *(how: the S3 datasheet; the ESP32 and ESP32-S3 differ here)*.
- The MP3 decode path proven in [voice.md](voice.md) works and is reusable for
  any audio that can be fetched over HTTPS. The decoder is not the problem;
  **where the bytes come from legally is the problem.**

## Blockers

- **Blocked on the source of audio, not on code.** Nothing can be built until
  a source exists that this device is allowed to fetch bytes from.

## Open questions

- **Which way out?** Three that are actually open:
  1. **Self-hosted library** — Jellyfin, Navidrome or any Subsonic server.
     Official HTTP APIs, real tokens, and the server transcodes to MP3, which
     drops straight into the existing decode path. This is the one that most
     resembles the original mockup, and it is the recommendation.
  2. **Internet radio / podcasts** — plain MP3 over HTTPS, no account at all.
     Smallest amount of work; no playlists of your own.
  3. **Remote control only** — keep the now-playing mockup, drive playback on
     the phone through the Spotify Web API, and let the phone make the sound.
     Legal and supported, but the gadget is then a display, not a speaker.
- If (1) or (3): the OAuth flow needs a browser and a redirect URI. Where does
  the token live on a device with no keyboard, and how is it refreshed?

## References

- The MP3 decode path proven in [voice.md](voice.md)
- `esp32/components/muse/muse_tts.c` — the HTTPS fetch and stream buffer it uses
