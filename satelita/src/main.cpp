#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <Update.h>

// === Wersja ===

#define SAT_FW_VERSION "3.0"

// === Konfiguracja ===

// ID zapisywane do Preferences (NVS) przy pierwszym flashu USB.
// OTA NIE nadpisuje Preferences — ID jest bezpieczne.
#define DEFAULT_ID      1     // zmień przed pierwszym flashem USB
#define PIN_DS18B20     4     // GPIO4 na C3 Super Mini

#define INTERWAL_DOMYSLNY_S 1800  // 30 minut
#define TIMEOUT_ACK_MS      2000

// MAC adres Matki — z Serial Monitor
uint8_t adresMatki[] = {0x80, 0xB5, 0x4E, 0xC3, 0x3C, 0xB8};

// === Struktury ESP-NOW ===

typedef struct __attribute__((packed)) {
    uint8_t  id_czujnika;
    uint8_t  typ_zasilania;   // zawsze 2 (zasilacz)
    float    temperatura;
    uint8_t  bateria_procent; // zawsze 100
    uint32_t timestamp;
    bool     blad_czujnika;
    char     fw_version[8];
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

// === Zmienne globalne ===

Preferences prefs;
uint8_t id_czujnika;

OneWire oneWire(PIN_DS18B20);
DallasTemperature czujniki(&oneWire);

volatile bool ack_otrzymany = false;
struct_ack ostatni_ack;
uint32_t interwal_s = 60; // krótki start zanim Matka wyśle konfigurację

// Przeżywa power-off (RTC RAM) — hint kanału i interwał
RTC_DATA_ATTR uint8_t ostatni_kanal = 0;
RTC_DATA_ATTR uint32_t rtc_interwal_s = 60;

// === Callback ACK ===

void onDataRecv(const uint8_t *mac, const uint8_t *data, int len) {
    if (len == sizeof(struct_ack)) {
        memcpy(&ostatni_ack, data, sizeof(struct_ack));
        ack_otrzymany = true;
        Serial.println("[ESP-NOW] ACK otrzymany!");
    }
}

void onDataSent(const uint8_t *mac, esp_now_send_status_t status) {
    Serial.printf("[ESP-NOW] Wyslano: %s\n",
        status == ESP_NOW_SEND_SUCCESS ? "OK" : "BLAD");
}

// === Odczyt temperatury ===

float odczytajTemperature(bool &blad) {
    czujniki.requestTemperatures();
    float temp = czujniki.getTempCByIndex(0);

    if (temp == DEVICE_DISCONNECTED_C || temp == -127.0f || temp == 85.0f) {
        blad = true;
        return -127.0f;
    }

    blad = false;
    return temp;
}

// === Wysyłka pomiaru ===

bool wyslijPomiar(float temp, bool blad, uint8_t bateria) {
    struct_message msg;
    msg.id_czujnika = id_czujnika;
    msg.typ_zasilania = 2;
    msg.temperatura = temp;
    msg.bateria_procent = bateria;
    msg.timestamp = millis() / 1000;
    msg.blad_czujnika = blad;
    strlcpy(msg.fw_version, SAT_FW_VERSION, sizeof(msg.fw_version));

    Serial.println("========================================");
    if (blad) {
        Serial.println("  Temperatura:   BLAD CZUJNIKA");
    } else {
        Serial.printf("  Temperatura:   %.2f C\n", temp);
    }
    Serial.println("========================================");

    esp_err_t wynik = esp_now_send(adresMatki, (uint8_t*)&msg, sizeof(msg));
    return wynik == ESP_OK;
}

// === Czekanie na ACK ===

bool czekajNaACK() {
    ack_otrzymany = false;
    unsigned long start = millis();
    while (!ack_otrzymany && millis() - start < TIMEOUT_ACK_MS) {
        delay(10);
    }
    return ack_otrzymany;
}

// === Channel Hopping ===

bool znajdzKanal() {
    Serial.println("[CH] Szukam Matki...");
    for (int k = 1; k <= 13; k++) {
        esp_wifi_set_channel(k, WIFI_SECOND_CHAN_NONE);
        Serial.printf("[CH] Kanal %d... ", k);

        // 3 proby na kanal — Matka moze byc chwilowo zajeta (Telegram, LittleFS)
        for (int pr = 0; pr < 3; pr++) {
            if (wyslijPomiar(0, true, 0) && czekajNaACK()) {
                Serial.printf("ZNALEZIONO!\n");
                ostatni_kanal = k;
                return true;
            }
            if (pr < 2) delay(300);
        }
        Serial.printf("brak\n");
    }
    Serial.println("[CH] Nie znaleziono Matki na zadnym kanale!");
    return false;
}

// === OTA Download ===

void wykonajOTA(const char *url, const char *ssid, const char *pass) {
    Serial.printf("[OTA] Laczenie z WiFi: %s\n", ssid);

    esp_now_deinit();
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);

    int proby = 0;
    while (WiFi.status() != WL_CONNECTED && proby < 30) {
        delay(500);
        Serial.print(".");
        proby++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\n[OTA] WiFi nie polaczone — rezygnuje");
        WiFi.disconnect();
        ESP.restart();
        return;
    }
    Serial.printf("\n[OTA] WiFi OK! IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[OTA] Pobieram: %s\n", url);

    HTTPClient http;
    http.begin(url);
    http.setTimeout(60000);
    int code = http.GET();

    if (code != 200) {
        Serial.printf("[OTA] HTTP blad: %d — restart\n", code);
        http.end();
        delay(500);
        ESP.restart();
    }

    int totalSize = http.getSize();

    if (totalSize <= 0) {
        Serial.println("[OTA] Pusty plik — restart");
        http.end();
        delay(500);
        ESP.restart();
    }

    Serial.printf("[OTA] Rozmiar: %d bajtow\n", totalSize);

    if (!Update.begin(totalSize)) {
        Serial.println("[OTA] Update.begin failed — restart");
        Update.printError(Serial);
        Update.abort();
        http.end();
        delay(500);
        ESP.restart();
    }

    WiFiClient *stream = http.getStreamPtr();
    if (!stream) {
        Serial.println("[OTA] Brak strumienia — restart");
        Update.abort();
        http.end();
        delay(500);
        ESP.restart();
    }
    uint8_t buf[512];
    int written = 0;
    unsigned long lastData = millis();

    while (written < totalSize) {
        int available = stream->available();
        if (available > 0) {
            int toRead = min(available, min(totalSize - written, (int)sizeof(buf)));
            int n = stream->readBytes(buf, toRead);
            if (n > 0) {
                Update.write(buf, n);
                written += n;
                lastData = millis();
                if (written % (totalSize / 10) < 512) {
                    Serial.printf("[OTA] %d / %d (%d%%)\n", written, totalSize, written * 100 / totalSize);
                }
            }
        } else {
            if (millis() - lastData > 30000) {
                Serial.println("[OTA] Timeout!");
                break;
            }
            delay(10);
            yield();
        }
    }

    http.end();
    Serial.printf("[OTA] Pobrano: %d / %d bajtow\n", written, totalSize);

    if (written == totalSize && Update.end(true)) {
        Serial.println("[OTA] Sukces! Restart...");
        delay(500);
        ESP.restart();
    } else {
        Serial.println("[OTA] BLAD!");
        Update.printError(Serial);
        Update.abort();
        delay(500);
        ESP.restart();
    }
}

// === Setup ===

void setup() {
    Serial.begin(115200);
    delay(1000);

    // Preferences — ID przeżywa OTA
    prefs.begin("satelita", false);
    if (!prefs.isKey("id")) {
        prefs.putUChar("id", DEFAULT_ID);
        Serial.println("[Prefs] Pierwszy flash — zapisano ID");
    }
    id_czujnika = prefs.getUChar("id", DEFAULT_ID);
    prefs.end();

    interwal_s = rtc_interwal_s;

    Serial.println();
    Serial.println("================================");
    Serial.printf("  SATELITA — Smart Mleko v%s\n", SAT_FW_VERSION);
    Serial.printf("  ID: %d  TYP: zasilacz\n", id_czujnika);
    Serial.println("================================");

    czujniki.begin();
    Serial.printf("[DS18B20] Znaleziono czujnikow: %d\n", czujniki.getDeviceCount());

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    Serial.printf("MAC Satelity: %s\n", WiFi.macAddress().c_str());

    if (esp_now_init() != ESP_OK) {
        Serial.println("[BLAD] ESP-NOW init!");
        return;
    }

    esp_now_register_recv_cb(onDataRecv);
    esp_now_register_send_cb(onDataSent);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, adresMatki, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    Serial.println("[OK] ESP-NOW aktywny");
    Serial.printf("[OK] Interwal: %lu sekund\n", (unsigned long)interwal_s);
    Serial.println();
}

// === Loop ===

void loop() {
    Serial.println("--- Nowy pomiar ---");

    // Pomiar rozgrzewkowy — odrzuć
    bool dummy_blad;
    odczytajTemperature(dummy_blad);
    delay(100);

    bool blad;
    float temp = odczytajTemperature(blad);

    bool polaczono = false;
    if (ostatni_kanal > 0) {
        esp_wifi_set_channel(ostatni_kanal, WIFI_SECOND_CHAN_NONE);
        Serial.printf("[CH] Hint: kanal %d\n", ostatni_kanal);
        for (int retry = 0; retry < 3 && !polaczono; retry++) {
            if (retry > 0) {
                Serial.printf("[CH] Hint retry %d...\n", retry);
                delay(500);
            }
            if (wyslijPomiar(temp, blad, 100) && czekajNaACK()) {
                polaczono = true;
            }
        }
    }

    if (!polaczono) {
        if (wyslijPomiar(temp, blad, 100) && czekajNaACK()) {
            polaczono = true;
        } else {
            Serial.println("[WARN] Brak ACK — proba channel hopping...");
            if (znajdzKanal()) {
                polaczono = true;
            }
        }
    }

    if (polaczono) {
        uint8_t kanal;
        wifi_second_chan_t second;
        esp_wifi_get_channel(&kanal, &second);
        ostatni_kanal = kanal;

        if (ostatni_ack.nowy_interwal_s > 0) {
            interwal_s = ostatni_ack.nowy_interwal_s;
            rtc_interwal_s = interwal_s;
            Serial.printf("[ACK] Nowy interwal: %d s\n", interwal_s);
        }
        if (ostatni_ack.ota_pending && strlen(ostatni_ack.ota_url) > 0 && strlen(ostatni_ack.wifi_ssid) > 0) {
            Serial.printf("[ACK] OTA czeka: %s\n", ostatni_ack.ota_url);
            wykonajOTA(ostatni_ack.ota_url, ostatni_ack.wifi_ssid, ostatni_ack.wifi_pass);
        }
    }

    uint32_t czekaj_s = polaczono ? interwal_s : 60;
    Serial.printf("[SLEEP] Czekam %lu s...\n\n", (unsigned long)czekaj_s);
    delay(czekaj_s * 1000);
}
