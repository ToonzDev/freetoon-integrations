# Radiatoren for FreeToon (0.3.1)

Rooms with their own comfort and eco temperature and a weekly schedule of time
blocks, for a Toon 1 or Toon 2 running FreeToon (LVGL). Port of the QML Radiatoren app
(0.2.0, ToonStore), which does not show under FreeToon.

Radiator valves are linked in **Home Assistant**: every `climate.*` in the Home
Assistant area with the same name as a room follows that room's target
(example package: `homeassistant/radiatoren.yaml`; replace `TOON_IP`).

## Pieces

| File | What |
|---|---|
| `radiatoren` | launcher named in `manifest.json` (`binary`); runs `radiatoren.toon2` |
| `radiatoren.toon2` | daemon, respawned by init |
| `radiatoren_ui.toon2.so` | UI plugin; toonui `dlopen()`s it when the tile is tapped (`screen_create`) |
| `manifest.json` | id `radiatoren`, BoxTalk service `radiatoren`, tile fields `value` / `subtitle` |
| `radiatoren_logic.[ch]` | the room logic (C port of `RadiatorenLogic.js`), shared by both |
| `homeassistant/radiatoren.yaml` | example Home Assistant package (replace `TOON_IP`) |

### Data

- **Settings**: `/mnt/data/tsc/radiatoren.userSettings.json`, the same file and
  format (`nl.toon.radiatoren/settings-v1`) as the QML app, so existing rooms
  carry over and the file survives uninstalling the integration (the
  Marketplace removes `/mnt/data/integrations/radiatoren/` completely). Only the
  UI plugin writes it, atomically (tmp + fsync + rename).
- **For Home Assistant**: `/qmf/www/tsc/radiatoren.json`, served by lighttpd as
  `http://<toon>/tsc/radiatoren.json`. Unchanged contract
  `nl.toon.radiatoren/targets-v1`: `updated` (unix s), `hysteresis`,
  `rooms[{id,name,target,mode,reason}]`. Written atomically by the daemon on
  every settings change (seen within 1 s), on every schedule edge, and at least
  every 30 s.
- **Tile**: the daemon notifies BoxTalk service `radiatoren` with `<value>` (the
  first room's target, e.g. `20,5°`) and `<subtitle>` (e.g. `Woonkamer, 2/4 op
  comfort`), posts the same to toonui's `:10081/api/v1/marketplace/apps/radiatoren/state`
  and writes `/mnt/data/integrations/radiatoren/state.json` (read by toonui at boot).
  It also keeps its stable BoxTalk uuid in `.../radiatoren/uuid`.
- Log: `/var/volatile/tmp/integration-radiatoren.log`.

## Install

**Settings > Marketplace > Radiatoren > Install**, then bind the tile:
**Settings > Tiles** (or long-press one of the right-hand tiles) and choose
*Radiatoren*. Tapping the tile opens the app. Rooms from the older QML
Radiatoren app (ToonStore) carry over automatically.

## Build

With the toolchains from `freetoon-lvgl/scripts/setup-toolchains.sh` and
`freetoon-lvgl` checked out next to this repo (`FREETOON_SRC`):

```sh
make TARGET=toon2 && make TARGET=toon2 abi-check
make TARGET=toon1 && make TARGET=toon1 abi-check
make test        # host unit test of the room logic (cc/clang)
```

## Notes

- The plugin declares the settings fields it reads (`language`, `home_light_mode`)
  with `FT_PLUGIN_DECLARE_ABI`; toonui refuses it if those moved. 0.3.1 is built
  against the settings layout of FreeToon 1.0.12 (upstream commit 91be3c9 inserted
  `dim_elec_max_kwh` before `home_light_mode`, so toonui 1.0.12+ refused the 0.3.0
  plugin); hence `min_toonui` 1.0.12. Build against a FreeToon tree of 1.0.12 or later. The ABI check
  compares LVGL major/minor only: built against LVGL 8.3.11 headers.
- UI text is Dutch with English through FreeToon's `tr()` (Settings > language).
- Times use the Toon's local time (`localtime_r`, `/etc/localtime`).
