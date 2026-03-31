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

#define SAT_FW_VERSION "2.4"

// === Konfiguracja ===

// Domyślne wartości — używane TYLKO przy pierwszym flashu USB.
// Po pierwszym uruchomieniu zapisywane do Preferences (NVS).
// OTA NIE nadpisuje Preferences — ID i TYP są bezpieczne.
#define DEFAULT_ID          1     // zmień przed pierwszym flashem USB

#ifdef PLATFORM_C3
  #define DEFAULT_TYP       2     // zasilacz (C3 Super Mini)
  #define PIN_DS18B20       2     // GPIO2 na C3 Super Mini
#else
  #define DEFAULT_TYP       1     // bateria (ESP32-S3 + 18650)
  #define PIN_DS18B20       4     // GPIO4 na WEMOS D1
  #define PIN_ADC_BATERIA   34
  #define WSPOLCZYNNIK_ADC  2.0f
#endif

#define INTERWAL_DOMYSLNY_S 1800  // 30 minut
#define TIMEOUT_ACK_MS      2000

// MAC adres Matki — z Serial Monitor
uint8_t adresMatki[] = {0x80, 0xB5, 0x4E, 0xC3, 0x3C, 0xB8};

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

// === Zmienne globalne ===

Preferences prefs;
uint8_t id_czujnika;
uint8_t typ_zasilania;

OneWire oneWire(PIN_DS18B20);
DallasTemperature czujniki(&oneWire);

volatile bool ack_otrzymany = false;
struct_ack ostatni_ack;
uint32_t interwal_s = INTERWAL_DOMYSLNY_S;

// Kanał WiFi — zapisywany w RTC RAM (przeżywa deep sleep, nie przeżywa power off)
RTC_DATA_ATTR uint8_t ostatni_kanal = 0;

// Na etapie debugowania używamy delay() zamiast Deep Sleep
// Deep Sleep wprowadzimy w Etapie 2

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

// === Odczyt baterii ===

#ifndef PLATFORM_C3
uint8_t odczytajBaterie() {
    int raw = analogRead(PIN_ADC_BATERIA);
    float napiecie = raw * (3.3f / 4095.0f) * WSPOLCZYNNIK_ADC;
    int procent = constrain((int)((napiecie - 3.0f) / 1.2f * 100.0f), 0, 100);

    Serial.printf("[BAT] RAW=%d, V=%.2f, %d%%\n", raw, napiecie, procent);
    return (uint8_t)procent;
}
#endif

// === Wysyłka pomiaru ===

bool wyslijPomiar(float temp, bool blad, uint8_t bateria) {
    struct_message msg;
    msg.id_czujnika = id_czujnika;
    msg.typ_zasilania = typ_zasilania;
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
    Serial.printf("  Bateria:       %d%%\n", bateria);
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

        if (wyslijPomiar(0, true, 0) && czekajNaACK()) {
            Serial.printf("ZNALEZIONO!\n");
            ostatni_kanal = k;
            return true;
        }
        Serial.printf("brak\n");
    }
    Serial.println("[CH] Nie znaleziono Matki na zadnym kanale!");
    return false;
}

// === OTA Download ===

void wykonajOTA(const char *url, const char *ssid, const char *pass) {
    Serial.printf("[OTA] Laczenie z WiFi: %s\n", ssid);

    // Rozłącz ESP-NOW i połącz do WiFi
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
        ESP.restart(); // restart żeby wrócić do ESP-NOW
        return;
    }
    Serial.printf("\n[OTA] WiFi OK! IP: %s\n", WiFi.localIP().toString().c_str());
    Serial.printf("[OTA] Pobieram: %s\n", url);

    // Pobierz rozmiar pliku
    HTTPClient http;
    http.begin(url);
    http.setTimeout(15000);
    int code = http.GET();

    if (code != 200) {
        Serial.printf("[OTA] HTTP blad: %d\n", code);
        http.end();
        return;
    }

    int totalSize = http.getSize();
    http.end(); // zamknij — będziemy pobierać chunkami

    if (totalSize <= 0) {
        Serial.println("[OTA] Pusty plik!");
        return;
    }

    Serial.printf("[OTA] Rozmiar: %d bajtow\n", totalSize);

    if (!Update.begin(totalSize)) {
        Serial.println("[OTA] Update.begin failed!");
        Update.printError(Serial);
        return;
    }

    // Pobieraj chunkami z retry — każdy chunk 64KB
    int written = 0;
    int chunkSize = 32768;
    int maxRetries = 3;

    while (written < totalSize) {
        int remaining = totalSize - written;
        int toFetch = min(remaining, chunkSize);
        bool chunkOk = false;

        for (int retry = 0; retry < maxRetries && !chunkOk; retry++) {
            if (retry > 0) {
                Serial.printf("[OTA] Retry chunk %d...\n", retry);
                delay(1000);
            }

            HTTPClient chunkHttp;
            chunkHttp.begin(url);
            chunkHttp.setTimeout(15000);
            // HTTP Range request
            String rangeHeader = "bytes=" + String(written) + "-" + String(written + toFetch - 1);
            chunkHttp.addHeader("Range", rangeHeader);

            int chunkCode = chunkHttp.GET();
            if (chunkCode != 206 && chunkCode != 200) {
                Serial.printf("[OTA] Chunk HTTP blad: %d\n", chunkCode);
                chunkHttp.end();
                continue;
            }

            WiFiClient *stream = chunkHttp.getStreamPtr();
            uint8_t buf[512];
            int chunkWritten = 0;
            unsigned long lastData = millis();

            while (chunkWritten < toFetch) {
                int available = stream->available();
                if (available > 0) {
                    int toRead = min(available, min(toFetch - chunkWritten, (int)sizeof(buf)));
                    int readBytes = stream->readBytes(buf, toRead);
                    if (readBytes > 0) {
                        Update.write(buf, readBytes);
                        chunkWritten += readBytes;
                        lastData = millis();
                    }
                } else {
                    if (millis() - lastData > 10000) break;
                    delay(5);
                    yield();
                }
            }

            chunkHttp.end();

            if (chunkWritten == toFetch) {
                chunkOk = true;
                written += chunkWritten;
                Serial.printf("[OTA] %d / %d (%d%%)\n", written, totalSize, written * 100 / totalSize);
            } else {
                Serial.printf("[OTA] Chunk niepelny: %d / %d\n", chunkWritten, toFetch);
                // Nie możemy kontynuować Update z luką — abort
                break;
            }
        }

        if (!chunkOk) {
            Serial.println("[OTA] Nie udalo sie pobrac chunk — abort");
            break;
        }
    }

    Serial.printf("[OTA] Pobrano: %d / %d bajtow\n", written, totalSize);

    if (written == totalSize && Update.end(true)) {
        Serial.println("[OTA] Sukces! Restart...");
        delay(500);
        ESP.restart();
    } else {
        Serial.println("[OTA] BLAD!");
        Update.printError(Serial);
    }

    http.end();
    Serial.println("[OTA] Restart...");
    delay(500);
    ESP.restart();
}

// === Setup ===

void setup() {
    Serial.begin(115200);
    delay(1000);

    // Preferences — ID i TYP przeżywają OTA
    prefs.begin("satelita", false);
    if (!prefs.isKey("id")) {
        // Pierwszy flash — zapisz domyślne wartości
        prefs.putUChar("id", DEFAULT_ID);
        prefs.putUChar("typ", DEFAULT_TYP);
        Serial.println("[Prefs] Pierwszy flash — zapisano ID i TYP");
    }
    id_czujnika = prefs.getUChar("id", DEFAULT_ID);
    typ_zasilania = prefs.getUChar("typ", DEFAULT_TYP);

    Serial.println();
    Serial.println("================================");
    Serial.printf("  SATELITA — Smart Mleko v%s\n", SAT_FW_VERSION);
    Serial.printf("  ID: %d  TYP: %s\n", id_czujnika, typ_zasilania == 1 ? "bateria" : "zasilacz");
    Serial.println("================================");

    // DS18B20
    czujniki.begin();
    int ilosc = czujniki.getDeviceCount();
    Serial.printf("[DS18B20] Znaleziono czujnikow: %d\n", ilosc);

    // WiFi w trybie STA (wymagane dla ESP-NOW)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    Serial.printf("MAC Satelity: %s\n", WiFi.macAddress().c_str());

    // ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("[BLAD] ESP-NOW init!");
        return;
    }

    esp_now_register_recv_cb(onDataRecv);
    esp_now_register_send_cb(onDataSent);

    // Dodaj Matkę jako peer
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, adresMatki, 6);
    peer.channel = 0;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    Serial.println("[OK] ESP-NOW aktywny");
    Serial.printf("[OK] Interwal: %d sekund (delay na etapie debug)\n", interwal_s);
    Serial.println();
}

// === Loop ===

void loop() {
    Serial.println("--- Nowy pomiar ---");

    // Pomiar rozgrzewkowy — odrzuć
    bool dummy_blad;
    odczytajTemperature(dummy_blad);
    delay(100);

    // Właściwy pomiar
    bool blad;
    float temp = odczytajTemperature(blad);

    // Bateria
    #ifdef PLATFORM_C3
    uint8_t bateria = 100; // zasilacz — zawsze 100%
    #else
    uint8_t bateria = odczytajBaterie();
    #endif

    // Ustaw ostatnio znany kanał (hint z RTC RAM)
    bool polaczono = false;
    if (ostatni_kanal > 0) {
        esp_wifi_set_channel(ostatni_kanal, WIFI_SECOND_CHAN_NONE);
        Serial.printf("[CH] Hint: kanal %d\n", ostatni_kanal);
        for (int retry = 0; retry < 3 && !polaczono; retry++) {
            if (retry > 0) {
                Serial.printf("[CH] Hint retry %d...\n", retry);
                delay(500);
            }
            if (wyslijPomiar(temp, blad, bateria) && czekajNaACK()) {
                polaczono = true;
            }
        }
    }

    // Fallback — channel hopping jeśli hint nie zadziałał
    if (!polaczono) {
        if (wyslijPomiar(temp, blad, bateria) && czekajNaACK()) {
            polaczono = true;
        } else {
            Serial.println("[WARN] Brak ACK — proba channel hopping...");
            if (znajdzKanal()) {
                polaczono = true;
            }
        }
    }

    // Obsługa ACK — interwał, OTA
    if (polaczono) {
        // Zapisz kanał na przyszłość
        uint8_t kanal;
        wifi_second_chan_t second;
        esp_wifi_get_channel(&kanal, &second);
        ostatni_kanal = kanal;

        if (ostatni_ack.nowy_interwal_s > 0) {
            interwal_s = ostatni_ack.nowy_interwal_s;
            Serial.printf("[ACK] Nowy interwal: %d s\n", interwal_s);
        }
        if (ostatni_ack.ota_pending && strlen(ostatni_ack.ota_url) > 0 && strlen(ostatni_ack.wifi_ssid) > 0) {
            Serial.printf("[ACK] OTA czeka: %s\n", ostatni_ack.ota_url);
            wykonajOTA(ostatni_ack.ota_url, ostatni_ack.wifi_ssid, ostatni_ack.wifi_pass);
        }
    }

    // Na etapie debug: delay zamiast Deep Sleep
    Serial.printf("[SLEEP] Czekam %d sekund...\n\n", interwal_s);

    // Dla debugowania skracamy do 10 sekund
    delay(10000);
}
