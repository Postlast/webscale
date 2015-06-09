# esp8266web
Small web server on ESP8266 + TCP2UART 
---

HTTP-Web supports more than hundred variables -
All I/О, Wifi, system, ... variables.

GET/POST, cookie, load/upload data/Flash, multipart/form-data,...

TCP2UART: 300..3000000 Baud, Flow Control On/Off, Inverse On/Of all signals, ...

HTTP-Web services to more than 10 simultaneous open connections.

TCP transfer speed > 1 Mbytes/sec (debug off).

Supported 512k..16 MBytes Flash (Web Disk).

From SDK 1.1.1 used only: 
libpp.a, libwpa.a, libnet80211.a, parts libphy.a, eagle_lib.o, mem_manager.o, user_interface.o

Make WEBFS CmdLine:

PVFS2.exe -h "*.htm, *.html, *.cgi, *.xml, *.bin, *.txt, *.wav" -z "*.inc, snmp.bib" ./WEBFiles ./webbin ./WEBFiles.bin

Upload WEBFS:

1) Connect AP 'ESP8266', password '0123456789'

2) Explorer: http://192.168.4.1/fsupload (name and password from WiFi AP)

3) Select WEBFiles.bin file. Upload.


esp8266web Web_Base
http://esp8266.ru/forum/threads/razrabotka-biblioteki-malogo-webservera-na-esp8266.56/

UDK
http://esp8266.ru/forum/forums/devkit/