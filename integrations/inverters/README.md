# Omvormers (inverters) - FreeToon integration

Reads a solar inverter and hands the production to FreeToon, which shows it on
the Zon tile, the dim screen and (later) the statistics. BACKLOG #60 in
freetoon-lvgl, phase 2: the core owns everything solar, this app only measures.

Driver logic ported from oepi-loepi's ToonStore `solarPanel` app
(`solarpanel-plugins`: ENPHASE1, ENPHASE-T1, OMNIK2, SOLISCLOUD1), with his
permission.

## Supported

| Setting `TYPE` | What it reads | Notes |
|---|---|---|
| `enphase` | Envoy `/api/v1/production` on the LAN | Firmware < 7: plain HTTP, nothing else needed. Firmware 7+: HTTPS with an owner token, pasted in or fetched by the app from the Enlighten account (login.json + entrez `/tokens`, like pyenphase). The fetched token is cached per Envoy address. |
| `solis_local` | WiFi data logger `/status.html` (basic auth, default admin/admin) | Older Solis / Ginlong sticks; falls back to `/js/status.js`. Not on newer "S3" sticks: use SolisCloud. |
| `soliscloud` | SolisCloud API `/v1/api/userStationList` | Needs a KeyId + KeySecret requested from Solis. HMAC-SHA1 signed; first station. Polled every 2 min at most. |
| `omnik` | Logger `/js/status.js` (`webData` / `myDeviceArray`), falls back to `/status.html` | The Omnik portal is gone; local only. |

## How it works

- Settings: `inverters.conf` (KEY=VALUE) in the app directory, written by
  FreeToon's web page `http://<toon>:10081/solar`, which builds the form from
  the `settings` block in `manifest.json`. Secrets are write-only there, and a
  changed address needs the token / stick password typed again.
- Every poll (30 s, 2 min at night or for the cloud) the daemon POSTs to the
  core on 127.0.0.1:10081: `/api/v1/solar/source` (power, today, lifetime) and
  `/api/v1/marketplace/apps/inverters/state` (its own tile).
- HTTP goes through the Toon's `curl`; arguments (tokens, passwords) are
  passed in a 0600 config file that is removed right after, never on the
  command line. Nothing secret is logged. `INV_DEBUG=1` logs each request's
  URL and status.

## Build and test

```sh
make TARGET=toon2 && make TARGET=toon1 OUT=freetoon-app-inverters.toon1
make test     # parsers + signature on the build host, synthetic fixtures
```

Needs FreeToon 1.0.18 or newer (`/api/v1/solar/source` and the `/solar`
page). The catalog entry carries `"min_toonui": "1.0.18"`, so older Toons
(1.0.8+) show "Vereist 1.0.18" in the Marketplace instead of installing. If it
gets installed anyway (the `/api/install` route, or a Toon older than 1.0.8),
the core answers the source post with 404 (401 with the web login on) and the
app's tile says "Vereist FreeToon 1.0.18". Keep `MIN_CORE` in
`inverters_daemon.c` and `min_toonui` in the catalog in step.
