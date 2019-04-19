/*
ESP8266_ArtNetNode v3.0.0
Copyright (c) 2018, Tinic Uro
https://github.com/tinic/ESP8266_ArtNetNode_v2

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

// Change this if the settings structure changes
#define CONFIG_VERSION "300"

enum fx_mode {
  FX_MODE_PIXEL_MAP = 0,
  FX_MODE_12 = 1
};

enum p_type {
  TYPE_DMX_OUT = 0,
  TYPE_RDM_OUT = 1,
  TYPE_DMX_IN = 2,
  TYPE_SERIAL_LED = 3
};

enum p_protocol {
  PROT_ARTNET = 0,
  PROT_ARTNET_SACN = 1
};

enum p_merge {
  MERGE_LTP = 0,
  MERGE_HTP = 1
};

struct StoreStruct {
  // StoreStruct version
  char version[4];

  // Device settings:
  IPAddress ip;
  IPAddress subnet;
  IPAddress gateway;
  IPAddress broadcast;
  IPAddress hotspotIp;
  IPAddress hotspotSubnet;
  IPAddress hotspotBroadcast;
  IPAddress dmxInBroadcast;
  
  bool dhcpEnable;
  bool standAloneEnable;
  bool ethernetEnable;
  
  char nodeName[18];
  char longName[64];
  char wifiSSID[40];
  char wifiPass[40];
  char hotspotSSID[20];
  char hotspotPass[20];
  
  uint16_t hotspotDelay;
  uint8_t portAmode;
  uint8_t portBmode; 
  uint8_t portAprot;
  uint8_t portBprot;
  uint8_t portAmerge;
  uint8_t portBmerge;
  
  uint8_t portAnet;
  uint8_t portAsub;
  uint8_t portAuni[4];
  uint8_t portBnet;
  uint8_t portBsub;
  uint8_t portBuni[4];
  uint8_t portAsACNuni[4];
  uint8_t portBsACNuni[4];
  
  uint16_t portAnumPix;
  uint16_t portBnumPix;
  uint16_t portApixConfig;
  uint16_t portBpixConfig;

  bool doFirmwareUpdate;
  
  uint8_t portApixMode;
  uint8_t portBpixMode;
  
  uint16_t portApixFXstart;
  uint16_t portBpixFXstart;
  
  uint8_t resetCounter;
  uint8_t wdtCounter;
  
} deviceSettings = {

  CONFIG_VERSION,

  // The default values
  IPAddress(2,0,0,1),         // ip
  IPAddress(255,0,0,0),       // subnet
  IPAddress(2,0,0,1),         // gateway
  IPAddress(2,255,255,255),   // broadcast
  IPAddress(2,0,0,1),         // hotspotIP
  IPAddress(255,0,0,0),       // hotspotSubnet
  IPAddress(2,255,255,255),   // hotspotBroadcast
  IPAddress(2,255,255,255),   // dmxInBroadcast

  true,                       // dhcpEnable
  false,                      // standAloneEnable
  false,                      // ethernetEnable
  
  "espArtNetNode",            // nodeName
  "espArtNetNode",            // longName
  "",                         // wifiSSID
  "",                         // wifiPass
  "espArtNetNode",            // hotspotSSID
  "1234567890123",            // hotspotPass
  15,                         // hotspotDelay

  TYPE_SERIAL_LED,            // portAmode
  TYPE_SERIAL_LED,            // portBmode
  PROT_ARTNET,                // portAprot
  PROT_ARTNET,                // portBprot 
  MERGE_HTP,                  // portAmerge 
  MERGE_HTP,                  // portBmerge

  0,                          // portAnet
  0,                          // portAsub
  {0, 1, 2, 3},               // portAuni[4]
  
  0,                          // portBnet
  0,                          // portBsub
  {4, 5, 6, 7},               // portBuni[4]

  {1, 2, 3, 4},               // portAsACNuni[4]
  {5, 6, 7, 8},               // portBsACNuni[4]

  24,                         // portAnumPix
  24,                         // portBnumPix
   
  WS2812_RGB_800KHZ,          // portApixConfig
  WS2812_RGB_800KHZ,          // portBpixConfig

  false,                      // doFirmwareUpdate
  
  FX_MODE_PIXEL_MAP,          // portApixMode
  FX_MODE_PIXEL_MAP,          // portBpixMode

  1,                          // portApixFXstart
  1,                          // portBpixFXstart
  
  0,                          // resetCounter
  0                           // wdtCounter
};


void eepromSave() {
  for (uint16_t t = 0; t < sizeof(deviceSettings); t++) {
    EEPROM.write(t, *((char*)&deviceSettings + t));
  }
  EEPROM.commit();
}

void eepromLoad() {
  // To make sure there are settings, and they are YOURS!
  // If nothing is found it will use the default settings.
  if (EEPROM.read(0) == CONFIG_VERSION[0] &&
      EEPROM.read(1) == CONFIG_VERSION[1] &&
      EEPROM.read(2) == CONFIG_VERSION[2]) {

    // Store defaults for if we need them
    StoreStruct tmpStore;
    tmpStore = deviceSettings;
    
    // Copy data to deviceSettings structure
    for (uint16_t t = 0; t < sizeof(deviceSettings); t++) {
      *((char*)&deviceSettings + t) = EEPROM.read(t);
    }
    
#if 0
    // If we want to restore all our settings
    if (deviceSettings.resetCounter >= 5 || deviceSettings.wdtCounter >= 10) {
      deviceSettings.wdtCounter = 0;
      deviceSettings.resetCounter = 0;

      // Store defaults back into main settings
      deviceSettings = tmpStore;
    }
#endif  //#if 0

  // If config files dont match, save defaults
  } else {
    eepromSave();
    delay(500);
  }
}
