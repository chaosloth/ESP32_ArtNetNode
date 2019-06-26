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

//
// esptool.py --chip esp32 --port COM<??????> write_flash -z 0x1000 -b 5 ArtNetNode.ino.esp32-poe.bin
//

#include <Arduino.h>

#include "serialLEDDriver.h"
#include "wsFX.h"
#include "espDMX_RDM.h"
#include "espArtNetRDM.h"

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <EEPROM.h>
#include <Update.h>

#define ETH_CLK_MODE ETH_CLOCK_GPIO17_OUT
#define ETH_PHY_POWER 12

#include <ETH.h>
#include <FS.h>
#include <SPIFFS.h>

#include <ArduinoJson.h>

#include <rom/rtc.h>

#define CONFIG_VERSION "303"
#define FIRMWARE_VERSION "3.0.3"
#define ART_FIRM_VERSION 0x0303   // Firmware given over Artnet (2 uint8_ts)

#define ARTNET_OEM 0x0123     // Artnet OEM Code
#define ESTA_MAN 0x555F       // ESTA Manufacturer Code
#define ESTA_DEV 0xEE000000   // RDM Device ID (used with Man Code to make 48bit UID)

//
// Unchangable pins in ESP32:
//
// GPIO1  -> U0TXD -> DMX uart port A pin
// GPIO10 -> U1TXD -> DMX uart port B pin
// Direction pins for DMX can be anything, set below
//
// GPIO23 -> VSPID -> serial LED output port A pin
// GPIO18 -> VSPICLK -> serial LED output point A clock pin (for APA102)
// GPIO13 -> HSPID -> serial LED output port B pin
// GPIO14 -> HSPICLK -> serial LED output point B clock pin (for APA102)
//

//
// Customizable pins:
//
#define DMX_DIR_A       0     // DMX data UART Direction port A pin 
#define DMX_DIR_B       39    // DMX data UART Direction port B pin

//#define NO_RESET            // Un comment to disable the reset button
#ifndef NO_RESET
#define SETTINGS_RESET  34    // GPIO34 is a user button on Olimex ESP32-PoE
#endif

//
// Olimex ESP32-PoE notes:
//
// Port A serial LED is not accessible! (Unless you don't use ethernet and solder directly the ESP32-WROOM module)
//
// Port B serial LED output pin is: EXT2 pin 10
// Port B serial CLK output pin is: EXT2 pin 9 (for APA102)
//
// Port A DMX uart pin is: EXT1 pin 6 (GPIO1)
// Port A DMX dir pin is: EXT1 pin 5 (GPIO0), set DMX_DIR_A to 0 above
//
// Port B DMX uart is not accessible! (Unless you solder directly to the ESP32-WROOM module)
//
// So ideal setup is to set Port A as DMX and port B and serial LED
//

static uint8_t portA[5] = { 0 };
static uint8_t portB[5] = { 0 };
static uint8_t MAC_array[6] = { 0 };

static serialLEDDriver pixDriver;
static espArtNetRDM artRDM;

static WebServer webServer(80);
static DynamicJsonDocument jsonDocument(65536);

static File fsUploadFile;

static pixPatterns pixFXA(0, &pixDriver);
static pixPatterns pixFXB(1, &pixDriver);

static const char PROGMEM mainPage[] = "<!DOCTYPE html><meta content='text/html; charset=utf-8' http-equiv=Content-Type /><title>ESP32 ArtNetNode Config</title><meta content='Matthew Tong - http://github.com/mtongnz/' name=DC.creator /><meta content=en name=DC.language /><meta content='width=device-width,initial-scale=1' name=viewport /><link href=style.css rel=stylesheet /><div id=page><div class=inner><div class=mast><div class=title>ESP32<h1>ArtNet & sACN</h1>to<h1>DMX & LED Pixels</h1></div><ul class=nav><li class=first><a href='javascript: menuClick(1)'>Device Status</a><li><a href='javascript: menuClick(2)'>Network</a><li><a href='javascript: menuClick(3)'>IP & Name</a><li><a href='javascript: menuClick(4)'>Port A</a><li><a href='javascript: menuClick(5)'>Port B</a><li><a href='javascript: menuClick(6)'>Scenes</a><li><a href='javascript: menuClick(7)'>Firmware</a><li class=last><a href='javascript: reboot()'>Reboot</a></ul><div class=author><i>Design by</i> Matthew Tong</div></div><div class='main section'><div class=hide name=error><h2>Error</h2><p class=center>There was an error communicating with the device. Refresh the page and try again.</div><div class=show name=sections><h2>Fetching Data</h2><p class=center>Fetching data from device. If this message is still here in 15 seconds, try refreshing the page or clicking the menu option again.</div><div class=hide name=sections><h2>Device Status</h2><p class=left>Device Name:<p class=right name=nodeName><p class=left>MAC Address:<p class=right name=macAddress><p class=left>Network Status:<p class=right name=wifiStatus><p class=left>IP Address:<p class=right name=ipAddressT><p class=left>Subnet Address:<p class=right name=subAddressT><p class=left>Port A:<p class=right name=portAStatus><p class=left>Port A LED type:<p class=right name=portApixConfig><p class=left>Port B:<p class=right name=portBStatus><p class=left>Port B LED type:<p class=right name=portBpixConfig><p class=left>Scene Storage:<p class=right name=sceneStatus><p class=left>Firmware:<p class=right name=firmwareStatus></div><div class=hide name=sections><input name=save type=button value='Save Changes'/><h2>Network Settings</h2><p class=left>MAC Address:<p class=right name=macAddress><p class=spacer><p class=left>Wifi SSID:<p class=right><input type=text name=wifiSSID /><p class=left>Password:<p class=right><input type=text name=wifiPass /><p class=spacer><p class=left>Hotspot SSID:<p class=right><input type='text' name='hotspotSSID' /><p class=left>Password:<p class=right><input type=text name=hotspotPass /><p class=left>Start Delay:<p class=right><input name=hotspotDelay type=number min=0 max=180 class=number /> (seconds)<p class=spacer><p class=left>Stand Alone:<p class=right><input name=standAloneEnable type=checkbox value=true /><p class=left>Ethernet:<p class=right><input name=ethernetEnable type=checkbox value=true /><p class=right>In normal mode, the hotspot will start after <i>delay</i> seconds if the main WiFi won't connect. If no users connect, the device will reset and attempt the main WiFi again. This feature is purely for changing settings and ArtNet data is ignored.<p class=right>Stand alone mode disables the primary WiFi connection and allows ArtNet data to be received via the hotspot connection.</div><div class=hide name=sections><input name=save type=button value='Save Changes'/><h2>IP & Node Name</h2><p class='left'>Short Name:</p><p class=right><input type=text name=nodeName /><p class=left>Long Name:<p class=right><input type=text name=longName /><p class=spacer><p class=left>Enable DHCP:<p class=right><input name=dhcpEnable type=checkbox value=true /><p class=left>IP Address:<p class=right><input name=ipAddress type=number min=0 max=255 class=number /> <input name=ipAddress type=number min=0 max=255 class=number /> <input name=ipAddress type=number min=0 max=255 class=number /> <input name=ipAddress type=number min=0 max=255 class=number /><p class=left>Subnet Address:<p class=right><input name=subAddress type=number min=0 max=255 class=number /> <input name=subAddress type=number min=0 max=255 class=number /> <input name=subAddress type=number min=0 max=255 class=number /> <input name=subAddress type=number min=0 max=255 class=number /><p class=left>Gateway Address:<p class=right><input name=gwAddress type=number min=0 max=255 class=number /> <input name=gwAddress type=number min=0 max=255 class=number /> <input name=gwAddress type=number min=0 max=255 class=number /> <input name=gwAddress type=number min=0 max=255 class=number /><p class=left>Broadcast Address:<p class=right name=bcAddress><p class=center>These settings only affect the main WiFi connection. The hotspot will always have DHCP enabled and an IP of <b>2.0.0.1</b></div><div class=hide name=sections><input name=save type=button value='Save Changes'/><h2>Port A Settings</h2><p class=left>Port Type:<p class=right><select class=select name=portAmode><option value=0>DMX Output<option value=1>DMX Output with RDM<option value=2>DMX Input<option value=3>LED Pixels - WS2812</select><p class=left>Protocol:<p class=right><select class=select name=portAprot><option value=0>Artnet v4<option value=1>Artnet v4 with sACN DMX</select><p class=left>Merge Mode:<p class=right><select class=select name=portAmerge><option value=0>Merge LTP<option value=1>Merge HTP</select><p class=left>LED Type:<p class=right><select class=select name=portApixConfig><option value=0>WS2812 RGB<option value=1>WS2812 RGBW<option value=2>WS2812 RGBW Split W<option value=3>APA102 RGBB</select><p class=left>Net:<p class=right><input name=portAnet type=number min=0 max=127 class=number /><p class=left>Subnet:<p class=right><input name=portAsub type=number min=0 max=15 class=number /><p class=left>Universe:<p class=right><input type=number min=0 max=15 name=portAuni class=number /><span name=portApix> <input type=number min=0 max=15 name=portAuni class=number /> <input type=number min=0 max=15 name=portAuni class=number /> <input type=number min=0 max=15 name=portAuni class=number /></span></p><p class=left>sACN Universe:<p class=right><input type=number min=0 max=63999 name=portAsACNuni class=number /> <input type=number min=1 max=63999 name=portAsACNuni class=number /> <input type=number min=1 max=63999 name=portAsACNuni class=number /> <input type=number min=1 max=63999 name=portAsACNuni class=number /></p><span name=DmxInBcAddrA><p class=left>Broadcast Address:<p class=right><input type=number min=0 max=255 name=dmxInBroadcast class=number /> <input type=number min=0 max=255 name=dmxInBroadcast class=number /> <input type=number min=0 max=255 name=dmxInBroadcast class=number /> <input type=number min=0 max=255 name=dmxInBroadcast class=number /></p></span><span name=portApix><p class=left>Number of Pixels:<p class=right><input type=number min=0 max=512 name=portAnumPix class=number /> 512 max - 128 per universe</p><p class=left>Mode:</p><p class=right><select class=select name=portApixMode><option value=0>Pixel Mapping</option><option value=1>12 Channel FX</option></select><p class=left>Start Channel:</p><p class=right><input type=number name=portApixFXstart class=number min=1 max=501 /> FX modes only</p></span></div><div class=hide name=sections><input name=save type=button value='Save Changes'/><h2>Port B Settings</h2><p class=left>Port Type:<p class=right><select class=select name=portBmode><option value=0>DMX Output<option value=1>DMX Output with RDM<option value=3>LED Pixels - WS2812</select><p class=left>Protocol:<p class=right><select class=select name=portBprot><option value=0>Artnet v4<option value=1>Artnet v4 with sACN DMX</select><p class=left>Merge Mode:<p class=right><select class=select name=portBmerge><option value=0>Merge LTP<option value=1>Merge HTP</select><p class=left>LED Type:<p class=right><select class=select name=portBpixConfig><option value=0>WS2812 RGB<option value=1>WS2812 RGBW<option value=2>WS2812 RGBW Split W<option value=3>APA102 RGBB</select><p class=left>Net:<p class=right><input name=portBnet type=number min=0 max=127 class=number /><p class=left>Subnet:<p class=right><input name=portBsub type=number min=0 max=15 class=number /><p class=left>Universe:<p class=right><input type=number min=0 max=15 name=portBuni class=number /><span name=portBpix> <input type=number min=0 max=15 name=portBuni class=number /> <input type=number min=0 max=15 name=portBuni class=number /> <input type=number min=0 max=15 name=portBuni class=number /></span></p><p class=left>sACN Universe:<p class=right><input type=number min=1 max=63999 name=portBsACNuni class=number /> <input type=number min=1 max=63999 name=portBsACNuni class=number /> <input type=number min=1 max=63999 name=portBsACNuni class=number /> <input type=number min=1 max=63999 name=portBsACNuni class=number /></p><span name=portBpix><p class=left>Number of Pixels:<p class=right><input type=number min=0 max=512 name=portBnumPix class=number /> 512 max - 170 per universe</p><p class=left>Mode:</p><p class=right><select class=select name=portBpixMode><option value=0>Pixel Mapping</option><option value=1>12 Channel FX</option></select><p class=left>Start Channel:</p><p class=right><input type=number name=portBpixFXstart class=number min=1 max=501 /> FX modes only</p></span></div><div class=hide name=sections><h2>Stored Scenes</h2><p class=center>Not yet implemented</div><div class=hide name=sections><form action=/update enctype=multipart/form-data method=POST id=firmForm><h2>Update Firmware</h2><p class=left>Firmware:<p class=right name=firmwareStatus><p class=right><input name=update type=file id=update><label for=update><svg height=17 viewBox='0 0 20 17' width=20 xmlns=http://www.w3.org/2000/svg><path d='M10 0l-5.2 4.9h3.3v5.1h3.8v-5.1h3.3l-5.2-4.9zm9.3 11.5l-3.2-2.1h-2l3.4 2.6h-3.5c-.1 0-.2.1-.2.1l-.8 2.3h-6l-.8-2.2c-.1-.1-.1-.2-.2-.2h-3.6l3.4-2.6h-2l-3.2 2.1c-.4.3-.7 1-.6 1.5l.6 3.1c.1.5.7.9 1.2.9h16.3c.6 0 1.1-.4 1.3-.9l.6-3.1c.1-.5-.2-1.2-.7-1.5z'/></svg> <span>Choose Firmware</span></label><p class=right id=uploadMsg></p><p class=right><input type=button class=submit value='Upload Now' id=fUp></div></div><div class=footer><p>Coding and hardware © 2016-2017 <a href=http://github.com/mtongnz/ >Matthew Tong</a>.<p>Released under <a href=http://www.gnu.org/licenses/ >GNU General Public License V3</a>.</div></div></div><script>var cl=0;var num=0;var err=0;var o=document.getElementsByName('sections');var s=document.getElementsByName('save');for (var i=0, e; e=s[i++];)e.addEventListener( 'click', function(){sendData();}); var u=document.getElementById('fUp');var um=document.getElementById('uploadMsg');var fileSelect=document.getElementById('update');u.addEventListener('click',function(){uploadPrep()});function uploadPrep(){if(fileSelect.files.length===0) return;u.disabled=!0;u.value='Preparing Device…';var x=new XMLHttpRequest();x.onreadystatechange=function(){if(x.readyState==XMLHttpRequest.DONE){try{var r=JSON.parse(x.response)}catch(e){var r={success:0,doUpdate:1}} if(r.success==1&&r.doUpdate==1){uploadWait()}else{um.value='<b>Update failed!</b>';u.value='Upload Now';u.disabled=!1}}};x.open('POST','/ajax',!0);x.setRequestHeader('Content-Type','application/json');x.send('{\"doUpdate\":1,\"success\":1}')} function uploadWait(){setTimeout(function(){var z=new XMLHttpRequest();z.onreadystatechange=function(){if(z.readyState==XMLHttpRequest.DONE){try{var r=JSON.parse(z.response)}catch(e){var r={success:0}} console.log('r=' + r.success); if(r.success==1){upload()}else{uploadWait()}}};z.open('POST','/ajax',!0);z.setRequestHeader('Content-Type','application/json');z.send('{\"doUpdate\":2,\"success\":1}')},1000)} var upload=function(){u.value='Uploading… 0%';var data=new FormData();data.append('update',fileSelect.files[0]);var x=new XMLHttpRequest();x.onreadystatechange=function(){if(x.readyState==4){try{var r=JSON.parse(x.response)}catch(e){var r={success:0,message:'No response from device.'}} console.log(r.success+': '+r.message);if(r.success==1){u.value=r.message;setTimeout(function(){location.reload()},15000)}else{um.value='<b>Update failed!</b> '+r.message;u.value='Upload Now';u.disabled=!1}}};x.upload.addEventListener('progress',function(e){var p=Math.ceil((e.loaded/e.total)*100);console.log('Progress: '+p+'%');if(p<100) u.value='Uploading... '+p+'%';else u.value='Upload complete. Processing…'},!1);x.open('POST','/upload',!0);x.send(data)}; function reboot() { if (err == 1) return; var r = confirm('Are you sure you want to reboot?'); if (r != true) return; o[cl].className = 'hide'; o[0].childNodes[0].innerHTML = 'Rebooting'; o[0].childNodes[1].innerHTML = 'Please wait while the device reboots. This page will refresh shortly unless you changed the IP or Wifi.'; o[0].className = 'show'; err = 0; var x = new XMLHttpRequest(); x.onreadystatechange = function(){ if(x.readyState == 4){ try { var r = JSON.parse(x.response); } catch (e){ var r = {success: 0, message: 'Unknown error: [' + x.responseText + ']'}; } if (r.success != 1) { o[0].childNodes[0].innerHTML = 'Reboot Failed'; o[0].childNodes[1].innerHTML = 'Something went wrong and the device didn\\'t respond correctly. Please try again.'; } setTimeout(function() { location.reload(); }, 5000); } }; x.open('POST', '/ajax', true); x.setRequestHeader('Content-Type', 'application/json'); x.send('{\"reboot\":1,\"success\":1}'); } function sendData(){var d={'page':num};for (var i=0, e; e=o[cl].getElementsByTagName('INPUT')[i++];){var k=e.getAttribute('name');var v=e.value;if (k in d) continue; if (k=='ipAddress' || k=='subAddress' || k=='gwAddress' || k=='portAuni' || k=='portBuni' || k=='portAsACNuni' || k=='portBsACNuni' || k=='dmxInBroadcast'){var c=[v];for (var z=1; z < 4; z++){c.push(o[cl].getElementsByTagName('INPUT')[i++].value);}d[k]=c; continue;}if (e.type==='text')d[k]=v;if (e.type==='number'){if (v=='')v=0;d[k]=v;}if (e.type==='checkbox'){if (e.checked)d[k]=1;else d[k]=0;}}for (var i=0, e; e=o[cl].getElementsByTagName('SELECT')[i++];){d[e.getAttribute('name')]=e.options[e.selectedIndex].value;}d['success']=1;var x=new XMLHttpRequest();x.onreadystatechange=function(){handleAJAX(x);};x.open('POST', '/ajax');x.setRequestHeader('Content-Type', 'application/json');x.send(JSON.stringify(d));console.log(d);} function menuClick(n){if (err==1) return; num=n; setTimeout(function(){if (cl==num || err==1) return; o[cl].className='hide'; o[0].className='show'; cl=0;}, 100); var x=new XMLHttpRequest(); x.onreadystatechange=function(){handleAJAX(x);}; x.open('POST', '/ajax'); x.setRequestHeader('Content-Type', 'application/json'); x.send(JSON.stringify({\"page\":num,\"success\":1}));}function handleAJAX(x){if (x.readyState==XMLHttpRequest.DONE ){if (x.status==200){var response=JSON.parse(x.responseText);console.log(response);if (!response.hasOwnProperty('success')){err=1; o[cl].className='hide'; document.getElementsByName('error')[0].className='show';return;}if (response['success'] !=1){err=1; o[cl].className='hide';document.getElementsByName('error')[0].getElementsByTagName('P')[0].innerHTML=response['message']; document.getElementsByName('error')[0].className='show';return;}if (response.hasOwnProperty('message')) { for (var i = 0, e; e = s[i++];) { e.value = response['message']; e.className = 'showMessage' } setTimeout(function() { for (var i = 0, e; e = s[i++];) { e.value = 'Save Changes'; e.className = '' } }, 5000); } o[cl].className='hide'; o[num].className='show'; cl=num; for (var key in response){if (response.hasOwnProperty(key)){var a=document.getElementsByName(key); if (key=='ipAddress' || key=='subAddress'){var b=document.getElementsByName(key + 'T'); for (var z=0; z < 4; z++){a[z].value=response[key][z]; if (z==0) b[0].innerHTML=''; else b[0].innerHTML=b[0].innerHTML + ' . '; b[0].innerHTML=b[0].innerHTML + response[key][z];}continue;}else if (key=='bcAddress'){for (var z=0; z < 4; z++){if (z==0) a[0].innerHTML=''; else a[0].innerHTML=a[0].innerHTML + ' . '; a[0].innerHTML=a[0].innerHTML + response[key][z];}continue;} else if (key=='gwAddress' || key=='dmxInBroadcast' || key=='portAuni' || key=='portBuni' || key=='portAsACNuni' || key=='portBsACNuni'){for(var z=0;z<4;z++){a[z].value = response[key][z];}continue}if(key=='portAmode'){var b = document.getElementsByName('portApix');var c = document.getElementsByName('DmxInBcAddrA');if(response[key] == 3) {b[0].style.display = '';b[1].style.display = '';} else {b[0].style.display = 'none';b[1].style.display = 'none';}if (response[key] == 2){c[0].style.display = '';}else{c[0].style.display = 'none';}} else if (key == 'portBmode') {var b = document.getElementsByName('portBpix');if(response[key] == 3) {b[0].style.display = '';b[1].style.display = '';} else {b[0].style.display = 'none';b[1].style.display = 'none';}}for (var z=0; z < a.length; z++){switch (a[z].nodeName){case 'P': case 'DIV': a[z].innerHTML=response[key]; break; case 'INPUT': if (a[z].type=='checkbox'){if (response[key]==1) a[z].checked=true; else a[z].checked=false;}else a[z].value=response[key]; break; case 'SELECT': for (var y=0; y < a[z].options.length; y++){if (a[z].options[y].value==response[key]){a[z].options.selectedIndex=y; break;}}break;}}}}}else{err=1; o[cl].className='hide'; document.getElementsByName('error')[0].className='show';}}}var update=document.getElementById('update');var label=update.nextElementSibling;var labelVal=label.innerHTML;update.addEventListener( 'change', function( e ){var fileName=e.target.value.split( '\\\\' ).pop(); if( fileName ) label.querySelector( 'span' ).innerHTML=fileName; else label.innerHTML=labelVal; update.blur();}); document.onkeydown=function(e){if(cl < 2 || cl > 6)return; var e = e||window.event; if (e.keyCode == 13)sendData();}; menuClick(1);</script></body></html>";
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
static uint8_t* dataIn = 0;

static void wifiStart();
static void webStart();
static void artStart();
static void portSetup();
static void startHotspot();
static void doNodeReport();

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

} deviceSettings = {

  CONFIG_VERSION,

  // The default values
  IPAddress(2, 0, 0, 1),       // ip
  IPAddress(255, 0, 0, 0),     // subnet
  IPAddress(2, 0, 0, 1),       // gateway
  IPAddress(2, 255, 255, 255), // broadcast
  IPAddress(2, 0, 0, 1),       // hotspotIP
  IPAddress(255, 0, 0, 0),     // hotspotSubnet
  IPAddress(2, 255, 255, 255), // hotspotBroadcast
  IPAddress(2, 255, 255, 255), // dmxInBroadcast

  true,                        // dhcpEnable
  false,                       // standAloneEnable
  true,                        // ethernetEnable

  "espArtNetNode",             // nodeName
  "espArtNetNode",             // longName
  "",                          // wifiSSID
  "",                          // wifiPass
  "espArtNetNode",             // hotspotSSID
  "1234567890123",             // hotspotPass
  15,                          // hotspotDelay

  TYPE_DMX_OUT,                // portAmode
  TYPE_SERIAL_LED,             // portBmode
  PROT_ARTNET,                 // portAprot
  PROT_ARTNET,                 // portBprot
  MERGE_HTP,                   // portAmerge
  MERGE_HTP,                   // portBmerge

  0,                           // portAnet
  0,                           // portAsub
  {4, 5, 6, 7},                // portAuni[4]

  0,                           // portBnet
  0,                           // portBsub
  {0, 1, 2, 3},                // portBuni[4]

  {5, 6, 7, 8},                // portAsACNuni[4]
  {1, 2, 3, 4},                // portBsACNuni[4]

  72,                          // portAnumPix
  72,                          // portBnumPix

  WS2812_RGBW_SPLIT,            // portApixConfig
  WS2812_RGBW_SPLIT,           // portBpixConfig

  false,                       // doFirmwareUpdate

  FX_MODE_PIXEL_MAP,           // portApixMode
  FX_MODE_PIXEL_MAP,           // portBpixMode

  1,                           // portApixFXstart
  1,                           // portBpixFXstart
};

static void eepromSave() {
  for (uint16_t t = 0; t < sizeof(deviceSettings); t++) {
    EEPROM.write(t, *((char*)&deviceSettings + t));
  }
  EEPROM.commit();
}

static void eepromLoad() {
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

    // If config files dont match, save defaults
  } else {
    eepromSave();
    delay(500);
  }
}

void setup(void) {
  // Make direction input to avoid boot garbage being sent out
  pinMode(DMX_DIR_A, OUTPUT);
  digitalWrite(DMX_DIR_A, LOW);
  pinMode(DMX_DIR_B, OUTPUT);
  digitalWrite(DMX_DIR_B, LOW);

  bool resetDefaults = false;

#ifdef SETTINGS_RESET
  pinMode(SETTINGS_RESET, INPUT);
  delay(5);
  // button pressed = low reading
  if (!digitalRead(SETTINGS_RESET)) {
    delay(50);
    if (!digitalRead(SETTINGS_RESET)) {
      resetDefaults = true;
    }
  }
#endif  // #ifdef SETTINGS_RESET

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
  if (!resetDefaults) {
    eepromLoad();
  }

  // Store values
  eepromSave();

  // Start wifi
  wifiStart();

  // Start web server
  webStart();

  // Don't start our Artnet or DMX in firmware update mode or after multiple WDT resets
  if (!deviceSettings.doFirmwareUpdate) {

    // We only allow 1 DMX input - and RDM can't run alongside DMX in
    if (deviceSettings.portAmode == TYPE_DMX_IN && deviceSettings.portBmode == TYPE_RDM_OUT) {
      deviceSettings.portBmode = TYPE_DMX_OUT;
    }

    // Setup Artnet Ports & Callbacks
    artStart();

    // Don't open any ports for a bit to let the ESP spill it's garbage to serial
    while (millis() < 3500) {
      yield();
    }

    // Port Setup
    portSetup();

  } else {
    deviceSettings.doFirmwareUpdate = false;
  }

  delay(10);
}

void loop(void) {
  webServer.handleClient();

  // Get the node details and handle Artnet
  doNodeReport();
  artRDM.handler();

  yield();

  // DMX handlers
  dmxA.handler();
  dmxB.handler();

  // Do Pixel FX on port A
  if (deviceSettings.portAmode == TYPE_SERIAL_LED && deviceSettings.portApixMode != FX_MODE_PIXEL_MAP) {
    if (pixFXA.Update()) {
      pixDone = 0;
    }
  }

  // Do Pixel FX on port B
  if (deviceSettings.portBmode == TYPE_SERIAL_LED && deviceSettings.portBpixMode != FX_MODE_PIXEL_MAP) {
    if (pixFXB.Update()) {
      pixDone = 0;
    }
  }

  // Do pixel string output
  if (!pixDone) {
    pixDone = pixDriver.show();
  }

  // Handle received DMX
  if (newDmxIn) {
    uint8_t g, p;

    newDmxIn = false;

    g = portA[0];
    p = portA[1];

    IPAddress bc = deviceSettings.dmxInBroadcast;
    artRDM.sendDMX(g, p, bc, dataIn, 512);
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
}

static void dmxHandle(uint8_t group, uint8_t port, uint16_t numChans, bool syncEnabled) {
  if (portA[0] == group) {
    if (deviceSettings.portAmode == TYPE_SERIAL_LED) {

      if (deviceSettings.portApixMode == FX_MODE_PIXEL_MAP) {
        switch (deviceSettings.portApixConfig) {
          case WS2812_RGB:
            if (numChans > 510) {
              numChans = 510;  
            }
            // Copy DMX data to the pixels buffer
            pixDriver.setBuffer(0, port * 510, artRDM.getDMX(group, port), numChans);
            break;
          case WS2812_RGBW:
          case WS2812_RGBW_SPLIT:
          case APA102_RGBB:
            if (numChans > 512) {
              numChans = 512;  
            }
            // Copy DMX data to the pixels buffer
            pixDriver.setBuffer(0, port * 512, artRDM.getDMX(group, port), numChans);
            break;
        }

        // Output to pixel strip
        if (!syncEnabled)
          pixDone = false;

        return;

        // FX 12 Mode
      } else if (port == portA[1]) {
        uint8_t* a = artRDM.getDMX(group, port);
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
    }
  }
  else if (portB[0] == group) {
    if (deviceSettings.portBmode == TYPE_SERIAL_LED) {

      if (deviceSettings.portBpixMode == FX_MODE_PIXEL_MAP) {
        switch (deviceSettings.portApixConfig) {
          case WS2812_RGB:
            if (numChans > 510) {
              numChans = 510;
            }    
            // Copy DMX data to the pixels buffer
            pixDriver.setBuffer(1, port * 510, artRDM.getDMX(group, port), numChans);
            break;

          case WS2812_RGBW:
          case WS2812_RGBW_SPLIT:
          case APA102_RGBB:
            if (numChans > 512) {
              numChans = 512;
            }    
            // Copy DMX data to the pixels buffer
            pixDriver.setBuffer(1, port * 512, artRDM.getDMX(group, port), numChans);
            break;
        }

        // Output to pixel strip
        if (!syncEnabled) {
          pixDone = false;
        }

        return;

        // FX 12 mode
      } else if (port == portB[1]) {
        uint8_t* a = artRDM.getDMX(group, port);
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
    }
  }
}

static void syncHandle() {
  if (deviceSettings.portAmode == TYPE_SERIAL_LED) {
    rdmPause(1);
    pixDone = pixDriver.show();
    rdmPause(0);
  } else if (deviceSettings.portAmode != TYPE_DMX_IN) {
    dmxA.unPause();
  }

  if (deviceSettings.portBmode == TYPE_SERIAL_LED) {
    rdmPause(1);
    pixDone = pixDriver.show();
    rdmPause(0);
  } else if (deviceSettings.portBmode != TYPE_DMX_IN) {
    dmxB.unPause();
  }
}

static void ipHandle() {
  if (artRDM.getDHCP()) {
    deviceSettings.gateway = INADDR_NONE;

    deviceSettings.dhcpEnable = 1;
    doReboot = true;

  } else {
    deviceSettings.ip = artRDM.getIP();
    deviceSettings.subnet = artRDM.getSubnetMask();
    deviceSettings.gateway = deviceSettings.ip;
    deviceSettings.gateway[3] = 1;
    deviceSettings.broadcast = {uint8_t(~deviceSettings.subnet[0] | (deviceSettings.ip[0] & deviceSettings.subnet[0])),
                                uint8_t(~deviceSettings.subnet[1] | (deviceSettings.ip[1] & deviceSettings.subnet[1])),
                                uint8_t(~deviceSettings.subnet[2] | (deviceSettings.ip[2] & deviceSettings.subnet[2])),
                                uint8_t(~deviceSettings.subnet[3] | (deviceSettings.ip[3] & deviceSettings.subnet[3]))
                               };
    deviceSettings.dhcpEnable = 0;

    doReboot = true;
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

  if (artRDM.getE131(portA[0], portA[1])) {
    deviceSettings.portAprot = PROT_ARTNET_SACN;
  } else {
    deviceSettings.portAprot = PROT_ARTNET;
  }

  deviceSettings.portBnet = artRDM.getNet(portB[0]);
  deviceSettings.portBsub = artRDM.getSubNet(portB[0]);
  deviceSettings.portBuni[0] = artRDM.getUni(portB[0], portB[1]);
  deviceSettings.portBmerge = artRDM.getMerge(portB[0], portB[1]);

  if (artRDM.getE131(portB[0], portB[1])) {
    deviceSettings.portBprot = PROT_ARTNET_SACN;
  } else {
    deviceSettings.portBprot = PROT_ARTNET;
  }

  // Store everything to EEPROM
  eepromSave();
}

static void rdmHandle(uint8_t group, uint8_t port, rdm_data* c) {
  if (portA[0] == group && portA[1] == port) {
    dmxA.rdmSendCommand(c);
  }
  else if (portB[0] == group && portB[1] == port) {
    dmxB.rdmSendCommand(c);
  }
}

static void rdmReceivedA(rdm_data* c) {
  artRDM.rdmResponse(c, portA[0], portA[1]);
}

static void sendTodA() {
  artRDM.artTODData(portA[0], portA[1], dmxA.todMan(), dmxA.todDev(), dmxA.todCount(), dmxA.todStatus());
}

static void rdmReceivedB(rdm_data* c) {
  artRDM.rdmResponse(c, portB[0], portB[1]);
}

static void sendTodB() {
  artRDM.artTODData(portB[0], portB[1], dmxB.todMan(), dmxB.todDev(), dmxB.todCount(), dmxB.todStatus());
}

static void todRequest(uint8_t group, uint8_t port) {
  if (portA[0] == group && portA[1] == port) {
    sendTodA();
  }
  else if (portB[0] == group && portB[1] == port) {
    sendTodB();
  }
}

static void todFlush(uint8_t group, uint8_t port) {
  if (portA[0] == group && portA[1] == port) {
    dmxA.rdmDiscovery();
  }
  else if (portB[0] == group && portB[1] == port) {
    dmxB.rdmDiscovery();
  }
}

static void dmxIn(uint16_t num) {
  // Double buffer switch
  uint8_t* tmp = dataIn;
  dataIn = dmxA.getChans();
  dmxA.setBuffer(tmp);

  newDmxIn = true;
}

static bool ajaxSave(uint8_t page, DynamicJsonDocument& json) {
  // This is a load request, not a save
  if (json.size() == 2) {
    return true;
  }

  switch (page) {
    case 1:     // Device Status
      // We don't need to save anything for this.  Go straight to load
      return true;
      break;

    case 2:     // Wifi
      strncpy(deviceSettings.wifiSSID, json["wifiSSID"], 40);
      strncpy(deviceSettings.wifiPass, json["wifiPass"], 40);
      strncpy(deviceSettings.hotspotSSID, json["hotspotSSID"], 20);
      strncpy(deviceSettings.hotspotPass, json["hotspotPass"], 20);
      deviceSettings.hotspotDelay = (uint8_t)json["hotspotDelay"];
      deviceSettings.standAloneEnable = (bool)json["standAloneEnable"];
      deviceSettings.ethernetEnable = (bool)json["ethernetEnable"];

      eepromSave();
      return true;
      break;

    case 3:     // IP Address & Node Name
      deviceSettings.ip = IPAddress(json["ipAddress"][0], json["ipAddress"][1], json["ipAddress"][2], json["ipAddress"][3]);
      deviceSettings.subnet = IPAddress(json["subAddress"][0], json["subAddress"][1], json["subAddress"][2], json["subAddress"][3]);
      deviceSettings.gateway = IPAddress(json["gwAddress"][0], json["gwAddress"][1], json["gwAddress"][2], json["gwAddress"][3]);
      deviceSettings.broadcast = uint32_t(deviceSettings.ip) | uint32_t(~uint32_t(deviceSettings.subnet));

      //deviceSettings.broadcast = {uint8_t(~deviceSettings.subnet[0] | (deviceSettings.ip[0] & deviceSettings.subnet[0])),
      //                            uint8_t(~deviceSettings.subnet[1] | (deviceSettings.ip[1] & deviceSettings.subnet[1])),
      //                            uint8_t(~deviceSettings.subnet[2] | (deviceSettings.ip[2] & deviceSettings.subnet[2])),
      //                            uint8_t(~deviceSettings.subnet[3] | (deviceSettings.ip[3] & deviceSettings.subnet[3]))};

      strncpy(deviceSettings.nodeName, json["nodeName"], 18);
      strncpy(deviceSettings.longName, json["longName"], 64);

      if (!isHotspot && (bool)json["dhcpEnable"] != deviceSettings.dhcpEnable) {
        if ((bool)json["dhcpEnable"]) {
          deviceSettings.gateway = INADDR_NONE;

        }
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

        if ((uint8_t)json["portAnet"] < 128) {
          deviceSettings.portAnet = (uint8_t)json["portAnet"];
        }

        if ((uint8_t)json["portAsub"] < 16) {
          deviceSettings.portAsub = (uint8_t)json["portAsub"];
        }

        for (uint8_t x = 0; x < 4; x++) {
          if ((uint8_t)json["portAuni"][x] < 16) {
            deviceSettings.portAuni[x] = (uint8_t)json["portAuni"][x];
          }

          if ((uint16_t)json["portAsACNuni"][x] > 0 && (uint16_t)json["portAsACNuni"][x] < 64000) {
            deviceSettings.portAsACNuni[x] = (uint16_t)json["portAsACNuni"][x];
          }

          artRDM.setE131(portA[0], portA[x + 1], e131);
          artRDM.setE131Uni(portA[0], portA[x + 1], deviceSettings.portAsACNuni[x]);
        }

        uint8_t newMode = json["portAmode"];
        uint8_t oldMode = deviceSettings.portAmode;
        uint8_t newConfig = json["portApixConfig"];
        uint8_t oldConfig = deviceSettings.portApixConfig;
        bool updatePorts = false;

        // RDM and DMX input can't run together
        if (newMode == TYPE_DMX_IN && deviceSettings.portBmode == TYPE_RDM_OUT) {
          deviceSettings.portBmode = TYPE_DMX_OUT;
          dmxB.rdmDisable();
        }

        if (newMode == TYPE_DMX_IN && json.containsKey("dmxInBroadcast")) {
          deviceSettings.dmxInBroadcast = IPAddress(json["dmxInBroadcast"][0], json["dmxInBroadcast"][1], json["dmxInBroadcast"][2], json["dmxInBroadcast"][3]);
        }

        if (newConfig != oldConfig) {
          // Store the nem mode to settings
          deviceSettings.portApixConfig = newConfig;
          doReboot = true;
        }

        if (newMode != oldMode) {
          // Store the nem mode to settings
          deviceSettings.portAmode = newMode;
          doReboot = true;
        }

        // Update the Artnet class
        artRDM.setNet(portA[0], deviceSettings.portAnet);
        artRDM.setSubNet(portA[0], deviceSettings.portAsub);
        artRDM.setUni(portA[0], portA[1], deviceSettings.portAuni[0]);
        artRDM.setMerge(portA[0], portA[1], deviceSettings.portAmerge);

        // Lengthen or shorten our pixel strip & handle required Artnet ports
        if (newMode == TYPE_SERIAL_LED && !doReboot) {
          // Get the new & old lengths of pixel strip
          uint16_t newLen = (json.containsKey("portAnumPix")) ? (uint16_t)json["portAnumPix"] : deviceSettings.portAnumPix;
          uint16_t lim = 0;
          switch (deviceSettings.portApixConfig) {
            default:
            case WS2812_RGB:
              lim = 680;
              break;
            case WS2812_RGBW:
            case WS2812_RGBW_SPLIT:
            case APA102_RGBB:
              lim = 512;
              break;
          }
          uint16_t oldLen = deviceSettings.portAnumPix;
          // If pixel size has changed
          if (newLen <= lim && oldLen != newLen) {
            // Update our pixel strip
            deviceSettings.portAnumPix = newLen;
            pixDriver.updateStrip(1, deviceSettings.portAnumPix, deviceSettings.portApixConfig);

            // If the old mode was pixel map then update the Artnet ports
            if (deviceSettings.portApixMode == FX_MODE_PIXEL_MAP) {
              updatePorts = true;
            }
          }

          // If the old mode was 12 channel FX, update oldLen to represent the number of channels we used
          if (deviceSettings.portApixMode == FX_MODE_12) {
            oldLen = 12;
          }

          // If our mode changes then update the Artnet ports
          if (deviceSettings.portApixMode != (uint8_t)json["portApixMode"]) {
            updatePorts = true;
          }

          // Store the new pixel mode
          deviceSettings.portApixMode = (uint8_t)json["portApixMode"];

          // If our new mode is FX12 then we need 12 channels & store the start address
          if (deviceSettings.portApixMode == FX_MODE_12) {
            if ((uint16_t)json["portApixFXstart"] <= 501 && (uint16_t)json["portApixFXstart"] > 0) {
              deviceSettings.portApixFXstart = (uint16_t)json["portApixFXstart"];
            }
            newLen = 12;
          }

          // If needed, open and close Artnet ports
          if (updatePorts) {
            for (uint8_t x = 1, y = 2; x < 4; x++, y++) {
              uint16_t c = (x * 170);
              if (newLen > c) {
                portA[y] = artRDM.addPort(portA[0], x, deviceSettings.portAuni[x], TYPE_DMX_OUT, deviceSettings.portAmerge);
              } else if (oldLen > c) {
                artRDM.closePort(portA[0], portA[y]);
              }
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
      {
        deviceSettings.portBprot = (uint8_t)json["portBprot"];
        bool e131 = (deviceSettings.portBprot == PROT_ARTNET_SACN) ? true : false;

        deviceSettings.portBmerge = (uint8_t)json["portBmerge"];

        if ((uint8_t)json["portBnet"] < 128) {
          deviceSettings.portBnet = (uint8_t)json["portBnet"];
        }

        if ((uint8_t)json["portBsub"] < 16) {
          deviceSettings.portBsub = (uint8_t)json["portBsub"];
        }

        for (uint8_t x = 0; x < 4; x++) {
          if ((uint8_t)json["portBuni"][x] < 16) {
            deviceSettings.portBuni[x] = (uint8_t)json["portBuni"][x];
          }

          if ((uint16_t)json["portBsACNuni"][x] > 0 && (uint16_t)json["portBsACNuni"][x] < 64000) {
            deviceSettings.portBsACNuni[x] = (uint16_t)json["portBsACNuni"][x];
          }

          artRDM.setE131(portB[0], portB[x + 1], e131);
          artRDM.setE131Uni(portB[0], portB[x + 1], deviceSettings.portBsACNuni[x]);
        }

        uint8_t newMode = json["portBmode"];
        uint8_t oldMode = deviceSettings.portBmode;
        uint8_t newConfig = json["portBpixConfig"];
        uint8_t oldConfig = deviceSettings.portBpixConfig;
        bool updatePorts = false;

        // RDM and DMX input can't run together
        if (newMode == TYPE_RDM_OUT && deviceSettings.portAmode == TYPE_DMX_IN)
          newMode = TYPE_DMX_OUT;

        if (newConfig != oldConfig) {
          deviceSettings.portBpixConfig = newConfig;
          doReboot = true;
        }

        if (newMode != oldMode) {
          // Store the nem mode to settings
          deviceSettings.portBmode = newMode;
          doReboot = true;
        }

        // Update the Artnet class
        artRDM.setNet(portB[0], deviceSettings.portBnet);
        artRDM.setSubNet(portB[0], deviceSettings.portBsub);
        artRDM.setUni(portB[0], portB[1], deviceSettings.portBuni[0]);
        artRDM.setMerge(portB[0], portB[1], deviceSettings.portBmerge);

        // Lengthen or shorten our pixel strip & handle required Artnet ports
        if (newMode == TYPE_SERIAL_LED && !doReboot) {
          // Get the new & old lengths of pixel strip
          uint16_t newLen = (json.containsKey("portBnumPix")) ? (uint16_t)json["portBnumPix"] : deviceSettings.portBnumPix;
          uint16_t lim = 0;
          switch (deviceSettings.portBpixConfig) {
            default:
            case WS2812_RGB:
              lim = 680;
              break;
            case WS2812_RGBW:
            case WS2812_RGBW_SPLIT:
            case APA102_RGBB:
              lim = 512;
              break;
          }
          uint16_t oldLen = deviceSettings.portBnumPix;

          // If pixel size has changed
          if (newLen <= lim && oldLen != newLen) {
            // Update our pixel strip
            deviceSettings.portBnumPix = newLen;
            pixDriver.updateStrip(1, deviceSettings.portBnumPix, deviceSettings.portBpixConfig);

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
  return false;
}

static void ajaxLoad(uint8_t page, JsonDocument& jsonReply) {

  // Create the needed arrays here - doesn't work within the switch below
  JsonArray ipAddress = jsonReply.createNestedArray("ipAddress");
  JsonArray subAddress = jsonReply.createNestedArray("subAddress");
  JsonArray gwAddress = jsonReply.createNestedArray("gwAddress");
  JsonArray bcAddress = jsonReply.createNestedArray("bcAddress");
  JsonArray portAuni = jsonReply.createNestedArray("portAuni");
  JsonArray portBuni = jsonReply.createNestedArray("portBuni");
  JsonArray portAsACNuni = jsonReply.createNestedArray("portAsACNuni");
  JsonArray portBsACNuni = jsonReply.createNestedArray("portBsACNuni");
  JsonArray dmxInBroadcast = jsonReply.createNestedArray("dmxInBroadcast");

  // Get MAC Address
  char MAC_char[30] = "";
  sprintf(MAC_char, "%02X", MAC_array[0]);
  for (int i = 1; i < 6; ++i) {
    sprintf(MAC_char, "%s:%02X", MAC_char, MAC_array[i]);
  }

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

          case TYPE_SERIAL_LED:
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

          case TYPE_SERIAL_LED:
            jsonReply["portBStatus"] = "WS2812 mode";
            break;
        }
        switch (deviceSettings.portApixConfig) {
          case WS2812_RGB:
            jsonReply["portApixConfig"] = "WS2812 RGB";
            break;

          case WS2812_RGBW:
            jsonReply["portApixConfig"] = "WS2812 RGBW";
            break;

          case WS2812_RGBW_SPLIT:
            jsonReply["portApixConfig"] = "WS2812 RGBW Split W";
            break;

          case APA102_RGBB:
            jsonReply["portApixConfig"] = "APA102 RGBB";
            break;
        }
        switch (deviceSettings.portBpixConfig) {
          case WS2812_RGB:
            jsonReply["portBpixConfig"] = "WS2812 RGB";
            break;

          case WS2812_RGBW:
            jsonReply["portBpixConfig"] = "WS2812 RGBW";
            break;

          case WS2812_RGBW_SPLIT:
            jsonReply["portBpixConfig"] = "WS2812 RGBW Split W";
            break;

          case APA102_RGBB:
            jsonReply["portBpixConfig"] = "APA102 RGBB";
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
      jsonReply["ethernetEnable"] = deviceSettings.ethernetEnable;

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
      if (deviceSettings.portAmode == TYPE_DMX_IN) {
        jsonReply["portAprot"] = PROT_ARTNET;
      } else {
        jsonReply["portAprot"] = deviceSettings.portAprot;
      }

      jsonReply["portAmerge"] = deviceSettings.portAmerge;
      jsonReply["portAnet"] = deviceSettings.portAnet;
      jsonReply["portAsub"] = deviceSettings.portAsub;
      jsonReply["portAnumPix"] = deviceSettings.portAnumPix;

      jsonReply["portApixMode"] = deviceSettings.portApixMode;
      jsonReply["portApixConfig"] = deviceSettings.portApixConfig;
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
      jsonReply["portBpixConfig"] = deviceSettings.portBpixConfig;
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

static void ajaxHandle() {
  deserializeJson(jsonDocument, webServer.arg("plain"));
  DynamicJsonDocument jsonReply(65536);

  String reply;

  // Handle request to reboot into update mode
  if (jsonDocument.containsKey("success") && jsonDocument["success"] == 1 && jsonDocument.containsKey("doUpdate")) {
    artRDM.end();

    jsonReply["success"] = 1;
    jsonReply["doUpdate"] = 1;

    serializeJson(jsonReply, reply);
    webServer.send(200, "application/json", reply);

    if (jsonDocument["doUpdate"] == 1) {
      // Turn pixel strips off if they're on
      pixDriver.updateStrip(0, 0, deviceSettings.portApixConfig);
      pixDriver.updateStrip(1, 0, deviceSettings.portBpixConfig);

      deviceSettings.doFirmwareUpdate = true;
      eepromSave();

      doReboot = true;
    }

    // Handle load and save of data
  } else if (jsonDocument.containsKey("success") && jsonDocument["success"] == 1 && jsonDocument.containsKey("page")) {
    if (ajaxSave((uint8_t)jsonDocument["page"], jsonDocument)) {
        ajaxLoad((uint8_t)jsonDocument["page"], jsonReply);

      if (jsonDocument.size() > 2) {
        jsonReply["message"] = "Settings Saved";
      }
    } else {
      jsonReply["success"] = 0;
      jsonReply["message"] = "Failed to save data.  Reload page and try again.";
    }

    // Handle reboots
  } else if (jsonDocument.containsKey("success") && jsonDocument.containsKey("reboot") && jsonDocument["reboot"] == 1) {
    jsonReply["success"] = 1;
    jsonReply["message"] = "Device Restarting.";

    // Turn pixel strips off if they're on
    pixDriver.updateStrip(0, 0, deviceSettings.portApixConfig);
    pixDriver.updateStrip(1, 0, deviceSettings.portBpixConfig);

    doReboot = true;

    // Handle errors
  }

  serializeJson(jsonReply, reply);
  webServer.send(200, "application/json", reply);
}

/* webFirmwareUpdate()
    display update status after firmware upload and restart
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
    handle firmware upload and update
*/
static void webFirmwareUpload() {
  String reply = "";
  HTTPUpload& upload = webServer.upload();

  if (upload.status == UPLOAD_FILE_START) {
    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSketchSpace)) { //start with max available size
      reply = "{\"success\":0,\"message\":\"Insufficient space.\"}";
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      reply = "{\"success\":0,\"message\":\"Failed to save\"}";
    }

  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) { //true to set the size to the current progress
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

  static char c[128] = { 0 };

  if (nodeErrorTimeout > millis()) {
    nextNodeReport = millis() + 2000;
  } else {
    nextNodeReport = millis() + 5000;
  }

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

      case TYPE_SERIAL_LED:
        if (deviceSettings.portApixMode == FX_MODE_12)
          sprintf(c, "%s 12chan", c);
        sprintf(c, "%s WS2812 %ipixels", c, deviceSettings.portAnumPix);
        break;
    }

    sprintf(c, "%s. PortB:", c);

    switch (deviceSettings.portBmode) {
      case TYPE_DMX_OUT:
        sprintf(c, "%s DMX Out", c);
        break;

      case TYPE_RDM_OUT:
        sprintf(c, "%s RDM Out", c);
        break;

      case TYPE_SERIAL_LED:
        if (deviceSettings.portBpixMode == FX_MODE_12)
          sprintf(c, "%s 12chan", c);
        sprintf(c, "%s WS2812 %ipixels", c, deviceSettings.portBnumPix);
        break;
    }
  }

  artRDM.setNodeReport(c, ARTNET_RC_POWER_OK);
}

static void portSetup() {

  if (deviceSettings.portAmode == TYPE_DMX_OUT || deviceSettings.portAmode == TYPE_RDM_OUT) {

    dmxA.begin(DMX_DIR_A, artRDM.getDMX(portA[0], portA[1]));
    if (deviceSettings.portAmode == TYPE_RDM_OUT && !dmxA.rdmEnabled()) {
      dmxA.rdmEnable(ESTA_MAN, ESTA_DEV);
      dmxA.rdmSetCallBack(rdmReceivedA);
      dmxA.todSetCallBack(sendTodA);
    }

  } else if (deviceSettings.portAmode == TYPE_DMX_IN) {

    dmxA.begin(DMX_DIR_A, artRDM.getDMX(portA[0], portA[1]));
    dmxA.dmxIn(true);
    dmxA.setInputCallback(dmxIn);

    dataIn = (uint8_t*) malloc(sizeof(uint8_t) * 512);
    memset(dataIn, 0, 512);

  } else if (deviceSettings.portAmode == TYPE_SERIAL_LED) {
    pixDriver.setStrip(0, deviceSettings.portAnumPix, deviceSettings.portApixConfig);
  }

  if (deviceSettings.portBmode == TYPE_DMX_OUT || deviceSettings.portBmode == TYPE_RDM_OUT) {
    dmxB.begin(DMX_DIR_B, artRDM.getDMX(portB[0], portB[1]));
    if (deviceSettings.portBmode == TYPE_RDM_OUT && !dmxB.rdmEnabled()) {
      dmxB.rdmEnable(ESTA_MAN, ESTA_DEV);
      dmxB.rdmSetCallBack(rdmReceivedB);
      dmxB.todSetCallBack(sendTodB);
    }
  } else if (deviceSettings.portBmode == TYPE_SERIAL_LED)  {
    pixDriver.setStrip(1, deviceSettings.portBnumPix, deviceSettings.portBpixConfig);
  }
}

static void artStart() {
  // Initialise out ArtNet
  if (isHotspot) {
    artRDM.init(deviceSettings.hotspotIp, deviceSettings.hotspotSubnet, true, deviceSettings.nodeName, deviceSettings.longName, ARTNET_OEM, ESTA_MAN, MAC_array);
  } else {
    artRDM.init(deviceSettings.ip, deviceSettings.subnet, deviceSettings.dhcpEnable, deviceSettings.nodeName, deviceSettings.longName, ARTNET_OEM, ESTA_MAN, MAC_array);
  }

  // Set firmware
  artRDM.setFirmwareVersion(ART_FIRM_VERSION);

  // Add Group
  portA[0] = artRDM.addGroup(deviceSettings.portAnet, deviceSettings.portAsub);

  bool e131 = (deviceSettings.portAprot == PROT_ARTNET_SACN) ? true : false;

  // WS2812 uses TYPE_DMX_OUT - the rest use the value assigned
  if (deviceSettings.portAmode == TYPE_SERIAL_LED) {
    portA[1] = artRDM.addPort(portA[0], 0, deviceSettings.portAuni[0], TYPE_DMX_OUT, deviceSettings.portAmerge);
  } else {
    portA[1] = artRDM.addPort(portA[0], 0, deviceSettings.portAuni[0], deviceSettings.portAmode, deviceSettings.portAmerge);
  }

  artRDM.setE131(portA[0], portA[1], e131);
  artRDM.setE131Uni(portA[0], portA[1], deviceSettings.portAsACNuni[0]);

  // Add extra Artnet ports for WS2812
  if (deviceSettings.portAmode == TYPE_SERIAL_LED && deviceSettings.portApixMode == FX_MODE_PIXEL_MAP) {
    int32_t lim1 = 170;
    int32_t lim2 = 340;
    int32_t lim3 = 510;
    switch (deviceSettings.portApixConfig) {
      case WS2812_RGB:
        lim1 = 170;
        lim2 = 340;
        lim3 = 510;
        break;
      case WS2812_RGBW:
      case WS2812_RGBW_SPLIT:
      case APA102_RGBB:
        lim1 = 128;
        lim2 = 256;
        lim3 = 384;
        break;
    }
    if (deviceSettings.portAnumPix > lim1) {
      portA[2] = artRDM.addPort(portA[0], 1, deviceSettings.portAuni[1], TYPE_DMX_OUT, deviceSettings.portAmerge);

      artRDM.setE131(portA[0], portA[2], e131);
      artRDM.setE131Uni(portA[0], portA[2], deviceSettings.portAsACNuni[1]);
    }
    if (deviceSettings.portAnumPix > lim2) {
      portA[3] = artRDM.addPort(portA[0], 2, deviceSettings.portAuni[2], TYPE_DMX_OUT, deviceSettings.portAmerge);

      artRDM.setE131(portA[0], portA[3], e131);
      artRDM.setE131Uni(portA[0], portA[3], deviceSettings.portAsACNuni[2]);
    }
    if (deviceSettings.portAnumPix > lim3) {
      portA[4] = artRDM.addPort(portA[0], 3, deviceSettings.portAuni[3], TYPE_DMX_OUT, deviceSettings.portAmerge);

      artRDM.setE131(portA[0], portA[4], e131);
      artRDM.setE131Uni(portA[0], portA[4], deviceSettings.portAsACNuni[3]);
    }
  }


  // Add Group
  portB[0] = artRDM.addGroup(deviceSettings.portBnet, deviceSettings.portBsub);
  e131 = (deviceSettings.portBprot == PROT_ARTNET_SACN) ? true : false;

  // WS2812 uses TYPE_DMX_OUT - the rest use the value assigned
  if (deviceSettings.portBmode == TYPE_SERIAL_LED) {
    portB[1] = artRDM.addPort(portB[0], 0, deviceSettings.portBuni[0], TYPE_DMX_OUT, deviceSettings.portBmerge);
  } else {
    portB[1] = artRDM.addPort(portB[0], 0, deviceSettings.portBuni[0], deviceSettings.portBmode, deviceSettings.portBmerge);
  }

  artRDM.setE131(portB[0], portB[1], e131);
  artRDM.setE131Uni(portB[0], portB[1], deviceSettings.portBsACNuni[0]);

  // Add extra Artnet ports for WS2812
  if (deviceSettings.portBmode == TYPE_SERIAL_LED && deviceSettings.portBpixMode == FX_MODE_PIXEL_MAP) {
    int32_t lim1 = 170;
    int32_t lim2 = 340;
    int32_t lim3 = 510;
    switch (deviceSettings.portBpixConfig) {
      case WS2812_RGB:
        lim1 = 170;
        lim2 = 340;
        lim3 = 510;
        break;
      case WS2812_RGBW:
      case WS2812_RGBW_SPLIT:
      case APA102_RGBB:
        lim1 = 128;
        lim2 = 256;
        lim3 = 384;
        break;
    }
    if (deviceSettings.portBnumPix > lim1) {
      portB[2] = artRDM.addPort(portB[0], 1, deviceSettings.portBuni[1], TYPE_DMX_OUT, deviceSettings.portBmerge);

      artRDM.setE131(portB[0], portB[2], e131);
      artRDM.setE131Uni(portB[0], portB[2], deviceSettings.portBsACNuni[1]);
    }
    if (deviceSettings.portBnumPix > lim2) {
      portB[3] = artRDM.addPort(portB[0], 2, deviceSettings.portBuni[2], TYPE_DMX_OUT, deviceSettings.portBmerge);

      artRDM.setE131(portB[0], portB[3], e131);
      artRDM.setE131Uni(portB[0], portB[3], deviceSettings.portBsACNuni[2]);
    }
    if (deviceSettings.portBnumPix > lim3) {
      portB[4] = artRDM.addPort(portB[0], 3, deviceSettings.portBuni[3], TYPE_DMX_OUT, deviceSettings.portBmerge);

      artRDM.setE131(portB[0], portB[4], e131);
      artRDM.setE131Uni(portB[0], portB[4], deviceSettings.portBsACNuni[3]);
    }
  }

  // Add required callback functions
  artRDM.setArtDMXCallback(dmxHandle);
  artRDM.setArtRDMCallback(rdmHandle);
  artRDM.setArtSyncCallback(syncHandle);
  artRDM.setArtIPCallback(ipHandle);
  artRDM.setArtAddressCallback(addressHandle);
  artRDM.setTODRequestCallback(todRequest);
  artRDM.setTODFlushCallback(todFlush);

  switch (rtc_get_reset_reason(xPortGetCoreID())) {
    case NO_MEAN:
    case POWERON_RESET:
    case SW_RESET:
    case SW_CPU_RESET:
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

  // Start artnet
  artRDM.begin();

  yield();
}

static void webStart() {
  webServer.on("/", []() {
    artRDM.pause();
    webServer.send_P(200, typeHTML, mainPage);
    webServer.sendHeader("Connection", "close");
    yield();
    artRDM.begin();
  });

  webServer.on("/style.css", []() {
    artRDM.pause();
    // If no style.css in SPIFFS, send default
    if (!SPIFFS.exists("/style.css"))
      webServer.send_P(200, typeCSS, css);
    else {
      File f = SPIFFS.open("/style.css", "r");
      webServer.streamFile(f, typeCSS);
      f.close();
    }
    webServer.sendHeader("Connection", "close");
    yield();
    artRDM.begin();
  });

  webServer.on("/ajax", HTTP_POST, ajaxHandle);

  webServer.on("/upload", HTTP_POST, webFirmwareUpdate, webFirmwareUpload);

  webServer.on("/style", []() {
    webServer.send_P(200, typeHTML, cssUploadPage);
    webServer.sendHeader("Connection", "close");
  });

  webServer.on("/style_delete", []() {
    if (SPIFFS.exists("/style.css"))
      SPIFFS.remove("/style.css");

    webServer.send(200, "text/plain", "style.css deleted.  The default style is now in use.");
    webServer.sendHeader("Connection", "close");
  });

  webServer.on("/style_upload", HTTP_POST, []() {
    webServer.send(200, "text/plain", "Upload successful!");
  }, []() {
    HTTPUpload& upload = webServer.upload();

    if (upload.status == UPLOAD_FILE_START) {
      String filename = upload.filename;
      if (!filename.startsWith("/")) filename = "/" + filename;
      fsUploadFile = SPIFFS.open(filename, "w");
      filename = String();

    } else if (upload.status == UPLOAD_FILE_WRITE) {
      if (fsUploadFile)
        fsUploadFile.write(upload.buf, upload.currentSize);

    } else if (upload.status == UPLOAD_FILE_END) {
      if (fsUploadFile) {
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

static void startHotspot() {
  yield();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(deviceSettings.hotspotSSID, deviceSettings.hotspotPass);
  WiFi.softAPConfig(deviceSettings.hotspotIp, deviceSettings.hotspotIp, deviceSettings.hotspotSubnet);

  sprintf(wifiStatus, "No Wifi. Hotspot started.<br />\nHotspot SSID: %s", deviceSettings.hotspotSSID);
  WiFi.macAddress(MAC_array);

  isHotspot = true;

  if (deviceSettings.standAloneEnable) {
    return;
  }

  webStart();

  unsigned long endTime = millis() + 30000;

  // Stay here if not in stand alone mode - no dmx or artnet
  while (endTime > millis() || WiFi.softAPgetStationNum() > 0) {
    webServer.handleClient();
    yield();
  }

  ESP.restart();
  isHotspot = false;
}

static void ETHEvent(WiFiEvent_t event)
{
  switch (event) {
    case SYSTEM_EVENT_ETH_START:
      ETH.setHostname(deviceSettings.nodeName);
      break;
    case SYSTEM_EVENT_ETH_CONNECTED:
      break;
    case SYSTEM_EVENT_ETH_GOT_IP:
      deviceSettings.ip = ETH.localIP();
      deviceSettings.subnet = ETH.subnetMask();
      if (deviceSettings.gateway == INADDR_NONE) {
        deviceSettings.gateway = ETH.gatewayIP();
      }
      deviceSettings.broadcast = {uint8_t(~deviceSettings.subnet[0] | (deviceSettings.ip[0] & deviceSettings.subnet[0])),
                                  uint8_t(~deviceSettings.subnet[1] | (deviceSettings.ip[1] & deviceSettings.subnet[1])),
                                  uint8_t(~deviceSettings.subnet[2] | (deviceSettings.ip[2] & deviceSettings.subnet[2])),
                                  uint8_t(~deviceSettings.subnet[3] | (deviceSettings.ip[3] & deviceSettings.subnet[3]))
                               };
      esp_eth_get_mac(MAC_array);
      break;
    case SYSTEM_EVENT_ETH_DISCONNECTED:
      break;
    case SYSTEM_EVENT_ETH_STOP:
      break;
    default:
      break;
  }
}

static void wifiStart() {
  // If it's the default WiFi SSID, make it unique
  if (strcmp(deviceSettings.hotspotSSID, "espArtNetNode") == 0 || deviceSettings.hotspotSSID[0] == '\0') {
    sprintf(deviceSettings.hotspotSSID, "espArtNetNode_%05u", uint32_t((ESP.getEfuseMac() >> 32) & 0xFFFF));
  }

  if (deviceSettings.standAloneEnable) {
    startHotspot();

    deviceSettings.ip = deviceSettings.hotspotIp;
    deviceSettings.subnet = deviceSettings.hotspotSubnet;
    deviceSettings.broadcast = {uint8_t(~deviceSettings.subnet[0] | (deviceSettings.ip[0] & deviceSettings.subnet[0])),
                                uint8_t(~deviceSettings.subnet[1] | (deviceSettings.ip[1] & deviceSettings.subnet[1])),
                                uint8_t(~deviceSettings.subnet[2] | (deviceSettings.ip[2] & deviceSettings.subnet[2])),
                                uint8_t(~deviceSettings.subnet[3] | (deviceSettings.ip[3] & deviceSettings.subnet[3]))
                               };

    return;
  }

  if (deviceSettings.ethernetEnable) {
    WiFi.onEvent(ETHEvent);
    ETH.begin();
    if (!deviceSettings.dhcpEnable) {
      ETH.config(deviceSettings.ip, deviceSettings.gateway, deviceSettings.subnet);
    }
    return;
  }

  if (deviceSettings.wifiSSID[0] != '\0') {
    WiFi.begin(deviceSettings.wifiSSID, deviceSettings.wifiPass);
    WiFi.mode(WIFI_STA);

    WiFi.setHostname(deviceSettings.nodeName);

    unsigned long endTime = millis() + (deviceSettings.hotspotDelay * 1000);

    if (deviceSettings.dhcpEnable) {
      while (WiFi.status() != WL_CONNECTED && endTime > millis()) {
        yield();
      }

      if (millis() >= endTime) {
        startHotspot();
      }

      deviceSettings.ip = WiFi.localIP();
      deviceSettings.subnet = WiFi.subnetMask();

      if (deviceSettings.gateway == INADDR_NONE) {
        deviceSettings.gateway = WiFi.gatewayIP();
      }

      deviceSettings.broadcast = { uint8_t(~deviceSettings.subnet[0] | (deviceSettings.ip[0] & deviceSettings.subnet[0])),
                                   uint8_t(~deviceSettings.subnet[1] | (deviceSettings.ip[1] & deviceSettings.subnet[1])),
                                   uint8_t(~deviceSettings.subnet[2] | (deviceSettings.ip[2] & deviceSettings.subnet[2])),
                                   uint8_t(~deviceSettings.subnet[3] | (deviceSettings.ip[3] & deviceSettings.subnet[3]))
                                 };
    } else {
      WiFi.config(deviceSettings.ip, deviceSettings.gateway, deviceSettings.subnet);
    }

    //sprintf(wifiStatus, "Wifi connected.  Signal: %ld<br />SSID: %s", WiFi.RSSI(), deviceSettings.wifiSSID);
    sprintf(wifiStatus, "Wifi connected.<br />SSID: %s", deviceSettings.wifiSSID);
    WiFi.macAddress(MAC_array);

  } else {
    startHotspot();
  }

  yield();
}
