# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Smart Mleko — ESP-NOW-based milk temperature monitoring system with star topology. One **Matka** (hub, always-on) and up to 8 **Satelity** (sensors). Each Satelita can be battery-powered (deep sleep) or mains-powered (always on).

## Architecture

- **Matka** (`matka/`): ESP32-S3 N16R8 hub running 24/7. Receives ESP-NOW data from Satelita, serves web dashboard at `http://mleko.local`, sends Telegram alerts, handles WiFi captive portal, NTP sync, OTA updates. Single-file firmware (`src/main.cpp`) with inline HTML/CSS/JS for both captive portal and dashboard.
- **Satelita** (`satelita/`): DS18B20 temperature sensor, two PlatformIO environments. One universal firmware for all satellites.
  - `esp32_bat` env: WEMOS D1 ESP32 (board `lolin_d32`) for battery variant — **always use this**
  - `esp32c3_zas` env: ESP32-C3 Super Mini for mains variant
  - `ID_CZUJNIKA` and `TYP_ZASILANIA` stored in **Preferences (NVS)**, not `#define` — survive OTA updates
  - First USB flash sets ID and type; all subsequent OTA updates use the same `.bin` for every satellite
  - `TYP_ZASILANIA`: `1` = battery (18650 + deep sleep), `2` = mains (continuous loop)
  - `typ==1`: deep sleep `esp_deep_sleep(interwal_s * 1e6)`, backoff 60s gdy brak Matki
  - `typ==2`: `delay(interwal_s * 1000)` w pętli
  - `RTC_DATA_ATTR uint32_t rtc_interwal_s` — interwał przeżywa deep sleep; aktualizowany z ACK
- **Spec document** (`smart_mleko_v8.md`): Complete project specification including roadmap, data structures, and planned features.
- **Technical docs** (`docs/DOKUMENTACJA_TECHNICZNA.md`): Full Polish-language technical documentation.

## Build & Flash (PlatformIO)

```bash
# Build
cd matka && pio run -e esp32s3
cd satelita && pio run -e esp32_bat

# Upload (Matka connected via USB — port changes after each reset!)
ls /dev/cu.usb*       # find current port first
cd matka && pio run -t upload   # requires BOOT+EN bootloader mode

# Upload filesystem (LittleFS) for Matka
cd matka && pio run -t uploadfs
```

Satelita bateriowa (esp32_bat) — flash przez Raspberry Pi (`ssh admin@pi.local`, hasło: `admin`):
```bash
# Kopiuj na Pi
scp .pio/build/esp32_bat/bootloader.bin .pio/build/esp32_bat/partitions.bin satelita/firmware.bin "admin@pi.local:/home/admin/"

# Flash (chip esp32, nie esp32c3!)
ssh admin@pi.local "~/esp/bin/esptool.py --chip esp32 --port /dev/ttyUSB0 erase_flash && ~/esp/bin/esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash 0x1000 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin"
```

Satelita zasilaczowa C3 Super Mini (esp32c3_zas) — flash bezpośrednio USB-C do Maca:
```bash
cd satelita && pio run -t upload -e esp32c3_zas
```

Logi satelity przez Pi (persistent — reconnectuje po deep sleep):
```bash
ssh admin@pi.local "while true; do stty -F /dev/ttyUSB0 115200 raw 2>/dev/null; cat /dev/ttyUSB0 2>/dev/null; sleep 1; done"
```

**OTA bin esp32_bat**: `satelita/firmware.bin` — auto-kopiowany przez `copy_firmware.py` po `pio run -e esp32_bat`.
**OTA bin esp32c3_zas**: `.pio/build/esp32c3_zas/firmware.bin` — `copy_firmware.py` kopiuje tylko esp32_bat, dla C3 wskaż ręcznie.

## ESP-NOW Data Structures

Both devices share identical packed structs — keep them in sync when modifying:

- `struct_message` (Satelita → Matka): `id_czujnika`, `typ_zasilania`, `temperatura`, `bateria_procent`, `timestamp`, `blad_czujnika`, `fw_version[8]`
- `struct_ack` (Matka → Satelita): `nowy_interwal_s`, `godzina_start`, `godzina_stop`, `ota_pending`, `ota_url[64]`, `wifi_ssid[32]`, `wifi_pass[64]`

## Key Technical Details

- **Secrets**: WiFi credentials and Telegram tokens in `matka/src/secrets.h` (gitignored, contains real tokens — **NEVER commit**). Defines: `DEFAULT_WIFI_SSID`, `DEFAULT_WIFI_PASS`, `TELEGRAM_BOT_TOKEN`, `TELEGRAM_GROUP_ID`
- **WiFi flow**: Matka tries saved LittleFS creds → fallback to hardcoded defaults → AP mode captive portal (`Smart_Mleko_Setup`)
- **HTML is inline**: Dashboard and captive portal HTML live as `PROGMEM` C-string literals in `matka/src/main.cpp` (no separate HTML files)
- **NTP timezone**: `CET-1CEST,M3.5.0,M10.5.0/3` (Europe/Warsaw)
- **Partition scheme**: Matka uses `default_16MB.csv` (16MB flash), Satelita uses `min_spiffs.csv` — both needed for OTA support
- **DS18B20 quirk**: First reading after wake returns stale data — always do a warmup read and discard it
- **All UI text and variable names are in Polish**
- **ESP-NOW channel**: Matka inits ESP-NOW *after* WiFi.begin() so the channel matches the router. Satelita stores last successful channel in RTC RAM (`ostatni_kanal`) as a hint — tries it first on wake, then falls back to full channel hopping (1–13)
- **MAC addresses**: Matka MAC is hardcoded in `satelita/src/main.cpp` (`adresMatki[]`). Satelita MACs are auto-learned on first ESP-NOW receive (auto-discovery)
- **Multi-satellite**: Matka supports up to `MAX_SATELITY` (8) satellites. Each tracked in `SatelitaInfo` struct array with: id, MAC, type (battery/mains), last measurement, timestamps, OTA state. Satellites auto-register on first message.
- **Matka is a single file** (`matka/src/main.cpp`) — dashboard HTML, captive portal HTML, API handlers, Telegram bot, ESP-NOW callbacks, and config persistence all live in one file
- **Build flags**: Matka uses `ARDUINO_USB_CDC_ON_BOOT=1`, `BOARD_HAS_PSRAM`, and `board_build.arduino.memory_type = qio_opi` (required for OPI PSRAM on S3 N16R8). Satelita does NOT use `ARDUINO_USB_CDC_ON_BOOT` — `esp32_bat` uses `-DPLATFORM_ESP32`, `esp32c3_zas` uses `-DPLATFORM_C3`
- **NVS Preferences**: Matka namespace `"mleko"` — keys: `prog_max`, `prog_min`, `prog_wzrost`, `interwal`, `interwal_zasil`, `cichy_od`, `cichy_do`. Satelita namespace `"satelita"` — keys: `id`, `typ`
- **LittleFS runtime paths**: `/wifi.json` (WiFi credentials), `/nazwy.json` (satellite display names), `/monitoring.json` (only_monitoring flags, only `true` entries stored), `/ota/satelita.bin` (satellite firmware, fallback when PSRAM `ota_buf` is null)
- **Satellite names**: Stored in `satellite_names[MAX_SAT_ID][32]` global (indexed by ID). Loaded at boot from LittleFS, applied to `SatelitaInfo.nazwa` when satellite registers. Editable via `POST /api/satelita/nazwa` and pencil icon in dashboard.
- **Satellite removal**: `POST /api/satelita/usun` removes satellite from the in-memory `satelity[]` array and calls `esp_now_del_peer()`. Names and `tylko_monitoring` flags are preserved in LittleFS — auto-restored when satellite reconnects. `satelity[]` is NOT persisted across reboots by design.

## REST API (Matka, STA mode)

```
GET  /api/status           — sensor data + system info
GET  /api/historia         — ring buffer per satellite (48 entries, 24h at 30min). Supports ?id=X filter
GET  /api/ustawienia       — current config
POST /api/ustawienia       — update config (JSON body)
POST /api/satelita/nazwa       — set satellite display name (JSON: {id, nazwa})
POST /api/satelita/monitoring  — set monitoring-only flag (JSON: {id, tylko_monitoring})
POST /api/satelita/usun        — remove satellite from tracking list (JSON: {id})
POST /api/wifi-reset       — delete WiFi creds, restart to AP mode
POST /ota/matka            — multipart firmware upload for Matka
POST /ota/satelita/begin   — start chunked upload session (?size=XXXXX)
POST /ota/satelita/chunk   — upload 8KB chunk (raw binary body)
POST /ota/satelita/finish  — finalize upload, set OTA pending for all satellites
GET  /ota/satelita.bin     — serve stored satellite firmware (for OTA download)
```

## Telegram Commands

`/status`, `/historia`, `/srednia`, `/raport`, `/set_max <val>`, `/set_min <val>`, `/interwal <min>`, `/cichy <od> <do>`, `/ustawienia`, `/wifi-reset`, `/pomoc`

## OTA Flow (Satelita) — WORKING as of v2.8

**Upload (browser → Matka):**
1. JS splits `.bin` into 8KB chunks, calls `begin?size=N` → Matka allocates `ps_malloc(N)` in PSRAM (`ota_buf`)
2. Each chunk: `POST /ota/satelita/chunk` (raw binary) → `memcpy` to `ota_buf + offset` — no flash write
3. `POST /ota/satelita/finish` → validates `offset == size` → sets `ota_write_pending = true`, returns 200 immediately
4. In `loop()` (not in async handler!): writes `ota_buf` to LittleFS in 8KB chunks with `delay(1)` → sets `ota_pending=true` for all satellites
5. **`ota_buf` is NOT freed after LittleFS write** — kept in PSRAM for serving

**Serving .bin:**
- `GET /ota/satelita.bin` serves **directly from PSRAM** (`ota_buf`) using `beginResponse` with fill callback
- **NOT from LittleFS** — LittleFS reading stalls at 4KB block boundary 186 (byte 761856), causing >30s timeout
- Falls back to LittleFS only if `ota_buf` is null (e.g., after Matka restart)

**Download (Satelita ← Matka):**
1. Satelita receives ACK with `ota_pending=true`, `ota_url`, `wifi_ssid`, `wifi_pass`
2. `esp_now_deinit()` → `WiFi.begin(ssid, pass)`
3. `HTTPClient.GET(url)` — streams full file (no Range requests — ESPAsyncWebServer doesn't support them)
4. `Update.begin(totalSize)` → read 512B chunks → `Update.write()` → `Update.end(true)` → restart
5. Timeouts: `http.setTimeout(60000)`, no-data timeout 30s

**OTA flags per satellite (`SatelitaInfo`):**
- `ota_pending`: set when firmware available; Matka includes OTA URL in ACK
- `ota_url_wyslany`: set when URL sent in ACK; cleared (along with `ota_pending`) on next message from satellite — regardless of `blad_czujnika`
- `.bin` NOT auto-deleted — overwritten by next upload
- Heartbeat alert suppressed while `ota_url_wyslany=true` (satellite silent during OTA)

## Dependencies

**Matka** (`matka/platformio.ini`): ESPAsyncWebServer, ArduinoJson v7
**Satelita** (`satelita/platformio.ini`): OneWire, DallasTemperature
