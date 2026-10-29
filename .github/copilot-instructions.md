# Copilot Instructions - FermWatch

## Project Overview

MAX7219 8×32 display driven by an ESP8266 (D1 Mini). It shows specific gravity (SG), temperature and the 24 h gravity trend of a fermentation, received over MQTT from Home Assistant. An embedded web page configures the display.

**Tech**: Arduino C++ (PlatformIO), MAX7219, MQTT, WiFi, LittleFS + ArduinoJson

Inspired by ESP_DispSpotTrack (same hardware) and ESPTimeCast (https://github.com/mfactory-osaka/ESPTimeCast).

## Architecture

- **Home Assistant** is the source of truth. It publishes SG, temperature and the 24 h trend (computed in HA, not on the ESP) over MQTT.
- **ESP8266** is a display device: MQTT client, web server, page handling.
- The firmware has no Tilt/RAPT-specific logic. Each sensor publishes one retained JSON message `{"sensor","sg","temperature","trend"}` on `<topicBase>/<x>`; the ESP keeps a table of up to 16 sensors (`Sensor` in `src/main.cpp`) keyed by the name in the payload.

## Hardware

- ESP8266 Wemos D1 Mini, MAX7219 8×32 (4× 8×8, FC16), 5 V USB power

```
GPIO14 (D5) → CLK
GPIO13 (D7) → CS
GPIO15 (D8) → DIN
GND → GND, 5V → VCC
```

## Code Structure

- `src/main.cpp`: Wi-Fi (STA + fallback AP `FermWatch-Setup`), MQTT, display pages, web server
- `src/config.h`: `Config` struct, JSON ↔ config, persistence in `/config.json` (LittleFS)
- `src/web_ui.h`: HTML/JS/CSS page in PROGMEM (English, ASCII only)
- `platformio.ini`: `esp8266` environment

## MQTT Topics

| Topic | Direction | Content |
|-------|-----------|---------|
| `<topicBase>/+` (default `fermwatch/+`) | HA → ESP | JSON per sensor: `sensor` (required), `sg`, `temperature`, `trend` (all optional, numbers or numeric strings) |
| `fermwatch/<deviceName>/config/set` | HA → ESP | partial JSON config |
| `fermwatch/<deviceName>/config` | ESP → HA | current config (retained, no secrets) |
| `fermwatch/<deviceName>/status` | ESP → HA | `online` / `offline` (LWT, retained) |

Partial messages merge into the sensor's stored values. `unknown`/`unavailable` are ignored. Values are rounded on receipt (SG to 3 decimals, trend to 4, temperature to 1). An **empty payload** on `<topicBase>/<key>` removes the sensor immediately; `<key>` is the last topic segment, matched against the key the sensor last used or its name (`Sensor.key`). Publish it retained (`mosquitto_pub -r -n`) to also clear the broker's copy. HA can send it when a sensor goes silent (see README). Sensors silent for `staleMinutes` get a `!` prefix; after `expireMinutes` they leave the rotation. When the table is full the least recently updated sensor is evicted.

## Web API

- `GET /`: configuration page
- `GET /api/config`: config (no passwords)
- `POST /api/config`: partial JSON; empty password = unchanged; restart if Wi-Fi/name changes
- `GET /api/state`: sensors with values/age, MQTT state, IP, RSSI

## Display Behavior

- For each sensor in turn, one page per enabled field (missing values are skipped), starting with the sensor name (full-height standard font if it fits in 32 px; otherwise narrow font and scrolling): `TILT YELLOW`, `SG 1.012`, `T 20.4°C`, `-0.0042/d` (trend per day)
- Text that fits 32 px is static for `pageSeconds`; otherwise it scrolls at `scrollMs` ms/pixel
- Units (`tempUnit` C/F/K/R, `gravUnit` SG/P) only affect the display: incoming data is always °C and SG. The Plato trend is the Plato difference between SG and SG minus trend (needs `sg` in the message). `^` is the degree sign in the narrow font
- Data older than `staleMinutes`: last value kept, prefixed with `!`
- At boot: `FermWatch v<version>` scrolls once in the standard font on the 7 bottom rows (same as the sensor name)
- No sensor yet: `--`
- AP mode without Wi-Fi: shows `AP 192.168.4.1`

## Build & Upload

```bash
make build       # pio run -e esp8266
make upload
make monitor     # 115200 baud
```

## Quick Test

```bash
mosquitto_pub -h <broker> -r -t fermwatch/tilt_yellow \
  -m '{"sensor":"Tilt Yellow","temperature":20.4,"sg":1.012,"trend":-0.004}'
```

## Code Guidelines

- Arduino C++, 2-space indentation, clear style
- Comment only non-obvious logic
- All UI text, messages, comments and docs in English
- Keep web UI strings ASCII (use HTML entities); non-ASCII truncated the page once
- Never return passwords in APIs or MQTT topics
- Upload fails with "Timed out waiting for packet header" or `Invalid head of packet` when a serial monitor holds the port or by chance: close the monitor and retry
- Use a DHCP reservation for a stable address (`http://<deviceName>.local` also works)

## References

- [ESP8266 Arduino](https://github.com/esp8266/Arduino)
- [MD_MAX72XX](https://github.com/MajicDesigns/MD_MAX72XX) · [MD_Parola](https://github.com/MajicDesigns/MD_Parola)
- [PubSubClient](https://github.com/knolleary/pubsubclient) · [ArduinoJson](https://arduinojson.org/)
