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

#define FW_VERSION "4.1"

// === WiFi ===

#define AP_SSID "Smart_Mleko_Setup"
#define AP_TIMEOUT_MS 180000  // 3 min
#define WIFI_CREDS_PATH "/wifi.json"

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
const unsigned long POLL_INTERVAL = 3000; // sprawdzaj komendy co 3s

// === Preferences ===

Preferences prefs;
float prog_max = 8.0;   // alarm gdy temp powyżej
float prog_min = -99.0; // -99 = wyłączony; alarm gdy temp poniżej
uint32_t interwal_s = 1800; // 30 min
uint8_t cichy_od = 0;  // 0 = wyłączony
uint8_t cichy_do = 0;

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

AsyncWebServer server(80);

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
    for (int i = 0; i < MAX_HISTORIA_PER; i++) s->historia[i].pusty = true;
    Serial.printf("[SAT] Nowa satelita #%d typ=%d MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
        id, s->typ, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return s;
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

void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
    if (len != sizeof(struct_message)) return;

    struct_message msg;
    memcpy(&msg, data, sizeof(struct_message));

    SatelitaInfo *s = znajdzLubDodajSatelite(msg.id_czujnika, msg.typ_zasilania, mac);
    if (!s) {
        Serial.println("[ESP-NOW] Brak miejsca na nowa satelite!");
        return;
    }

    memcpy(&s->pomiar, &msg, sizeof(struct_message));
    s->ostatni_czas = millis();
    s->aktywna = true;

    Serial.printf("[ESP-NOW] Satelita #%d od %s: %.1f°C bat=%d%%\n",
        msg.id_czujnika, macToString(mac).c_str(),
        msg.temperatura, msg.bateria_procent);

    // Reset OTA po udanym update
    if (s->ota_pending && !msg.blad_czujnika) {
        s->ota_pending = false;
        Serial.printf("[OTA] Satelita #%d zaktualizowana\n", s->id);
        // Kasuj .bin dopiero gdy WSZYSTKIE satelity się zaktualizowały
        bool ktos_czeka = false;
        for (int i = 0; i < ile_satelit; i++) {
            if (satelity[i].ota_pending) { ktos_czeka = true; break; }
        }
        if (!ktos_czeka) {
            LittleFS.remove("/ota/satelita.bin");
            Serial.println("[OTA] Wszystkie zaktualizowane — kasuje .bin");
        }
    }
}

void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
    Serial.printf("[ESP-NOW] ACK: %s\n",
        status == ESP_NOW_SEND_SUCCESS ? "OK" : "BLAD");
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
    ack.nowy_interwal_s = interwal_s;
    ack.godzina_start = 0;
    ack.godzina_stop = 0;
    ack.ota_pending = s->ota_pending;
    memset(ack.ota_url, 0, sizeof(ack.ota_url));
    if (s->ota_pending) {
        String url = "http://" + ip_adres + "/ota/satelita.bin";
        strlcpy(ack.ota_url, url.c_str(), sizeof(ack.ota_url));
        Serial.printf("[ACK] OTA dla #%d: %s\n", s->id, ack.ota_url);
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
        Serial.printf("[Telegram] Wyslano (HTTP %d)\n", code);
    } else {
        Serial.printf("[Telegram] Blad: %s\n", http.errorToString(code).c_str());
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
    if (millis() - ostatni_telegram < TELEGRAM_COOLDOWN && ostatni_telegram > 0) return;

    String prefix = "Czujnik #" + String(s->id) + ": ";
    String msg = "";
    bool krytyczny = false;

    if (s->pomiar.blad_czujnika) {
        msg = "⚠️ <b>Smart Mleko</b>\n" + prefix + "Błąd czujnika temperatury!";
        krytyczny = true;
    } else if (s->pomiar.temperatura > prog_max) {
        msg = "🔴 <b>Smart Mleko</b>\n" + prefix + "Temperatura za wysoka: <b>";
        msg += String(s->pomiar.temperatura, 1);
        msg += "°C</b> (próg: " + String(prog_max, 1) + "°C)";
        krytyczny = true;
    } else if (prog_min > -50 && s->pomiar.temperatura < prog_min) {
        msg = "🔵 <b>Smart Mleko</b>\n" + prefix + "Temperatura za niska: <b>";
        msg += String(s->pomiar.temperatura, 1);
        msg += "°C</b> (próg: " + String(prog_min, 1) + "°C)";
        krytyczny = true;
    } else if (s->pomiar.bateria_procent <= 5 && s->typ == 1) {
        msg = "🔋 <b>Smart Mleko</b>\n" + prefix + "Bateria krytyczna: <b>";
        msg += String(s->pomiar.bateria_procent);
        msg += "%</b>";
        krytyczny = true;
    } else if (s->pomiar.bateria_procent <= 15 && s->typ == 1) {
        msg = "🔋 <b>Smart Mleko</b>\n" + prefix + "Niski poziom baterii: <b>";
        msg += String(s->pomiar.bateria_procent);
        msg += "%</b>";
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

        // Bateriowe: timeout = 3× interwał, zasilaczowe: 5 min
        unsigned long timeout = (s->typ == 1) ? interwal_s * 3000UL : 300000UL;
        if (timeout < 300000UL) timeout = 300000UL; // minimum 5 min

        if ((millis() - s->ostatni_czas) > timeout) {
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
    interwal_s = prefs.getUInt("interwal", 1800);
    cichy_od = prefs.getUChar("cichy_od", 0);
    cichy_do = prefs.getUChar("cichy_do", 0);
    Serial.printf("[Prefs] max=%.1f min=%.1f interwal=%ds cichy=%d-%d\n",
        prog_max, prog_min, interwal_s, cichy_od, cichy_do);
}

void zapiszPreferences() {
    prefs.putFloat("prog_max", prog_max);
    prefs.putFloat("prog_min", prog_min);
    prefs.putUInt("interwal", interwal_s);
    prefs.putUChar("cichy_od", cichy_od);
    prefs.putUChar("cichy_do", cichy_do);
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
        msg += "Wpisz <i>wyl</i> zamiast liczby żeby wyłączyć\n\n";
        msg += "<b>Pomiary:</b>\n";
        msg += "/interwal 30 — co ile minut mierzyć\n\n";
        msg += "<b>Powiadomienia:</b>\n";
        msg += "/cichy 23 7 — wycisz łagodne alerty\n";
        msg += "/cichy 0 0 — wyłącz tryb cichy\n\n";
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
                        Serial.printf("[Telegram] Komenda: %s\n", tekst.c_str());
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
body{font-family:-apple-system,system-ui,sans-serif;background:#0f172a;color:#e2e8f0;min-height:100vh;padding:20px}
.header{text-align:center;margin-bottom:30px}
.header h1{font-size:2em;color:#38bdf8}
.header .status{font-size:0.9em;color:#94a3b8;margin-top:5px}
.card{background:#1e293b;border-radius:16px;padding:24px;margin-bottom:20px;border:1px solid #334155}
.card h2{color:#94a3b8;font-size:0.85em;text-transform:uppercase;letter-spacing:1px;margin-bottom:16px}
.temp-display{text-align:center;padding:20px 0}
.temp-value{font-size:4em;font-weight:700;color:#22c55e}
.temp-value.alarm{color:#ef4444}
.temp-value.blad{color:#f59e0b}
.temp-unit{font-size:1.5em;color:#94a3b8}
.info-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.info-item{background:#0f172a;border-radius:10px;padding:14px}
.info-label{font-size:0.75em;color:#64748b;text-transform:uppercase}
.info-value{font-size:1.3em;font-weight:600;margin-top:4px}
.bat-ok{color:#22c55e} .bat-mid{color:#f59e0b} .bat-low{color:#ef4444}
.status-badge{display:inline-block;padding:6px 14px;border-radius:20px;font-size:0.85em;font-weight:600}
.status-ok{background:#166534;color:#4ade80}
.status-wait{background:#854d0e;color:#fbbf24}
.status-err{background:#991b1b;color:#fca5a5}
.sys-info{font-size:0.8em;color:#64748b;line-height:1.8}
.refresh-note{text-align:center;font-size:0.75em;color:#475569;margin-top:10px}
.cfg-row{display:flex;align-items:center;justify-content:space-between;margin-bottom:14px}
.cfg-row label{font-size:0.85em;color:#94a3b8;flex:1}
.cfg-row input,.cfg-row select{background:#0f172a;border:1px solid #334155;color:#e2e8f0;border-radius:8px;padding:8px 12px;width:90px;font-size:0.95em;text-align:center}
.cfg-row .cfg-hint{font-size:0.7em;color:#64748b;margin-top:2px}
.cfg-btn{background:#16a34a;color:white;border:none;padding:12px 24px;border-radius:10px;font-size:1em;cursor:pointer;width:100%;margin-top:8px}
.cfg-btn:disabled{background:#334155;cursor:default}
.cfg-msg{text-align:center;font-size:0.85em;margin-top:10px;min-height:1.2em}
.cfg-check{display:flex;align-items:center;gap:8px}
.cfg-check input[type=checkbox]{width:18px;height:18px;accent-color:#16a34a}
.sat-type{font-size:0.7em;color:#64748b;margin-left:6px}
</style>
</head>
<body>
<div class="header">
<h1>Smart Mleko <span id="ver"></span></h1>
<div class="status" id="czas">---</div>
</div>

<div id="czujniki"></div>

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
<div><label>Alarm gorny</label><div class="cfg-hint">Powiadomienie gdy temp powyej</div></div>
<div class="cfg-check"><input type="checkbox" id="maxOn" onchange="toggleMax()"><input type="number" id="cfgMax" step="0.5" style="width:75px"></div>
</div>
<div class="cfg-row">
<div><label>Alarm dolny</label><div class="cfg-hint">Powiadomienie gdy temp poniej</div></div>
<div class="cfg-check"><input type="checkbox" id="minOn" onchange="toggleMin()"><input type="number" id="cfgMin" step="0.5" style="width:75px"></div>
</div>
<div class="cfg-row">
<div><label>Interwal pomiaru</label><div class="cfg-hint">Co ile minut czujnik mierzy</div></div>
<select id="cfgInt"><option value="5">5 min</option><option value="10">10 min</option><option value="15">15 min</option><option value="30">30 min</option><option value="60">1 godz</option><option value="120">2 godz</option></select>
</div>
<div class="cfg-row">
<div><label>Tryb cichy</label><div class="cfg-hint">Wycisz lagodne alerty w nocy</div></div>
<div class="cfg-check"><input type="checkbox" id="cichyOn" onchange="toggleCichy()">
<select id="cfgCOd" style="width:65px"><option value="0">--</option></select>
<span style="color:#64748b">-</span>
<select id="cfgCDo" style="width:65px"><option value="0">--</option></select></div>
</div>
<button class="cfg-btn" id="cfgBtn" onclick="saveConfig()">Zapisz ustawienia</button>
<div class="cfg-msg" id="cfgMsg"></div>
</div>

<div class="card">
<h2>Aktualizacja OTA — Matka</h2>
<div style="margin-bottom:12px">
<label style="font-size:0.85em;color:#94a3b8">Firmware Matki (.bin):</label>
<input type="file" id="otaFile" accept=".bin" style="margin-top:8px;color:#e2e8f0;font-size:0.85em">
</div>
<button onclick="uploadOTA()" id="otaBtn" style="background:#2563eb;color:white;border:none;padding:12px 24px;border-radius:10px;font-size:1em;cursor:pointer;width:100%">Wgraj firmware</button>
<div id="otaStatus" style="margin-top:12px;font-size:0.85em;color:#94a3b8;text-align:center"></div>
<div style="background:#0f172a;border-radius:8px;height:8px;margin-top:8px;display:none" id="otaBarWrap">
<div id="otaBar" style="background:#2563eb;height:100%;border-radius:8px;width:0%;transition:width 0.3s"></div>
</div>
</div>

<div class="card">
<h2>Aktualizacja OTA — Satelita</h2>
<div style="margin-bottom:4px;font-size:0.8em;color:#64748b">Jeden firmware dla wszystkich satelitow (ID i typ w Preferences)</div>
<div style="margin-bottom:12px">
<label style="font-size:0.85em;color:#94a3b8">Firmware Satelity (.bin):</label>
<input type="file" id="otaSatFile" accept=".bin" style="margin-top:8px;color:#e2e8f0;font-size:0.85em">
</div>
<button onclick="uploadOTASat()" id="otaSatBtn" style="background:#7c3aed;color:white;border:none;padding:12px 24px;border-radius:10px;font-size:1em;cursor:pointer;width:100%">Wgraj firmware Satelity</button>
<div id="otaSatStatus" style="margin-top:12px;font-size:0.85em;color:#94a3b8;text-align:center"></div>
<div style="background:#0f172a;border-radius:8px;height:8px;margin-top:8px;display:none" id="otaSatBarWrap">
<div id="otaSatBar" style="background:#7c3aed;height:100%;border-radius:8px;width:0%;transition:width 0.3s"></div>
</div>
</div>

<div class="refresh-note">Odswiezanie co 5 sekund</div>

<script>
function renderCzujniki(satelity){
let c=document.getElementById('czujniki');
if(!satelity||!satelity.length){
c.innerHTML='<div class="card"><h2>Czujniki</h2><div style="text-align:center;color:#64748b;padding:20px">Brak polaczonych czujnikow.<br>Podlacz Satelite — pojawi sie automatycznie.</div></div>';
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
if(s.trend>0.1){arrow='\u2191';cls='color:#ef4444'}
else if(s.trend<-0.1){arrow='\u2193';cls='color:#22c55e'}
else{arrow='\u2192';cls='color:#94a3b8'}
let sign=s.trend>=0?'+':'';
trendHtml='<div style="font-size:1.2em;margin-top:8px"><span style="'+cls+'">'+arrow+' '+sign+s.trend.toFixed(1)+'\u00b0C/h</span></div>';
}
let fwInfo=s.fw?' v'+s.fw:'';
html+='<div class="card"><h2>Czujnik #'+s.id+' <span class="sat-type">'+typIcon+' '+typName+fwInfo+'</span></h2>';
html+='<div style="text-align:center;margin-bottom:12px"><span class="'+badgeCls+'">'+badgeText+'</span></div>';
html+='<div class="temp-display"><span class="'+tempClass+'">'+tempVal+'</span><span class="temp-unit">&deg;C</span>'+trendHtml+'</div>';
html+='<div class="info-grid">';
if(s.typ===1){html+='<div class="info-item"><div class="info-label">Bateria</div><div class="info-value'+(s.bateria>15?' bat-ok':s.bateria>5?' bat-mid':' bat-low')+'">'+s.bateria+'%</div></div>';}
else{html+='<div class="info-item"><div class="info-label">Zasilanie</div><div class="info-value" style="color:#22c55e">Sieciowe</div></div>';}
html+='<div class="info-item"><div class="info-label">Ostatni pomiar</div><div class="info-value">'+s.ostatni+'</div></div>';
html+='</div>';
html+='<canvas id="chart_'+s.id+'" height="150" style="width:100%;margin-top:16px;background:#0f172a;border-radius:10px"></canvas>';
html+='</div>';
});
c.innerHTML=html;
// Rysuj wykresy
loadCharts();
}

function loadCharts(){
fetch('/api/historia').then(r=>r.json()).then(arr=>{
arr.forEach(function(sat){
let cv=document.getElementById('chart_'+sat.id);
if(!cv||!sat.dane||sat.dane.length<2)return;
let ctx=cv.getContext('2d');
let w=cv.width=cv.offsetWidth;
let h=cv.height=150;
let d=sat.dane;
let vmin=999,vmax=-999;
d.forEach(function(p){if(p.v<vmin)vmin=p.v;if(p.v>vmax)vmax=p.v});
let pad=0.5;vmin-=pad;vmax+=pad;
if(vmax-vmin<1){vmin-=0.5;vmax+=0.5}
let tmin=d[0].t,tmax=d[d.length-1].t;
if(tmax===tmin)tmax=tmin+1;
ctx.clearRect(0,0,w,h);
// Siatka
ctx.strokeStyle='#1e293b';ctx.lineWidth=1;
for(let i=0;i<=4;i++){
let y=h-((i/4)*(h-30))-15;
ctx.beginPath();ctx.moveTo(40,y);ctx.lineTo(w-10,y);ctx.stroke();
let val=vmin+((i/4)*(vmax-vmin));
ctx.fillStyle='#64748b';ctx.font='11px sans-serif';ctx.textAlign='right';
ctx.fillText(val.toFixed(1),36,y+4);
}
// Linia
ctx.strokeStyle='#38bdf8';ctx.lineWidth=2;
ctx.beginPath();
d.forEach(function(p,i){
let x=40+((p.t-tmin)/(tmax-tmin))*(w-50);
let y=h-15-((p.v-vmin)/(vmax-vmin))*(h-30);
if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);
});
ctx.stroke();
// Oś czasu
ctx.fillStyle='#64748b';ctx.font='10px sans-serif';ctx.textAlign='center';
[0,0.25,0.5,0.75,1].forEach(function(f){
let t=tmin+f*(tmax-tmin);
let date=new Date(t*1000);
let lbl=('0'+date.getHours()).slice(-2)+':'+('0'+date.getMinutes()).slice(-2);
let x=40+f*(w-50);
ctx.fillText(lbl,x,h-2);
});
});
}).catch(()=>{});
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
renderCzujniki(d.satelity);
}).catch(e=>console.log(e));
}
update();
setInterval(update,5000);

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

function uploadOTASat(){
let f=document.getElementById('otaSatFile').files[0];
if(!f){document.getElementById('otaSatStatus').textContent='Wybierz plik .bin!';return}
let btn=document.getElementById('otaSatBtn');
btn.disabled=true;btn.textContent='Wgrywanie...';
document.getElementById('otaSatBarWrap').style.display='block';
let xhr=new XMLHttpRequest();
xhr.open('POST','/ota/satelita');
xhr.upload.onprogress=function(e){
if(e.lengthComputable){
let p=Math.round(e.loaded/e.total*100);
document.getElementById('otaSatBar').style.width=p+'%';
document.getElementById('otaSatStatus').textContent=p+'%';
}};
xhr.onload=function(){
if(xhr.responseText==='OK'){
document.getElementById('otaSatStatus').textContent='Zapisano! Wszystkie satelity pobiorza przy nastepnym budzeniu.';
}else{
document.getElementById('otaSatStatus').textContent='BLAD: '+xhr.responseText;
}
btn.disabled=false;btn.textContent='Wgraj firmware Satelity';
};
xhr.onerror=function(){
document.getElementById('otaSatStatus').textContent='Blad polaczenia';
btn.disabled=false;btn.textContent='Wgraj firmware Satelity';
};
let fd=new FormData();fd.append('firmware',f);
xhr.send(fd);
}

// === Ustawienia ===
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
document.getElementById('cfgInt').value=d.interwal_min;
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
interwal_min:parseInt(document.getElementById('cfgInt').value),
cichy_od:document.getElementById('cichyOn').checked?parseInt(document.getElementById('cfgCOd').value):0,
cichy_do:document.getElementById('cichyOn').checked?parseInt(document.getElementById('cfgCDo').value):0
};
fetch('/api/ustawienia',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(r=>{
document.getElementById('cfgMsg').textContent=r.ok?'Zapisano!':'Blad';
document.getElementById('cfgMsg').style.color=r.ok?'#4ade80':'#fca5a5';
btn.disabled=false;btn.textContent='Zapisz ustawienia';
setTimeout(()=>{document.getElementById('cfgMsg').textContent=''},3000);
}).catch(()=>{
document.getElementById('cfgMsg').textContent='Blad polaczenia';
document.getElementById('cfgMsg').style.color='#fca5a5';
btn.disabled=false;btn.textContent='Zapisz ustawienia';
})}
</script>
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
        doc["interwal_min"] = interwal_s / 60;
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
            if (doc.containsKey("interwal_min")) interwal_s = doc["interwal_min"].as<int>() * 60;
            if (doc.containsKey("cichy_od")) cichy_od = doc["cichy_od"].as<int>();
            if (doc.containsKey("cichy_do")) cichy_do = doc["cichy_do"].as<int>();
            zapiszPreferences();
            Serial.printf("[WWW] Ustawienia zapisane: max=%.1f min=%.1f int=%ds cichy=%d-%d\n",
                prog_max, prog_min, interwal_s, cichy_od, cichy_do);
            req->send(200, "text/plain", "OK");
        }
    );

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
                Serial.println("[OTA] Sukces! Restart...");
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

    // OTA Satelity — zapis .bin do LittleFS
    server.on("/ota/satelita", HTTP_POST,
        [](AsyncWebServerRequest *req) {
            bool ok = LittleFS.exists("/ota/satelita.bin");
            req->send(200, "text/plain", ok ? "OK" : "BLAD");
            if (ok) {
                // Ustaw OTA pending dla wszystkich satelitów
                for (int i = 0; i < ile_satelit; i++) {
                    satelity[i].ota_pending = true;
                }
                Serial.printf("[OTA-SAT] Flaga ustawiona dla %d satelitow\n", ile_satelit);
            }
        },
        [](AsyncWebServerRequest *req, const String &filename, size_t index,
           uint8_t *data, size_t len, bool final) {
            if (!index) {
                Serial.printf("[OTA-SAT] Start: %s\n", filename.c_str());
                LittleFS.mkdir("/ota");
                ota_satelity_file = LittleFS.open("/ota/satelita.bin", "w");
                if (!ota_satelity_file) {
                    Serial.println("[OTA-SAT] BLAD otwarcia pliku!");
                    return;
                }
            }
            if (ota_satelity_file) {
                ota_satelity_file.write(data, len);
            }
            if (final) {
                if (ota_satelity_file) {
                    ota_satelity_file.close();
                    Serial.printf("[OTA-SAT] Zapisano: %u bajtow\n", index + len);
                }
            }
        }
    );

    // Serwowanie .bin dla Satelity
    server.on("/ota/satelita.bin", HTTP_GET, [](AsyncWebServerRequest *req) {
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
        Serial.printf("\n[OK] WiFi polaczone! IP: %s\n", ip_adres.c_str());
        return true;
    }
    Serial.println("\n[BLAD] WiFi nie polaczone!");
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
        Serial.println("[FS] LittleFS mount failed!");
    }

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
        Serial.println("[WARN] Brak synchronizacji NTP!");
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
    Serial.printf("[OK] ESP-NOW na kanale %d\n", kanal);

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
            Serial.println("[WiFi] Polaczenie utracone!");
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
            if (s->ostatni_czas > 0 && (millis() - s->ostatni_czas) < 2000) {
                // Sprawdź czy ten pomiar był już przetworzony
                static unsigned long przetworzone[MAX_SATELITY] = {0};
                if (przetworzone[i] != s->ostatni_czas) {
                    przetworzone[i] = s->ostatni_czas;
                    wyswietlPomiar(s);
                    wyslijACK(s);
                    sprawdzAlerty(s);
                    if (!s->pomiar.blad_czujnika) {
                        dodajDoHistorii(s, s->pomiar.temperatura);
                    }
                }
            }
        }
    }

    sprawdzTelegram();
    sprawdzHeartbeat();
    delay(10);
}
