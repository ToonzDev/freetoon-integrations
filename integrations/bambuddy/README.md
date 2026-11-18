# 3D-printer (BamBuddy) for FreeToon

Shows the status of a Bambu Lab printer managed by
[BamBuddy](https://github.com/maziggy/bambuddy) on the Toon.

- **Tile**: progress (`63%`), time left and file name; `Klaar` / `Pauze` /
  `Vrij` / `Offline` otherwise. Options (off by default): an Inbox
  notification when a print finishes or fails ("3D-print klaar: ...", cleared
  when the next print starts), and the finish time ("klaar om 16:42") on the
  tile instead of the time left.
- **Screen** (tap the tile): plate thumbnail, file, progress bar, time left and
  finish time, layers, nozzle (both nozzles on an H2D/H2C), bed and chamber
  temperature, AMS spools with colour, type and what is left. The gear opens
  the settings.

## Two ways to get the data

| | When | Set |
|---|---|---|
| **pull** | the Toon can reach BamBuddy | `url` (e.g. `http://192.168.1.20:8000`) and `api_key` |
| **push** | the Toon can't reach BamBuddy (Toon in an IoT VLAN, BamBuddy on the LAN) but something next to BamBuddy can reach the Toon | leave `url` empty; run `relay/bambuddy-relay.sh` next to BamBuddy |

Pull asks `GET /api/v1/printers/<id>/status` every 15 s (header `X-API-Key`)
and fetches the thumbnail from the status' `cover_url` once per print.

Push: the daemon listens on `push_port` (e.g. **8766**) for `POST /status` (the JSON
BamBuddy's status endpoint returns) and `POST /cover` (the PNG). Its reply to
`/status` says `{"need_cover":true}` when it has no thumbnail for the current
print. Push is off until you set `push_port` (e.g. 8766) **and** a
`push_token` of at least 8 characters; the relay sends the same secret as
`TOON_TOKEN` (header `X-Toon-Token`), and pushes without it are refused.

```sh
BAMBUDDY_URL=http://127.0.0.1:8000 BAMBUDDY_KEY=... TOON=192.168.1.50 TOON_TOKEN=... \
  sh relay/bambuddy-relay.sh
```

The relay needs only `sh` and `curl` (an `alpine` container with
`apk add curl` is enough).

## Settings

`/mnt/data/integrations/bambuddy/bambuddy.conf`, edited on the Toon (tile >
gear) or by hand; the daemon re-reads it within a second. It survives package
updates (not an uninstall).

```
url=
api_key=
# 0 = first printer
printer_id=0
poll=15
# push: port (0 = off), shared secret, the relay's address (empty = any)
push_port=0
push_token=
push_from=
# options, 1 = on (all off by default)
notify=0
tile_eta=0
```

Comments go on their own line. The stock Toon firewall (`HCB-INPUT`) drops
every inbound TCP port except 22, 80, 5900 and 10081, so while push is on the
daemon inserts one runtime rule for `push_port` (only from `push_from` when
set) and removes it when push is switched off or the daemon stops. It is not
persistent; the daemon adds it again after a reboot.

A read-only BamBuddy API key is enough.

## Files

| File | What |
|---|---|
| `bambuddy` | launcher (manifest `binary`), runs `bambuddy.toon2` or `bambuddy.toon1` |
| `bambuddy_daemon.c` | daemon: pull / push, tile + alert over BoxTalk and toonui's `:10081` API |
| `screen_bambuddy.c` | UI plugin `bambuddy_ui.<toon>.so` |
| `ftapp.h`, `ftjson.h`, `ftui.h` | small shared helpers (same copy in the other apps of this set) |
| `relay/bambuddy-relay.sh` | the push relay |

Live state: `/var/volatile/tmp/bambuddy/state.json` and `cover.png` (tmpfs, so
polling does not wear the flash). Log: `/var/volatile/tmp/integration-bambuddy.log`.

## Build

```sh
make TARGET=toon2 FREETOON_SRC=../../../freetoon-lvgl
make TARGET=toon1 FREETOON_SRC=../../../freetoon-lvgl
make package
```

Cross builds use the Linaro (Toon 2) and Bootlin (Toon 1) toolchains in
`~/toolchains`, e.g. in the `freetoon-build` container. The plugin only uses
`THEME_*` colours and declares the settings fields it reads
(`FT_PLUGIN_DECLARE_ABI`).

Install without the Marketplace: `scripts/install-local.sh` in this repo.
