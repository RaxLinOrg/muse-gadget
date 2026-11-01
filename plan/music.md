# Music / now playing

**Status:** design decision open
**Branch:** TBD

## Goal

A page showing what is playing, and a playlist list to pick from.

## Mockup

Drawn at the real 466 x 466 in the firmware's own palette — see the published
mockup. Save it here as `assets/music-mockup.png` so this file stands alone.

## Findings

- **The Spotify Web API cannot stream audio.** It exposes playback *state* and
  *remote control* of a device that already has the Spotify client running; it
  does not hand out an audio stream a gadget could decode. This rules out the
  obvious reading of the plan.

## Open questions

- **Which shape?** Either (a) the gadget becomes a remote control and a
  now-playing display for playback happening on the phone, which the Web API
  does support, or (b) audio comes from somewhere other than Spotify. Decide
  before writing code — the two share almost nothing.
- Auth: the Spotify OAuth flow needs a browser and a redirect URI. Where does
  the token live, and how does it get refreshed on a device with no keyboard?

## Blockers

- Blocked on the decision above.

## References

- The MP3 decode path proven in [voice.md](voice.md) is reusable for any audio
  source that can be fetched over HTTPS.
