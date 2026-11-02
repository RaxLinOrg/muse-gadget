# Spoken replies

**Status:** working
**Branch:** `feat/reply-by-voice` — merged to `main`; step 4 below is still open
**Last updated:** 2026-11-01

## Goal

The gadget reads its replies aloud instead of only printing them, in English
and Traditional Chinese, with a setting to turn it off.

## Plan

1. ~~Verify the audio output chain works at all.~~
2. ~~Find out whether Muse sends any audio to a gadget.~~
3. ~~Fetch our own TTS audio and feed it into the existing MP3 decode path.~~
4. Add a user-visible "spoken replies" setting (NVS + a `Settings > Sound` row
   + a `>speak=` console key, following the existing `speaker` pattern).

## Findings

- **Muse sends the gadget no audio at all.** The chat stream's `resources`,
  `presentations`, `widgets`, `actions` and `transcript` fields are empty on
  every event — *(how: instrumented dump of every stream event)*. The "MP3
  attachment" Muse.ai support describes never reaches a gadget.
- **The silence is deliberate.** `start_tts()` in
  `esp32/components/muse/muse_chat_session.cpp` feeds silence whose length is
  derived from the reply's character count (`m.len * MIC_RATE /
  TEXT_CHARS_PER_S`) purely to pace the captions — *(how: read the source, then
  confirmed by the log line it emits)*. So "reply audio after Ns" and "N s of
  audio" appear in the log while the device is silent. Not a playback bug.
- **The output chain was never broken.** Sending a bare `m` to the serial
  console plays a built-in MP3 through the real decoder — *(how: heard it)*.
  Use `m` as the regression check before suspecting playback.
- `muse_audio_write()`'s return value is discarded in `muse_voice.c`, so
  "N s of audio" logs appear even in total silence. The level meter is computed
  before the write, so an animating meter proves nothing either.
- **The blocker was TLS, not memory.** Attaching the cert bundle is not enough
  for Google hosts: they serve a chain ending in a root the bundle trusts but
  cross-signed by one it does not, so
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y` is required
  (~700 bytes of handshake heap). Without it `esp_http_client_open` just fails.
  Worth remembering for any new HTTPS host on ESP-IDF.
- Verified end to end: English 1 segment / 8064 B / 1.01 s; Traditional Chinese
  498 chars / 4 segments / 441792 B / 55.22 s with no "MP3 buffer full".
- Backpressure is demonstrated but the margin is thin: a 562-char Chinese reply
  fetched 535104 B against a 512 KB buffer with no overflow warning (1.02x), so
  the buffer did drain into the decoder while the fetch held off. Not a stress
  test.

## Blockers

- Voice notes over ~12 s (>500 KB) consistently fail to upload, and **one
  failed note blocks every later note in the saved-note queue until the device
  is rebooted** (logs "sending a saved note: ...s, from ...s ago, try 5").
  Unrelated to TTS; keep spoken questions short. Reboot clears it.
- The Google Translate endpoint is keyless and undocumented — fine for
  prototyping, not for anything shipped. Azure gives better zh-TW quality.

## References

- `esp32/components/muse/muse_tts.c` — worker task, HTTPS fetch, stream buffer
- `esp32/components/muse/muse_chat_session.cpp` — `start_tts()`, `pump_tts()`
- `esp32/devices/sdkconfig.muse` — the cross-signed cert option
- Upstream PR for the cert fix: facebookincubator/muse-gadget-sdk#258
