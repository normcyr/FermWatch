### Update an existing device

Open the device web page, expand **Firmware update**, choose the attached `.bin` file and upload it (needs the Web password; the configuration is kept).

### Flash over USB

Requires `pip install esptool` and the device connected via USB.

```bash
esptool.py -p /dev/cu.usbserial-XXXX write_flash 0x0 @FIRMWARE@
```

On Windows, use a port such as `COM3`. Replace the port with your actual serial port.

### First setup

1. The device starts its Wi-Fi access point `FermWatch-Setup`.
2. Connect to it and open `http://192.168.4.1`.
3. Under Advanced settings, set a Web password (8-64 characters) and enter the Wi-Fi and MQTT broker details.
4. Save; the device restarts and joins your network.

### Documentation

See the [README](https://github.com/@REPO@/blob/main/README.md) for the full setup and configuration.
