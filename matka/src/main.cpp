#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <ESPmDNS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Update.h>
#include <time.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <DNSServer.h>
#include "secrets.h"

// === Wersja ===

#define FW_VERSION "5.4"

// === WiFi ===

#define AP_SSID "Smart_Mleko_Setup"
#define AP_TIMEOUT_MS 180000  // 3 min
#define WIFI_CREDS_PATH "/wifi.json"
#define NAZWY_PATH "/nazwy.json"
#define MONITORING_PATH "/monitoring.json"
#define HISTORIA_PATH_PREFIX "/hist_"

// Cache nazw i flag satelit indexed by ID (0 nieużywany, IDs 1-8)
#define MAX_SAT_ID 9
char satellite_names[MAX_SAT_ID][32] = {};
bool satellite_monitoring[MAX_SAT_ID] = {};  // false = alerty aktywne (domyślnie)

DNSServer dnsServer;
bool tryb_ap = false;
unsigned long ap_start_time = 0;

// === NTP ===

#define NTP_STREFA "CET-1CEST,M3.5.0,M10.5.0/3"

// === Telegram ===

const char* TELEGRAM_TOKEN = TELEGRAM_BOT_TOKEN;
const int64_t TELEGRAM_CHAT_ID = TELEGRAM_GROUP_ID;
unsigned long ostatni_telegram = 0;
const unsigned long TELEGRAM_COOLDOWN = 300000; // 5 min między alertami
long telegram_offset = 0;
unsigned long ostatni_poll = 0;
const unsigned long POLL_INTERVAL = 10000; // sprawdzaj komendy co 10s (rzadziej = mniej blokuje ESP-NOW)

// === Preferences ===

Preferences prefs;
float prog_max = 8.0;    // alarm gdy temp powyżej
float prog_min = -99.0;  // -99 = wyłączony; alarm gdy temp poniżej
float prog_wzrost = 2.0; // °C/h — alert gdy wzrost szybszy; 0 = wyłączony
uint32_t interwal_s = 1800;       // sekundy — dla bateriowych (deep sleep)
uint32_t interwal_zasil_s = 300;  // sekundy — dla zasilaczowych (always on)
uint8_t cichy_od = 0;  // 0 = wyłączony
uint8_t cichy_do = 0;
uint32_t alert_cykl_s = 900;      // 15 min domyślnie — cykl powtarzania alertów temp
unsigned long cichy_temp_do = 0;   // millis() do kiedy wyciszone alerty temp (0 = nie wyciszony)

// === Struktury ESP-NOW ===

typedef struct __attribute__((packed)) {
    uint8_t  id_czujnika;
    uint8_t  typ_zasilania;   // 1 = bateria (deep sleep), 2 = zasilacz (always on)
    float    temperatura;
    uint8_t  bateria_procent;
    uint32_t timestamp;
    bool     blad_czujnika;
    char     fw_version[8];   // wersja firmware satelity
} struct_message;

typedef struct __attribute__((packed)) {
    uint32_t nowy_interwal_s;
    uint8_t  godzina_start;
    uint8_t  godzina_stop;
    bool     ota_pending;
    char     ota_url[64];
    char     wifi_ssid[32];
    char     wifi_pass[64];
} struct_ack;

// === Multi-Satellite ===

#define MAX_SATELITY 8
#define MAX_HISTORIA_PER 48  // 24h co 30min per satelita

struct HistoriaWpis {
    float temperatura;
    uint32_t czas_unix;
    bool pusty;
};

struct SatelitaInfo {
    uint8_t id;
    uint8_t typ;              // 1=bateria, 2=zasilacz
    uint8_t mac[6];
    struct_message pomiar;
    unsigned long ostatni_czas; // millis() ostatniego odbioru
    bool aktywna;
    bool ota_pending;
    bool ota_url_wyslany;  // true po wysłaniu URL w ACK — przy następnej wiad. czyścimy flagę
    char nazwa[32];         // nazwa nadana przez użytkownika, pusta = "Czujnik #N"
    bool tylko_monitoring;  // true = brak alertów Telegram, tylko podgląd
    unsigned long ostatni_alert_temp_high; // millis() ostatniego alertu temp za wysoka
    unsigned long ostatni_alert_temp_low;  // millis() ostatniego alertu temp za niska
    // Historia per satelita
    HistoriaWpis historia[MAX_HISTORIA_PER];
    int hist_idx;
    int hist_count;
};

SatelitaInfo satelity[MAX_SATELITY];
int ile_satelit = 0;

// === Zmienne globalne ===

unsigned long boot_time = 0;
String ip_adres = "";
File ota_satelity_file;
uint8_t *ota_buf = nullptr;
size_t ota_buf_size = 0;
size_t ota_buf_offset = 0;
bool ota_write_pending = false;

AsyncWebServer server(80);

// === Ring buffer logów ===

#define LOG_BUF_SIZE 50
#define LOG_MAX_LEN  100
char log_buf[LOG_BUF_SIZE][LOG_MAX_LEN];
int  log_idx   = 0;
int  log_count = 0;

// Forward declarations
void wczytajHistorie(SatelitaInfo *s);
void zapiszHistorie(SatelitaInfo *s);

// === Log ===

void addLog(const char *fmt, ...) {
    char tmp[LOG_MAX_LEN - 12];
    va_list args;
    va_start(args, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);

    struct tm t;
    char line[LOG_MAX_LEN];
    if (getLocalTime(&t)) {
        snprintf(line, sizeof(line), "[%02d:%02d:%02d] %s", t.tm_hour, t.tm_min, t.tm_sec, tmp);
    } else {
        snprintf(line, sizeof(line), "[--:--:--] %s", tmp);
    }
    strlcpy(log_buf[log_idx], line, LOG_MAX_LEN);
    log_idx = (log_idx + 1) % LOG_BUF_SIZE;
    if (log_count < LOG_BUF_SIZE) log_count++;
    Serial.println(line);
}

// === Pomocnicze — satelity ===

SatelitaInfo* znajdzSatelite(uint8_t id) {
    for (int i = 0; i < ile_satelit; i++) {
        if (satelity[i].id == id) return &satelity[i];
    }
    return nullptr;
}

SatelitaInfo* znajdzLubDodajSatelite(uint8_t id, uint8_t typ, const uint8_t *mac) {
    SatelitaInfo *s = znajdzSatelite(id);
    if (s) {
        memcpy(s->mac, mac, 6);
        if (typ > 0) s->typ = typ;
        return s;
    }
    if (ile_satelit >= MAX_SATELITY) return nullptr;
    s = &satelity[ile_satelit++];
    memset(s, 0, sizeof(SatelitaInfo));
    s->id = id;
    s->typ = typ > 0 ? typ : 1;
    memcpy(s->mac, mac, 6);
    s->aktywna = true;
    if (id > 0 && id < MAX_SAT_ID && strlen(satellite_names[id]) > 0) {
        strlcpy(s->nazwa, satellite_names[id], sizeof(s->nazwa));
    }
    s->tylko_monitoring = (id > 0 && id < MAX_SAT_ID) ? satellite_monitoring[id] : false;
    for (int i = 0; i < MAX_HISTORIA_PER; i++) s->historia[i].pusty = true;
    wczytajHistorie(s);
    addLog("[SAT] Nowa satelita #%d typ=%d MAC=%02X:%02X:%02X:%02X:%02X:%02X",
        id, s->typ, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return s;
}

void usunSatelite(uint8_t id) {
    for (int i = 0; i < ile_satelit; i++) {
        if (satelity[i].id == id) {
            esp_now_del_peer(satelity[i].mac);
            for (int j = i; j < ile_satelit - 1; j++) {
                satelity[j] = satelity[j+1];
            }
            ile_satelit--;
            addLog("[SAT] Satelita #%d usunieta", id);
            return;
        }
    }
}

String macToString(const uint8_t *mac) {
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return String(buf);
}

void dodajDoHistorii(SatelitaInfo *s, float temp) {
    struct tm info;
    if (!getLocalTime(&info)) return;
    time_t now;
    time(&now);
    s->historia[s->hist_idx].temperatura = temp;
    s->historia[s->hist_idx].czas_unix = (uint32_t)now;
    s->historia[s->hist_idx].pusty = false;
    s->hist_idx = (s->hist_idx + 1) % MAX_HISTORIA_PER;
    if (s->hist_count < MAX_HISTORIA_PER) s->hist_count++;
}

// === Historia satelit (LittleFS) ===

void zapiszHistorie(SatelitaInfo *s) {
    char path[24];
    snprintf(path, sizeof(path), "%s%d.json", HISTORIA_PATH_PREFIX, s->id);
    File f = LittleFS.open(path, "w");
    if (!f) return;
    f.write('[');
    bool first = true;
    int start = (s->hist_count == MAX_HISTORIA_PER) ? s->hist_idx : 0;
    for (int i = 0; i < s->hist_count; i++) {
        int idx = (start + i) % MAX_HISTORIA_PER;
        if (s->historia[idx].pusty) continue;
        if (!first) f.write(',');
        char buf[32];
        snprintf(buf, sizeof(buf), "{\"v\":%.2f,\"t\":%lu}",
            s->historia[idx].temperatura,
            (unsigned long)s->historia[idx].czas_unix);
        f.print(buf);
        first = false;
    }
    f.write(']');
    f.close();
    Serial.printf("[FS] Historia #%d zapisana\n", s->id);
}

void wczytajHistorie(SatelitaInfo *s) {
    char path[24];
    snprintf(path, sizeof(path), "%s%d.json", HISTORIA_PATH_PREFIX, s->id);
    File f = LittleFS.open(path, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return; }
    f.close();
    JsonArray arr = doc.as<JsonArray>();
    s->hist_idx = 0;
    s->hist_count = 0;
    for (JsonObject entry : arr) {
        if (s->hist_count >= MAX_HISTORIA_PER) break;
        s->historia[s->hist_idx].temperatura = entry["v"] | 0.0f;
        s->historia[s->hist_idx].czas_unix   = entry["t"] | (uint32_t)0;
        s->historia[s->hist_idx].pusty       = false;
        s->hist_idx = (s->hist_idx + 1) % MAX_HISTORIA_PER;
        s->hist_count++;
    }
    Serial.printf("[FS] Historia #%d wczytana (%d wpisow)\n", s->id, s->hist_count);
}

// === Nazwy satelit (LittleFS) ===

void wczytajNazwy() {
    File f = LittleFS.open(NAZWY_PATH, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return; }
    f.close();
    for (JsonPair kv : doc.as<JsonObject>()) {
        int id = atoi(kv.key().c_str());
        if (id > 0 && id < MAX_SAT_ID) {
            strlcpy(satellite_names[id], kv.value().as<const char*>(), 32);
        }
    }
    Serial.println("[FS] Nazwy satelit wczytane");
}

void zapiszNazwy() {
    JsonDocument doc;
    for (int id = 1; id < MAX_SAT_ID; id++) {
        if (strlen(satellite_names[id]) > 0) {
            doc[String(id)] = satellite_names[id];
        }
    }
    File f = LittleFS.open(NAZWY_PATH, "w");
    if (!f) return;
    serializeJson(doc, f);
    f.close();
}

// === Flagi "tylko monitoring" (LittleFS) ===

void wczytajMonitoring() {
    File f = LittleFS.open(MONITORING_PATH, "r");
    if (!f) return;
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return; }
    f.close();
    for (JsonPair kv : doc.as<JsonObject>()) {
        int id = atoi(kv.key().c_str());
        if (id > 0 && id < MAX_SAT_ID) {
            satellite_monitoring[id] = kv.value().as<bool>();
        }
    }
    Serial.println("[FS] Flagi monitoring wczytane");
}

void zapiszMonitoring() {
    JsonDocument doc;
    for (int id = 1; id < MAX_SAT_ID; id++) {
        if (satellite_monitoring[id]) {
            doc[String(id)] = true;
        }
    }
    File f = LittleFS.open(MONITORING_PATH, "w");
    if (!f) return;
    serializeJson(doc, f);
    f.close();
}

// === WiFi Credentials (LittleFS) ===

bool wczytajWiFiCreds(char *ssid, char *pass) {
    File f = LittleFS.open(WIFI_CREDS_PATH, "r");
    if (!f) return false;
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return false; }
    f.close();
    const char *s = doc["ssid"] | "";
    const char *p = doc["pass"] | "";
    if (strlen(s) == 0) return false;
    strlcpy(ssid, s, 64);
    strlcpy(pass, p, 64);
    Serial.printf("[FS] WiFi creds: ssid='%s'\n", ssid);
    return true;
}

bool zapiszWiFiCreds(const char *ssid, const char *pass) {
    JsonDocument doc;
    doc["ssid"] = ssid;
    doc["pass"] = pass;
    File f = LittleFS.open(WIFI_CREDS_PATH, "w");
    if (!f) return false;
    serializeJson(doc, f);
    f.close();
    Serial.printf("[FS] WiFi saved: ssid='%s'\n", ssid);
    return true;
}

void usunWiFiCreds() {
    if (LittleFS.exists(WIFI_CREDS_PATH)) {
        LittleFS.remove(WIFI_CREDS_PATH);
        Serial.println("[FS] WiFi creds deleted");
    }
}

// === Callback ESP-NOW ===

void wyslijACK(SatelitaInfo *s);  // forward declaration

void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
    if (len != sizeof(struct_message)) return;

    struct_message msg;
    memcpy(&msg, data, sizeof(struct_message));

    SatelitaInfo *s = znajdzLubDodajSatelite(msg.id_czujnika, msg.typ_zasilania, mac);
    if (!s) {
        Serial.println("[ESP-NOW] Brak miejsca na nowa satelite!");
        return;
    }

    // Filtruj sondy channel-hopping (temp=0, bat=0, blad=true) — nie nadpisuj danych czujnika
    bool is_probe = (msg.blad_czujnika && msg.temperatura == 0.0f && msg.bateria_procent == 0);
    if (!is_probe) {
        memcpy(&s->pomiar, &msg, sizeof(struct_message));
        s->ostatni_czas = millis();
        s->aktywna = true;
        addLog("[ESP-NOW] Satelita #%d: %.1f°C bat=%d%%",
            msg.id_czujnika, msg.temperatura, msg.bateria_procent);
        // Reset OTA — gdy satelita wróciła po restarcie (URL już był wysłany)
        if (s->ota_pending && s->ota_url_wyslany) {
            s->ota_pending = false;
            s->ota_url_wyslany = false;
            addLog("[OTA] Satelita #%d — flaga wyczyszczona", s->id);
        }
    }

    // ACK zawsze — sonda też potrzebuje odpowiedzi żeby znaleźć kanał
    wyslijACK(s);
}

void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
    if (status != ESP_NOW_SEND_SUCCESS)
        addLog("[ESP-NOW] ACK: BLAD");
}

// === ACK ===

void wyslijACK(SatelitaInfo *s) {
    if (!esp_now_is_peer_exist(s->mac)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, s->mac, 6);
        peer.channel = 0;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    struct_ack ack = {};
    ack.nowy_interwal_s = (s->typ == 2) ? interwal_zasil_s : interwal_s;
    ack.godzina_start = 0;
    ack.godzina_stop = 0;
    ack.ota_pending = s->ota_pending;
    memset(ack.ota_url, 0, sizeof(ack.ota_url));
    memset(ack.wifi_ssid, 0, sizeof(ack.wifi_ssid));
    memset(ack.wifi_pass, 0, sizeof(ack.wifi_pass));
    if (s->ota_pending) {
        String url = "http://" + ip_adres + "/ota/satelita.bin";
        strlcpy(ack.ota_url, url.c_str(), sizeof(ack.ota_url));
        // Wyślij WiFi credentials żeby Satelita mogła pobrać .bin
        String ssid = WiFi.SSID();
        String pass = WiFi.psk();
        strlcpy(ack.wifi_ssid, ssid.c_str(), sizeof(ack.wifi_ssid));
        strlcpy(ack.wifi_pass, pass.c_str(), sizeof(ack.wifi_pass));
        Serial.printf("[ACK] OTA dla #%d: %s\n", s->id, ack.ota_url);
        s->ota_url_wyslany = true;
    }

    esp_now_send(s->mac, (uint8_t*)&ack, sizeof(ack));
}

// === Czas ===

String pobierzCzas() {
    struct tm info;
    if (!getLocalTime(&info)) return "brak synchronizacji";
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &info);
    return String(buf);
}

String czasOd(unsigned long ms) {
    if (ms == 0) return "---";
    unsigned long sek = (millis() - ms) / 1000;
    if (sek < 60) return String(sek) + "s temu";
    if (sek < 3600) return String(sek / 60) + "min temu";
    return String(sek / 3600) + "h " + String((sek % 3600) / 60) + "min temu";
}

// === Telegram ===

void wyslijTelegram(const String &msg) {
    WiFiClientSecure client;
    client.setInsecure(); // ESP32 nie ma certyfikatów CA
    HTTPClient http;

    String url = "https://api.telegram.org/bot";
    url += TELEGRAM_TOKEN;
    url += "/sendMessage";

    http.begin(client, url);
    http.addHeader("Content-Type", "application/json");

    JsonDocument doc;
    doc["chat_id"] = TELEGRAM_CHAT_ID;
    doc["text"] = msg;
    doc["parse_mode"] = "HTML";

    String body;
    serializeJson(doc, body);

    int code = http.POST(body);
    if (code > 0) {
        addLog("[Telegram] Wyslano (HTTP %d)", code);
    } else {
        addLog("[Telegram] Blad: %s", http.errorToString(code).c_str());
    }
    http.end();
}

bool czyTrybCichy() {
    if (cichy_od == 0 && cichy_do == 0) return false;
    struct tm info;
    if (!getLocalTime(&info)) return false;
    uint8_t h = info.tm_hour;
    if (cichy_od < cichy_do) return h >= cichy_od && h < cichy_do;
    return h >= cichy_od || h < cichy_do; // np. 23-7
}

void sprawdzAlerty(SatelitaInfo *s) {
    if (s->tylko_monitoring) return;
    if (s->ota_url_wyslany) return;

    // Sprawdź czy wyciszenie temp nie wygasło
    bool cichy_temp_aktywny = false;
    if (cichy_temp_do > 0) {
        if (millis() > cichy_temp_do) {
            cichy_temp_do = 0; // wygasł — reset
        } else {
            cichy_temp_aktywny = true;
        }
    }

    String prefix = "Czujnik #" + String(s->id) + ": ";

    // === Alerty temperaturowe — własny per-satelita cooldown, niezależny od globalnego ===
    if (!cichy_temp_aktywny && !s->pomiar.blad_czujnika) {
        unsigned long cooldown = (unsigned long)alert_cykl_s * 1000UL;
        if (s->pomiar.temperatura > prog_max) {
            if (s->ostatni_alert_temp_high == 0 || millis() - s->ostatni_alert_temp_high >= cooldown) {
                wyslijTelegram("🔴 <b>Smart Mleko</b>\n" + prefix + "Temperatura za wysoka: <b>"
                    + String(s->pomiar.temperatura, 1) + "°C</b> (próg: " + String(prog_max, 1) + "°C)");
                s->ostatni_alert_temp_high = millis();
            }
        }
        if (prog_min > -50 && s->pomiar.temperatura < prog_min) {
            if (s->ostatni_alert_temp_low == 0 || millis() - s->ostatni_alert_temp_low >= cooldown) {
                wyslijTelegram("🔵 <b>Smart Mleko</b>\n" + prefix + "Temperatura za niska: <b>"
                    + String(s->pomiar.temperatura, 1) + "°C</b> (próg: " + String(prog_min, 1) + "°C)");
                s->ostatni_alert_temp_low = millis();
            }
        }
    }

    // === Pozostałe alerty — globalny cooldown 5 min ===
    if (millis() - ostatni_telegram < TELEGRAM_COOLDOWN && ostatni_telegram > 0) return;

    String msg = "";
    bool krytyczny = false;

    if (s->pomiar.blad_czujnika) {
        msg = "⚠️ <b>Smart Mleko</b>\n" + prefix + "Błąd czujnika temperatury!";
        krytyczny = true;
    } else if (s->pomiar.bateria_procent <= 5 && s->typ == 1) {
        msg = "🔋 <b>Smart Mleko</b>\n" + prefix + "Bateria krytyczna: <b>"
            + String(s->pomiar.bateria_procent) + "%</b>";
        krytyczny = true;
    } else if (s->pomiar.bateria_procent <= 15 && s->typ == 1) {
        msg = "🔋 <b>Smart Mleko</b>\n" + prefix + "Niski poziom baterii: <b>"
            + String(s->pomiar.bateria_procent) + "%</b>";
    } else if (!s->pomiar.blad_czujnika && prog_wzrost > 0 && s->hist_count >= 2) {
        int last_idx = (s->hist_idx - 1 + MAX_HISTORIA_PER) % MAX_HISTORIA_PER;
        int prev_idx = (s->hist_idx - 2 + MAX_HISTORIA_PER) % MAX_HISTORIA_PER;
        if (!s->historia[last_idx].pusty && !s->historia[prev_idx].pusty) {
            float dt = (float)(s->historia[last_idx].czas_unix - s->historia[prev_idx].czas_unix) / 3600.0f;
            if (dt > 0) {
                float rate = (s->historia[last_idx].temperatura - s->historia[prev_idx].temperatura) / dt;
                if (rate > prog_wzrost) {
                    msg = "📈 <b>Smart Mleko</b>\n" + prefix + "Szybki wzrost temperatury: <b>+"
                        + String(rate, 1) + "°C/h</b>\n(próg: " + String(prog_wzrost, 1) + "°C/h)";
                }
            }
        }
    }

    if (msg.length() > 0) {
        if (!krytyczny && czyTrybCichy()) return;
        wyslijTelegram(msg);
        ostatni_telegram = millis();
    }
}

void sprawdzHeartbeat() {
    for (int i = 0; i < ile_satelit; i++) {
        SatelitaInfo *s = &satelity[i];
        if (!s->aktywna) continue;

        // Bateriowe: timeout = 3× interwał, zasilaczowe: 3× interwał_zasil, minimum 5 min
        unsigned long timeout = (s->typ == 1) ? interwal_s * 3000UL : interwal_zasil_s * 3000UL;
        if (timeout < 300000UL) timeout = 300000UL; // minimum 5 min

        if ((millis() - s->ostatni_czas) > timeout) {
            if (s->ota_url_wyslany) continue; // satelita jest w trakcie OTA — nie alarmuj
            if (millis() - ostatni_telegram < TELEGRAM_COOLDOWN && ostatni_telegram > 0) return;
            wyslijTelegram("📡 <b>Smart Mleko</b>\nCzujnik #" + String(s->id) +
                " milczy od: " + czasOd(s->ostatni_czas));
            ostatni_telegram = millis();
        }
    }
}

// === Preferences ===

void wczytajPreferences() {
    prefs.begin("mleko", false);
    prog_max = prefs.getFloat("prog_max", 8.0);
    prog_min = prefs.getFloat("prog_min", -99.0);
    prog_wzrost = prefs.getFloat("prog_wzrost", 2.0);
    interwal_s = prefs.getUInt("interwal", 1800);
    interwal_zasil_s = prefs.getUInt("interwal_zasil", 300);
    cichy_od = prefs.getUChar("cichy_od", 0);
    cichy_do = prefs.getUChar("cichy_do", 0);
    alert_cykl_s = prefs.getUInt("alert_cykl", 900);
    Serial.printf("[Prefs] max=%.1f min=%.1f wzrost=%.1f interwal=%ds cichy=%d-%d alert_cykl=%ds\n",
        prog_max, prog_min, prog_wzrost, interwal_s, cichy_od, cichy_do, alert_cykl_s);
}

void zapiszPreferences() {
    prefs.putFloat("prog_max", prog_max);
    prefs.putFloat("prog_min", prog_min);
    prefs.putFloat("prog_wzrost", prog_wzrost);
    prefs.putUInt("interwal", interwal_s);
    prefs.putUInt("interwal_zasil", interwal_zasil_s);
    prefs.putUChar("cichy_od", cichy_od);
    prefs.putUChar("cichy_do", cichy_do);
    prefs.putUInt("alert_cykl", alert_cykl_s);
}

// === Komendy Telegram ===

void obsluzKomende(const String &tekst) {
    String cmd = tekst;
    cmd.trim();
    cmd.toLowerCase();

    if (cmd == "/status") {
        String msg = "📊 <b>Smart Mleko — Status</b>\n";
        if (ile_satelit == 0) {
            msg += "Brak czujników";
        } else {
            for (int i = 0; i < ile_satelit; i++) {
                SatelitaInfo *s = &satelity[i];
                msg += "\n<b>Czujnik #" + String(s->id) + "</b>";
                msg += (s->typ == 1) ? " 🔋" : " ⚡";
                msg += "\n";
                if (s->pomiar.blad_czujnika) {
                    msg += "Temperatura: BŁĄD CZUJNIKA\n";
                } else {
                    msg += "Temperatura: <b>" + String(s->pomiar.temperatura, 1) + "°C</b>\n";
                }
                if (s->typ == 1) msg += "Bateria: " + String(s->pomiar.bateria_procent) + "%\n";
                msg += "Ostatni pomiar: " + czasOd(s->ostatni_czas) + "\n";
            }
        }
        unsigned long sek = millis() / 1000;
        msg += "\nUptime: " + String(sek / 3600) + "h " + String((sek % 3600) / 60) + "min";
        msg += "\nIP: " + ip_adres;
        wyslijTelegram(msg);

    } else if (cmd.startsWith("/set_max ")) {
        String arg = cmd.substring(9);
        arg.trim();
        if (arg == "wyl" || arg == "wył") {
            prog_max = 99.0;
            zapiszPreferences();
            wyslijTelegram("✅ Alarm górny <b>wyłączony</b>");
        } else {
            float val = arg.toFloat();
            if ((val != 0 || arg == "0") && val >= -30 && val <= 50) {
                prog_max = val;
                zapiszPreferences();
                wyslijTelegram("✅ Alarm górny ustawiony na <b>" + String(prog_max, 1) + "°C</b>\n"
                    "Dostaniesz alert gdy temperatura mleka przekroczy tę wartość.");
            } else {
                wyslijTelegram("❌ Podaj temperaturę -30 do 50, np. /set_max 8.5\n"
                    "Lub /set_max wyl żeby wyłączyć alarm górny.");
            }
        }

    } else if (cmd.startsWith("/set_min ")) {
        String arg = cmd.substring(9);
        arg.trim();
        if (arg == "wyl" || arg == "wył") {
            prog_min = -99.0;
            zapiszPreferences();
            wyslijTelegram("✅ Alarm dolny <b>wyłączony</b>");
        } else {
            float val = arg.toFloat();
            if ((val != 0 || arg == "0") && val >= -30 && val <= 50) {
                prog_min = val;
                zapiszPreferences();
                wyslijTelegram("✅ Alarm dolny ustawiony na <b>" + String(prog_min, 1) + "°C</b>\n"
                    "Dostaniesz alert gdy temperatura mleka spadnie poniżej tej wartości.");
            } else {
                wyslijTelegram("❌ Podaj temperaturę -30 do 50, np. /set_min 2.0\n"
                    "Lub /set_min wyl żeby wyłączyć alarm dolny.");
            }
        }

    } else if (cmd.startsWith("/set_wzrost ")) {
        String arg = cmd.substring(12);
        arg.trim();
        if (arg == "wyl" || arg == "wył" || arg == "0") {
            prog_wzrost = 0.0;
            zapiszPreferences();
            wyslijTelegram("✅ Alert wzrostu temperatury <b>wyłączony</b>");
        } else {
            float val = arg.toFloat();
            if (val > 0 && val <= 20) {
                prog_wzrost = val;
                zapiszPreferences();
                wyslijTelegram("✅ Alert wzrostu ustawiony na <b>" + String(prog_wzrost, 1) + "°C/h</b>\n"
                    "Dostaniesz alert gdy temperatura rośnie szybciej niż ta wartość.");
            } else {
                wyslijTelegram("❌ Podaj wartość 0.1–20, np. /set_wzrost 2.0\n"
                    "Lub /set_wzrost 0 żeby wyłączyć.");
            }
        }

    } else if (cmd.startsWith("/interwal ")) {
        int val = cmd.substring(10).toInt();
        if (val >= 1 && val <= 1440) {
            interwal_s = val * 60;
            zapiszPreferences();
            wyslijTelegram("✅ Interwał ustawiony na <b>" + String(val) + " min</b>\n"
                "Zmiana wejdzie w życie przy następnym budzeniu Satelity.");
        } else {
            wyslijTelegram("❌ Podaj minuty 1-1440, np. /interwal 30");
        }

    } else if (cmd.startsWith("/cichy ")) {
        int space = cmd.indexOf(' ', 7);
        if (space > 0) {
            cichy_od = cmd.substring(7, space).toInt();
            cichy_do = cmd.substring(space + 1).toInt();
            zapiszPreferences();
            if (cichy_od == 0 && cichy_do == 0) {
                wyslijTelegram("✅ Tryb cichy <b>wyłączony</b>");
            } else {
                wyslijTelegram("✅ Tryb cichy: <b>" + String(cichy_od) + ":00 — " +
                    String(cichy_do) + ":00</b>\nŁagodne alerty wstrzymane w tym oknie.");
            }
        } else {
            wyslijTelegram("❌ Podaj godziny, np. /cichy 23 7 (lub /cichy 0 0 żeby wyłączyć)");
        }

    } else if (cmd == "/cichy_temp" || cmd.startsWith("/cichy_temp ")) {
        String arg = "";
        if (cmd.length() > 12) { arg = cmd.substring(12); arg.trim(); }
        if (arg == "off") {
            cichy_temp_do = 0;
            wyslijTelegram("✅ Alerty temperaturowe <b>wznowione</b>.");
        } else {
            cichy_temp_do = millis() + 86400000UL; // 24h
            wyslijTelegram("🔕 Alerty temp wyciszone na <b>24h</b>.\nBłąd czujnika i bateria nadal aktywne.\nOdwołaj: /cichy_temp off");
        }

    } else if (cmd == "/ustawienia") {
        String msg = "⚙️ <b>Ustawienia</b>\n\n";
        if (prog_max < 50) {
            msg += "🔴 Alarm górny: " + String(prog_max, 1) + "°C\n";
        } else {
            msg += "🔴 Alarm górny: wyłączony\n";
        }
        if (prog_min > -50) {
            msg += "🔵 Alarm dolny: " + String(prog_min, 1) + "°C\n";
        } else {
            msg += "🔵 Alarm dolny: wyłączony\n";
        }
        msg += "⏱ Interwał pomiaru: " + String(interwal_s / 60) + " min\n";
        if (cichy_od == 0 && cichy_do == 0) {
            msg += "🔕 Tryb cichy: wyłączony\n";
        } else {
            msg += "🔕 Tryb cichy: " + String(cichy_od) + ":00 — " + String(cichy_do) + ":00\n";
        }
        msg += "\n<b>Czujniki:</b> " + String(ile_satelit) + " zarejestrowanych";
        for (int i = 0; i < ile_satelit; i++) {
            msg += "\n#" + String(satelity[i].id);
            msg += (satelity[i].typ == 1) ? " 🔋 bateria" : " ⚡ zasilacz";
        }
        wyslijTelegram(msg);

    } else if (cmd == "/historia") {
        if (ile_satelit == 0) {
            wyslijTelegram("📜 <b>Historia</b>\nBrak czujników.");
        } else {
            String msg = "📜 <b>Ostatnie pomiary</b>\n";
            for (int si = 0; si < ile_satelit; si++) {
                SatelitaInfo *s = &satelity[si];
                msg += "\n<b>Czujnik #" + String(s->id) + "</b>\n";
                if (s->hist_count == 0) {
                    msg += "Brak danych\n";
                    continue;
                }
                int ile = min(s->hist_count, 10);
                for (int i = 0; i < ile; i++) {
                    int idx = (s->hist_idx - 1 - i + MAX_HISTORIA_PER) % MAX_HISTORIA_PER;
                    if (s->historia[idx].pusty) continue;
                    struct tm info;
                    time_t t = (time_t)s->historia[idx].czas_unix;
                    localtime_r(&t, &info);
                    char buf[6];
                    strftime(buf, sizeof(buf), "%H:%M", &info);
                    msg += String(buf) + " — <b>" + String(s->historia[idx].temperatura, 1) + "°C</b>\n";
                }
            }
            wyslijTelegram(msg);
        }

    } else if (cmd == "/srednia") {
        if (ile_satelit == 0) {
            wyslijTelegram("📊 <b>Średnia 24h</b>\nBrak czujników.");
        } else {
            time_t now;
            time(&now);
            uint32_t granica = (uint32_t)now - 86400;
            String msg = "📊 <b>Średnia 24h</b>\n";
            for (int si = 0; si < ile_satelit; si++) {
                SatelitaInfo *s = &satelity[si];
                msg += "\n<b>Czujnik #" + String(s->id) + "</b>\n";
                float suma = 0, tmin = 999, tmax = -999;
                int n = 0;
                for (int i = 0; i < s->hist_count; i++) {
                    int idx = (s->hist_idx - 1 - i + MAX_HISTORIA_PER) % MAX_HISTORIA_PER;
                    if (s->historia[idx].pusty || s->historia[idx].czas_unix < granica) continue;
                    float t = s->historia[idx].temperatura;
                    suma += t;
                    if (t < tmin) tmin = t;
                    if (t > tmax) tmax = t;
                    n++;
                }
                if (n == 0) {
                    msg += "Brak danych\n";
                } else {
                    msg += "Średnia: <b>" + String(suma / n, 1) + "°C</b>\n";
                    msg += "Min: " + String(tmin, 1) + "°C, Max: " + String(tmax, 1) + "°C\n";
                    msg += "Pomiarów: " + String(n) + "\n";
                }
            }
            wyslijTelegram(msg);
        }

    } else if (cmd == "/raport") {
        String msg = "📋 <b>Smart Mleko — Raport</b>\n";

        // Czujniki
        if (ile_satelit == 0) {
            msg += "\nBrak czujników\n";
        } else {
            time_t now;
            time(&now);
            uint32_t granica = (uint32_t)now - 86400;
            for (int si = 0; si < ile_satelit; si++) {
                SatelitaInfo *s = &satelity[si];
                msg += "\n<b>Czujnik #" + String(s->id) + "</b>";
                msg += (s->typ == 1) ? " 🔋\n" : " ⚡\n";
                if (s->pomiar.blad_czujnika) {
                    msg += "Temperatura: BŁĄD\n";
                } else {
                    msg += "Temperatura: <b>" + String(s->pomiar.temperatura, 1) + "°C</b>\n";
                }
                if (s->typ == 1) msg += "Bateria: " + String(s->pomiar.bateria_procent) + "%\n";
                msg += "Ostatni: " + czasOd(s->ostatni_czas) + "\n";

                // Średnia 24h
                float suma = 0, tmin = 999, tmax = -999;
                int n = 0;
                for (int i = 0; i < s->hist_count; i++) {
                    int idx = (s->hist_idx - 1 - i + MAX_HISTORIA_PER) % MAX_HISTORIA_PER;
                    if (s->historia[idx].pusty || s->historia[idx].czas_unix < granica) continue;
                    float t = s->historia[idx].temperatura;
                    suma += t;
                    if (t < tmin) tmin = t;
                    if (t > tmax) tmax = t;
                    n++;
                }
                if (n > 0) {
                    msg += "24h: śr=" + String(suma / n, 1) + "°C (min:" + String(tmin, 1) + " max:" + String(tmax, 1) + ")\n";
                }
            }
        }

        // Ustawienia
        msg += "\n<b>Ustawienia:</b>\n";
        msg += "Alarm górny: " + String(prog_max < 50 ? String(prog_max, 1) + "°C" : "wył") + "\n";
        msg += "Alarm dolny: " + String(prog_min > -50 ? String(prog_min, 1) + "°C" : "wył") + "\n";
        msg += "Alert wzrostu: " + String(prog_wzrost > 0 ? String(prog_wzrost, 1) + "°C/h" : "wył") + "\n";
        msg += "Interwał: " + String(interwal_s / 60) + " min\n";
        if (cichy_od != 0 || cichy_do != 0) {
            msg += "Tryb cichy: " + String(cichy_od) + ":00—" + String(cichy_do) + ":00\n";
        }

        // System
        unsigned long sek = millis() / 1000;
        msg += "\n<b>System:</b>\n";
        msg += "Uptime: " + String(sek / 3600) + "h " + String((sek % 3600) / 60) + "min\n";
        msg += "IP: " + ip_adres + "\n";
        msg += "Czujniki: " + String(ile_satelit);
        wyslijTelegram(msg);

    } else if (cmd == "/wifi-reset") {
        wyslijTelegram("🔄 <b>Smart Mleko</b>\nKasuję dane WiFi i restartuję w tryb konfiguracji...\n"
            "Połącz się z siecią <b>Smart_Mleko_Setup</b> żeby wpisać nowe WiFi.");
        usunWiFiCreds();
        delay(1000);
        ESP.restart();

    } else if (cmd == "/pomoc" || cmd == "/help" || cmd == "/start") {
        String msg = "📋 <b>Komendy Smart Mleko</b>\n\n";
        msg += "<b>Podgląd:</b>\n";
        msg += "/status — temperatura, bateria, stan\n";
        msg += "/historia — ostatnie 10 pomiarów\n";
        msg += "/srednia — średnia, min, max z 24h\n";
        msg += "/raport — pełny raport\n";
        msg += "/ustawienia — aktualne ustawienia\n\n";
        msg += "<b>Alarmy:</b>\n";
        msg += "/set_max 8 — alarm gdy temp powyżej (°C)\n";
        msg += "/set_min 2 — alarm gdy temp poniżej (°C)\n";
        msg += "/set_wzrost 2 — alert szybkiego wzrostu (°C/h), 0=wył\n";
        msg += "Wpisz <i>wyl</i> zamiast liczby żeby wyłączyć\n\n";
        msg += "<b>Pomiary:</b>\n";
        msg += "/interwal 30 — co ile minut mierzyć\n\n";
        msg += "<b>Powiadomienia:</b>\n";
        msg += "/cichy 23 7 — wycisz łagodne alerty\n";
        msg += "/cichy 0 0 — wyłącz tryb cichy\n";
        msg += "/cichy_temp — wycisz alerty temp na 24h\n";
        msg += "/cichy_temp off — wznów alerty temp\n\n";
        msg += "<b>WiFi:</b>\n";
        msg += "/wifi-reset — zmień sieć WiFi\n\n";
        msg += "/pomoc — ta lista";
        wyslijTelegram(msg);
    }
}

void sprawdzTelegram() {
    if (millis() - ostatni_poll < POLL_INTERVAL) return;
    ostatni_poll = millis();

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;

    String url = "https://api.telegram.org/bot";
    url += TELEGRAM_TOKEN;
    url += "/getUpdates?timeout=0&limit=5&offset=";
    url += String(telegram_offset);

    http.begin(client, url);
    int code = http.GET();

    if (code == 200) {
        String payload = http.getString();
        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload);
        if (!err && doc["ok"].as<bool>()) {
            JsonArray results = doc["result"].as<JsonArray>();
            for (JsonObject upd : results) {
                telegram_offset = upd["update_id"].as<long>() + 1;
                int64_t chat_id = upd["message"]["chat"]["id"].as<int64_t>();
                if (chat_id == TELEGRAM_CHAT_ID) {
                    String tekst = upd["message"]["text"].as<String>();
                    if (tekst.startsWith("/")) {
                        addLog("[Telegram] Komenda: %s", tekst.c_str());
                        obsluzKomende(tekst);
                    }
                }
            }
        }
    }
    http.end();
}

// === Captive Portal HTML ===

const char AP_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html><head>
<meta charset='utf-8'>
<meta name='viewport' content='width=device-width,initial-scale=1'>
<title>Smart Mleko - Konfiguracja WiFi</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,system-ui,sans-serif;background:#0f172a;color:#e2e8f0;padding:16px;max-width:460px;margin:0 auto}
h1{font-size:1.3em;text-align:center;color:#38bdf8;margin:16px 0 4px}
.sub{text-align:center;font-size:.8em;color:#94a3b8;margin-bottom:16px}
.c{background:#1e293b;border-radius:14px;padding:16px;margin-bottom:12px;border:1px solid #334155}
h2{font-size:.9em;color:#94a3b8;margin-bottom:10px}
label{display:block;font-size:.82em;color:#94a3b8;margin-bottom:3px;margin-top:10px}
input[type=text],input[type=password]{width:100%;background:#0f172a;border:1px solid #334155;border-radius:8px;padding:9px 12px;color:#e2e8f0;font-size:.95em}
input:focus{outline:none;border-color:#38bdf8}
.btn{display:block;width:100%;background:#2563eb;color:#fff;border:none;border-radius:8px;padding:11px;font-size:.95em;cursor:pointer;margin-top:14px;font-weight:600}
.btn:hover{background:#1d4ed8}
.btn.sec{background:#16a34a}
#msg{text-align:center;padding:10px;border-radius:8px;margin-top:10px;font-size:.88em;display:none}
.ok{background:#166534;color:#4ade80;border:1px solid #4ade80}
.err{background:#991b1b;color:#fca5a5;border:1px solid #fca5a5}
.warn{background:#854d0e;border:1px solid #f59e0b;border-radius:8px;padding:10px;font-size:.82em;color:#fbbf24;margin-top:8px}
.net{display:flex;align-items:center;justify-content:space-between;padding:9px 4px;border-bottom:1px solid #334155;cursor:pointer}
.net:hover{background:#334155;border-radius:6px}.ns{font-size:.9em;color:#e2e8f0}.rssi{font-size:.75em;color:#64748b}
.inf{display:grid;grid-template-columns:1fr 1fr;gap:4px 12px;font-size:.82em}
.inf .k{color:#64748b}.inf .v{color:#e2e8f0;text-align:right}
</style></head><body>
<h1>Smart Mleko</h1>
<div class="sub">Konfiguracja WiFi</div>
<div class="c">
<h2>Polacz z siecia WiFi</h2>
<label>Nazwa sieci (SSID)</label>
<input type="text" id="ssid" placeholder="Wpisz lub wybierz ponizej">
<label>Haslo</label>
<input type="password" id="pass" placeholder="Haslo WiFi">
<button class="btn" onclick="save()">Zapisz i polacz</button>
<div id="msg"></div>
<div class="warn">Po zapisaniu urzadzenie uruchomi sie ponownie i polaczy z WiFi. Dashboard bedzie dostepny pod http://mleko.local</div>
</div>
<div class="c">
<h2>Dostepne sieci</h2>
<button class="btn sec" style="margin-top:0" onclick="scan()">Skanuj sieci WiFi</button>
<div id="nets" style="margin-top:8px"></div>
</div>
<div class="c">
<h2>System</h2>
<div class="inf"><span class="k">MAC</span><span class="v" id="mac">---</span>
<span class="k">Heap</span><span class="v" id="heap">---</span></div>
</div>
<script>
function msg(t,ok){let m=document.getElementById('msg');m.className=ok?'ok':'err';m.textContent=t;m.style.display='block'}
function save(){
let s=document.getElementById('ssid').value.trim();
let p=document.getElementById('pass').value;
if(!s){msg('Podaj nazwe sieci!',false);return}
msg('Zapisuje i restartuje...',true);
fetch('/api/wifi-save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:s,pass:p})})
.then(r=>r.json()).then(d=>{if(d.ok)msg('Zapisano! Restart...',true);else msg('Blad: '+d.error,false)})
.catch(()=>msg('Wyslano (restart...)',true));
}
function scan(){
let n=document.getElementById('nets');
n.innerHTML='<div style="color:#94a3b8;padding:8px 0">Skanowanie...</div>';
fetch('/api/wifi-scan').then(r=>r.json()).then(nets=>{
if(!nets.length){n.innerHTML='<div style="color:#94a3b8;padding:8px 0">Brak sieci</div>';return}
n.innerHTML='';
nets.forEach(function(net){
let d=document.createElement('div');d.className='net';
let ns=document.createElement('span');ns.className='ns';ns.textContent=net.ssid;
let rs=document.createElement('span');rs.className='rssi';
rs.textContent=net.rssi+' dBm '+(net.enc?'🔒':'');
d.appendChild(ns);d.appendChild(rs);
d.addEventListener('click',function(){document.getElementById('ssid').value=net.ssid;document.getElementById('pass').value='';document.getElementById('pass').focus()});
n.appendChild(d)});
}).catch(()=>{n.innerHTML='<div style="color:#fca5a5;padding:8px 0">Blad skanowania</div>'});
}
fetch('/api/sys').then(r=>r.json()).then(s=>{
document.getElementById('mac').textContent=s.mac;
document.getElementById('heap').textContent=(s.heap/1024).toFixed(0)+' KB'});
</script>
</body></html>
)rawliteral";

// === Dashboard HTML ===

const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="pl">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Smart Mleko</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:-apple-system,system-ui,sans-serif;background:#f5f5f7;color:#1d1d1f;min-height:100vh;padding:16px}
.wrap{max-width:600px;margin:0 auto}
.header{text-align:center;margin-bottom:24px;padding-top:8px}
.header h1{font-size:1.5em;font-weight:600;color:#1d1d1f;letter-spacing:-0.3px}
.header .status{font-size:0.85em;color:#6e6e73;margin-top:4px}
.card{background:#fff;border-radius:18px;padding:20px;margin-bottom:14px;box-shadow:0 2px 20px rgba(0,0,0,0.07)}
@media(max-width:480px){
.card{padding:14px;border-radius:14px}
.temp-value{font-size:3.2em}
.cfg-row{flex-wrap:wrap;gap:6px}
.cfg-row>div:first-child{width:100%}
}
.card h2{color:#6e6e73;font-size:0.75em;text-transform:uppercase;letter-spacing:1.2px;margin-bottom:16px;font-weight:600}
.temp-display{text-align:center;padding:16px 0 8px}
.temp-value{font-size:4.5em;font-weight:200;letter-spacing:-2px;color:#1d1d1f}
.temp-value.alarm{color:#ff3b30!important}
.temp-value.blad{color:#ff9f0a!important}
.temp-unit{font-size:1.8em;font-weight:300;color:#6e6e73}
.info-grid{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.info-item{background:#f5f5f7;border-radius:12px;padding:12px}
.info-label{font-size:0.7em;color:#aeaeb2;text-transform:uppercase;letter-spacing:0.5px;font-weight:500}
.info-value{font-size:1.2em;font-weight:600;margin-top:4px;color:#1d1d1f}
.bat-ok{color:#34c759}.bat-mid{color:#ff9f0a}.bat-low{color:#ff3b30}
.status-badge{display:inline-block;padding:5px 14px;border-radius:20px;font-size:0.82em;font-weight:600}
.status-ok{background:#e8f9ef;color:#1a7f3c}
.status-wait{background:#fff8e6;color:#b45309}
.status-err{background:#fff0f0;color:#cc1c1c}
.sys-info{font-size:0.82em;color:#6e6e73;line-height:2}
.sys-info strong{color:#1d1d1f;font-weight:600}
.refresh-note{text-align:center;font-size:0.72em;color:#aeaeb2;margin-top:8px}
.cfg-row{display:flex;align-items:center;justify-content:space-between;margin-bottom:14px;gap:12px}
.cfg-row label{font-size:0.85em;color:#1d1d1f;font-weight:500;flex:1}
.cfg-row input,.cfg-row select{background:#fff;border:1.5px solid #d2d2d7;color:#1d1d1f;border-radius:10px;padding:7px 10px;width:90px;font-size:0.92em;text-align:center}
.cfg-row input:focus,.cfg-row select:focus{outline:none;border-color:#0071e3}
.cfg-row .cfg-hint{font-size:0.7em;color:#aeaeb2;margin-top:2px}
.cfg-btn{background:#0071e3;color:#fff;border:none;padding:12px 24px;border-radius:12px;font-size:0.95em;cursor:pointer;width:100%;margin-top:8px;font-weight:600}
.cfg-btn:disabled{background:#d2d2d7;cursor:default}
.cfg-msg{text-align:center;font-size:0.85em;margin-top:10px;min-height:1.2em}
.cfg-check{display:flex;align-items:center;gap:8px}
.cfg-check input[type=checkbox]{width:18px;height:18px;accent-color:#0071e3}
.sat-type{font-size:0.68em;color:#aeaeb2;margin-left:6px;font-weight:400}
@media(min-width:768px){
.wrap{max-width:1100px}
.bottom-grid{display:grid;grid-template-columns:1fr 1fr;gap:14px}
.bottom-grid .card{margin-bottom:0}
}
</style>
</head>
<body>
<div class="wrap">
<div class="header">
<h1>Smart Mleko <span id="ver" style="font-size:0.45em;color:#aeaeb2;font-weight:400;letter-spacing:0;vertical-align:middle"></span></h1>
<div class="status" id="czas">---</div>
</div>

<div id="czujniki"><div class="card"><h2>Czujniki</h2><div style="text-align:center;color:#aeaeb2;padding:20px">Ladowanie...</div></div></div>

<div class="bottom-grid">
<div class="card">
<h2>System</h2>
<div class="sys-info">
<div>MAC Matki: <strong id="mac">---</strong></div>
<div>IP: <strong id="ip">---</strong></div>
<div>Uptime: <strong id="uptime">---</strong></div>
<div>NTP: <strong id="ntp">---</strong></div>
<div>Czujniki: <strong id="satCount">0</strong></div>
</div>
</div>

<div class="card">
<h2>Ustawienia</h2>
<div class="cfg-row">
<div><label>Alarm gorny</label><div class="cfg-hint">Powiadomienie gdy temp powyzej</div></div>
<div class="cfg-check"><input type="checkbox" id="maxOn" onchange="toggleMax()"><input type="number" id="cfgMax" step="0.5" style="width:75px"></div>
</div>
<div class="cfg-row">
<div><label>Alarm dolny</label><div class="cfg-hint">Powiadomienie gdy temp ponizej</div></div>
<div class="cfg-check"><input type="checkbox" id="minOn" onchange="toggleMin()"><input type="number" id="cfgMin" step="0.5" style="width:75px"></div>
</div>
<div class="cfg-row">
<div><label>Alert wzrostu</label><div class="cfg-hint">Alarm gdy temp rosnie zbyt szybko (C/h)</div></div>
<div class="cfg-check"><input type="checkbox" id="wzrostOn" onchange="toggleWzrost()"><input type="number" id="cfgWzrost" step="0.5" min="0.1" max="20" style="width:75px"></div>
</div>
<div class="cfg-row" style="display:none">
<div><label>Interwal - bateria</label><div class="cfg-hint">Co ile minut czujnik bateryjny mierzy</div></div>
<select id="cfgInt"><option value="5">5 min</option><option value="10">10 min</option><option value="15">15 min</option><option value="30">30 min</option><option value="60">1 godz</option><option value="120">2 godz</option></select>
</div>
<div class="cfg-row">
<div><label>Interwal - zasilacz</label><div class="cfg-hint">Co ile minut czujnik sieciowy mierzy</div></div>
<select id="cfgIntZ"><option value="1">1 min</option><option value="2">2 min</option><option value="5">5 min</option><option value="10">10 min</option><option value="15">15 min</option><option value="30">30 min</option></select>
</div>
<div class="cfg-row">
<div><label>Cykl alertu temp</label><div class="cfg-hint">Co ile minut powtarzac alert gdy temp poza progiem</div></div>
<select id="cfgAlertCykl"><option value="5">5 min</option><option value="10">10 min</option><option value="15">15 min</option><option value="30">30 min</option><option value="60">1 godz</option></select>
</div>
<div class="cfg-row">
<div><label>Tryb cichy</label><div class="cfg-hint">Wycisz lagodne alerty w nocy</div></div>
<div class="cfg-check"><input type="checkbox" id="cichyOn" onchange="toggleCichy()">
<select id="cfgCOd" style="width:65px"><option value="0">--</option></select>
<span style="color:#aeaeb2">-</span>
<select id="cfgCDo" style="width:65px"><option value="0">--</option></select></div>
</div>
<button class="cfg-btn" id="cfgBtn" onclick="saveConfig()">Zapisz ustawienia</button>
<div class="cfg-msg" id="cfgMsg"></div>
</div>

<div class="card">
<h2>Aktualizacja OTA — Matka</h2>
<div style="margin-bottom:12px">
<label style="font-size:0.85em;color:#6e6e73">Firmware Matki (.bin):</label>
<input type="file" id="otaFile" accept=".bin" style="margin-top:8px;color:#1d1d1f;font-size:0.85em">
</div>
<button onclick="uploadOTA()" id="otaBtn" style="background:#0071e3;color:#fff;border:none;padding:12px 24px;border-radius:12px;font-size:0.95em;cursor:pointer;width:100%;font-weight:600">Wgraj firmware</button>
<div id="otaStatus" style="margin-top:12px;font-size:0.85em;color:#6e6e73;text-align:center"></div>
<div style="background:#f5f5f7;border-radius:8px;height:6px;margin-top:8px;display:none" id="otaBarWrap">
<div id="otaBar" style="background:#0071e3;height:100%;border-radius:8px;width:0%;transition:width 0.3s"></div>
</div>
</div>

<div class="card">
<h2>Aktualizacja OTA — Satelita</h2>
<div style="margin-bottom:4px;font-size:0.8em;color:#aeaeb2">Jeden firmware dla wszystkich satelitow (ID i typ w Preferences)</div>
<div style="margin-bottom:12px">
<label style="font-size:0.85em;color:#6e6e73">Firmware Satelity (.bin):</label>
<input type="file" id="otaSatFile" accept=".bin" style="margin-top:8px;color:#1d1d1f;font-size:0.85em">
</div>
<button onclick="uploadOTASat()" id="otaSatBtn" style="background:#5856d6;color:#fff;border:none;padding:12px 24px;border-radius:12px;font-size:0.95em;cursor:pointer;width:100%;font-weight:600">Wgraj firmware Satelity</button>
<div id="otaSatStatus" style="margin-top:12px;font-size:0.85em;color:#6e6e73;text-align:center"></div>
<div style="background:#f5f5f7;border-radius:8px;height:6px;margin-top:8px;display:none" id="otaSatBarWrap">
<div id="otaSatBar" style="background:#5856d6;height:100%;border-radius:8px;width:0%;transition:width 0.3s"></div>
</div>
</div>
</div>

<div class="refresh-note">Odswiezanie co 5 sekund</div>

<script>
var KOLORY=['','#0071e3','#34c759','#ff9f0a','#ff3b30','#af52de','#5ac8fa','#ff2d55','#ffcc00'];
function kolorSat(id){return KOLORY[id]||'#0071e3';}
var nazwyMap={};
function renderCzujniki(satelity){
let c=document.getElementById('czujniki');
if(!satelity||!satelity.length){
c.innerHTML='<div class="card"><h2>Czujniki</h2><div style="text-align:center;color:#aeaeb2;padding:20px">Brak polaczonych czujnikow.<br>Podlacz Satelite — pojawi sie automatycznie.</div></div>';
return;
}
let html='';
satelity.forEach(function(s){
let typIcon=s.typ===1?'&#x1F50B;':'&#x26A1;';
let typName=s.typ===1?'bateria':'zasilacz';
let tempVal='--',tempClass='temp-value',badgeText='Czekam',badgeCls='status-badge status-wait';
let trendHtml='';
if(s.blad_czujnika){tempVal='ERR';tempClass='temp-value blad';badgeText='Blad czujnika!';badgeCls='status-badge status-err';}
else if(s.temperatura!==undefined){tempVal=s.temperatura.toFixed(1);badgeText='OK';badgeCls='status-badge status-ok';}
if(s.trend!==undefined&&s.trend!==null){
let arrow,cls;
if(s.trend>0.1){arrow='↑';cls='color:#ff3b30'}
else if(s.trend<-0.1){arrow='↓';cls='color:#34c759'}
else{arrow='→';cls='color:#aeaeb2'}
let sign=s.trend>=0?'+':'';
trendHtml='<div style="font-size:1.1em;margin-top:6px"><span style="'+cls+'">'+arrow+' '+sign+s.trend.toFixed(1)+'°C/h</span></div>';
}
let fwInfo=s.fw?' v'+s.fw:'';
let nazwaDisplay=(s.nazwa&&s.nazwa.length>0)?s.nazwa:('Czujnik #'+s.id);
nazwyMap[s.id]=nazwaDisplay;
let kolor=s.blad_czujnika?'#ff9f0a':kolorSat(s.id);
html+='<div class="card"><h2>'+nazwaDisplay+' <span onclick="zmienNazwe('+s.id+')" title="Zmien nazwe" style="cursor:pointer;font-size:0.9em;color:#aeaeb2;vertical-align:middle">&#9998;</span> <span class="sat-type">'+typIcon+' '+typName+fwInfo+'</span></h2>';
html+='<div style="text-align:center;margin-bottom:12px"><span class="'+badgeCls+'">'+badgeText+'</span></div>';
html+='<div class="temp-display"><span class="'+tempClass+'" style="color:'+kolor+'">'+tempVal+'</span><span class="temp-unit">&deg;C</span>'+trendHtml+'</div>';
html+='<div class="info-grid">';
if(s.typ===1){html+='<div class="info-item"><div class="info-label">Bateria</div><div class="info-value'+(s.bateria>15?' bat-ok':s.bateria>5?' bat-mid':' bat-low')+'">'+s.bateria+'%</div></div>';}
else{html+='<div class="info-item"><div class="info-label">Zasilanie</div><div class="info-value" style="color:#34c759">Sieciowe</div></div>';}
html+='<div class="info-item"><div class="info-label">Ostatni pomiar</div><div class="info-value" id="ago_'+s.id+'">'+s.ostatni+'</div></div>';
html+='</div>';
let monLabel=s.tylko_monitoring?'&#128065; Tylko monitoring':'&#128276; Alerty aktywne';
let monStyle=s.tylko_monitoring?'background:#f5f5f7;color:#6e6e73;border:1px solid #d2d2d7':'background:#e8f9ef;color:#1a7f3c;border:1px solid #b7f0cd';
html+='<div style="text-align:center;margin-top:12px">';
html+='<button style="'+monStyle+';padding:5px 14px;border-radius:20px;font-size:0.78em;cursor:pointer;font-weight:600" onclick="toggleMonitoring('+s.id+','+s.tylko_monitoring+')">'+monLabel+'</button>';
html+=' <button style="background:#fff0f0;color:#cc1c1c;border:1px solid #ffc9c9;padding:5px 12px;border-radius:20px;font-size:0.78em;cursor:pointer;font-weight:600" onclick="usunSatelite('+s.id+',\''+nazwaDisplay+'\')" >&#x2715; Usun</button>';
html+='</div>';
html+='<canvas id="chart_'+s.id+'" style="width:100%;margin-top:16px;border-radius:12px"></canvas>';
html+='</div>';
});
c.innerHTML=html;
loadCharts();
}

function loadCharts(){
fetch('/api/historia').then(r=>r.json()).then(arr=>{
arr.forEach(function(sat){
let cv=document.getElementById('chart_'+sat.id);
if(!cv||!sat.dane||sat.dane.length<2)return;
let ctx=cv.getContext('2d');
let w=cv.width=cv.offsetWidth;
let h=cv.height=window.innerWidth>=768?280:160;
let d=sat.dane;
let vmin=999,vmax=-999;
d.forEach(function(p){if(p.v<vmin)vmin=p.v;if(p.v>vmax)vmax=p.v});
let pad=0.5;vmin-=pad;vmax+=pad;
if(vmax-vmin<1){vmin-=0.5;vmax+=0.5}
let tmin=d[0].t,tmax=d[d.length-1].t;
if(tmax===tmin)tmax=tmin+1;
ctx.clearRect(0,0,w,h);
ctx.fillStyle='#fafafa';ctx.fillRect(0,0,w,h);
ctx.strokeStyle='#efefef';ctx.lineWidth=1;
for(let i=0;i<=4;i++){
let y=h-((i/4)*(h-30))-15;
ctx.beginPath();ctx.moveTo(40,y);ctx.lineTo(w-10,y);ctx.stroke();
let val=vmin+((i/4)*(vmax-vmin));
ctx.fillStyle='#aeaeb2';ctx.font='11px -apple-system,sans-serif';ctx.textAlign='right';
ctx.fillText(val.toFixed(1),36,y+4);
}
var c=kolorSat(sat.id);
var cr=parseInt(c.slice(1,3),16),cg=parseInt(c.slice(3,5),16),cb=parseInt(c.slice(5,7),16);
ctx.beginPath();
d.forEach(function(p,i){
let x=40+((p.t-tmin)/(tmax-tmin))*(w-50);
let y=h-15-((p.v-vmin)/(vmax-vmin))*(h-30);
if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);
});
ctx.lineTo(40+((d[d.length-1].t-tmin)/(tmax-tmin))*(w-50),h-15);
ctx.lineTo(40,h-15);ctx.closePath();
var grad=ctx.createLinearGradient(0,0,0,h);
grad.addColorStop(0,'rgba('+cr+','+cg+','+cb+',0.15)');
grad.addColorStop(1,'rgba('+cr+','+cg+','+cb+',0)');
ctx.fillStyle=grad;ctx.fill();
ctx.strokeStyle=kolorSat(sat.id);ctx.lineWidth=1.5;
ctx.beginPath();
d.forEach(function(p,i){
let x=40+((p.t-tmin)/(tmax-tmin))*(w-50);
let y=h-15-((p.v-vmin)/(vmax-vmin))*(h-30);
if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);
});
ctx.stroke();
// Kropki na każdej próbce
ctx.lineWidth=1.5;
d.forEach(function(p){
let x=40+((p.t-tmin)/(tmax-tmin))*(w-50);
let y=h-15-((p.v-vmin)/(vmax-vmin))*(h-30);
ctx.beginPath();ctx.arc(x,y,3,0,Math.PI*2);
ctx.fillStyle=kolorSat(sat.id);ctx.fill();
ctx.strokeStyle='#fff';ctx.stroke();
});
// Tooltip hover
cv._cd={d:d,tmin:tmin,tmax:tmax,vmin:vmin,vmax:vmax,sid:sat.id};
if(!cv._hl){cv._hl=true;
cv.addEventListener('mousemove',function(e){
var cd=cv._cd;if(!cd||!cd.d.length)return;
var r=cv.getBoundingClientRect();
var mx=(e.clientX-r.left)*(cv.width/r.width);
var best=null,bd=9999;
cd.d.forEach(function(p){
var x=40+((p.t-cd.tmin)/(cd.tmax-cd.tmin))*(cv.width-50);
var dx=Math.abs(mx-x);if(dx<bd){bd=dx;best=p;}
});
var tip=document.getElementById('chart-tip');
if(!tip){tip=document.createElement('div');tip.id='chart-tip';tip.style.cssText='position:fixed;background:rgba(30,30,30,0.85);color:#fff;padding:4px 10px;border-radius:6px;font-size:12px;pointer-events:none;white-space:nowrap;z-index:999';document.body.appendChild(tip);}
if(!best||bd>30){tip.style.display='none';return;}
var date=new Date(best.t*1000);
var ts=('0'+date.getHours()).slice(-2)+':'+('0'+date.getMinutes()).slice(-2);
tip.textContent=best.v.toFixed(1)+'\u00b0C \u2014 '+ts;
tip.style.display='block';tip.style.left=(e.clientX+14)+'px';tip.style.top=(e.clientY-32)+'px';
});
cv.addEventListener('mouseleave',function(){
var tip=document.getElementById('chart-tip');if(tip)tip.style.display='none';
});}
ctx.fillStyle='#aeaeb2';ctx.font='10px -apple-system,sans-serif';
[0,0.25,0.5,0.75,1].forEach(function(f){
let t=tmin+f*(tmax-tmin);
let date=new Date(t*1000);
let lbl=('0'+date.getHours()).slice(-2)+':'+('0'+date.getMinutes()).slice(-2);
let x=40+f*(w-50);
ctx.textAlign=f===1?'right':(f===0?'left':'center');
ctx.fillText(lbl,x,h-2);
});
});
}).catch(()=>{});
}

var _lastSatKey='';
function satKey(sats){
if(!sats)return'';
return JSON.stringify(sats.map(function(s){
return{id:s.id,t:s.temperatura,b:s.bateria,e:s.blad_czujnika,
a:s.aktywna,fw:s.fw,n:s.nazwa,tm:s.tylko_monitoring,
tr:s.trend!==undefined&&s.trend!==null?s.trend.toFixed(1):null};
}));
}
function update(){
fetch('/api/status').then(r=>r.json()).then(d=>{
document.getElementById('czas').textContent=d.czas_ntp;
document.getElementById('ver').textContent='v'+d.wersja;
document.getElementById('mac').textContent=d.mac;
document.getElementById('ip').textContent=d.ip;
document.getElementById('uptime').textContent=d.uptime;
document.getElementById('ntp').textContent=d.czas_ntp;
document.getElementById('satCount').textContent=d.satelity?d.satelity.length:'0';
// Aktualizuj "ostatni pomiar" bez przebudowy DOM
if(d.satelity)d.satelity.forEach(function(s){
var el=document.getElementById('ago_'+s.id);if(el)el.textContent=s.ostatni;
});
// Pełna przebudowa tylko gdy dane pomiarowe się zmieniły
var newKey=satKey(d.satelity);
if(newKey!==_lastSatKey){_lastSatKey=newKey;renderCzujniki(d.satelity);}
}).catch(e=>console.log(e));
if(_logOpen)odswiezLogi();
}
var _logOpen=false;
document.getElementById('logDetails').addEventListener('toggle',function(){
_logOpen=this.open;if(_logOpen)odswiezLogi();
});
function odswiezLogi(){
fetch('/api/log').then(r=>r.json()).then(function(lines){
var pre=document.getElementById('logPre');
pre.textContent=lines.join('\n');
pre.scrollTop=pre.scrollHeight;
}).catch(function(){document.getElementById('logPre').textContent='Blad pobierania logow';});
}
update();
setInterval(update,5000);

function toggleMonitoring(id,obecny){
fetch('/api/satelita/monitoring',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:id,tylko_monitoring:!obecny})})
.then(r=>r.json()).then(d=>{if(d.ok)update();});
}
function zmienNazwe(id){
var aktualna=nazwyMap[id]||'';
if(aktualna==='Czujnik #'+id)aktualna='';
var nowa=prompt('Nazwa czujnika #'+id+':',aktualna);
if(nowa===null)return;
fetch('/api/satelita/nazwa',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:id,nazwa:nowa.trim()})})
.then(r=>r.json()).then(d=>{if(d.ok)update();});
}
function usunSatelite(id,nazwa){
if(!confirm('Usunac czujnik "'+nazwa+'" (#'+id+') z listy?\n\nJesli wróci, zarejestruje sie ponownie automatycznie.'))return;
fetch('/api/satelita/usun',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({id:id})})
.then(r=>r.json()).then(d=>{if(d.ok)update();});
}

function uploadOTA(){
let f=document.getElementById('otaFile').files[0];
if(!f){document.getElementById('otaStatus').textContent='Wybierz plik .bin!';return}
let btn=document.getElementById('otaBtn');
btn.disabled=true;btn.textContent='Wgrywanie...';
document.getElementById('otaBarWrap').style.display='block';
let xhr=new XMLHttpRequest();
xhr.open('POST','/ota/matka');
xhr.upload.onprogress=function(e){
if(e.lengthComputable){
let p=Math.round(e.loaded/e.total*100);
document.getElementById('otaBar').style.width=p+'%';
document.getElementById('otaStatus').textContent=p+'%';
}};
xhr.onload=function(){
if(xhr.responseText==='OK'){
document.getElementById('otaStatus').textContent='OK! Matka sie restartuje...';
setTimeout(()=>location.reload(),5000);
}else{
document.getElementById('otaStatus').textContent='BLAD: '+xhr.responseText;
btn.disabled=false;btn.textContent='Wgraj firmware';
}};
xhr.onerror=function(){
document.getElementById('otaStatus').textContent='Blad polaczenia';
btn.disabled=false;btn.textContent='Wgraj firmware';
};
let fd=new FormData();fd.append('firmware',f);
xhr.send(fd);
}

async function uploadOTASat(){
let f=document.getElementById('otaSatFile').files[0];
if(!f){document.getElementById('otaSatStatus').textContent='Wybierz plik .bin!';return}
let btn=document.getElementById('otaSatBtn');
btn.disabled=true;btn.textContent='Wgrywanie...';
document.getElementById('otaSatBarWrap').style.display='block';
let st=document.getElementById('otaSatStatus');
let bar=document.getElementById('otaSatBar');
try{
let r=await fetch('/ota/satelita/begin?size='+f.size,{method:'POST'});
if(!r.ok)throw new Error('begin failed');
let chunkSize=8192;
let sent=0;
let buf=await f.arrayBuffer();
while(sent<f.size){
let end=Math.min(sent+chunkSize,f.size);
let chunk=buf.slice(sent,end);
let cr=await fetch('/ota/satelita/chunk',{method:'POST',body:chunk,headers:{'Content-Type':'application/octet-stream'}});
if(!cr.ok)throw new Error('chunk failed at '+sent);
sent=end;
let p=Math.round(sent/f.size*100);
bar.style.width=p+'%';
st.textContent=p+'% ('+Math.round(sent/1024)+'/'+Math.round(f.size/1024)+' KB)';
await new Promise(r=>setTimeout(r,50));
}
await new Promise(r=>setTimeout(r,300));
let fr=await fetch('/ota/satelita/finish',{method:'POST'});
let fsize=await fr.text();
if(parseInt(fsize)===f.size){
st.textContent='OK! '+Math.round(f.size/1024)+' KB zapisane. Satelity pobiorza przy nastepnym budzeniu.';
}else{
st.textContent='UWAGA: plik na Matce ma '+fsize+' bajtow (powinno byc '+f.size+')';
}
}catch(e){
st.textContent='BLAD: '+e.message;
}
btn.disabled=false;btn.textContent='Wgraj firmware Satelity';
}

(function(){
let s=document.getElementById('cfgCOd');
let e=document.getElementById('cfgCDo');
for(let i=0;i<24;i++){
let o=document.createElement('option');o.value=i;o.textContent=i+':00';
s.appendChild(o);
let o2=document.createElement('option');o2.value=i;o2.textContent=i+':00';
e.appendChild(o2);
}})();

function toggleMax(){document.getElementById('cfgMax').disabled=!document.getElementById('maxOn').checked}
function toggleMin(){document.getElementById('cfgMin').disabled=!document.getElementById('minOn').checked}
function toggleWzrost(){document.getElementById('cfgWzrost').disabled=!document.getElementById('wzrostOn').checked}
function toggleCichy(){let on=document.getElementById('cichyOn').checked;document.getElementById('cfgCOd').disabled=!on;document.getElementById('cfgCDo').disabled=!on}

function loadConfig(){
fetch('/api/ustawienia').then(r=>r.json()).then(d=>{
let maxOn=d.prog_max<50;
document.getElementById('maxOn').checked=maxOn;
document.getElementById('cfgMax').value=maxOn?d.prog_max:8;
document.getElementById('cfgMax').disabled=!maxOn;
let minOn=d.prog_min>-50;
document.getElementById('minOn').checked=minOn;
document.getElementById('cfgMin').value=minOn?d.prog_min:0;
document.getElementById('cfgMin').disabled=!minOn;
let wzrostOn=d.prog_wzrost>0;
document.getElementById('wzrostOn').checked=wzrostOn;
document.getElementById('cfgWzrost').value=wzrostOn?d.prog_wzrost:2;
document.getElementById('cfgWzrost').disabled=!wzrostOn;
document.getElementById('cfgInt').value=d.interwal_min;
document.getElementById('cfgIntZ').value=d.interwal_zasil_min;
document.getElementById('cfgAlertCykl').value=d.alert_cykl_min;
let cOn=d.cichy_od!==0||d.cichy_do!==0;
document.getElementById('cichyOn').checked=cOn;
document.getElementById('cfgCOd').value=d.cichy_od;
document.getElementById('cfgCDo').value=d.cichy_do;
document.getElementById('cfgCOd').disabled=!cOn;
document.getElementById('cfgCDo').disabled=!cOn;
})}
loadConfig();

function saveConfig(){
let btn=document.getElementById('cfgBtn');
btn.disabled=true;btn.textContent='Zapisuje...';
let body={
prog_max:document.getElementById('maxOn').checked?parseFloat(document.getElementById('cfgMax').value):99,
prog_min:document.getElementById('minOn').checked?parseFloat(document.getElementById('cfgMin').value):-99,
prog_wzrost:document.getElementById('wzrostOn').checked?parseFloat(document.getElementById('cfgWzrost').value):0,
interwal_min:parseInt(document.getElementById('cfgInt').value),
interwal_zasil_min:parseInt(document.getElementById('cfgIntZ').value),
alert_cykl_min:parseInt(document.getElementById('cfgAlertCykl').value),
cichy_od:document.getElementById('cichyOn').checked?parseInt(document.getElementById('cfgCOd').value):0,
cichy_do:document.getElementById('cichyOn').checked?parseInt(document.getElementById('cfgCDo').value):0
};
fetch('/api/ustawienia',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(r=>{
document.getElementById('cfgMsg').textContent=r.ok?'Zapisano!':'Blad';
document.getElementById('cfgMsg').style.color=r.ok?'#1a7f3c':'#cc1c1c';
btn.disabled=false;btn.textContent='Zapisz ustawienia';
setTimeout(()=>{document.getElementById('cfgMsg').textContent=''},3000);
}).catch(()=>{
document.getElementById('cfgMsg').textContent='Blad polaczenia';
document.getElementById('cfgMsg').style.color='#cc1c1c';
btn.disabled=false;btn.textContent='Zapisz ustawienia';
})}
</script>
<div class="card" style="margin-top:16px">
<details id="logDetails">
<summary style="cursor:pointer;font-size:1em;font-weight:600;color:#1d1d1f;padding:4px 0;user-select:none">Logi systemowe</summary>
<div style="margin-top:12px">
<button onclick="odswiezLogi()" style="font-size:0.8em;padding:6px 14px;border-radius:8px;border:1px solid #d1d1d6;background:#f5f5f7;cursor:pointer;margin-bottom:8px">Odswiez</button>
<pre id="logPre" style="font-size:0.75em;line-height:1.5;color:#3a3a3c;background:#f5f5f7;border-radius:8px;padding:12px;overflow-x:auto;white-space:pre-wrap;max-height:300px;overflow-y:auto;margin:0"></pre>
</div>
</details>
</div>
</div>
</body></html>
)rawliteral";

// === AP Mode Server ===

void setupAPServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send_P(200, "text/html", AP_PAGE);
    });

    server.on("/api/sys", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        doc["mac"] = WiFi.macAddress();
        doc["heap"] = ESP.getFreeHeap();
        String json;
        serializeJson(doc, json);
        req->send(200, "application/json", json);
    });

    server.on("/api/wifi-save", HTTP_POST, [](AsyncWebServerRequest *req) {},
        NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                req->send(400, "application/json", "{\"error\":\"json\"}");
                return;
            }
            const char *ssid = doc["ssid"] | "";
            const char *pass = doc["pass"] | "";
            if (strlen(ssid) == 0) {
                req->send(400, "application/json", "{\"error\":\"empty ssid\"}");
                return;
            }
            if (zapiszWiFiCreds(ssid, pass)) {
                req->send(200, "application/json", "{\"ok\":true}");
                Serial.printf("[AP] WiFi saved: %s — restarting\n", ssid);
                delay(1500);
                ESP.restart();
            } else {
                req->send(500, "application/json", "{\"error\":\"save failed\"}");
            }
        }
    );

    server.on("/api/wifi-scan", HTTP_GET, [](AsyncWebServerRequest *req) {
        int n = WiFi.scanComplete();
        if (n == WIFI_SCAN_FAILED) {
            WiFi.scanNetworks(true);
            req->send(200, "application/json", "[]");
            return;
        }
        if (n == WIFI_SCAN_RUNNING) {
            req->send(200, "application/json", "[]");
            return;
        }
        JsonDocument doc;
        JsonArray arr = doc.to<JsonArray>();
        for (int i = 0; i < n; i++) {
            String ssid = WiFi.SSID(i);
            if (ssid.length() == 0) continue;
            bool dup = false;
            for (JsonObject existing : arr) {
                if (ssid == existing["ssid"].as<const char*>()) {
                    if (WiFi.RSSI(i) > existing["rssi"].as<int>()) {
                        existing["rssi"] = WiFi.RSSI(i);
                    }
                    dup = true; break;
                }
            }
            if (!dup) {
                JsonObject o = arr.add<JsonObject>();
                o["ssid"] = ssid;
                o["rssi"] = WiFi.RSSI(i);
                o["enc"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
            }
        }
        WiFi.scanDelete();
        String json;
        serializeJson(doc, json);
        req->send(200, "application/json", json);
    });

    // Catchall → captive portal redirect
    server.onNotFound([](AsyncWebServerRequest *req) {
        req->redirect("http://192.168.4.1/");
    });

    dnsServer.start(53, "*", WiFi.softAPIP());
    server.begin();
    Serial.println("[AP] Captive portal aktywny — http://192.168.4.1");
}

// === STA Mode Server ===

void setupServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send_P(200, "text/html", DASHBOARD_HTML);
    });

    server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;

        doc["mac"] = WiFi.macAddress();
        doc["ip"] = ip_adres;
        doc["wersja"] = FW_VERSION;
        doc["czas_ntp"] = pobierzCzas();

        // Uptime
        unsigned long sek = millis() / 1000;
        String up = String(sek / 3600) + "h " + String((sek % 3600) / 60) + "min";
        doc["uptime"] = up;

        // Satelity
        JsonArray arr = doc["satelity"].to<JsonArray>();
        for (int i = 0; i < ile_satelit; i++) {
            SatelitaInfo *s = &satelity[i];
            JsonObject o = arr.add<JsonObject>();
            o["id"] = s->id;
            o["nazwa"] = s->nazwa;
            o["tylko_monitoring"] = s->tylko_monitoring;
            o["typ"] = s->typ;
            o["mac"] = macToString(s->mac);
            o["temperatura"] = s->pomiar.temperatura;
            o["bateria"] = s->pomiar.bateria_procent;
            o["blad_czujnika"] = s->pomiar.blad_czujnika;
            o["ostatni"] = czasOd(s->ostatni_czas);
            o["fw"] = s->pomiar.fw_version;

            // Trend °C/h z historii
            if (s->hist_count >= 2) {
                int last_idx = (s->hist_idx - 1 + MAX_HISTORIA_PER) % MAX_HISTORIA_PER;
                int prev_idx = (s->hist_idx - 2 + MAX_HISTORIA_PER) % MAX_HISTORIA_PER;
                if (!s->historia[last_idx].pusty && !s->historia[prev_idx].pusty) {
                    float dt = (float)(s->historia[last_idx].czas_unix - s->historia[prev_idx].czas_unix) / 3600.0f;
                    if (dt > 0) {
                        float rate = (s->historia[last_idx].temperatura - s->historia[prev_idx].temperatura) / dt;
                        o["trend"] = round(rate * 10.0f) / 10.0f;
                    }
                }
            }
        }

        String json;
        serializeJson(doc, json);
        req->send(200, "application/json", json);
    });

    // Historia — per satelita via ?id=X, lub wszystkie
    server.on("/api/historia", HTTP_GET, [](AsyncWebServerRequest *req) {
        int targetId = -1;
        if (req->hasParam("id")) {
            targetId = req->getParam("id")->value().toInt();
        }

        JsonDocument doc;
        JsonArray root = doc.to<JsonArray>();

        for (int si = 0; si < ile_satelit; si++) {
            SatelitaInfo *s = &satelity[si];
            if (targetId >= 0 && s->id != targetId) continue;

            JsonObject sat = root.add<JsonObject>();
            sat["id"] = s->id;
            JsonArray hist = sat["dane"].to<JsonArray>();
            for (int i = 0; i < s->hist_count; i++) {
                int idx = (s->hist_idx - s->hist_count + i + MAX_HISTORIA_PER) % MAX_HISTORIA_PER;
                if (s->historia[idx].pusty) continue;
                JsonObject entry = hist.add<JsonObject>();
                entry["t"] = s->historia[idx].czas_unix;
                entry["v"] = round(s->historia[idx].temperatura * 10.0f) / 10.0f;
            }
        }

        String json;
        serializeJson(doc, json);
        req->send(200, "application/json", json);
    });

    // Ustawienia GET
    server.on("/api/ustawienia", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        doc["prog_max"] = prog_max;
        doc["prog_min"] = prog_min;
        doc["prog_wzrost"] = prog_wzrost;
        doc["interwal_min"] = interwal_s / 60;
        doc["interwal_zasil_min"] = interwal_zasil_s / 60;
        doc["alert_cykl_min"] = alert_cykl_s / 60;
        doc["cichy_od"] = cichy_od;
        doc["cichy_do"] = cichy_do;
        String json;
        serializeJson(doc, json);
        req->send(200, "application/json", json);
    });

    // Ustawienia POST
    server.on("/api/ustawienia", HTTP_POST, [](AsyncWebServerRequest *req) {},
        NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, data, len);
            if (err) {
                req->send(400, "text/plain", "Blad JSON");
                return;
            }
            if (doc.containsKey("prog_max")) prog_max = doc["prog_max"].as<float>();
            if (doc.containsKey("prog_min")) prog_min = doc["prog_min"].as<float>();
            if (doc.containsKey("prog_wzrost")) prog_wzrost = doc["prog_wzrost"].as<float>();
            if (doc.containsKey("interwal_min")) interwal_s = doc["interwal_min"].as<int>() * 60;
            if (doc.containsKey("interwal_zasil_min")) interwal_zasil_s = doc["interwal_zasil_min"].as<int>() * 60;
            if (doc.containsKey("alert_cykl_min")) alert_cykl_s = doc["alert_cykl_min"].as<int>() * 60;
            if (doc.containsKey("cichy_od")) cichy_od = doc["cichy_od"].as<int>();
            if (doc.containsKey("cichy_do")) cichy_do = doc["cichy_do"].as<int>();
            zapiszPreferences();
            Serial.printf("[WWW] Ustawienia zapisane: max=%.1f min=%.1f wzrost=%.1f int=%ds cichy=%d-%d\n",
                prog_max, prog_min, prog_wzrost, interwal_s, cichy_od, cichy_do);
            req->send(200, "text/plain", "OK");
        }
    );

    // Nazwa satelity
    server.on("/api/satelita/nazwa", HTTP_POST, [](AsyncWebServerRequest *req) {},
        NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                req->send(400, "application/json", "{\"ok\":false}");
                return;
            }
            int id = doc["id"] | 0;
            const char *nazwa = doc["nazwa"] | "";
            if (id <= 0 || id >= MAX_SAT_ID) {
                req->send(400, "application/json", "{\"ok\":false}");
                return;
            }
            strlcpy(satellite_names[id], nazwa, 32);
            SatelitaInfo *s = znajdzSatelite(id);
            if (s) strlcpy(s->nazwa, nazwa, sizeof(s->nazwa));
            zapiszNazwy();
            req->send(200, "application/json", "{\"ok\":true}");
        }
    );

    // Flaga "tylko monitoring" per satelita
    server.on("/api/satelita/monitoring", HTTP_POST, [](AsyncWebServerRequest *req) {},
        NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                req->send(400, "application/json", "{\"ok\":false}");
                return;
            }
            int id = doc["id"] | 0;
            bool mon = doc["tylko_monitoring"] | false;
            if (id <= 0 || id >= MAX_SAT_ID) {
                req->send(400, "application/json", "{\"ok\":false}");
                return;
            }
            satellite_monitoring[id] = mon;
            SatelitaInfo *s = znajdzSatelite(id);
            if (s) s->tylko_monitoring = mon;
            zapiszMonitoring();
            req->send(200, "application/json", "{\"ok\":true}");
        }
    );

    // Usuniecie satelity z listy
    server.on("/api/satelita/usun", HTTP_POST, [](AsyncWebServerRequest *req) {},
        NULL,
        [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
            JsonDocument doc;
            if (deserializeJson(doc, data, len)) {
                req->send(400, "application/json", "{\"ok\":false}");
                return;
            }
            int id = doc["id"] | 0;
            if (id <= 0 || id >= MAX_SAT_ID) {
                req->send(400, "application/json", "{\"ok\":false}");
                return;
            }
            usunSatelite((uint8_t)id);
            req->send(200, "application/json", "{\"ok\":true}");
        }
    );

    // Logi systemowe
    server.on("/api/log", HTTP_GET, [](AsyncWebServerRequest *req) {
        String json = "[";
        int start = (log_count == LOG_BUF_SIZE) ? log_idx : 0;
        for (int i = 0; i < log_count; i++) {
            int idx = (start + i) % LOG_BUF_SIZE;
            if (i > 0) json += ",";
            json += "\"";
            for (int j = 0; log_buf[idx][j]; j++) {
                char c = log_buf[idx][j];
                if (c == '"') json += "\\\"";
                else if (c == '\\') json += "\\\\";
                else json += c;
            }
            json += "\"";
        }
        json += "]";
        req->send(200, "application/json", json);
    });

    // WiFi reset
    server.on("/api/wifi-reset", HTTP_POST, [](AsyncWebServerRequest *req) {
        usunWiFiCreds();
        req->send(200, "text/plain", "OK — restart w tryb konfiguracji");
        delay(1000);
        ESP.restart();
    });

    // OTA Matki
    server.on("/ota/matka", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            bool ok = !Update.hasError();
            req->send(200, "text/plain", ok ? "OK" : "BLAD");
            if (ok) {
                addLog("[OTA] Sukces! Restart...");
                delay(500);
                ESP.restart();
            }
        },
        [](AsyncWebServerRequest *req, const String &filename, size_t index,
           uint8_t *data, size_t len, bool final) {
            if (!index) {
                Serial.printf("[OTA] Start: %s\n", filename.c_str());
                if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
                    Update.printError(Serial);
                }
            }
            if (Update.isRunning()) {
                if (Update.write(data, len) != len) {
                    Update.printError(Serial);
                }
            }
            if (final) {
                if (Update.end(true)) {
                    Serial.printf("[OTA] Gotowe: %u bajtow\n", index + len);
                } else {
                    Update.printError(Serial);
                }
            }
        }
    );

    // OTA Satelity — chunked upload API
    // POST /ota/satelita/begin?size=XXXXX — buforuj w PSRAM (LittleFS niedostępny podczas WiFi RX)
    server.on("/ota/satelita/begin", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!req->hasParam("size")) {
            req->send(400, "text/plain", "brak size");
            return;
        }
        int totalSize = req->getParam("size")->value().toInt();
        addLog("[OTA-SAT] Begin: %d bajtow", totalSize);
        if (ota_buf) { free(ota_buf); ota_buf = nullptr; }
        ota_buf = (uint8_t *)ps_malloc(totalSize);
        if (!ota_buf) {
            req->send(500, "text/plain", "BLAD alokacji PSRAM");
            return;
        }
        ota_buf_size = totalSize;
        ota_buf_offset = 0;
        req->send(200, "text/plain", "OK");
    });

    // POST /ota/satelita/chunk — memcpy do PSRAM (szybkie, bez zapisu flash)
    server.on("/ota/satelita/chunk", HTTP_POST, [](AsyncWebServerRequest *req) {
        req->send(200, "text/plain", "OK");
    }, NULL,
    [](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total) {
        if (ota_buf && ota_buf_offset + len <= ota_buf_size) {
            memcpy(ota_buf + ota_buf_offset, data, len);
            ota_buf_offset += len;
        }
    });

    // POST /ota/satelita/finish — ustaw flagę, zapis w loop() żeby nie blokować watchdoga
    server.on("/ota/satelita/finish", HTTP_POST, [](AsyncWebServerRequest *req) {
        if (!ota_buf || ota_buf_offset == 0) {
            req->send(500, "text/plain", "Brak danych w buforze");
            return;
        }
        if (ota_buf_offset != ota_buf_size) {
            addLog("[OTA-SAT] BLAD: niepelny upload %d / %d", ota_buf_offset, ota_buf_size);
            free(ota_buf); ota_buf = nullptr; ota_buf_offset = 0; ota_buf_size = 0;
            req->send(400, "text/plain", "Niepelny upload: " + String(ota_buf_offset) + " / " + String(ota_buf_size));
            return;
        }
        size_t fileSize = ota_buf_offset;
        ota_write_pending = true;
        addLog("[OTA-SAT] Finish: %d bajtow — zapis w tle", fileSize);
        req->send(200, "text/plain", String(fileSize));
    });

    // Serwowanie .bin dla Satelity — z PSRAM jeśli dostępny (szybkie), fallback LittleFS
    server.on("/ota/satelita.bin", HTTP_GET, [](AsyncWebServerRequest *req) {
        if (ota_buf && ota_buf_offset > 0) {
            // Bezpośrednio z PSRAM — brak granic bloków LittleFS, pełne Content-Length
            size_t total = ota_buf_offset;
            AsyncWebServerResponse *resp = req->beginResponse("application/octet-stream", total,
                [total](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
                    if (index >= total) return 0;
                    size_t toSend = min(maxLen, total - index);
                    memcpy(buffer, ota_buf + index, toSend);
                    return toSend;
                });
            req->send(resp);
            return;
        }
        if (!LittleFS.exists("/ota/satelita.bin")) {
            req->send(404, "text/plain", "Brak pliku");
            return;
        }
        req->send(LittleFS, "/ota/satelita.bin", "application/octet-stream");
    });

    server.begin();
    Serial.println("[OK] Serwer WWW aktywny");
}

// === Wyświetlanie pomiaru ===

void wyswietlPomiar(SatelitaInfo *s) {
    Serial.println("========================================");
    Serial.printf("  Czujnik #%d (%s)\n", s->id, s->typ == 1 ? "bateria" : "zasilacz");
    if (s->pomiar.blad_czujnika) {
        Serial.println("  Temperatura:   BLAD CZUJNIKA!");
    } else {
        Serial.printf("  Temperatura:   %.2f C\n", s->pomiar.temperatura);
    }
    if (s->typ == 1) Serial.printf("  Bateria:       %d%%\n", s->pomiar.bateria_procent);
    Serial.println("========================================");
}

// === Setup ===

void startAPMode() {
    tryb_ap = true;
    ap_start_time = millis();
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID);
    delay(100);
    Serial.printf("[AP] Tryb konfiguracji — SSID: %s\n", AP_SSID);
    Serial.printf("[AP] IP: %s\n", WiFi.softAPIP().toString().c_str());
    setupAPServer();
}

bool connectWiFi(const char *ssid, const char *pass) {
    WiFi.mode(WIFI_STA);
    Serial.printf("Lacze z WiFi: %s...\n", ssid);
    WiFi.begin(ssid, pass);
    int proby = 0;
    while (WiFi.status() != WL_CONNECTED && proby < 30) {
        delay(500);
        Serial.print(".");
        proby++;
    }
    if (WiFi.status() == WL_CONNECTED) {
        ip_adres = WiFi.localIP().toString();
        addLog("[OK] WiFi polaczone! IP: %s", ip_adres.c_str());
        return true;
    }
    addLog("[BLAD] WiFi nie polaczone!");
    return false;
}

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println();
    Serial.println("================================");
    Serial.printf("  MATKA — Smart Mleko v%s\n", FW_VERSION);
    Serial.println("================================");

    // LittleFS
    if (!LittleFS.begin(true)) {
        addLog("[FS] LittleFS mount failed!");
    }
    wczytajNazwy();
    wczytajMonitoring();

    // Preferences
    wczytajPreferences();

    // WiFi — próba połączenia z zapisanymi credentials
    WiFi.mode(WIFI_STA);
    Serial.printf("MAC adres Matki: %s\n", WiFi.macAddress().c_str());

    char ssid[64] = {0};
    char pass[64] = {0};
    bool polaczono = false;

    if (wczytajWiFiCreds(ssid, pass)) {
        polaczono = connectWiFi(ssid, pass);
    }

    // Fallback: jeśli brak zapisanych creds, spróbuj domyślne i zapisz
    if (!polaczono) {
        strlcpy(ssid, DEFAULT_WIFI_SSID, sizeof(ssid));
        strlcpy(pass, DEFAULT_WIFI_PASS, sizeof(pass));
        polaczono = connectWiFi(ssid, pass);
        if (polaczono) {
            zapiszWiFiCreds(ssid, pass);
            Serial.println("[WiFi] Zapisano domyslne creds do LittleFS");
        }
    }

    if (!polaczono) {
        Serial.println("[WiFi] Nie udalo sie polaczyc — tryb AP");
        startAPMode();
        return; // loop obsłuży AP
    }

    // NTP
    configTzTime(NTP_STREFA, "pool.ntp.org", "time.google.com");
    struct tm info;
    int ntp_proby = 0;
    while (!getLocalTime(&info) && ntp_proby++ < 10) delay(500);
    if (ntp_proby < 10) {
        Serial.printf("[OK] NTP: %s\n", pobierzCzas().c_str());
    } else {
        addLog("[WARN] Brak synchronizacji NTP!");
    }

    // mDNS
    if (MDNS.begin("mleko")) {
        Serial.println("[OK] mDNS: http://mleko.local");
    }

    // ESP-NOW — musi być po WiFi.begin() żeby kanał się zgadzał
    if (esp_now_init() != ESP_OK) {
        Serial.println("[BLAD] ESP-NOW init!");
        return;
    }
    esp_now_register_recv_cb(onDataRecv);
    esp_now_register_send_cb(onDataSent);

    uint8_t kanal;
    wifi_second_chan_t second;
    esp_wifi_get_channel(&kanal, &second);
    addLog("[OK] ESP-NOW na kanale %d", kanal);

    // Serwer WWW
    setupServer();

    // Telegram — wiadomość startowa
    wyslijTelegram("✅ <b>Smart Mleko v" FW_VERSION "</b>\nMatka uruchomiona\nIP: " + ip_adres);

    Serial.println();
    Serial.println(">>> Dashboard: http://mleko.local");
    Serial.printf(">>> Lub: http://%s\n", ip_adres.c_str());
    Serial.println(">>> Czekam na dane z Satelitow...");
    Serial.println();
}

// === Loop ===

void loop() {
    // Zapis PSRAM → LittleFS w kawałkach żeby nie blokować watchdoga
    if (ota_write_pending && ota_buf && ota_buf_offset > 0) {
        addLog("[OTA-SAT] Zapisuje %d bajtow do LittleFS...", ota_buf_offset);
        LittleFS.mkdir("/ota");
        LittleFS.remove("/ota/satelita.bin");
        File f = LittleFS.open("/ota/satelita.bin", "w");
        if (f) {
            const size_t CHUNK = 8192;
            size_t offset = 0;
            while (offset < ota_buf_offset) {
                size_t n = min(CHUNK, ota_buf_offset - offset);
                f.write(ota_buf + offset, n);
                offset += n;
                delay(1);
            }
            f.close();
            addLog("[OTA-SAT] Zapisano %d bajtow — flaga OTA dla %d satelitow", ota_buf_offset, ile_satelit);
            for (int i = 0; i < ile_satelit; i++) satelity[i].ota_pending = true;
        } else {
            addLog("[OTA-SAT] BLAD otwarcia pliku!");
        }
        // NIE zwalniamy ota_buf — serwujemy bezposrednio z PSRAM (szybsze, bez granic blokow LittleFS)
        ota_write_pending = false;
    }

    if (tryb_ap) {
        dnsServer.processNextRequest();
        // Timeout AP — restart po 3 min
        if (millis() - ap_start_time > AP_TIMEOUT_MS) {
            Serial.println("[AP] Timeout — restart");
            ESP.restart();
        }
        delay(10);
        return;
    }

    // Auto-reconnect WiFi — restart jeśli brak połączenia >60s
    static unsigned long wifi_utracone = 0;
    if (WiFi.status() != WL_CONNECTED) {
        if (wifi_utracone == 0) {
            wifi_utracone = millis();
            addLog("[WiFi] Polaczenie utracone!");
        } else if (millis() - wifi_utracone > 60000) {
            Serial.println("[WiFi] Brak WiFi >60s — restart");
            ESP.restart();
        }
    } else {
        wifi_utracone = 0;
    }

    // Przetwarzanie nowych pomiarów
    static unsigned long ostatnie_sprawdzenie = 0;
    if (millis() - ostatnie_sprawdzenie > 1000) {
        ostatnie_sprawdzenie = millis();
        for (int i = 0; i < ile_satelit; i++) {
            SatelitaInfo *s = &satelity[i];
            // Jeśli pomiar jest świeży (< 2s temu) i jeszcze nie przetworzony
            if (s->ostatni_czas > 0 && (millis() - s->ostatni_czas) < 10000) {
                // Sprawdź czy ten pomiar był już przetworzony
                static unsigned long przetworzone[MAX_SATELITY] = {0};
                if (przetworzone[i] != s->ostatni_czas) {
                    przetworzone[i] = s->ostatni_czas;
                    wyswietlPomiar(s);
                    sprawdzAlerty(s);
                    if (!s->pomiar.blad_czujnika) {
                        dodajDoHistorii(s, s->pomiar.temperatura);
                        if (s->hist_idx % 4 == 0) zapiszHistorie(s);
                    }
                }
            }
        }
    }

    sprawdzTelegram();
    sprawdzHeartbeat();
    delay(10);
}
