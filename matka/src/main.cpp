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
unsigned long heartbeat_timeout = 7200000; // 2h w ms

// === Struktury ESP-NOW ===

typedef struct __attribute__((packed)) {
    uint8_t  id_czujnika;
    float    temperatura;
    uint8_t  bateria_procent;
    uint32_t timestamp;
    bool     blad_czujnika;
} struct_message;

typedef struct __attribute__((packed)) {
    uint32_t nowy_interwal_s;
    uint8_t  godzina_start;
    uint8_t  godzina_stop;
    bool     ota_pending;
    char     ota_url[64];
} struct_ack;

// === Zmienne globalne ===

struct_message ostatni_pomiar;
bool nowy_pomiar = false;
bool mamy_dane = false;
uint8_t adres_satelity[6];
bool satelita_znana = false;
unsigned long czas_ostatniego = 0;
unsigned long boot_time = 0;
String ip_adres = "";

AsyncWebServer server(80);

// === Historia (ring buffer 48h) ===

#define MAX_HISTORIA 96  // 48h co 30 min
struct HistoriaWpis {
    float temperatura;
    uint32_t czas_unix;  // epoch
    bool pusty;
};
HistoriaWpis historia[MAX_HISTORIA];
int hist_idx = 0;
int hist_count = 0;

void dodajDoHistorii(float temp) {
    struct tm info;
    if (!getLocalTime(&info)) return;
    time_t now;
    time(&now);
    historia[hist_idx].temperatura = temp;
    historia[hist_idx].czas_unix = (uint32_t)now;
    historia[hist_idx].pusty = false;
    hist_idx = (hist_idx + 1) % MAX_HISTORIA;
    if (hist_count < MAX_HISTORIA) hist_count++;
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

    memcpy(&ostatni_pomiar, data, sizeof(struct_message));
    nowy_pomiar = true;
    mamy_dane = true;
    czas_ostatniego = millis();

    memcpy(adres_satelity, mac, 6);
    satelita_znana = true;

    Serial.printf("[ESP-NOW] Odebrano od %02X:%02X:%02X:%02X:%02X:%02X\n",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
    Serial.printf("[ESP-NOW] ACK: %s\n",
        status == ESP_NOW_SEND_SUCCESS ? "OK" : "BLAD");
}

// === ACK ===

void wyslijACK() {
    if (!satelita_znana) return;

    if (!esp_now_is_peer_exist(adres_satelity)) {
        esp_now_peer_info_t peer = {};
        memcpy(peer.peer_addr, adres_satelity, 6);
        peer.channel = 0;
        peer.encrypt = false;
        esp_now_add_peer(&peer);
    }

    struct_ack ack = {};
    ack.nowy_interwal_s = interwal_s;
    ack.godzina_start = 0;
    ack.godzina_stop = 0;
    ack.ota_pending = false;
    memset(ack.ota_url, 0, sizeof(ack.ota_url));

    esp_now_send(adres_satelity, (uint8_t*)&ack, sizeof(ack));
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

void sprawdzAlerty() {
    if (!mamy_dane) return;
    if (millis() - ostatni_telegram < TELEGRAM_COOLDOWN && ostatni_telegram > 0) return;

    String msg = "";
    bool krytyczny = false;

    if (ostatni_pomiar.blad_czujnika) {
        msg = "⚠️ <b>Smart Mleko</b>\nBłąd czujnika temperatury!";
        krytyczny = true;
    } else if (ostatni_pomiar.temperatura > prog_max) {
        msg = "🔴 <b>Smart Mleko</b>\nTemperatura za wysoka: <b>";
        msg += String(ostatni_pomiar.temperatura, 1);
        msg += "°C</b> (próg: " + String(prog_max, 1) + "°C)";
        krytyczny = true;
    } else if (prog_min > -50 && ostatni_pomiar.temperatura < prog_min) {
        msg = "🔵 <b>Smart Mleko</b>\nTemperatura za niska: <b>";
        msg += String(ostatni_pomiar.temperatura, 1);
        msg += "°C</b> (próg: " + String(prog_min, 1) + "°C)";
        krytyczny = true;
    } else if (ostatni_pomiar.bateria_procent <= 5) {
        msg = "🔋 <b>Smart Mleko</b>\nBateria krytyczna: <b>";
        msg += String(ostatni_pomiar.bateria_procent);
        msg += "%</b>";
        krytyczny = true;
    } else if (ostatni_pomiar.bateria_procent <= 15) {
        msg = "🔋 <b>Smart Mleko</b>\nNiski poziom baterii: <b>";
        msg += String(ostatni_pomiar.bateria_procent);
        msg += "%</b>";
    }

    if (msg.length() > 0) {
        if (!krytyczny && czyTrybCichy()) return; // łagodne blokowane w trybie cichym
        wyslijTelegram(msg);
        ostatni_telegram = millis();
    }
}

void sprawdzHeartbeat() {
    if (!mamy_dane && millis() > heartbeat_timeout) {
        // Nigdy nie dostaliśmy danych i minęło >2h od startu
        if (millis() - ostatni_telegram < TELEGRAM_COOLDOWN && ostatni_telegram > 0) return;
        wyslijTelegram("📡 <b>Smart Mleko</b>\nBrak sygnału z Satelity od ponad 2h!");
        ostatni_telegram = millis();
    } else if (mamy_dane && (millis() - czas_ostatniego) > heartbeat_timeout) {
        if (millis() - ostatni_telegram < TELEGRAM_COOLDOWN && ostatni_telegram > 0) return;
        wyslijTelegram("📡 <b>Smart Mleko</b>\nSatelita milczy od: " + czasOd(czas_ostatniego));
        ostatni_telegram = millis();
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
        if (!mamy_dane) {
            msg += "Brak danych z Satelity";
        } else if (ostatni_pomiar.blad_czujnika) {
            msg += "Temperatura: <b>BŁĄD CZUJNIKA</b>\n";
            msg += "Bateria: " + String(ostatni_pomiar.bateria_procent) + "%\n";
            msg += "Ostatni pomiar: " + czasOd(czas_ostatniego);
        } else {
            msg += "Temperatura: <b>" + String(ostatni_pomiar.temperatura, 1) + "°C</b>\n";
            msg += "Bateria: " + String(ostatni_pomiar.bateria_procent) + "%\n";
            msg += "Ostatni pomiar: " + czasOd(czas_ostatniego) + "\n";
            if (prog_max < 50) msg += "Alarm górny: " + String(prog_max, 1) + "°C\n";
            if (prog_min > -50) msg += "Alarm dolny: " + String(prog_min, 1) + "°C\n";
            msg += "Interwał: " + String(interwal_s / 60) + " min";
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
            msg += "🔕 Tryb cichy: wyłączony";
        } else {
            msg += "🔕 Tryb cichy: " + String(cichy_od) + ":00 — " + String(cichy_do) + ":00";
        }
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
</style>
</head>
<body>
<div class="header">
<h1>Smart Mleko v3.0</h1>
<div class="status" id="czas">---</div>
</div>

<div class="card">
<h2>Czujnik mleka</h2>
<div style="text-align:center;margin-bottom:12px">
<span class="status-badge status-wait" id="statusBadge">Ladowanie...</span>
</div>
<div class="temp-display">
<span class="temp-value" id="temp">--</span>
<span class="temp-unit">&deg;C</span>
</div>
<div class="info-grid">
<div class="info-item">
<div class="info-label">Bateria</div>
<div class="info-value" id="bat">--%</div>
</div>
<div class="info-item">
<div class="info-label">Ostatni pomiar</div>
<div class="info-value" id="ago">---</div>
</div>
</div>
</div>

<div class="card">
<h2>Historia temperatury</h2>
<div id="chartInfo" style="font-size:0.8em;color:#64748b;margin-bottom:8px">Ladowanie...</div>
<canvas id="chart" height="200" style="width:100%;background:#0f172a;border-radius:10px"></canvas>
</div>

<div class="card">
<h2>System</h2>
<div class="sys-info">
<div>MAC Matki: <strong id="mac">---</strong></div>
<div>IP: <strong id="ip">---</strong></div>
<div>Uptime: <strong id="uptime">---</strong></div>
<div>NTP: <strong id="ntp">---</strong></div>
<div>Satelita: <strong id="sat">---</strong></div>
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
<h2>Aktualizacja OTA</h2>
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

<div class="refresh-note">Odswiezanie co 5 sekund</div>

<script>
function update(){
fetch('/api/status').then(r=>r.json()).then(d=>{
document.getElementById('czas').textContent=d.czas_ntp;
document.getElementById('mac').textContent=d.mac;
document.getElementById('ip').textContent=d.ip;
document.getElementById('uptime').textContent=d.uptime;
document.getElementById('ntp').textContent=d.czas_ntp;

let el=document.getElementById('temp');
let badge=document.getElementById('statusBadge');

if(!d.mamy_dane){
el.textContent='--';
el.className='temp-value';
badge.textContent='Czekam na Satelite';
badge.className='status-badge status-wait';
document.getElementById('bat').textContent='--%';
document.getElementById('ago').textContent='---';
document.getElementById('sat').textContent='nie polaczona';
}else if(d.blad_czujnika){
el.textContent='ERR';
el.className='temp-value blad';
badge.textContent='Blad czujnika!';
badge.className='status-badge status-err';
document.getElementById('bat').textContent=d.bateria+'%';
document.getElementById('ago').textContent=d.ostatni_pomiar_temu;
document.getElementById('sat').textContent=d.mac_satelity;
}else{
el.textContent=d.temperatura.toFixed(1);
el.className='temp-value';
badge.textContent='OK';
badge.className='status-badge status-ok';
document.getElementById('bat').textContent=d.bateria+'%';
document.getElementById('ago').textContent=d.ostatni_pomiar_temu;
document.getElementById('sat').textContent=d.mac_satelity;

let b=document.getElementById('bat');
if(d.bateria>15)b.className='info-value bat-ok';
else if(d.bateria>5)b.className='info-value bat-mid';
else b.className='info-value bat-low';
}
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

function toggleMax(){
document.getElementById('cfgMax').disabled=!document.getElementById('maxOn').checked;
}
function toggleMin(){
document.getElementById('cfgMin').disabled=!document.getElementById('minOn').checked;
}
function toggleCichy(){
let on=document.getElementById('cichyOn').checked;
document.getElementById('cfgCOd').disabled=!on;
document.getElementById('cfgCDo').disabled=!on;
}

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

let cOn=!(d.cichy_od==0&&d.cichy_do==0);
document.getElementById('cichyOn').checked=cOn;
document.getElementById('cfgCOd').value=d.cichy_od;
document.getElementById('cfgCDo').value=d.cichy_do;
document.getElementById('cfgCOd').disabled=!cOn;
document.getElementById('cfgCDo').disabled=!cOn;
});
}
loadConfig();

function drawChart(){
fetch('/api/historia').then(r=>r.json()).then(data=>{
let info=document.getElementById('chartInfo');
if(!data.length){info.textContent='Brak danych';return}
info.textContent=data.length+' pomiarow';
let c=document.getElementById('chart');
let ctx=c.getContext('2d');
c.width=c.offsetWidth*2;c.height=400;
ctx.clearRect(0,0,c.width,c.height);
let vals=data.map(d=>d.v);
let minV=Math.floor(Math.min(...vals)-1);
let maxV=Math.ceil(Math.max(...vals)+1);
if(maxV-minV<4){minV-=2;maxV+=2}
let pad={l:50,r:20,t:20,b:40};
let w=c.width-pad.l-pad.r;
let h=c.height-pad.t-pad.b;
// siatka
ctx.strokeStyle='#1e293b';ctx.lineWidth=1;
ctx.fillStyle='#64748b';ctx.font='20px sans-serif';
for(let v=minV;v<=maxV;v++){
let y=pad.t+h-(v-minV)/(maxV-minV)*h;
ctx.beginPath();ctx.moveTo(pad.l,y);ctx.lineTo(pad.l+w,y);ctx.stroke();
ctx.fillText(v+'°',4,y+6);
}
// oś czasu
let t0=data[0].t;let t1=data[data.length-1].t;
let span=t1-t0;
if(span>0){
let step=span>7200?3600:1800;
for(let t=Math.ceil(t0/step)*step;t<=t1;t+=step){
let x=pad.l+(t-t0)/span*w;
ctx.beginPath();ctx.moveTo(x,pad.t);ctx.lineTo(x,pad.t+h);ctx.stroke();
let d=new Date(t*1000);
ctx.fillText(d.getHours()+':'+(d.getMinutes()<10?'0':'')+d.getMinutes(),x-15,c.height-10);
}}
// linia
ctx.strokeStyle='#22c55e';ctx.lineWidth=3;
ctx.beginPath();
for(let i=0;i<data.length;i++){
let x=pad.l+(span>0?(data[i].t-t0)/span:0.5)*w;
let y=pad.t+h-(data[i].v-minV)/(maxV-minV)*h;
if(i===0)ctx.moveTo(x,y);else ctx.lineTo(x,y);
}
ctx.stroke();
// kropki
ctx.fillStyle='#22c55e';
for(let i=0;i<data.length;i++){
let x=pad.l+(span>0?(data[i].t-t0)/span:0.5)*w;
let y=pad.t+h-(data[i].v-minV)/(maxV-minV)*h;
ctx.beginPath();ctx.arc(x,y,4,0,Math.PI*2);ctx.fill();
}
});
}
drawChart();
setInterval(drawChart,30000);

function saveConfig(){
let btn=document.getElementById('cfgBtn');
btn.disabled=true;btn.textContent='Zapisywanie...';
let body={
prog_max:document.getElementById('maxOn').checked?parseFloat(document.getElementById('cfgMax').value):99,
prog_min:document.getElementById('minOn').checked?parseFloat(document.getElementById('cfgMin').value):-99,
interwal_min:parseInt(document.getElementById('cfgInt').value),
cichy_od:document.getElementById('cichyOn').checked?parseInt(document.getElementById('cfgCOd').value):0,
cichy_do:document.getElementById('cichyOn').checked?parseInt(document.getElementById('cfgCDo').value):0
};
fetch('/api/ustawienia',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
.then(r=>r.text()).then(t=>{
document.getElementById('cfgMsg').textContent=t==='OK'?'Zapisano!':'Blad: '+t;
document.getElementById('cfgMsg').style.color=t==='OK'?'#4ade80':'#fca5a5';
btn.disabled=false;btn.textContent='Zapisz ustawienia';
setTimeout(()=>{document.getElementById('cfgMsg').textContent=''},3000);
}).catch(()=>{
document.getElementById('cfgMsg').textContent='Blad polaczenia';
document.getElementById('cfgMsg').style.color='#fca5a5';
btn.disabled=false;btn.textContent='Zapisz ustawienia';
});
}
</script>
</body>
</html>
)rawliteral";

// === AP Mode Server ===

void setupAPServer() {
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send_P(200, "text/html", AP_PAGE);
    });
    server.on("/index.html", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send_P(200, "text/html", AP_PAGE);
    });
    // Captive portal detection
    server.on("/hotspot-detect.html", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send_P(200, "text/html", AP_PAGE);
    });
    server.on("/generate_204", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send_P(200, "text/html", AP_PAGE);
    });
    server.on("/fwlink", HTTP_GET, [](AsyncWebServerRequest *req) {
        req->send_P(200, "text/html", AP_PAGE);
    });

    server.on("/api/sys", HTTP_GET, [](AsyncWebServerRequest *req) {
        JsonDocument doc;
        doc["heap"] = ESP.getFreeHeap();
        doc["mac"] = WiFi.macAddress();
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
        doc["czas_ntp"] = pobierzCzas();
        doc["mamy_dane"] = mamy_dane;

        // Uptime
        unsigned long sek = millis() / 1000;
        String up = String(sek / 3600) + "h " + String((sek % 3600) / 60) + "min";
        doc["uptime"] = up;

        if (mamy_dane) {
            doc["temperatura"] = ostatni_pomiar.temperatura;
            doc["bateria"] = ostatni_pomiar.bateria_procent;
            doc["blad_czujnika"] = ostatni_pomiar.blad_czujnika;
            doc["ostatni_pomiar_temu"] = czasOd(czas_ostatniego);

            char macbuf[18];
            snprintf(macbuf, sizeof(macbuf), "%02X:%02X:%02X:%02X:%02X:%02X",
                adres_satelity[0], adres_satelity[1], adres_satelity[2],
                adres_satelity[3], adres_satelity[4], adres_satelity[5]);
            doc["mac_satelity"] = macbuf;
        }

        String json;
        serializeJson(doc, json);
        req->send(200, "application/json", json);
    });

    // Historia
    server.on("/api/historia", HTTP_GET, [](AsyncWebServerRequest *req) {
        String json = "[";
        for (int i = 0; i < hist_count; i++) {
            int idx = (hist_idx - hist_count + i + MAX_HISTORIA) % MAX_HISTORIA;
            if (i > 0) json += ",";
            json += "{\"t\":" + String(historia[idx].czas_unix) +
                    ",\"v\":" + String(historia[idx].temperatura, 1) + "}";
        }
        json += "]";
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

    server.begin();
    Serial.println("[OK] Serwer WWW aktywny");
}

// === Wyświetlanie pomiaru ===

void wyswietlPomiar() {
    Serial.println("========================================");
    Serial.printf("  Czujnik ID:    %d\n", ostatni_pomiar.id_czujnika);
    if (ostatni_pomiar.blad_czujnika) {
        Serial.println("  Temperatura:   BLAD CZUJNIKA!");
    } else {
        Serial.printf("  Temperatura:   %.2f C\n", ostatni_pomiar.temperatura);
    }
    Serial.printf("  Bateria:       %d%%\n", ostatni_pomiar.bateria_procent);
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
    Serial.println("  MATKA — Smart Mleko v3.0");
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
    wyslijTelegram("✅ <b>Smart Mleko v3.0</b>\nMatka uruchomiona\nIP: " + ip_adres);

    Serial.println();
    Serial.println(">>> Dashboard: http://mleko.local");
    Serial.printf(">>> Lub: http://%s\n", ip_adres.c_str());
    Serial.println(">>> Czekam na dane z Satelity...");
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

    if (nowy_pomiar) {
        nowy_pomiar = false;
        wyswietlPomiar();
        wyslijACK();
        sprawdzAlerty();
        if (!ostatni_pomiar.blad_czujnika) {
            dodajDoHistorii(ostatni_pomiar.temperatura);
        }
    }
    sprawdzTelegram();
    sprawdzHeartbeat();
    delay(10);
}
