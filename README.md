# ESP32_ArtNetNode_v2
ESP32 based WiFi ArtNet V4 to DMX, RDM and LED Pixels

This is a fork of https://github.com/mtongnz/ESP8266_ArtNetNode_v2

To compile get the latest version of the Arduino environment: https://www.arduino.cc/en/Main/Software

Post install in the Arduino IDE to get a working ESP32 environment:

1. Add "https://dl.espressif.com/dl/package_esp32_index.json, http://arduino.esp8266.com/stable/package_esp8266com_index.json" to the "Additional Boards Manager URLs" text field in the main preferences.
2. Go to Tools -> Board: XXX -> Boards Manager and  search for ESP32. Install it. Version 1.0.2 is known to work.
3. Go to Tools -> Manage Libraries. Search for 'ArduinoJson' (without an underscore) and install version 5.13.5 (later versions do NOT work).
4. Go to Tools -> Boards: XXX and select "OLIMEX ESP32-PoE" in the ESP32 section.
5. Open ESP32_ArtNetNode/ArtNetNode/ArtNetNode.ino, compile and upload
