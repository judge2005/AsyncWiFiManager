#ifndef ESPAsyncWiFiManager_h
#define ESPAsyncWiFiManager_h

#if defined(ESP8266)
#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino
#else
#include <WiFi.h>
#include "esp_wps.h"
#define ESP_WPS_MODE WPS_TYPE_PBC
#endif
#include <ESPAsyncWebServer.h>
#ifdef USE_EADNS
#include <ESPAsyncDNSServer.h>    //https://github.com/devyte/ESPAsyncDNSServer
                                  //https://github.com/me-no-dev/ESPAsyncUDP
#else
#include <DNSServer.h>
#endif
#include <memory>

// fix crash on ESP32 (see https://github.com/alanswx/ESPAsyncWiFiManager/issues/44)
#if defined(ESP8266)
typedef int wifi_ssid_count_t;
#else
typedef int16_t wifi_ssid_count_t;
#endif

#if defined(ESP8266)
extern "C" {
#include "user_interface.h"
}
#else
#include <rom/rtc.h>
#endif

const char WFM_HTTP_HEAD[] PROGMEM
		= "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/><title>{v}</title>";
const char HTTP_STYLE[] PROGMEM
		= "<style>.c{text-align: center;} div,input{padding:5px;font-size:1em;} input{width:95%;} body{text-align: center;font-family:verdana;} button{border:0;border-radius:0.3rem;background-color:#1fa3ec;color:#fff;line-height:2.4rem;font-size:1.2rem;width:100%;} .q{float: right;width: 64px;text-align: right;} .l{background: url(\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACAAAAAgCAMAAABEpIrGAAAALVBMVEX///8EBwfBwsLw8PAzNjaCg4NTVVUjJiZDRUUUFxdiZGSho6OSk5Pg4eFydHTCjaf3AAAAZElEQVQ4je2NSw7AIAhEBamKn97/uMXEGBvozkWb9C2Zx4xzWykBhFAeYp9gkLyZE0zIMno9n4g19hmdY39scwqVkOXaxph0ZCXQcqxSpgQpONa59wkRDOL93eAXvimwlbPbwwVAegLS1HGfZAAAAABJRU5ErkJggg==\") no-repeat left center;background-size: 1em;}</style>";
const char HTTP_SCRIPT[] PROGMEM
		= "<script>function c(l){document.getElementById('s').value=l.innerText||l.textContent;document.getElementById('p').focus();};function t(){var x=document.getElementById('p');if(x.type === 'password'){x.type='text';}else{x.type='password';}}</script>";
const char HTTP_HEAD_END[] PROGMEM
		= "</head><body><div style='text-align:left;display:inline-block;min-width:260px;'>";
const char HTTP_PORTAL_OPTIONS[] PROGMEM
		= "<form action=\"/wifi\" method=\"get\"><button>Configure WiFi</button></form><br/><form action=\"/0wifi\" method=\"get\"><button>Configure WiFi (No Scan)</button></form><br/><form action=\"/i\" method=\"get\"><button>Info</button></form><br/><form action=\"/r\" method=\"post\"><button>Reset</button></form>";
const char HTTP_ITEM[]
		= "<div><a href='#p' onclick='c(this)'>%s</a>&nbsp;<span class='q %c'>%d%</span></div>";
const char HTTP_FORM_START[] PROGMEM
		= "<form method='get' action='wifisave'><input id='s' name='s' autocapitalize='none' length=32 placeholder='SSID'><br/><input id='p' name='p' length=64 type='password' placeholder='password'><p><input type='checkbox' style='width:auto' onclick='t()'><label for='p'>Show Password</label><br>";
const char HTTP_FORM_PARAM[] PROGMEM
		= "<br/><input id='{i}' name='{n}' length={l} placeholder='{p}' value='{v}' {c}>";
const char HTTP_FORM_END[] PROGMEM
		= "<br/><button type='submit'>save</button></form>";
const char HTTP_SCAN_LINK[] PROGMEM
		= "<br/><div class=\"c\"><a href=\"/wifi?scan=1\">Scan</a></div>";
const char HTTP_SAVED[] PROGMEM
		= "<div>Credentials Saved<br />Trying to connect ESP to network.<br />If it fails reconnect to AP to try again</div>";
const char HTTP_END[] PROGMEM = "</div></body></html>";

#define WIFI_MANAGER_MAX_PARAMS 10

class AsyncWiFiManagerParameter {
public:
	AsyncWiFiManagerParameter(const char *custom);
	AsyncWiFiManagerParameter(const char *id, const char *placeholder,
			const char *defaultValue, int length);
	AsyncWiFiManagerParameter(const char *id, const char *placeholder,
			const char *defaultValue, int length, const char *custom);

	const char* getID();
	const char* getValue();
	const char* getPlaceholder();
	int getValueLength();
	const char* getCustomHTML();
private:
	const char *_id;
	const char *_placeholder;
	char *_value;
	int _length;
	const char *_customHTML;

	void init(const char *id, const char *placeholder, const char *defaultValue,
			int length, const char *custom);

	friend class AsyncWiFiManager;
};

class WiFiResult {
public:
	bool duplicate;
	String SSID;
	uint8_t encryptionType;
	int32_t RSSI;
	uint8_t *BSSID;
	int32_t channel;
	bool isHidden;

	WiFiResult() {
	}
};

class AsyncWiFiManager {
public:
#ifdef USE_EADNS
	AsyncWiFiManager(AsyncWebServer * server, AsyncDNSServer *dns);
#else
	AsyncWiFiManager(AsyncWebServer * server, DNSServer *dns);
#endif
	~AsyncWiFiManager() {}

	void loop();
	bool start();
	void connect();

	void setHostname(const char* hostname);
	void setDebugOutput(bool debug);
	void setRemoveDuplicateAPs(bool flag);
	void setCustomOptionsHTML(const char* html);
	void setCustomHeadHTML(const char* html);
	void addParameter(AsyncWiFiManagerParameter *p);

	void setMinimumSignalQuality(int quality = 8);
	//sets a custom ip /gateway /subnet configuration
	void setAPStaticIPConfig(IPAddress ip, IPAddress gw, IPAddress sn);
	//sets config for a static IP
	void setSTAStaticIPConfig(IPAddress ip, IPAddress gw, IPAddress sn, IPAddress dns1=(uint32_t)0x00000000, IPAddress dns2=(uint32_t)0x00000000);

	void setSaveConfigCallback(void (*func)(void));
	void setConnectedCallback(void (*func)(void));
	void setConnectTimeout(unsigned long timeout);
	void setAPCallback(void (*func)(AsyncWiFiManager *myAsyncWiFiManager));

	void setRouterCredentials(const char* ssid, const char* pass);
	void setAPCredentials(const char* ssid, const char* pass);

	void stopConfigPortal(int timeoutMs=1);
	void startConfigPortal();
	void startConfigPortal(const char* ssid, const char* pass);
	bool isAP();

	void dumpInfo();

private:
	bool _debug = false;

	void _stopConfigPortal();
	bool _startConfigPortal();
	void _setupConfigPortal();
	wl_status_t _connectWiFi();
	void _scanNetworks();
	bool _start();
	void _claim();
	void _release();
	void _cacheHeads();

	AsyncWebServer *server;
#ifdef USE_EADNS
    AsyncDNSServer *dnsServer;
#else
    DNSServer *dnsServer;
#endif

    unsigned long _connectTimeout = 0;	// After initial connect attempt, wait this long for a connection to be created - can prevent creation of AP
    unsigned long _connectRetryTimeout = 0;
    unsigned long _apOffTimeout = 0;
    unsigned long _lastConnectTime = 0;
    unsigned long _lastLoopTime = 0;

    bool _connect = false;			// Config Portal requested connection
	bool _isAP = false;				// True if AP is enabled
	int _loop_ap_state = -1;
	bool _loop_scan = false;
	bool _dnsRunning = false;		// Make calls to dns server idempotent
	String _router_ssid;
	String _router_pass;
	String _ap_ssid;
	String _ap_pass;

	bool   _portalSet = false;		// Enforce single initialization of ConfigPortal

	AsyncWebHandler* rootApHandler;
	AsyncWebHandler* wifiApHandler;
	AsyncWebHandler* wifi0ApHandler;
	AsyncWebHandler* wifiSaveApHandler;
	AsyncWebHandler* iApHandler;
	AsyncWebHandler* rApHandler;
	AsyncWebHandler* fwLinkApHandler;
	
	bool   _refresh_info = true;	// Refresh the info HTML when true
	String _customHeadHTML;
	String _customOptionsHTML;
	String _wifiSaveHead;
	String _wifiHead;
	String _rootHead;
	String _infoHead;
	String _resetHead;

	IPAddress _ap_static_ip;
	IPAddress _ap_static_gw;
	IPAddress _ap_static_sn;
	IPAddress _sta_static_ip;
	IPAddress _sta_static_gw;
	IPAddress _sta_static_sn;
	IPAddress _sta_static_dns1= (uint32_t)0x00000000;
	IPAddress _sta_static_dns2= (uint32_t)0x00000000;

	void sendInfo(AsyncResponseStream *response);

	void handleRoot(AsyncWebServerRequest*);
	void handleWifi(AsyncWebServerRequest*, bool scan);
	void handleWifiSave(AsyncWebServerRequest*);
	void handleInfo(AsyncWebServerRequest*);
	void handleReset(AsyncWebServerRequest*);
	void handleNotFound(AsyncWebServerRequest*);
	void handle204(AsyncWebServerRequest*);
	bool captivePortal(AsyncWebServerRequest*);
	void dnsStart(bool start);
#ifdef ESP8266
	void onStationIP(const WiFiEventStationModeGotIP& evt);
	void onConnected(const WiFiEventStationModeConnected& evt);
	void onDisconnected(const WiFiEventStationModeDisconnected& evt);
#else
	SemaphoreHandle_t loopMutex;
	void onStationIP(WiFiEvent_t event, WiFiEventInfo_t info);
	void onConnected(WiFiEvent_t event, WiFiEventInfo_t info);
	void onDisconnected(WiFiEvent_t event, WiFiEventInfo_t info);
#endif

	// DNS server
	const byte DNS_PORT = 53;

	// Scanned WiFi access point SSIDs
	WiFiResult *wifiSSIDs = 0;
	wifi_ssid_count_t wifiSSIDCount = 0;
	bool _removeDuplicateAPs = true;
	int  _minimumQuality     = -1;
	bool shouldscan          = false;

	void          sendNetworkList(AsyncResponseStream *response);
	static int    getRSSIasQuality(int RSSI);
	static bool   isIp(String str);
	static String toStringIp(IPAddress ip);
	void          copySSIDInfo(wifi_ssid_count_t n);

	void (*_apcallback)(AsyncWiFiManager*) = NULL;	// Call when AP mode state changes

	void (*_savecallback)(void) = NULL;				// Call when ConfigPortal saves data

    bool _loop_call_connected = false;					// call the connected callback when true - avoids re-entrancy issues
	void (*_connectedcallback)(void) = NULL;		// Call when we have an IP address
#ifdef ESP8266
	WiFiEventHandler stationGotIPHandler;
	WiFiEventHandler stationConnectedHandler;
	WiFiEventHandler stationDisconnectedHandler;
#else
	WiFiEventId_t stationGotIPHandler;
	WiFiEventId_t stationConnectedHandler;
	WiFiEventId_t stationDisconnectedHandler;
#endif

	int _paramsCount = 0;
	AsyncWiFiManagerParameter *_params[WIFI_MANAGER_MAX_PARAMS];

	template<typename Generic> void DEBUG_WM(Generic text);

	template<class T>
	auto optionalIPFromString(T *obj, const char *s) ->
			decltype( obj->fromString(s) ) {
		return obj->fromString(s);
	}
	auto optionalIPFromString(...) -> bool {
		DEBUG_WM(
				"NO fromString METHOD ON IPAddress, you need ESP8266 core 2.1.0 or newer for Custom IP configuration to work.");
		return false;
	}
};

#endif
