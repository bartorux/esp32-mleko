# Dokumentacja Smart Mleko

Katalog zawiera kompletną dokumentację techniczną projektu Smart Mleko ESP-NOW.

## Pliki

### DOKUMENTACJA_TECHNICZNA.md (główny dokument)

Kompletna dokumentacja techniczna dla deweloperów zawierająca:

- **Przegląd systemu** — cel projektu, główne cechy, kluczowe ustalenia
- **Architektura** — topologia gwiazdowa ESP-NOW, przepływ danych, diagramy
- **Hardware** — specyfikacja Matki, Satelity (bateria/zasilacz), schemat DS18B20
- **Struktury danych** — `struct_message` i `struct_ack` (packed, identyczne na obu urządzeniach)
- **Oprogramowanie Satelity** — v2.6, logika Deep Sleep, channel hopping, OTA download
- **Oprogramowanie Matki** — v4.1, single-file firmware, multi-satellite, ring buffer, alerty
- **REST API** — endpoints `/api/status`, `/api/historia`, `/ota/satelita/*`, itp.
- **Telegram bot** — komendy `/status`, `/historia`, `/set_max`, `/interwal`, itp.
- **System OTA** — procedura upload (PSRAM → LittleFS), download (HTTPClient), validacja
- **Troubleshooting OTA** — konkretne błędy i rozwiązania (CRC, Magic byte, timeout)
- **Jak zacząć po przerwie** — szybki start, etapy pracy, checklist, debugowanie
- **Znane problemy** — lista TODO, status debugowania

## Jak czytać dokumentację

1. **Szybki start** (10 min):
   - Przegląd systemu
   - Architektura
   - Szybki start z sekcji "Jak zacząć po przerwie"

2. **Głębokie zagłębienie** (1–2 h):
   - Wszystkie sekcje od 1 do 7
   - Szczególnie: Struktury danych, Oprogramowanie Satelity/Matki, REST API

3. **OTA debugging** (30 min):
   - Sekcja "System OTA (over-the-air updates)" — procedury krok po kroku
   - Sekcja "Troubleshooting OTA" — konkretne problemy i rozwiązania

4. **Terenu (5 min):
   - Sekcja "Debugowanie w terenie" — komendy curl, grep dla logów

## Struktura projektu

```
esp32-mleko/
├── CLAUDE.md                          ← instrukcje dla Claude Code
├── smart_mleko_v8.md                  ← specyfikacja projektowa (BOM, roadmap)
├── docs/
│   ├── README.md                      ← ten plik
│   └── DOKUMENTACJA_TECHNICZNA.md     ← główna dokumentacja (tego plik)
├── matka/
│   ├── platformio.ini
│   ├── src/
│   │   ├── main.cpp                   ← firmware Matki (v4.1, ~2000 linii)
│   │   └── secrets.h                  ← gitignored: WiFi, Telegram
│   └── data/                          ← LittleFS (jeśli potrzebny)
└── satelita/
    ├── platformio.ini                 ← 2 environments: esp32_bat, esp32c3_zas
    ├── src/
    │   └── main.cpp                   ← firmware Satelity (v2.6)
    ├── firmware.bin                   ← output dla OTA
    └── copy_firmware.py               ← auto-copy build → firmware.bin
```

## Kluczowe ustalenia (zasady projektu)

1. **Struktury danych** muszą być identyczne na Matce i Satelicie (packed)
2. **Single-file firmware** Matki: wszystko w `src/main.cpp` (dashboard HTML inline)
3. **OTA Upload**: PSRAM (nie flash) → write w `loop()` (nie async callback)
4. **OTA Download**: pełny .bin bez Range requests (ESPAsyncWebServer nie obsługuje)
5. **Deep Sleep**: tylko Satelita bateriowa, Matka 24/7
6. **Preferences (NVS)**: przeżywają OTA, zawsze jeden `.bin` dla wszystkich satelit
7. **Okno godzinowe**: w RTC RAM (przeżywa Deep Sleep, nie power off)
8. **Język**: Polski (wszystkie UI, zmienne, komentarze)

## Build i flash

**Matka**:
```bash
cd matka && pio run && pio run -t upload
```

**Satelita**:
```bash
cd satelita && pio run -e esp32_bat
# Plik: .pio/build/esp32_bat/firmware.bin
```

Flash Satelity na Raspberry Pi (SSH):
```bash
~/esp/bin/esptool.py --chip esp32 --port /dev/ttyUSB0 erase_flash && \
~/esp/bin/esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash \
  0x1000 bootloader.bin 0x8000 partitions.bin 0x10000 firmware.bin
```

## Wersje

- **Matka**: v4.1 (w gałęzi `main`)
- **Satelita**: v2.6 (w gałęzi `main`)
- **Dokumentacja**: v1.0 (2026-04-02)

## Wsparcie

Wszystkie decyzje architektoniczne są udokumentowane w `DOKUMENTACJA_TECHNICZNA.md`. Zmiany wymuszają Update dokumentacji + commit do git.

---

**Data utworzenia**: 2026-04-02  
**Ostatnia aktualizacja**: 2026-04-02  
**Status**: Produkcyjny

