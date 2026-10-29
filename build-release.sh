#!/bin/bash
# Compile le firmware et produit releases/esp8266-fermwatch-vX.Y.Z-AAAAMMJJ.bin
set -e

VERSION=$(tr -d '[:space:]' < version.txt)
RELEASE_DIR="releases"
FIRMWARE_NAME="esp8266-fermwatch-v${VERSION}-$(date +%Y%m%d).bin"

# sed -i differe entre macOS et Linux
if [[ "$OSTYPE" == "darwin"* ]]; then
  sed -i '' "s/#define FIRMWARE_VERSION \"[^\"]*\"/#define FIRMWARE_VERSION \"${VERSION}\"/" src/main.cpp
else
  sed -i "s/#define FIRMWARE_VERSION \"[^\"]*\"/#define FIRMWARE_VERSION \"${VERSION}\"/" src/main.cpp
fi

pio run -e esp8266

mkdir -p "$RELEASE_DIR"
cp .pio/build/esp8266/firmware.bin "$RELEASE_DIR/$FIRMWARE_NAME"

echo "Firmware: $RELEASE_DIR/$FIRMWARE_NAME"
echo "SHA256:   $(shasum -a 256 "$RELEASE_DIR/$FIRMWARE_NAME" | awk '{print $1}')"
echo "Flash:    esptool.py -p /dev/cu.usbserial-XXXX write_flash 0x0 $RELEASE_DIR/$FIRMWARE_NAME"
