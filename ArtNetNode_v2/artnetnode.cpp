/*
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

Note:
This is a pre-release version of this software.  It is not yet ready for prime time and contains bugs (known and unknown).
Please submit any bugs or code changes so they can be included into the next release.

Prizes up for grabs:
I am giving away a few of my first batch of prototype PCBs.  They will be fully populated - valued at over $30 just for parts.
In order to recieve one, please complete one of the following tasks.  You can "win" multiple boards.
1 - Fix the WDT reset issue (https://github.com/mtongnz/ESP8266_ArtNetNode_v2/issues/41)
2 - Implement stored scenes function.  I want it to allow for static scenes or for chases to run.
3 - Most bug fixes, code improvements, feature additions & helpful submissions.
    eg. Fixing the flickering WS2812 (https://github.com/mtongnz/ESP8266_ArtNetNode_v2/issues/36)
        Adding other pixel strips (https://github.com/mtongnz/ESP8266_ArtNetNode_v2/issues/42)
        Creating new web UI theme (https://github.com/mtongnz/ESP8266_ArtNetNode_v2/issues/22)
        
These prizes will be based on the first person to submit a solution that I judge to be adequate.  My decision is final.
This competition will open to the general public a couple of weeks after the private code release to supporters.
*/

#ifdef ESP32

#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>
#include <Update.h>
#include <rom/rtc.h>

#define os_malloc malloc
#define os_free free

#else // #ifdef ESP32

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>

extern "C" {
  #include "user_interface.h"
  extern struct rst_info resetInfo;
}

#endif  // #ifdef ESP32

#include <WiFiClient.h>
#include <ArduinoJson.h>
#include <EEPROM.h>
#include <FS.h>
#include "ws2812Driver.h"
#include "store.h"
#include "wsFX.h"
#include "espDMX_RDM.h"
#include "espArtNetRDM.h"

#define FIRMWARE_VERSION "v2.0.0 (beta 5g)"
#define ART_FIRM_VERSION 0x0200   // Firmware given over Artnet (2 bytes)

//#define ESP_01              // Un comment for ESP_01 board settings
//#define NO_RESET            // Un comment to disable the reset button

// Wemos boards use 4M (3M SPIFFS) compiler option

#define ARTNET_OEM 0x0123    // Artnet OEM Code
#define ESTA_MAN 0x08DD      // ESTA Manufacturer Code
#define ESTA_DEV 0xEE000000  // RDM Device ID (used with Man Code to make 48bit UID)

#ifdef ESP_01
  #define DMX_DIR_A 2   // Same pin as TX1
  #define DMX_TX_A 1
  #define ONE_PORT
  #define NO_RESET

  #define WS2812_ALLOW_INT_SINGLE false
  #define WS2812_ALLOW_INT_DOUBLE false
  
#else
  #define NO_RESET

  #define DMX_DIR_A 8 // GPIO32
  #define DMX_TX_A 32

  #define DMX_DIR_B 9 // GPIO33
  #define DMX_TX_B 33

  #define STATUS_LED_PIN 12
//  #define STATUS_LED_MODE_WS2812
//  #define STATUS_LED_MODE_APA106
  #define STATUS_LED_A 0  // Physical wiring order for status LEDs
  #define STATUS_LED_B 1
  #define STATUS_LED_S 2
  
  #define WS2812_ALLOW_INT_SINGLE false
  #define WS2812_ALLOW_INT_DOUBLE false
#endif

#ifndef NO_RESET
  #define SETTINGS_RESET 14
#endif

// Definitions for status leds  xxBBRRGG
#define BLACK 0x00000000
#define WHITE 0x00FFFFFF
#define RED 0x0000FF00
#define GREEN 0x000000FF
#define BLUE 0x00FF0000
#define CYAN 0x00FF00FF
#define PINK 0x0066FF22
#define MAGENTA 0x00FFFF00
#define YELLOW 0x0000FFFF
#define ORANGE 0x0000FF33
#define STATUS_DIM 0x0F

static uint8_t portA[5], portB[5];
static uint8_t MAC_array[6];
static uint8_t dmxInSeqID = 0;
static uint8_t statusLedData[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
static uint32_t statusTimer = 0;

static esp8266ArtNetRDM artRDM;
#ifdef ESP32
static WebServer webServer(80);
#else  // #ifdef ESP32
static ESP8266WebServer webServer(80);
#endif  // #ifdef ESP32
static DynamicJsonBuffer jsonBuffer;
static ws2812Driver pixDriver;
static File fsUploadFile;
static bool statusLedsDim = true;
static bool statusLedsOff = false;

static pixPatterns pixFXA(0, &pixDriver);
static pixPatterns pixFXB(1, &pixDriver);

static const char PROGMEM mainPage[] = "<!DOCTYPE html><meta content='text/html; charset=utf-8' http-equiv=Content-Type /><title>ESP ArtNetNode Config</title><meta content='Matthew Tong - http://github.com/mtongnz/' name=DC.creator /><meta content=en name=DC.language /><meta content='width=device-width,initial-scale=1' name=viewport /><link href=style.css rel=stylesheet /><div id=page><div class=inner><div class=mast><div class=title>esp<h1>ArtNet & sACN</h1>to<h1>DMX & LED Pixels</h1></div><ul class=nav><li class=first><a href='javascript: menuClick(1)'>Device Status</a><li><a href='javascript: menuClick(2)'>WiFi</a><li><a href='javascript: menuClick(3)'>IP & Name</a><li><a href='javascript: menuClick(4)'>Port A</a>"
    #ifndef ONE_PORT
      "<li><a href='javascript: menuClick(5)'>Port B</a>"
    #endif
  "<li><a href='javascript: menuClick(6)'>Scenes</a><li><a href='javascript: menuClick(7)'>Firmware</a><li class=last><a href='javascript: reboot()'>Reboot</a></ul><div class=author><i>Design by</i> Matthew Tong</div></div><div class='main section'><div class=hide name=error><h2>Error</h2><p class=center>There was an error communicating with the device. Refresh the page and try again.</div><div class=show name=sections><h2>Fetching Data</h2><p class=center>Fetching data from device. If this message is still here in 15 seconds, try refreshing the page or clicking the menu option again.</div><div class=hide name=sections><h2>Device Status</h2><p class=left>Device Name:<p class=right name=nodeName><p class=left>MAC Address:<p class=right name=macAddress><p class=left>Wifi Status:<p class=right name=wifiStatus><p class=left>IP Address:<p class=right name=ipAddressT><p class=left>Subnet Address:<p class=right name=subAddressT><p class=left>Port A:<p class=right name=portAStatus>"
    #ifndef ONE_PORT
      "<p class=left>Port B:<p class=right name=portBStatus>"
    #endif
  "<p class=left>Scene Storage:<p class=right name=sceneStatus><p class=left>Firmware:<p class=right name=firmwareStatus></div><div class=hide name=sections><input name=save type=button value='Save Changes'/><h2>WiFi Settings</h2><p class=left>MAC Address:<p class=right name=macAddress><p class=spacer><p class=left>Wifi SSID:<p class=right><input type=text name=wifiSSID /><p class=left>Password:<p class=right><input type=text name=wifiPass /><p class=spacer><p class=left>Hotspot SSID:<p class=right><input type='text' name='hotspotSSID' /><p class=left>Password:<p class=right><input type=text name=hotspotPass /><p class=left>Start Delay:<p class=right><input name=hotspotDelay type=number min=0 max=180 class=number /> (seconds)<p class=spacer><p class=left>Stand Alone:<p class=right><input name=standAloneEnable type=checkbox value=true /><p class=right>In normal mode, the hotspot will start after <i>delay</i> seconds if the main WiFi won't connect. If no users connect, the device will reset and attempt the main WiFi again. This feature is purely for changing settings and ArtNet data is ignored.<p class=right>Stand alone mode disables the primary WiFi connection and allows ArtNet data to be received via the hotspot connection.</div><div class=hide name=sections><input name=save type=button value='Save Changes'/><h2>IP & Node Name</h2><p class='left'>Short Name:</p><p class=right><input type=text name=nodeName /><p class=left>Long Name:<p class=right><input type=text name=longName /><p class=spacer><p class=left>Enable DHCP:<p class=right><input name=dhcpEnable type=checkbox value=true /><p class=left>IP Address:<p class=right><input name=ipAddress type=number min=0 max=255 class=number /> <input name=ipAddress type=number min=0 max=255 class=number /> <input name=ipAddress type=number min=0 max=255 class=number /> <input name=ipAddress type=number min=0 max=255 class=number /><p class=left>Subnet Address:<p class=right><input name=subAddress type=number min=0 max=255 class=number /> <input name=subAddress type=number min=0 max=255 class=number /> <input name=subAddress type=number min=0 max=255 class=number /> <input name=subAddress type=number min=0 max=255 class=number /><p class=left>Gateway Address:<p class=right><input name=gwAddress type=number min=0 max=255 class=number /> <input name=gwAddress type=number min=0 max=255 class=number /> <input name=gwAddress type=number min=0 max=255 class=number /> <input name=gwAddress type=number min=0 max=255 class=number /><p class=left>Broadcast Address:<p class=right name=bcAddress><p class=center>These settings only affect the main WiFi connection. The hotspot will always have DHCP enabled and an IP of <b>2.0.0.1</b></div><div class=hide name=sections><input name=save type=button value='Save Changes'/><h2>Port A Settings</h2><p class=left>Port Type:<p class=right><select class=select name=portAmode><option value=0>DMX Output<option value=1>DMX Output with RDM<option value=2>DMX Input<option value=3>LED Pixels - WS2812</select><p class=left>Protocol:<p class=right><select class=select name=portAprot><option value=0>Artnet v4<option value=1>Artnet v4 with sACN DMX</select><p class=left>Merge Mode:<p class=right><select class=select name=portAmerge><option value=0>Merge LTP<option value=1>Merge HTP</select><p class=left>Net:<p class=right><input name=portAnet type=number min=0 max=127 class=number /><p class=left>Subnet:<p class=right><input name=portAsub type=number min=0 max=15 class=number /><p class=left>Universe:<p class=right><input type=number min=0 max=15 name=portAuni class=number /><span name=portApix> <input type=number min=0 max=15 name=portAuni class=number /> <input type=number min=0 max=15 name=portAuni class=number /> <input type=number min=0 max=15 name=portAuni class=number /></span></p><p class=left>sACN Universe:<p class=right><input type=number min=0 max=63999 name=portAsACNuni class=number /> <input type=number min=1 max=63999 name=portAsACNuni class=number /> <input type=number min=1 max=63999 name=portAsACNuni class=number /> <input type=number min=1 max=63999 name=portAsACNuni class=number /></p><span name=DmxInBcAddrA><p class=left>Broadcast Address:<p class=right><input type=number min=0 max=255 name=dmxInBroadcast class=number /> <input type=number min=0 max=255 name=dmxInBroadcast class=number /> <input type=number min=0 max=255 name=dmxInBroadcast class=number /> <input type=number min=0 max=255 name=dmxInBroadcast class=number /></p></span><span name=portApix><p class=left>Number of Pixels:<p class=right><input type=number min=0 max=680 name=portAnumPix class=number /> 680 max - 170 per universe</p><p class=left>Mode:</p><p class=right><select class=select name=portApixMode><option value=0>Pixel Mapping</option><option value=1>12 Channel FX</option></select><p class=left>Start Channel:</p><p class=right><input type=number name=portApixFXstart class=number min=1 max=501 /> FX modes only</p></span></div><div class=hide name=sections><input name=save type=button value='Save Changes'/><h2>Port B Settings</h2><p class=left>Port Type:<p class=right><select class=select name=portBmode><option value=0>DMX Output<option value=1>DMX Output with RDM<option value=3>LED Pixels - WS2812</select><p class=left>Protocol:<p class=right><select class=select name=portBprot><option value=0>Artnet v4<option value=1>Artnet v4 with sACN DMX</select><p class=left>Merge Mode:<p class=right><select class=select name=portBmerge><option value=0>Merge LTP<option value=1>Merge HTP</select><p class=left>Net:<p class=right><input name=portBnet type=number min=0 max=127 class=number /><p class=left>Subnet:<p class=right><input name=portBsub type=number min=0 max=15 class=number /><p class=left>Universe:<p class=right><input type=number min=0 max=15 name=portBuni class=number /><span name=portBpix> <input type=number min=0 max=15 name=portBuni class=number /> <input type=number min=0 max=15 name=portBuni class=number /> <input type=number min=0 max=15 name=portBuni class=number /></span></p><p class=left>sACN Universe:<p class=right><input type=number min=1 max=63999 name=portBsACNuni class=number /> <input type=number min=1 max=63999 name=portBsACNuni class=number /> <input type=number min=1 max=63999 name=portBsACNuni class=number /> <input type=number min=1 max=63999 name=portBsACNuni class=number /></p><span name=portBpix><p class=left>Number of Pixels:<p class=right><input type=number min=0 max=680 name=portBnumPix class=number /> 680 max - 170 per universe</p><p class=left>Mode:</p><p class=right><select class=select name=portBpixMode><option value=0>Pixel Mapping</option><option value=1>12 Channel FX</option></select><p class=left>Start Channel:</p><p class=right><input type=number name=portBpixFXstart class=number min=1 max=501 /> FX modes only</p></span></div><div class=hide name=sections><h2>Stored Scenes</h2><p class=center>Not yet implemented</div><div class=hide name=sections><form action=/update enctype=multipart/form-data method=POST id=firmForm><h2>Update Firmware</h2><p class=left>Firmware:<p class=right name=firmwareStatus><p class=right><input name=update type=file id=update><label for=update><svg height=17 viewBox='0 0 20 17' width=20 xmlns=http://www.w3.org/2000/svg><path d='M10 0l-5.2 4.9h3.3v5.1h3.8v-5.1h3.3l-5.2-4.9zm9.3 11.5l-3.2-2.1h-2l3.4 2.6h-3.5c-.1 0-.2.1-.2.1l-.8 2.3h-6l-.8-2.2c-.1-.1-.1-.2-.2-.2h-3.6l3.4-2.6h-2l-3.2 2.1c-.4.3-.7 1-.6 1.5l.6 3.1c.1.5.7.9 1.2.9h16.3c.6 0 1.1-.4 1.3-.9l.6-3.1c.1-.5-.2-1.2-.7-1.5z'/></svg> <span>Choose Firmware</span></label><p class=right id=uploadMsg></p><p class=right><input type=button class=submit value='Upload Now' id=fUp></div></div><div class=footer><p>Coding and hardware © 2016-2017 <a href=http://github.com/mtongnz/ >Matthew Tong</a>.<p>Released under <a href=http://www.gnu.org/licenses/ >GNU General Public License V3</a>.</div></div></div><script>var cl=0;var num=0;var err=0;var o=document.getElementsByName('sections');var s=document.getElementsByName('save');for (var i=0, e; e=s[i++];)e.addEventListener( 'click', function(){sendData();}); var u=document.getElementById('fUp');var um=document.getElementById('uploadMsg');var fileSelect=document.getElementById('update');u.addEventListener('click',function(){uploadPrep()});function uploadPrep(){if(fileSelect.files.length===0) return;u.disabled=!0;u.value='Preparing Device…';var x=new XMLHttpRequest();x.onreadystatechange=function(){if(x.readyState==XMLHttpRequest.DONE){try{var r=JSON.parse(x.response)}catch(e){var r={success:0,doUpdate:1}} if(r.success==1&&r.doUpdate==1){uploadWait()}else{um.value='<b>Update failed!</b>';u.value='Upload Now';u.disabled=!1}}};x.open('POST','/ajax',!0);x.setRequestHeader('Content-Type','application/json');x.send('{\"doUpdate\":1,\"success\":1}')} function uploadWait(){setTimeout(function(){var z=new XMLHttpRequest();z.onreadystatechange=function(){if(z.readyState==XMLHttpRequest.DONE){try{var r=JSON.parse(z.response)}catch(e){var r={success:0}} console.log('r=' + r.success); if(r.success==1){upload()}else{uploadWait()}}};z.open('POST','/ajax',!0);z.setRequestHeader('Content-Type','application/json');z.send('{\"doUpdate\":2,\"success\":1}')},1000)} var upload=function(){u.value='Uploading… 0%';var data=new FormData();data.append('update',fileSelect.files[0]);var x=new XMLHttpRequest();x.onreadystatechange=function(){if(x.readyState==4){try{var r=JSON.parse(x.response)}catch(e){var r={success:0,message:'No response from device.'}} console.log(r.success+': '+r.message);if(r.success==1){u.value=r.message;setTimeout(function(){location.reload()},15000)}else{um.value='<b>Update failed!</b> '+r.message;u.value='Upload Now';u.disabled=!1}}};x.upload.addEventListener('progress',function(e){var p=Math.ceil((e.loaded/e.total)*100);console.log('Progress: '+p+'%');if(p<100) u.value='Uploading... '+p+'%';else u.value='Upload complete. Processing…'},!1);x.open('POST','/upload',!0);x.send(data)}; function reboot() { if (err == 1) return; var r = confirm('Are you sure you want to reboot?'); if (r != true) return; o[cl].className = 'hide'; o[0].childNodes[0].innerHTML = 'Rebooting'; o[0].childNodes[1].innerHTML = 'Please wait while the device reboots. This page will refresh shortly unless you changed the IP or Wifi.'; o[0].className = 'show'; err = 0; var x = new XMLHttpRequest(); x.onreadystatechange = function(){ if(x.readyState == 4){ try { var r = JSON.parse(x.response); } catch (e){ var r = {success: 0, message: 'Unknown error: [' + x.responseText + ']'}; } if (r.success != 1) { o[0].childNodes[0].innerHTML = 'Reboot Failed'; o[0].childNodes[1].innerHTML = 'Something went wrong and the device didn\\'t respond correctly. Please try again.'; } setTimeout(function() { location.reload(); }, 5000); } }; x.open('POST', '/ajax', true); x.setRequestHeader('Content-Type', 'application/json'); x.send('{\"reboot\":1,\"success\":1}'); } function sendData(){var d={'page':num};for (var i=0, e; e=o[cl].getElementsByTagName('INPUT')[i++];){var k=e.getAttribute('name');var v=e.value;if (k in d) continue; if (k=='ipAddress' || k=='subAddress' || k=='gwAddress' || k=='portAuni' || k=='portBuni' || k=='portAsACNuni' || k=='portBsACNuni' || k=='dmxInBroadcast'){var c=[v];for (var z=1; z < 4; z++){c.push(o[cl].getElementsByTagName('INPUT')[i++].value);}d[k]=c; continue;}if (e.type==='text')d[k]=v;if (e.type==='number'){if (v=='')v=0;d[k]=v;}if (e.type==='checkbox'){if (e.checked)d[k]=1;else d[k]=0;}}for (var i=0, e; e=o[cl].getElementsByTagName('SELECT')[i++];){d[e.getAttribute('name')]=e.options[e.selectedIndex].value;}d['success']=1;var x=new XMLHttpRequest();x.onreadystatechange=function(){handleAJAX(x);};x.open('POST', '/ajax');x.setRequestHeader('Content-Type', 'application/json');x.send(JSON.stringify(d));console.log(d);} function menuClick(n){if (err==1) return; num=n; setTimeout(function(){if (cl==num || err==1) return; o[cl].className='hide'; o[0].className='show'; cl=0;}, 100); var x=new XMLHttpRequest(); x.onreadystatechange=function(){handleAJAX(x);}; x.open('POST', '/ajax'); x.setRequestHeader('Content-Type', 'application/json'); x.send(JSON.stringify({\"page\":num,\"success\":1}));}function handleAJAX(x){if (x.readyState==XMLHttpRequest.DONE ){if (x.status==200){var response=JSON.parse(x.responseText);console.log(response);if (!response.hasOwnProperty('success')){err=1; o[cl].className='hide'; document.getElementsByName('error')[0].className='show';return;}if (response['success'] !=1){err=1; o[cl].className='hide';document.getElementsByName('error')[0].getElementsByTagName('P')[0].innerHTML=response['message']; document.getElementsByName('error')[0].className='show';return;}if (response.hasOwnProperty('message')) { for (var i = 0, e; e = s[i++];) { e.value = response['message']; e.className = 'showMessage' } setTimeout(function() { for (var i = 0, e; e = s[i++];) { e.value = 'Save Changes'; e.className = '' } }, 5000); } o[cl].className='hide'; o[num].className='show'; cl=num; for (var key in response){if (response.hasOwnProperty(key)){var a=document.getElementsByName(key); if (key=='ipAddress' || key=='subAddress'){var b=document.getElementsByName(key + 'T'); for (var z=0; z < 4; z++){a[z].value=response[key][z]; if (z==0) b[0].innerHTML=''; else b[0].innerHTML=b[0].innerHTML + ' . '; b[0].innerHTML=b[0].innerHTML + response[key][z];}continue;}else if (key=='bcAddress'){for (var z=0; z < 4; z++){if (z==0) a[0].innerHTML=''; else a[0].innerHTML=a[0].innerHTML + ' . '; a[0].innerHTML=a[0].innerHTML + response[key][z];}continue;} else if (key=='gwAddress' || key=='dmxInBroadcast' || key=='portAuni' || key=='portBuni' || key=='portAsACNuni' || key=='portBsACNuni'){for(var z=0;z<4;z++){a[z].value = response[key][z];}continue}if(key=='portAmode'){var b = document.getElementsByName('portApix');var c = document.getElementsByName('DmxInBcAddrA');if(response[key] == 3) {b[0].style.display = '';b[1].style.display = '';} else {b[0].style.display = 'none';b[1].style.display = 'none';}if (response[key] == 2){c[0].style.display = '';}else{c[0].style.display = 'none';}} else if (key == 'portBmode') {var b = document.getElementsByName('portBpix');if(response[key] == 3) {b[0].style.display = '';b[1].style.display = '';} else {b[0].style.display = 'none';b[1].style.display = 'none';}}for (var z=0; z < a.length; z++){switch (a[z].nodeName){case 'P': case 'DIV': a[z].innerHTML=response[key]; break; case 'INPUT': if (a[z].type=='checkbox'){if (response[key]==1) a[z].checked=true; else a[z].checked=false;}else a[z].value=response[key]; break; case 'SELECT': for (var y=0; y < a[z].options.length; y++){if (a[z].options[y].value==response[key]){a[z].options.selectedIndex=y; break;}}break;}}}}}else{err=1; o[cl].className='hide'; document.getElementsByName('error')[0].className='show';}}}var update=document.getElementById('update');var label=update.nextElementSibling;var labelVal=label.innerHTML;update.addEventListener( 'change', function( e ){var fileName=e.target.value.split( '\\\\' ).pop(); if( fileName ) label.querySelector( 'span' ).innerHTML=fileName; else label.innerHTML=labelVal; update.blur();}); document.onkeydown=function(e){if(cl < 2 || cl > 6)return; var e = e||window.event; if (e.keyCode == 13)sendData();}; menuClick(1);</script></body></html>";

static const char PROGMEM cssUploadPage[] = "<html><head><title>espArtNetNode CSS Upload</title></head><body>Select and upload your CSS file.  This will overwrite any previous uploads but you can restore the default below.<br /><br /><form method='POST' action='/style_upload' enctype='multipart/form-data'><input type='file' name='css'><input type='submit' value='Upload New CSS'></form><br /><a href='/style_delete'>Restore default CSS</a></body></html>";

static const char PROGMEM css[] = ".author,.title,ul.nav a{text-align:center}.author i,.show,.title h1,ul.nav a{display:block}input,ul.nav a:hover{background-color:#DADADA}a,abbr,acronym,address,applet,b,big,blockquote,body,caption,center,cite,code,dd,del,dfn,div,dl,dt,em,fieldset,font,form,h1,h2,h3,h4,h5,h6,html,i,iframe,img,ins,kbd,label,legend,li,object,ol,p,pre,q,s,samp,small,span,strike,strong,sub,sup,table,tbody,td,tfoot,th,thead,tr,tt,u,ul,var{margin:0;padding:0;border:0;outline:0;font-size:100%;vertical-align:baseline;background:0 0}.main h2,li.last{border-bottom:1px solid #888583}body{line-height:1;background:#E4E4E4;color:#292929;color:rgba(0,0,0,.82);font:400 100% Cambria,Georgia,serif;-moz-text-shadow:0 1px 0 rgba(255,255,255,.8);}ol,ul{list-style:none}a{color:#890101;text-decoration:none;-moz-transition:.2s color linear;-webkit-transition:.2s color linear;transition:.2s color linear}a:hover{color:#DF3030}#page{padding:0}.inner{margin:0 auto;width:91%}.amp{font-family:Baskerville,Garamond,Palatino,'Palatino Linotype','Hoefler Text','Times New Roman',serif;font-style:italic;font-weight:400}.mast{float:left;width:31.875%}.title{font:semi 700 16px/1.2 Baskerville,Garamond,Palatino,'Palatino Linotype','Hoefler Text','Times New Roman',serif;padding-top:0}.title h1{font:700 20px/1.2 'Book Antiqua','Palatino Linotype',Georgia,serif;padding-top:0}.author{font:400 100% Cambria,Georgia,serif}.author i{font:400 12px Baskerville,Garamond,Palatino,'Palatino Linotype','Hoefler Text','Times New Roman',serif;letter-spacing:.05em;padding-top:.7em}.footer,.main{float:right;width:65.9375%}ul.nav{margin:1em auto 0;width:11em}ul.nav a{font:700 14px/1.2 'Book Antiqua','Palatino Linotype',Georgia,serif;letter-spacing:.1em;padding:.7em .5em;margin-bottom:0;text-transform:uppercase}input[type=button],input[type=button]:focus{background-color:#E4E4E4;color:#890101}li{border-top:1px solid #888583}.hide{display:none}.main h2{font-size:1.4em;text-align:left;margin:0 0 1em;padding:0 0 .3em}.main{position:relative}p.left{clear:left;float:left;width:20%;min-width:120px;max-width:300px;margin:0 0 .6em;padding:0;text-align:right}p.right,select{min-width:200px}p.right{overflow:auto;margin:0 0 .6em .4em;padding-left:.6em;text-align:left}p.center,p.spacer{padding:0;display:block}.footer,p.center{text-align:center}p.center{float:left;clear:both;margin:3em 0 3em 15%;width:70%}p.spacer{float:left;clear:both;margin:0;width:100%;height:20px}input{margin:0;border:0;color:#890101;outline:0;font:400 100% Cambria,Georgia,serif}input[type=text]{width:70%;min-width:200px;padding:0 5px}input[type=number]{min-width:50px;width:50px}input:focus{background-color:silver;color:#000}input[type=checkbox]{-webkit-appearance:none;background-color:#fafafa;border:1px solid #cacece;box-shadow:0 1px 2px rgba(0,0,0,.05),inset 0 -15px 10px -12px rgba(0,0,0,.05);padding:9px;border-radius:5px;display:inline-block;position:relative}input[type=checkbox]:active,input[type=checkbox]:checked:active{box-shadow:0 1px 2px rgba(0,0,0,.05),inset 0 1px 3px rgba(0,0,0,.1)}input[type=checkbox]:checked{background-color:#fafafa;border:1px solid #adb8c0;box-shadow:0 1px 2px rgba(0,0,0,.05),inset 0 -15px 10px -12px rgba(0,0,0,.05),inset 15px 10px -12px rgba(255,255,255,.1);color:#99a1a7}input[type=checkbox]:checked:after{content:'\\2714';font-size:14px;position:absolute;top:0;left:3px;color:#890101}input[type=button],input[type=file]+label{font:700 16px/1.2 'Book Antiqua','Palatino Linotype',Georgia,serif;margin:17px 0 0}input[type=button]{position:absolute;right:0;display:block;border:1px solid #adb8c0;float:right;border-radius:12px;padding:5px 20px 2px 23px;-webkit-transition-duration:.3s;transition-duration:.3s}input[type=button]:hover{background-color:#909090;color:#fff;padding:5px 62px 2px 65px}input.submit{float:left;position: relative}input.showMessage,input.showMessage:focus,input.showMessage:hover{background-color:#6F0;color:#000;padding:5px 62px 2px 65px}input[type=file]{width:.1px;height:.1px;opacity:0;overflow:hidden;position:absolute;z-index:-1}input[type=file]+label{float:left;clear:both;cursor:pointer;border:1px solid #adb8c0;border-radius:12px;padding:5px 20px 2px 23px;display:inline-block;background-color:#E4E4E4;color:#890101;overflow:hidden;-webkit-transition-duration:.3s;transition-duration:.3s}input[type=file]+label:hover,input[type=file]:focus+label{background-color:#909090;color:#fff;padding:5px 40px 2px 43px}input[type=file]+label svg{width:1em;height:1em;vertical-align:middle;fill:currentColor;margin-top:-.25em;margin-right:.25em}select{margin:0;border:0;background-color:#DADADA;color:#890101;outline:0;font:400 100% Cambria,Georgia,serif;width:50%;padding:0 5px}.footer{border-top:1px solid #888583;display:block;font-size:12px;margin-top:20px;padding:.7em 0 20px}.footer p{margin-bottom:.5em}@media (min-width:600px){.inner{min-width:600px}}@media (max-width:600px){.inner,.page{min-width:300px;width:100%;overflow-x:hidden}.footer,.main,.mast{float:left;width:100%}.mast{border-top:1px solid #888583;border-bottom:1px solid #888583}.main{margin-top:4px;width:98%}ul.nav{margin:0 auto;width:100%}ul.nav li{float:left;min-width:100px;width:33%}ul.nav a{font:12px Helvetica,Arial,sans-serif;letter-spacing:0;padding:.8em}.title,.title h1{padding:0;text-align:center}ul.nav a:focus,ul.nav a:hover{background-position:0 100%}.author{display:none}.title{border-bottom:1px solid #888583;width:100%;display:block;font:400 15px Baskerville,Garamond,Palatino,'Palatino Linotype','Hoefler Text','Times New Roman',serif}.title h1{font:600 15px Baskerville,Garamond,Palatino,'Palatino Linotype','Hoefler Text','Times New Roman',serif;display:inline}p.left,p.right{clear:both;float:left;margin-right:1em}li,li.first,li.last{border:0}p.left{width:100%;text-align:left;margin-left:.4em;font-weight:600}p.right{margin-left:1em;width:100%}p.center{margin:1em 0;width:100%}p.spacer{display:none}input[type=text],select{width:85%;}@media (min-width:1300px){.page{width:1300px}}";
static const char PROGMEM typeHTML[] = "text/html";
static const char PROGMEM typeCSS[] = "text/css";

static char wifiStatus[60] = "";
static bool isHotspot = false;
static uint32_t nextNodeReport = 0;
static char nodeError[ARTNET_NODE_REPORT_LENGTH] = "";
static bool nodeErrorShowing = 1;
static uint32_t nodeErrorTimeout = 0;
static bool pixDone = true;
static bool newDmxIn = false;
static bool doReboot = false;
static byte* dataIn = 0;

static void setStatusLed(uint8_t num, uint32_t col);
static void doStatusLedOutput();
static void wifiStart();
static void webStart();
static void artStart();
static void portSetup();
static void startHotspot();
static void doNodeReport();

void artnet_setup(void) {
  //pinMode(4, OUTPUT);
  //digitalWrite(4, LOW);

  // Make direction input to avoid boot garbage being sent out
  pinMode(DMX_DIR_A, OUTPUT);
  digitalWrite(DMX_DIR_A, LOW);
  #ifndef ONE_PORT
    pinMode(DMX_DIR_B, OUTPUT);
    digitalWrite(DMX_DIR_B, LOW);
  #endif

  #ifndef ESP_01
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);
    delay(1);
    setStatusLed(STATUS_LED_S, PINK);
    doStatusLedOutput();
  #endif

#ifndef ESP32
  wifi_set_sleep_type(NONE_SLEEP_T);
#endif  // #ifdef ESP32

  bool resetDefaults = false;
  
  #ifdef SETTINGS_RESET
    pinMode(SETTINGS_RESET, INPUT);

    delay(5);
    // button pressed = low reading
    if (!digitalRead(SETTINGS_RESET)) {
      delay(50);
      if (!digitalRead(SETTINGS_RESET))
        resetDefaults = true;
    }
  #endif
  
  // Start EEPROM
  EEPROM.begin(512);

  // Start SPIFFS file system
  SPIFFS.begin();

  // Check if SPIFFS formatted
  if (!SPIFFS.exists("/formatted.txt")) {
    SPIFFS.format();
    
    File f = SPIFFS.open("/formatted.txt", "w");
    f.print("Formatted");
    f.close();
  }

  // Load our saved values or store defaults
  if (!resetDefaults)
    eepromLoad();

  // Store our counters for resetting defaults
#if 0
#ifdef ESP32
  if (rtc_get_reset_reason(xPortGetCoreID()) != EXT_CPU_RESET && rtc_get_reset_reason(xPortGetCoreID()) != POWERON_RESET && rtc_get_reset_reason(xPortGetCoreID()) != SW_RESET)
#else  // #ifdef ESP32
  if (resetInfo.reason != REASON_DEFAULT_RST && resetInfo.reason != REASON_EXT_SYS_RST && resetInfo.reason != REASON_SOFT_RESTART)
#endif  // #ifdef ESP32
    deviceSettings.wdtCounter++;
  else
    deviceSettings.resetCounter++;
#endif

  // Store values
  eepromSave();

  // Start wifi
  wifiStart();

  // Start web server
  webStart();
  
  // Don't start our Artnet or DMX in firmware update mode or after multiple WDT resets
  if (!deviceSettings.doFirmwareUpdate && deviceSettings.wdtCounter <= 3) {

    // We only allow 1 DMX input - and RDM can't run alongside DMX in
    if (deviceSettings.portAmode == TYPE_DMX_IN && deviceSettings.portBmode == TYPE_RDM_OUT)
      deviceSettings.portBmode = TYPE_DMX_OUT;
    
    // Setup Artnet Ports & Callbacks
    artStart();

    // Don't open any ports for a bit to let the ESP spill it's garbage to serial
    while (millis() < 3500)
      yield();
    
    // Port Setup
    portSetup();

  } else
    deviceSettings.doFirmwareUpdate = false;

  delay(10);
}

void artnet_loop(void){
  // If the device lasts for 6 seconds, clear our reset timers
#if 0
  if (deviceSettings.resetCounter != 0 && millis() > 6000) {
    deviceSettings.resetCounter = 0;
    deviceSettings.wdtCounter = 0;
    eepromSave();
  }
#endif  // #if 0
  
  webServer.handleClient();

  // Get the node details and handle Artnet
  doNodeReport();
  artRDM.handler();

  yield();

  // DMX handlers
  dmxA.handler();
  #ifndef ONE_PORT
  dmxB.handler();
  #endif

  // Do Pixel FX on port A
  if (deviceSettings.portAmode == TYPE_WS2812 && deviceSettings.portApixMode != FX_MODE_PIXEL_MAP) {
    if (pixFXA.Update())
      pixDone = 0;
  }

  // Do Pixel FX on port B
  #ifndef ONE_PORT
    if (deviceSettings.portBmode == TYPE_WS2812 && deviceSettings.portBpixMode != FX_MODE_PIXEL_MAP) {
      if (pixFXB.Update())
        pixDone = 0;
    }
  #endif

  // Do pixel string output
  if (!pixDone)
    pixDone = pixDriver.show();
  
  // Handle received DMX
  if (newDmxIn) { 
    uint8_t g, p, n;
    
    newDmxIn = false;

    g = portA[0];
    p = portA[1];
    
    IPAddress bc = deviceSettings.dmxInBroadcast;
    artRDM.sendDMX(g, p, bc, dataIn, 512);

    #ifndef ESP_01
      setStatusLed(STATUS_LED_A, CYAN);
    #endif
  }

  // Handle rebooting the system
  if (doReboot) {
    static char c[ARTNET_NODE_REPORT_LENGTH] = "Device rebooting...";
    artRDM.setNodeReport(c, ARTNET_RC_POWER_OK);
    artRDM.artPollReply();
    
    // Ensure all web data is sent before we reboot
    uint32_t n = millis() + 1000;
    while (millis() < n)
      webServer.handleClient();

    ESP.restart();
  }
  
  #ifdef STATUS_LED_PIN
    // Output status to LEDs once per second
    if (statusTimer < millis()) {

      // Flash our main status LED
      if ((statusTimer % 2000) > 1000)
        setStatusLed(STATUS_LED_S, BLACK);
      else if (nodeError[0] != '\0')
        setStatusLed(STATUS_LED_S, RED);
      else
        setStatusLed(STATUS_LED_S, GREEN);

      doStatusLedOutput();
      statusTimer = millis() + 1000;
    }
  #endif
}

static void dmxHandle(uint8_t group, uint8_t port, uint16_t numChans, bool syncEnabled) {
  if (portA[0] == group) {
    if (deviceSettings.portAmode == TYPE_WS2812) {
      
      #ifndef ESP_01
        setStatusLed(STATUS_LED_A, GREEN);
      #endif
      
      if (deviceSettings.portApixMode == FX_MODE_PIXEL_MAP) {
        if (numChans > 510)
          numChans = 510;
        
        // Copy DMX data to the pixels buffer
        pixDriver.setBuffer(0, port * 510, artRDM.getDMX(group, port), numChans);
        
        // Output to pixel strip
        if (!syncEnabled)
          pixDone = false;

        return;

      // FX 12 Mode
      } else if (port == portA[1]) {
        byte* a = artRDM.getDMX(group, port);
        uint16_t s = deviceSettings.portApixFXstart - 1;
        
        pixFXA.Intensity = a[s + 0];
        pixFXA.setFX(a[s + 1]);
        pixFXA.setSpeed(a[s + 2]);
        pixFXA.Pos = a[s + 3];
        pixFXA.Size = a[s + 4];
        
        pixFXA.setColour1((a[s + 5] << 16) | (a[s + 6] << 8) | a[s + 7]);
        pixFXA.setColour2((a[s + 8] << 16) | (a[s + 9] << 8) | a[s + 10]);
        pixFXA.Size1 = a[s + 11];
        //pixFXA.Fade = a[s + 12];

        pixFXA.NewData = 1;
          
      }

    // DMX modes
    } else if (deviceSettings.portAmode != TYPE_DMX_IN && port == portA[1]) {
      dmxA.chanUpdate(numChans);
      
      #ifndef ESP_01
        setStatusLed(STATUS_LED_A, BLUE);
      #endif
    }
      

  #ifndef ONE_PORT
  } else if (portB[0] == group) {
    if (deviceSettings.portBmode == TYPE_WS2812) {
      setStatusLed(STATUS_LED_B, GREEN);
      
      if (deviceSettings.portBpixMode == FX_MODE_PIXEL_MAP) {
        if (numChans > 510)
          numChans = 510;
        
        // Copy DMX data to the pixels buffer
        pixDriver.setBuffer(1, port * 510, artRDM.getDMX(group, port), numChans);
        
        // Output to pixel strip
        if (!syncEnabled)
          pixDone = false;

        return;

      // FX 12 mode
      } else if (port == portB[1]) {
        byte* a = artRDM.getDMX(group, port);
        uint16_t s = deviceSettings.portBpixFXstart - 1;
        
        pixFXB.Intensity = a[s + 0];
        pixFXB.setFX(a[s + 1]);
        pixFXB.setSpeed(a[s + 2]);
        pixFXB.Pos = a[s + 3];
        pixFXB.Size = a[s + 4];
        pixFXB.setColour1((a[s + 5] << 16) | (a[s + 6] << 8) | a[s + 7]);
        pixFXB.setColour2((a[s + 8] << 16) | (a[s + 9] << 8) | a[s + 10]);
        pixFXB.Size1 = a[s + 11];
        //pixFXB.Fade = a[s + 12];

        pixFXB.NewData = 1;
      }
    } else if (deviceSettings.portBmode != TYPE_DMX_IN && port == portB[1]) {
      dmxB.chanUpdate(numChans);
      setStatusLed(STATUS_LED_B, BLUE);
    }
  #endif
  }

}

static void syncHandle() {
  if (deviceSettings.portAmode == TYPE_WS2812) {
    rdmPause(1);
    pixDone = pixDriver.show();
    rdmPause(0);
  } else if (deviceSettings.portAmode != TYPE_DMX_IN)
    dmxA.unPause();

  #ifndef ONE_PORT
    if (deviceSettings.portBmode == TYPE_WS2812) {
      rdmPause(1);
      pixDone = pixDriver.show();
      rdmPause(0);
    } else if (deviceSettings.portBmode != TYPE_DMX_IN)
      dmxB.unPause();
  #endif
}

static void ipHandle() {
  if (artRDM.getDHCP()) {
    deviceSettings.gateway = INADDR_NONE;
    
    deviceSettings.dhcpEnable = 1;
    doReboot = true;
    /*
    // Re-enable DHCP
    WiFi.begin(deviceSettings.wifiSSID, deviceSettings.wifiPass);

    // Wait for an IP
    while (WiFi.status() != WL_CONNECTED)
      yield();
    
    // Save settings to struct
    deviceSettings.ip = WiFi.localIP();
    deviceSettings.subnet = WiFi.subnetMask();
    deviceSettings.broadcast = {~deviceSettings.subnet[0] | (deviceSettings.ip[0] & deviceSettings.subnet[0]), ~deviceSettings.subnet[1] | (deviceSettings.ip[1] & deviceSettings.subnet[1]), ~deviceSettings.subnet[2] | (deviceSettings.ip[2] & deviceSettings.subnet[2]), ~deviceSettings.subnet[3] | (deviceSettings.ip[3] & deviceSettings.subnet[3])};

    // Pass IP to artRDM
    artRDM.setIP(deviceSettings.ip, deviceSettings.subnet);
    */
  
  } else {
    deviceSettings.ip = artRDM.getIP();
    deviceSettings.subnet = artRDM.getSubnetMask();
    deviceSettings.gateway = deviceSettings.ip;
    deviceSettings.gateway[3] = 1;
    deviceSettings.broadcast = {~deviceSettings.subnet[0] | (deviceSettings.ip[0] & deviceSettings.subnet[0]), ~deviceSettings.subnet[1] | (deviceSettings.ip[1] & deviceSettings.subnet[1]), ~deviceSettings.subnet[2] | (deviceSettings.ip[2] & deviceSettings.subnet[2]), ~deviceSettings.subnet[3] | (deviceSettings.ip[3] & deviceSettings.subnet[3])};
    deviceSettings.dhcpEnable = 0;
    
    doReboot = true;
    
    //WiFi.config(deviceSettings.ip,deviceSettings.ip,deviceSettings.ip,deviceSettings.subnet);
  }

  // Store everything to EEPROM
  eepromSave();
}

static void addressHandle() {
  memcpy(&deviceSettings.nodeName, artRDM.getShortName(), ARTNET_SHORT_NAME_LENGTH);
  memcpy(&deviceSettings.longName, artRDM.getLongName(), ARTNET_LONG_NAME_LENGTH);
  
  deviceSettings.portAnet = artRDM.getNet(portA[0]);
  deviceSettings.portAsub = artRDM.getSubNet(portA[0]);
  deviceSettings.portAuni[0] = artRDM.getUni(portA[0], portA[1]);
  deviceSettings.portAmerge = artRDM.getMerge(portA[0], portA[1]);

  if (artRDM.getE131(portA[0], portA[1]))
    deviceSettings.portAprot = PROT_ARTNET_SACN;
  else
    deviceSettings.portAprot = PROT_ARTNET;


  #ifndef ONE_PORT
    deviceSettings.portBnet = artRDM.getNet(portB[0]);
    deviceSettings.portBsub = artRDM.getSubNet(portB[0]);
    deviceSettings.portBuni[0] = artRDM.getUni(portB[0], portB[1]);
    deviceSettings.portBmerge = artRDM.getMerge(portB[0], portB[1]);
    
    if (artRDM.getE131(portB[0], portB[1]))
      deviceSettings.portBprot = PROT_ARTNET_SACN;
    else
      deviceSettings.portBprot = PROT_ARTNET;
  #endif
  
  // Store everything to EEPROM
  eepromSave();
}

static void rdmHandle(uint8_t group, uint8_t port, rdm_data* c) {
  if (portA[0] == group && portA[1] == port)
    dmxA.rdmSendCommand(c);

  #ifndef ONE_PORT
    else if (portB[0] == group && portB[1] == port)
      dmxB.rdmSendCommand(c);
  #endif
}

static void rdmReceivedA(rdm_data* c) {
  artRDM.rdmResponse(c, portA[0], portA[1]);
}

static void sendTodA() {
  artRDM.artTODData(portA[0], portA[1], dmxA.todMan(), dmxA.todDev(), dmxA.todCount(), dmxA.todStatus());
}

#ifndef ONE_PORT
static void rdmReceivedB(rdm_data* c) {
  artRDM.rdmResponse(c, portB[0], portB[1]);
}

static void sendTodB() {
  artRDM.artTODData(portB[0], portB[1], dmxB.todMan(), dmxB.todDev(), dmxB.todCount(), dmxB.todStatus());
}
#endif

static void todRequest(uint8_t group, uint8_t port) {
  if (portA[0] == group && portA[1] == port)
    sendTodA();

  #ifndef ONE_PORT
    else if (portB[0] == group && portB[1] == port)
      sendTodB();
  #endif
}

static void todFlush(uint8_t group, uint8_t port) {
  if (portA[0] == group && portA[1] == port)
    dmxA.rdmDiscovery();

  #ifndef ONE_PORT
    else if (portB[0] == group && portB[1] == port)
      dmxB.rdmDiscovery();
  #endif
}

static void dmxIn(uint16_t num) {
  // Double buffer switch
  byte* tmp = dataIn;
  dataIn = dmxA.getChans();
  dmxA.setBuffer(tmp);
  
  newDmxIn = true;
}

static void doStatusLedOutput() {
  uint8_t a[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};
  
  if (!statusLedsOff) {
    if (statusLedsDim) {
      for (uint8_t x = 0; x < 9; x++)
        a[x] = statusLedData[x] & STATUS_DIM;
    } else {
      for (uint8_t x = 0; x < 9; x++)
        a[x] = statusLedData[x];
    }
  }

  #ifdef STATUS_LED_MODE_APA106
    pixDriver.doAPA106(&a[0], STATUS_LED_PIN, 9);
  #endif

  #ifdef STATUS_LED_MODE_WS2812
    pixDriver.doPixel(&a[0], STATUS_LED_PIN, 9);
  #endif

  // Tint LEDs red slightly - they'll be changed back before being displayed if no errors
  for (uint8_t x = 1; x < 9; x += 3)
    statusLedData[x] = 125;
}

static void setStatusLed(uint8_t num, uint32_t col) {
  memcpy(&statusLedData[num*3], &col, 3);
}

static bool ajaxSave(uint8_t page, JsonObject& json);
static void ajaxLoad(uint8_t page, JsonObject& jsonReply);

static void ajaxHandle() {
  JsonObject& json = jsonBuffer.parseObject(webServer.arg("plain"));
  JsonObject& jsonReply = jsonBuffer.createObject();
  
  String reply;
  
  // Handle request to reboot into update mode
  if (json.containsKey("success") && json["success"] == 1 && json.containsKey("doUpdate")) {
    artRDM.end();
    
    jsonReply["success"] = 1;
    jsonReply["doUpdate"] = 1;
    
    jsonReply.printTo(reply);
    webServer.send(200, "application/json", reply);

    if (json["doUpdate"] == 1) {
      // Turn pixel strips off if they're on
      pixDriver.updateStrip(0, 0, deviceSettings.portApixConfig);
      pixDriver.updateStrip(1, 0, deviceSettings.portBpixConfig);
      
      deviceSettings.doFirmwareUpdate = true;
      eepromSave();
      
      doReboot = true;
    }
    
  // Handle load and save of data
  } else if (json.containsKey("success") && json["success"] == 1 && json.containsKey("page")) {
    if (ajaxSave((uint8_t)json["page"], json)) {
      ajaxLoad((uint8_t)json["page"], jsonReply);

      if (json.size() > 2)
        jsonReply["message"] = "Settings Saved";

    } else {
      jsonReply["success"] = 0;
      jsonReply["message"] = "Failed to save data.  Reload page and try again.";
    }
    
  // Handle reboots
  } else if (json.containsKey("success") && json.containsKey("reboot") && json["reboot"] == 1) {
    jsonReply["success"] = 1;
    jsonReply["message"] = "Device Restarting.";
    
    // Turn pixel strips off if they're on
    pixDriver.updateStrip(0, 0, deviceSettings.portApixConfig);
    pixDriver.updateStrip(1, 0, deviceSettings.portBpixConfig);
    
    doReboot = true;

  // Handle errors
  } 

  jsonReply.printTo(reply);
  webServer.send(200, "application/json", reply);
}

static bool ajaxSave(uint8_t page, JsonObject& json) {
  // This is a load request, not a save
  if (json.size() == 2)
    return true;
  
  switch (page) {
    case 1:     // Device Status
      // We don't need to save anything for this.  Go straight to load
      return true;
      break;

    case 2:     // Wifi
      json.get<String>("wifiSSID").toCharArray(deviceSettings.wifiSSID, 40);
      json.get<String>("wifiPass").toCharArray(deviceSettings.wifiPass, 40);
      json.get<String>("hotspotSSID").toCharArray(deviceSettings.hotspotSSID, 20);
      json.get<String>("hotspotPass").toCharArray(deviceSettings.hotspotPass, 20);
      deviceSettings.hotspotDelay = (uint8_t)json["hotspotDelay"];
      deviceSettings.standAloneEnable = (bool)json["standAloneEnable"];

      eepromSave();
      return true;
      break;

    case 3:     // IP Address & Node Name
      deviceSettings.ip = IPAddress(json["ipAddress"][0],json["ipAddress"][1],json["ipAddress"][2],json["ipAddress"][3]);
      deviceSettings.subnet = IPAddress(json["subAddress"][0],json["subAddress"][1],json["subAddress"][2],json["subAddress"][3]);
      deviceSettings.gateway = IPAddress(json["gwAddress"][0],json["gwAddress"][1],json["gwAddress"][2],json["gwAddress"][3]);
      deviceSettings.broadcast = uint32_t(deviceSettings.ip) | uint32_t(~uint32_t(deviceSettings.subnet));
      //deviceSettings.broadcast = {~deviceSettings.subnet[0] | (deviceSettings.ip[0] & deviceSettings.subnet[0]), ~deviceSettings.subnet[1] | (deviceSettings.ip[1] & deviceSettings.subnet[1]), ~deviceSettings.subnet[2] | (deviceSettings.ip[2] & deviceSettings.subnet[2]), ~deviceSettings.subnet[3] | (deviceSettings.ip[3] & deviceSettings.subnet[3])};

      json.get<String>("nodeName").toCharArray(deviceSettings.nodeName, 18);
      json.get<String>("longName").toCharArray(deviceSettings.longName, 64);

      if (!isHotspot && (bool)json["dhcpEnable"] != deviceSettings.dhcpEnable) {
        
        if ((bool)json["dhcpEnable"]) {
          /*
          // Re-enable DHCP
          WiFi.begin(deviceSettings.wifiSSID, deviceSettings.wifiPass);

          // Wait for an IP
          while (WiFi.status() != WL_CONNECTED)
            yield();
          
          // Save settings to struct
          deviceSettings.ip = WiFi.localIP();
          deviceSettings.subnet = WiFi.subnetMask();
          deviceSettings.broadcast = {~deviceSettings.subnet[0] | (deviceSettings.ip[0] & deviceSettings.subnet[0]), ~deviceSettings.subnet[1] | (deviceSettings.ip[1] & deviceSettings.subnet[1]), ~deviceSettings.subnet[2] | (deviceSettings.ip[2] & deviceSettings.subnet[2]), ~deviceSettings.subnet[3] | (deviceSettings.ip[3] & deviceSettings.subnet[3])};
          */
          
          deviceSettings.gateway = INADDR_NONE;
          
        }
        /*
        else {
          // Set static IP
          WiFi.config(deviceSettings.ip,deviceSettings.ip,deviceSettings.ip,deviceSettings.subnet);
        }

        // Add any changes to artnet settings - this will send correct artpollreply
        artRDM.setIP(deviceSettings.ip, deviceSettings.subnet);
        artRDM.setDHCP(deviceSettings.dhcpEnable);
        */

        doReboot = true;
      }

      if (!isHotspot) {
        artRDM.setShortName(deviceSettings.nodeName);
        artRDM.setLongName(deviceSettings.longName);
      }
      
      deviceSettings.dhcpEnable = (bool)json["dhcpEnable"];
      
      eepromSave();
      return true;
      break;

    case 4:     // Port A
      {
        deviceSettings.portAprot = (uint8_t)json["portAprot"];
        bool e131 = (deviceSettings.portAprot == PROT_ARTNET_SACN) ? true : false;
        
        deviceSettings.portAmerge = (uint8_t)json["portAmerge"];
  
        if ((uint8_t)json["portAnet"] < 128)
          deviceSettings.portAnet = (uint8_t)json["portAnet"];
  
        if ((uint8_t)json["portAsub"] < 16)
        deviceSettings.portAsub = (uint8_t)json["portAsub"];
  
        for (uint8_t x = 0; x < 4; x++) {
          if ((uint8_t)json["portAuni"][x] < 16)
            deviceSettings.portAuni[x] = (uint8_t)json["portAuni"][x];
          
          if ((uint16_t)json["portAsACNuni"][x] > 0 && (uint16_t)json["portAsACNuni"][x] < 64000)
            deviceSettings.portAsACNuni[x] = (uint16_t)json["portAsACNuni"][x];

          artRDM.setE131(portA[0], portA[x+1], e131);
          artRDM.setE131Uni(portA[0], portA[x+1], deviceSettings.portAsACNuni[x]);
        }
  
        uint8_t newMode = json["portAmode"];
        uint8_t oldMode = deviceSettings.portAmode;
        bool updatePorts = false;

        #ifndef ONE_PORT
          // RDM and DMX input can't run together
          if (newMode == TYPE_DMX_IN && deviceSettings.portBmode == TYPE_RDM_OUT) {
            deviceSettings.portBmode = TYPE_DMX_OUT;
            dmxB.rdmDisable();
          }
        #endif
        
        if (newMode == TYPE_DMX_IN && json.containsKey("dmxInBroadcast"))
          deviceSettings.dmxInBroadcast = IPAddress(json["dmxInBroadcast"][0],json["dmxInBroadcast"][1],json["dmxInBroadcast"][2],json["dmxInBroadcast"][3]);
         
        
        if (newMode != oldMode) {
  
          // Store the nem mode to settings
          deviceSettings.portAmode = newMode;

          doReboot = true;
          /*
          if (oldMode == TYPE_WS2812) {
            doReboot = true;
            
            // Set pixel strip length to zero
            pixDriver.updateStrip(0, 0, deviceSettings.portApixConfig);
  
            // Close ports from pixels - library handles if they dont exist
            for (uint8_t x = 2; x <= 4; x++)
              artRDM.closePort(portA[0], portA[x]);

            // Start our DMX port
            dmxA.begin(DMX_DIR_A, artRDM.getDMX(portA[0], portA[1]));
            
          } else if (oldMode == TYPE_DMX_IN)
            dmxA.dmxIn(false);
          else if (oldMode == TYPE_RDM_OUT)
            dmxA.rdmDisable();

          // Start DMX output with no DMX
          if (newMode == TYPE_DMX_OUT) {
            
            
            artRDM.setPortType(portA[0], portA[1], DMX_OUT);

          // Start DMX output with RDM
          } else if (newMode == TYPE_RDM_OUT) {
            dmxA.rdmEnable(ESTA_MAN, ESTA_DEV);
            dmxA.rdmSetCallBack(rdmReceivedA);
            dmxA.todSetCallBack(sendTodA);
            artRDM.setPortType(portA[0], portA[1], RDM_OUT);

          // Start DMX input
          } else if (newMode == TYPE_DMX_IN) {
            dmxA.dmxIn(true);
            dmxA.setInputCallback(dmxIn);
            
            artRDM.setPortType(portA[0], portA[1], DMX_IN);

          // Start WS2812 output
          } else if (newMode == TYPE_WS2812) {
            doReboot = true;

            dmxA.end();
            
            artRDM.setPortType(portA[0], portA[1], TYPE_DMX_OUT);
            updatePorts = true;
            
            // Initialize the pixel strip
            pixDriver.setStrip(0, DMX_TX_A, deviceSettings.portAnumPix, deviceSettings.portApixConfig);
            
          }
          */
        }
        
        // Update the Artnet class
        artRDM.setNet(portA[0], deviceSettings.portAnet);
        artRDM.setSubNet(portA[0], deviceSettings.portAsub);
        artRDM.setUni(portA[0], portA[1], deviceSettings.portAuni[0]);
        artRDM.setMerge(portA[0], portA[1], deviceSettings.portAmerge);

        // Lengthen or shorten our pixel strip & handle required Artnet ports
        if (newMode == TYPE_WS2812 && !doReboot) {
          // Get the new & old lengths of pixel strip
          uint16_t newLen = (json.containsKey("portAnumPix")) ? (uint16_t)json["portAnumPix"] : deviceSettings.portAnumPix;
          if (newLen > 680)
            newLen = 680;
          
          uint16_t oldLen = deviceSettings.portAnumPix;
          bool lenChanged = false;
  
          // If pixel size has changed
          if (newLen <= 680 && oldLen != newLen) {
            // Update our pixel strip
            deviceSettings.portAnumPix = newLen;
            pixDriver.updateStrip(1, deviceSettings.portAnumPix, deviceSettings.portApixConfig);
  
            lenChanged = true;

            // If the old mode was pixel map then update the Artnet ports
            if (deviceSettings.portApixMode == FX_MODE_PIXEL_MAP)
              updatePorts = true;
          }
            
          // If the old mode was 12 channel FX, update oldLen to represent the number of channels we used
          if (deviceSettings.portApixMode == FX_MODE_12)
            oldLen = 12;
  
          // If our mode changes then update the Artnet ports
          if (deviceSettings.portApixMode != (uint8_t)json["portApixMode"])
            updatePorts = true;
  
          // Store the new pixel mode
          deviceSettings.portApixMode = (uint8_t)json["portApixMode"];
  
          // If our new mode is FX12 then we need 12 channels & store the start address
          if (deviceSettings.portApixMode == FX_MODE_12) {
            if ((uint16_t)json["portApixFXstart"] <= 501 && (uint16_t)json["portApixFXstart"] > 0)
              deviceSettings.portApixFXstart = (uint16_t)json["portApixFXstart"];
            newLen = 12;
          }

          // If needed, open and close Artnet ports
          if (updatePorts) {
            for (uint8_t x = 1, y = 2; x < 4; x++, y++) {
              uint16_t c = (x * 170);
              if (newLen > c)
                portA[y] = artRDM.addPort(portA[0], x, deviceSettings.portAuni[x], TYPE_DMX_OUT, deviceSettings.portAmerge);
              else if (oldLen > c)
                artRDM.closePort(portA[0], portA[y]);
            }
          }
  
          // Set universe and merge settings (port 1 is done above for all port types)
          for (uint8_t x = 1, y = 2; x < 4; x++, y++) {
            if (newLen > (x * 170)) {
              artRDM.setUni(portA[0], portA[y], deviceSettings.portAuni[x]);
              artRDM.setMerge(portA[0], portA[y], deviceSettings.portAmerge);
            }
          }
        }
  
        artRDM.artPollReply();
  
        eepromSave();
        return true;
      }
      break;

    case 5:     // Port B
      #ifndef ONE_PORT
      {
        deviceSettings.portBprot = (uint8_t)json["portBprot"];
        bool e131 = (deviceSettings.portBprot == PROT_ARTNET_SACN) ? true : false;
        
        deviceSettings.portBmerge = (uint8_t)json["portBmerge"];
  
        if ((uint8_t)json["portBnet"] < 128)
          deviceSettings.portBnet = (uint8_t)json["portBnet"];
  
        if ((uint8_t)json["portBsub"] < 16)
        deviceSettings.portBsub = (uint8_t)json["portBsub"];
  
        for (uint8_t x = 0; x < 4; x++) {
          if ((uint8_t)json["portBuni"][x] < 16)
            deviceSettings.portBuni[x] = (uint8_t)json["portBuni"][x];
          
          if ((uint16_t)json["portBsACNuni"][x] > 0 && (uint16_t)json["portBsACNuni"][x] < 64000)
            deviceSettings.portBsACNuni[x] = (uint16_t)json["portBsACNuni"][x];

          artRDM.setE131(portB[0], portB[x+1], e131);
          artRDM.setE131Uni(portB[0], portB[x+1], deviceSettings.portBsACNuni[x]);
        }
  
        uint8_t newMode = json["portBmode"];
        uint8_t oldMode = deviceSettings.portBmode;
        bool updatePorts = false;
        
        // RDM and DMX input can't run together
        if (newMode == TYPE_RDM_OUT && deviceSettings.portAmode == TYPE_DMX_IN)
          newMode = TYPE_DMX_OUT;
        
        if (newMode != oldMode) {
          
          // Store the nem mode to settings
          deviceSettings.portBmode = newMode;

          doReboot = true;

          /*
          if (oldMode == TYPE_WS2812) {
            doReboot = true;
            
            // Set pixel strip length to zero
            pixDriver.updateStrip(1, 0, deviceSettings.portBpixConfig);
  
            // Close ports from pixels - library handles if they dont exist
            for (uint8_t x = 2; x <= 4; x++)
              artRDM.closePort(portB[0], portB[x]);

            // Start our DMX port
            dmxB.begin(DMX_DIR_B, artRDM.getDMX(portB[0], portB[1]));
            
            
          } else if (oldMode == TYPE_RDM_OUT)
            dmxB.rdmDisable();
          
          

          // Start DMX output with no DMX
          if (newMode == TYPE_DMX_OUT) {
            artRDM.setPortType(portB[0], portB[1], DMX_OUT);

          // Start DMX output with RDM
          } else if (newMode == TYPE_RDM_OUT) {
            dmxB.rdmEnable(ESTA_MAN, ESTA_DEV);
            dmxB.rdmSetCallBack(rdmReceivedB);
            dmxB.todSetCallBack(sendTodB);
            artRDM.setPortType(portB[0], portB[1], RDM_OUT);

          // Start WS2812 output
          } else if (newMode == TYPE_WS2812) {
            doReboot = true;

            
            //dmxB.end();
            artRDM.setPortType(portB[0], portB[1], TYPE_DMX_OUT);
            updatePorts = true;
            
            // Initialize the pixel strip
            pixDriver.setStrip(1, DMX_TX_B, deviceSettings.portBnumPix, deviceSettings.portBpixConfig);
            
          }
          */
        }
        
        // Update the Artnet class
        artRDM.setNet(portB[0], deviceSettings.portBnet);
        artRDM.setSubNet(portB[0], deviceSettings.portBsub);
        artRDM.setUni(portB[0], portB[1], deviceSettings.portBuni[0]);
        artRDM.setMerge(portB[0], portB[1], deviceSettings.portBmerge);

        // Lengthen or shorten our pixel strip & handle required Artnet ports
        if (newMode == TYPE_WS2812 && !doReboot) {
          // Get the new & old lengths of pixel strip
          uint16_t newLen = (json.containsKey("portBnumPix")) ? (uint16_t)json["portBnumPix"] : deviceSettings.portBnumPix;
          if (newLen > 680)
            newLen = 680;
          
          uint16_t oldLen = deviceSettings.portBnumPix;
          bool lenChanged = false;
  
          // If pixel size has changed
          if (newLen <= 680 && oldLen != newLen) {
            // Update our pixel strip
            deviceSettings.portBnumPix = newLen;
            pixDriver.updateStrip(1, deviceSettings.portBnumPix, deviceSettings.portBpixConfig);
  
            lenChanged = true;

            // If the old mode was pixel map then update the Artnet ports
            if (deviceSettings.portBpixMode == FX_MODE_PIXEL_MAP)
              updatePorts = true;
          }
            
          // If the old mode was 12 channel FX, update oldLen to represent the number of channels we used
          if (deviceSettings.portBpixMode == FX_MODE_12)
            oldLen = 12;
  
          // If our mode changes then update the Artnet ports
          if (deviceSettings.portBpixMode != (uint8_t)json["portBpixMode"])
            updatePorts = true;
  
          // Store the new pixel mode
          deviceSettings.portBpixMode = (uint8_t)json["portBpixMode"];
  
          // If our new mode is FX12 then we need 12 channels & store the start address
          if (deviceSettings.portBpixMode == FX_MODE_12) {
            if ((uint16_t)json["portBpixFXstart"] <= 501 && (uint16_t)json["portBpixFXstart"] > 0)
              deviceSettings.portBpixFXstart = (uint16_t)json["portBpixFXstart"];
            newLen = 12;
          }

          // If needed, open and close Artnet ports
          if (updatePorts) {
            for (uint8_t x = 1, y = 2; x < 4; x++, y++) {
              uint16_t c = (x * 170);
              if (newLen > c)
                portB[y] = artRDM.addPort(portB[0], x, deviceSettings.portBuni[x], TYPE_DMX_OUT, deviceSettings.portBmerge);
              else if (oldLen > c)
                artRDM.closePort(portB[0], portB[y]);
            }
          }
  
          // Set universe and merge settings (port 1 is done above for all port types)
          for (uint8_t x = 1, y = 2; x < 4; x++, y++) {
            if (newLen > (x * 170)) {
              artRDM.setUni(portB[0], portB[y], deviceSettings.portBuni[x]);
              artRDM.setMerge(portB[0], portB[y], deviceSettings.portBmerge);
            }
          }
        }
  
        artRDM.artPollReply();
  
        eepromSave();
        return true;
      }
      #endif
      break;

    case 6:     // Scenes
      // Not yet implemented
      
      return true;
      break;

    case 7:     // Firmware
      // Doesn't come here
      
      break;

    default:
      // Catch errors
      return false;
  }
}

static void ajaxLoad(uint8_t page, JsonObject& jsonReply) {

  // Create the needed arrays here - doesn't work within the switch below
  JsonArray& ipAddress = jsonReply.createNestedArray("ipAddress");
  JsonArray& subAddress = jsonReply.createNestedArray("subAddress");
  JsonArray& gwAddress = jsonReply.createNestedArray("gwAddress");
  JsonArray& bcAddress = jsonReply.createNestedArray("bcAddress");
  JsonArray& portAuni = jsonReply.createNestedArray("portAuni");
  JsonArray& portBuni = jsonReply.createNestedArray("portBuni");
  JsonArray& portAsACNuni = jsonReply.createNestedArray("portAsACNuni");
  JsonArray& portBsACNuni = jsonReply.createNestedArray("portBsACNuni");
  JsonArray& dmxInBroadcast = jsonReply.createNestedArray("dmxInBroadcast");

  // Get MAC Address
  char MAC_char[30] = "";
  sprintf(MAC_char, "%02X", MAC_array[0]);
  for (int i = 1; i < 6; ++i)
    sprintf(MAC_char, "%s:%02X", MAC_char, MAC_array[i]);
  
  jsonReply["macAddress"] = String(MAC_char);

  switch (page) {
    case 1:     // Device Status
      jsonReply.remove("ipAddress");
      jsonReply.remove("subAddress");
      jsonReply.remove("gwAddress");
      jsonReply.remove("bcAddress");
      jsonReply.remove("portAuni");
      jsonReply.remove("portBuni");
      jsonReply.remove("portAsACNuni");
      jsonReply.remove("portBsACNuni");
      jsonReply.remove("dmxInBroadcast");
      
      jsonReply["nodeName"] = deviceSettings.nodeName;
      jsonReply["wifiStatus"] = wifiStatus;
      
      if (isHotspot) {
        jsonReply["ipAddressT"] = deviceSettings.hotspotIp.toString();
        jsonReply["subAddressT"] = deviceSettings.hotspotSubnet.toString();
      } else {
        jsonReply["ipAddressT"] = deviceSettings.ip.toString();
        jsonReply["subAddressT"] = deviceSettings.subnet.toString();
      }

      if (isHotspot && !deviceSettings.standAloneEnable) {
        jsonReply["portAStatus"] = "Disabled in hotspot mode";
        jsonReply["portBStatus"] = "Disabled in hotspot mode";
      } else {
        switch (deviceSettings.portAmode) {
          case TYPE_DMX_OUT:
            jsonReply["portAStatus"] = "DMX output";
            break;
  
          case TYPE_RDM_OUT:
            jsonReply["portAStatus"] = "DMX/RDM output";
            break;
  
          case TYPE_DMX_IN:
            jsonReply["portAStatus"] = "DMX input";
            break;
  
          case TYPE_WS2812:
            jsonReply["portAStatus"] = "WS2812 mode";
            break;
        }
        switch (deviceSettings.portBmode) {
          case TYPE_DMX_OUT:
            jsonReply["portBStatus"] = "DMX output";
            break;
  
          case TYPE_RDM_OUT:
            jsonReply["portBStatus"] = "DMX/RDM output";
            break;
  
          case TYPE_DMX_IN:
            jsonReply["portBStatus"] = "DMX input";
            break;
  
          case TYPE_WS2812:
            jsonReply["portBStatus"] = "WS2812 mode";
            break;
        }
      }
      
      jsonReply["sceneStatus"] = "Not outputting<br />0 Scenes Recorded<br />0 of 250KB used";
      jsonReply["firmwareStatus"] = FIRMWARE_VERSION;

      jsonReply["success"] = 1;
      break;

    case 2:     // Wifi
      jsonReply.remove("ipAddress");
      jsonReply.remove("subAddress");
      jsonReply.remove("gwAddress");
      jsonReply.remove("bcAddress");
      jsonReply.remove("portAuni");
      jsonReply.remove("portBuni");
      jsonReply.remove("portAsACNuni");
      jsonReply.remove("portBsACNuni");
      jsonReply.remove("dmxInBroadcast");
      
      jsonReply["wifiSSID"] = deviceSettings.wifiSSID;
      jsonReply["wifiPass"] = deviceSettings.wifiPass;
      jsonReply["hotspotSSID"] = deviceSettings.hotspotSSID;
      jsonReply["hotspotPass"] = deviceSettings.hotspotPass;
      jsonReply["hotspotDelay"] = deviceSettings.hotspotDelay;
      jsonReply["standAloneEnable"] = deviceSettings.standAloneEnable;
      
      jsonReply["success"] = 1;
      break;

    case 3:     // IP Address & Node Name
      jsonReply.remove("portAuni");
      jsonReply.remove("portBuni");
      jsonReply.remove("portAsACNuni");
      jsonReply.remove("portBsACNuni");
      jsonReply.remove("dmxInBroadcast");
      
      jsonReply["dhcpEnable"] = deviceSettings.dhcpEnable;

      for (uint8_t x = 0; x < 4; x++) {
        ipAddress.add(deviceSettings.ip[x]);
        subAddress.add(deviceSettings.subnet[x]);
        gwAddress.add(deviceSettings.gateway[x]);
        bcAddress.add(deviceSettings.broadcast[x]);
      }

      jsonReply["nodeName"] = deviceSettings.nodeName;
      jsonReply["longName"] = deviceSettings.longName;

      jsonReply["success"] = 1;
      break;

    case 4:     // Port A
      jsonReply.remove("ipAddress");
      jsonReply.remove("subAddress");
      jsonReply.remove("gwAddress");
      jsonReply.remove("bcAddress");
      jsonReply.remove("portBuni");
      jsonReply.remove("portBsACNuni");
      
      jsonReply["portAmode"] = deviceSettings.portAmode;

      // Only Artnet supported for receiving right now
      if (deviceSettings.portAmode == TYPE_DMX_IN)
        jsonReply["portAprot"] = PROT_ARTNET;
      else
        jsonReply["portAprot"] = deviceSettings.portAprot;
 
      jsonReply["portAmerge"] = deviceSettings.portAmerge;
      jsonReply["portAnet"] = deviceSettings.portAnet;
      jsonReply["portAsub"] = deviceSettings.portAsub;
      jsonReply["portAnumPix"] = deviceSettings.portAnumPix;
      
      jsonReply["portApixMode"] = deviceSettings.portApixMode;
      jsonReply["portApixFXstart"] = deviceSettings.portApixFXstart;

      for (uint8_t x = 0; x < 4; x++) {
        portAuni.add(deviceSettings.portAuni[x]);
        portAsACNuni.add(deviceSettings.portAsACNuni[x]);
        dmxInBroadcast.add(deviceSettings.dmxInBroadcast[x]);
      }

      jsonReply["success"] = 1;
      break;

    case 5:     // Port B
      jsonReply.remove("ipAddress");
      jsonReply.remove("subAddress");
      jsonReply.remove("gwAddress");
      jsonReply.remove("bcAddress");
      jsonReply.remove("portAuni");
      jsonReply.remove("portAsACNuni");
      
      jsonReply["portBmode"] = deviceSettings.portBmode;
      jsonReply["portBprot"] = deviceSettings.portBprot;
      jsonReply["portBmerge"] = deviceSettings.portBmerge;
      jsonReply["portBnet"] = deviceSettings.portBnet;
      jsonReply["portBsub"] = deviceSettings.portBsub;
      jsonReply["portBnumPix"] = deviceSettings.portBnumPix;
      
      jsonReply["portBpixMode"] = deviceSettings.portBpixMode;
      jsonReply["portBpixFXstart"] = deviceSettings.portBpixFXstart;

      for (uint8_t x = 0; x < 4; x++) {
        portBuni.add(deviceSettings.portBuni[x]);
        portBsACNuni.add(deviceSettings.portBsACNuni[x]);
        dmxInBroadcast.add(deviceSettings.dmxInBroadcast[x]);
      }
      
      jsonReply["success"] = 1;
      break;

    case 6:     // Scenes
      jsonReply.remove("ipAddress");
      jsonReply.remove("subAddress");
      jsonReply.remove("gwAddress");
      jsonReply.remove("bcAddress");
      jsonReply.remove("portAuni");
      jsonReply.remove("portBuni");
      jsonReply.remove("portAsACNuni");
      jsonReply.remove("portBsACNuni");
      jsonReply.remove("dmxInBroadcast");
      

      jsonReply["success"] = 1;
      break;

    case 7:     // Firmware
      jsonReply.remove("ipAddress");
      jsonReply.remove("subAddress");
      jsonReply.remove("gwAddress");
      jsonReply.remove("bcAddress");
      jsonReply.remove("portAuni");
      jsonReply.remove("portBuni");
      jsonReply.remove("portAsACNuni");
      jsonReply.remove("portBsACNuni");
      jsonReply.remove("dmxInBroadcast");
      
      jsonReply["firmwareStatus"] = FIRMWARE_VERSION;
      jsonReply["success"] = 1;
      break;

    default:
      jsonReply.remove("ipAddress");
      jsonReply.remove("subAddress");
      jsonReply.remove("gwAddress");
      jsonReply.remove("bcAddress");
      jsonReply.remove("portAuni");
      jsonReply.remove("portBuni");
      jsonReply.remove("portAsACNuni");
      jsonReply.remove("portBsACNuni");
      jsonReply.remove("dmxInBroadcast");
      
      jsonReply["success"] = 0;
      jsonReply["message"] = "Invalid or incomplete data received.";
  }
}

/* webFirmwareUpdate()
 *  display update status after firmware upload and restart
 */
static void webFirmwareUpdate() {
  // Generate the webpage from the variables above
  String fail = "{\"success\":0,\"message\":\"Unknown Error\"}";
  String ok = "{\"success\":1,\"message\":\"Success: Device restarting\"}";

  // Send to the client
  webServer.sendHeader("Connection", "close");
  webServer.sendHeader("Access-Control-Allow-Origin", "*");
  webServer.send(200, "application/json", (Update.hasError()) ? fail : ok);

  doReboot = true;
}

/* webFirmwareUpload()
 *  handle firmware upload and update
 */
static void webFirmwareUpload() {
  String reply = "";
  HTTPUpload& upload = webServer.upload();
    
  if(upload.status == UPLOAD_FILE_START){
    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if(!Update.begin(maxSketchSpace)){//start with max available size
      reply = "{\"success\":0,\"message\":\"Insufficient space.\"}";
    }
  } else if(upload.status == UPLOAD_FILE_WRITE){
    if(Update.write(upload.buf, upload.currentSize) != upload.currentSize){
      reply = "{\"success\":0,\"message\":\"Failed to save\"}";
    }
    
  } else if(upload.status == UPLOAD_FILE_END){
    if(Update.end(true)){ //true to set the size to the current progress
      reply = "{\"success\":1,\"message\":\"Success: Device Restarting\"}";
    } else {
      reply = "{\"success\":0,\"message\":\"Unknown Error\"}";
    }
  }
  yield();
  
  // Send to the client
  if (reply.length() > 0) {
    webServer.sendHeader("Connection", "close");
    webServer.sendHeader("Access-Control-Allow-Origin", "*");
    webServer.send(200, "application/json", reply);
  }
}

static void doNodeReport() {
  if (nextNodeReport > millis())
    return;
  
  static char c[128];

  if (nodeErrorTimeout > millis())
    nextNodeReport = millis() + 2000;
  else
    nextNodeReport = millis() + 5000;
  
  if (nodeError[0] != '\0' && !nodeErrorShowing && nodeErrorTimeout > millis()) {
    
    nodeErrorShowing = true;
    strcpy(c, nodeError);
    
  } else {
    nodeErrorShowing = false;
    
    strcpy(c, "OK: PortA:");

    switch (deviceSettings.portAmode) {
      case TYPE_DMX_OUT:
        sprintf(c, "%s DMX Out", c);
        break;
      
      case TYPE_RDM_OUT:
        sprintf(c, "%s RDM Out", c);
        break;
      
      case TYPE_DMX_IN:
        sprintf(c, "%s DMX In", c);
        break;
      
      case TYPE_WS2812:
        if (deviceSettings.portApixMode == FX_MODE_12)
            sprintf(c, "%s 12chan", c);
          sprintf(c, "%s WS2812 %ipixels", c, deviceSettings.portAnumPix);
        break;
    }
  
    #ifndef ONE_PORT
      sprintf(c, "%s. PortB:", c);
      
      switch (deviceSettings.portBmode) {
        case TYPE_DMX_OUT:
          sprintf(c, "%s DMX Out", c);
          break;
        
        case TYPE_RDM_OUT:
          sprintf(c, "%s RDM Out", c);
          break;
        
        case TYPE_WS2812:
          if (deviceSettings.portBpixMode == FX_MODE_12)
            sprintf(c, "%s 12chan", c);
          sprintf(c, "%s WS2812 %ipixels", c, deviceSettings.portBnumPix);
          break;
      }
    #endif
  }
  
  artRDM.setNodeReport(c, ARTNET_RC_POWER_OK);
}

static void portSetup() {
  if (deviceSettings.portAmode == TYPE_DMX_OUT || deviceSettings.portAmode == TYPE_RDM_OUT) {
    #ifndef ESP_01
      setStatusLed(STATUS_LED_A, BLUE);
    #endif
    
    dmxA.begin(DMX_DIR_A, artRDM.getDMX(portA[0], portA[1]));
    if (deviceSettings.portAmode == TYPE_RDM_OUT && !dmxA.rdmEnabled()) {
      dmxA.rdmEnable(ESTA_MAN, ESTA_DEV);
      dmxA.rdmSetCallBack(rdmReceivedA);
      dmxA.todSetCallBack(sendTodA);
    }

  } else if (deviceSettings.portAmode == TYPE_DMX_IN) {
    #ifndef ESP_01
      setStatusLed(STATUS_LED_A, CYAN);
    #endif
    
    dmxA.begin(DMX_DIR_A, artRDM.getDMX(portA[0], portA[1]));
    dmxA.dmxIn(true);
    dmxA.setInputCallback(dmxIn);

    dataIn = (byte*) os_malloc(sizeof(byte) * 512);
    memset(dataIn, 0, 512);

  } else if (deviceSettings.portAmode == TYPE_WS2812) {
    #ifndef ESP_01
      setStatusLed(STATUS_LED_A, GREEN);
    #endif
    
    digitalWrite(DMX_DIR_A, HIGH);
    pixDriver.setStrip(0, DMX_TX_A, deviceSettings.portAnumPix, deviceSettings.portApixConfig);
  }
  
  #ifndef ONE_PORT
    if (deviceSettings.portBmode == TYPE_DMX_OUT || deviceSettings.portBmode == TYPE_RDM_OUT) {
      setStatusLed(STATUS_LED_B, BLUE);
      
      dmxB.begin(DMX_DIR_B, artRDM.getDMX(portB[0], portB[1]));
      if (deviceSettings.portBmode == TYPE_RDM_OUT && !dmxB.rdmEnabled()) {
        dmxB.rdmEnable(ESTA_MAN, ESTA_DEV);
        dmxB.rdmSetCallBack(rdmReceivedB);
        dmxB.todSetCallBack(sendTodB);
      }
      
    } else if (deviceSettings.portBmode == TYPE_WS2812) {
      setStatusLed(STATUS_LED_B, GREEN);
      
      digitalWrite(DMX_DIR_B, HIGH);
      pixDriver.setStrip(1, DMX_TX_B, deviceSettings.portBnumPix, deviceSettings.portBpixConfig);
    }
  #endif

  pixDriver.allowInterruptSingle = WS2812_ALLOW_INT_SINGLE;
  pixDriver.allowInterruptDouble = WS2812_ALLOW_INT_DOUBLE;
}

static void artStart() {
  // Initialise out ArtNet
  if (isHotspot)
    artRDM.init(deviceSettings.hotspotIp, deviceSettings.hotspotSubnet, true, deviceSettings.nodeName, deviceSettings.longName, ARTNET_OEM, ESTA_MAN, MAC_array);
  else
    artRDM.init(deviceSettings.ip, deviceSettings.subnet, deviceSettings.dhcpEnable, deviceSettings.nodeName, deviceSettings.longName, ARTNET_OEM, ESTA_MAN, MAC_array);

  // Set firmware
  artRDM.setFirmwareVersion(ART_FIRM_VERSION);

  // Add Group
  portA[0] = artRDM.addGroup(deviceSettings.portAnet, deviceSettings.portAsub);
  
  bool e131 = (deviceSettings.portAprot == PROT_ARTNET_SACN) ? true : false;

  // WS2812 uses TYPE_DMX_OUT - the rest use the value assigned
  if (deviceSettings.portAmode == TYPE_WS2812)
    portA[1] = artRDM.addPort(portA[0], 0, deviceSettings.portAuni[0], TYPE_DMX_OUT, deviceSettings.portAmerge);
  else
    portA[1] = artRDM.addPort(portA[0], 0, deviceSettings.portAuni[0], deviceSettings.portAmode, deviceSettings.portAmerge);

  artRDM.setE131(portA[0], portA[1], e131);
  artRDM.setE131Uni(portA[0], portA[1], deviceSettings.portAsACNuni[0]);
  
  // Add extra Artnet ports for WS2812
  if (deviceSettings.portAmode == TYPE_WS2812 && deviceSettings.portApixMode == FX_MODE_PIXEL_MAP) {
    if (deviceSettings.portAnumPix > 170) {
      portA[2] = artRDM.addPort(portA[0], 1, deviceSettings.portAuni[1], TYPE_DMX_OUT, deviceSettings.portAmerge);
      
      artRDM.setE131(portA[0], portA[2], e131);
      artRDM.setE131Uni(portA[0], portA[2], deviceSettings.portAsACNuni[1]);
    }
    if (deviceSettings.portAnumPix > 340) {
      portA[3] = artRDM.addPort(portA[0], 2, deviceSettings.portAuni[2], TYPE_DMX_OUT, deviceSettings.portAmerge);
      
      artRDM.setE131(portA[0], portA[3], e131);
      artRDM.setE131Uni(portA[0], portA[3], deviceSettings.portAsACNuni[2]);
    }
    if (deviceSettings.portAnumPix > 510) {
      portA[4] = artRDM.addPort(portA[0], 3, deviceSettings.portAuni[3], TYPE_DMX_OUT, deviceSettings.portAmerge);
      
      artRDM.setE131(portA[0], portA[4], e131);
      artRDM.setE131Uni(portA[0], portA[4], deviceSettings.portAsACNuni[3]);
    }
  }


  #ifndef ONE_PORT
    // Add Group
    portB[0] = artRDM.addGroup(deviceSettings.portBnet, deviceSettings.portBsub);
    e131 = (deviceSettings.portBprot == PROT_ARTNET_SACN) ? true : false;
    
    // WS2812 uses TYPE_DMX_OUT - the rest use the value assigned
    if (deviceSettings.portBmode == TYPE_WS2812)
      portB[1] = artRDM.addPort(portB[0], 0, deviceSettings.portBuni[0], TYPE_DMX_OUT, deviceSettings.portBmerge);
    else
      portB[1] = artRDM.addPort(portB[0], 0, deviceSettings.portBuni[0], deviceSettings.portBmode, deviceSettings.portBmerge);

    artRDM.setE131(portB[0], portB[1], e131);
    artRDM.setE131Uni(portB[0], portB[1], deviceSettings.portBsACNuni[0]);
  
    // Add extra Artnet ports for WS2812
    if (deviceSettings.portBmode == TYPE_WS2812 && deviceSettings.portBpixMode == FX_MODE_PIXEL_MAP) {
      if (deviceSettings.portBnumPix > 170) {
        portB[2] = artRDM.addPort(portB[0], 1, deviceSettings.portBuni[1], TYPE_DMX_OUT, deviceSettings.portBmerge);
        
        artRDM.setE131(portB[0], portB[2], e131);
        artRDM.setE131Uni(portB[0], portB[2], deviceSettings.portBsACNuni[1]);
      }
      if (deviceSettings.portBnumPix > 340) {
        portB[3] = artRDM.addPort(portB[0], 2, deviceSettings.portBuni[2], TYPE_DMX_OUT, deviceSettings.portBmerge);
        
        artRDM.setE131(portB[0], portB[3], e131);
        artRDM.setE131Uni(portB[0], portB[3], deviceSettings.portBsACNuni[2]);
      }
      if (deviceSettings.portBnumPix > 510) {
        portB[4] = artRDM.addPort(portB[0], 3, deviceSettings.portBuni[3], TYPE_DMX_OUT, deviceSettings.portBmerge);
        
        artRDM.setE131(portB[0], portB[4], e131);
        artRDM.setE131Uni(portB[0], portB[4], deviceSettings.portBsACNuni[3]);
      }
    }
  #endif

  // Add required callback functions
  artRDM.setArtDMXCallback(dmxHandle);
  artRDM.setArtRDMCallback(rdmHandle);
  artRDM.setArtSyncCallback(syncHandle);
  artRDM.setArtIPCallback(ipHandle);
  artRDM.setArtAddressCallback(addressHandle);
  artRDM.setTODRequestCallback(todRequest);
  artRDM.setTODFlushCallback(todFlush);

#ifdef ESP32
  switch (rtc_get_reset_reason(xPortGetCoreID())) {
  case NO_MEAN:
  case POWERON_RESET:
  case SW_RESET:
  case EXT_CPU_RESET:
    artRDM.setNodeReport("OK: Device 1 started", ARTNET_RC_POWER_OK);
    nextNodeReport = millis() + 4000;
    break;

  case OWDT_RESET:
  case TG0WDT_SYS_RESET:
  case TG1WDT_SYS_RESET:
  case RTCWDT_SYS_RESET:
  case INTRUSION_RESET:
  case TGWDT_CPU_RESET:
  case RTCWDT_CPU_RESET:
  case RTCWDT_BROWN_OUT_RESET:
  case RTCWDT_RTC_RESET:
    artRDM.setNodeReport("ERROR: (WDT) Unexpected device restart", ARTNET_RC_POWER_FAIL);
    strcpy(nodeError, "Restart error: WDT");
    nextNodeReport = millis() + 10000;
    nodeErrorTimeout = millis() + 30000;
    break;

  case SDIO_RESET:
      artRDM.setNodeReport("ERROR: (SDIO) Unexpected device restart", ARTNET_RC_POWER_FAIL);
      strcpy(nodeError, "Restart error: EXCP");
      nextNodeReport = millis() + 10000;
      nodeErrorTimeout = millis() + 30000;
    break;
    
  case DEEPSLEEP_RESET:
    break;
  }
#else  // #ifdef ESP32
  switch (resetInfo.reason) {
    case REASON_DEFAULT_RST:  // normal start
    case REASON_EXT_SYS_RST:
    case REASON_SOFT_RESTART:
      artRDM.setNodeReport("OK: Device started", ARTNET_RC_POWER_OK);
      nextNodeReport = millis() + 4000;
      break;
      
    case REASON_WDT_RST:
      artRDM.setNodeReport("ERROR: (HWDT) Unexpected device restart", ARTNET_RC_POWER_FAIL);
      strcpy(nodeError, "Restart error: HWDT");
      nextNodeReport = millis() + 10000;
      nodeErrorTimeout = millis() + 30000;
      break;
    case REASON_EXCEPTION_RST:
      artRDM.setNodeReport("ERROR: (EXCP) Unexpected device restart", ARTNET_RC_POWER_FAIL);
      strcpy(nodeError, "Restart error: EXCP");
      nextNodeReport = millis() + 10000;
      nodeErrorTimeout = millis() + 30000;
      break;
    case REASON_SOFT_WDT_RST:
      artRDM.setNodeReport("ERROR: (SWDT) Unexpected device restart", ARTNET_RC_POWER_FAIL);
      strcpy(nodeError, "Error on Restart: SWDT");
      nextNodeReport = millis() + 10000;
      nodeErrorTimeout = millis() + 30000;
      break;
    case REASON_DEEP_SLEEP_AWAKE:
      // not used
      break;
  }
#endif  // #ifdef ESP32
  
  // Start artnet
  artRDM.begin();

  yield();
}

static void webStart() {
  webServer.on("/", [](){
    artRDM.pause();
    webServer.send_P(200, typeHTML, mainPage);
    webServer.sendHeader("Connection", "close");
    yield();
    artRDM.begin();
  });
  
  webServer.on("/style.css", [](){
    artRDM.pause();
    // If no style.css in SPIFFS, send default
    if (!SPIFFS.exists("/style.css"))
      webServer.send_P(200, typeCSS, css);
    else {
      File f = SPIFFS.open("/style.css", "r");
      size_t sent = webServer.streamFile(f, typeCSS);    
      f.close();
    }
    webServer.sendHeader("Connection", "close");
    yield();
    artRDM.begin();
  });
  
  webServer.on("/ajax", HTTP_POST, ajaxHandle);
  
  webServer.on("/upload", HTTP_POST, webFirmwareUpdate, webFirmwareUpload);

  webServer.on("/style", [](){
    webServer.send_P(200, typeHTML, cssUploadPage);
    webServer.sendHeader("Connection", "close");
  });
  
  webServer.on("/style_delete", [](){
    if (SPIFFS.exists("/style.css"))
      SPIFFS.remove("/style.css");
        
    webServer.send(200, "text/plain", "style.css deleted.  The default style is now in use.");
    webServer.sendHeader("Connection", "close");
  });

  webServer.on("/style_upload", HTTP_POST, [](){
    webServer.send(200, "text/plain", "Upload successful!");
  }, [](){
    HTTPUpload& upload = webServer.upload();
    
    if(upload.status == UPLOAD_FILE_START){
      String filename = upload.filename;
      if(!filename.startsWith("/")) filename = "/"+filename;
      fsUploadFile = SPIFFS.open(filename, "w");
      filename = String();
      
    } else if(upload.status == UPLOAD_FILE_WRITE){
      if(fsUploadFile)
        fsUploadFile.write(upload.buf, upload.currentSize);
        
    } else if(upload.status == UPLOAD_FILE_END){
      if(fsUploadFile) {
        fsUploadFile.close();
        
        if (upload.filename != "/style.css")
          SPIFFS.rename(upload.filename, "/style.css");
      }
    }
  });
  
  webServer.onNotFound([]() {
    webServer.send(404, "text/plain", "Page not found");
  });
  
  webServer.begin();
  
  yield();
}

static void wifiStart() {
  // If it's the default WiFi SSID, make it unique
  if (strcmp(deviceSettings.hotspotSSID, "espArtNetNode") == 0 || deviceSettings.hotspotSSID[0] == '\0')

#ifdef ESP32
    sprintf(deviceSettings.hotspotSSID, "espArtNetNode_%05u", ((ESP.getEfuseMac() >> 32) & 0xFFFF));
#else  // #ifdef ESP32
    sprintf(deviceSettings.hotspotSSID, "espArtNetNode_%05u", (ESP.getChipId() & 0xFF));
#endif  // #ifdef ESP32
  
  if (deviceSettings.standAloneEnable) {
    startHotspot();

    deviceSettings.ip = deviceSettings.hotspotIp;
    deviceSettings.subnet = deviceSettings.hotspotSubnet;
    deviceSettings.broadcast = {~deviceSettings.subnet[0] | (deviceSettings.ip[0] & deviceSettings.subnet[0]), ~deviceSettings.subnet[1] | (deviceSettings.ip[1] & deviceSettings.subnet[1]), ~deviceSettings.subnet[2] | (deviceSettings.ip[2] & deviceSettings.subnet[2]), ~deviceSettings.subnet[3] | (deviceSettings.ip[3] & deviceSettings.subnet[3])};
  
    return;
  }
  
  if (deviceSettings.wifiSSID[0] != '\0') {
    WiFi.begin(deviceSettings.wifiSSID, deviceSettings.wifiPass);
    WiFi.mode(WIFI_STA);

#ifdef ESP32
    WiFi.setHostname(deviceSettings.nodeName);
#else  // #ifdef ESP32
    WiFi.hostname(deviceSettings.nodeName);
#endif  // #ifdef ESP32
    
    unsigned long endTime = millis() + (deviceSettings.hotspotDelay * 1000);

    if (deviceSettings.dhcpEnable) {
      while (WiFi.status() != WL_CONNECTED && endTime > millis())
        yield();

      if (millis() >= endTime)
        startHotspot();
      
      deviceSettings.ip = WiFi.localIP();
      deviceSettings.subnet = WiFi.subnetMask();

      if (deviceSettings.gateway == INADDR_NONE)
        deviceSettings.gateway = WiFi.gatewayIP();
      
      deviceSettings.broadcast = {~deviceSettings.subnet[0] | (deviceSettings.ip[0] & deviceSettings.subnet[0]), ~deviceSettings.subnet[1] | (deviceSettings.ip[1] & deviceSettings.subnet[1]), ~deviceSettings.subnet[2] | (deviceSettings.ip[2] & deviceSettings.subnet[2]), ~deviceSettings.subnet[3] | (deviceSettings.ip[3] & deviceSettings.subnet[3])};
    } else
      WiFi.config(deviceSettings.ip, deviceSettings.gateway, deviceSettings.subnet);

    //sprintf(wifiStatus, "Wifi connected.  Signal: %ld<br />SSID: %s", WiFi.RSSI(), deviceSettings.wifiSSID);
    sprintf(wifiStatus, "Wifi connected.<br />SSID: %s", deviceSettings.wifiSSID);
    WiFi.macAddress(MAC_array);
    
  } else
    startHotspot();
  
  yield();
}

static void startHotspot() {
  yield();
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP(deviceSettings.hotspotSSID, deviceSettings.hotspotPass);
  WiFi.softAPConfig(deviceSettings.hotspotIp, deviceSettings.hotspotIp, deviceSettings.hotspotSubnet);

  sprintf(wifiStatus, "No Wifi. Hotspot started.<br />\nHotspot SSID: %s", deviceSettings.hotspotSSID);
  WiFi.macAddress(MAC_array);

  isHotspot = true;
  
  if (deviceSettings.standAloneEnable)
    return;
  
  webStart();

  unsigned long endTime = millis() + 30000;

  // Stay here if not in stand alone mode - no dmx or artnet
  while (endTime > millis() || 
#ifdef ESP32
    WiFi.softAPgetStationNum() > 0
#else  // #ifdef ESP32
    wifi_softap_get_station_num() > 0
#endif  // #ifdef ESP32
    ) {
    webServer.handleClient();
    yield();
  }

  ESP.restart();
  isHotspot = false;
}
