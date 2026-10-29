#pragma once

#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

#define CONFIG_PATH "/config.json"

struct Config
{
  String ssid;
  String password;
  String mqttHost;
  uint16_t mqttPort = 1883;
  String mqttUser;
  String mqttPass;
  String deviceName = "fermwatch";

  // One topic per sensor: <topicBase>/<anything>, the sensor name is in the JSON payload.
  String topicBase = "fermwatch";

  bool showName = true;
  bool showSg = true;
  bool showTemp = true;
  bool showTrend = true;

  uint16_t pageSeconds = 5;
  uint16_t scrollMs = 35;
  // Incoming data is always degrees Celsius and SG; units only affect the display.
  String tempUnit = "C"; // C, F, K, R
  String gravUnit = "SG"; // SG, P
  uint8_t brightness = 3;
  uint16_t staleMinutes = 30;
  uint16_t expireMinutes = 1440;
};

static void copyString(JsonObjectConst o, const char *key, String &dst)
{
  if (o[key].is<const char *>())
    dst = o[key].as<String>();
}

static void copySecret(JsonObjectConst o, const char *key, String &dst)
{
  // An empty value means "unchanged".
  if (o[key].is<const char *>() && strlen(o[key].as<const char *>()) > 0)
    dst = o[key].as<String>();
}

static void copyBool(JsonObjectConst o, const char *key, bool &dst)
{
  if (o[key].is<bool>())
    dst = o[key].as<bool>();
}

template <typename T>
static void copyClamped(JsonObjectConst o, const char *key, T &dst, long lo, long hi)
{
  if (o[key].is<long>())
    dst = (T)constrain(o[key].as<long>(), lo, hi);
}

// Applies a partial JSON config. Returns true if Wi-Fi or device name changed (restart required).
static bool applyConfig(Config &c, JsonObjectConst o)
{
  String oldSsid = c.ssid, oldPass = c.password, oldName = c.deviceName;

  copyString(o, "ssid", c.ssid);
  copySecret(o, "password", c.password);
  copyString(o, "mqttHost", c.mqttHost);
  copyClamped(o, "mqttPort", c.mqttPort, 1, 65535);
  copyString(o, "mqttUser", c.mqttUser);
  copySecret(o, "mqttPass", c.mqttPass);
  copyString(o, "deviceName", c.deviceName);

  copyString(o, "topicBase", c.topicBase);

  copyBool(o, "showName", c.showName);
  copyBool(o, "showSg", c.showSg);
  copyBool(o, "showTemp", c.showTemp);
  copyBool(o, "showTrend", c.showTrend);

  copyClamped(o, "pageSeconds", c.pageSeconds, 1, 60);
  copyClamped(o, "scrollMs", c.scrollMs, 10, 100);
  copyString(o, "tempUnit", c.tempUnit);
  copyString(o, "gravUnit", c.gravUnit);
  c.tempUnit.toUpperCase();
  c.gravUnit.toUpperCase();
  if (c.tempUnit != "C" && c.tempUnit != "F" && c.tempUnit != "K" && c.tempUnit != "R")
    c.tempUnit = "C";
  if (c.gravUnit != "SG" && c.gravUnit != "P")
    c.gravUnit = "SG";
  copyClamped(o, "brightness", c.brightness, 0, 15);
  copyClamped(o, "staleMinutes", c.staleMinutes, 1, 1440);
  copyClamped(o, "expireMinutes", c.expireMinutes, 1, 10080);

  c.topicBase.trim();
  if (c.topicBase.isEmpty())
    c.topicBase = "fermwatch";
  c.deviceName.trim();
  if (c.deviceName.isEmpty())
    c.deviceName = "fermwatch";

  return c.ssid != oldSsid || c.password != oldPass || c.deviceName != oldName;
}

static void configToJson(const Config &c, JsonObject o, bool includeSecrets)
{
  o["ssid"] = c.ssid;
  o["mqttHost"] = c.mqttHost;
  o["mqttPort"] = c.mqttPort;
  o["mqttUser"] = c.mqttUser;
  o["deviceName"] = c.deviceName;
  o["topicBase"] = c.topicBase;
  o["showName"] = c.showName;
  o["showSg"] = c.showSg;
  o["showTemp"] = c.showTemp;
  o["showTrend"] = c.showTrend;
  o["pageSeconds"] = c.pageSeconds;
  o["scrollMs"] = c.scrollMs;
  o["tempUnit"] = c.tempUnit;
  o["gravUnit"] = c.gravUnit;
  o["brightness"] = c.brightness;
  o["staleMinutes"] = c.staleMinutes;
  o["expireMinutes"] = c.expireMinutes;
  if (includeSecrets)
  {
    o["password"] = c.password;
    o["mqttPass"] = c.mqttPass;
  }
}

static void loadConfig(Config &c)
{
  File f = LittleFS.open(CONFIG_PATH, "r");
  if (!f)
    return;
  JsonDocument doc;
  if (!deserializeJson(doc, f))
    applyConfig(c, doc.as<JsonObjectConst>());
  f.close();
}

static bool saveConfig(const Config &c)
{
  File f = LittleFS.open(CONFIG_PATH, "w");
  if (!f)
    return false;
  JsonDocument doc;
  configToJson(c, doc.to<JsonObject>(), true);
  bool ok = serializeJson(doc, f) > 0;
  f.close();
  return ok;
}
