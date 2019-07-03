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

#include "driver/spi_master.h"
#include "esp_timer.h"

#include "serialLEDDriver.h"


// Timings from here:
// https://wp.josh.com/2014/05/13/ws2812-neopixels-are-not-so-finicky-once-you-get-to-know-them/

serialLEDDriver::serialLEDDriver() {
  _datalen[0] = 0;
  _datalen[1] = 0;

  _spi_datalen[0] = SPI_LATCH_BITS;
  _spi_datalen[1] = SPI_LATCH_BITS;

  _pixellen = 3;
  
  _spi_speed = 500000 * 8;
  
  memset(buffer, 0, sizeof(buffer));
  memset(_spi_buffer, 0, sizeof(_spi_buffer));

  {
    static spi_bus_config_t vspi_buscfg;
    memset(&vspi_buscfg, 0, sizeof(vspi_buscfg));
    vspi_buscfg.mosi_io_num = VSPI_IOMUX_PIN_NUM_MOSI;
    vspi_buscfg.sclk_io_num = VSPI_IOMUX_PIN_NUM_CLK;
    vspi_buscfg.miso_io_num = -1;
    vspi_buscfg.quadwp_io_num = -1;
    vspi_buscfg.quadhd_io_num = -1;
    vspi_buscfg.max_transfer_sz = sizeof(_spi_buffer);
    spi_bus_initialize(VSPI_HOST, &vspi_buscfg, 1);
    
    static spi_device_interface_config_t vspi_devcfg;
    memset(&vspi_devcfg, 0, sizeof(vspi_devcfg));
    vspi_devcfg.clock_speed_hz = _spi_speed;
    vspi_devcfg.spics_io_num = -1;
    vspi_devcfg.queue_size = 1;
    spi_bus_add_device(VSPI_HOST, &vspi_devcfg, &vspi_dev_handle);
  }
  
  {
    static spi_bus_config_t hspi_buscfg;
    memset(&hspi_buscfg, 0, sizeof(hspi_buscfg));
    hspi_buscfg.mosi_io_num = HSPI_IOMUX_PIN_NUM_MOSI;
    hspi_buscfg.sclk_io_num = HSPI_IOMUX_PIN_NUM_CLK;
    hspi_buscfg.miso_io_num = -1;
    hspi_buscfg.quadwp_io_num = -1;
    hspi_buscfg.quadhd_io_num = -1;
    hspi_buscfg.max_transfer_sz = sizeof(_spi_buffer);
    spi_bus_initialize(HSPI_HOST, &hspi_buscfg, 2);

    static spi_device_interface_config_t hspi_devcfg;
    memset(&hspi_devcfg, 0, sizeof(hspi_devcfg));
    hspi_devcfg.clock_speed_hz = _spi_speed;
    hspi_devcfg.spics_io_num = -1;
    hspi_devcfg.queue_size = 1;
    spi_bus_add_device(HSPI_HOST, &hspi_devcfg, &hspi_dev_handle);
  }

  esp_timer_init();
  
  static esp_timer_create_args_t timer_create_arg;
  memset(&timer_create_arg, 0, sizeof(timer_create_arg));
  timer_create_arg.callback = timerCallback;
  timer_create_arg.arg = this;
  
  static esp_timer_handle_t timer_handle;
  esp_timer_create(&timer_create_arg, &timer_handle);
  esp_timer_start_periodic(timer_handle, 1000);
}

void serialLEDDriver::timerCallback(void *arg) {
  ((serialLEDDriver *)arg)->timer();
}

void serialLEDDriver::timer() {

  static bool vspi_in_flight = false;
  spi_transaction_t *vspi_ret_trans = 0;
  if (!vspi_in_flight || 
       spi_device_get_trans_result(vspi_dev_handle, &vspi_ret_trans, 0) != ESP_ERR_TIMEOUT ) {

    static spi_transaction_t trans;
    memset(&trans, 0, sizeof(trans));
    trans.length = _spi_datalen[0] * 8;
    trans.tx_buffer = (void *)&_spi_buffer[0][0];
    spi_device_queue_trans(vspi_dev_handle, &trans, 0);
    vspi_in_flight = true;
  }
  
  static bool hspi_in_flight = false;
  spi_transaction_t *hspi_ret_trans = 0;
  if (!hspi_in_flight || 
       spi_device_get_trans_result(hspi_dev_handle, &hspi_ret_trans, 0) != ESP_ERR_TIMEOUT ) {

    static spi_transaction_t trans;
    memset(&trans, 0, sizeof(trans));
    trans.length = _spi_datalen[1] * 8;
    trans.tx_buffer = (void *)&_spi_buffer[1][0];
    spi_device_queue_trans(hspi_dev_handle, &trans, 0);
    hspi_in_flight = true;
  }

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
}

void serialLEDDriver::doPixel_ws2812(uint8_t* data, uint8_t port, uint16_t numBytes) {
  // Convert to SPI data
  uint8_t *dst = &_spi_buffer[port][0];
  for (int32_t c = 0; c < _datalen[port]; c++) {
    uint8_t p = data[c];
    *dst++ =
      ((p & (1 << 7)) ? 0b11000000 : 0b10000000)|
      ((p & (1 << 6)) ? 0b00001100 : 0b00001000);
    *dst++ =
      ((p & (1 << 5)) ? 0b11000000 : 0b10000000)|
      ((p & (1 << 4)) ? 0b00001100 : 0b00001000);
    *dst++ =
      ((p & (1 << 3)) ? 0b11000000 : 0b10000000)|
      ((p & (1 << 2)) ? 0b00001100 : 0b00001000);
    *dst++ =
      ((p & (1 << 1)) ? 0b11000000 : 0b10000000)|
      ((p & (1 << 0)) ? 0b00001100 : 0b00001000);
  }
  for (int32_t c = 0; c < SPI_LATCH_BITS; c++) {
      *dst++ = 0;
  }
  _spi_datalen[port] = dst - (&_spi_buffer[port][0]);
}
