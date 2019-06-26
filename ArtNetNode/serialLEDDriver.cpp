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
#include <Arduino.h>
#include <SPI.h>

#include "serialLEDDriver.h"

// Timings from here:
// https://wp.josh.com/2014/05/13/ws2812-neopixels-are-not-so-finicky-once-you-get-to-know-them/

serialLEDDriver::serialLEDDriver() {
  _datalen[0] = 0;
  _datalen[1] = 0;
  _pixellen = 3;
  _spi_speed = 800000 * 8;
  _vspi.begin();
  _hspi.begin();
}

void serialLEDDriver::setConfig(uint16_t config) {
  switch (config) {
    default:
    case WS2812_RGB:
      _pixellen = 3;
      break;
    case WS2812_RGBW:
    case WS2812_RGBW_SPLIT:
    case APA102_RGBB:
      _pixellen = 4;
      break;
  }
}

void serialLEDDriver::setStrip(uint8_t port, uint16_t size, uint16_t config) {
  setConfig(config);

  _datalen[port] = size  * _pixellen;
  _config[port] = config;

  clearBuffer(port);

  // Clear the strip
  uint8_t* b = buffer[port];
  doPixel(b, port, PIX_MAX_BUFFER_SIZE);
}

void serialLEDDriver::updateStrip(uint8_t port, uint16_t size, uint16_t config) {
  setConfig(config);

  size *= _pixellen;

  // Clear the strip if it's shorter than our current strip
  if (size < _datalen[port] || _config[port] != config) {
    clearBuffer(port, size);

    uint8_t* b = buffer[port];
    doPixel(b, port, _datalen[port]);
  }

  _datalen[port] = size;
  _config[port] = config;
}

uint8_t* serialLEDDriver::getBuffer(uint8_t port) {
  return buffer[port];
}

void serialLEDDriver::clearBuffer(uint8_t port, uint16_t start) {
  memset(&buffer[port][start], 0, PIX_MAX_BUFFER_SIZE - start);
}

void serialLEDDriver::setBuffer(uint8_t port, uint16_t startChan, uint8_t* data, uint16_t size) {
  uint8_t* a = buffer[port];

  if (_config[port] == WS2812_RGBW_SPLIT) {
    // Interleave W channel
    if (startChan >= 512*3) {
      uint8_t *dst = &a[startChan-512*3];
      uint8_t *src = &data[0];
      for (size_t c=0; c<size; c++) {
        dst[c*4+3] = src[c];
      }
    } else {
      uint8_t *dst = &a[startChan];
      uint8_t *src = &data[0];
      for (size_t c=0; c<size/4; c++) {
        dst[c*4+0] = src[c*3+1];
        dst[c*4+1] = src[c*3+0];
        dst[c*4+2] = src[c*3+2];
      }
    }
    return;
  }

  uint8_t *dst = &a[startChan];
  memcpy(dst, data, size);
  for (size_t c=0; c<size; c+=_pixellen) {
    uint8_t tmp = dst[c+0];
    dst[c+0] = dst[c+1];
    dst[c+1] = tmp;
  }
}

uint8_t serialLEDDriver::setPixel(uint8_t port, uint16_t pixel, uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
  uint8_t* a = buffer[port];

  uint16_t chan = pixel * _pixellen;
  a[chan + 1] = r;
  a[chan + 0] = g;
  a[chan + 2] = b;
  if (_pixellen > 3) {
    a[chan + 3] = w;
  }
}

uint8_t serialLEDDriver::setPixel(uint8_t port, uint16_t pixel, uint32_t colour) {
  setPixel(port, pixel, ((colour >> 16) & 0xFF), ((colour >> 8) & 0xFF), (colour & 0xFF), ((colour >> 24) & 0xFF));
}

uint32_t serialLEDDriver::getPixel(uint8_t port) {
  uint8_t* b = buffer[port];
  uint16_t chan = _datalen[port] * _pixellen;
  // ws2812 is GRB ordering - return RGB
  if (_pixellen > 3) {
    return ((b[chan + 3] << 24) | (b[chan + 1] << 16) | (b[chan] << 8) | (b[chan + 2]));
  } else {
    return ((b[chan + 1] << 16) | (b[chan] << 8) | (b[chan + 2]));
  }
}

uint16_t serialLEDDriver::numPixels(uint8_t port) {
  return _datalen[port] / _pixellen;
}

bool serialLEDDriver::show() {
  if (_datalen[0] == 0 && _datalen[1] == 0) {
    return 1;
  }

  if ( _datalen[0] != 0) {
    uint8_t* b0 = buffer[0];
    doPixel(b0, 0, _datalen[0]);
  }

  if (_datalen[1] != 0) {
    uint8_t* b1 = buffer[1];
    doPixel(b1, 1, _datalen[1]);
  }

  return 1;
}

void serialLEDDriver::doPixel(uint8_t* data, uint8_t port, uint16_t numBytes) {
  switch (_config[port])
  {
    case WS2812_RGB:
    case WS2812_RGBW_SPLIT:
    case WS2812_RGBW: {
        doPixel_ws2812(data, port, numBytes);
      } break;
    case APA102_RGBB: {
        doPixel_apa102(data, port, numBytes);
      } break;
  }
}

void serialLEDDriver::doPixel_apa102(uint8_t* data, uint8_t port, uint16_t numBytes) {

  if (port == 0) {
    // Convert to SPI data
    uint8_t *dst = &_spi_buffer[0];
    for (int32_t c = 0; c < _datalen[0]; c += 4) {
      uint32_t p0 = data[c + 0];
      uint32_t p1 = data[c + 1];
      uint32_t p2 = data[c + 2];
      uint32_t p3 = data[c + 3];
      *dst ++ = 0xE0 | min(p3, uint32_t(0x1F));
      *dst ++ = p2;
      *dst ++ = p0;
      *dst ++ = p1;
    }

    _vspi.beginTransaction(SPISettings(_spi_speed, MSBFIRST, SPI_MODE0));

    _vspi.write(0);
    _vspi.write(0);
    _vspi.write(0);
    _vspi.write(0);

    uint8_t *src = &_spi_buffer[0];
    int32_t leds = _datalen[0] / _pixellen;
    for (size_t c = 0; c < leds; c++) {
      _vspi.writeBytes(src, _pixellen);
      src += _pixellen;
    }

    uint32_t latch_bytes = ( leds / 2 + 8 ) / 8;
    for (size_t c = 0; c < latch_bytes; c++) {
      _vspi.write(0);
    }

  } else if (port == 1) {
    uint8_t *dst = &_spi_buffer[0];
    for (int32_t c = 0; c < _datalen[0]; c += 4) {
      uint32_t p0 = data[c + 0];
      uint32_t p1 = data[c + 1];
      uint32_t p2 = data[c + 2];
      uint32_t p3 = data[c + 3];
      *dst ++ = 0xE0 | min(p3, uint32_t(0x1F));
      *dst ++ = p2;
      *dst ++ = p0;
      *dst ++ = p1;
    }

    _hspi.beginTransaction(SPISettings(_spi_speed, MSBFIRST, SPI_MODE0));

    _hspi.write(0);
    _hspi.write(0);
    _hspi.write(0);
    _hspi.write(0);

    uint8_t *src = &_spi_buffer[0];
    int32_t leds = _datalen[0] / _pixellen;
    for (size_t c = 0; c < leds; c++) {
      _hspi.writeBytes(src, _pixellen);
      src += _pixellen;
    }

    uint32_t latch_bytes = ( leds / 2 + 8 ) / 8;
    for (size_t c = 0; c < latch_bytes; c++) {
      _hspi.write(0);
    }
  }
}

void serialLEDDriver::doPixel_ws2812(uint8_t* data, uint8_t port, uint16_t numBytes) {

  if (port == 0) {
    // Convert to SPI data
    uint8_t *dst = &_spi_buffer[0];
    for (int32_t c = 0; c < _datalen[0]; c++) {
      uint8_t p = data[c];
      for (int32_t d = 7; d >= 0; d--) {
        if (p & (1 << d)) {
          *dst++ = 0b11110000;
        } else {
          *dst++ = 0b11000000;
        }
      }
    }

    _vspi.beginTransaction(SPISettings(_spi_speed, MSBFIRST, SPI_MODE0));
    uint8_t *src = &_spi_buffer[0];
    int32_t leds = _datalen[0] / _pixellen;
    for (size_t c = 0; c < leds; c++) {
      _vspi.writeBytes(src, _pixellen * 8);
      src += _pixellen * 8;
    }
    for (size_t c = 0; c < SPI_LATCH_BITS; c++) {
      _vspi.write(0);
    }
    _vspi.endTransaction();

  } else if (port == 1) {
    // Convert to SPI data
    uint8_t *dst = &_spi_buffer[0];
    for (int32_t c = 0; c < _datalen[1]; c++) {
      uint8_t p = data[c];
      for (int32_t d = 7; d >= 0; d--) {
        if (p & (1 << d)) {
          *dst++ = 0b11110000;
        } else {
          *dst++ = 0b11000000;
        }
      }
    }

    _hspi.beginTransaction(SPISettings(_spi_speed, MSBFIRST, SPI_MODE0));
    uint8_t *src = &_spi_buffer[0];
    int32_t leds = _datalen[1] / _pixellen;
    for (size_t c = 0; c < leds; c++) {
      _hspi.writeBytes(src, _pixellen * 8);
      src += _pixellen * 8;
    }
    for (size_t c = 0; c < SPI_LATCH_BITS; c++) {
      _hspi.write(0);
    }
    _hspi.endTransaction();
  }
}
