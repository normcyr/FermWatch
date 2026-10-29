.PHONY: help build upload monitor clean erase-flash format format-fix

help:
	@echo "FermWatch"
	@echo ""
	@echo "build         - Compile the firmware"
	@echo "upload        - Upload to the ESP8266"
	@echo "monitor       - Serial monitor (115200)"
	@echo "format        - Check formatting (clang-format)"
	@echo "format-fix    - Fix formatting"
	@echo "clean         - Remove build files"
	@echo "erase-flash   - Erase the ESP8266 flash"

build:
	pio run -e esp8266

upload:
	pio run -e esp8266 -t upload

monitor:
	pio device monitor -b 115200

format:
	clang-format --dry-run -Werror src/*.cpp src/*.h

format-fix:
	clang-format -i src/*.cpp src/*.h

clean:
	pio run -t clean

erase-flash:
	esptool.py --port $$(ls /dev/cu.usbserial-* /dev/cu.wchusbserial* 2>/dev/null | head -1) erase_flash

.DEFAULT_GOAL := help
