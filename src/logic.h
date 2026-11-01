#pragma once

// Hardware-independent logic (no Arduino types), so it can be unit tested on
// the host with `make test`.

#include <ArduinoJson.h>
#include <cmath>
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_SENSORS 16
#define NAME_LEN 24

struct Sensor {
  char name[NAME_LEN] = "";
  char key[NAME_LEN] =
      ""; // last topic segment, used to clear the sensor with an empty message
  float sg = NAN;
  float temp = NAN;
  float trend = NAN;
  uint32_t lastUpdate = 0; // millis() of the last message
  bool used = false;
};

// ============ Timeouts ============
// Unsigned subtraction keeps working when millis() wraps around.
static bool olderThanMinutes(uint32_t now, uint32_t last, uint16_t minutes) {
  return (uint32_t)(now - last) > (uint32_t)minutes * 60000UL;
}

static bool isStale(const Sensor &s, uint32_t now, uint16_t staleMinutes) {
  return olderThanMinutes(now, s.lastUpdate, staleMinutes);
}

static bool isExpired(const Sensor &s, uint32_t now, uint16_t expireMinutes) {
  return olderThanMinutes(now, s.lastUpdate, expireMinutes);
}

// ============ Units ============
static float convertTemp(float c, char unit) {
  if (unit == 'F')
    return c * 9.0f / 5.0f + 32.0f;
  if (unit == 'K')
    return c + 273.15f;
  if (unit == 'R')
    return (c + 273.15f) * 9.0f / 5.0f;
  return c;
}

// Kelvin has no degree sign; the others use "^" (degree in the display font).
static const char *tempSuffix(char unit) {
  if (unit == 'F')
    return "^F";
  if (unit == 'K')
    return "K";
  if (unit == 'R')
    return "^R";
  return "^C";
}

// Degrees Plato from specific gravity (standard polynomial).
static float sgToPlato(float sg) {
  return -616.868f + 1111.14f * sg - 630.272f * sg * sg +
         135.997f * sg * sg * sg;
}

// ============ Pages ============
struct PageOptions {
  bool showName = true;
  bool showSg = true;
  bool showTemp = true;
  bool showTrend = true;
  bool plato = false;  // gravity unit: Plato instead of SG
  char tempUnit = 'C'; // C, F, K, R
  uint16_t staleMinutes = 30;
};

static void formatPage(char *dst, size_t n, const char *flag,
                       const char *prefix, float v, int decimals, bool sign,
                       const char *suffix) {
  snprintf(dst, n, sign ? "%s%s%+.*f%s" : "%s%s%.*f%s", flag, prefix, decimals,
           v, suffix);
}

// Walks every page (per sensor: name, SG, temperature, trend) in a stable
// order. Returns the page count and, if 0 <= target < count and out is not
// null, fills out/isName with that page.
static int collectPages(const Sensor *sensors, size_t count,
                        const PageOptions &opt, uint32_t now, int target,
                        char *out, size_t outLen, bool &isName) {
  int n = 0;
  char text[64];
  auto commit = [&](bool name) {
    if (n == target && out) {
      snprintf(out, outLen, "%s", text);
      isName = name;
    }
    n++;
  };
  for (size_t i = 0; i < count; i++) {
    const Sensor &s = sensors[i];
    if (!s.used)
      continue;
    const char *flag = isStale(s, now, opt.staleMinutes) ? "!" : "";
    if (opt.showName) {
      size_t j = 0;
      for (; s.name[j] && j < sizeof(text) - 1; j++)
        text[j] = (char)toupper((unsigned char)s.name[j]);
      text[j] = '\0';
      commit(true);
    }
    if (opt.showSg && !std::isnan(s.sg)) {
      if (opt.plato)
        formatPage(text, sizeof(text), flag, "", sgToPlato(s.sg), 1, false,
                   "^P");
      else
        formatPage(text, sizeof(text), flag, "SG ", s.sg, 3, false, "");
      commit(false);
    }
    if (opt.showTemp && !std::isnan(s.temp)) {
      formatPage(text, sizeof(text), flag, "T ",
                 convertTemp(s.temp, opt.tempUnit), 1, false,
                 tempSuffix(opt.tempUnit));
      commit(false);
    }
    if (opt.showTrend && !std::isnan(s.trend)) {
      // The Plato trend is the Plato difference over 24 h, so it needs the
      // current SG.
      if (!opt.plato) {
        formatPage(text, sizeof(text), flag, "", s.trend, 4, true, "/d");
        commit(false);
      } else if (!std::isnan(s.sg)) {
        formatPage(text, sizeof(text), flag, "",
                   sgToPlato(s.sg) - sgToPlato(s.sg - s.trend), 1, true,
                   "^P/d");
        commit(false);
      }
    }
  }
  return n;
}

// ============ MQTT sensor messages ============
// Reads a number from a JSON field, accepting a number or a numeric string.
// "unknown"/"unavailable" and missing fields leave the stored value untouched.
static bool readNumber(JsonObjectConst o, const char *lower, const char *upper,
                       float &out) {
  JsonVariantConst v = o[lower];
  if (v.isNull())
    v = o[upper];
  if (v.is<float>()) {
    out = v.as<float>();
    return !std::isnan(out);
  }
  if (v.is<const char *>()) {
    char *end = nullptr;
    const char *str = v.as<const char *>();
    float f = strtof(str, &end);
    if (end != str && !std::isnan(f)) {
      out = f;
      return true;
    }
  }
  return false;
}

static Sensor *findOrCreateSensor(Sensor *sensors, size_t count,
                                  const char *name) {
  Sensor *free = nullptr, *oldest = nullptr;
  for (size_t i = 0; i < count; i++) {
    Sensor &s = sensors[i];
    if (!s.used) {
      if (!free)
        free = &s;
      continue;
    }
    if (strcasecmp(s.name, name) == 0)
      return &s;
    if (!oldest || s.lastUpdate < oldest->lastUpdate)
      oldest = &s;
  }
  Sensor *s =
      free ? free : oldest; // table full: evict the least recently updated
  *s = Sensor();
  snprintf(s->name, NAME_LEN, "%s", name);
  s->used = true;
  return s;
}

// An empty payload on <topicBase>/<key> removes the sensor from the rotation.
static bool removeSensorByKey(Sensor *sensors, size_t count, const char *key) {
  bool removed = false;
  for (size_t i = 0; i < count; i++) {
    Sensor &s = sensors[i];
    if (s.used &&
        (strcasecmp(s.key, key) == 0 || strcasecmp(s.name, key) == 0)) {
      s = Sensor();
      removed = true;
    }
  }
  return removed;
}

enum SensorMessageResult { MSG_IGNORED, MSG_UPDATED, MSG_REMOVED };

// Applies one message received on a sensor topic to the table.
static SensorMessageResult applySensorMessage(Sensor *sensors, size_t count,
                                              const char *topic,
                                              const char *body, uint32_t now) {
  const char *slash = strrchr(topic, '/');
  const char *key = slash ? slash + 1 : topic;
  if (body[0] == '\0')
    return removeSensorByKey(sensors, count, key) ? MSG_REMOVED : MSG_IGNORED;

  JsonDocument doc;
  if (deserializeJson(doc, body) || !doc.is<JsonObject>())
    return MSG_IGNORED;
  JsonObjectConst o = doc.as<JsonObjectConst>();
  const char *name = o["sensor"] | (o["name"] | "");
  if (!name || !*name)
    return MSG_IGNORED;

  float sg, temp, trend;
  bool hasSg = readNumber(o, "sg", "SG", sg);
  bool hasTemp = readNumber(o, "temperature", "temp", temp);
  bool hasTrend = readNumber(o, "trend", "Trend", trend);
  if (!hasSg && !hasTemp && !hasTrend)
    return MSG_IGNORED;

  Sensor *s = findOrCreateSensor(sensors, count, name);
  snprintf(s->key, NAME_LEN, "%s", key);
  if (hasSg)
    s->sg = roundf(sg * 1000.0f) / 1000.0f;
  if (hasTemp)
    s->temp = roundf(temp * 10.0f) / 10.0f;
  if (hasTrend)
    s->trend = roundf(trend * 10000.0f) / 10000.0f;
  s->lastUpdate = now;
  return MSG_UPDATED;
}
