/**
 * FermWatch - Fermentation display (gravity / temperature / trend)
 *
 * ESP8266 (D1 Mini) + MAX7219 8x32. Readings arrive over MQTT (Home Assistant);
 * the configuration is edited through a web page and saved as JSON (LittleFS).
 *
 * Pins: GPIO14 (D5) = CLK, GPIO13 (D7) = CS, GPIO15 (D8) = DIN
 */

#include <Arduino.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <MD_MAX72xx.h>
#include <MD_Parola.h>
#include <PubSubClient.h>
#include <Updater.h>

#include "config.h"
#include "font.h"
#include "logic.h"
#include "web_ui.h"

#define FIRMWARE_VERSION "0.1.0"

#define AP_SSID "FermWatch-Setup"
#define WIFI_CONNECT_TIMEOUT 20000UL
#define MQTT_RECONNECT_INTERVAL 5000UL

#define CLK_PIN D5
#define CS_PIN D7
#define DIN_PIN D8
#define MAX_DEVICES 4
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW

// ============ Objets globaux ============
Config cfg;
ESP8266WebServer server(80);
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
MD_Parola display(HARDWARE_TYPE, DIN_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

// ============ Sensors ============
Sensor sensors[MAX_SENSORS];

// ============ Etat ============
char textBuf[64];
int pageIndex = 0;
bool displayDirty = true;
bool apActive = false;
bool wifiWasConnected = false;
unsigned long wifiStart = 0;
unsigned long lastMqttAttempt = 0;
unsigned long restartAt = 0;

// ============ Pages ============
bool isStale(const Sensor &s) { return isStale(s, millis(), cfg.staleMinutes); }

void purgeExpired() {
  for (Sensor &s : sensors)
    if (s.used && isExpired(s, millis(), cfg.expireMinutes)) {
      s.used = false;
      displayDirty = true;
    }
}

PageOptions pageOptions() {
  PageOptions o;
  o.showName = cfg.showName;
  o.showSg = cfg.showSg;
  o.showTemp = cfg.showTemp;
  o.showTrend = cfg.showTrend;
  o.plato = cfg.gravUnit == "P";
  o.tempUnit = cfg.tempUnit.length() ? cfg.tempUnit[0] : 'C';
  o.staleMinutes = cfg.staleMinutes;
  return o;
}

// Returns the page count and, if 0 <= target < count, fills text/isName with
// that page.
int collectPages(int target, String &text, bool &isName) {
  char buf[64];
  int n = collectPages(sensors, MAX_SENSORS, pageOptions(), millis(), target,
                       buf, sizeof(buf), isName);
  if (target >= 0 && target < n)
    text = buf;
  return n;
}

extern const uint8_t PROGMEM _sysfont[]; // MD_MAX72XX standard font

// Copy of the standard font moved down one row (bit 0 = top row): capitals use
// the 7 bottom rows. Lowercase descenders would be clipped, but names are
// uppercase.
MD_MAX72XX::fontType_t *nameFont = nullptr;

void buildNameFont() {
  size_t pos = 7; // header: 'F', 2, first (16 bit), last (16 bit), height
  uint16_t count =
      ((pgm_read_byte(_sysfont + 4) << 8) | pgm_read_byte(_sysfont + 5)) -
      ((pgm_read_byte(_sysfont + 2) << 8) | pgm_read_byte(_sysfont + 3)) + 1;
  for (uint16_t i = 0; i < count; i++)
    pos += 1 + pgm_read_byte(_sysfont + pos);
  uint8_t *buf = (uint8_t *)malloc(pos);
  if (!buf)
    return;
  for (size_t i = 0; i < pos; i++)
    buf[i] = pgm_read_byte(_sysfont + i);
  size_t q = 7;
  for (uint16_t i = 0; i < count; i++) {
    uint8_t w = buf[q++];
    for (uint8_t c = 0; c < w; c++)
      buf[q + c] <<= 1;
    q += w;
  }
  nameFont = buf;
}

// Sensor name: use the standard full-height font when it fits the display;
// otherwise fall back to the narrow font and scroll.
void prepareFont(bool isName) {
  display.setCharSpacing(1);
  if (isName) {
    display.setFont(nameFont);
    if (display.getTextColumns(textBuf) <= MAX_DEVICES * 8)
      return;
  }
  display.setFont(narrowFont);
}

void showText(const String &text, bool hold, bool isName = false) {
  text.toCharArray(textBuf, sizeof(textBuf));
  display.displayClear();
  prepareFont(isName);
  uint16_t width = display.getTextColumns(textBuf);
  if (width <= MAX_DEVICES * 8) {
    // Text fits: static display for the page duration.
    uint16_t pause = hold ? 0 : cfg.pageSeconds * 1000;
    display.displayText(textBuf, PA_CENTER, cfg.scrollMs, pause, PA_PRINT,
                        PA_NO_EFFECT);
  } else {
    display.displayText(textBuf, PA_LEFT, cfg.scrollMs, 0, PA_SCROLL_LEFT,
                        PA_SCROLL_LEFT);
  }
}

void nextPage() {
  if (apActive && WiFi.status() != WL_CONNECTED) {
    showText(String("AP ") + WiFi.softAPIP().toString(), false);
    return;
  }
  String text;
  bool isName = false;
  int n = collectPages(-1, text, isName);
  if (n == 0) {
    showText("--", false);
    return;
  }
  pageIndex %= n;
  collectPages(pageIndex, text, isName);
  showText(text, false, isName);
  pageIndex++;
}

bool splashActive = false;

// Boot banner: standard font on the 7 bottom rows, scrolled once.
void startSplash() {
  snprintf(textBuf, sizeof(textBuf), "FermWatch v%s", FIRMWARE_VERSION);
  display.displayClear();
  display.setFont(nameFont);
  display.setCharSpacing(1);
  display.displayText(textBuf, PA_LEFT, cfg.scrollMs * 2, 0, PA_SCROLL_LEFT,
                      PA_SCROLL_LEFT); // twice as slow as normal text
  splashActive = true;
}

void updateDisplay() {
  if (splashActive) {
    if (display.displayAnimate()) {
      splashActive = false;
      displayDirty = true;
    }
    return;
  }
  if (displayDirty) {
    displayDirty = false;
    display.setIntensity(cfg.brightness);
    pageIndex = 0;
    nextPage();
  }
  if (display.displayAnimate())
    nextPage();
}

// ============ MQTT ============
String baseTopic() { return "fermwatch/" + cfg.deviceName; }

void publishConfigState() {
  if (!mqtt.connected())
    return;
  JsonDocument doc;
  configToJson(cfg, doc.to<JsonObject>(), false);
  String out;
  serializeJson(doc, out);
  mqtt.publish((baseTopic() + "/config").c_str(), out.c_str(), true);
}

String sensorFilter() { return cfg.topicBase + "/+"; }

void subscribeTopics() {
  mqtt.subscribe(sensorFilter().c_str());
  mqtt.subscribe((baseTopic() + "/config/set").c_str());
}

void unsubscribeTopics() { mqtt.unsubscribe(sensorFilter().c_str()); }

void handleSensorMessage(const String &topic, const String &body) {
  if (applySensorMessage(sensors, MAX_SENSORS, topic.c_str(), body.c_str(),
                         millis()) == MSG_REMOVED)
    displayDirty = true;
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String t(topic);
  String body;
  body.reserve(length);
  for (unsigned int i = 0; i < length; i++)
    body += (char)payload[i];

  if (t == baseTopic() + "/config/set") {
    JsonDocument doc;
    if (!deserializeJson(doc, body) && doc.is<JsonObject>()) {
      unsubscribeTopics();
      bool restart = applyConfig(cfg, doc.as<JsonObjectConst>());
      saveConfig(cfg);
      subscribeTopics();
      publishConfigState();
      displayDirty = true;
      if (restart)
        restartAt = millis() + 1000;
    }
    return;
  }
  handleSensorMessage(t, body);
}

void connectMQTT() {
  if (cfg.mqttHost.isEmpty() || WiFi.status() != WL_CONNECTED ||
      mqtt.connected())
    return;
  if (millis() - lastMqttAttempt < MQTT_RECONNECT_INTERVAL)
    return;
  lastMqttAttempt = millis();

  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  String status = baseTopic() + "/status";
  String clientId = cfg.deviceName + "-" + String(ESP.getChipId(), HEX);
  bool ok = cfg.mqttUser.length()
                ? mqtt.connect(clientId.c_str(), cfg.mqttUser.c_str(),
                               cfg.mqttPass.c_str(), status.c_str(), 0, true,
                               "offline")
                : mqtt.connect(clientId.c_str(), nullptr, nullptr,
                               status.c_str(), 0, true, "offline");
  if (ok) {
    Serial.println("[MQTT] connected");
    mqtt.publish(status.c_str(), "online", true);
    subscribeTopics();
    publishConfigState();
  } else {
    Serial.printf("[MQTT] failed rc=%d\n", mqtt.state());
  }
}

// ============ Wi-Fi ============
void startAccessPoint(bool apOnly = false) {
  if (apActive)
    return;
  WiFi.mode(apOnly ? WIFI_AP : WIFI_AP_STA);
  WiFi.softAP(AP_SSID);
  apActive = true;
  displayDirty = true;
  Serial.printf("[WiFi] AP %s on %s\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());
}

void setupWiFi() {
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.hostname(cfg.deviceName);
  if (cfg.webPassword.isEmpty()) {
    startAccessPoint(true);
    return;
  }
  if (cfg.ssid.isEmpty()) {
    startAccessPoint();
    return;
  }
  Serial.printf("[WiFi] connecting to '%s'\n", cfg.ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid.c_str(), cfg.password.c_str());
  wifiStart = millis();
}

void handleWiFi() {
  if (cfg.webPassword.isEmpty())
    return;
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wifiWasConnected) {
    Serial.printf("[WiFi] connected, IP %s\n",
                  WiFi.localIP().toString().c_str());
    MDNS.begin(cfg.deviceName);
    if (apActive) {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      apActive = false;
    }
    displayDirty = true;
  }
  wifiWasConnected = connected;
  // No network after the timeout: fallback AP, STA reconnection keeps going.
  if (!connected && !apActive && millis() - wifiStart > WIFI_CONNECT_TIMEOUT) {
    Serial.printf("[WiFi] connection to '%s' failed (status %d)\n",
                  cfg.ssid.c_str(), WiFi.status());
    startAccessPoint();
  }
  if (connected)
    MDNS.update();
}

// ============ Serveur Web ============
bool authorizeWebRequest() {
  if (cfg.webPassword.isEmpty()) {
    if (apActive && WiFi.status() != WL_CONNECTED)
      return true;
    server.requestAuthentication(DIGEST_AUTH, "FermWatch");
    return false;
  }
  if (server.authenticate("admin", cfg.webPassword.c_str()))
    return true;
  server.requestAuthentication(DIGEST_AUTH, "FermWatch");
  return false;
}

void sendJson(int code, const JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

// Sent in small chunks: the low-memory lwIP build has tiny TCP buffers.
void handleRoot() {
  if (!authorizeWebRequest())
    return;
  const size_t total = strlen_P(INDEX_HTML);
  server.setContentLength(total);
  server.send(200, "text/html", "");
  for (size_t off = 0; off < total; off += 512) {
    server.sendContent_P(INDEX_HTML + off, min((size_t)512, total - off));
    yield();
  }
}

void handleGetConfig() {
  if (!authorizeWebRequest())
    return;
  JsonDocument doc;
  configToJson(cfg, doc["config"].to<JsonObject>(), false);
  doc["webPasswordSet"] = !cfg.webPassword.isEmpty();
  sendJson(200, doc);
}

void handlePostConfig() {
  if (!authorizeWebRequest())
    return;
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) || !doc.is<JsonObject>()) {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"Invalid JSON\"}");
    return;
  }
  String webPassword = doc["webPassword"] | "";
  if (webPassword.length() > 0 &&
      (webPassword.length() < 8 || webPassword.length() > 64)) {
    server.send(
        400, "application/json",
        "{\"ok\":false,\"error\":\"Web password must be 8 to 64 characters\"}");
    return;
  }
  bool needsProvisioningRestart =
      cfg.webPassword.isEmpty() && webPassword.length() > 0;
  unsubscribeTopics();
  bool restart = applyConfig(cfg, doc.as<JsonObjectConst>());
  if (webPassword.length() > 0)
    cfg.webPassword = webPassword;
  restart = restart || needsProvisioningRestart;
  bool saved = saveConfig(cfg);
  Serial.printf("[Web] config POST: saved=%d restart=%d ssid='%s'\n", saved,
                restart, cfg.ssid.c_str());
  if (mqtt.connected())
    mqtt.disconnect(); // reconnect with the new broker/topics settings
  displayDirty = true;
  if (!saved) {
    server.send(500, "application/json",
                "{\"ok\":false,\"error\":\"Flash write failed\"}");
    return;
  }
  if (restart)
    restartAt = millis() + 1500;
  server.send(200, "application/json",
              restart ? "{\"ok\":true,\"restart\":true}"
                      : "{\"ok\":true,\"restart\":false}");
}

void handleState() {
  if (!authorizeWebRequest())
    return;
  JsonDocument doc;
  doc["version"] = FIRMWARE_VERSION;
  doc["mqtt"] = mqtt.connected();
  doc["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString()
                                            : WiFi.softAPIP().toString();
  doc["host"] = cfg.deviceName;
  doc["rssi"] = WiFi.RSSI();
  JsonArray arr = doc["sensors"].to<JsonArray>();
  for (const Sensor &s : sensors) {
    if (!s.used)
      continue;
    JsonObject o = arr.add<JsonObject>();
    o["name"] = s.name;
    if (!isnan(s.sg))
      o["sg"] = s.sg;
    if (!isnan(s.temp))
      o["temp"] = s.temp;
    if (!isnan(s.trend))
      o["trend"] = s.trend;
    o["age"] = (millis() - s.lastUpdate) / 1000;
    o["stale"] = isStale(s);
  }
  sendJson(200, doc);
}

// Firmware upload (OTA). Never available until a Web password is set, so the
// open setup access point cannot be used to flash the device.
bool updateAuthorized = false;
String updateError;

void handleUpdateUpload() {
  HTTPUpload &upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    updateError = "";
    updateAuthorized = !cfg.webPassword.isEmpty() &&
                       server.authenticate("admin", cfg.webPassword.c_str());
    if (!updateAuthorized)
      return;
    Serial.printf("[OTA] receiving %s\n", upload.filename.c_str());
    uint32_t maxSize = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSize))
      updateError = Update.getErrorString();
  } else if (!updateAuthorized) {
    return;
  } else if (upload.status == UPLOAD_FILE_WRITE && updateError.isEmpty()) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize)
      updateError = Update.getErrorString();
  } else if (upload.status == UPLOAD_FILE_END && updateError.isEmpty()) {
    if (!Update.end(true))
      updateError = Update.getErrorString();
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
  }
}

void handleUpdateDone() {
  if (cfg.webPassword.isEmpty()) {
    server.send(403, "application/json",
                "{\"ok\":false,\"error\":\"Set a Web password first\"}");
    return;
  }
  if (!updateAuthorized) {
    server.requestAuthentication(DIGEST_AUTH, "FermWatch");
    return;
  }
  updateAuthorized = false;
  if (!updateError.isEmpty()) {
    Serial.printf("[OTA] failed: %s\n", updateError.c_str());
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = updateError;
    sendJson(500, doc);
    return;
  }
  Serial.println("[OTA] done, restarting");
  server.send(200, "application/json", "{\"ok\":true}");
  restartAt = millis() + 1000;
}

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/state", HTTP_GET, handleState);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

// ============ Setup / loop ============
void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.printf("\n[*] FermWatch v%s\n", FIRMWARE_VERSION);

  if (!LittleFS.begin()) {
    LittleFS.format();
    LittleFS.begin();
  }
  loadConfig(cfg);
  Serial.printf("[Config] ssid='%s' mqttHost='%s'\n", cfg.ssid.c_str(),
                cfg.mqttHost.c_str());

  display.begin();
  buildNameFont();
  display.setFont(narrowFont);
  display.setIntensity(cfg.brightness);
  display.displayClear();
  startSplash();

  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(1024);

  setupWiFi();
  setupWebServer();
}

void loop() {
  server.handleClient();
  handleWiFi();
  connectMQTT();
  mqtt.loop();
  updateDisplay();

  static unsigned long lastPurge = 0;
  if (millis() - lastPurge > 30000UL) {
    lastPurge = millis();
    purgeExpired();
  }

  if (restartAt && millis() > restartAt) {
    delay(100);
    ESP.restart();
  }
}
