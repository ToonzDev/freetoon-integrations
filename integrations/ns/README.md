# NS Vertrektijden for FreeToon

The next trains from your station on the Toon, from the NS Reisinformatie API.

- **Tile**: the next train you can still catch (`11:03`) with delay, train type,
  destination and track (`+2  IC Amsterdam Centraal - spoor 7`).
- **Screen** (tap the tile): the next train large (time, delay, destination,
  route, track; a changed track in red with the old one under it) and a board
  of the trains after it. Delays, track changes and cancellations are the only
  red on the screen. The gear opens the settings.

## API key

Create a free account on [apiportal.ns.nl](https://apiportal.ns.nl), subscribe
to the product **Ns-App** and copy its primary key into the app (tile > gear >
NS API-sleutel). Without a key the app shows sample departures (marked
*VOORBEELDGEGEVENS*) so you can see how it looks.

## Settings

`/mnt/data/integrations/ns/ns.conf`, edited on the Toon or by hand; the daemon
re-reads it within a second and fetches straight away.

```
api_key=
# a name ("Utrecht Centraal") or a code ("UT")
station=
# filled in by the app after the lookup
station_code=
station_name=
# only trains to or via these places, comma-separated (empty = all)
direction=Amsterdam, Schiphol
# minutes to walk to the station: trains you can't catch are hidden
walk_min=0
max=8
poll=60
# 1 = sample data even with a key
mock=0
# options, all off by default
# tile shows when to leave (departure minus walk_min) instead of the departure
tile_leave=0
# Inbox warning when this train (scheduled time) is cancelled or warn_delay+ min late
my_train=07:42
warn_delay=5
# the next train as an Inbox message in this window
morning=06:30-09:00
```

**Options** (settings > *Opties*): the tile can show when to leave instead of
when the train goes; a warning in the Inbox (envelope on the home screen) when
your usual train is cancelled or late; and in a morning window the next train
as an Inbox message ("Vertrek over 6 min: 07:42 IC naar Amsterdam Centraal,
spoor 5"), refreshed every minute and cleared after the window. FreeToon's dim
screen does not show Inbox messages yet, so these appear on the home screen.

A station name is looked up once with
`/reisinformatie-api/api/v2/stations?q=...`; departures come from
`/reisinformatie-api/api/v2/departures?station=<code>` every 60 s (and when
the screen opens). That is about 1,500 requests a day, well inside the free
quota. The file survives package updates (not an uninstall); the key stays on
the Toon.

## Files

| File | What |
|---|---|
| `ns` | launcher (manifest `binary`), runs `ns.toon2` or `ns.toon1` |
| `ns_daemon.c` | daemon: fetch, filter, tile, `state.json`, sample data |
| `screen_ns.c` | UI plugin `ns_ui.<toon>.so` |
| `ftapp.h`, `ftjson.h`, `ftui.h` | small shared helpers (same copy in the other apps of this set) |

Live state: `/var/volatile/tmp/ns/state.json` (tmpfs). Log:
`/var/volatile/tmp/integration-ns.log`.

## Build

```sh
make TARGET=toon2 FREETOON_SRC=../../../freetoon-lvgl
make TARGET=toon1 FREETOON_SRC=../../../freetoon-lvgl
make package
```

Install without the Marketplace: `scripts/install-local.sh` in this repo.
