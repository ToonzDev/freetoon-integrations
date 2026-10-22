# freetoon-integrations

Official community integrations repository and package catalog for FreeToon (`freetoon-lvgl`).

## Directory Structure

```text
freetoon-integrations/
├── catalog/
│   └── index.json             # Marketplace index fetched by FreeToon
├── packages/                  # Downloadable .tgz distribution archives
│   ├── temperaturelogger.tgz
│   └── crypto.tgz
├── integrations/              # Source folders for each integration
│   ├── temperaturelogger/
│   └── crypto/
└── scripts/
    └── package.sh             # Packages all integrations into packages/*.tgz
```

## Available Integrations

| ID | Name | Version | Description |
|---|---|---|---|
| `temperaturelogger` | Temperature Logger | 1.0.0 | 24-uurs temperatuurlogger met grafiek (kamer, setpoint, buiten). |
| `crypto` | Crypto Tracker | 1.0.0 | Live crypto koersen en 24h verandering via CoinGecko API. |

## Adding an Integration

1. Create a new folder under `integrations/<id>/`.
2. Add a `manifest.json` describing the integration, its binary, tile settings, and optional detail screen.
3. Run `./scripts/package.sh` to produce `packages/<id>.tgz`.
4. Add the entry to `catalog/index.json`.
5. Submit a pull request.

