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
| `solaredge_cloud` | SolarEdge Monitoring API v2 (OAuth "Site Access": client id + secret from developer.solaredge.com) | Needs FreeToon 1.0.22+: the /solar page has a "Koppelen met SolarEdge" button (connect.solaredge.com/authorize) and the callback `/solar/callback` hands `?code=&site_id=` to the app (file `oauth_callback`); pasting that address into SE_AUTH also works. Tokens in `solaredge.state` (refresh token rotates). Quarter-hour power every 30 min and the lifetime counter every 2 h, 06:00-22:00 (free tier ~2000 calls/month); HTTP 429 "credit limit" pauses until the 1st. Wire details from the open-source HA / Domoticz clients, not verified on a real account yet. `INV_SE_API` points at a test server. |
| `goodwe_sems` | GoodWe SEMS+ portal (classic SEMS closed 2026-09-23) | E-mail + password of the SEMS+ account, optional installation name (else the first). Ported from the solarPanel app's GOODWE1/GOODWE2 v2.1.0 plugin (oepi-loepi & WickedMaster): cross-login (pwd = base64(md5 hex)), stations/simple-query, device/all-status (INVERTER first), equipments/<sn>/telemetry (pAc kW) and telecounting today / lifetime (one groups value per call). token header + X-Signature (base64(sha256hex(ms@uid@token)@ms)) on every call. Polled every 5 min. `INV_GW_LOGIN` / `INV_GW_API` point at a test server. |
| `fronius` | Fronius Solar API v1 (local) | IP. GetInverterRealtimeData (Device 1): PAC W while StatusCode 7, DAY_ENERGY / TOTAL_ENERGY Wh. Port of solarPanel FRONIUS1. |
| `sma_web` | SMA webconnect (local) | IP + 'User' password. /dyn/login.json (right usr) -> sid, /dyn/getValues.json 6100_40263F00 W, 6400_00262200 today Wh, 6400_00260100 total Wh. https (self-signed) tried first, then http; the sid is kept and renewed only when refused (SMA allows few sessions). Port of SMA1/SMA3. |
| `kostal_piko` | Kostal Piko web page (local) | IP; basic auth pvserver:pvwr. Values after `bgcolor="#FFFFFF">`: power W, total kWh, today kWh. Port of KOSTAL1. |
| `zeversolar` | ZeverSolar home.cgi (local) | IP. Line 11 W, line 12 today kWh ("4.5" = 4.05), line 13 OK. No lifetime total. Port of ZEVER1. |
| `saj_local` | SAJ real_time_data.xml (local) | IP. p-ac W, e-today / e-total kWh. Port of SAJ1. |
| `fp4all` | FP4ALL datalogger status.xml (local) | IP. gauge_power W, energy_today / energy_total Wh. Port of FP4ALL. |
| `opendtu` | openDTU (Hoymiles, local) | IP. /api/livedata/status total.Power W, YieldDay Wh, YieldTotal kWh. Port of OPENDTU1. |
| `domoticz` | Domoticz solar device | host:port + IDX, or two as `12+19` (added up). Usage W, CounterToday / Data kWh; new getdevices API, then the old one. Port of DOM-DOUBLE1/2. |
| `pvoutput` | PVOutput (cloud) | API key + system id. getstatus.jsp (today Wh, power W) + getstatistic.jsp (lifetime Wh); every 5 min (60 calls/hour limit). Port of PVOUTPUT1. `INV_PVO_API` = test server. |
| `solax_cloud` | SolaxCloud (cloud) | Token id + wifi-dongle registration number. getRealtimeInfo acpower W, yieldtoday / yieldtotal kWh; every 5 min. Port of SOLAX1. `INV_SOLAX_API` = test server. |
| `growatt_cloud` | Growatt portal | Username + password. newTwoLoginAPI.do (password = MD5 hex with '0' at even positions replaced by 'c'; server-api.growatt.com, server.growatt.com on 405), session cookie kept in a jar, newPlantAPI.do getUserCenterEnertyDataTwo: powerValue W, todayValue / totalValue kWh. Port of GROW1. `INV_GROWATT_API` / `INV_GROWATT_API2` = test servers. |
| `fusionsolar` | Huawei FusionSolar portal | Username + password. App token login (intl.fusionsolar.huawei.com:32800, Host region01eu5 first) -> roaRand + accessToken; station-list on uni004eu5 with Host region01eu5, then region04eu5; first station currentPower kW, dailyEnergy / cumulativeEnergy kWh. An explicit refusal is not retried (account lock). Port of HUAWEI-T1 + solarPanel.sh. `INV_FS_LOGIN` / `INV_FS_DATA` = test servers. |
| `hoymiles_cloud` | Hoymiles S-Miles Cloud | Username + password + station id. iam/auth_login (MD5 hex) -> token; pvm-data/data_count_station_real_data: real_power W, today_eq / total_eq Wh; new login only when the token is refused. Port of HOY1. `INV_HOY_API` = test server. |

Portal logins (Growatt, FusionSolar, Hoymiles): a refused login is not tried again for 60 minutes or until the settings are saved, so a wrong password cannot lock the account. `INV_TEST_WAIT=<s>` shortens the poll wait in tests.
| `omnik` | Logger `/js/status.js` (`webData` / `myDeviceArray`), falls back to `/status.html` | The Omnik portal is gone; local only. |
| `sunspec` | SunSpec over Modbus TCP (`MB_PORT`, `MB_UNIT`): SolarEdge, SMA, Fronius, Kostal and other SunSpec inverters | Finds the "SunS" marker at register 40000, 50000 or 0, walks the model chain to inverter model 101-103 (integer + scale factor) or 111-113 (float) and reads W (power) and WH (lifetime). One short connection per poll (SolarEdge serves one Modbus client at a time). Modbus TCP must be enabled on the inverter; SolarEdge: port 1502, reliable over a LAN cable, over WiFi only on some firmware. Typical unit ids: SolarEdge / Fronius 1, SMA 126, Kostal 71. Tested against a synthetic SunSpec server only (tests and `sunspec` e2e), not yet on a real inverter. |
| `huawei` | Huawei SUN2000, holding registers: 32080 I32 power W, 32106 U32 lifetime 0.01 kWh, 32114 U32 today 0.01 kWh | SDongle port 502 unit 1 (Modbus TCP set to unrestricted in FusionSolar), or the inverter's own WLAN on port 6607 unit 0. Waits 1 s after connecting before the first request. |
| `sungrow` | Sungrow via WiNet-S, input registers 5031 U32 power W, 5004 U32 lifetime kWh, 5003 U16 today 0.1 kWh (documented 1-based: read at 5030 / 5003 / 5002), low word first | Modbus on in the WiNet-S, whitelist off or including the Toon. |
| `growatt` | Growatt Modbus RTU protocol II, input registers 35 U32 Pac 0.1 W, 55 U32 lifetime 0.1 kWh, 53 U32 today 0.1 kWh | Needs an RS485-to-TCP gateway (Modbus TCP to RTU). |
| `solis_modbus` | Solis / Ginlong RS485 Modbus, input registers 3005 U32 power W, 3009 U32 lifetime kWh, 3015 U16 today 0.1 kWh (1-based: read at 3004 / 3008 / 3014) | Needs an RS485-to-TCP gateway. |

The brand maps (`MB_PROFILES` in inv_drivers.c) come from community register lists and are tested against synthetic servers built from the same lists; the raw register words are logged once per settings load (`<brand> raw: ...`), so a tester's log shows what the inverter really answers. A wrong scale or word order shows up there and is a one-line fix in the table. GoodWe is not included: older models speak a UDP protocol on port 8899, Modbus TCP on the newer ones has to be enabled by GoodWe support.

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
