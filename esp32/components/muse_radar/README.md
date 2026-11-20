# Radar page

A swipe page showing what is moving around a place you pick: aircraft
overhead, ships in a strait, or where the rain is. Drop the folder in and it
appears between the character and settings — no edits to the firmware's own
files.

## What it shows

The scope is drawn over a real coastline, with range rings, a bearing scale
in degrees and the nearest contact ringed and named along the bottom.

- **Planes** — ADS-B from [adsb.lol](https://api.adsb.lol), no key. Blips are
  triangles pointing along the track, coloured by altitude: white below
  5,000 ft, bright green to 20,000 ft, dim green above.
- **Ships** — AIS from [aisstream.io](https://aisstream.io), which wants a
  free key. Contacts carry a ten-minute vector.
- **Weather** — a [RainViewer](https://www.rainviewer.com) radar tile,
  decoded, recoloured to the usual green/yellow/red and reprojected into the
  scope.

## Settings

The page adds one **Radar** row to settings. Opening it gives three choices:

- **Range** cycles `12 NM`, `40 NM`, `250 NM`, `Off`. Off is a real off: the
  page's `enabled()` answers false, so it leaves the tileview and its worker
  never fetches anything.
- **Show** cycles Planes, Ships, Weather.
- **Place** is a list: Bay Area, Los Angeles, New York, Texas, Hormuz, Taiwan
  Strait, Japan, Korea, Malacca, Panama.

All three are kept in this component's own NVS namespace (`muse_radar`).

Ships need a key, from `idf.py menuconfig` under **Muse radar page**:

```
CONFIG_MUSE_RADAR_AIS_KEY="..."
```

Sign in with GitHub at <https://aisstream.io/account> to make one. Without it
the Ships mode says `NO AIS KEY` and fetches nothing; planes and weather need
no key at all.

`CONFIG_MUSE_RADAR_ENABLED=n` leaves the page out of the build entirely.

## How often it fetches

Only while the page is on screen, and once more the moment it comes into
view. That is what keeps adsb.lol's 500-requests-a-day quota irrelevant: at a
30-second refresh it buys about four hours of watching a day.

The period follows the range, because a blip has to move a sensible number of
pixels between frames rather than jumping the width of the scope:

| Range | Crossing time at 450 kt | Refresh |
|---|---|---|
| 12 NM | 3.2 min | 30 s |
| 40 NM | 10.7 min | 60 s |
| 250 NM | 67 min | 5 min |

Weather ignores all of that and uses ten minutes at every range, because
RainViewer publishes a frame every 600 seconds exactly. Ships ignore it too:
AIS is a stream, and positions arrive when vessels transmit them.

## Things worth knowing before you point it somewhere

- **Planes come from volunteers.** ADS-B aggregators are fed by hobbyists with
  receivers, and East Asia has roughly a tenth of the US feeder density.
  Japan, Korea and the Taiwan Strait each returned about 33 aircraft inside
  250 nm against the Bay Area's 312, and the same ratio showed up on an
  unrelated network. The page is showing what is being *received*, not what is
  flying.
- **Panama has almost no air traffic** — five aircraft inside 250 nm. It is on
  the list for ships.
- **Big metros overflow.** New York inside 250 nm was 1,199 aircraft in a
  615 KB body. The parser streams the response and keeps only the nearest 80,
  so this costs the same memory as a quiet place; the card says how many there
  really were.
- **RainViewer caps at zoom 7.** Zoom 8 answers with a "Zoom Level Not
  Supported" image, so the 12 NM range shows a magnified tile rather than more
  detail. Their API is personal and educational use only.

## How it is put together

| File | What it does |
|---|---|
| `muse_radar_page.c` | The page, its settings, and the scope rasteriser |
| `muse_radar_net.c` | Settings in NVS, the worker task, the plane feed |
| `muse_radar_rain.c` | RainViewer tiles: fetch, decode, recolour, reproject |
| `muse_radar_ais.c` | The AIS websocket and its ship table |
| `muse_radar_places.h` | Generated: places and their coastlines |

The scope is rasterised into an RGB565 image rather than drawn with LVGL
widgets — a sweep is a few hundred lines and triangles, and one image costs
one object instead of eighty. Only the text is a real label.

Coastlines are baked in at build time from Natural Earth's 50m data, as
milli-degree offsets from each place's centre: 0.06 nm a step, under a pixel
even on the 12 NM range, and 15 KB of flash for all ten places. To change the
list, edit `PLACES` in `tools/muse/make_places.py` and run it:

```
curl -LO https://raw.githubusercontent.com/nvkelso/natural-earth-vector/master/geojson/ne_50m_coastline.geojson
tools/muse/make_places.py ne_50m_coastline.geojson components/muse_radar/muse_radar_places.h
```

As with `muse_clock`, nothing in the firmware references this component, so
`idf_component_register(... WHOLE_ARCHIVE)` is what keeps the `MUSE_PAGE`
constructor from being dropped at link time.
