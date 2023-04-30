# AsyncWiFiManager
This library came about because of issues I had with the existing ESP8266/ESP32 wifi managers. Specifically:
- I needed something that worked with [ESPAsyncWebServer](https://github.com/me-no-dev/ESPAsyncWebServer)
- I wanted the solution itself to be asnynchronous - i.e. it shouldn't block trying to connect
- Existing solutions suffered from several issues:
  - The ESP8266 versions would not automatically re-connect to a router, either if the connection was briefly interrupted, or if the router was initially down when the client came up.
  - If re-connects were enabled separately, the ESP8266 attempted to re-connect every second, the ESP32 sat in a tight loop. In both cases it was difficult if not impossible for a client (phone or PC for example) to connect to the access point created by the ESP32 or ESP8266.
  - It was not possible for an application to create or destroy the access point using the wifi manager.
  - If the wifi manager created an access point, it would be there forever. This could lead to so many access points existing that the wifi environment became very unstable. The wifi manager should destroy the access point after a router connection has been successfully established.
  - Wifi managers should automatically create an access point if one can't be found when the ESP8266 or ESP32 is started.
  - Network re-scanning was not implemented in the config portal
  - Network scans were incorrectly implemented on ESP32
  - It should be possible to see the network password being entered.

This library fixes all of those issues and has been tested on multiple versions of Arduino framework for both the ESP8266 and ESP32, though this is an initial release so there may be problems I haven't encountered, and certainly features that could be added.
