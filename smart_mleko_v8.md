# Projekt „Smart Mleko / Smart Gospodarstwo" 🥛📡
**Wersja 8.0 — Plik roboczy do Claude Code**

Skalowalny, energooszczędny system monitorowania temperatury mleka oparty na ESP-NOW (topologia gwiazdy), z powiadomieniami Telegram (czat grupowy), lokalnym Dashboardem WWW pod adresem `mleko.local`, automatyczną konfiguracją Wi-Fi, monitoringiem baterii, alertem szybkiego wzrostu temperatury i OTA dla obu urządzeń.

---

## 1. Architektura systemu

```
[ Satelita: przy mleku ]
  ESP32-S3 + 18650 2500mAh + DS18B20
  sonda zanurzona w płynie
         |  ESP-NOW (dwukierunkowy: dane + ACK + konfiguracja + OTA flag)
         ↓
[ Matka: w domu, 24/7 na prądzie ]
  ESP32-S3 N16R8, Dual USB-C, zasilacz 5V/1A
  Czas: NTP (Europe/Warsaw), synchronizacja po starcie
  Dostępna pod: http://mleko.local
         |
  ┌──────┼──────────────┐
  ↓      ↓              ↓
Telegram  Dashboard WWW  Heartbeat
(grupowy) (mleko.local)  watchdog
```

### Podział ról

| Cecha | Satelita | Matka |
|---|---|---|
| Zasilanie | Bateria 18650 2500 mAh | Stałe — zasilacz 5V/1A USB-C |
| Montaż czujnika | Sonda DS18B20 zanurzona w mleku | — |
| Łączność | ESP-NOW (dwukierunkowy) | ESP-NOW + Wi-Fi |
| Tryb pracy | Deep Sleep, domyślnie co 30 min | Aktywna 24/7 |
| Czas pracy | ~150 dni (30 min interwał, 20°C) | bezterminowo |
| Adres lokalny | — | `http://mleko.local` |

---

## 2. Lista zakupów (BOM)

### Satelita

| Komponent | Opis | AliExpress |
|---|---|---|
| Mikrokontroler | WEMOS D1 ESP32-S3 z koszykiem 18650 i ładowarką | `WEMOS D1 ESP32-S3 18650` |
| Bateria | Li-Ion 18650, 2500 mAh | — |
| Czujnik | DS18B20 wodoodporny, metalowa sonda na kablu | `DS18B20 waterproof` |
| Rezystor | 4,7 kΩ pull-up | — |
| Obudowa 3D | MakerWorld: `Wemos Lolin32 18650 case` | — |

**Montaż sondy:** DS18B20 w stalowej gilzie bezpieczny do kontaktu z żywnością. Kabel przez dławik, uszczelniony silikonem spożywczym. Elektronika powyżej poziomu mleka.

### Matka

| Komponent | Opis | AliExpress |
|---|---|---|
| Mikrokontroler | ESP32-S3 N16R8 DevKitC Dual USB-C | `ESP32-S3 N16R8 DevKitC USB-C` |
| Zasilanie | Ładowarka 5V/1A (markowa) + kabel USB-C | — |
| Obudowa 3D | MakerWorld: `ESP32-S3 DevKitC case` | — |

**Zasilacz:** każda markowa ładowarka 5V/1A lub wyższa (Samsung, Xiaomi, Anker). Tanie podróbki bez certyfikatów mogą powodować losowe resety ESP32 przez niestabilne napięcie.

---

## 3. Schemat podłączenia DS18B20

```
ESP32-S3 3.3V ──────┬──── [4,7 kΩ] ────┐
                    │                   │
ESP32-S3 GPIO4 ─────┼───────────────────┤→ DS18B20 DATA (żółty)
                    │
ESP32-S3 GND ───────────────────────── DS18B20 GND (czarny)
                                        DS18B20 VCC (czerwony) ← 3.3V
```

- Zasilać z **3.3V** — nie z 5V
- Bez rezystora 4,7 kΩ odczyt zwraca **-127°C** (błąd 1-Wire)

---

## 4. Struktury danych ESP-NOW

```cpp
// Satelita → Matka
typedef struct struct_message {
    uint8_t  id_czujnika;       // 1 = mleko (rozszerzalne)
    float    temperatura;
    uint8_t  bateria_procent;
    uint32_t timestamp;
    bool     blad_czujnika;     // true = DS18B20 zwrócił -127°C
} struct_message;

// Matka → Satelita (ACK)
typedef struct struct_ack {
    uint32_t nowy_interwał_s;   // 0 = bez zmian
    uint8_t  godzina_start;     // okno pomiaru od (0–23)
    uint8_t  godzina_stop;      // okno pomiaru do (0–23); 0,0 = 24h
    bool     ota_pending;       // true = zostań aktywny, czeka aktualizacja
    char     ota_url[64];       // "http://mleko.local/ota/satelita.bin"
} struct_ack;
```

---

## 5. Oprogramowanie — Satelita

### Logika główna

```
[Budzenie z Deep Sleep]
    ↓
[Sprawdź okno godzinowe (RTC RAM)]
    ├── Poza oknem → wróć do snu do początku okna
    └── W oknie →
            [Pomiar "rozgrzewkowy" DS18B20 — odrzuć wynik]
            [Właściwy odczyt DS18B20, max 3 próby]
            [Odczyt ADC baterii → %]
            [Wyślij paczkę ESP-NOW]
            [Czekaj na ACK, max 500 ms]
                ├── ACK: ota_pending = true →
                │       Pobierz .bin z ota_url (HTTP)
                │       Flashuj → restart
                ├── ACK: nowe ustawienia →
                │       Zapisz w RTC RAM
                │       Deep Sleep (nowy interwał)
                ├── ACK OK → Deep Sleep
                └── Brak ACK → Channel Hopping (kanały 1–13)
                                    ├── Znalazł → zapisz → Deep Sleep
                                    └── Nie znalazł → Deep Sleep
```

**Ważne:** DS18B20 przy pierwszym pomiarze po budzeniu zwraca niekiedy starą wartość z bufora. Zawsze wykonaj jeden pomiar "na rozgrzewkę" i odrzuć go — dopiero drugi wysyłaj.

### Okno godzinowe pomiaru

```cpp
RTC_DATA_ATTR uint8_t g_start = 0;
RTC_DATA_ATTR uint8_t g_stop  = 0;

bool czyWOknie() {
    if (g_start == 0 && g_stop == 0) return true;  // tryb 24h
    uint8_t h = rtc_godzina();
    if (g_start < g_stop) return h >= g_start && h < g_stop;
    return h >= g_start || h < g_stop;  // okno przez północ np. 22–6
}
```

Zmiana interwału lub okna wchodzi w życie po maksymalnie jednym pełnym starym interwale.

### OTA Satelity

```cpp
void wykonajOTA(const char* url) {
    HTTPClient http;
    http.begin(url);
    if (http.GET() == 200) {
        int rozmiar = http.getSize();
        WiFiClient* strumien = http.getStreamPtr();
        if (Update.begin(rozmiar)) {
            Update.writeStream(*strumien);
            if (Update.end() && Update.isFinished()) ESP.restart();
        }
    }
}
```

**Koszt OTA:** jednorazowo ~45 sekund i ~10 mAh (0,4% baterii). W normalnym cyklu bez flagi — zero kosztu.

### Channel Hopping

```cpp
RTC_DATA_ATTR int8_t kanal_rtc = 1;

void znajdzKanal() {
    for (int k = 1; k <= 13; k++) {
        esp_wifi_set_channel(k, WIFI_SECOND_CHAN_NONE);
        if (wyslij_i_czekaj_ack()) { kanal_rtc = k; return; }
    }
}
```

### Odczyt baterii

```cpp
// ⚠️ PIN i współczynnik zależą od konkretnej płytki — sprawdzić przed kodowaniem
float napiecie = analogRead(PIN_ADC_BATERIA) * (3.3f / 4095.0f) * WSPOLCZYNNIK_ADC;
int procent = constrain((int)((napiecie - 3.0f) / 1.2f * 100.0f), 0, 100);
```

### Zabezpieczenia

- DS18B20 zwraca -127°C → `blad_czujnika = true`, wyślij z flagą
- Skok temperatury >20°C między pomiarami → wyślij poprzednią + flagę
- Watchdog Timer 8 s — auto-restart przy zawieszeniu

---

## 6. Oprogramowanie — Matka

### Osiem zadań równolegle

1. Nasłuch ESP-NOW + odsyłanie ACK z konfiguracją
2. WiFiManager — portal konfiguracji sieci
3. NTP — synchronizacja czasu po starcie (strefa Europe/Warsaw)
4. mDNS — rozgłaszanie `mleko.local`
5. Serwer WWW — Dashboard + API REST + upload OTA
6. Bot Telegram — czat grupowy, alerty + komendy
7. Heartbeat watchdog — alert gdy Satelita milczy >2h
8. OTA własna — aktualizacja Matki przez Dashboard

### Czas — NTP

```cpp
#include <time.h>

void synchronizujCzas() {
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.google.com");
    struct tm info;
    int próby = 0;
    while (!getLocalTime(&info) && próby++ < 10) delay(500);
    // Jeśli NTP niedostępny — ESP32 startuje od 1970-01-01
    // Tryb cichy i okno godzinowe nie będą działać poprawnie bez czasu
    // Warto wysłać alert Telegram: "Brak synchronizacji NTP!"
}
```

**Uwaga:** bez internetu po restarcie czas będzie błędny — tryb cichy i okno godzinowe nie zadziałają poprawnie. Matka powinna wysłać alert Telegram gdy NTP się nie synchronizuje.

### WiFiManager + mDNS

**Gotowa implementacja z poprzedniego projektu** — przenieść i dostosować zamiast pisać od zera.

| Plik | Co wziąć |
|---|---|
| `storage.h/cpp` | `struct WiFiCreds` + `Load/Save/DeleteWiFiCreds` |
| `web_dashboard.cpp` | `AP_PAGE` HTML + handlery `apHandle*` + `apServerInit/Handle` |
| `main.cpp` | `loadSavedCreds()` + `connectWiFi()` + `startAPMode()` + logika timeout w `loop()` |
| `config.h` | Stałe `AP_SSID`, `AP_PASS`, `AP_IP`, `AP_TIMEOUT_MS`, `WIFI_CREDS_PATH` |

**Zmiany względem poprzedniego projektu:**
- `AP_SSID` → `"Konfiguracja_Mleko"`
- `WiFiMulti` z hardcoded SSID → uprościć do jednej warstwy (tylko saved creds z LittleFS)
- Po udanym `WiFi.begin()` dołożyć: `synchronizujCzas()` + `MDNS.begin("mleko")`
- Alert Telegram gdy NTP nie odpowie (nowe, nie było w poprzednim projekcie)

**Kluczowe — trzy linie captive portalu (bez nich telefony nie pokażą automatycznego popupu):**
```cpp
dnsServer.start(53, "*", WiFi.softAPIP());  // DNS catchall
apServer.onNotFound(redirect302);            // HTTP catchall
// + handlery: /generate_204, /hotspot-detect.html, /fwlink
```

**Mechanizmy do zachowania:**
- Timeout AP (`AP_TIMEOUT_MS` = 3 min) z resetem gdy user podłączony
- `POST /api/wifi-reset` → kasuje `/wifi.json` → restart → tryb AP (do Dashboardu Matki)
- Skan sieci `GET /api/wifi-scan` → lista kliknij-żeby-wybrać

```cpp
// Po połączeniu z Wi-Fi — dołożyć do connectWiFi():
synchronizujCzas();
MDNS.begin("mleko");  // → http://mleko.local
```

`mleko.local` działa natywnie na iOS, macOS, Windows 10+, Linux. Na starszych Androidach może wymagać aplikacji mDNS — IP zawsze widoczne na Dashboardzie jako fallback.

### Historia temperatur (ring buffer)

```cpp
#define MAX_PROBEK 96  // 48h co 30 min
struct HistoriaWpis { float temperatura; uint32_t czas; };
HistoriaWpis historia[MAX_CZUJNIKOW][MAX_PROBEK];
int hist_idx[MAX_CZUJNIKOW] = {0};
```

### Alert szybkiego wzrostu temperatury

Wykrywanie wczesnej awarii schładzalnika — zanim temperatura przekroczy próg bezwzględny.

```cpp
// Konfigurowalny próg — domyślnie do ustalenia przez użytkownika
// Komenda: /set_wzrost 2.0  (°C na godzinę)
float PROG_WZROSTU_C_H = 0.0f;  // 0 = wyłączony dopóki nie ustawi użytkownik

void sprawdzWzrost(uint8_t id, float nowaTemp, uint32_t nowyCzas) {
    if (PROG_WZROSTU_C_H == 0.0f) return;  // nie skonfigurowany
    // Porównaj z pomiarem sprzed ~1h (2 próbki przy 30 min interwale)
    int idx_przed = (hist_idx[id] - 2 + MAX_PROBEK) % MAX_PROBEK;
    HistoriaWpis poprzedni = historia[id][idx_przed];
    if (poprzedni.czas == 0) return;  // brak danych historycznych

    float deltaT = nowaTemp - poprzedni.temperatura;
    float deltaH = (nowyCzas - poprzedni.czas) / 3600.0f;
    if (deltaH <= 0) return;

    float wzrost_na_h = deltaT / deltaH;
    if (wzrost_na_h >= PROG_WZROSTU_C_H) {
        wyslijAlert("Szybki wzrost temp! +" + String(wzrost_na_h, 1)
                    + "C/h (próg: " + String(PROG_WZROSTU_C_H, 1) + "C/h)",
                    ALERT_KRYTYCZNY);
    }
}
```

### Komendy Telegram

| Komenda | Opis |
|---|---|
| `/status` | Temperatura + bateria + ostatni pomiar |
| `/srednia` | Średnia 24h |
| `/historia` | Ostatnie 10 pomiarów |
| `/set_max 8.5` | Górny próg alarmu (°C) |
| `/set_min 2.0` | Dolny próg alarmu |
| `/set_wzrost 2.0` | Próg szybkości wzrostu temp (°C/h); 0 = wyłącz |
| `/interwał 30` | Zmień interwał pomiaru (minuty) |
| `/okno 6 22` | Pomiar tylko między 6:00 a 22:00 |
| `/okno 0 0` | Pomiar 24h |
| `/cichy 23 7` | Tryb cichy od 23:00 do 7:00 |
| `/kalibracja -0.3` | Offset kalibracyjny w °C |
| `/raport` | Pełny raport na żądanie |
| `/pomoc` | Lista komend |

### System alertów

| Alert | Typ | Przez tryb cichy? |
|---|---|---|
| Temperatura poza progiem | Krytyczny | Tak, zawsze |
| Szybki wzrost temperatury | Krytyczny | Tak, zawsze |
| Brak sygnału >2h | Krytyczny | Tak, zawsze |
| Błąd DS18B20 | Krytyczny | Tak, zawsze |
| Brak synchronizacji NTP | Krytyczny | Tak, zawsze |
| Bateria <15% | Łagodny | Nie |
| Bateria <5% | Krytyczny | Tak, zawsze |

Anti-spam: łagodne max raz na 12h, krytyczne co 1h dopóki problem trwa.

### OTA Matki

```cpp
server.on("/ota/matka", HTTP_POST, [](AsyncWebServerRequest* req){
    req->send(200, "text/plain", Update.hasError() ? "BLAD" : "OK");
    if (!Update.hasError()) ESP.restart();
}, [](AsyncWebServerRequest* req, String filename, size_t index,
       uint8_t* data, size_t len, bool final){
    if (!index) Update.begin(UPDATE_SIZE_UNKNOWN);
    Update.write(data, len);
    if (final) Update.end(true);
});
```

### OTA Satelity

```
Użytkownik wgrywa satelita.bin przez Dashboard (http://mleko.local)
    ↓
Matka zapisuje w LittleFS (/ota/satelita.bin)
Matka ustawia ota_pending[id] = true w Preferences
    ↓
Satelita budzi się normalnie → wysyła pomiar
ACK: ota_pending = true, ota_url = "http://mleko.local/ota/satelita.bin"
    ↓
Satelita NIE zasypia → pobiera HTTP → flashuje → restart
    ↓
Matka kasuje ota_pending[id] po pierwszym pomiarze po restarcie
```

### Konfiguracja trwała (Preferences)

```cpp
prefs.putFloat("max_1",      8.5f);
prefs.putFloat("min_1",      2.0f);
prefs.putFloat("wzrost_1",   0.0f);   // °C/h; 0 = wyłączony
prefs.putFloat("kal_1",      0.0f);
prefs.putUInt("int_1",       1800);
prefs.putUInt("okno_od_1",   0);
prefs.putUInt("okno_do_1",   0);
prefs.putInt("cichy_od",     23);
prefs.putInt("cichy_do",     7);
prefs.putString("chat_id",   "...");
```

---

## 7. Dashboard WWW — http://mleko.local

### Pliki w LittleFS

```
/data/
  index.html
  style.css
  app.js
  chart.min.js
/ota/
  satelita.bin      — tymczasowy, kasowany po wgraniu
```

### Sekcje

**Kafelek czujnika:** temperatura, trend wzrostu (°C/h), bateria (zielony/żółty/czerwony), czas ostatniego pomiaru, status (OK / ALARM / SZYBKI WZROST / BRAK SYGNAŁU / OTA W TOKU)

**Wykres:** historia 48h

**Panel ustawień:**
- Progi alarmu (min/max)
- Próg szybkości wzrostu (°C/h)
- Interwał pomiaru
- Okno godzinowe (start/stop)
- Tryb cichy (od/do)
- Offset kalibracyjny

**Panel OTA:**
- "Aktualizacja Matki" → upload .bin → natychmiastowy flash
- "Aktualizacja Satelity" → upload .bin → flash przy budzeniu
- Status: "Oczekuje na Satelitę..." gdy `ota_pending = true`

**Informacje systemowe:** uptime Matki, czas NTP, IP (fallback dla Androidów bez mDNS)

### API REST

```
GET  /api/status              → JSON: czujniki + konfiguracja + czas NTP
GET  /api/historia?id=1       → JSON: ring buffer 48h
POST /api/ustawienia          → JSON: wszystkie parametry konfiguracji
POST /ota/matka               → multipart .bin → flash Matki
POST /ota/satelita?id=1       → multipart .bin → zapis + flaga
```

---

## 8. Stałe konfiguracyjne

```cpp
// ===== SATELITA =====
#define ID_CZUJNIKA           1
#define PIN_DS18B20           4
#define PIN_ADC_BATERIA       34        // ⚠️ sprawdzić dla konkretnej płytki!
#define WSPOLCZYNNIK_ADC      2.0f      // ⚠️ sprawdzić dla konkretnej płytki!
#define INTERWAŁ_DOMYŚLNY_S   1800      // 30 minut
#define TIMEOUT_ACK_MS        500
#define WATCHDOG_S            8

// ===== MATKA =====
#define NAZWA_MDNS            "mleko"   // → http://mleko.local
#define NAZWA_AP              "Konfiguracja_Mleko"
#define NTP_STREFA            "CET-1CEST,M3.5.0,M10.5.0/3"  // Europe/Warsaw
#define NTP_SERWER1           "pool.ntp.org"
#define NTP_SERWER2           "time.google.com"
#define TIMEOUT_CZUJNIKA_MS   (2UL * 3600 * 1000)
#define PROG_BATERII_LAGODNY  15
#define PROG_BATERII_KRYT     5
#define MAX_CZUJNIKOW         8
#define MAX_PROBEK            96
#define ANTISPAM_LAGODNY_MS   (12UL * 3600 * 1000)
#define ANTISPAM_KRYT_MS      (1UL * 3600 * 1000)
```

---

## 9. Biblioteki Arduino

### Satelita
- `OneWire`, `DallasTemperature` — DS18B20
- `esp_now.h`, `esp_wifi.h` — komunikacja
- `HTTPClient`, `Update` — OTA

### Matka
- `WiFi.h`, `WiFiMulti.h`, `WebServer.h`, `DNSServer.h` — captive portal (z poprzedniego projektu)
- `ESPmDNS` — wbudowana, adres `mleko.local`
- `time.h` — wbudowana, synchronizacja NTP
- `UniversalTelegramBot` by Brian Lough
- `ESPAsyncWebServer`
- `ArduinoJson`
- `LittleFS`
- `Preferences`
- `Update` — OTA własna

---

## 10. Szacowany czas pracy baterii (2500 mAh)

| Interwał | Czas pracy (20°C) |
|---|---|
| Co 15 min | ~90–100 dni |
| **Co 30 min (domyślny)** | **~150 dni (~5 miesięcy)** |
| Co 60 min | ~190–200 dni |
| Okno 6–22h + 30 min | ~200+ dni |

Na mrozie poniżej 0°C pojemność spada o 30–50%. OTA jednorazowo ~10 mAh (0,4% baterii).

---

## 11. Zalecenia i zastrzeżenia

### Przed napisaniem kodu
- **Ustaw schemat partycji OTA** w Arduino IDE (`Tools → Partition Scheme → "With OTA"`) — zrób to przed pierwszym flashem. Zmiana później wymaga reflasha kablem. Bez poprawnych partycji przerwany OTA może zablokować płytkę.
- **Sprawdź PIN_ADC_BATERIA** — zmierz multimetrem które GPIO ma dzielnik napięcia.
- **Sprawdź WSPOLCZYNNIK_ADC** — zależy od konkretnej płytki.

### Kolejność uruchamiania
- Zawsze najpierw Matka w sieci Wi-Fi, potem Satelita. Jeśli Matka jest w trybie portalu AP, Satelita nie znajdzie jej podczas channel hopping.

### Debugowanie
- Używaj `delay()` zamiast Deep Sleep dopóki cała logika nie działa. Deep Sleep wprowadź na końcu.
- DS18B20 — zawsze pomiar rozgrzewkowy, odrzuć pierwszy wynik.

### Czas i NTP
- Bez internetu po restarcie czas będzie błędny — tryb cichy i okno godzinowe nie zadziałają. Matka wysyła alert Telegram gdy NTP się nie synchronizuje.

### mDNS
- `mleko.local` działa natywnie na iOS, macOS, Windows 10+, Linux.
- Na starszych Androidach może wymagać aplikacji. IP zawsze widoczne na Dashboardzie jako fallback.
- Rozważ rezerwację DHCP w routerze po MAC adresie Matki — IP nie zmieni się po restarcie routera.

### ADC
- Nieliniowość ~5% — wystarczające do alertów bateryjnych, nie do precyzji co do procenta.

### Alert wzrostu temperatury
- Domyślnie wyłączony (próg = 0). Użytkownik ustawia przez `/set_wzrost` lub Dashboard przed wdrożeniem.

---

## 12. Roadmap dla Claude Code

### Etap 1 — Satelita: podstawa
- [ ] Schemat partycji OTA ustawiony od razu
- [ ] Odczyt DS18B20 z pomiarem rozgrzewkowym, obsługa błędów
- [ ] Odczyt ADC baterii → procenty (po weryfikacji pinu i współczynnika)
- [ ] `delay()` zamiast Deep Sleep na etapie debugowania
- [ ] Wysyłka ESP-NOW (sztywny kanał na start)

### Etap 2 — Komunikacja dwukierunkowa
- [ ] Odbiór ESP-NOW na Matce, routing po `id_czujnika`
- [ ] ACK z Matki (interwał, okno godzinowe, ota_pending)
- [ ] Obsługa okna godzinowego na Satelicie (RTC RAM)
- [ ] Channel Hopping + zapis kanału w RTC RAM
- [ ] Zastąpienie `delay()` przez Deep Sleep
- [ ] Heartbeat watchdog na Matce

### Etap 3 — Telegram
- [ ] WiFi setup z poprzedniego projektu (captive portal, saved creds, timeout AP, wifi-reset)
- [ ] Dołożyć NTP + mDNS do `connectWiFi()` po udanym połączeniu
- [ ] Alert braku synchronizacji NTP
- [ ] Bot Telegram: czat grupowy
- [ ] Alerty: temperatura, szybki wzrost, bateria, błąd, brak sygnału
- [ ] Tryb cichy + anti-spam
- [ ] Wszystkie komendy z tabeli (w tym `/set_wzrost`)
- [ ] Preferences — zapis całej konfiguracji

### Etap 4 — Dashboard WWW
- [ ] ESPAsyncWebServer + LittleFS
- [ ] API REST
- [ ] Ring buffer 48h
- [ ] Frontend: kafelki (z trendem °C/h) + wykres + panel ustawień
- [ ] Wyświetlanie IP jako fallback dla mDNS

### Etap 5 — OTA
- [ ] OTA Matki: upload .bin → flash
- [ ] OTA Satelity: upload .bin → flaga → ACK → flash przy budzeniu
- [ ] Dashboard: status OTA

### Etap 6 — Szlif i wdrożenie
- [ ] Kalibracja DS18B20
- [ ] Walidacja skoków temperatury
- [ ] Konfiguracja progu wzrostu temperatury
- [ ] Obudowy 3D + uszczelnienie sondy
- [ ] Testy zasięgu ESP-NOW
- [ ] Test Channel Hopping
- [ ] Test OTA end-to-end
- [ ] Wdrożenie u teścia — WiFiManager + mleko.local

---

## 13. Znane ryzyka i rozwiązania

| Ryzyko | Rozwiązanie |
|---|---|
| Zły pin ADC baterii | Zmierzyć multimetrem przed kodowaniem |
| Brak partycji OTA | Ustawić w Etapie 1 |
| Matka w trybie AP podczas startu Satelity | Zawsze najpierw Matka w sieci |
| Brak NTP po restarcie | Alert Telegram + czas błędny do synchronizacji |
| Zmiana kanału routera | Channel Hopping |
| Restart Matki | Preferences |
| DS18B20 odpada | Flaga + alert krytyczny |
| Bateria <15% / <5% | Alert łagodny / krytyczny |
| Satelita milczy >2h | Heartbeat alert |
| Mróz | Izolacja obudowy |
| Przerwany OTA Satelity | Poprawne partycje = rollback |
| mleko.local na starym Androidzie | Fallback IP na Dashboardzie |
| Tania ładowarka — losowe resety | Markowa ładowarka 5V/1A |
| Awaria schładzalnika bez przekroczenia progu | Alert szybkiego wzrostu temp (°C/h) |

---

## 14. Potencjalne rozszerzenia

- Kolejne Satelity — nowy `ID_CZUJNIKA`, reszta działa
- Raport automatyczny o ustalonej godzinie
- MQTT + Home Assistant
- Drugi DS18B20 na jednym Satelicie (1-Wire to obsługuje)
- Powiadomienia push alternatywne dla Telegrama

---

*Wersja 8.0 — wszystkie decyzje zatwierdzone.*

*Kluczowe ustalenia:*
- *Sonda zanurzona w mleku, uszczelnienie silikonem spożywczym*
- *Interwał domyślny 30 min (~150 dni baterii 2500 mAh)*
- *Okno godzinowe przez Telegram i Dashboard*
- *Czat grupowy Telegram*
- *Tryb cichy: łagodne blokowane, krytyczne zawsze*
- *Alert szybkiego wzrostu temperatury (krytyczny, próg konfigurowalny, domyślnie wyłączony)*
- *Czas: NTP (Europe/Warsaw), alert gdy brak synchronizacji*
- *Dashboard pod http://mleko.local (mDNS) + IP jako fallback*
- *Zasilacz Matki: 5V/1A markowy*
- *OTA Matki: upload .bin → natychmiastowy flash*
- *OTA Satelity: upload .bin → flaga w ACK → flash przy budzeniu*
- *Schemat partycji OTA ustawić w Etapie 1*
- *Deep Sleep wprowadzić dopiero po weryfikacji całej logiki*
- *Raport tylko na żądanie (/raport)*
