# Verkeer — FreeToon integratie

Actuele files en wegafsluitingen in Nederland op de FreeToon, uit de open data van
het Nationaal Dataportaal Wegverkeer (NDW, licentie CC0 1.0).

## Wat je ziet
- **Tegel**: met ingestelde wegen het aantal files op jouw wegen (bij één file: weg,
  lengte en vertraging), daaronder het aantal afsluitingen op je wegen en het landelijke
  totaal ("NL: 12 files, 45 km"). Zonder wegen: het landelijke totaal en de langste file.
- **Detailscherm** (tik op de tegel):
  - files op je wegen, dan afsluitingen op je wegen (met eindtijd en toelichting),
  - alle files in Nederland, de grootste vertraging bovenaan.
  - **Wegen**: kies de wegen die je rijdt, met komma's (bijv. `A2, A12, N201`).

## Hoe het werkt
- De daemon haalt elke 5 minuten `https://opendata.ndw.nu/actueel_beeld.xml.gz` op
  (~320 KB; alleen als het bestand veranderd is), pakt het uit met `gunzip` en leest de
  files (`AbnormalTraffic`) en afsluitingen (weg of rijbaan dicht) die nu gelden.
- De feed heeft geen wegnummers of plaatsnamen. Die komen uit de VILD-locatietabel van
  NDW, meegeleverd als `vild.tsv` (gemaakt met `tools/make_vild.py` uit `VILD6.14.A.zip`
  van opendata.ndw.nu). Meldingen zonder VILD-locatie (vaak gemeentelijke wegen) worden
  niet getoond.
- Bestanden in `/mnt/data/integrations/verkeer/`: `verkeer.conf` (WEGEN=, INTERVAL=
  in minuten, 3-30), `items.tsv` (voor het detailscherm), `state.json`.
- Data: ca. 90 MB per dag bij het standaardinterval van 5 minuten.

## Bouwen en testen
- `make TARGET=toon2` / `make TARGET=toon1` (of `scripts/package.sh` voor het pakket).
- `make host`: daemon op de pc. Test met een lokaal bestand:
  `VK_DIR=<dir met vild.tsv> VK_URL=<feed.xml.gz> VK_CORE=- VK_ONCE=1 tests/freetoon-app-verkeer.host`
- Detailscherm in de FreeToon-simulator: bouw het scherm voor de host (`make sim`) en
  start `SIM_PLUGIN=tests/verkeer_ui.sim.so VK_DIR=<dir> toonui-sim plugin out.ppm`.
