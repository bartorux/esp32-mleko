# DOKUMENTACJA TECHNICZNA: Smart Mleko ESP-NOW

**Projekt:** Smart Mleko — System monitorowania temperatury mleka oparte o ESP-NOW  
**Wersja dokumentacji:** 2.1  
**Data:** 2026-04-08  
**Status:** Produkcyjny (Matka v5.3, Satelita v3.0)  
**Typ:** Dokumentacja techniczna dla developerów

---

## Spis treści

1. [Przegląd systemu](#1-przegląd-systemu)
2. [Architektura i topologia](#2-architektura-i-topologia)
3. [Hardware i komponenty](#3-hardware-i-komponenty)
4. [Struktury danych ESP-NOW](#4-struktury-danych-esp-now)
5. [Oprogramowanie Satelity](#5-oprogramowanie-satelity)
6. [Oprogramowanie Matki](#6-oprogramowanie-matki)
7. [REST API Matki](#7-rest-api-matki)
8. [Komendy Telegram](#8-komendy-telegram)
9. [System OTA (over-the-air updates)](#9-system-ota)
10. [Konfiguracja i build](#10-konfiguracja-i-build)
11. [Troubleshooting OTA](#11-troubleshooting-ota)
12. [Jak zacząć po przerwie](#12-jak-zacząć-po-przerwie)
13. [Znane problemy i TODO](#13-znane-problemy-i-todo)

---

## 1. Przegląd systemu

### Cel

System monitorowania temperatury mleka w schładzalniku dla teścia. Topologia gwiazdowa ESP-NOW:
- **1 Matka** (hub centralny, 24/7)
- **Do 8 Satelitów** (czujniki temperaturowe rozłożone w opadach lub przy zdrojach)

### Główne cechy

- **Komunikacja**: ESP-NOW (2.4 GHz, bez WiFi, zasięg ~100 m)
- **Czujnik**: DS18B20 One-Wire (dokładność ±0.5°C)
- **Zasilanie Matki**: 5V/1A z USB-C (24/7)
- **Zasilanie Satelity**: USB-C 5V (always-on, bez baterii)
- **Interface**: Dashboard web (mleko.local), Telegram bot
- **Interwał pomiaru**: 5 minut (domyślnie, konfigurowalny)

### Kluczowe ustalenia

- Sonda DS18B20 zanurzona w mleku, uszczelnienie silikonem spożywczym
- Okno godzinowe i konfiguracja przez Telegram i Dashboard
- Tryb cichy: alerty łagodne blokowane, krytyczne zawsze przechodzą
- Czas synchronizowany z NTP (Europe/Warsaw)
- OTA dla obu urządzeń
- Satelita always-on (zasilacz), brak Deep Sleep

---

## 2. Architektura i topologia

```
┌─────────────────────────────────────────────────────────────┐
│                         INTERNET / WiFi                      │
└────┬──────────────────────────────────────────────────────┬──┘
     │                                                        │
     │  ┌──────────────────────────────────────┐           NTP
     │  │         MATKA (ESP32-S3 N16R8)      │        Telegram
     │  │    ▲ Hub centralny (24/7 na prądzie)│         mDNS
     │  │    │ • WiFi  + ESP-NOW              │      (mleko.local)
     │  │    │ • NTP   + Telegram             │
     │  │    │ • Dashboard + REST API         │
     │  │    │ • LittleFS (firmware satelit)  │
     │  └────┼──────────────────────────────┘
     │       │
     │  ┌────┴──────────────────────────────┐
     │  │ ESP-NOW (dwukierunkowy)           │
     │  │ • Dane + ACK                      │
     │  │ • Config (interwal, OTA flag)    │
     │  └────────────────────────────────┘
     │
 ┌───┴────────┬───────────────┬──────────┬──────────┐
 │            │               │          │          │
SAT#1 (Zasil) SAT#2 (Zasil)  SAT#3 (Zasil)  ... SAT#8
 │            │               │          │          │
 DS18B20    DS18B20         DS18B20    DS18B20     ...
 USB-C      USB-C           USB-C      USB-C

Legenda:
  SAT  = Satelita (sensor + mikrokontroler)
  ACK  = potwierdzenie + konfiguracja z Matki
  OTA  = over-the-air firmware update
```

### Przepływ danych

1. **Satelita** budzi się co N sekund (domyślnie 30 min)
2. **Satelita** wysyła pomiar ESP-NOW do Matki: `struct_message`
3. **Matka** odbiera, weryfikuje, dodaje do ring buffora
4. **Matka** odpowiada ACK z konfiguracją: `struct_ack`
5. **Satelita** odbiera ACK, aktualizuje parametry, wraca do Deep Sleep
6. **Matka** wysyła alerty Telegram i wyświetla na Dashboardzie

Jeśli Satelita nie otrzyma ACK (brak Matki w zasiegu):
- Spróbuj channel hopping (kanały 1–13)
- Jeśli znaleziona: zapisz kanał do RTC RAM (przeżyje Deep Sleep)
- Jeśli nie znaleziona: wrócić do Deep Sleep, spróbować przy następnym budzeniu

---

## 3. Hardware i komponenty

### Matka (hub)

| Parametr | Wartość |
|----------|---------|
| Mikrokontroler | ESP32-S3 N16R8 DevKit Dual USB-C |
| Pamięć flash | 16 MB (OTA dual boot) |
| PSRAM | 8 MB OPI (dla OTA buffer) |
| Zasilanie | 5V/1A USB-C (markowa ładowarka) |
| Łączność | WiFi 802.11b/g/n + ESP-NOW |
| Schładzanie | Pasywne |
| Uptime | 24/7 |

**Ładowarka**: Samsung, Xiaomi, Anker lub markowa. Tanie klony bez certyfikatów mogą powodować losowe resety przez niestabilne napięcie.

### Satelita (esp32c3_zas)

| Parametr | Wartość |
|----------|---------|
| Mikrokontroler | ESP32-C3 Super Mini |
| Czujnik | DS18B20 waterproof (GPIO4) |
| Zasilanie | USB-C 5V/1A |
| Łączność | ESP-NOW |
| Tryb pracy | Pętla nieskończona (no Deep Sleep) |
| Interwał | 5 min domyślnie (konfigurowalny z dashboardu) |
| Rezystor pull-up | 4.7 kΩ (DATA–3.3V) |

> **Uwaga:** Wariant bateryjny (WEMOS D1 lolin_d32) został usunięty z projektu. Jeśli w przyszłości dodany zostanie chip H2 lub inna platforma, tworzyć nowy env w platformio.ini.

### Schemat podłączenia DS18B20

```
ESP32 3.3V ────┬────── [4.7kΩ] ──────┐
               │                     │
ESP32 GPIO ────┼─────────────────────┤→ DS18B20 DATA
               │                     │  (żółty)
ESP32 GND ─────┴─────────────────────┤
                                     │
                          DS18B20 GND (czarny)
                          DS18B20 VCC (czerwony) ← 3.3V
```

**WAŻNE**: DS18B20 zasilać TYLKO z 3.3V, nigdy z 5V!  
Bez rezystora pull-up zwraca -127°C.

### Stałe hardware'owe (GPIO, ADC)

```cpp
// Satelita (ESP32-C3 Super Mini)
#define PIN_DS18B20   4   // GPIO4
// Brak ADC — typ=2 (zasilacz), bateria_procent zawsze 100
```

---

## 4. Struktury danych ESP-NOW

Beide urządzenia muszą mieć **identyczne** struktury (packed). Zmiana wymaga OTA dla OBU.

### struct_message (Satelita → Matka, 23 bajty)

```cpp
typedef struct __attribute__((packed)) {
    uint8_t  id_czujnika;      // 1–8, nadawca
    uint8_t  typ_zasilania;    // 1=bateria, 2=zasilacz
    float    temperatura;       // °C z DS18B20
    uint8_t  bateria_procent;  // 0–100% (tylko typ=1)
    uint32_t timestamp;        // millis()/1000 (od boot)
    bool     blad_czujnika;    // DS18B20 error (true = błąd)
    char     fw_version[8];    // np. "2.6", zakończony '\0'
} struct_message;
```

**Pola**:
- `id_czujnika`: 1–8, identyfikator satelity
- `typ_zasilania`: 1 = bateria (deep sleep), 2 = zasilacz (always on)
- `temperatura`: float, odczyt z DS18B20
- `bateria_procent`: wyliczone z ADC GPIO34 (typ=1 tylko)
- `timestamp`: uptime Satelity (nie czasu rzeczywistego!)
- `blad_czujnika`: true gdy DS18B20 zwróci -127°C lub 85°C
- `fw_version`: wersja firmware Satelity, np. "2.6"

### struct_ack (Matka → Satelita, 164 bajty)

```cpp
typedef struct __attribute__((packed)) {
    uint32_t nowy_interwal_s;  // 0 = bez zmian, >0 = zmień interwał
    uint8_t  godzina_start;    // okno pomiaru od (0–23)
    uint8_t  godzina_stop;     // okno pomiaru do (0–23)
    bool     ota_pending;      // czy czeka OTA
    char     ota_url[64];      // http://IP/ota/satelita.bin
    char     wifi_ssid[32];    // credentials do WiFi (dla OTA download)
    char     wifi_pass[64];    // hasło WiFi
} struct_ack;
```

**Pola**:
- `nowy_interwal_s`: 0 = utrzymaj aktualny, >0 = ustaw nowy (sekundy)
- `godzina_start`, `godzina_stop`: okno 0–23 (00:00–23:00). 0,0 = 24h, 22,6 = 22:00–06:00
- `ota_pending`: flaga OTA, Satelita pobiera firmware
- `ota_url`: adres HTTP dla `HTTPClient.GET()`
- `wifi_ssid`, `wifi_pass`: dla WiFi.begin() podczas OTA

---

## 5. Oprogramowanie Satelity

### Wersja: 3.0

**Plik**: `satelita/src/main.cpp`  
**Platforma**: ESP32-C3 Super Mini (mains-only, always-on)

### Schemat logiczny

```
┌──────────────────────────────────────┐
│         Boot Satelity                │
│   Wczytaj ID z Preferences (NVS)     │
│   Wczytaj ostatni_kanal z RTC RAM    │
│   Wczytaj rtc_interwal_s z RTC RAM   │
└──────────────────────────────────────┘
                 ↓
┌──────────────────────────────────────┐
│   Inicjalizacja ESP-NOW              │
│   Ustawienie kanału (hint z RTC RAM) │
│   Dodanie Matki jako peer            │
└──────────────────────────────────────┘
                 ↓
        ╔════════════════════╗
        ║  GŁÓWNA PĘTLA      ║
        ║  delay(interwal_s) ║
        ╚════════════════════╝
                 ↓
   ┌─────────────────────────────────┐
   │  Pomiar DS18B20                 │
   │  (1) warmup — odrzuć            │
   │  (2) właściwy — użyj            │
   └─────────────────────────────────┘
                 ↓
   ┌─────────────────────────────────┐
   │  Wyślij struct_message ESP-NOW  │
   │  Hint: spróbuj ostatni_kanal    │
   │  Fallback: channel hopping 1–13 │
   │  3 próby na kanał, 300ms przerwa│
   └─────────────────────────────────┘
                 ↓
   ┌─────────────────────────────────┐
   │  Czekaj ACK (timeout 2000 ms)   │
   └─────────────────────────────────┘
         │              │
      ACK OK        TIMEOUT (60s retry)
         │
         ↓
   ┌──────────────────────────────┐
   │ Z ACK: zaktualizuj           │
   │ • interwal_s → rtc_interwal_s│
   │ • ostatni_kanal (RTC RAM)    │
   │ Czy ota_pending?             │
   │   TAK → wykonajOTA()         │
   └──────────────────────────────┘
```

### Procedury kluczowe

#### Odczyt temperatury (ze warmupem)

```cpp
float odczytajTemperature(bool &blad) {
    // Warmup — odrzuć stary wynik z bufora DS18B20
    czujniki.requestTemperatures();
    czujniki.getTempCByIndex(0);  // odrzuć
    delay(100);

    // Właściwy pomiar
    czujniki.requestTemperatures();
    float temp = czujniki.getTempCByIndex(0);

    if (temp == DEVICE_DISCONNECTED_C || 
        temp == -127.0f || 
        temp == 85.0f) {
        blad = true;
        return -127.0f;
    }

    blad = false;
    return temp;
}
```

**Dlaczego warmup?** DS18B20 po budzeniu może zwrócić stary wynik z bufora. Zawsze 2 czytania.

#### Channel Hopping (z retry na kanał)

```cpp
bool znajdzKanal() {
    for (int k = 1; k <= 13; k++) {
        esp_wifi_set_channel(k, WIFI_SECOND_CHAN_NONE);
        // 3 próby na kanał — Matka może być chwilowo zajęta (Telegram)
        for (int pr = 0; pr < 3; pr++) {
            if (wyslijPomiar(0, true, 0) && czekajNaACK()) {
                ostatni_kanal = k;  // zapisz do RTC RAM
                return true;
            }
            if (pr < 2) delay(300);
        }
    }
    return false;
}
```

**Ważne:** Sonda channel-hopping ma `temperatura=0, bateria=0, blad=true`. Matka rozpoznaje ten wzorzec i **nie aktualizuje danych czujnika** ani nie wysyła alertu Telegram — tylko odpowiada ACK.

#### Konfiguracja Preferences (NVS)

```cpp
prefs.begin("satelita", false);
if (!prefs.isKey("id")) {
    prefs.putUChar("id", DEFAULT_ID);  // tylko przy pierwszym flashu USB
}
id_czujnika = prefs.getUChar("id", DEFAULT_ID);
prefs.end();
```

**Preferences przeżywają OTA** — ID jest bezpieczne. `typ_zasilania` usunięty z NVS (zawsze 2).

#### RTC RAM — przeżywa power-off

```cpp
RTC_DATA_ATTR uint8_t ostatni_kanal = 0;      // hint kanału na start
RTC_DATA_ATTR uint32_t rtc_interwal_s = 60;   // interwał z poprzedniego cyklu
```

`rtc_interwal_s` startuje od 60s (szybki pierwszy pomiar zanim Matka wyśle konfigurację).

#### OTA Download

```cpp
void wykonajOTA(const char *url, const char *ssid, const char *pass) {
    Serial.println("[OTA] Rozłącz ESP-NOW, łącz WiFi...");
    
    // Disable ESP-NOW
    esp_now_deinit();
    
    // Enable WiFi
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    
    // Wait for connection
    int proby = 0;
    while (WiFi.status() != WL_CONNECTED && proby < 30) {
        delay(500);
        proby++;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[OTA] WiFi failed, restarting...");
        WiFi.disconnect();
        ESP.restart();  // back to ESP-NOW
        return;
    }
    
    // Download .bin
    HTTPClient http;
    http.begin(url);
    int httpCode = http.GET();
    
    if (httpCode == 200) {
        int totalSize = http.getSize();
        WiFiClient *stream = http.getStreamPtr();
        
        if (Update.begin(totalSize)) {
            Update.writeStream(*stream);
            if (Update.end(true)) {
                Serial.println("[OTA] Success, restarting...");
                ESP.restart();
            }
        }
    }
    
    http.end();
    WiFi.disconnect();
    ESP.restart();
}
```

### Konfiguracja Preferences (NVS)

```cpp
Preferences prefs;

void setup() {
    prefs.begin("mleko", false);
    
    // Wczytaj lub ustaw domyślnie
    id_czujnika = prefs.getUChar("id", DEFAULT_ID);
    typ_zasilania = prefs.getUChar("typ", DEFAULT_TYP);
    interwal_s = prefs.getUInt("interwal", INTERWAL_DOMYSLNY_S);
    
    // RTC RAM — przeżywa deep sleep
    RTC_DATA_ATTR uint8_t ostatni_kanal = 0;
    RTC_DATA_ATTR uint8_t g_start = 0;
    RTC_DATA_ATTR uint8_t g_stop = 0;
    RTC_DATA_ATTR uint32_t interwal_s_rtc = INTERWAL_DOMYSLNY_S;
}

// Zmiana ID (przed pierwszym flashem USB):
// prefs.putUChar("id", 2);
// prefs.putUChar("typ", 1);
```

**Preferences przeżywają OTA** — zawsze jeden `.bin` dla wszystkich satelit.

---

## 6. Oprogramowanie Matki

### Wersja: 5.1

**Plik**: `matka/src/main.cpp` (single-file firmware, ~2000 linii)

### Zmienne globalne

```cpp
SatelitaInfo satelity[MAX_SATELITY];  // do 8 satelit
int ile_satelit = 0;

// OTA buffer w PSRAM (dla Upload)
uint8_t *ota_buf = nullptr;
size_t ota_buf_size = 0;
size_t ota_buf_offset = 0;
bool ota_write_pending = false;

// Konfiguracja (Preferences)
float prog_max = 8.0;      // alarm gdy temp > prog_max
float prog_min = 2.0;      // alarm gdy temp < prog_min
uint32_t interwal_s = 1800; // 30 min
uint8_t cichy_od = 23;     // tryb cichy 23:00–7:00
uint8_t cichy_do = 7;      // (jeśli > od: przez północ)
```

### Struktura SatelitaInfo

```cpp
struct SatelitaInfo {
    uint8_t id;
    uint8_t typ;                    // 1=bateria, 2=zasilacz
    uint8_t mac[6];
    struct_message pomiar;          // ostatni odebrane dane
    unsigned long ostatni_czas;     // millis()
    bool aktywna;
    bool ota_pending;               // czeka na OTA
    bool ota_url_wyslany;           // flaga: URL już wysłany
    HistoriaWpis historia[MAX_HISTORIA_PER];  // ring buffer 48h
    int hist_idx;                   // index w ring buffie
    int hist_count;                 // ile wpisów (do 48)
};
```

### Schemat logiczny

```
┌────────────────────────────────────┐
│    Boot Matki                      │
│  • Wczytaj WiFi creds (LittleFS)   │
│  • Połącz WiFi lub start AP        │
│  • NTP sync (CET-1CEST)            │
│  • mDNS (mleko.local)              │
└────────────────────────────────────┘
             ↓
┌────────────────────────────────────┐
│   Inicjalizacja ESP-NOW            │
│   Ustaw MAC jako broadcast         │
│   Zaciągnij kanal z WiFi           │
└────────────────────────────────────┘
             ↓
┌────────────────────────────────────┐
│   Start Serwera ESPAsyncWebServer  │
│   REST API + Dashboard + Upload OTA│
└────────────────────────────────────┘
             ↓
┌────────────────────────────────────┐
│   Start Telegram bot               │
│   Polling co 10s                   │
└────────────────────────────────────┘
             ↓
    ╔════════════════════════╗
    ║    GŁÓWNA PĘTLA        ║
    ║    loop() forever      ║
    ╚════════════════════════╝
             ↓
    ┌────────────────────────┐
    │ onDataRecv callback    │ ← ESP-NOW data
    │ (uruchamia się async)  │
    └────────────────────────┘
             ↓
   ┌─────────────────────────────────┐
   │ 1. Przetwórz pakiet             │
   │ 2. Dodaj do ring buffora        │
   │ 3. Reset OTA flag (jeśli trzeba)│
   │ 4. Wyślij ACK                   │
   └─────────────────────────────────┘
             ↓
   ┌─────────────────────────────────┐
   │ Sprawdzaj alerty:               │
   │ • Przekroczenie progu           │
   │ • Szybki wzrost temp            │
   │ • Bateria <15% / <5%            │
   │ • Brak sygnału >2h              │
   │ • Błąd DS18B20                  │
   │ • Brak NTP sync                 │
   └─────────────────────────────────┘
             ↓
   ┌─────────────────────────────────┐
   │ Sprawdzaj OTA write pending:    │
   │ Jeśli ota_write_pending:        │
   │   Zapisz ota_buf → /ota/sat.bin │
   │   w porcjach 8KB                │
   │   Ustaw ota_pending dla sat     │
   │   Wyślij ACK z flagą            │
   └─────────────────────────────────┘
```

### Callback ESP-NOW (onDataRecv)

ACK jest wysyłany **natychmiast z callbacku**, nie z `loop()`. Eliminuje opóźnienie spowodowane przez `sprawdzTelegram()` (blokujące HTTP, do kilku sekund).

```cpp
void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
    if (len != sizeof(struct_message)) return;

    struct_message msg;
    memcpy(&msg, data, sizeof(struct_message));

    SatelitaInfo *s = znajdzLubDodajSatelite(
        msg.id_czujnika, msg.typ_zasilania, mac
    );
    if (!s) return;

    // Filtruj sondy channel-hopping (temp=0, bat=0, blad=true)
    // Sonda NIE aktualizuje danych, NIE triggeruje alertów Telegram
    bool is_probe = (msg.blad_czujnika && msg.temperatura == 0.0f 
                     && msg.bateria_procent == 0);
    if (!is_probe) {
        memcpy(&s->pomiar, &msg, sizeof(struct_message));
        s->ostatni_czas = millis();
        s->aktywna = true;
        // Reset OTA: gdy satelita wróciła po restarcie
        if (s->ota_pending && s->ota_url_wyslany) {
            s->ota_pending = false;
            s->ota_url_wyslany = false;
        }
    }

    // ACK zawsze — sonda też potrzebuje odpowiedzi
    wyslijACK(s);
}
```

**Uwaga:** `sprawdzAlerty()` i `dodajDoHistorii()` wywoływane są w `loop()` na podstawie `s->ostatni_czas`. Sondy channel-hopping nie aktualizują `ostatni_czas` → nie trafiają do historii ani alertów.

### Wysłanie ACK

```cpp
void wyslijACK(SatelitaInfo *s) {
    // Dodaj peer jeśli nowy
    if (!esp_now_is_peer_exist(s->mac)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, s->mac, 6);
        peer.channel = 0;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    struct_ack ack = {};
    ack.nowy_interwal_s = interwal_s;  // zawsze wysyłaj
    ack.godzina_start = 0;              // TODO: per satelita
    ack.godzina_stop = 0;
    ack.ota_pending = s->ota_pending;

    if (s->ota_pending) {
        String url = "http://" + ip_adres + "/ota/satelita.bin";
        strlcpy(ack.ota_url, url.c_str(), sizeof(ack.ota_url));
        
        // Wyślij WiFi credentials do OTA download
        String ssid = WiFi.SSID();
        String pass = WiFi.psk();
        strlcpy(ack.wifi_ssid, ssid.c_str(), sizeof(ack.wifi_ssid));
        strlcpy(ack.wifi_pass, pass.c_str(), sizeof(ack.wifi_pass));
        
        s->ota_url_wyslany = true;
        Serial.printf("[ACK] OTA dla #%d\n", s->id);
    }

    esp_now_send(s->mac, (uint8_t*)&ack, sizeof(ack));
}
```

### Alerty

| Alert | Warunek | Cooldown | Dotyczy cichy_temp |
|-------|---------|----------|-------------------|
| Alarm max | temp > prog_max | `alert_cykl_s` per-satelita | TAK |
| Alarm min | temp < prog_min | `alert_cykl_s` per-satelita | TAK |
| Błąd DS18B20 | blad_czujnika=true | Globalny 5 min | NIE |
| Bateria <5% | bateria_procent < 5 | Globalny 5 min | NIE |
| Bateria <15% | bateria_procent < 15 | Globalny 5 min | NIE |
| Szybki wzrost | ΔT/Δh > prog_wzrost | Globalny 5 min | NIE |
| Brak sygnału | >3× interwal bez wiadomości | Globalny 5 min | NIE |

**System alertów temperaturowych** (v5.3):
- Alerty temp (max/min) mają własny per-satelita cooldown (`alert_cykl_s`, domyślnie 15 min)
- Cooldown niezależny od globalnego — błąd czujnika i bateria nie blokują alertów temp
- `/cichy_temp` wycisza alerty temp na 24h — błąd czujnika i bateria nadal aktywne
- Konfigurowalny z dashboardu: 5/10/15/30/60 min

**Heartbeat** (brak sygnału):
- Zasilaczowe (typ=2): timeout = 3× `interwal_zasil_s`, minimum 5 min
- Bateriowe (typ=1): timeout = 3× `interwal_s`, minimum 5 min

**Tryb cichy** (cichy_od → cichy_do):
- Alerty łagodne: blokowane
- Alerty krytyczne: zawsze przechodzą
- Np. `cichy_od=23, cichy_do=7` → blokada 23:00–07:00

### System konfiguracji (Preferences)

```cpp
Preferences prefs;

void wczytajPrefs() {
    prefs.begin("mleko", true);  // read-only dla tesowania
    
    prog_max = prefs.getFloat("prog_max", 8.0f);
    prog_min = prefs.getFloat("prog_min", 2.0f);
    interwal_s = prefs.getUInt("interwal_s", 1800);
    cichy_od = prefs.getUChar("cichy_od", 0);
    cichy_do = prefs.getUChar("cichy_do", 0);
    alert_cykl_s = prefs.getUInt("alert_cykl", 900); // 15 min domyślnie
}

void zapiszPrefs() {
    prefs.putFloat("prog_max", prog_max);
    prefs.putFloat("prog_min", prog_min);
    prefs.putFloat("prog_wzrost", prog_wzrost);
    prefs.putUInt("interwal", interwal_s);
    prefs.putUInt("interwal_zasil", interwal_zasil_s);
    prefs.putUChar("cichy_od", cichy_od);
    prefs.putUChar("cichy_do", cichy_do);
    prefs.putUInt("alert_cykl", alert_cykl_s);
}
```

---

## 7. REST API Matki

**Host**: `http://mleko.local` lub `http://IP` (fallback)  
**Port**: 80  
**Autoryzacja**: Brak (dostęp lokalny)

### Endpoints

#### GET `/`

Dashboard HTML (PROGMEM, single-file).

**Odpowiedź**: 200 OK, `text/html`

```html
<!DOCTYPE html>
<html>
<head>...</head>
<body>
  Kafelki czujników, wykresy, panel ustawień, upload OTA
</body>
</html>
```

#### GET `/api/status`

Pobierz status wszystkich satelit + systemowy.

**Odpowiedź**: 200 OK, `application/json`

```json
{
  "aktualny_czas": "2026-04-02 15:30:45",
  "ntp_zsynchronizowany": true,
  "ip": "192.168.1.100",
  "mdns": "mleko.local",
  "uptime_s": 86400,
  "fw_version": "4.1",
  "satelity": [
    {
      "id": 1,
      "typ": 1,
      "aktywna": true,
      "temperatura": 7.3,
      "bateria_procent": 85,
      "ostatni_pomiar": "15 min temu",
      "blad_czujnika": false,
      "fw_version": "2.6"
    },
    {
      "id": 2,
      "typ": 2,
      "aktywna": true,
      "temperatura": 6.8,
      "bateria_procent": 100,
      "ostatni_pomiar": "3 min temu",
      "blad_czujnika": false,
      "fw_version": "2.6"
    }
  ],
  "konfiguracja": {
    "prog_max": 8.0,
    "prog_min": 2.0,
    "interwal_s": 1800,
    "cichy_od": 23,
    "cichy_do": 7
  }
}
```

#### GET `/api/historia?id=1`

Pobierz ring buffer dla satelity ID=1 (48 wpisów, 24h).

**Parametry**:
- `id` (uint8): satelita 1–8, brak = wszystkie

**Odpowiedź**: 200 OK, `application/json`

```json
{
  "id": 1,
  "historia": [
    {"czas_unix": 1743667800, "temperatura": 6.5},
    {"czas_unix": 1743668700, "temperatura": 6.7},
    ...
  ]
}
```

#### GET `/api/ustawienia`

Pobierz aktualną konfigurację.

**Odpowiedź**: 200 OK, `application/json`

```json
{
  "prog_max": 8.0,
  "prog_min": -99.0,
  "prog_wzrost": 2.0,
  "interwal_min": 30,
  "interwal_zasil_min": 5,
  "alert_cykl_min": 15,
  "cichy_od": 0,
  "cichy_do": 0
}
```

#### POST `/api/ustawienia`

Zaktualizuj konfigurację.

**Body**: `application/json`

```json
{
  "prog_max": 8.5,
  "prog_min": -99.0,
  "interwal_zasil_min": 5,
  "alert_cykl_min": 10,
  "cichy_od": 22,
  "cichy_do": 8
}
```

**Odpowiedź**: 200 OK, `application/json`

```json
{
  "success": true,
  "message": "Konfiguracja zaktualizowana"
}
```

#### POST `/api/wifi-reset`

Usuń saved WiFi credentials, restart do AP mode.

**Body**: (pusty)

**Odpowiedź**: 200 OK, `application/json`

```json
{
  "message": "WiFi reset, restarting to AP mode..."
}
```

Matka restartuje się do trybu AP (Smart_Mleko_Setup).

#### POST `/ota/matka`

Multipart upload firmware Matki.

**Content-Type**: `multipart/form-data`  
**Form field**: `file` (binary .bin)

**Odpowiedź**: 200 OK, `text/plain`

```
OK
```

Matka natychmiast flashuje i restartuje.

#### POST `/ota/satelita/begin?size=XXXXX`

Rozpocznij sesję chunked upload dla Satelity.

**Parametry**:
- `size` (uint32): całkowity rozmiar .bin

**Akcja**: Alloc `ota_buf` (PSRAM) na `size` bajtów

**Odpowiedź**: 200 OK, `application/json`

```json
{
  "session_id": "xyz",
  "size": 262144,
  "chunk_size": 8192,
  "expected_chunks": 32
}
```

#### POST `/ota/satelita/chunk`

Wyślij 8KB chunk.

**Content-Type**: `application/octet-stream`  
**Body**: 8192 bajtów (raw binary)

**Akcja**: `memcpy(ota_buf + offset, data, len)`, `offset += len`

**Odpowiedź**: 200 OK, `application/json`

```json
{
  "received": 8192,
  "offset": 8192,
  "remaining": 254000
}
```

#### POST `/ota/satelita/finish`

Finalizuj upload.

**Akcja**:
- Waliduj: `offset == size`
- Ustaw `ota_write_pending = true`
- W `loop()`: zapisz PSRAM → LittleFS `/ota/satelita.bin` (8KB porcje)
- Ustaw `ota_pending = true` dla WSZYSTKICH satelit

**Odpowiedź**: 200 OK, `application/json`

```json
{
  "success": true,
  "message": "Firmware validated. OTA pending for all satellites."
}
```

#### GET `/ota/satelita.bin`

Serwuj firmware dla Satelity.

**Źródło**: PSRAM (jeśli ota_buf), inaczej LittleFS `/ota/satelita.bin`

**Odpowiedź**: 200 OK, `application/octet-stream`

```
[binary firmware data]
```

---

## 8. Komendy Telegram

**Bot API**: Polling co 10 sekund  
**Chat**: Grupa Telegram ID: `TELEGRAM_GROUP_ID` (z secrets.h)

### Komenda: `/status`

Temperaturę ostatniego pomiaru + bateria + czas pomiaru.

```
Satelita #1 (bateria):
  Temperatura: 7.3°C
  Bateria: 85%
  Ostatni pomiar: 3 min temu
  Status: OK ✓
```

### Komenda: `/historia`

Ostatnie 10 pomiarów.

```
Satelita #1 historia (24h):
  [15:30] 7.3°C
  [15:00] 7.1°C
  ...
```

### Komenda: `/srednia`

Średnia temperatura 24h, min, max.

```
Satelita #1 statystyka 24h:
  Średnia: 7.0°C
  Min: 5.8°C
  Max: 8.2°C
```

### Komenda: `/raport`

Pełny raport na żądanie.

```
=== RAPORT SMART MLEKO ===
Czas: 2026-04-02 15:30:45
Uptime Matki: 30d 12h

Satelita #1 (WEMOS D1, bateria):
  Temperatura: 7.3°C
  Bateria: 85%
  Ostatni pomiar: 3 min temu
  Status: OK ✓

Satelita #2 (C3 Mini, zasilacz):
  Temperatura: 6.8°C
  Bateria: 100%
  Ostatni pomiar: 1 min temu
  Status: OK ✓

Konfiguracja:
  Alarm max: 8.0°C
  Alarm min: 2.0°C
  Interwał: 30 min
  Tryb cichy: 23:00–07:00
```

### Komenda: `/set_max <float>`

Ustaw próg alarmu max.

```
/set_max 8.5
✓ Próg max zmieniony na 8.5°C
```

### Komenda: `/set_min <float>`

Ustaw próg alarmu min.

```
/set_min 1.5
✓ Próg min zmieniony na 1.5°C
```

### Komenda: `/interwal <min>`

Zmień interwał pomiaru (minuty).

```
/interwal 45
✓ Interwał zmieniony na 45 min
  Zmiana wejdzie w życie po budzeniu Satelity
```

### Komenda: `/cichy <od> <do>`

Ustaw tryb cichy (godziny 0–23).

```
/cichy 23 7
✓ Tryb cichy: 23:00–07:00
  Alerty łagodne blokowane, krytyczne zawsze

/cichy 0 0
✓ Tryb cichy: wyłączony (alerty zawsze)
```

### Komenda: `/cichy_temp`

Wycisz alerty temperaturowe (max/min) na 24h. Błąd czujnika i bateria nadal aktywne.

```
/cichy_temp
🔕 Alerty temp wyciszone na 24h.
  Błąd czujnika i bateria nadal aktywne.
  Odwołaj: /cichy_temp off

/cichy_temp off
✅ Alerty temperaturowe wznowione.
```

### Komenda: `/ustawienia`

Pokaż aktualną konfigurację.

```
Aktualna konfiguracja:
  Alarm max: 8.0°C
  Alarm min: 2.0°C
  Interwał: 30 min
  Tryb cichy: 23:00–07:00
```

### Komenda: `/wifi-reset`

Usuń saved WiFi, restart do AP mode.

```
/wifi-reset
⚠ WiFi reset! Matka restartuje do AP mode.
  Połącz się z "Smart_Mleko_Setup" i przejdź do captive portalu.
```

### Komenda: `/pomoc`

Pokaż listę komend.

```
Dostępne komendy:
  /status       — status ostatniego pomiaru
  /historia     — ostatnie 10 pomiarów
  /srednia      — średnia 24h
  /raport       — pełny raport
  /set_max <C>  — ustaw próg max
  /set_min <C>  — ustaw próg min
  /interwal <m> — zmień interwał (minuty)
  /cichy <h> <h>  — tryb cichy (godziny)
  /cichy_temp     — wycisz alerty temp na 24h
  /cichy_temp off — wznów alerty temp
  /ustawienia     — pokaż config
  /wifi-reset     — reset WiFi
  /pomoc          — ta lista
```

---

## 9. System OTA

### Problem do rozwiązania

ESPAsyncWebServer + LittleFS = **flash bus contention** podczas WiFi RX → dane gubione, watchdog reset.

**Rozwiązanie**: Upload do PSRAM (nie flash), write w `loop()` asynchronicznie.

### OTA Upload (Browser → Matka → LittleFS)

```
┌─────────────────┐
│ Browser         │
│ mleko.local     │
└────────┬────────┘
         │
         │ POST /ota/satelita/begin?size=262144
         ↓
┌─────────────────────────────────┐
│ Matka                           │
│ ps_malloc(262144) → ota_buf     │
│ (PSRAM, nie flash!)             │
└────────┬────────────────────────┘
         │
         │ POST /ota/satelita/chunk (8KB, raw binary) ×32
         │ memcpy(ota_buf + offset, chunk, 8192)
         ↓
┌─────────────────────────────────┐
│ Matka                           │
│ ota_buf = [pełny firmware]      │
│ ota_write_pending = true        │
└────────┬────────────────────────┘
         │
         │ POST /ota/satelita/finish
         │ validate(offset == size)
         ↓
┌─────────────────────────────────┐
│ loop() (nie async callback)     │
│ if (ota_write_pending):         │
│   for i = 0..size step 8192:    │
│     write ota_buf[i:i+8192] →   │
│     LittleFS /ota/satelita.bin  │
│     delay(1)  ← wichtig!        │
│   ota_pending = true (wszystkie)│
│   wyslij ACK z flagą            │
└─────────────────────────────────┘
         │
         │ GET /ota/satelita.bin
         │ (Satelita pobiera z Matki)
         ↓
```

### OTA Download (Satelita ← Matka ← WiFi)

```
Satelita:
1. Odbierz ACK: ota_pending=true, ota_url, wifi_ssid, wifi_pass
2. esp_now_deinit()  ← wyłącz ESP-NOW
3. WiFi.begin(ssid, pass)  ← włącz WiFi
4. HTTPClient.GET(ota_url)  ← pobierz .bin (PEŁNY plik, bez Range!)
5. Update.begin(size) → Update.writeStream(data) → Update.end(true)
6. ESP.restart()  ← wznów ESP-NOW

Powód brak Range: ESPAsyncWebServer NIE obsługuje Range requests.
```

### OTA Flow — Szczegółowo

#### 1. Upload firmware z dashboardu

Browser: `form.append('file', blobFirmware)`  
POST `/ota/matka` (OTA Matki) lub chunked `/ota/satelita/*` (OTA Satelity)

#### 2. Matka: alloc + write do PSRAM

```cpp
// onRequest GET /ota/satelita/begin?size=XXXXX
ps_malloc(size) → ota_buf
ota_buf_size = size
ota_buf_offset = 0

// onRequest POST /ota/satelita/chunk
memcpy(ota_buf + ota_buf_offset, data, len)
ota_buf_offset += len

// onRequest POST /ota/satelita/finish
if (ota_buf_offset == ota_buf_size) {
    ota_write_pending = true  ← flaga dla loop()
}
```

#### 3. loop() (nie callback!): write PSRAM → LittleFS

```cpp
if (ota_write_pending) {
    File f = LittleFS.open("/ota/satelita.bin", "w");
    for (size_t i = 0; i < ota_buf_size; i += 8192) {
        size_t chunk = min(8192, ota_buf_size - i);
        f.write(ota_buf + i, chunk);
        delay(1);  ← ważne!
    }
    f.close();
    
    ota_write_pending = false;
    
    // Ustaw flagę dla wszystkich satelit
    for (int i = 0; i < ile_satelit; i++) {
        satelity[i].ota_pending = true;
    }
}
```

**Dlaczego PSRAM?** LittleFS czyta plik blokami 4096B, przy granicy 186 (761856 bajtów) ESPAsyncWebServer timeout >30s. Z PSRAM = streaming bezpośrednio z RAM.

#### 4. Satelita: OTA download

```cpp
// ACK: ota_pending=true, ota_url, wifi_ssid, wifi_pass
void wykonajOTA(const char *url, const char *ssid, const char *pass) {
    esp_now_deinit();
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    
    // Wait for WiFi
    int proby = 0;
    while (WiFi.status() != WL_CONNECTED && proby < 30) {
        delay(500);
        proby++;
    }
    
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect();
        ESP.restart();  ← back to ESP-NOW
        return;
    }
    
    // Download (pełny plik, brak Range)
    HTTPClient http;
    http.begin(url);
    int httpCode = http.GET();
    
    if (httpCode == 200) {
        int totalSize = http.getSize();
        WiFiClient *stream = http.getStreamPtr();
        
        if (Update.begin(totalSize)) {
            Update.writeStream(*stream);
            if (Update.end(true)) {
                ESP.restart();  ← back to ESP-NOW
            }
        }
    }
    
    http.end();
    WiFi.disconnect();
    ESP.restart();
}
```

#### 5. Reset flagi OTA

Po restarcie Satelita wysyła nową wiadomość. Matka odbiera i resetuje flagę:

```cpp
void onDataRecv(...) {
    // ...
    if (s->ota_pending && s->ota_url_wyslany) {
        s->ota_pending = false;
        s->ota_url_wyslany = false;  ← czyść flagę
    }
}
```

### OTA Matki (bezpośrednio)

```cpp
// POST /ota/matka (multipart)
server.on("/ota/matka", HTTP_POST, [](AsyncWebServerRequest *req) {
    if (!Update.hasError()) {
        req->send(200, "text/plain", "OK");
        delay(500);
        ESP.restart();  ← natychmiast
    } else {
        req->send(500, "text/plain", "BLAD");
    }
}, [](AsyncWebServerRequest *req, String filename, 
      size_t index, uint8_t *data, size_t len, bool final) {
    if (!index) Update.begin(UPDATE_SIZE_UNKNOWN);
    Update.write(data, len);
    if (final) Update.end(true);
});
```

### Walidacja firmware

NIE ma! Ufamy że użytkownik wysyła poprawny `.bin`.

W przyszłości: SHA256 checksum w payload lub versioning.

---

## 10. Konfiguracja i build

### Wymagania

- **PlatformIO**: latest version
- **Arduino SDK**: esp32 v3.x
- **Python**: esptool.py (do flashu Satelity na Pi)

### Struktura katalogów

```
esp32-mleko/
├── CLAUDE.md                  ← instrukcje dla Claude Code
├── smart_mleko_v8.md         ← specyfikacja projektu
├── docs/
│   └── DOKUMENTACJA_TECHNICZNA.md  ← ten plik
│
├── matka/
│   ├── platformio.ini
│   ├── src/
│   │   ├── main.cpp           ← cały firmware (single-file)
│   │   └── secrets.h          ← WiFi/Telegram (gitignored!)
│   └── data/                  ← LittleFS (jeśli potrzebny)
│
└── satelita/
    ├── platformio.ini         ← 1 environment: esp32c3_zas
    ├── src/
    │   └── main.cpp           ← firmware satelity (C3 only)
    ├── firmware.bin           ← output dla OTA (auto-kopiowany po build)
    └── copy_firmware.py       ← copy build → firmware.bin
```

### Build Matki

```bash
cd matka

# Compile
pio run

# Upload (naciśnij BOOT + EN)
# Port zmienia się po resecie — sprawdź: ls /dev/cu.usb*
pio run -t upload

# Serial monitor
pio device monitor -b 115200
```

**Port domyślny**: `/dev/cu.usbmodem212401` (zmienia się!)

### Build Satelity

```bash
cd satelita

# Compile (jedyny env)
pio run -e esp32c3_zas

# Plik output: .pio/build/esp32c3_zas/firmware.bin
# copy_firmware.py automatycznie kopie do: satelita/firmware.bin
# Ten plik używany do OTA przez dashboard Matki
```

### Flash Satelity (USB-C, pierwsze uruchomienie lub reset ID)

```bash
cd satelita
pio run -t upload -e esp32c3_zas
```

C3 Super Mini flash bezpośrednio przez USB-C do Maca. Przed flashem zmień `DEFAULT_ID` w `main.cpp` jeśli to nie Satelita #1.

### Build flags

**Matka** (`matka/platformio.ini`):
```ini
build_flags =
    -DARDUINO_USB_CDC_ON_BOOT=1
    -DBOARD_HAS_PSRAM
```

**Satelita** (`satelita/platformio.ini`):
```ini
[env:esp32c3_zas]
board = esp32-c3-devkitm-1
build_flags =
    -DPLATFORM_C3
    # typ=2 (zasilacz), GPIO4 DS18B20
```

### Partition scheme

**Matka**: `default_16MB.csv` (16MB flash, OTA dual boot)  
**Satelita**: `min_spiffs.csv` (OTA support)

Ustawiać PRZED pierwszym flashem!

```bash
# Sprawdź w Arduino IDE:
# Tools → Partition Scheme → "Minimal SPIFFS"
```

### Sekrety (secrets.h)

`matka/src/secrets.h` (gitignored):

```cpp
#ifndef SECRETS_H
#define SECRETS_H

#define DEFAULT_WIFI_SSID "twoj_SSID"
#define DEFAULT_WIFI_PASS "twoje_haslo"

#define TELEGRAM_BOT_TOKEN "123456:ABC-DEF1234..."
#define TELEGRAM_GROUP_ID 123456789

#endif
```

Wygeneruj bot Telegram: `/newbot` u `@BotFather`

---

## 11. Troubleshooting OTA

### Problem: "SPIFFS error: CRC mismatch"

**Przyczyna**: Przerwany upload lub zła partycja.

**Rozwiązanie**:
1. Ustaw partition scheme: `Tools → Partition Scheme → "With OTA"`
2. Flashuj od nowa (erase flash):
   ```bash
   pio run -t erase
   pio run -t upload
   ```
3. Jeśli dalej: zmień USB kabel, port, zasilacz.

### Problem: "Update error: Magic byte mismatch"

**Przyczyna**: Upload uszkodzony lub nie-ESP32 firmware.

**Rozwiązanie**:
1. Sprawdź że wysyłasz `.pio/build/esp32/firmware.bin` (nie inny)
2. Skompiluj od nowa:
   ```bash
   pio run --target clean
   pio run
   ```
3. Spróbuj multipart upload (POST chunked) zamiast single.

### Problem: "OTA timeout, no ACK"

**Przyczyna**: Satelita nie słyszy Matki lub zły kanał.

**Rozwiązanie**:
1. Sprawdź kanał WiFi routera (musi być 1–13)
2. Matka musi być w sieci WiFi (channel hopping zależy od routera)
3. Debuguj serial output Satelity:
   ```bash
   ssh admin@pi.local
   stty -F /dev/ttyUSB0 115200 raw
   cat /dev/ttyUSB0
   ```
4. Sprawdź ostatni_kanal w RTC RAM (hint).

### Problem: "HTTP 404: /ota/satelita.bin"

**Przyczyna**: Firmware nie zapisany w LittleFS.

**Rozwiązanie**:
1. Sprawdź `ota_write_pending` flag w Matce
2. Upewnij się że upload finished (POST `/ota/satelita/finish`)
3. Sprawdź rozmiar: `GET /api/status` → czy `ota_buf_offset == ota_buf_size`
4. Jeśli nie: spróbuj upload ponownie

### Problem: "WiFi credentials brak podczas OTA"

**Przyczyna**: ACK wysłane bez SSID/PASS.

**Rozwiązanie**:
1. Matka musi być w WiFi STA (nie AP)
2. Sprawdź `WiFi.SSID()` i `WiFi.psk()` zwracają niepuste
3. Debug: dodaj Serial.printf w wyslijACK()

### Problem: "Satelita restartuje w pętli podczas OTA"

**Przyczyna**: Update.begin() faile albo Update.write() overflow.

**Rozwiązanie**:
1. Sprawdź rozmiar .bin: `ls -lh satelita/firmware.bin`
2. Porównaj z `totalSize` z HTTP header
3. Sprawdzić czy Satelita ma dość RAM na Update buffer
4. Debug serial Satelity podczas OTA

### Problem: "PSRAM full, OTA buffer allocation failed"

**Przyczyna**: OTA firmware > 8 MB PSRAM.

**Rozwiązanie**:
1. Zmniejsz firmware (optimize code)
2. Lub użyj LittleFS bezpośrednio (ryzyko: flash bus contention)
3. Sprawdź czy oto_buf się nie leakuje w poprzednim OTA

### Problem: "Satelita nie budzi się po OTA"

**Przyczyna**: RTC RAM zresetowana (zwłaszcza po power off).

**Rozwiązanie**:
1. Sprawdzić resetowania podczas OTA (power off?)
2. Po restarcie: Satelita próbuje kanału z RTC RAM (może być stary)
3. Channel hopping fallback: spróbuje wszystkie kanały 1–13
4. Jeśli Matka w AP mode: Satelita nie ją znajdzie — najpierw Matka do WiFi!

### Problem: "Dashboard nie widzi firmware po uploadzie"

**Przyczyna**: GET `/ota/satelita.bin` czyta z LittleFS, write zawiódł.

**Rozwiązanie**:
1. Sprawdzić `ota_write_pending` → czy kончи write w loop()
2. Debug serial: `[FS] Write OK` czy `[FS] Write FAIL`
3. Sprawdzić LittleFS free space: `LittleFS.totalBytes()`

---

## 12. Jak zacząć po przerwie

### Szybki start (10 min)

1. **Sprawdź aktualny stan**:
   ```bash
   cd ~/Documents/esp32-mleko
   git log -5 --oneline
   git status
   ```
   
2. **Przeczytaj ostatnie commits**:
   ```bash
   git show HEAD
   ```
   
3. **Zapoznaj się z kontekstem**:
   - `CLAUDE.md` — instrukcje dla Claude Code
   - `smart_mleko_v8.md` — specyfikacja całego projektu
   - `docs/DOKUMENTACJA_TECHNICZNA.md` — ten plik (dla zagłębienia się)

4. **Build i test**:
   ```bash
   cd matka && pio run
   cd ../satelita && pio run -e esp32c3_zas
   ```

### Etapy pracy

#### Etap 1: Hardware setup
- [ ] Ustaw `DEFAULT_ID` w `satelita/src/main.cpp` (domyślnie 1)
- [ ] Podłącz DS18B20 do GPIO4, 3.3V, GND + rezystor 4.7kΩ pull-up

#### Etap 2: Upload initial firmware
- [ ] Build Matki: `cd matka && pio run -t upload` (BOOT+EN)
- [ ] Build Satelity: `cd satelita && pio run -t upload -e esp32c3_zas`
- [ ] Serial monitor Matki: `pio device monitor`

#### Etap 3: First communication
- [ ] Monitoruj Matka serial: `pio device monitor`
- [ ] Monitoruj Satelita serial: `ssh pi.local...`
- [ ] Wyślij pomiaru Satelity: powinny widzieć się wzajemnie
- [ ] Sprawdź ACK w obóg stron

#### Etap 4: Konfiguracja WiFi i OTA
- [ ] Matka: create `matka/src/secrets.h` z WiFi i Telegram
- [ ] `pio run -t uploadfs` (jeśli potrzebny LittleFS)
- [ ] Dashboard: `http://mleko.local` → powinien być dostępny

#### Etap 5: OTA upload
- [ ] Dashboard: upload `satelita/firmware.bin` (OTA dla Satelity)
- [ ] Satelita pobierze firmware i zrestartuje się automatycznie
- [ ] Sprawdź wersję w dashboardzie po restarcie

#### Etap 6: Telegram + alerty
- [ ] `/status` → temperatura + typ zasilania
- [ ] `/set_max 8.5` → zmień próg
- [ ] Zagrzej czujnik (dłoń) → sprawdź alert Telegram

### Checklist przed wdrożeniem u teścia

- [ ] Wszystkie komunikaty po polsku
- [ ] WiFiManager + AP mode: `Smart_Mleko_Setup`
- [ ] Dashboard responsive (mobile-friendly)
- [ ] Telegram bot: wszystkie komendy
- [ ] OTA dla Matki i Satelity: test end-to-end
- [ ] Alerty Telegram: test wszystkich typów (nie spam podczas channel-hopping!)
- [ ] Tryb cichy: test blokowania łagodnych
- [ ] NTP sync: test zmiany czasu (lato/zima)
- [ ] Sonda DS18B20: test pod wodą
- [ ] Zasięg ESP-NOW: test w rzeczywistym otoczeniu
- [ ] Dokumentacja dla teścia: instrukcja obsługi

### Debugowanie w terenie

```bash
# Na Matce (USB):
pio device monitor -b 115200 | grep -E "\[ESP-NOW\]|\[ALERT\]|\[OTA\]"

# Na Satelicie (Raspberry Pi):
ssh admin@pi.local "stty -F /dev/ttyUSB0 115200 raw && cat /dev/ttyUSB0" | grep -E "\[CH\]|\[OTA\]|\[ESP-NOW\]"

# Dashboard API:
curl http://mleko.local/api/status | jq .

# Telegram log:
# /raport — full dump z alertami
```

### Git workflow (dobrze mieć)

```bash
# Przed zmianami
git checkout main
git pull

# Po zmianach
git add -A
git commit -m "Satelita: fix DS18B20 warmup"
git push

# Powrót do stanego stanu (jeśli coś się sypie)
git reset --hard HEAD~1
```

---

## 13. Znane problemy i TODO

### Problemy w trakcie debugowania

| Problem | Status | Notatka |
|---------|--------|---------|
| MAC Matki hardcoded | KNOWN | `satelita/src/main.cpp:36` `adresMatki[]` — zmienić gdy wymiana Matki |
| Zmiana kanału WiFi | SOLVED | Channel hopping 1–13, 3 próby/kanał, hint RTC RAM |
| OTA timeout LittleFS | SOLVED | Serwowanie z PSRAM (nie LittleFS) |
| Telegram spam podczas channel-hopping | SOLVED v5.1 | Sonda wykrywana po temp=0+bat=0+blad=true |
| Dashboard migotanie | SOLVED v5.2 | renderCzujniki() tylko gdy dane się zmienią (satKey bez czasów) |
| ACK opóźniony przez Telegram HTTP | SOLVED v5.2 | ACK wysyłany z onDataRecv callback, nie loop() |
| Alert temp spam (co 5 min przez całą dobę) | SOLVED v5.3 | Per-satelita cooldown `alert_cykl_s` (domyślnie 15 min) |
| Heartbeat fałszywy alarm (zasilaczowa) | SOLVED v5.3 | Timeout = 3× interwal_zasil_s (było hardcoded 5 min = interwał) |
| mDNS na starym Androidzie | KNOWN | Fallback na IP w dashboardzie |

### TODO do następnych etapów

- [ ] Druhgy DS18B20 na jednym Satelicie (1-Wire obsługuje)
- [ ] MQTT + Home Assistant integration
- [ ] Raport automatyczny o ustalonej godzinie (np. codziennie o 20:00)
- [ ] Powiadomienia push alternatywne (push.service, IFTTT)
- [ ] Kalibracja DS18B20 na Dashboard
- [ ] Wersjonowanie firmware + changelog
- [ ] Przechowywanie historii dłużej niż 48h (SD card?)
- [ ] Synchnronizacja czasu między Satelitami (dla dokładnych pomiarów)
- [ ] Alert na SMS (jeśli no internet, tylko modem)

---

## Dodatek A: Stałe i Macro

### Satelita

```cpp
#define SAT_FW_VERSION "2.6"

// Hardware
#define PIN_DS18B20 4              // GPIO4 (WEMOS), GPIO2 (C3)
#define PIN_ADC_BATERIA 34         // GPIO34 (WEMOS only)
#define WSPOLCZYNNIK_ADC 2.0f      // zmierzyć multimetrem!

// Timing
#define INTERWAL_DOMYSLNY_S 1800   // 30 minut
#define TIMEOUT_ACK_MS 2000        // czekaj ACK

// MAC Matki
uint8_t adresMatki[] = {0x80, 0xB5, 0x4E, 0xC3, 0x3C, 0xB8};
```

### Matka

```cpp
#define FW_VERSION "4.1"

// WiFi AP
#define AP_SSID "Smart_Mleko_Setup"
#define AP_TIMEOUT_MS 180000       // 3 min

// NTP
#define NTP_STREFA "CET-1CEST,M3.5.0,M10.5.0/3"

// System
#define MAX_SATELITY 8
#define MAX_HISTORIA_PER 48        // 24h co 30min

// Telegram
const unsigned long TELEGRAM_COOLDOWN = 300000;  // 5 min
const unsigned long POLL_INTERVAL = 10000;      // 10s
```

---

## Dodatek B: Urządzenia w projekcie

### Urządzenia wdrożone u teścia

| Lp | Urządzenie | Typ | ID | Funkcja |
|----|-----------|----|-----|---------|
| 1 | Matka | ESP32-S3 N16R8 | — | Hub, Dashboard, Telegram |
| 2 | Satelita | WEMOS D1 (lolin_d32) | 1 | Monitowanie mleka (schładzalnik) |

### Plany rozszerzenia

- Satelita #2 (zasilacz, ESP32-C3 Super Mini) — inny punkt monitorowania
- Satelita #3–8 — jeśli rozszerzy się system

---

## Dodatek C: Dokumenty powiązane

| Dokument | Zawartość |
|----------|-----------|
| `CLAUDE.md` | Instrukcje dla Claude Code, schemat architektur, build |
| `smart_mleko_v8.md` | Specyfikacja projektowa, BOM, schemat, roadmap |
| `docs/DOKUMENTACJA_TECHNICZNA.md` | **Ten plik** — szczegółowe API, OTA, troubleshooting |

---

## Dodatek D: Historia zmian

| Data | Wersja | Zmiany |
|------|--------|--------|
| 2026-04-02 | 1.0 | Pierwsza wersja doc techniczna |
| — | — | — |

---

## Dodatek E: Kontakty i referencje

**Autor projektu**: teść  
**Deweloper**: Claude Code  
**Ostatnia aktualizacja**: 2026-04-02

**Telegram Bot API**: https://core.telegram.org/bots/api  
**ESPAsyncWebServer**: https://github.com/me-no-dev/ESPAsyncWebServer  
**ArduinoJson**: https://arduinojson.org  
**ESP32 Arduino SDK**: https://docs.espressif.com/projects/arduino-esp32  
**DS18B20 Datasheet**: https://datasheets.maximintegrated.com/en/ds/DS18B20.pdf

---

**Koniec dokumentacji.**

*Dokument ma być źródłem prawdy dla wszystkich prac technicznych nad Smart Mleko.*  
*Wszystkie decyzje architektoniczne są zatwierdzone i nie ulegają zmianom bez dokumentu Update.*

