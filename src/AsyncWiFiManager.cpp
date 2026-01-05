/**************************************************************
 AsyncWiFiManager is a library for the ESP8266/Arduino platform
 (https://github.com/esp8266/Arduino) to enable easy
 configuration and reconfiguration of WiFi credentials using a Captive Portal
 inspired by:
 http://www.esp8266.com/viewtopic.php?f=29&t=2520
 https://github.com/chriscook8/esp-arduino-apboot
 https://github.com/esp8266/Arduino/tree/esp8266/hardware/esp8266com/esp8266/libraries/DNSServer/examples/CaptivePortalAdvanced
 Built by AlexT https://github.com/tzapu
 Ported to Async Web Server by https://github.com/alanswx
 Licensed under MIT license
 **************************************************************/

#include "AsyncWiFiManager.h"
#if !defined(ESP8266)
#include <esp_wifi.h>
#else
#include <core_version.h>
#endif

AsyncWiFiManagerParameter::AsyncWiFiManagerParameter(const char *custom) {
	_id = NULL;
	_placeholder = NULL;
	_length = 0;
	_value = NULL;

	_customHTML = custom;
}

AsyncWiFiManagerParameter::AsyncWiFiManagerParameter(const char *id, const char *placeholder, const char *defaultValue, int length) {
	init(id, placeholder, defaultValue, length, "");
}

AsyncWiFiManagerParameter::AsyncWiFiManagerParameter(const char *id, const char *placeholder, const char *defaultValue, int length, const char *custom) {
	init(id, placeholder, defaultValue, length, custom);
}

void AsyncWiFiManagerParameter::init(const char *id, const char *placeholder, const char *defaultValue, int length, const char *custom) {
	_id = id;
	_placeholder = placeholder;
	_length = length;
	_value = new char[length + 1];
	for (int i = 0; i < length; i++) {
		_value[i] = 0;
	}
	if (defaultValue != NULL) {
		strncpy(_value, defaultValue, length);
	}

	_customHTML = custom;
}

const char* AsyncWiFiManagerParameter::getValue() {
	return _value;
}
const char* AsyncWiFiManagerParameter::getID() {
	return _id;
}
const char* AsyncWiFiManagerParameter::getPlaceholder() {
	return _placeholder;
}
int AsyncWiFiManagerParameter::getValueLength() {
	return _length;
}
const char* AsyncWiFiManagerParameter::getCustomHTML() {
	return _customHTML;
}

#ifdef USE_EADNS
AsyncWiFiManager::AsyncWiFiManager(AsyncWebServer *server, AsyncDNSServer *dns) : server(server), dnsServer(dns) {
	_cacheHeads();
	wifiSSIDs = NULL;
#ifdef ESP32
	loopMutex = xSemaphoreCreateMutex();
#endif
}
#else
AsyncWiFiManager::AsyncWiFiManager(AsyncWebServer *server, DNSServer *dns) : server(server), dnsServer(dns) {
	_cacheHeads();
	wifiSSIDs = NULL;
#ifdef ESP32
	loopMutex = xSemaphoreCreateMutex();
#endif
}
#endif

void AsyncWiFiManager::_cacheHeads() {
	_wifiSaveHead = FPSTR(WFM_HTTP_HEAD);
	_wifiSaveHead.replace("{v}", "Credentials Saved");

	_wifiHead = FPSTR(WFM_HTTP_HEAD);
	_wifiHead.replace("{v}", "Config ESP");

	_rootHead = FPSTR(WFM_HTTP_HEAD);
	_rootHead.replace("{v}", "Options");

	_infoHead = FPSTR(WFM_HTTP_HEAD);
	_infoHead.replace("{v}", "Info");

	_resetHead = FPSTR(WFM_HTTP_HEAD);
	_resetHead.replace("{v}", "Reset");

}

void AsyncWiFiManager::setHostname(const char* hostname) {
#ifdef ARDUINO_ESP8266_RELEASE_2_3_0
	WiFi.hostname(hostname);
#else
	WiFi.setHostname(hostname);
#endif
}

void AsyncWiFiManager::addParameter(AsyncWiFiManagerParameter *p) {
	_params[_paramsCount] = p;
	_paramsCount++;
//  DEBUG_WM(F("Adding parameter"));
//  DEBUG_WM(p->getID());
}

void AsyncWiFiManager::dnsStart(bool start) {
	//Make DNS control idempotent

	if (start && !_dnsRunning) {
		DEBUG_WM(F("Starting DNS server"));
		_dnsRunning = true;
		/* Setup the DNS server redirecting all the domains to the apIP */
#ifdef USE_EADNS
		dnsServer->setErrorReplyCode(AsyncDNSReplyCode::NoError);
#else
		dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
#endif

		dnsServer->setTTL(5);
		DEBUG_WM(toStringIp(WiFi.softAPIP()));
		if (!dnsServer->start(DNS_PORT, "*", WiFi.softAPIP())) {
			DEBUG_WM(F("DNS server did not start"));
		}
	}

	if (!start && _dnsRunning) {
		DEBUG_WM(F("Stopping DNS server"));
		_dnsRunning = false;
		dnsServer->stop();
	}
}

bool AsyncWiFiManager::isAP() {
	// Don't call a WiFi class method to do this, it causes a periodic power surge
	return _isAP;
}

wl_status_t AsyncWiFiManager::_connectWiFi() {
	wl_status_t status = WL_DISCONNECTED;
	if (_router_ssid.length() > 0) {
		if (_router_pass.length() > 0) {
			DEBUG_WM(_router_ssid);
			DEBUG_WM(_router_pass);
			status = WiFi.begin(_router_ssid.c_str(), _router_pass.c_str());
		} else {
			DEBUG_WM(_router_ssid);
			status = WiFi.begin(_router_ssid.c_str());
		}
	} else {
		DEBUG_WM("Connecting with saved credentials:");
		DEBUG_WM(WiFi.psk());
#ifdef ESP32
	    wifi_config_t conf;
	    esp_wifi_get_config((wifi_interface_t)ESP_IF_WIFI_STA, &conf);
	    DEBUG_WM(String(reinterpret_cast<char*>(conf.sta.ssid)));
#else
		DEBUG_WM(WiFi.SSID());
#endif
		status = WiFi.begin();
	}

	DEBUG_WM(status);

	return status;
}

void AsyncWiFiManager::_setupConfigPortal() {
	DEBUG_WM(F(""));
	DEBUG_WM(F("Configuring access point... "));
	DEBUG_WM(_ap_ssid);
	DEBUG_WM(_ap_pass);

	if (_ap_pass.length() < 8 || _ap_pass.length() > 63) {
		// fail passphrase to short or long!
		DEBUG_WM(F("Invalid AccessPoint password. Ignoring"));
		_ap_pass = "";
	}

	//optional soft ip config
	if (_ap_static_ip) {
		DEBUG_WM(F("Custom AP IP/GW/Subnet"));
		WiFi.softAPConfig(_ap_static_ip, _ap_static_gw, _ap_static_sn);
	}

	if (_ap_pass.length() > 0) {
		WiFi.softAP(_ap_ssid.c_str(), _ap_pass.c_str()); //password option
	} else {
		WiFi.softAP(_ap_ssid.c_str());
	}


	delay(500); // Without delay I've seen the IP address blank
	dnsStart(true);

	if (!_portalSet) {
		_portalSet = true;
		/* Setup web pages: root, wifi config pages, SO captive portal detectors and not found. */
		rootApHandler = &server->on("/", [this](AsyncWebServerRequest *req){ this->handleRoot(req); }).setFilter(ON_AP_FILTER);
        wifiApHandler = &server->on("/wifi", HTTP_GET, [this](AsyncWebServerRequest *req){ this->handleWifi(req); }).setFilter(ON_AP_FILTER);
        wifiSaveApHandler = &server->on("/wifisave", [this](AsyncWebServerRequest *req){ this->handleWifiSave(req); }).setFilter(ON_AP_FILTER);
        iApHandler = &server->on("/i", [this](AsyncWebServerRequest *req){ this->handleInfo(req); }).setFilter(ON_AP_FILTER);
        rApHandler = &server->on("/r", [this](AsyncWebServerRequest *req){ this->handleReset(req); }).setFilter(ON_AP_FILTER);
        fwLinkApHandler = &server->on("/fwlink", [this](AsyncWebServerRequest *req){ this->handleRoot(req); }).setFilter(ON_AP_FILTER);
        server->onNotFound([this](AsyncWebServerRequest *req){ this->handleNotFound(req); });
		server->begin(); // Web server start
	}
}

static const char HEX_CHAR_ARRAY[17] = "0123456789ABCDEF";
/**
 * convert char array (hex values) to readable string by seperator
 * buf:           buffer to convert
 * length:        data length
 * strSeperator   seperator between each hex value
 * return:        formated value as String
 */
static String byteToHexString(uint8_t *buf, uint8_t length,
		String strSeperator = "-") {
	String dataString = "";
	for (uint8_t i = 0; i < length; i++) {
		byte v = buf[i] / 16;
		byte w = buf[i] % 16;
		if (i > 0) {
			dataString += strSeperator;
		}
		dataString += String(HEX_CHAR_ARRAY[v]);
		dataString += String(HEX_CHAR_ARRAY[w]);
	}
	dataString.toUpperCase();
	return dataString;
} // byteToHexString

#if !defined(ESP8266)
String getESP32ChipID() {
  uint64_t chipid;
  chipid=ESP.getEfuseMac();//The chip ID is essentially its MAC address(length: 6 bytes).
  int chipid_size = 6;
  uint8_t chipid_arr[chipid_size];
  for (uint8_t i=0; i < chipid_size; i++) {
    chipid_arr[i] = (chipid >> (8 * i)) & 0xff;
  }
  return byteToHexString(chipid_arr, chipid_size, "");
}
#endif

void AsyncWiFiManager::setConnectTimeout(unsigned long timeout) {
	_connectTimeout = timeout;
}

void AsyncWiFiManager::connect() {
	_connect = true;
}

bool AsyncWiFiManager::start() {
	DEBUG_WM(F(""));

	WiFi.setAutoReconnect(true);
	WiFi.persistent(true);
#ifdef ESP8266
	WiFi.setAutoConnect(false);
	stationConnectedHandler = WiFi.onStationModeConnected(std::bind(&AsyncWiFiManager::onConnected, this, std::placeholders::_1));
	stationDisconnectedHandler = WiFi.onStationModeDisconnected(std::bind(&AsyncWiFiManager::onDisconnected, this, std::placeholders::_1));
#else
#if ESP_ARDUINO_VERSION_MAJOR >= 2
	stationConnectedHandler = WiFi.onEvent(std::bind(&AsyncWiFiManager::onConnected, this, std::placeholders::_1, std::placeholders::_2), ARDUINO_EVENT_WIFI_STA_CONNECTED);
	stationDisconnectedHandler = WiFi.onEvent(std::bind(&AsyncWiFiManager::onDisconnected, this, std::placeholders::_1, std::placeholders::_2), ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
#else
	stationConnectedHandler = WiFi.onEvent(std::bind(&AsyncWiFiManager::onConnected, this, std::placeholders::_1, std::placeholders::_2), SYSTEM_EVENT_STA_CONNECTED);
	stationDisconnectedHandler = WiFi.onEvent(std::bind(&AsyncWiFiManager::onDisconnected, this, std::placeholders::_1, std::placeholders::_2), SYSTEM_EVENT_STA_DISCONNECTED);
#endif
#endif

	WiFi.mode(WIFI_STA);
	_isAP = false;

	_connect = true;
	bool started = _start();
	WiFi.setAutoReconnect(false);	// Otherwise connecting to our AP is almost impossible
	_connect = false;

	return started;
}

bool AsyncWiFiManager::_start() {
	if (_sta_static_ip) {
		DEBUG_WM(F("Custom STA IP/GW/Subnet/DNS"));
		WiFi.config(_sta_static_ip, _sta_static_gw, _sta_static_sn, _sta_static_dns1, _sta_static_dns2);
		DEBUG_WM(WiFi.localIP());
	}

	// attempt to connect; should it fail, fall back to AP
	if (_connect) {
		_connectWiFi();
	}

	unsigned long startMs = millis();

	while (!WiFi.isConnected() && (millis() - startMs < _connectTimeout)) {
		delay(10);
	}

	if (WiFi.isConnected()) {
		DEBUG_WM(F("returning"));
		return true;
	}

	DEBUG_WM(WiFi.status());
	DEBUG_WM(WiFi.psk());
#ifdef ESP32
    wifi_config_t conf;
    esp_wifi_get_config((wifi_interface_t)ESP_IF_WIFI_STA, &conf);
    DEBUG_WM(String(reinterpret_cast<char*>(conf.sta.ssid)));
#endif
#ifdef ESP8266
	DEBUG_WM(WiFi.SSID());
	// If we don't do this, the persisted credentials get cleared
	_router_ssid = WiFi.SSID();
	_router_pass = WiFi.psk();
#endif
	_startConfigPortal();
	DEBUG_WM(WiFi.SSID());

	_claim();
	_lastConnectTime = millis();
	_connectRetryTimeout = 10000;	// ms
	_release();

	// Have to do this if we want any automatic connection retries to happen
	return WiFi.isConnected();
}

void AsyncWiFiManager::_claim() {
#ifdef ESP8266
	noInterrupts();
#else
	xSemaphoreTake(loopMutex, portMAX_DELAY);
#endif
}

void AsyncWiFiManager::_release() {
#ifdef ESP8266
	interrupts();
#else
	xSemaphoreGive(loopMutex);
#endif
}

void AsyncWiFiManager::dumpInfo() {
	Serial.printf("WM lastConnectTime=%lu, lastLoopTime=%lu, WiFi status=%d\n", _lastConnectTime, _lastLoopTime, WiFi.status());
}

void AsyncWiFiManager::loop() {
	unsigned long now = millis();

	_lastLoopTime = now;

#ifndef USE_EADNS
	if (isAP()) {
		dnsServer->processNextRequest();
	}
#endif

	if (_connect) {
		DEBUG_WM(F("Connecting to new AP"));
		unsigned long savedTimeout = _connectTimeout;
#ifdef ESP32
		WiFi.disconnect();
#endif
		_start();
		_connect = false;
		if ( _savecallback != NULL) {
		  //todo: check if any custom parameters actually exist, and check if they really changed maybe
		  _savecallback();
		}
	}

	_claim();
	if (_connectRetryTimeout > 0 || _loop_ap_state >= 0 || _loop_scan || _loop_call_connected) {
		if (_connectRetryTimeout > 0) {
			if (now - _lastConnectTime > _connectRetryTimeout) {
				DEBUG_WM(_connectRetryTimeout);
				_lastConnectTime = now;
				_release();
#ifdef ESP32
				WiFi.disconnect();
#endif
				_connectWiFi();
			} else {
				_release();
			}
		} else {
			_release();
		}

		_claim();
		if (_loop_ap_state >= 0) {
			if (_loop_ap_state > 0 && millis() - _apOffTimeout > _loop_ap_state) {
				_loop_ap_state = -1;
				_release();
				_stopConfigPortal();
			} else if (_loop_ap_state == 0) {
				_loop_ap_state = -1;
				_release();
				_startConfigPortal();
			} else {
				_release();
			}
		} else {
			_release();
		}

		_claim();
		if (_loop_call_connected) {
			_loop_call_connected = false;
			_release();
			stopConfigPortal(30000);
			(*_connectedcallback)();
		} else {
			_release();
		}

		_claim();
		if (_loop_scan) {
			_release();
			_scanNetworks();
			_loop_scan = false;
		} else {
			_release();
		}
	} else {
		_release();
	}
}

void AsyncWiFiManager::sendNetworkList(AsyncResponseStream *response) {
	String pager;
	//display networks in page
	for (int i = 0; i < wifiSSIDCount; i++) {
		if (wifiSSIDs[i].duplicate == true) {
			continue; // skip dups
		}

		int quality = getRSSIasQuality(wifiSSIDs[i].RSSI);

		if (_minimumQuality == -1 || _minimumQuality < quality) {
			char locked = ' ';
#if defined(ESP8266)
			if (wifiSSIDs[i].encryptionType != ENC_TYPE_NONE) {
#else
            if (wifiSSIDs[i].encryptionType != WIFI_AUTH_OPEN) {
#endif
				locked = 'l';
			}
			response->printf(HTTP_ITEM, wifiSSIDs[i].SSID.c_str(), locked, quality);
		} else {
			DEBUG_WM(F("Skipping due to quality"));
		}
	}

	if (wifiSSIDCount == 0) {
		response->print(F("No networks found"));
	}
}

void AsyncWiFiManager::copySSIDInfo(wifi_ssid_count_t n) {
	if (n == WIFI_SCAN_FAILED) {
		DEBUG_WM(F("scanNetworks returned: WIFI_SCAN_FAILED!"));
	} else if (n == WIFI_SCAN_RUNNING) {
		DEBUG_WM(F("scanNetworks returned: WIFI_SCAN_RUNNING!"));
	} else if (n < 0) {
		DEBUG_WM(F("scanNetworks failed with unknown error code!"));
	} else if (n == 0) {
		DEBUG_WM(F("No networks found"));
		// page += F("No networks found. Refresh to scan again.");
	} else {
		DEBUG_WM(String("Found ") + n + " SSIDs");
	}

	if (n > 0) {
		/* WE SHOULD MOVE THIS IN PLACE ATOMICALLY */
		if (wifiSSIDs) {
			delete[] wifiSSIDs;
		}

		wifiSSIDs = new WiFiResult[n];
		wifiSSIDCount = n;

		for (wifi_ssid_count_t i = 0; i < n; i++) {
			wifiSSIDs[i].duplicate = false;

#if defined(ESP8266)
			bool res = WiFi.getNetworkInfo(i, wifiSSIDs[i].SSID, wifiSSIDs[i].encryptionType, wifiSSIDs[i].RSSI, wifiSSIDs[i].BSSID, wifiSSIDs[i].channel, wifiSSIDs[i].isHidden);
#else
            bool res = WiFi.getNetworkInfo(i, wifiSSIDs[i].SSID, wifiSSIDs[i].encryptionType, wifiSSIDs[i].RSSI, wifiSSIDs[i].BSSID, wifiSSIDs[i].channel);
#endif
		}

		// RSSI SORT

		// old sort
		for (int i = 0; i < n; i++) {
			for (int j = i + 1; j < n; j++) {
				if (wifiSSIDs[j].RSSI > wifiSSIDs[i].RSSI) {
					std::swap(wifiSSIDs[i], wifiSSIDs[j]);
				}
			}
		}

		// remove duplicates ( must be RSSI sorted )
		if (_removeDuplicateAPs) {
			String cssid;
			for (int i = 0; i < n; i++) {
				if (wifiSSIDs[i].duplicate == true)
					continue;
				cssid = wifiSSIDs[i].SSID;
				for (int j = i + 1; j < n; j++) {
					if (cssid == wifiSSIDs[j].SSID) {
						DEBUG_WM("DUP AP: " + wifiSSIDs[j].SSID);
						wifiSSIDs[j].duplicate = true; // set dup aps to NULL
					}
				}
			}
		}

		WiFi.scanDelete();
	}
}

void AsyncWiFiManager::startConfigPortal() {
	_claim();
	_loop_ap_state = 0;
	_release();
}

void AsyncWiFiManager::startConfigPortal(const char *ssid, const char *pass) {
	_claim();
	setAPCredentials(ssid, pass);
	_loop_ap_state = 0;
	_release();
}

void AsyncWiFiManager::stopConfigPortal(int timeoutMs) {
	_claim();
	_apOffTimeout = millis();
	_loop_ap_state = timeoutMs;	// Turn off after timeoutMs milliseconds
	_release();
}

void AsyncWiFiManager::_scanNetworks() {
	wifi_ssid_count_t n = WiFi.scanNetworks(false);
	copySSIDInfo(n);
}

bool AsyncWiFiManager::_startConfigPortal() {
	if (!_isAP) {
		DEBUG_WM(F("Enable AP"));
		// Do one modal scan
		bool connectAgain = false;

#ifdef ESP8266
		// For ESP8266, need to save the current SSID and password
//		_router_ssid = WiFi.SSID();
//		_router_pass = WiFi.psk();
#else
		wl_status_t status = WiFi.status();
		if (status != WL_CONNECTED) {
			WiFi.disconnect();	// ESP32, can't scan while trying to connect to an AP.
		}
#endif

		_scanNetworks();

		_setupConfigPortal();
		WiFi.mode(WIFI_AP_STA);

		if (WiFi.status() != WL_CONNECTED) {
			_connectWiFi(); // Reconnect/carry on trying to connect
		}

		_isAP = true;

		//notify AP mode state
		if (_apcallback != NULL) {
			_apcallback(this);
		}
	}

	return WiFi.isConnected();
}

void AsyncWiFiManager::_stopConfigPortal() {
	if (_isAP) {
		DEBUG_WM(F("Disable AP"));
		WiFi.enableAP(false);
		_isAP = false;
		dnsStart(false);
		//notify AP mode state
		if (_apcallback != NULL) {
			_apcallback(this);
		}
	}

	if (wifiSSIDs != NULL) {
		delete[] wifiSSIDs;
		wifiSSIDs == NULL;
	}

	wifiSSIDCount = 0;

	if (_portalSet){
		_portalSet = false;
		server->removeHandler(rootApHandler);
		server->removeHandler(wifiApHandler);
		server->removeHandler(wifiSaveApHandler);
		server->removeHandler(iApHandler);
		server->removeHandler(rApHandler);
		server->removeHandler(fwLinkApHandler);
	}
}

void AsyncWiFiManager::setDebugOutput(bool debug) {
	_debug = debug;
}

void AsyncWiFiManager::setAPStaticIPConfig(IPAddress ip, IPAddress gw, IPAddress sn) {
	_ap_static_ip = ip;
	_ap_static_gw = gw;
	_ap_static_sn = sn;
}

void AsyncWiFiManager::setSTAStaticIPConfig(IPAddress ip, IPAddress gw, IPAddress sn, IPAddress dns1, IPAddress dns2) {
	_sta_static_ip = ip;
	_sta_static_gw = gw;
	_sta_static_sn = sn;
	_sta_static_dns1 = dns1;
	_sta_static_dns2 = dns2;
}

void AsyncWiFiManager::setMinimumSignalQuality(int quality) {
	_minimumQuality = quality;
}

/** Handle root or redirect to captive portal */
void AsyncWiFiManager::handleRoot(AsyncWebServerRequest *request) {
	// AJS - maybe we should set a scan when we get to the root???
	// and only scan on demand? timer + on demand? plus a link to make it happen?
	DEBUG_WM(F("Handle root"));
	if (captivePortal(request)) { // If captive portal redirect instead of displaying the page.
		return;
	}

//  delay(20);

	DEBUG_WM(F("Sending Captive Portal"));

	AsyncResponseStream *response = request->beginResponseStream("text/html");

	response->print(_rootHead);
	response->print(FPSTR(HTTP_SCRIPT));
	response->print(FPSTR(HTTP_STYLE));
	response->print(_customHeadHTML);
	response->print(FPSTR(HTTP_HEAD_END));
	response->print("<h1>");
	response->print(_ap_ssid);
	response->print("</h1>");
	response->print(F("<h3>AsyncWiFiManager</h3>"));
	response->print(FPSTR(HTTP_PORTAL_OPTIONS));
	response->print(_customOptionsHTML);
	response->print(FPSTR(HTTP_END));

	request->send(response);
//	AsyncWebServerResponse *response = request->beginResponse(200, "text/html", page);
//
//  response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
//  response->addHeader("Pragma", "no-cache");
//  response->addHeader("Expires", "-1");
//
//	request->send(response);

//	delay(100);

	DEBUG_WM(F("Sent..."));
}

/** Wifi config page handler */
static String oneString("1");

void AsyncWiFiManager::handleWifi(AsyncWebServerRequest *request) {
	DEBUG_WM(F("Handle wifi"));

	String useStatic = request->arg("static");
	_loop_scan = request->hasParam("scan");
	
	AsyncResponseStream *response = request->beginResponseStream("text/html");

	if (_loop_scan) {
		_release();

		response->print(_wifiHead);
		response->print(FPSTR(HTTP_STYLE));
		response->print(_customHeadHTML);
		String refresh = String(HTTP_SCAN_REFRESH);
		refresh.replace("{s}", useStatic);
		response->print(refresh);
		response->print(FPSTR(HTTP_HEAD_END));
		response->print(F("Scanning..."));
		response->print(FPSTR(HTTP_END));

		request->send(response);
		return;
	}

	_release();

	response->print(_wifiHead);
	response->print(FPSTR(HTTP_SCRIPT));
	response->print(FPSTR(HTTP_STYLE));
	response->print(_customHeadHTML);
	response->print(FPSTR(HTTP_HEAD_END));

	//display networks in page
	sendNetworkList(response);
	response->print("<br/>");

	response->print(FPSTR(HTTP_FORM_START));
	char parLength[2];
	// add the extra parameters to the form
	for (int i = 0; i < _paramsCount; i++) {
		if (_params[i] == NULL) {
			break;
		}

		String pitem = FPSTR(HTTP_FORM_PARAM);
		if (_params[i]->getID() != NULL) {
			pitem.replace("{i}", _params[i]->getID());
			pitem.replace("{n}", _params[i]->getID());
			pitem.replace("{p}", _params[i]->getPlaceholder());
			snprintf(parLength, 2, "%d", _params[i]->getValueLength());
			pitem.replace("{l}", parLength);
			pitem.replace("{v}", _params[i]->getValue());
			pitem.replace("{c}", _params[i]->getCustomHTML());
		} else {
			pitem = _params[i]->getCustomHTML();
		}

		response->print(pitem);
	}
	if (_params[0] != NULL) {
		response->print("<br/>");
	}

	if (useStatic == oneString) {

		String item = FPSTR(HTTP_FORM_PARAM);
		item.replace("{i}", "ip");
		item.replace("{n}", "ip");
		item.replace("{p}", "Static IP");
		item.replace("{l}", "15");
		item.replace("{v}", _sta_static_ip.toString());

		response->print(item);

		item = FPSTR(HTTP_FORM_PARAM);
		item.replace("{i}", "gw");
		item.replace("{n}", "gw");
		item.replace("{p}", "Static Gateway");
		item.replace("{l}", "15");
		item.replace("{v}", _sta_static_gw.toString());

		response->print(item);

		item = FPSTR(HTTP_FORM_PARAM);
		item.replace("{i}", "sn");
		item.replace("{n}", "sn");
		item.replace("{p}", "Subnet");
		item.replace("{l}", "15");
		item.replace("{v}", _sta_static_sn.toString());

		response->print(item);

		item = FPSTR(HTTP_FORM_PARAM);
		item.replace("{i}", "dns1");
		item.replace("{n}", "dns1");
		item.replace("{p}", "DNS1");
		item.replace("{l}", "15");
		item.replace("{v}", _sta_static_dns1.toString());

		response->print(item);

		item = FPSTR(HTTP_FORM_PARAM);
		item.replace("{i}", "dns2");
		item.replace("{n}", "dns2");
		item.replace("{p}", "DNS2");
		item.replace("{l}", "15");
		item.replace("{v}", _sta_static_dns2.toString());

		response->print(item);

		response->print("<br/>");
	}

	response->print(FPSTR(HTTP_FORM_END));

	String scanLink = String(FPSTR(HTTP_SCAN_LINK));
	scanLink.replace("{s}", useStatic);
	response->print(scanLink);

	response->print(FPSTR(HTTP_END));

	request->send(response);

	DEBUG_WM(F("Sent config page"));
}

void AsyncWiFiManager::setRouterCredentials(const char *ssid, const char *pass) {
	_router_ssid = ssid;
	_router_pass = pass;
}

void AsyncWiFiManager::setAPCredentials(const char *ssid, const char *pass) {
	_ap_ssid = ssid;
	_ap_pass = pass;
}

/** Handle the WLAN save form and redirect to WLAN config page again */
void AsyncWiFiManager::handleWifiSave(AsyncWebServerRequest *request) {
	DEBUG_WM(F("WiFi save"));

	//SAVE/connect here
	_refresh_info = true;
	_router_ssid = request->arg("s").c_str();
	_router_pass = request->arg("p").c_str();

	//parameters
	for (int i = 0; i < _paramsCount; i++) {
		if (_params[i] == NULL) {
			break;
		}
		//read parameter
		String value = request->arg(_params[i]->getID()).c_str();
		//store it in array
		value.toCharArray(_params[i]->_value, _params[i]->_length);
		DEBUG_WM(F("Parameter"));
		DEBUG_WM(_params[i]->getID());
		DEBUG_WM(value);
	}

	if (request->hasArg("ip")) {
		DEBUG_WM(F("static ip"));
		DEBUG_WM(request->arg("ip"));
		//_sta_static_ip.fromString(request->arg("ip"));
		String ip = request->arg("ip");
		optionalIPFromString(&_sta_static_ip, ip.c_str());
	}
	if (request->hasArg("gw")) {
		DEBUG_WM(F("static gateway"));
		DEBUG_WM(request->arg("gw"));
		String gw = request->arg("gw");
		optionalIPFromString(&_sta_static_gw, gw.c_str());
	}
	if (request->hasArg("sn")) {
		DEBUG_WM(F("static netmask"));
		DEBUG_WM(request->arg("sn"));
		String sn = request->arg("sn");
		optionalIPFromString(&_sta_static_sn, sn.c_str());
	}
	if (request->hasArg("dns1")) {
		DEBUG_WM(F("static DNS 1"));
		DEBUG_WM(request->arg("dns1"));
		String dns1 = request->arg("dns1");
		optionalIPFromString(&_sta_static_dns1, dns1.c_str());
	}
	if (request->hasArg("dns2")) {
		DEBUG_WM(F("static DNS 2"));
		DEBUG_WM(request->arg("dns2"));
		String dns2 = request->arg("dns2");
		optionalIPFromString(&_sta_static_dns2, dns2.c_str());
	}

	AsyncResponseStream *response = request->beginResponseStream("text/html");

	response->print(_wifiSaveHead);
	response->print(FPSTR(HTTP_SCRIPT));
	response->print(FPSTR(HTTP_STYLE));
	response->print(_customHeadHTML);
	response->print(F("<meta http-equiv=\"refresh\" content=\"15; url=/i\">"));
	response->print(FPSTR(HTTP_HEAD_END));
	response->print(FPSTR(HTTP_SAVED));
	response->print(FPSTR(HTTP_END));

	request->send(response);

	DEBUG_WM(F("Sent wifi save page"));

	_connect = true; //signal ready to connect/reset
}

/** Handle the info page */
void AsyncWiFiManager::sendInfo(AsyncResponseStream *response) {
	response->print(F("<dt>Chip ID</dt><dd>"));
#if defined(ESP8266)
	response->print(ESP.getChipId());
#else
  response->print(getESP32ChipID());
#endif
	response->print(F("</dd>"));
	response->print(F("<dt>Flash Chip ID</dt><dd>"));
#if defined(ESP8266)
	response->print(ESP.getFlashChipId());
#else
  response->print(F("N/A for ESP32"));
#endif
	response->print(F("</dd>"));
	response->print(F("<dt>IDE Flash Size</dt><dd>"));
	response->print(ESP.getFlashChipSize());
	response->print(F(" bytes</dd>"));
	response->print(F("<dt>Real Flash Size</dt><dd>"));
#if defined(ESP8266)
	response->print(ESP.getFlashChipRealSize());
#else
  response->print(F("N/A for ESP32"));
#endif
	response->print(F(" bytes</dd>"));
	response->print(F("<dt>Soft AP IP</dt><dd>"));
	response->print(WiFi.softAPIP().toString());
	response->print(F("</dd>"));
	response->print(F("<dt>Soft AP MAC</dt><dd>"));
	response->print(WiFi.softAPmacAddress());
	response->print(F("</dd>"));
	response->print(F("<dt>AP SSID</dt><dd>"));
#if defined(ESP8266)
	struct softap_config conf_current;
	wifi_softap_get_config(&conf_current);
	response->print(String(reinterpret_cast<char*>(conf_current.ssid)));
#else
  wifi_config_t conf_current;
  esp_wifi_get_config(WIFI_IF_AP, &conf_current);
  response->print(String(reinterpret_cast<char*>(conf_current.ap.ssid)));
#endif
	response->print(F("</dd>"));
	response->print(F("<dt>Network SSID</dt><dd>"));
	response->print(WiFi.SSID());
	response->print(F("</dd>"));
	response->print(F("<dt>Station IP</dt><dd>"));
	response->print(WiFi.localIP().toString());
	response->print(F("</dd>"));
	response->print(F("<dt>Station MAC</dt><dd>"));
	response->print(WiFi.macAddress());
	response->print(F("</dd>"));
	response->print(F("</dl>"));
}

void AsyncWiFiManager::handleInfo(AsyncWebServerRequest *request) {
	DEBUG_WM(F("Info"));

	AsyncResponseStream *response = request->beginResponseStream("text/html");

	response->print(_infoHead);
	response->print(FPSTR(HTTP_SCRIPT));
	response->print(FPSTR(HTTP_STYLE));
	response->print(_customHeadHTML);
	if (_connect == true) {
		response->print(F("<meta http-equiv=\"refresh\" content=\"5; url=/i\">"));
	}
	response->print(FPSTR(HTTP_HEAD_END));
	response->print(F("<dl>"));
	if (_connect == true) {
		response->print(F("<dt>Trying to connect</dt><dd>"));
		response->print(WiFi.status());
		response->print(F("</dd>"));
	}

	sendInfo(response);

	response->print(FPSTR(HTTP_END));

	request->send(response);

	DEBUG_WM(F("Sent info page"));
}

/** Handle the reset page */
void AsyncWiFiManager::handleReset(AsyncWebServerRequest *request) {
	DEBUG_WM(F("Reset"));

	AsyncResponseStream *response = request->beginResponseStream("text/html");

	response->print(_resetHead);
	response->print(FPSTR(HTTP_SCRIPT));
	response->print(FPSTR(HTTP_STYLE));
	response->print(_customHeadHTML);
	response->print(FPSTR(HTTP_HEAD_END));
	response->print(F("Module will reset in a few seconds."));
	response->print(FPSTR(HTTP_END));

	request->send(response);

	DEBUG_WM(F("Sent reset page"));
	delay(5000);
#if defined(ESP8266)
	ESP.reset();
#else
    ESP.restart();
#endif
	delay(2000);
}

//removed as mentioned here https://github.com/tzapu/AsyncWiFiManager/issues/114
/*void AsyncWiFiManager::handle204(AsyncWebServerRequest *request) {
 DEBUG_WM(F("204 No Response"));
 request->sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
 request->sendHeader("Pragma", "no-cache");
 request->sendHeader("Expires", "-1");
 request->send ( 204, "text/plain", "");

 }*/

void AsyncWiFiManager::handleNotFound(AsyncWebServerRequest *request) {
	DEBUG_WM(F("Handle not found"));

	if (_connect) {
//	  DEBUG_WM(F("Connecting, returning"));
		return;
	}

	if (captivePortal(request)) { // If captive portal redirect instead of displaying the error page.
		return;
	}

	AsyncResponseStream *response = request->beginResponseStream("text/plain");
	response->setCode(404);
	response->addHeader("Cache-Control", "no-cache, no-store, must-revalidate");
	response->addHeader("Pragma", "no-cache");
	response->addHeader("Expires", "-1");

	response->print("File Not Found\n\n");
	response->print("URI: ");
	response->print(request->url());
	response->print("\nMethod: ");
	response->print((request->method() == HTTP_GET) ? "GET" : "POST");
	response->print("\nArguments: ");
	response->print(request->args());
	response->print("\n");

	for (uint8_t i = 0; i < request->args(); i++) {
		response->print(" ");
		response->print(request->argName(i));
		response->print(": ");
		response->print(request->arg(i));
		response->print("\n");
	}

	request->send(response);
}

/** Redirect to captive portal if we got a request for another domain. Return true in that case so the page handler do not try to handle the request again. */
bool AsyncWiFiManager::captivePortal(AsyncWebServerRequest *request) {
	if (!isIp(request->host())) {
	DEBUG_WM(request->url());
	DEBUG_WM(F("Request redirected to captive portal, AP IP="));
	DEBUG_WM(WiFi.softAPIP());
	DEBUG_WM(F("Client IP="));
	DEBUG_WM(request->client()->localIP());
		AsyncWebServerResponse *response = request->beginResponse(302,
				"text/html", "");
		response->addHeader("Location",
				String("http://") + toStringIp(request->client()->localIP()));
		request->send(response);
		return true;
	}
	return false;
}

//start up config portal callback
void AsyncWiFiManager::setAPCallback(void (*func)(AsyncWiFiManager *myAsyncWiFiManager)) {
	_apcallback = func;
}

//start up save config callback
void AsyncWiFiManager::setSaveConfigCallback(void (*func)(void)) {
	_savecallback = func;
}

#ifdef ESP8266
void AsyncWiFiManager::onStationIP(const WiFiEventStationModeGotIP& evt) {
	_claim();
	_loop_call_connected = true;
	_release();
	DEBUG_WM(toStringIp(evt.ip));
}

void AsyncWiFiManager::onConnected(const WiFiEventStationModeConnected& evt) {
	DEBUG_WM(F("Connected"));
	_claim();
	_connectRetryTimeout = 0;
	_release();
}

void AsyncWiFiManager::onDisconnected(const WiFiEventStationModeDisconnected& evt) {
	DEBUG_WM(F("Disconnected"));
	_claim();
	_lastConnectTime = millis();
	_connectRetryTimeout = 10000;	// ms
	_release();
}
#else
void AsyncWiFiManager::onStationIP(WiFiEvent_t event, WiFiEventInfo_t info) {
	_claim();
	_loop_call_connected = true;
	_release();
	DEBUG_WM(toStringIp(WiFi.localIP()));
}

void AsyncWiFiManager::onConnected(WiFiEvent_t event, WiFiEventInfo_t info) {
	DEBUG_WM(F("Connected"));
	_claim();
	_connectRetryTimeout = 0;
	_release();
}

void AsyncWiFiManager::onDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
	DEBUG_WM(F("Disconnected"));
	_claim();
	_lastConnectTime = millis();
	_connectRetryTimeout = 10000;	// ms
	_release();
}
#endif

//start up connected callback
void AsyncWiFiManager::setConnectedCallback(void (*func)(void)) {
	_connectedcallback = func;
#ifdef ESP8266
	stationGotIPHandler = WiFi.onStationModeGotIP(std::bind(&AsyncWiFiManager::onStationIP, this, std::placeholders::_1));
#else
#if ESP_ARDUINO_VERSION_MAJOR >= 2
	stationGotIPHandler = WiFi.onEvent(std::bind(&AsyncWiFiManager::onStationIP, this, std::placeholders::_1, std::placeholders::_2), ARDUINO_EVENT_WIFI_STA_GOT_IP);
#else
	stationGotIPHandler = WiFi.onEvent(std::bind(&AsyncWiFiManager::onStationIP, this, std::placeholders::_1, std::placeholders::_2), SYSTEM_EVENT_STA_GOT_IP);
#endif
#endif
}

//sets a custom element to add to head, like a new style tag
void AsyncWiFiManager::setCustomHeadHTML(const char *element) {
	_customHeadHTML = element;
}

//sets a custom element to add to options page
void AsyncWiFiManager::setCustomOptionsHTML(const char *element) {
	_customOptionsHTML = element;
}

//if this is true, remove duplicated Access Points - defaut true
void AsyncWiFiManager::setRemoveDuplicateAPs(bool removeDuplicates) {
	_removeDuplicateAPs = removeDuplicates;
}

template<typename Generic>
void AsyncWiFiManager::DEBUG_WM(Generic text) {
	if (_debug) {
		Serial.print("*WM: ");
		Serial.println(text);
	}
}

int AsyncWiFiManager::getRSSIasQuality(int RSSI) {
	int quality = 0;

	if (RSSI <= -100) {
		quality = 0;
	} else if (RSSI >= -50) {
		quality = 100;
	} else {
		quality = 2 * (RSSI + 100);
	}
	return quality;
}

/** Is this an IP? */
bool AsyncWiFiManager::isIp(String str) {
	for (int i = 0; i < str.length(); i++) {
		int c = str.charAt(i);
		if (c != '.' && (c < '0' || c > '9')) {
			return false;
		}
	}
	return true;
}

/** IP to String? */
String AsyncWiFiManager::toStringIp(IPAddress ip) {
	String res = "";
	for (int i = 0; i < 3; i++) {
		res += String((ip >> (8 * i)) & 0xFF) + ".";
	}
	res += String(((ip >> 8 * 3)) & 0xFF);
	return res;
}
