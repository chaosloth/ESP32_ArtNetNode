/*
  ArtNetNode v3.0.0
  Copyright (c) 2018, Tinic Uro
  https://github.com/tinic/ESP32_ArtNetNode

  ESP8266_ArtNetNode v2.0.0
  Copyright (c) 2016, Matthew Tong
  https://github.com/mtongnz/ESP8266_ArtNetNode_v2

  This program is free software: you can redistribute it and/or modify it under the terms of the GNU General Public
  License as published by the Free Software Foundation, either version 3 of the License, or (at your option) any
  later version.

  This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
  warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
  You should have received a copy of the GNU General Public License along with this program.
  If not, see http://www.gnu.org/licenses/
*/


#ifndef serialLEDDriver_h
#define serialLEDDriver_h

#include <SPI.h>

#define LED_PORTS 2
#define PIX_MAX_BUFFER_SIZE 2048
#define SPI_LATCH_BITS 100

enum conf_type {
  // WS2812 use VSPI/HSPI pins only, port 0 and port 1 respectively.
  // On a ESP32 Wroom module that would be:
  // WS2812DATA Port 0 => VSPID => GPIO23 => Pin 37
  // WS2812DATA Port 1 => HSPID => GPIO13 => Pin 16
  WS2812_RGB,
  WS2812_RGBW,
  WS2812_RGBW_SPLIT,
  // APA102 use VSPI/HSPI pins only, port 0 and port 1 respectively.
  // On a ESP32 Wroom module that would be:
  // APA102 Data Port 0 => VSPID => GPIO23 => Pin 37
  // APA102 Clock Port 0 => VSPID => GPIO18 => Pin 30
  // APA102 Data Port 1 => HSPID => GPIO13 => Pin 16
  // APA102 Clock Port 1 => HSPID => GPIO14 => Pin 13
  APA102_RGBB,
};

class SPIClass;

class serialLEDDriver {
  public:

    serialLEDDriver(void);

    void setStrip(uint8_t port, uint16_t size, uint16_t config);
    void updateStrip(uint8_t port, uint16_t size, uint16_t config);

    uint8_t* getBuffer(uint8_t port);
    void clearBuffer(uint8_t port, uint16_t start);
    void clearBuffer(uint8_t port) {
      clearBuffer(port, 0);
    }
    void setBuffer(uint8_t port, uint16_t startChan, uint8_t* data, uint16_t size);

    uint8_t setPixel(uint8_t port, uint16_t pixel, uint8_t r, uint8_t g, uint8_t b, uint8_t w = 0);
    uint8_t setPixel(uint8_t port, uint16_t pixel, uint32_t colour);
    uint32_t getPixel(uint8_t port);

    bool show();

    uint16_t numPixels(uint8_t port);

    uint8_t buffer[LED_PORTS][PIX_MAX_BUFFER_SIZE];

    void doPixel(uint8_t* data, uint8_t pin, uint16_t numBytes);

  private:
    void setConfig(uint16_t config);

    void doPixel_apa102(uint8_t* data, uint8_t pin, uint16_t numBytes);
    void doPixel_ws2812(uint8_t* data, uint8_t pin, uint16_t numBytes);

    uint8_t _spi_buffer[PIX_MAX_BUFFER_SIZE * 8];
    uint16_t _datalen[LED_PORTS];
    uint16_t _config[LED_PORTS];
    uint32_t _pixellen;
    uint32_t _spi_speed;

    SPIClass _vspi;
    SPIClass _hspi;
};

#endif
