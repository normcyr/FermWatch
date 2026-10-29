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

#include "config.h"
#include "font.h"
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
#define MAX_SENSORS 16
#define NAME_LEN 24

struct Sensor
{
  char name[NAME_LEN] = "";
  char key[NAME_LEN] = ""; // last topic segment, used to clear the sensor with an empty message
  float sg = NAN;
  float temp = NAN;
  float trend = NAN;
  unsigned long lastUpdate = 0; // millis() of the last message
  bool used = false;
};
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
bool isStale(const Sensor &s)
{
  return (millis() - s.lastUpdate) > (unsigned long)cfg.staleMinutes * 60000UL;
}

bool isExpired(const Sensor &s)
{
  return (millis() - s.lastUpdate) > (unsigned long)cfg.expireMinutes * 60000UL;
}

void purgeExpired()
{
  for (Sensor &s : sensors)
    if (s.used && isExpired(s))
    {
      s.used = false;
      displayDirty = true;
    }
}

String formatValue(float v, int decimals, bool sign)
{
  char b[16];
  snprintf(b, sizeof(b), sign ? "%+.*f" : "%.*f", decimals, v);
  return String(b);
}

float convertTemp(float c)
{
  if (cfg.tempUnit == "F")
    return c * 9.0f / 5.0f + 32.0f;
  if (cfg.tempUnit == "K")
    return c + 273.15f;
  if (cfg.tempUnit == "R")
    return (c + 273.15f) * 9.0f / 5.0f;
  return c;
}

// Kelvin has no degree sign; the others use "^" (degree in the display font).
const char *tempSuffix()
{
  if (cfg.tempUnit == "F")
    return "^F";
  if (cfg.tempUnit == "K")
    return "K";
  if (cfg.tempUnit == "R")
    return "^R";
  return "^C";
}

// Degrees Plato from specific gravity (standard polynomial).
float sgToPlato(float sg)
{
  return -616.868f + 1111.14f * sg - 630.272f * sg * sg + 135.997f * sg * sg * sg;
}

// Walks every page (per sensor: name, SG, temperature, trend) in a stable order.
// Returns the page count and, if 0 <= target < count, fills text/isName with that page.
int collectPages(int target, String &text, bool &isName)
{
  int n = 0;
  auto add = [&](const String &t, bool name)
  {
    if (n == target)
    {
      text = t;
      isName = name;
    }
    n++;
  };
  for (const Sensor &s : sensors)
  {
    if (!s.used)
      continue;
    String flag = isStale(s) ? "!" : "";
    if (cfg.showName)
    {
      String name = s.name;
      name.toUpperCase();
      add(name, true);
    }
    bool plato = cfg.gravUnit == "P";
    if (cfg.showSg && !isnan(s.sg))
      add(flag + (plato ? formatValue(sgToPlato(s.sg), 1, false) + "^P" : "SG " + formatValue(s.sg, 3, false)), false);
    if (cfg.showTemp && !isnan(s.temp))
      add(flag + "T " + formatValue(convertTemp(s.temp), 1, false) + tempSuffix(), false);
    if (cfg.showTrend && !isnan(s.trend))
    {
      // The Plato trend is the Plato difference over 24 h, so it needs the current SG.
      if (!plato)
        add(flag + formatValue(s.trend, 4, true) + "/d", false);
      else if (!isnan(s.sg))
        add(flag + formatValue(sgToPlato(s.sg) - sgToPlato(s.sg - s.trend), 1, true) + "^P/d", false);
    }
  }
  return n;
}

extern const uint8_t PROGMEM _sysfont[]; // MD_MAX72XX standard font

// Copy of the standard font moved down one row (bit 0 = top row): capitals use the
// 7 bottom rows. Lowercase descenders would be clipped, but names are uppercase.
MD_MAX72XX::fontType_t *nameFont = nullptr;

void buildNameFont()
{
  size_t pos = 7; // header: 'F', 2, first (16 bit), last (16 bit), height
  uint16_t count = ((pgm_read_byte(_sysfont + 4) << 8) | pgm_read_byte(_sysfont + 5)) -
                   ((pgm_read_byte(_sysfont + 2) << 8) | pgm_read_byte(_sysfont + 3)) + 1;
  for (uint16_t i = 0; i < count; i++)
    pos += 1 + pgm_read_byte(_sysfont + pos);
  uint8_t *buf = (uint8_t *)malloc(pos);
  if (!buf)
    return;
  for (size_t i = 0; i < pos; i++)
    buf[i] = pgm_read_byte(_sysfont + i);
  size_t q = 7;
  for (uint16_t i = 0; i < count; i++)
  {
    uint8_t w = buf[q++];
    for (uint8_t c = 0; c < w; c++)
      buf[q + c] <<= 1;
    q += w;
  }
  nameFont = buf;
}

// Sensor name: use the standard full-height font when it fits the display;
// otherwise fall back to the narrow font and scroll.
void prepareFont(bool isName)
{
  display.setCharSpacing(1);
  if (isName)
  {
    display.setFont(nameFont);
    if (display.getTextColumns(textBuf) <= MAX_DEVICES * 8)
      return;
  }
  display.setFont(narrowFont);
}

void showText(const String &text, bool hold, bool isName = false)
{
  text.toCharArray(textBuf, sizeof(textBuf));
  display.displayClear();
  prepareFont(isName);
  uint16_t width = display.getTextColumns(textBuf);
  if (width <= MAX_DEVICES * 8)
  {
    // Text fits: static display for the page duration.
    uint16_t pause = hold ? 0 : cfg.pageSeconds * 1000;
    display.displayText(textBuf, PA_CENTER, cfg.scrollMs, pause, PA_PRINT, PA_NO_EFFECT);
  }
  else
  {
    display.displayText(textBuf, PA_LEFT, cfg.scrollMs, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  }
}

void nextPage()
{
  if (apActive && WiFi.status() != WL_CONNECTED)
  {
    showText(String("AP ") + WiFi.softAPIP().toString(), false);
    return;
  }
  String text;
  bool isName = false;
  int n = collectPages(-1, text, isName);
  if (n == 0)
  {
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
void startSplash()
{
  snprintf(textBuf, sizeof(textBuf), "FermWatch v%s", FIRMWARE_VERSION);
  display.displayClear();
  display.setFont(nameFont);
  display.setCharSpacing(1);
  display.displayText(textBuf, PA_LEFT, cfg.scrollMs * 2, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT); // twice as slow as normal text
  splashActive = true;
}

void updateDisplay()
{
  if (splashActive)
  {
    if (display.displayAnimate())
    {
      splashActive = false;
      displayDirty = true;
    }
    return;
  }
  if (displayDirty)
  {
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

void publishConfigState()
{
  if (!mqtt.connected())
    return;
  JsonDocument doc;
  configToJson(cfg, doc.to<JsonObject>(), false);
  String out;
  serializeJson(doc, out);
  mqtt.publish((baseTopic() + "/config").c_str(), out.c_str(), true);
}

String sensorFilter() { return cfg.topicBase + "/+"; }

void subscribeTopics()
{
  mqtt.subscribe(sensorFilter().c_str());
  mqtt.subscribe((baseTopic() + "/config/set").c_str());
}

void unsubscribeTopics() { mqtt.unsubscribe(sensorFilter().c_str()); }

// Reads a number from a JSON field, accepting a number or a numeric string.
// "unknown"/"unavailable" and missing fields leave the stored value untouched.
bool readNumber(JsonObjectConst o, const char *lower, const char *upper, float &out)
{
  JsonVariantConst v = o[lower];
  if (v.isNull())
    v = o[upper];
  if (v.is<float>())
  {
    out = v.as<float>();
    return !isnan(out);
  }
  if (v.is<const char *>())
  {
    char *end = nullptr;
    const char *str = v.as<const char *>();
    float f = strtof(str, &end);
    if (end != str && !isnan(f))
    {
      out = f;
      return true;
    }
  }
  return false;
}

Sensor *findOrCreateSensor(const char *name)
{
  Sensor *free = nullptr, *oldest = nullptr;
  for (Sensor &s : sensors)
  {
    if (!s.used)
    {
      if (!free)
        free = &s;
      continue;
    }
    if (strcasecmp(s.name, name) == 0)
      return &s;
    if (!oldest || s.lastUpdate < oldest->lastUpdate)
      oldest = &s;
  }
  Sensor *s = free ? free : oldest; // table full: evict the least recently updated
  *s = Sensor();
  strlcpy(s->name, name, NAME_LEN);
  s->used = true;
  return s;
}

// An empty payload on <topicBase>/<key> removes the sensor from the rotation.
void removeSensorByKey(const String &key)
{
  for (Sensor &s : sensors)
  {
    if (s.used && (strcasecmp(s.key, key.c_str()) == 0 || strcasecmp(s.name, key.c_str()) == 0))
    {
      s = Sensor();
      displayDirty = true;
    }
  }
}

void handleSensorMessage(const String &topic, const String &body)
{
  String key = topic.substring(topic.lastIndexOf('/') + 1);
  if (body.length() == 0)
  {
    removeSensorByKey(key);
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, body) || !doc.is<JsonObject>())
    return;
  JsonObjectConst o = doc.as<JsonObjectConst>();
  const char *name = o["sensor"] | (o["name"] | "");
  if (!name || !*name)
    return;

  float sg, temp, trend;
  bool hasSg = readNumber(o, "sg", "SG", sg);
  bool hasTemp = readNumber(o, "temperature", "temp", temp);
  bool hasTrend = readNumber(o, "trend", "Trend", trend);
  if (!hasSg && !hasTemp && !hasTrend)
    return;

  Sensor *s = findOrCreateSensor(name);
  strlcpy(s->key, key.c_str(), NAME_LEN);
  if (hasSg)
    s->sg = roundf(sg * 1000.0f) / 1000.0f;
  if (hasTemp)
    s->temp = roundf(temp * 10.0f) / 10.0f;
  if (hasTrend)
    s->trend = roundf(trend * 10000.0f) / 10000.0f;
  s->lastUpdate = millis();
}

void mqttCallback(char *topic, byte *payload, unsigned int length)
{
  String t(topic);
  String body;
  body.reserve(length);
  for (unsigned int i = 0; i < length; i++)
    body += (char)payload[i];

  if (t == baseTopic() + "/config/set")
  {
    JsonDocument doc;
    if (!deserializeJson(doc, body) && doc.is<JsonObject>())
    {
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

void connectMQTT()
{
  if (cfg.mqttHost.isEmpty() || WiFi.status() != WL_CONNECTED || mqtt.connected())
    return;
  if (millis() - lastMqttAttempt < MQTT_RECONNECT_INTERVAL)
    return;
  lastMqttAttempt = millis();

  mqtt.setServer(cfg.mqttHost.c_str(), cfg.mqttPort);
  String status = baseTopic() + "/status";
  String clientId = cfg.deviceName + "-" + String(ESP.getChipId(), HEX);
  bool ok = cfg.mqttUser.length()
                ? mqtt.connect(clientId.c_str(), cfg.mqttUser.c_str(), cfg.mqttPass.c_str(), status.c_str(), 0, true, "offline")
                : mqtt.connect(clientId.c_str(), nullptr, nullptr, status.c_str(), 0, true, "offline");
  if (ok)
  {
    Serial.println("[MQTT] connected");
    mqtt.publish(status.c_str(), "online", true);
    subscribeTopics();
    publishConfigState();
  }
  else
  {
    Serial.printf("[MQTT] failed rc=%d\n", mqtt.state());
  }
}

// ============ Wi-Fi ============
void startAccessPoint()
{
  if (apActive)
    return;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID);
  apActive = true;
  displayDirty = true;
  Serial.printf("[WiFi] AP %s on %s\n", AP_SSID, WiFi.softAPIP().toString().c_str());
}

void setupWiFi()
{
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.hostname(cfg.deviceName);
  if (cfg.ssid.isEmpty())
  {
    startAccessPoint();
    return;
  }
  Serial.printf("[WiFi] connecting to '%s'\n", cfg.ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfg.ssid.c_str(), cfg.password.c_str());
  wifiStart = millis();
}

void handleWiFi()
{
  bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wifiWasConnected)
  {
    Serial.printf("[WiFi] connected, IP %s\n", WiFi.localIP().toString().c_str());
    MDNS.begin(cfg.deviceName);
    if (apActive)
    {
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      apActive = false;
    }
    displayDirty = true;
  }
  wifiWasConnected = connected;
  // No network after the timeout: fallback AP, STA reconnection keeps going.
  if (!connected && !apActive && millis() - wifiStart > WIFI_CONNECT_TIMEOUT)
  {
    Serial.printf("[WiFi] connection to '%s' failed (status %d)\n", cfg.ssid.c_str(), WiFi.status());
    startAccessPoint();
  }
  if (connected)
    MDNS.update();
}

// ============ Serveur Web ============
void sendJson(int code, const JsonDocument &doc)
{
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

// Sent in small chunks: the low-memory lwIP build has tiny TCP buffers.
void handleRoot()
{
  const size_t total = strlen_P(INDEX_HTML);
  server.setContentLength(total);
  server.send(200, "text/html", "");
  for (size_t off = 0; off < total; off += 512)
  {
    server.sendContent_P(INDEX_HTML + off, min((size_t)512, total - off));
    yield();
  }
}

void handleGetConfig()
{
  JsonDocument doc;
  configToJson(cfg, doc["config"].to<JsonObject>(), false);
  sendJson(200, doc);
}

void handlePostConfig()
{
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain")) || !doc.is<JsonObject>())
  {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid JSON\"}");
    return;
  }
  unsubscribeTopics();
  bool restart = applyConfig(cfg, doc.as<JsonObjectConst>());
  bool saved = saveConfig(cfg);
  Serial.printf("[Web] config POST: saved=%d restart=%d ssid='%s'\n", saved, restart, cfg.ssid.c_str());
  if (mqtt.connected())
    mqtt.disconnect(); // reconnect with the new broker/topics settings
  displayDirty = true;
  if (!saved)
  {
    server.send(500, "application/json", "{\"ok\":false,\"error\":\"Flash write failed\"}");
    return;
  }
  if (restart)
    restartAt = millis() + 1500;
  server.send(200, "application/json", restart ? "{\"ok\":true,\"restart\":true}" : "{\"ok\":true,\"restart\":false}");
}

void handleState()
{
  JsonDocument doc;
  doc["version"] = FIRMWARE_VERSION;
  doc["mqtt"] = mqtt.connected();
  doc["ip"] = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  doc["host"] = cfg.deviceName;
  doc["rssi"] = WiFi.RSSI();
  JsonArray arr = doc["sensors"].to<JsonArray>();
  for (const Sensor &s : sensors)
  {
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

void setupWebServer()
{
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/state", HTTP_GET, handleState);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

// ============ Setup / loop ============
void setup()
{
  Serial.begin(115200);
  delay(100);
  Serial.printf("\n[*] FermWatch v%s\n", FIRMWARE_VERSION);

  if (!LittleFS.begin())
  {
    LittleFS.format();
    LittleFS.begin();
  }
  loadConfig(cfg);
  Serial.printf("[Config] ssid='%s' mqttHost='%s'\n", cfg.ssid.c_str(), cfg.mqttHost.c_str());

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

void loop()
{
  server.handleClient();
  handleWiFi();
  connectMQTT();
  mqtt.loop();
  updateDisplay();

  static unsigned long lastPurge = 0;
  if (millis() - lastPurge > 30000UL)
  {
    lastPurge = millis();
    purgeExpired();
  }

  if (restartAt && millis() > restartAt)
  {
    delay(100);
    ESP.restart();
  }
}
