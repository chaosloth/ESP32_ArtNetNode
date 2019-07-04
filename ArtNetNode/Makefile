# My makefile
SKETCH = ArtNetNode.ino
CHIP=esp32
ESP_ROOT=/home/turo/Arduino/hardware/espressif/esp32

LIBS=/home/turo/Arduino/libraries/ArduinoJson/src \
	$(ESP_LIBS)/SPI \
	$(ESP_LIBS)/WiFi \
	$(ESP_LIBS)/WebServer \
	$(ESP_LIBS)/EEPROM \
	$(ESP_LIBS)/FS \
	$(ESP_LIBS)/SPIFFS \
	$(ESP_LIBS)/Update

UPLOAD_PORT = /dev/ttyUSB0
UPLOAD_SPEED = 3000000
BOARD = esp32-poe

include $(HOME)/makeEspArduino/makeEspArduino.mk
