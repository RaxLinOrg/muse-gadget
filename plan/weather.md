# Clock and weather

**Status:** mockup only
**Branch:** `feat/clock-weather`

## Goal

An idle page showing the time and current conditions.

## Mockup

Drawn at the real 466 x 466 in the firmware's own palette, with annotations on
data sources and costs — see the published mockup. Save it here as
`assets/weather-mockup.png`.

## Open questions

- Which weather API, and does it need a key? A keyless endpoint avoids the
  question of where a secret lives on the device.
- Where does the clock get its time, and does it survive sleep?
- How often to refresh, given this page is the idle view.

## Findings

- Any HTTPS host needs
  `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE_CROSS_SIGNED_VERIFY=y` if its chain is
  cross-signed — see [voice.md](voice.md). Check this first if a fetch fails
  before suspecting the code.

## References

- `esp32/components/muse/muse_ui.c`
