# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Smart Mleko — ESP-NOW-based milk temperature monitoring system with star topology. One **Matka** (hub, always-on) and up to 8 **Satelity** (sensors). Each Satelita can be battery-powered (deep sleep) or mains-powered (always on).

## Architecture

- **Matka** (`matka/`): ESP32-S3 N16R8 hub running 24/7. Receives ESP-NOW data from Satelita, serves web dashboard at `http://mleko.local`, sends Telegram alerts, handles WiFiManager captive portal, NTP sync, OTA updates. Single-file firmware (`src/main.cpp`) with inline HTML/CSS/JS for both captive portal and dashboard.
- **Satelita** (`satelita/`): ESP32-S3 with DS18B20 temperature sensor. One universal firmware for all satellites.
  - `ID_CZUJNIKA` and `TYP_ZASILANIA` stored in **Preferences (NVS)**, not `#define` — survive OTA updates
  - First USB flash sets ID and type; all subsequent OTA updates use the same `.bin` for every satellite
  - `TYP_ZASILANIA`: `1` = battery (18650 + deep sleep), `2` = mains (continuous loop)
  - Battery variant: wakes → measures → sends → gets ACK → sleeps
  - Mains variant: always on, measures at interval
- **Spec document** (`smart_mleko_v8.md`): Complete project specification including roadmap, data structures, and planned features.

## Build & Flash (PlatformIO)

```bash
# Build
cd matka && pio run
cd satelita && pio run

# Upload (Matka connected via USB)
cd matka && pio run -t upload

# Serial monitor
cd matka && pio run -t monitor
# or: pio device monitor -b 115200

# Upload filesystem (LittleFS) for Matka
cd matka && pio run -t uploadfs
```

Matka upload port: `/dev/cu.usbmodem212401`. Satelita upload port not yet configured (needs CP2102 driver).

## ESP-NOW Data Structures

Both devices share identical packed structs — keep them in sync when modifying:

- `struct_message` (Satelita → Matka): `id_czujnika`, `typ_zasilania`, `temperatura`, `bateria_procent`, `timestamp`, `blad_czujnika`
- `struct_ack` (Matka → Satelita): `nowy_interwal_s`, `godzina_start`, `godzina_stop`, `ota_pending`, `ota_url[64]`

## Key Technical Details

- **Secrets**: WiFi credentials and Telegram tokens in `matka/src/secrets.h` (gitignored, contains real tokens — **NEVER commit**). Defines: `DEFAULT_WIFI_SSID`, `DEFAULT_WIFI_PASS`, `TELEGRAM_BOT_TOKEN`, `TELEGRAM_GROUP_ID`
- **WiFi flow**: Matka tries saved LittleFS creds → fallback to hardcoded defaults → AP mode captive portal (`Smart_Mleko_Setup`)
- **HTML is inline**: Dashboard and captive portal HTML live as `PROGMEM` C-string literals in `matka/src/main.cpp` (no separate HTML files)
- **NTP timezone**: `CET-1CEST,M3.5.0,M10.5.0/3` (Europe/Warsaw)
- **Partition scheme**: `min_spiffs.csv` — needed for OTA support
- **DS18B20 quirk**: First reading after wake returns stale data — always do a warmup read and discard it
- **All UI text and variable names are in Polish**
- **ESP-NOW channel**: Matka inits ESP-NOW *after* WiFi.begin() so the channel matches the router. Satelita does channel hopping (1–13) if ACK is not received
- **MAC addresses**: Matka MAC is hardcoded in `satelita/src/main.cpp` (`adresMatki[]`). Satelita MACs are auto-learned on first ESP-NOW receive (auto-discovery)
- **Multi-satellite**: Matka supports up to `MAX_SATELITY` (8) satellites. Each tracked in `Satelita` struct array with: id, MAC, type (battery/mains), last measurement, timestamps, OTA state. Satellites auto-register on first message. Dashboard shows a card per satellite. Telegram alerts identify sensor by ID
- **Matka is a single file** (`matka/src/main.cpp`) — dashboard HTML, captive portal HTML, API handlers, Telegram bot, ESP-NOW callbacks, and config persistence all live in one file
- **Build flags**: Both devices use `ARDUINO_USB_CDC_ON_BOOT=1` for USB Serial output. Matka additionally has `BOARD_HAS_PSRAM`

## REST API (Matka, STA mode)

```
GET  /api/status       — sensor data + system info
GET  /api/historia     — ring buffer per satellite (48 entries, 24h at 30min). Supports ?id=X filter
GET  /api/ustawienia   — current config
POST /api/ustawienia   — update config (JSON body)
POST /api/wifi-reset   — delete WiFi creds, restart to AP mode
POST /ota/matka        — multipart firmware upload for Matka
POST /ota/satelita     — upload satellite firmware (.bin), stored in LittleFS
GET  /ota/satelita.bin — serve stored satellite firmware (for OTA download)
```

## Telegram Commands

`/status`, `/historia`, `/srednia`, `/raport`, `/set_max <val>`, `/set_min <val>`, `/interwal <min>`, `/cichy <od> <do>`, `/ustawienia`, `/wifi-reset`, `/pomoc`

## Dependencies

**Matka** (`matka/platformio.ini`): ESPAsyncWebServer, ArduinoJson v7
**Satelita** (`satelita/platformio.ini`): OneWire, DallasTemperature
