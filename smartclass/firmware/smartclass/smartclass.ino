// =====================================================================
//  FastRobox 1.0 - Smart Classroom        Level 1 + Level 2 firmware
//  Board: ESP32-S3-CAM      Arduino-ESP32 core 3.x REQUIRED
//  Level 2 adds: Wi-Fi setup AP, NVS settings, LittleFS status page,
//  mDNS, reconnect with backoff, NTP, HTTPS telemetry + events,
//  persistent seq, BOOT-button / "reset" factory reset.
//  No password, key or token is written in this file.
// =====================================================================
#include <Arduino.h>
#include <SPI.h>
#include <MFRC522.h>
#include <DHT.h>
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include "web_pages.h"

#if !defined(ESP_ARDUINO_VERSION_MAJOR) || ESP_ARDUINO_VERSION_MAJOR < 3
#error "Install esp32 by Espressif Systems 3.x in Boards Manager (needed for the built-in CA certificate bundle)."
#endif

// ===================== TEAM SETTINGS (not secrets) =====================
#define TEAM_ID             "T02"                            // <-- your team ID
#define DEVICE_ID           TEAM_ID "-D1"
#define DEFAULT_SERVER_URL  "https://your-app.onrender.com"  // editable on the setup page

// ===================== PIN DEFINITIONS =====================
#define PIN_DHT         1
#define PIN_FLAME       2     // Active LOW on standard 3-pin IR flame module
#define PIN_BUZZER      39
#define PIN_RGB_LED     48    // Change to 38 if your S3-CAM clone uses GPIO 38
#define PIN_BOOT_BTN    0     // Onboard BOOT button, active LOW

#define PIN_RFID_SS     21
#define PIN_RFID_RST    40
#define PIN_RFID_SCK    47
#define PIN_RFID_MOSI   41
#define PIN_RFID_MISO   42

// ===================== CONSTANTS =====================
#define FW_VERSION        "1.0.0"
#define WDT_TIMEOUT_S     5       // Restarts in ~5s (within the 10s limit)
#define DHTTYPE           DHT11
#define TELEMETRY_MS      5000
#define TRIAL_TIMEOUT_MS  12000   // new details: back to setup mode in 12s (<15s)
#define AP_GRACE_MS       8000    // keep AP up briefly after success so the page can show the IP
#define SEQ_BLOCK         50      // seq numbers reserved in NVS per flash write
#define OUTQ_LEN          40
#define TZ_DHAKA          "<+06>-6"

// ===================== TYPES (must stay above the first function) =====================
enum NetState : uint8_t { NS_SETUP, NS_TRIAL, NS_CONNECTING, NS_ONLINE };

struct OutMsg {
  uint32_t seq;
  uint32_t createdMs;   // millis() when the reading was taken
  uint32_t ts;          // unix time when taken; 0 = clock not synced yet (fixed up before sending)
  bool     isEvent;
  char     type[20];
  char     detail[72];
  float    t, rh, hi, chip;
  bool     flame, doorUnlocked, good;
  int16_t  rssi;        // 0 = not connected
  uint32_t heapKb, uptimeS;
};

struct ScanNet { char ssid[33]; int8_t rssi; bool lock; };

// ===================== GLOBALS =====================
DHT dht(PIN_DHT, DHTTYPE);
MFRC522 rfid(PIN_RFID_SS, PIN_RFID_RST);
WebServer server(80);
DNSServer dns;
Preferences prefs;

SemaphoreHandle_t serialMutex;
SemaphoreHandle_t seqMutex;
QueueHandle_t     outQ;

// Shared sensor state (Level 1)
volatile float g_tempC = NAN;
volatile float g_rh = NAN;
volatile bool  g_flame = false;
volatile bool  g_doorUnlocked = false;
volatile bool  g_dhtError = false;
volatile bool  g_rfidError = false;
volatile bool  g_triggerHang = false;
char g_lastUid[24] = "";

// Network state (Level 2)
volatile NetState g_net = NS_SETUP;
volatile bool     g_apActive = false;
volatile bool     g_serverUp = false;
volatile bool     g_authErr = false;
volatile bool     g_factoryReset = false;
volatile bool     g_replayReq = false;
volatile uint8_t  g_lastDisc = 0;      // last Wi-Fi disconnect reason
volatile uint32_t g_discCount = 0;

// Saved settings (NVS namespace "sc")
char cfgSsid[33] = "", cfgPass[65] = "", cfgRoom[33] = "", cfgToken[129] = "", cfgSrv[129] = "";
char apSsid[40], apPass[9], mdnsHost[40];
char setupError[96] = "";

uint32_t g_seq = 0, g_seqReserved = 0;

// Net task private state
bool     everConnected = false, attemptActive = false, trialRetried = false, mdnsOn = false, ntpStarted = false;
uint32_t trialStart = 0, attemptStart = 0, discAtBegin = 0, waitUntil = 0, lostAt = 0, apCloseAt = 0;
uint8_t  attemptNo = 0;
const uint16_t RETRY_GAPS_S[] = {1, 2, 4, 8, 10};   // increasing gaps, capped at 10s

ScanNet  scanNets[20];
uint8_t  scanCount = 0;
uint32_t scanAt = 0;
bool     scanEver = false;

TaskHandle_t hTaskFlameLED = NULL, hTaskRFID = NULL, hTaskDHT = NULL, hTaskSerial = NULL, hTaskNet = NULL, hTaskCloud = NULL;

// ===================== HELPERS =====================
void safePrintf(const char* format, ...) {
  char buf[256];
  va_list args;
  va_start(args, format);
  vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  if (xSemaphoreTake(serialMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    Serial.print(buf);
    xSemaphoreGive(serialMutex);
  }
}

const char* getResetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWER_ON";
    case ESP_RST_SW:        return "SOFTWARE";
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return "WATCHDOG";
    case ESP_RST_PANIC:     return "CRASH";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    default:                return "POWER_ON";
  }
}

bool timeSynced() { return time(nullptr) > 1700000000; }

bool wifiUp() { return g_net == NS_ONLINE && WiFi.status() == WL_CONNECTED; }

// Escape a string for use inside JSON quotes
void jsonEscape(const char* in, char* out, size_t outSize) {
  size_t o = 0;
  for (; *in && o + 7 < outSize; in++) {
    unsigned char c = (unsigned char)*in;
    if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
    else if (c < 0x20) { o += snprintf(out + o, outSize - o, "\\u%04x", c); }
    else out[o++] = c;
  }
  out[o] = 0;
}

void copyArg(char* dst, size_t dstSize, const String& s) {
  strncpy(dst, s.c_str(), dstSize - 1);
  dst[dstSize - 1] = 0;
}

// seq only increases and is never reused, even across restarts: a block of
// numbers is reserved in NVS before any number in that block is used.
uint32_t nextSeq() {
  xSemaphoreTake(seqMutex, portMAX_DELAY);
  uint32_t s = ++g_seq;
  if (g_seq >= g_seqReserved) {
    g_seqReserved += SEQ_BLOCK;
    prefs.putUInt("seq", g_seqReserved);
  }
  xSemaphoreGive(seqMutex);
  return s;
}

void enqueueMsg(OutMsg& m) {
  m.seq = nextSeq();
  m.createdMs = millis();
  m.ts = timeSynced() ? (uint32_t)time(nullptr) : 0;
  if (xQueueSend(outQ, &m, 0) != pdTRUE) {      // full: drop the oldest
    OutMsg dropped;
    xQueueReceive(outQ, &dropped, 0);
    xQueueSend(outQ, &m, 0);
  }
}

// Every event is printed as "[EVENT] <type> <detail>" and queued for the cloud
void emitEvent(const char* type, const char* detail) {
  safePrintf("[EVENT] %s %s\n", type, detail);
  OutMsg m = {};
  m.isEvent = true;
  strncpy(m.type, type, sizeof(m.type) - 1);
  strncpy(m.detail, detail, sizeof(m.detail) - 1);
  enqueueMsg(m);
}

const char* connStateStr() {
  if (g_net == NS_SETUP || g_net == NS_TRIAL) return "SETUP";
  if (g_net == NS_CONNECTING) return "OFFLINE";
  if (g_authErr) return "AUTH ERROR";
  return g_serverUp ? "LIVE" : "WIFI ONLY";
}

void printStatusLine() {
  uint32_t up = millis() / 1000;
  char tStr[12], rhStr[12], hiStr[12], rssiStr[8];
  float t = g_tempC, h = g_rh;
  bool valid = !(isnan(t) || isnan(h) || g_dhtError);

  if (valid) {
    snprintf(tStr, sizeof(tStr), "%.1fC", t);
    snprintf(rhStr, sizeof(rhStr), "%.0f%%", h);
    snprintf(hiStr, sizeof(hiStr), "%.1fC", dht.computeHeatIndex(t, h, false));
  } else {
    strcpy(tStr, "-"); strcpy(rhStr, "-"); strcpy(hiStr, "-");
  }
  if (wifiUp()) snprintf(rssiStr, sizeof(rssiStr), "%d", (int)WiFi.RSSI());
  else strcpy(rssiStr, "-");

  safePrintf("[STATUS] up=%lus T=%s RH=%s HI=%s flame=%d door=%s state=SAFE trend=- rssi=%s heap=%luk psram=%luk chip=%.1fC fw=%s\n",
             (unsigned long)up, tStr, rhStr, hiStr, g_flame ? 1 : 0,
             g_doorUnlocked ? "UNLOCKED" : "LOCKED", rssiStr,
             (unsigned long)(ESP.getFreeHeap() / 1024), (unsigned long)(ESP.getFreePsram() / 1024),
             temperatureRead(), FW_VERSION);
}

// ===================== TASK 1: FLAME (100ms), RGB LED (2Hz), BOOT BUTTON [Core 0] =====================
void TaskFlameAndLED(void *pvParameters) {
  esp_task_wdt_add(NULL);
  TickType_t xLastWakeTime = xTaskGetTickCount();
  uint32_t tick50ms = 0;
  uint16_t bootHeldTicks = 0;
  bool ledOn = false;

  for (;;) {
    esp_task_wdt_reset();

    if (tick50ms % 2 == 0) {
      g_flame = (digitalRead(PIN_FLAME) == LOW);
    }

    // BOOT held for 5 s (100 x 50 ms) while running -> clear saved details
    if (digitalRead(PIN_BOOT_BTN) == LOW) {
      if (bootHeldTicks < 0xFFFF) bootHeldTicks++;
      if (bootHeldTicks == 100) {
        safePrintf("[NET] BOOT held 5s: clearing saved Wi-Fi details\n");
        g_factoryReset = true;
      }
    } else {
      bootHeldTicks = 0;
    }

    if (tick50ms % 5 == 0) {
      ledOn = !ledOn;
      if (ledOn) {
        // Lower state number wins: 6 sensor error, 10 setup/offline, 11 safe
        if (g_dhtError || g_rfidError)   neopixelWrite(PIN_RGB_LED, 64, 0, 64); // Magenta
        else if (g_net != NS_ONLINE)     neopixelWrite(PIN_RGB_LED, 0, 0, 64);  // Blue
        else                             neopixelWrite(PIN_RGB_LED, 0, 64, 0);  // Green
      } else {
        neopixelWrite(PIN_RGB_LED, 0, 0, 0);
      }
    }

    tick50ms++;
    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(50));
  }
}

// ===================== TASK 2: RFID READER (200ms) [Core 1] =====================
void TaskRFID(void *pvParameters) {
  esp_task_wdt_add(NULL);
  TickType_t xLastWakeTime = xTaskGetTickCount();

  bool cardPresent = false;
  uint8_t missCount = 0;
  uint8_t failCount = 0;

  for (;;) {
    esp_task_wdt_reset();

    byte v = rfid.PCD_ReadRegister(MFRC522::VersionReg);
    bool commOk = (v == 0x91 || v == 0x92 || v == 0x90 || v == 0x88 || v == 0x12);

    if (!commOk) {
      failCount++;
      if (failCount >= 5 && !g_rfidError) {
        g_rfidError = true;
        safePrintf("[ERROR] SENSOR ERROR: RFID\n");
      }
    } else {
      if (g_rfidError) {
        rfid.PCD_Init();
        g_rfidError = false;
      }
      failCount = 0;

      byte atqa[2];
      byte atqaSize = sizeof(atqa);
      MFRC522::StatusCode status = rfid.PICC_WakeupA(atqa, &atqaSize);

      if (status == MFRC522::STATUS_OK || status == MFRC522::STATUS_COLLISION) {
        if (rfid.PICC_ReadCardSerial()) {
          if (!cardPresent) {
            cardPresent = true;
            char uidStr[24] = "";
            for (byte i = 0; i < rfid.uid.size && i < 10; i++) {
              char hex[4];
              snprintf(hex, sizeof(hex), "%02X", rfid.uid.uidByte[i]);
              strcat(uidStr, hex);
            }
            strncpy(g_lastUid, uidStr, sizeof(g_lastUid) - 1);
            safePrintf("[CARD] uid=%s\n", uidStr);
            char detail[40];
            snprintf(detail, sizeof(detail), "uid=%s", uidStr);
            emitEvent("CARD_TAP", detail);          // sent to the cloud at once
          }
          missCount = 0;
          rfid.PICC_HaltA();
        }
      } else {
        missCount++;
        if (missCount >= 2) cardPresent = false;
      }
    }

    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(200));
  }
}

// ===================== TASK 3: DHT11 SENSOR (5000ms) [Core 1] =====================
void TaskDHT(void *pvParameters) {
  esp_task_wdt_add(NULL);
  TickType_t xLastWakeTime = xTaskGetTickCount();
  uint32_t lastReadMs = 0;
  uint32_t lastValidMs = millis();

  for (;;) {
    esp_task_wdt_reset();
    uint32_t now = millis();
    uint32_t interval = g_dhtError ? 2000 : 5000;

    if (now - lastReadMs >= interval || lastReadMs == 0) {
      lastReadMs = now;
      float t = dht.readTemperature();
      float h = dht.readHumidity();

      if (isnan(t) || isnan(h)) {
        if (!g_dhtError && (now - lastValidMs >= 9500)) {
          g_dhtError = true;
          safePrintf("[ERROR] SENSOR ERROR: DHT11\n");
        }
      } else {
        g_tempC = t;
        g_rh = h;
        lastValidMs = now;
        if (g_dhtError) g_dhtError = false;
      }
    }

    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(200));
  }
}

// ===================== TASK 4: SERIAL CLI, 1Hz STATUS, 5s TELEMETRY [Core 1] =====================
void handleSerialCommand(String cmd) {
  cmd.trim();
  if (cmd.length() == 0) return;

  if (cmd == "help") {
    safePrintf("[CMD] Commands: help, status, health, tasks, ver, hang, reset, replay\n");
  }
  else if (cmd == "status") {
    safePrintf("[CMD] ");
    printStatusLine();
  }
  else if (cmd == "health") {
    char rssiStr[12];
    if (wifiUp()) snprintf(rssiStr, sizeof(rssiStr), "%ddBm", (int)WiFi.RSSI());
    else strcpy(rssiStr, "N/A");
    safePrintf("[CMD] chip=%.1fC heap=%luB psram=%luB flash=%luB cpu=%luMHz rssi=%s up=%lus reset=%s fw=%s\n",
               temperatureRead(),
               (unsigned long)ESP.getFreeHeap(),
               (unsigned long)ESP.getFreePsram(),
               (unsigned long)ESP.getFlashChipSize(),
               (unsigned long)ESP.getCpuFreqMHz(),
               rssiStr,
               (unsigned long)(millis() / 1000),
               getResetReasonStr(esp_reset_reason()),
               FW_VERSION);
  }
  else if (cmd == "tasks") {
    const char* names[] = {"FlameLED", "RFID", "DHT", "Serial", "Net", "Cloud"};
    TaskHandle_t handles[] = {hTaskFlameLED, hTaskRFID, hTaskDHT, hTaskSerial, hTaskNet, hTaskCloud};
    for (int i = 0; i < 6; i++) {
      if (handles[i]) {
        safePrintf("[CMD] task=%s core=%d stack_free=%uB\n", names[i],
                   (int)xTaskGetCoreID(handles[i]), (unsigned)uxTaskGetStackHighWaterMark(handles[i]));
      }
    }
  }
  else if (cmd == "ver") {
    safePrintf("[CMD] fw=%s build=%s %s\n", FW_VERSION, __DATE__, __TIME__);
  }
  else if (cmd == "hang") {
    safePrintf("[CMD] Freezing TaskSerial to trigger hardware watchdog...\n");
    g_triggerHang = true;
  }
  else if (cmd == "reset") {
    safePrintf("[CMD] Clearing saved Wi-Fi details, restarting in setup mode\n");
    g_factoryReset = true;
  }
  else if (cmd == "replay") {
    // Demo for the judge: re-send the last stored message; the server must reject it
    safePrintf("[CMD] Re-sending the last delivered message (server should reject it)\n");
    g_replayReq = true;
  }
  else {
    safePrintf("[CMD] Unknown command: %s\n", cmd.c_str());
  }
}

void queueTelemetry() {
  OutMsg m = {};
  float t = g_tempC, h = g_rh;
  m.good = !(isnan(t) || isnan(h) || g_dhtError);
  m.t = t; m.rh = h;
  m.hi = m.good ? dht.computeHeatIndex(t, h, false) : NAN;
  m.flame = g_flame;
  m.doorUnlocked = g_doorUnlocked;
  m.rssi = wifiUp() ? (int16_t)WiFi.RSSI() : 0;
  m.heapKb = ESP.getFreeHeap() / 1024;
  m.chip = temperatureRead();
  m.uptimeS = millis() / 1000;
  enqueueMsg(m);
}

void TaskSerialAndStatus(void *pvParameters) {
  esp_task_wdt_add(NULL);
  TickType_t xLastWakeTime = xTaskGetTickCount();
  uint32_t lastStatusMs = 0, lastTelemMs = 0;
  String inputBuf = "";

  for (;;) {
    if (g_triggerHang) {
      while (true) { ; }   // deliberate freeze -> watchdog restart
    }

    esp_task_wdt_reset();

    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\n' || c == '\r') {
        if (inputBuf.length() > 0) {
          handleSerialCommand(inputBuf);
          inputBuf = "";
        }
      } else if (inputBuf.length() < 64) {
        inputBuf += c;
      }
    }

    uint32_t now = millis();
    if (now - lastStatusMs >= 1000) {
      lastStatusMs = now;
      printStatusLine();
    }

    // One reading every 5 s once the device has been set up. It is time-stamped
    // here (when taken) and keeps being queued while Wi-Fi or the server is down.
    if (now - lastTelemMs >= TELEMETRY_MS) {
      lastTelemMs = now;
      if (g_net == NS_ONLINE || g_net == NS_CONNECTING) queueTelemetry();
    }

    vTaskDelayUntil(&xLastWakeTime, pdMS_TO_TICKS(50));
  }
}

// ===================== NETWORK: SETUP MODE / CONNECT / RECONNECT =====================
void onWiFiEvent(arduino_event_id_t event, arduino_event_info_t info) {
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    uint8_t r = info.wifi_sta_disconnected.reason;
    if (r == 8) return;            // ASSOC_LEAVE: caused by our own WiFi.disconnect()
    g_lastDisc = r;
    g_discCount++;
  }
}

void startSetupMode() {
  WiFi.mode(WIFI_AP_STA);                 // AP for the phone + STA for scanning / trying
  WiFi.softAP(apSsid, apPass);
  dns.start(53, "*", WiFi.softAPIP());    // captive portal: any name -> setup page
  g_apActive = true;
  g_net = NS_SETUP;
  safePrintf("[NET] setup mode: AP=%s password=%s page=http://%s\n",
             apSsid, apPass, WiFi.softAPIP().toString().c_str());
  WiFi.scanNetworks(true);
  scanEver = true;
}

void closeAccessPoint() {
  if (!g_apActive) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  g_apActive = false;
  safePrintf("[NET] access point closed\n");
}

void beginAttempt() {
  discAtBegin = g_discCount;
  attemptStart = millis();
  attemptActive = true;
  WiFi.begin(cfgSsid, cfgPass);
}

// Try freshly entered details while the AP stays up
void startTrial() {
  setupError[0] = 0;
  g_net = NS_TRIAL;
  trialStart = millis();
  trialRetried = false;
  g_lastDisc = 0;
  WiFi.scanDelete();
  safePrintf("[NET] connecting to \"%s\" ...\n", cfgSsid);
  beginAttempt();
}

void onConnected() {
  char ip[20];
  strncpy(ip, WiFi.localIP().toString().c_str(), sizeof(ip) - 1); ip[sizeof(ip) - 1] = 0;
  if (!everConnected) {
    safePrintf("[NET] connected ssid=\"%s\" ip=%s rssi=%d\n", cfgSsid, ip, (int)WiFi.RSSI());
  } else {
    safePrintf("[NET] reconnected ip=%s after %lus without restart (up=%lus)\n", ip,
               (unsigned long)((millis() - lostAt) / 1000), (unsigned long)(millis() / 1000));
  }
  everConnected = true;
  attemptActive = false;
  attemptNo = 0;

  if (mdnsOn) MDNS.end();
  mdnsOn = MDNS.begin(mdnsHost);
  if (mdnsOn) {
    MDNS.addService("http", "tcp", 80);
    safePrintf("[NET] mDNS name http://%s.local  (also http://%s)\n", mdnsHost, ip);
  } else {
    safePrintf("[ERROR] mDNS start failed\n");
  }
  if (!ntpStarted) {
    configTzTime(TZ_DHAKA, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
    ntpStarted = true;
  }
  g_net = NS_ONLINE;
}

void describeFailure(char* out, size_t n) {
  uint8_t r = g_lastDisc;
  if (r == 201)                                   snprintf(out, n, "Network \"%s\" not found or unreachable", cfgSsid);
  else if (r == 202 || r == 15 || r == 204 || r == 2 || r == 211)
                                                  snprintf(out, n, "Wrong password for \"%s\"", cfgSsid);
  else                                            snprintf(out, n, "Could not connect to \"%s\" (reason %u)", cfgSsid, r);
}

void netTick() {
  uint32_t now = millis();

  switch (g_net) {
    case NS_SETUP:
      break;

    case NS_TRIAL:
      if (WiFi.status() == WL_CONNECTED) {
        prefs.putBool("ok", true);                 // details proven good
        apCloseAt = now + AP_GRACE_MS;
        onConnected();
      } else if (now - trialStart >= TRIAL_TIMEOUT_MS) {
        describeFailure(setupError, sizeof(setupError));
        WiFi.disconnect();
        attemptActive = false;
        cfgSsid[0] = 0; cfgPass[0] = 0;            // bad details are not kept
        prefs.remove("ssid"); prefs.remove("pass"); prefs.putBool("ok", false);
        g_net = NS_SETUP;
        safePrintf("[NET] setup error: %s - back in setup mode after %lus\n", setupError,
                   (unsigned long)((now - trialStart) / 1000));
      } else if (!trialRetried && g_discCount != discAtBegin && now - attemptStart >= 2500
                 && now - trialStart < TRIAL_TIMEOUT_MS - 5000) {
        trialRetried = true;                       // one more try inside the 12 s window
        beginAttempt();
      }
      break;

    case NS_ONLINE:
      if (g_apActive && (int32_t)(now - apCloseAt) >= 0) closeAccessPoint();
      if (WiFi.status() != WL_CONNECTED) {
        safePrintf("[NET] disconnected ssid=\"%s\" reason=%u\n", cfgSsid, (unsigned)g_lastDisc);
        closeAccessPoint();
        lostAt = now;
        attemptNo = 0;
        attemptActive = false;
        waitUntil = now + RETRY_GAPS_S[0] * 1000UL;
        g_net = NS_CONNECTING;
      }
      break;

    case NS_CONNECTING:
      if (WiFi.status() == WL_CONNECTED) {
        onConnected();
      } else if (attemptActive) {
        // attempt finished when the driver reports a failure, or after a 10 s hard limit
        if (g_discCount != discAtBegin || now - attemptStart >= 10000) {
          attemptActive = false;
          uint8_t idx = attemptNo < 5 ? attemptNo : 4;
          if (attemptNo < 255) attemptNo++;
          waitUntil = now + RETRY_GAPS_S[idx] * 1000UL;
          safePrintf("[NET] connect attempt %u failed (reason %u), retry in %us\n",
                     (unsigned)attemptNo, (unsigned)g_lastDisc, (unsigned)RETRY_GAPS_S[idx]);
        }
      } else if ((int32_t)(now - waitUntil) >= 0) {
        safePrintf("[NET] connecting to \"%s\" ...\n", cfgSsid);
        beginAttempt();
      }
      break;
  }
}

void doFactoryReset() {
  prefs.remove("ssid"); prefs.remove("pass"); prefs.remove("room"); prefs.remove("token");
  prefs.putBool("ok", false);
  safePrintf("[NET] saved details cleared, restarting in setup mode\n");
  vTaskDelay(pdMS_TO_TICKS(300));
  ESP.restart();
}

// ===================== LOCAL WEB SERVER =====================
void syncFile(const char* path, const char* content) {
  size_t len = strlen(content);
  File f = LittleFS.open(path, "r");
  bool same = f && f.size() == len;
  if (f) f.close();
  if (same) return;
  f = LittleFS.open(path, "w");
  if (!f) { safePrintf("[ERROR] LittleFS write failed: %s\n", path); return; }
  f.print(content);
  f.close();
}

void sendFile(const char* path) {
  File f = LittleFS.open(path, "r");
  if (!f) { server.send(500, "text/plain", "page file missing in LittleFS"); return; }
  server.sendHeader("Cache-Control", "no-store");
  server.streamFile(f, "text/html");
  f.close();
}

void handleRoot() { sendFile(g_apActive && g_net != NS_ONLINE ? "/setup.html" : "/index.html"); }

void handleStatusApi() {
  char timeStr[24] = "-", roomEsc[80], tStr[12] = "null", rhStr[12] = "null", hiStr[12] = "null", rssiStr[8] = "null";
  if (timeSynced()) {
    time_t now = time(nullptr);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &tmv);
  }
  jsonEscape(cfgRoom, roomEsc, sizeof(roomEsc));
  float t = g_tempC, h = g_rh;
  if (!(isnan(t) || isnan(h) || g_dhtError)) {
    snprintf(tStr, sizeof(tStr), "%.1f", t);
    snprintf(rhStr, sizeof(rhStr), "%.0f", h);
    snprintf(hiStr, sizeof(hiStr), "%.1f", dht.computeHeatIndex(t, h, false));
  }
  if (wifiUp()) snprintf(rssiStr, sizeof(rssiStr), "%d", (int)WiFi.RSSI());

  char body[520];
  snprintf(body, sizeof(body),
           "{\"device_id\":\"%s\",\"room\":\"%s\",\"time\":\"%s\",\"temp\":%s,\"rh\":%s,\"hi\":%s,"
           "\"status\":\"SAFE\",\"flame\":%s,\"door\":\"%s\",\"uid\":\"%s\",\"rssi\":%s,\"chip\":%.1f,"
           "\"fw\":\"%s\",\"conn\":\"%s\",\"up\":%lu}",
           DEVICE_ID, roomEsc, timeStr, tStr, rhStr, hiStr,
           g_flame ? "true" : "false", g_doorUnlocked ? "UNLOCKED" : "LOCKED", g_lastUid, rssiStr,
           temperatureRead(), FW_VERSION, connStateStr(), (unsigned long)(millis() / 1000));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void handleScanApi() {
  if (!g_apActive) { server.send(403, "application/json", "{\"error\":\"not in setup mode\"}"); return; }
  int n = WiFi.scanComplete();
  if (n >= 0) {
    scanCount = 0;
    for (int i = 0; i < n; i++) {
      String s = WiFi.SSID(i);
      if (s.length() == 0) continue;
      int8_t r = (int8_t)WiFi.RSSI(i);
      bool dup = false;
      for (int k = 0; k < scanCount; k++) {
        if (s == scanNets[k].ssid) { dup = true; if (r > scanNets[k].rssi) scanNets[k].rssi = r; break; }
      }
      if (dup || scanCount >= 20) continue;
      copyArg(scanNets[scanCount].ssid, sizeof(scanNets[0].ssid), s);
      scanNets[scanCount].rssi = r;
      scanNets[scanCount].lock = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      scanCount++;
    }
    for (int i = 1; i < scanCount; i++) {          // strongest first
      ScanNet key = scanNets[i]; int j = i - 1;
      while (j >= 0 && scanNets[j].rssi < key.rssi) { scanNets[j + 1] = scanNets[j]; j--; }
      scanNets[j + 1] = key;
    }
    WiFi.scanDelete();
    scanAt = millis();
  }
  bool scanning = (n == WIFI_SCAN_RUNNING);
  bool wantNew = server.hasArg("force") || !scanEver || millis() - scanAt > 15000;
  if (!scanning && n < 0 && g_net == NS_SETUP && wantNew) {
    WiFi.scanNetworks(true);
    scanEver = true;
    scanning = true;
  }
  String out = "{\"scanning\":";
  out += scanning ? "true" : "false";
  out += ",\"nets\":[";
  for (int i = 0; i < scanCount; i++) {
    char esc[80], item[130];
    jsonEscape(scanNets[i].ssid, esc, sizeof(esc));
    snprintf(item, sizeof(item), "%s{\"ssid\":\"%s\",\"rssi\":%d,\"lock\":%s}", i ? "," : "", esc,
             (int)scanNets[i].rssi, scanNets[i].lock ? "true" : "false");
    out += item;
  }
  out += "]}";
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

void handleSetupApi() {
  char roomEsc[80], ssidEsc[80], srvEsc[280], errEsc[220], body[820];
  jsonEscape(cfgRoom, roomEsc, sizeof(roomEsc));
  jsonEscape(cfgSsid, ssidEsc, sizeof(ssidEsc));
  jsonEscape(cfgSrv, srvEsc, sizeof(srvEsc));
  jsonEscape(setupError, errEsc, sizeof(errEsc));
  const char* st = g_net == NS_TRIAL ? "connecting" : (g_net == NS_ONLINE ? "connected" : "idle");
  snprintf(body, sizeof(body),
           "{\"state\":\"%s\",\"error\":\"%s\",\"ssid\":\"%s\",\"room\":\"%s\",\"server\":\"%s\",\"hasToken\":%s,\"ip\":\"%s\",\"mdns\":\"%s\"}",
           st, errEsc, ssidEsc, roomEsc, srvEsc, cfgToken[0] ? "true" : "false",
           WiFi.localIP().toString().c_str(), mdnsHost);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", body);
}

void handleSave() {
  if (!g_apActive || g_net != NS_SETUP) { server.send(409, "application/json", "{\"ok\":false,\"error\":\"busy\"}"); return; }
  String ssid = server.arg("ssid"), room = server.arg("room"), token = server.arg("token"), srv = server.arg("server");
  String pass = server.arg("pass");
  ssid.trim(); room.trim(); token.trim(); srv.trim();
  while (srv.endsWith("/")) srv.remove(srv.length() - 1);

  const char* err = nullptr;
  if (ssid.length() == 0 || ssid.length() > 32)            err = "Choose a network";
  else if (pass.length() > 64)                              err = "Password too long";
  else if (room.length() == 0 || room.length() > 32)        err = "Enter the Room ID";
  else if (token.length() > 128)                            err = "Token too long";
  else if (token.length() == 0 && cfgToken[0] == 0)         err = "Enter the device token";
  else if (!srv.startsWith("https://") || srv.length() > 120) err = "Server URL must start with https://";
  if (err) {
    char body[120];
    snprintf(body, sizeof(body), "{\"ok\":false,\"error\":\"%s\"}", err);
    server.send(400, "application/json", body);
    return;
  }

  copyArg(cfgSsid, sizeof(cfgSsid), ssid);
  copyArg(cfgPass, sizeof(cfgPass), pass);
  copyArg(cfgRoom, sizeof(cfgRoom), room);
  if (token.length()) copyArg(cfgToken, sizeof(cfgToken), token);
  copyArg(cfgSrv, sizeof(cfgSrv), srv);

  // Everything is saved in flash (NVS). "ok" becomes true once the network has connected.
  prefs.putString("ssid", cfgSsid);
  prefs.putString("pass", cfgPass);
  prefs.putString("room", cfgRoom);
  prefs.putString("token", cfgToken);
  prefs.putString("srv", cfgSrv);
  prefs.putBool("ok", false);
  g_authErr = false;
  safePrintf("[NET] settings saved to NVS: ssid=\"%s\" room=\"%s\" server=%s\n", cfgSsid, cfgRoom, cfgSrv);

  server.send(200, "application/json", "{\"ok\":true}");
  startTrial();
}

void handleNotFound() {
  if (g_apActive) {                               // captive portal redirect
    server.sendHeader("Location", "http://192.168.4.1/");
    server.send(302, "text/plain", "");
  } else {
    server.send(404, "text/plain", "not found");
  }
}

// ===================== TASK 5: NETWORK + WEB SERVER [Core 0] =====================
// Not subscribed to the task watchdog: a slow web client may legitimately hold it.
void TaskNet(void *pvParameters) {
  for (;;) {
    if (g_factoryReset) doFactoryReset();
    if (g_apActive) dns.processNextRequest();
    server.handleClient();
    netTick();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ===================== TASK 6: CLOUD (HTTPS, certificate checked) [Core 0] =====================
esp_http_client_handle_t httpc = NULL;
char httpErr[40] = "";

void httpClose() {
  if (httpc) { esp_http_client_cleanup(httpc); httpc = NULL; }
}

// Returns the HTTP status, or -1 on a connection / TLS failure.
// The server certificate is verified against the built-in root CA bundle.
int httpPostOnce(const char* path, const char* body) {
  char url[200], auth[150];
  snprintf(url, sizeof(url), "%s%s", cfgSrv, path);
  snprintf(auth, sizeof(auth), "Bearer %s", cfgToken);

  if (!httpc) {
    esp_http_client_config_t c = {};
    c.url = url;
    c.method = HTTP_METHOD_POST;
    c.timeout_ms = 5000;
    c.crt_bundle_attach = esp_crt_bundle_attach;
    c.keep_alive_enable = true;                   // one TLS session reused for all messages
    httpc = esp_http_client_init(&c);
    if (!httpc) { strcpy(httpErr, "init failed"); return -1; }
  }
  esp_http_client_set_url(httpc, url);
  esp_http_client_set_method(httpc, HTTP_METHOD_POST);
  esp_http_client_set_header(httpc, "Content-Type", "application/json");
  esp_http_client_set_header(httpc, "Authorization", auth);
  esp_http_client_set_post_field(httpc, body, strlen(body));

  esp_err_t e = esp_http_client_perform(httpc);
  if (e != ESP_OK) {
    strncpy(httpErr, esp_err_to_name(e), sizeof(httpErr) - 1);
    httpClose();
    return -1;
  }
  return esp_http_client_get_status_code(httpc);
}

int httpPost(const char* path, const char* body, bool hadSession) {
  if (strncmp(cfgSrv, "https://", 8) != 0) { strcpy(httpErr, "server URL not https"); return -1; }
  int code = httpPostOnce(path, body);
  if (code < 0 && hadSession) code = httpPostOnce(path, body);   // kept-alive socket may have gone stale
  return code;
}

void buildBody(const OutMsg& m, char* body, size_t n) {
  char roomEsc[80];
  jsonEscape(cfgRoom, roomEsc, sizeof(roomEsc));
  if (m.isEvent) {
    char detEsc[160];
    jsonEscape(m.detail, detEsc, sizeof(detEsc));
    snprintf(body, n, "{\"device_id\":\"%s\",\"room_id\":\"%s\",\"seq\":%lu,\"ts\":%lu,\"type\":\"%s\",\"detail\":\"%s\"}",
             DEVICE_ID, roomEsc, (unsigned long)m.seq, (unsigned long)m.ts, m.type, detEsc);
    return;
  }
  char tStr[12] = "null", rhStr[12] = "null", hiStr[12] = "null", rssiStr[8] = "null";
  if (m.good) {
    snprintf(tStr, sizeof(tStr), "%.1f", m.t);
    snprintf(rhStr, sizeof(rhStr), "%.0f", m.rh);
    snprintf(hiStr, sizeof(hiStr), "%.1f", m.hi);
  }
  if (m.rssi != 0) snprintf(rssiStr, sizeof(rssiStr), "%d", (int)m.rssi);
  snprintf(body, n,
           "{\"device_id\":\"%s\",\"room_id\":\"%s\",\"seq\":%lu,\"ts\":%lu,\"uptime_s\":%lu,"
           "\"temp_c\":%s,\"rh\":%s,\"heat_index_c\":%s,\"flame\":%s,\"door\":\"%s\","
           "\"status\":\"SAFE\",\"heat_trend\":\"STABLE\",\"rssi\":%s,\"heap_kb\":%lu,"
           "\"chip_temp_c\":%.1f,\"fw\":\"%s\",\"quality\":\"%s\"}",
           DEVICE_ID, roomEsc, (unsigned long)m.seq, (unsigned long)m.ts, (unsigned long)m.uptimeS,
           tStr, rhStr, hiStr, m.flame ? "true" : "false", m.doorUnlocked ? "UNLOCKED" : "LOCKED",
           rssiStr, (unsigned long)m.heapKb, m.chip, FW_VERSION, m.good ? "good" : "bad");
}

// Not subscribed to the task watchdog: a TLS handshake may block for seconds.
void TaskCloud(void *pvParameters) {
  static char body[640];
  static OutMsg lastSent;
  bool haveLast = false, everUp = false, timeAnnounced = false, authLogged = false;
  uint32_t backoffMs = 0, nextTry = 0, downSince = 0;

  for (;;) {
    if (!wifiUp()) {
      if (g_serverUp) {
        g_serverUp = false;
        downSince = millis();
        safePrintf("[NET] server disconnected (Wi-Fi lost)\n");
      }
      httpClose();
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    if (!timeSynced()) { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
    if (!timeAnnounced) {
      timeAnnounced = true;
      safePrintf("[NET] time synced (NTP) unix=%lu\n", (unsigned long)time(nullptr));
    }

    if (g_replayReq) {
      g_replayReq = false;
      if (haveLast) {
        buildBody(lastSent, body, sizeof(body));
        int code = httpPost(lastSent.isEvent ? "/api/v1/events" : "/api/v1/telemetry", body, httpc != NULL);
        safePrintf("[NET] replay test seq=%lu -> HTTP %d (%s)\n", (unsigned long)lastSent.seq, code,
                   code == 409 ? "rejected by server, not stored twice" : "unexpected");
      } else {
        safePrintf("[NET] replay test: nothing delivered yet\n");
      }
    }

    if ((int32_t)(millis() - nextTry) < 0) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }

    OutMsg m;
    if (xQueuePeek(outQ, &m, pdMS_TO_TICKS(100)) != pdTRUE) continue;
    if (m.ts == 0) {   // taken before the clock was set: work back from its age
      m.ts = (uint32_t)time(nullptr) - (millis() - m.createdMs) / 1000;
    }
    buildBody(m, body, sizeof(body));
    int code = httpPost(m.isEvent ? "/api/v1/events" : "/api/v1/telemetry", body, httpc != NULL);

    if (code < 0 || code >= 500) {
      // server unreachable: keep the message, retry with growing gaps (same seq)
      if (code >= 500) snprintf(httpErr, sizeof(httpErr), "HTTP %d", code);
      if (g_serverUp) {
        g_serverUp = false;
        downSince = millis();
        safePrintf("[NET] server disconnected (%s)\n", httpErr);
      }
      backoffMs = backoffMs == 0 ? 1000 : (backoffMs * 2 > 8000 ? 8000 : backoffMs * 2);
      nextTry = millis() + backoffMs;
      safePrintf("[NET] server unreachable (%s), retry in %lus\n", httpErr, (unsigned long)(backoffMs / 1000));
      continue;
    }

    // The server answered, so it is reachable
    backoffMs = 0;
    if (!g_serverUp) {
      g_serverUp = true;
      if (!everUp) safePrintf("[NET] server connected %s (TLS, certificate verified)\n", cfgSrv);
      else safePrintf("[NET] server reconnected after %lus\n", (unsigned long)((millis() - downSince) / 1000));
      everUp = true;
    }

    if (code == 401 || code == 403) {
      g_authErr = true;
      if (!authLogged) { authLogged = true; safePrintf("[ERROR] server rejected the device token (HTTP %d)\n", code); }
      nextTry = millis() + 5000;
      continue;
    }
    g_authErr = false;
    authLogged = false;

    if (code == 409) safePrintf("[NET] seq=%lu already stored on server (duplicate ignored)\n", (unsigned long)m.seq);
    else if (code >= 400) safePrintf("[ERROR] server refused seq=%lu (HTTP %d), dropped\n", (unsigned long)m.seq, code);
    else { lastSent = m; haveLast = true; }

    // Remove it from the queue (unless a producer already dropped it to make room)
    OutMsg head;
    if (xQueuePeek(outQ, &head, 0) == pdTRUE && head.seq == m.seq) xQueueReceive(outQ, &head, 0);
  }
}

// ===================== SETUP & LOOP =====================
void loadSettings() {
  prefs.getString("ssid", cfgSsid, sizeof(cfgSsid));
  prefs.getString("pass", cfgPass, sizeof(cfgPass));
  prefs.getString("room", cfgRoom, sizeof(cfgRoom));
  prefs.getString("token", cfgToken, sizeof(cfgToken));
  prefs.getString("srv", cfgSrv, sizeof(cfgSrv));
  if (cfgSrv[0] == 0) strcpy(cfgSrv, DEFAULT_SERVER_URL);

  // AP password: 8 random characters, created on first boot and kept in NVS (never in code)
  apPass[0] = 0;
  prefs.getString("appw", apPass, sizeof(apPass));
  if (strlen(apPass) != 8) {
    const char* alphabet = "abcdefghjkmnpqrstuvwxyz23456789";
    for (int i = 0; i < 8; i++) apPass[i] = alphabet[esp_random() % 31];
    apPass[8] = 0;
    prefs.putString("appw", apPass);
  }

  snprintf(apSsid, sizeof(apSsid), "SmartClass-%s", TEAM_ID);
  snprintf(mdnsHost, sizeof(mdnsHost), "smartclass-%s", TEAM_ID);
  for (char* p = mdnsHost; *p; p++) *p = tolower((unsigned char)*p);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  serialMutex = xSemaphoreCreateMutex();
  seqMutex = xSemaphoreCreateMutex();
  outQ = xQueueCreate(OUTQ_LEN, sizeof(OutMsg));

  // Level 1 task 6: check the flame sensor FIRST at start-up
  pinMode(PIN_FLAME, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);
  pinMode(PIN_BOOT_BTN, INPUT_PULLUP);

  g_flame = (digitalRead(PIN_FLAME) == LOW);
  g_doorUnlocked = g_flame;

  // Saved settings + seq counter (continue above everything ever reserved)
  prefs.begin("sc", false);
  g_seq = prefs.getUInt("seq", 0);
  g_seqReserved = g_seq + SEQ_BLOCK;
  prefs.putUInt("seq", g_seqReserved);
  loadSettings();

  // BOOT event: printed now, delivered to the cloud as soon as it is reachable
  char detail[40];
  snprintf(detail, sizeof(detail), "reason=%s", getResetReasonStr(esp_reset_reason()));
  emitEvent("BOOT", detail);

  dht.begin();
  SPI.begin(PIN_RFID_SCK, PIN_RFID_MISO, PIN_RFID_MOSI, PIN_RFID_SS);
  rfid.PCD_Init();

  // Web pages live in LittleFS
  if (LittleFS.begin(true)) {
    syncFile("/setup.html", SETUP_HTML);
    syncFile("/index.html", INDEX_HTML);
  } else {
    safePrintf("[ERROR] LittleFS mount failed (select a partition scheme with SPIFFS/LittleFS)\n");
  }

  // Wi-Fi: we manage saving and reconnecting ourselves
  WiFi.persistent(false);
  WiFi.setAutoReconnect(false);
  WiFi.onEvent(onWiFiEvent);
  WiFi.setHostname(mdnsHost);

  if (cfgSsid[0] == 0) {
    startSetupMode();                         // no saved network -> access point, blue light
  } else if (prefs.getBool("ok", false)) {
    WiFi.mode(WIFI_STA);                      // known-good network -> connect with no setup
    g_net = NS_CONNECTING;
    safePrintf("[NET] connecting to \"%s\" ...\n", cfgSsid);
    beginAttempt();
  } else {
    startSetupMode();                         // details saved but never proven (power cut mid-setup)
    startTrial();
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatusApi);
  server.on("/api/scan", HTTP_GET, handleScanApi);
  server.on("/api/setup", HTTP_GET, handleSetupApi);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  server.begin();

  // Task watchdog (5 s, restart on freeze)
  esp_task_wdt_config_t twdt_config = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  if (esp_task_wdt_reconfigure(&twdt_config) != ESP_OK) {
    esp_task_wdt_init(&twdt_config);
  }

  xTaskCreatePinnedToCore(TaskFlameAndLED,     "FlameLED", 4096,  NULL, 2, &hTaskFlameLED, 0);
  xTaskCreatePinnedToCore(TaskRFID,            "RFID",     6144,  NULL, 2, &hTaskRFID,     1);
  xTaskCreatePinnedToCore(TaskDHT,             "DHT",      4096,  NULL, 1, &hTaskDHT,      1);
  xTaskCreatePinnedToCore(TaskSerialAndStatus, "Serial",   8192,  NULL, 1, &hTaskSerial,   1);
  xTaskCreatePinnedToCore(TaskNet,             "Net",      8192,  NULL, 1, &hTaskNet,      0);
  xTaskCreatePinnedToCore(TaskCloud,           "Cloud",    12288, NULL, 1, &hTaskCloud,    0);
}

void loop() {
  vTaskDelete(NULL);   // all work runs in FreeRTOS tasks
}
