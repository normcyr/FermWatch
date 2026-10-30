# FermWatch

Fermentation display (ESP8266 + MAX7219 8×32): specific gravity, temperature and 24 h gravity trend for up to 16 sensors (Tilt, RAPT Pill, ...), received over MQTT from Home Assistant as one JSON message per sensor, with a built-in web page for configuration.

Source code: <https://github.com/normcyr/FermWatch>

Inspired by [ESP_DispSpotTrack](https://github.com/normcyr/ESP_DispSpotTrack) (same hardware) and [ESPTimeCast](https://github.com/mfactory-osaka/ESPTimeCast).

The display cycles through all sensors. For each sensor it shows one page per enabled value, in a narrow 3×5 font so the values fit in 32 columns:

| Page | Example |
| ------ | --------- |
| Sensor name: full-height standard font if it fits in 32 px, otherwise narrow font and scrolling | `RAPT`, `TILT YELLOW` |
| Specific gravity (or Plato) | `SG 1.012`, `3.1°P` |
| Temperature (°C, °F, K or °R) | `T 20.4°C`, `T 68.7°F`, `T 293.6K`, `T 528.4°R` |
| 24 h gravity trend (per day) | `-0.0042/d`, `-1.0°P/d` |

## Hardware

- ESP8266 Wemos D1 Mini
- MAX7219 8×32 LED matrix (4× 8×8, FC16), 5 V USB power

```
GPIO14 (D5) → CLK
GPIO13 (D7) → CS
GPIO15 (D8) → DIN
GND → GND, 5V → VCC
```

**Enclosure:** like [ESPTimeCast](https://github.com/mfactory-osaka/ESPTimeCast), you can 3D print a case that holds the ESP8266 and the LED matrix together, to get a compact desktop display. Check the ESPTimeCast repository for printable case designs that fit this same hardware.

## Build and flash

Requires [PlatformIO](https://platformio.org/).

```bash
make build      # compile
make upload     # flash over USB (close the serial monitor first)
make monitor    # serial monitor, 115200 baud
```

If the upload fails with a connection error, retry once. If the serial port is busy, stop any running monitor.

GitHub Actions builds the firmware on every push and pull request.

## First setup

1. Power the device. It first scrolls `FermWatch v<version>` once. Until a Web password is set, it opens the open `FermWatch-Setup` access point and shows `AP 192.168.4.1`; provision it only where nearby access is controlled.
2. Join that network from a phone or another computer and open `http://192.168.4.1`.
3. Under **Advanced settings**, set a Web password (8-64 characters), enter the Wi-Fi SSID/password and MQTT broker (host, port, user, password), then **Save**. The device restarts and joins your network.
4. Open `http://<device IP>` (or `http://fermwatch.local`). When prompted, use username `admin` and the Web password. The IP is printed on the serial monitor. A DHCP reservation in your router keeps it stable.
5. Choose the values to display, page duration, scroll speed and brightness, then **Save**. Sensors appear automatically when their first MQTT message arrives.

If the Wi-Fi is unavailable for more than 20 s, the access point comes back so the device stays reachable. Existing devices without a Web password also start in setup AP mode after upgrading; set the password once to resume normal Wi-Fi operation.

## Web interface

- **Sensors**: the list of sensors seen so far, with their last values and age. There is nothing to select: sensors identify themselves in their MQTT message.
- **Information to display**: sensor name, SG, temperature, 24 h trend (applies to all sensors).
- **Units**: temperature in °C, °F, K or °R; gravity in SG or °P (Plato). This only affects the display and the preview: MQTT data is always °C and SG.
- **Page duration**, **scroll speed** (used if a text is wider than the display) and **brightness**.
- **Display preview**: cycles through the live pages of all sensors, or sample values when none was received.
- **Advanced settings**: sensor topic base, stale/removal timeouts, MQTT broker, Wi-Fi, device name.
- The Web interface uses HTTP Digest authentication (`admin` plus the Web password). Passwords are never returned by the API or published over MQTT.
- Password fields left empty keep the stored value.
- The device serves plain HTTP, not HTTPS. Use it only on a trusted local network and do not expose it directly to the Internet; HTTP traffic, including configuration values, is not encrypted.

## MQTT

### Sensor messages

Publish one JSON message per sensor on `fermwatch/<anything>` (the device subscribes to `fermwatch/+`; the topic base is configurable). The sensor is identified by the `sensor` field of the payload, not by the topic, so use any topic name that is unique per sensor, such as `fermwatch/tilt_yellow`.

```json
{"sensor": "Tilt Yellow", "temperature": 20.4, "sg": 1.012, "trend": -0.004}
```

| Field | Meaning |
| ----- | ------- |
| `sensor` (or `name`) | required; sensor name shown on the display (max 23 characters), matched case-insensitively |
| `sg` (or `SG`) | specific gravity |
| `temperature` (or `temp`) | °C |
| `trend` | gravity change over 24 h, shown as `/d` |

- Always send °C and SG, whatever the display units.
- Every value field is optional. A message only updates the fields it contains, and pages for missing values are skipped.
- Values can be numbers or numeric strings. `unknown` / `unavailable` are ignored, so the last valid value stays.
- Publish with the **retain** flag so the display recovers its sensors after a restart.
- Up to 16 sensors. When the table is full, the sensor with the oldest message is replaced.
- Sensors that send nothing for `staleMinutes` (default 30) are shown with a `!` prefix. After `expireMinutes` (default 24 h) they are removed from the rotation.

### Device topics

| Topic | Direction | Payload |
| ----- | --------- | ------- |
| `fermwatch/<deviceName>/config/set` | HA → ESP | partial JSON configuration |
| `fermwatch/<deviceName>/config` | ESP → HA | current configuration (retained, no passwords) |
| `fermwatch/<deviceName>/status` | ESP → HA | `online` / `offline` (last will, retained) |

### Testing with mosquitto_pub

Keep the credentials in environment variables so they do not end up in files or shell history:

```bash
export MQTT_HOST=homeassistant.local
export MQTT_USER=your_user
export MQTT_PASS='your_password'
alias pub='mosquitto_pub -h "$MQTT_HOST" -u "$MQTT_USER" -P "$MQTT_PASS"'
```

Send test sensors (retained):

```bash
pub -r -t fermwatch/tilt_yellow -m '{"sensor":"Tilt Yellow","temperature":20.4,"sg":1.012,"trend":-0.004}'
pub -r -t fermwatch/rapt_blue -m '{"sensor":"RAPT","temperature":18.9,"sg":1.048,"trend":-0.002}'
```

To remove a sensor from the rotation, publish an **empty retained message** on its topic: `pub -r -n -t fermwatch/rapt_blue`. The display drops it immediately, and the broker forgets the retained message so it does not come back after a restart. (The topic's last segment is matched against the one the sensor used, or its name.) Silent sensors are also dropped automatically after `expireMinutes`.

Watch what the device publishes:

```bash
mosquitto_sub -h "$MQTT_HOST" -u "$MQTT_USER" -P "$MQTT_PASS" -t 'fermwatch/#' -v
```

### Changing the configuration over MQTT

Publish a partial JSON on `fermwatch/<deviceName>/config/set` (default name: `fermwatch`). Only the keys you send change; the result is saved and echoed on `.../config`.

```bash
pub -t fermwatch/fermwatch/config/set -m '{"pageSeconds":8}'
pub -t fermwatch/fermwatch/config/set -m '{"showTrend":false}'
pub -t fermwatch/fermwatch/config/set -m '{"tempUnit":"F","gravUnit":"P"}'
pub -t fermwatch/fermwatch/config/set -m '{"staleMinutes":60,"expireMinutes":2880}'
pub -t fermwatch/fermwatch/config/set -m '{"brightness":8,"scrollMs":50}'
```

| Key | Type | Range / values | Default |
| ----- | ------ | ---------------- | --------- |
| `topicBase` | string | the device subscribes to `<topicBase>/+` | `fermwatch` |
| `showName` | bool | `true` / `false` | `true` |
| `showSg`, `showTemp`, `showTrend` | bool | `true` / `false` | `true` |
| `tempUnit` | string | `C`, `F`, `K`, `R` | `C` |
| `gravUnit` | string | `SG`, `P` | `SG` |
| `pageSeconds` | int | 1-60 | 5 |
| `scrollMs` | int | 10-100 (ms per pixel) | 35 |
| `brightness` | int | 0-15 | 3 |
| `staleMinutes` | int | 1-1440 (`!` prefix) | 30 |
| `expireMinutes` | int | 1-10080 (sensor removed from rotation) | 1440 |
| `mqttHost`, `mqttPort`, `mqttUser`, `mqttPass` | string / int | broker settings | empty / 1883 |
| `ssid`, `password`, `deviceName` | string | changing any of these restarts the device | `fermwatch` |

`webPassword` is accepted only by the HTTP configuration endpoint, is never returned, and is not configurable over MQTT. The first time it is set, the device restarts so it can leave setup AP mode.

## HTTP API

| Method | Path | Description |
| -------- | ------ | ------------- |
| GET | `/` | configuration page |
| GET | `/api/config` | configuration (without passwords) |
| POST | `/api/config` | partial JSON, same keys as above; accepts `webPassword` (8-64 characters) to set/change the Web password; returns `{"ok":true,"restart":false}` |
| GET | `/api/state` | all sensors with values and age, MQTT state, IP, RSSI, firmware version |

```bash
curl http://fermwatch.local/api/state
curl -X POST http://fermwatch.local/api/config -H 'Content-Type: application/json' -d '{"pageSeconds":8}'
```

## Home Assistant

Home Assistant stays the source of truth: it already holds the data of your Tilt, RAPT Pill or any other sensor (as entities), and an automation pushes that data to the display over MQTT. The ESP8266 only displays what it receives, so nothing sensor-specific is needed in the firmware. Any Home Assistant entity can be sent, whatever integration provides it.

```
Sensor (Tilt, RAPT, ...) → Home Assistant entities → automation → MQTT → ESP8266 display
```

The firmware expects one retained JSON message per sensor, always in °C and SG (see [Sensor messages](#sensor-messages)). To show another sensor, copy the automation and change the topic, the name and the entities. Use one topic per sensor, otherwise a sensor overwrites the retained message of another.

### 24 h trend sensor

The trend is computed in Home Assistant, not on the ESP, with the **Statistics** helper.

**Using the UI (recommended):** go to **Settings → Devices & services → Helpers → Create helper → Statistics**, then set:

- **Entity**: `sensor.tilt_yellow_specific_gravity`
- **Statistic characteristic**: `Change` (`change`)
- **Max age**: 24 hours
- **Maximum number of stored samples**: 500
- **Precision**: 3 or 4

Name it `Tilt Yellow SG trend 24h`. The entity id is then `sensor.tilt_yellow_sg_trend_24h`, as used in the automation below.

**Using YAML (equivalent):** add to `configuration.yaml`, then restart Home Assistant or reload the entities:

```yaml
sensor:
  - platform: statistics
    name: Tilt Yellow SG trend 24h
    unique_id: tilt_yellow_sg_trend_24h
    entity_id: sensor.tilt_yellow_specific_gravity
    state_characteristic: change
    max_age:
      hours: 24
    sampling_size: 500
    precision: 4
```

`change` is the difference between the newest and the oldest value of the last 24 h, so the trend is only complete after 24 h of data. Check the exact entity id that Home Assistant created.

### Blueprint (recommended)

[![Import blueprint](https://my.home-assistant.io/badges/blueprint_import.svg)](https://my.home-assistant.io/redirect/blueprint_import/?blueprint_url=https%3A%2F%2Fgithub.com%2Fnormcyr%2FFermWatch%2Fblob%2Fmain%2Fhomeassistant%2Fblueprints%2Ffermwatch_sensor.yaml)

[homeassistant/blueprints/fermwatch_sensor.yaml](homeassistant/blueprints/fermwatch_sensor.yaml) replaces both automations below. Import it, then go to **Settings → Automations & scenes → Create automation → FermWatch** once per sensor and fill in:

- the sensor name shown on the display and a unique MQTT topic (such as `fermwatch/tilt_yellow`);
- the gravity entity, and optionally the temperature and the 24 h trend entities (select at most one each);
- whether to remove the sensor from the display when its gravity entity stays `unavailable` or `unknown`, or stops reporting while keeping its last value (dead battery), and after how long.

It publishes the same retained JSON message as the manual automation and only includes the fields you selected. It requires Home Assistant 2024.10 or later.

### Manual automation

If you prefer not to use the blueprint, adapt the names and entities:

```yaml
alias: Send fermentation data to the ESP8266 display
description: ''
triggers:
  - trigger: state
    entity_id:
      - sensor.tilt_yellow_temperature
      - sensor.tilt_yellow_specific_gravity
      - sensor.tilt_yellow_sg_trend_24h
conditions: []
actions:
  - action: mqtt.publish
    data:
      topic: fermwatch/tilt_yellow
      retain: true
      payload: >-
        {"sensor": "Tilt Yellow",
         "temperature": "{{ states('sensor.tilt_yellow_temperature') }}",
         "sg": "{{ states('sensor.tilt_yellow_specific_gravity') }}",
         "trend": "{{ states('sensor.tilt_yellow_sg_trend_24h') }}"}
mode: queued
```

Notes:

- **Trigger on the sensors themselves** so the display updates when a value changes.
- **`retain: true`** lets the display recover its sensors after a restart.
- **`mode: queued`** keeps messages from being lost when several entities change at almost the same time.
- If the entity does not provide a value (for example no trend yet), you can omit that field: every field except `sensor` is optional.

### Stop displaying a silent sensor

If a sensor stops reporting (dead battery, out of range), Home Assistant can clear it from the display after a while by sending an empty message. The blueprint does this when enabled. Without it, this second automation does that when the gravity entity has not changed for 1 hour (note that `unavailable` also counts as a change, so adjust `for` to taste):

```yaml
alias: Clear Tilt Yellow from the ESP8266 display
description: ''
triggers:
  - trigger: state
    entity_id: sensor.tilt_yellow_specific_gravity
    to:
      - unavailable
      - unknown
    for: "01:00:00"
  - trigger: template
    value_template: >-
      {{ (now() - states.sensor.tilt_yellow_specific_gravity.last_reported).total_seconds() > 3600 }}
conditions: []
actions:
  - action: mqtt.publish
    data:
      topic: fermwatch/tilt_yellow
      retain: true
      payload: ""
mode: single
```

The next normal message brings the sensor back. The device-side `expireMinutes` setting does the same without Home Assistant, but only works while the device is running.

To check the result, use **Settings → Devices & services → MQTT → Configure → Listen to a topic** on `fermwatch/#`, or `mosquitto_sub -h "$MQTT_HOST" -u "$MQTT_USER" -P "$MQTT_PASS" -t 'fermwatch/#' -v`. The sensor then appears in the **Sensors** section of the device's web page.

## Troubleshooting

| Symptom | Check |
| --------- | ------- |
| Display shows `AP 192.168.4.1` | No Wi-Fi configured or connection failed; configure it through the access point |
| Display shows `--` | No sensor message received yet, or all of them removed after `expireMinutes` |
| A sensor does not appear | Payload must be JSON with a `sensor` name and at least one of `sg`, `temperature`, `trend`; check the topic matches `<topicBase>/+` (one level) |
| Value prefixed with `!` | Sensor silent for longer than `staleMinutes` |
| Badge says "MQTT disconnected" | Broker host/credentials; serial log `[MQTT] failed rc=-2` (unreachable) or `rc=5` (refused) |
| `homeassistant.local` not found by the ESP | Use the broker's IP address instead of an mDNS name |
| Wi-Fi never connects | The ESP8266 only supports 2.4 GHz |
| Upload fails | Close the serial monitor, replug the board, retry |

See [.github/copilot-instructions.md](.github/copilot-instructions.md) for the architecture and code guidelines.

## Roadmap

Planned, in this order:

1. **OTA updates**: flash new firmware over Wi-Fi, protected by the Web password, so the USB cable is no longer needed.
2. **Automated tests**: unit tests for the pure logic (SG/Plato conversion, message parsing, expiry), run in CI.
3. **MQTT Discovery**: the device announces itself to Home Assistant, with its status and display settings as entities.

## License

[MIT](LICENSE)
