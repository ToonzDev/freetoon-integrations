# Matter Bridge for FreeToon (1.0.0)

The pairing screen of the Matter Bridge, for a Toon running FreeToon (LVGL).
The Matter Bridge ToonStore app (ToonSoftwareCollective/matterbridge) shares
the Toon thermostat and energy meter with Apple Home, Google Home and other
Matter controllers through a small Node.js service. Its screen was a QML app,
which FreeToon does not show, so a FreeToon Toon had no way to pair it. This
integration is that screen, plus a watchdog for the service.

- **Tile**: `Koppel` with the pairing code while the bridge waits for a
  controller, `Aan` with the number of shared devices once paired, `Uit` /
  `...` when the service is stopped or starting.
- **Screen**: the QR code (drawn by LVGL from the bridge's own payload) and the
  manual pairing code; service and pairing state with the bridge's message or
  the last error from its log; a switch per device to share; **Restart
  service** and **Remove pairing** (commissioning reset), each with a
  confirmation.

## Requirements

The Matter Bridge service must be installed (`/mnt/data/tsc/matter-service`,
`toon-matter-service.sh`), which in practice means a **Toon 2**: the service is
Node.js 22 for ARMv7 and needs ~110 MB of RAM. This package does not ship
Node.js (about 45 MB of runtime and Matter libraries) and does not install the
bridge; without it the screen says so. The Toon 1 build exists so the tile and
screen behave, but a Toon 1 cannot run the bridge.

## How it works

| Piece | What |
|---|---|
| `freetoon-app-matter` | launcher named in `manifest.json`; runs the `.toon2` / `.toon1` daemon |
| `freetoon-app-matter.toon2` | daemon, respawned by init (inittab row `ibf7`) |
| `matter_ui.toon2.so` | UI plugin; toonui `dlopen()`s it when the tile is tapped |
| `matter_logic.[ch]`, `matter_json.[ch]` | shared by both: reading the bridge's files, writing the device choice |

The integration only uses the bridge's existing files, so the QML app and this
screen can be used side by side:

- **Status** `/var/volatile/tmp/toon-matter-bridge/status.json`
  (`nl.toon.matter-bridge/status-v1`, written by the bridge): running,
  commissioned, message, pairing code, QR payload, energy.
- **Device choice** `/mnt/data/tsc/matter-bridge.json`
  (`nl.toon.matter-bridge/preferences-v1`, the QML app's file and format).
  The screen writes it atomically; the bridge applies it within 5 s, no
  restart. Shareable: the Toon thermostat, the Toon energy meter (when the
  bridge sees a meter) and ComfortHub climate devices
  (`/var/volatile/tmp/toon-app-bus/v1/comfort-hub/state.json`), as in the QML app.
- **Service**: `/qmf/bin/toon-matter-service.sh` (pid file
  `/var/run/toon-matter-service.pid`). "Starting" means the process lives but
  has not written a fresh status yet (about a minute on a Toon 2).

The daemon:

1. updates the tile over BoxTalk (service `matter`) and toonui's local API;
2. **keeps the bridge running**: the bridge starts at boot from
   `/etc/rc5.d/S99toon-matter`, but nothing restarts it after a crash. When the
   service has been down for 45 s the daemon starts it (three tries two
   minutes apart, then every 30 minutes; a healthy bridge resets the count);
3. runs the screen's requests, so toonui never blocks: the screen writes
   `restart` or `reset` to `/var/run/freetoon-matter/command`, the daemon runs
   it and reports in `/var/run/freetoon-matter/action.json`. That folder is
   root-only (0700) and is refused when it is a symlink or owned by someone
   else; files are written fresh (`O_EXCL|O_NOFOLLOW`) and renamed into place.
   **Remove pairing** stops the service, runs Matterbridge's own
   `--reset` (`toon-matter-service.sh cli --bridge --reset`: clears fabrics,
   sessions and commissioning data) and starts it again. The pairing code
   stays the same (passcode and discriminator are fixed in the service script).

Log: `/var/volatile/tmp/integration-matter.log`; the bridge's own log is
`/var/volatile/log/toon-matter-service.log`.

## Pairing (Apple Home)

1. Phone on the same network as the Toon (for a separate IoT network: mDNS and
   IPv6 link-local must reach the Toon; same Wi-Fi is simplest).
2. Open the Matter tile on the Toon.
3. Home app: **+** > **Add Accessory**, scan the QR code (or **More options** and
   type the code). At "Uncertified Accessory": **Add Anyway**.
4. Choose a room; the thermostat (and the energy meter, if shared) appear.

Google Home: **+** > **Set up device** > **Matter-enabled device**, same code.

## Build

Cross builds run in the `freetoon-build` container with the Linaro (Toon 2)
and Toon 1 toolchains in `~/toolchains`, against the FreeToon tree
(`FREETOON_SRC`, default `../../../freetoon-lvgl`). Use a tree with the
settings layout of FreeToon 1.0.12 or later (hence `min_toonui` 1.0.12 in the
catalog): toonui refuses a plugin whose settings fields moved. With the
theme tokens of FreeToon 1.1 in `theme.h` the screen follows the chosen theme;
on an older toonui the tokens fall back to light/dark.

```sh
docker run --rm --platform linux/amd64 -v ~/toolchains:/root/toolchains -v "$PWD/../../..":/src \
  -w /src/freetoon-integrations/integrations/matter freetoon-build \
  sh -c 'make TARGET=toon2 all abi-check && make TARGET=toon1 all abi-check'
```

On the build host:

```sh
make test                                   # unit test of the shared logic
make preview FREETOON_SRC=/path/to/freetoon-lvgl   # renders preview/*.png with the real LVGL
```

Test install on a Toon without the Marketplace: copy the package and
`tools/install-local.sh` to `/tmp` on the Toon under their own names and run
`sh /tmp/matter-install-local.sh /tmp/matter-ft.tgz`.

## Notes

- Look: FreeToon's `THEME_*` colour, radius and type tokens only, on an 8 px
  grid; surfaces separated by fill rather than outlines, one accent, status
  as a small dot with text. The one fixed colour is the QR panel: black on
  white with a quiet zone of about four modules in every theme, because phone
  scanners need that contrast. The setup code shows as 4-3-4 digits, the
  grouping Apple Home and Google Home use.
- The plugin declares the settings fields it reads (`language`,
  `home_light_mode`) with `FT_PLUGIN_DECLARE_ABI`; built against LVGL 8.3.
- UI text is Dutch with English through FreeToon's `tr()`.
