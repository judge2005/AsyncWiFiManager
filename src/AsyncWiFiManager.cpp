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
#if __has_include("esp_memory_utils.h")
#include "esp_memory_utils.h"		// ESP-IDF 5
#else
#include "soc/soc_memory_layout.h"	// ESP-IDF 4
#endif
#else
#include <core_version.h>
#endif
#include <algorithm>
#include <new>
#ifdef WM_LWIP_DNS
#include "lwip/udp.h"
#include "lwip/priv/tcpip_priv.h"
#endif

// Only works out what it would print when debug output is on
#define DEBUG_WM(text) do { if (_debug) _debugPrint(text); } while (0)

static const uint16_t PORTAL_DNS_PORT = 53;

// Looking for a network takes the radio away from the access point, so it's done less while someone is on it
static const unsigned long BUSY_AP_RETRY_MS = 60000;

// Time for the Reset page to be sent before restarting
static const unsigned long RESTART_DELAY_MS = 2000;

// The portal's pages
enum : uint8_t { NO_PAGE, ROOT_PAGE, WIFI_PAGE, SCANNING_PAGE, SAVED_PAGE, INFO_PAGE, RESET_PAGE };

// In the scan list: the first byte of a network with a password has this set, and this byte ends the list
static const uint8_t LOCKED_NETWORK = 0x80;
static const char NETWORKS_END = (char)0xFF;

// Whether text is in flash, where a string literal is. It can't change or go away, so the portal can
// use it where it is rather than keep a copy. Some chips map PSRAM where flash could be, so that's
// ruled out. An ESP8266 keeps literals in RAM, so it always copies.
static bool inFlash(const char *text) {
#ifdef ESP8266
	(void)text;
	return false;
#else
	return esp_ptr_in_drom(text) && !esp_ptr_external_ram(text);
#endif
}

// What the portal keeps of text the program gives it, see inFlash(). NULL for none. Read with the _P
// functions, as an ESP8266 program can pass text in flash.
static const char *keepText(const char *text) {
	if (text == NULL || pgm_read_byte(text) == 0) {
		return NULL;
	}
	if (inFlash(text)) {
		return text;
	}
	size_t size = strlen_P(text) + 1;
	char *copy = (char *)malloc(size);
	if (copy != NULL) {
		memcpy_P(copy, text, size);
	}
	return copy;
}

static void forgetText(const char *text) {
	if (text != NULL && !inFlash(text)) {
		free((void *)text);
	}
}

// Prints text as itself rather than as HTML. Network names can have anything in them.
static void printEscaped(Print &out, const char *text, size_t length) {
	const char *run = text;
	const char *end = text + length;
	for (const char *c = text; c < end; c++) {
		const char *entity;
		switch (*c) {
			case '&': entity = "&amp;"; break;
			case '<': entity = "&lt;"; break;
			case '>': entity = "&gt;"; break;
			case '"': entity = "&quot;"; break;
			case '\'': entity = "&#39;"; break;
			default: continue;
		}
		out.write((const uint8_t *)run, c - run);
		out.print(entity);
		run = c + 1;
	}
	out.write((const uint8_t *)run, end - run);
}

static void printEscaped(Print &out, const char *text) {
	if (text != NULL) {
		printEscaped(out, text, strlen(text));
	}
}

// Prints one of the page templates in the header, with what fill() prints in place of each {x} in it
template<typename Fill>
static void printTemplate(Print &out, PGM_P text, Fill fill) {
	char buffer[32];
	size_t length = 0;
	for (;;) {
		char c = pgm_read_byte(text++);
		bool token = c == '{' && pgm_read_byte(text) != 0 && pgm_read_byte(text + 1) == '}';
		if (c == 0 || token || length == sizeof(buffer)) {
			out.write((const uint8_t *)buffer, length);
			length = 0;
		}
		if (c == 0) {
			break;
		}
		if (token) {
			fill(out, (char)pgm_read_byte(text));
			text += 2;
		} else {
			buffer[length++] = c;
		}
	}
}

// One of the Wi-Fi page's form fields, from HTTP_FORM_PARAM. printValue() prints its value.
template<typename PrintValue>
static void printField(Print &out, const char *id, const char *label, int length, const char *custom, PrintValue printValue) {
	printTemplate(out, HTTP_FORM_PARAM, [&](Print &out, char token) {
		switch (token) {
			case 'i': case 'n': out.print(id); break;
			case 'p': out.print(label); break;
			case 'l': out.print(length); break;
			case 'v': printValue(out); break;
			case 'c': out.print(custom); break;
		}
	});
}

namespace {
	// Prints the part of a page a response is up to: from byte skip on, as much as fits in room. With
	// no room it only counts, to find the page's length.
	class PagePart : public Print {
	public:
		PagePart(uint8_t *buffer, size_t skip, size_t room) : _buffer(buffer), _skip(skip), _room(room) {}

		size_t write(uint8_t c) override {
			return write(&c, 1);
		}

		size_t write(const uint8_t *data, size_t length) override {
			_length += length;
			size_t skipped = std::min(_skip, length);
			_skip -= skipped;
			size_t kept = std::min(_room, length - skipped);
			if (kept > 0) {
				memcpy_P(_buffer + _filled, data + skipped, kept);	// An ESP8266's print(const char*) can pass text in flash
				_filled += kept;
				_room -= kept;
			}
			return length;
		}

		size_t length() const { return _length; }	// The whole page's
		size_t filled() const { return _filled; }

	private:
		uint8_t *_buffer;
		size_t _skip;
		size_t _room;
		size_t _filled = 0;
		size_t _length = 0;
	};
}

// The portal's pages, all in one handler rather than one handler each. It only answers while the
// portal is set up. ESPAsyncWebServer 3 made canHandle() and isRequestHandlerTrivial() const, so
// there's one of each to suit either version.
class AsyncWiFiManager::PortalHandler : public AsyncWebHandler {
public:
	explicit PortalHandler(AsyncWiFiManager *manager) : _manager(manager) {}

	bool canHandle(AsyncWebServerRequest *request) {
		uint8_t page = _manager->_portalPage(request);
#ifndef ASYNCWEBSERVER_VERSION_MAJOR
		// Older servers drop the headers no handler asked for, and read freed memory as they do (in
		// _removeNotInterestingHeaders()), so keep them all, as server->on()'s handlers do. The form's
		// callback is given the request too.
		if (page != NO_PAGE) {
			request->addInterestingHeader("ANY");
		}
#endif
		return page != NO_PAGE;
	}
	bool canHandle(AsyncWebServerRequest *request) const { return _manager->_portalPage(request) != NO_PAGE; }
	void handleRequest(AsyncWebServerRequest *request) { _manager->_handlePortal(request); }
	// Not trivial, so a POST's form is read
	bool isRequestHandlerTrivial() { return false; }
	bool isRequestHandlerTrivial() const { return false; }

private:
	AsyncWiFiManager *_manager;
};

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
	for (int i = 0; i <= length; i++) {
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
#ifdef ESP32
	loopMutex = xSemaphoreCreateMutex();
#endif
}
#else
AsyncWiFiManager::AsyncWiFiManager(AsyncWebServer *server, DNSServer *dns) : server(server), dnsServer(dns) {
#ifdef ESP32
	loopMutex = xSemaphoreCreateMutex();
#endif
}
#endif

AsyncWiFiManager::~AsyncWiFiManager() {
	if (_portalHandler != NULL) {
		server->removeHandler(_portalHandler);
	}
	forgetText(_customHeadHTML);
	forgetText(_customOptionsHTML);
}

void AsyncWiFiManager::setHostname(const char* hostname) {
#if ARDUINO_ESP8266_MAJOR < 3
	WiFi.hostname(hostname);
#else
	WiFi.setHostname(hostname);
#endif
}

void AsyncWiFiManager::addParameter(AsyncWiFiManagerParameter *p) {
	if (_paramsCount >= WIFI_MANAGER_MAX_PARAMS) {
		DEBUG_WM(F("Too many parameters, the most is:"));
		DEBUG_WM(WIFI_MANAGER_MAX_PARAMS);
		return;
	}
	_params[_paramsCount] = p;
	_paramsCount++;
}

#ifdef WM_LWIP_DNS
// The portal's DNS server, which gives every name the access point's address. That's how a phone or
// computer that joins finds the portal: it looks up a name of its own and is sent to the portal.
// DNSServer answers one query each time loop() runs, and lwIP keeps only a few waiting for it, so
// when lots arrive at once, as they do when a phone joins, most are lost, often including the one
// the phone checks for a portal with. This answers each query as it arrives, on lwIP's thread.
namespace {
	const uint8_t DNS_TTL = 5;	// Seconds
	const uint16_t DNS_TYPE_A = 1;
	const uint16_t DNS_TYPE_ANY = 255;
	const uint16_t DNS_CLASS_IN = 1;
	const uint16_t DNS_CLASS_ANY = 255;
	const uint8_t DNS_FORMERR = 1;
	const uint8_t DNS_NOTIMP = 4;

	uint16_t dnsGet16(struct pbuf *p, uint16_t offset) {
		return (pbuf_get_at(p, offset) << 8) | pbuf_get_at(p, offset + 1);
	}

	// An A query gets the access point's address, and any other type an empty answer, rather than an
	// A record it didn't ask for. Other records in a query, such as EDNS's, are left out of the reply
	// rather than making it fail.
	void dnsReceive(void *arg, struct udp_pcb *pcb, struct pbuf *query, const ip_addr_t *addr, u16_t port) {
		uint16_t length = query->tot_len;
		if (length < 12 || (pbuf_get_at(query, 2) & 0x80)) {
			pbuf_free(query);	// Too short, or a reply
			return;
		}

		uint8_t rcode = 0;
		uint16_t questionEnd = 0;	// 0 when the reply has no question
		bool answer = false;

		if ((pbuf_get_at(query, 2) & 0x78) != 0) {
			rcode = DNS_NOTIMP;		// Not a standard query
		} else if (dnsGet16(query, 4) != 1) {
			rcode = DNS_FORMERR;	// Not one question
		} else {
			uint16_t offset = 12;
			while (offset < length && pbuf_get_at(query, offset) != 0 && pbuf_get_at(query, offset) < 64) {
				offset += pbuf_get_at(query, offset) + 1;	// The next label
			}

			// The name's end, then its type and class
			if (offset + 5 <= length && pbuf_get_at(query, offset) == 0) {
				questionEnd = offset + 5;
				uint16_t type = dnsGet16(query, offset + 1);
				uint16_t cls = dnsGet16(query, offset + 3);
				answer = (type == DNS_TYPE_A || type == DNS_TYPE_ANY) && (cls == DNS_CLASS_IN || cls == DNS_CLASS_ANY);
			} else {
				rcode = DNS_FORMERR;
			}
		}

		uint16_t copied = questionEnd > 0 ? questionEnd : 12;	// The header, and the question if there is one
		struct pbuf *reply = pbuf_alloc(PBUF_TRANSPORT, copied + (answer ? 16 : 0), PBUF_RAM);
		if (reply != NULL) {
			uint8_t *data = (uint8_t *)reply->payload;
			pbuf_copy_partial(query, data, copied, 0);
			data[2] = 0x84 | (data[2] & 0x79);	// A reply, authoritative, with the query's opcode and recursion desired
			data[3] = rcode;					// No recursion available
			memset(data + 4, 0, 8);
			data[5] = questionEnd > 0 ? 1 : 0;	// Questions
			data[7] = answer ? 1 : 0;			// Answers

			if (answer) {
				const uint8_t record[] = {
					0xC0, 12,				// The question's name
					0, DNS_TYPE_A,
					0, DNS_CLASS_IN,
					0, 0, 0, DNS_TTL,
					0, 4					// The address's length
				};
				memcpy(data + copied, record, sizeof(record));
				memcpy(data + copied + sizeof(record), &netif_ip4_addr(ip_current_input_netif())->addr, 4);
			}

			udp_sendto(pcb, reply, addr, port);
			pbuf_free(reply);
		}

		pbuf_free(query);
	}

	// For calls into lwIP, which have to be made on its thread
	struct DNSCall {
		struct tcpip_api_call_data call;
		ip_addr_t address;
		uint16_t port;
		struct udp_pcb *pcb;
	};

	err_t dnsStartOnLwIP(struct tcpip_api_call_data *data) {
		DNSCall *call = (DNSCall *)data;
		call->pcb = udp_new_ip_type(IPADDR_TYPE_V4);
		if (call->pcb == NULL) {
			return ERR_MEM;
		}

		err_t err = udp_bind(call->pcb, &call->address, call->port);
		if (err != ERR_OK) {
			udp_remove(call->pcb);
			call->pcb = NULL;
			return err;
		}

		udp_recv(call->pcb, dnsReceive, NULL);
		return ERR_OK;
	}

	err_t dnsStopOnLwIP(struct tcpip_api_call_data *data) {
		udp_remove(((DNSCall *)data)->pcb);
		return ERR_OK;
	}
}
#endif

void AsyncWiFiManager::dnsStart(bool start) {
	//Make DNS control idempotent

	if (start && !_dnsRunning) {
		DEBUG_WM(F("Starting DNS server"));
		_dnsRunning = true;
		DEBUG_WM(WiFi.softAPIP());
#ifdef WM_LWIP_DNS
		// On the access point's address, so it doesn't answer queries from the network the station is on
		IPAddress ip = WiFi.softAPIP();
		DNSCall call;
		IP_ADDR4(&call.address, ip[0], ip[1], ip[2], ip[3]);
		call.port = PORTAL_DNS_PORT;
		call.pcb = NULL;
		tcpip_api_call(dnsStartOnLwIP, &call.call);
		_dnsPcb = call.pcb;
		if (_dnsPcb == NULL) {
			DEBUG_WM(F("DNS server did not start"));
		}
#else
		/* Setup the DNS server redirecting all the domains to the apIP */
#ifdef USE_EADNS
		dnsServer->setErrorReplyCode(AsyncDNSReplyCode::NoError);
#else
		dnsServer->setErrorReplyCode(DNSReplyCode::NoError);
#endif

		dnsServer->setTTL(5);
		if (!dnsServer->start(PORTAL_DNS_PORT, "*", WiFi.softAPIP())) {
			DEBUG_WM(F("DNS server did not start"));
		}
#endif
	}

	if (!start && _dnsRunning) {
		DEBUG_WM(F("Stopping DNS server"));
		_dnsRunning = false;
#ifdef WM_LWIP_DNS
		if (_dnsPcb != NULL) {
			DNSCall call;
			call.pcb = _dnsPcb;
			tcpip_api_call(dnsStopOnLwIP, &call.call);
			_dnsPcb = NULL;
		}
#else
		dnsServer->stop();
#endif
	}
}

bool AsyncWiFiManager::isAP() {
	// Don't call a WiFi class method to do this, it causes a periodic power surge
	return _isAP;
}

wl_status_t AsyncWiFiManager::_connectWiFi() {
	// Copies, as the portal's form or another task can change them
	_claim();
	String ssid = _router_ssid;
	String pass = _router_pass;
	_release();

	wl_status_t status = WL_DISCONNECTED;
	if (ssid.length() > 0) {
		DEBUG_WM(ssid);
		if (pass.length() > 0) {
			status = WiFi.begin(ssid.c_str(), pass.c_str());
		} else {
			status = WiFi.begin(ssid.c_str());
		}
	} else {
		DEBUG_WM(F("Connecting with saved credentials"));
		status = WiFi.begin();
	}

	DEBUG_WM(status);

	return status;
}

void AsyncWiFiManager::_setupConfigPortal() {
	_claim();
	String ssid = _ap_ssid;
	String pass = _ap_pass;
	_release();

	DEBUG_WM(F(""));
	DEBUG_WM(F("Configuring access point... "));
	DEBUG_WM(ssid);

	if (pass.length() > 0 && (pass.length() < 8 || pass.length() > 63)) {
		// fail passphrase to short or long!
		DEBUG_WM(F("Invalid AccessPoint password. Ignoring"));
		pass = "";
	}

	//optional soft ip config
	if (_ap_static_ip) {
		DEBUG_WM(F("Custom AP IP/GW/Subnet"));
		WiFi.softAPConfig(_ap_static_ip, _ap_static_gw, _ap_static_sn);
	}

	if (pass.length() > 0) {
		WiFi.softAP(ssid.c_str(), pass.c_str()); //password option
	} else {
		WiFi.softAP(ssid.c_str());
	}


	delay(500); // Without delay I've seen the IP address blank
	dnsStart(true);

	if (!_portalSet) {
		_portalSet = true;
		if (_portalHandler == NULL) {
			// Kept from then on, as removing it could free it while the server is using it on another
			// task. So a program shouldn't call server->reset() after this.
			_portalHandler = &server->addHandler(new PortalHandler(this));
		}
        server->onNotFound([this](AsyncWebServerRequest *req){ this->handleNotFound(req); });
		server->begin(); // Web server start
	}
}

#if !defined(ESP8266)
// The chip's ID, which is its MAC address
static void printChipID(Print &out) {
	uint64_t chipid = ESP.getEfuseMac();
	char id[13];
	for (int i = 0; i < 6; i++) {
		snprintf(id + 2 * i, 3, "%02X", (unsigned)((chipid >> (8 * i)) & 0xff));
	}
	out.print(id);
}
#endif

void AsyncWiFiManager::setConnectTimeout(unsigned long timeout) {
	_connectTimeout = timeout;
}

void AsyncWiFiManager::connect() {
	_claim();
	_connect = true;
	_release();
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

	_claim();
	_connect = false;	// This connects anyway
	_release();
	bool started = _start(true);
	WiFi.setAutoReconnect(false);	// Otherwise connecting to our AP is almost impossible

	return started;
}

bool AsyncWiFiManager::_start(bool connect) {
	if (_sta_static_ip) {
		DEBUG_WM(F("Custom STA IP/GW/Subnet/DNS"));
		WiFi.config(_sta_static_ip, _sta_static_gw, _sta_static_sn, _sta_static_dns1, _sta_static_dns2);
		DEBUG_WM(WiFi.localIP());
	}

	// attempt to connect; should it fail, fall back to AP
	if (connect) {
		_connectWiFi();
	}

	unsigned long startMs = millis();
	// With no network to join, there's nothing to wait for
	unsigned long timeout = _savedSSID().length() > 0 ? _connectTimeout : 0;

	while (!WiFi.isConnected() && (millis() - startMs < timeout)) {
		delay(10);
	}

	if (WiFi.isConnected()) {
		DEBUG_WM(F("returning"));
		return true;
	}

	DEBUG_WM(WiFi.status());
	DEBUG_WM(_savedSSID());
#ifdef ESP8266
	// If we don't do this, the persisted credentials get cleared
	String ssid = WiFi.SSID();
	String pass = WiFi.psk();
	_claim();
	_router_ssid = ssid;
	_router_pass = pass;
	_release();
#endif
	_startConfigPortal();

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
	_lastLoopTime = millis();

#if !defined(USE_EADNS) && !defined(WM_LWIP_DNS)
	if (isAP()) {
		dnsServer->processNextRequest();
	}
#endif

	// Taken and cleared together, so a connection asked for while this one is made isn't lost
	_claim();
	bool connect = _connect;
	_connect = false;
	_release();

	if (connect) {
		DEBUG_WM(F("Connecting to new AP"));
#ifdef ESP32
		WiFi.disconnect();
#endif
		_start(true);
		if ( _savecallback != NULL) {
		  //todo: check if any custom parameters actually exist, and check if they really changed maybe
		  _savecallback();
		}
	}

	_claim();
	// Read with the lock held, after _start(), so no time another task has set is later than this
	unsigned long now = millis();
	if (_restart && now - _restartTime >= RESTART_DELAY_MS) {
		_release();
		DEBUG_WM(F("Restarting"));
#if defined(ESP8266)
		ESP.reset();
#else
		ESP.restart();
#endif
	}

	if (_connectRetryTimeout > 0 || _loop_ap_state >= 0 || _loop_scan || _loop_call_connected) {
		if (_connectRetryTimeout > 0) {
			if (now - _lastConnectTime > _connectRetryTimeout) {
				bool early = now - _lastConnectTime <= BUSY_AP_RETRY_MS;
				_release();
				if (!early || !_isAP || WiFi.softAPgetStationNum() == 0) {
					DEBUG_WM(_connectRetryTimeout);
					_claim();
					_lastConnectTime = now;
					_release();
#ifdef ESP32
					WiFi.disconnect();
#endif
					_connectWiFi();
				}
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
			_claim();
			_loop_scan = false;
			_release();
		} else {
			_release();
		}
	} else {
		_release();
	}
}

// The scan's networks, as the Wi-Fi page lists them: strongest first, leaving out hidden ones, weak
// ones if there's a minimum quality, and repeats of a name if duplicates are removed. For each, a
// byte with its quality, and LOCKED_NETWORK if it has a password, then its name, ended. NETWORKS_END
// ends the list. It's one block, rather than a String for each, and made once. Pages being sent hold
// on to it, so a scan can replace it while they're sent.
std::shared_ptr<const char> AsyncWiFiManager::_listNetworks(wifi_ssid_count_t n) {
	// The order to list them in. Insertion sort, as there aren't many.
	std::unique_ptr<uint16_t[]> order(new (std::nothrow) uint16_t[n]);
	if (!order) {
		return nullptr;
	}
	size_t size = 1;	// NETWORKS_END
	for (wifi_ssid_count_t i = 0; i < n; i++) {
		int32_t rssi = WiFi.RSSI(i);
		wifi_ssid_count_t j = i;
		for (; j > 0 && WiFi.RSSI(order[j - 1]) < rssi; j--) {
			order[j] = order[j - 1];
		}
		order[j] = i;
		size += WiFi.SSID(i).length() + 2;
	}

	char *list = (char *)malloc(size);
	if (list == NULL) {
		return nullptr;
	}
	char *end = list;
	for (wifi_ssid_count_t i = 0; i < n; i++) {
		uint16_t network = order[i];
		String name = WiFi.SSID(network);
		int quality = getRSSIasQuality(WiFi.RSSI(network));
		if (name.length() == 0) {
			continue;	// Hidden, so it can't be picked from the list
		}
		if (_minimumQuality != -1 && quality <= _minimumQuality) {
			DEBUG_WM(F("Skipping due to quality"));
			continue;
		}
		if (_removeDuplicateAPs) {
			bool listed = false;
			for (const char *other = list; other < end && !listed; other += strlen(other + 1) + 2) {
				listed = strcmp(other + 1, name.c_str()) == 0;
			}
			if (listed) {
				DEBUG_WM(F("DUP AP:"));
				DEBUG_WM(name);
				continue;
			}
		}
#if defined(ESP8266)
		bool locked = WiFi.encryptionType(network) != ENC_TYPE_NONE;
#else
		bool locked = WiFi.encryptionType(network) != WIFI_AUTH_OPEN;
#endif
		*end++ = quality | (locked ? LOCKED_NETWORK : 0);
		memcpy(end, name.c_str(), name.length() + 1);
		end += name.length() + 1;
	}
	*end++ = NETWORKS_END;

	// Gives back what was left out
	char *shrunk = (char *)realloc(list, end - list);
	if (shrunk != NULL) {
		list = shrunk;
	}
	return std::shared_ptr<const char>(list, [](const char *p) { free((void *)p); });
}

void AsyncWiFiManager::_saveScan(wifi_ssid_count_t n) {
	if (n == WIFI_SCAN_FAILED) {
		DEBUG_WM(F("scanNetworks returned: WIFI_SCAN_FAILED!"));
	} else if (n == WIFI_SCAN_RUNNING) {
		DEBUG_WM(F("scanNetworks returned: WIFI_SCAN_RUNNING!"));
	} else if (n < 0) {
		DEBUG_WM(F("scanNetworks failed with unknown error code!"));
	} else if (n == 0) {
		DEBUG_WM(F("No networks found"));
	} else {
		DEBUG_WM(F("Found SSIDs:"));
		DEBUG_WM(n);
	}

	// Made on the side and then swapped in, since a web page could be reading the list
	std::shared_ptr<const char> networks;
	bool failed = n < 0;
	if (n > 0) {
		networks = _listNetworks(n);
		failed = !networks;
	}

	if (n >= 0) {
		WiFi.scanDelete();
	}

	_claim();
	_scanFailed = failed;
	if (!failed) {
		_networks.swap(networks);
	}
	_release();
	// The old list goes here, unless a page being sent still has it
}

void AsyncWiFiManager::startConfigPortal() {
	_claim();
	_loop_ap_state = 0;
	_release();
}

void AsyncWiFiManager::startConfigPortal(const char *ssid, const char *pass) {
	setAPCredentials(ssid, pass);
	startConfigPortal();
}

void AsyncWiFiManager::stopConfigPortal(int timeoutMs) {
	_claim();
	_apOffTimeout = millis();
	_loop_ap_state = timeoutMs;	// Turn off after timeoutMs milliseconds
	_release();
}

void AsyncWiFiManager::_scanNetworks() {
#ifdef ESP32
	if (WiFi.status() != WL_CONNECTED) {
		WiFi.disconnect();	// ESP32, can't scan while trying to connect to an AP.
	}
#endif
	wifi_ssid_count_t n = WiFi.scanNetworks(false);
	_saveScan(n);
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

	// The portal's handler stays, and stops answering
	_portalSet = false;

	std::shared_ptr<const char> networks;
	_claim();
	_networks.swap(networks);
	_scanFailed = false;
	_release();
	// The list goes here, unless a page being sent still has it
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

// Which of the portal's pages a request is for, if any
uint8_t AsyncWiFiManager::_portalPage(AsyncWebServerRequest *request) {
	if (!_portalSet || !ON_AP_FILTER(request)) {
		return NO_PAGE;
	}

	const String &url = request->url();
	if (url == "/" || url == "/fwlink") {
		return ROOT_PAGE;
	} else if (url == "/i") {
		return INFO_PAGE;
	} else if (url == "/wifi") {
		return request->method() == HTTP_GET ? WIFI_PAGE : NO_PAGE;
	} else if (url == "/wifisave") {
		return request->method() == HTTP_POST ? SAVED_PAGE : NO_PAGE;
	} else if (url == "/r") {
		return request->method() == HTTP_POST ? RESET_PAGE : NO_PAGE;	// So a link can't restart it
	}
	return NO_PAGE;
}

void AsyncWiFiManager::_handlePortal(AsyncWebServerRequest *request) {
	switch (_portalPage(request)) {
		case ROOT_PAGE: handleRoot(request); break;
		case WIFI_PAGE: handleWifi(request); break;
		case SAVED_PAGE: handleWifiSave(request); break;
		case INFO_PAGE: handleInfo(request); break;
		case RESET_PAGE: handleReset(request); break;
		default: handleNotFound(request); break;	// The portal closed as the request came in
	}
}

// Sends a page as it's printed rather than printing it into memory first, which took twice the
// page's size at times, as the buffer grew. It's printed once to find its length, then again for
// each part the connection has room for, keeping just that part. A Page holds what could change in
// between, so each part fits the others.
void AsyncWiFiManager::_sendPage(AsyncWebServerRequest *request, const Page &page) {
	PagePart counter(NULL, 0, 0);
	_printPage(counter, page);
	size_t length = counter.length();

	request->send(request->beginResponse("text/html", length, [this, page, length](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
		if (index >= length) {
			return 0;
		}
		size_t room = std::min(maxLen, length - index);
		PagePart part(buffer, index, room);
		_printPage(part, page);
		// Should something the Page doesn't hold have changed, the length sent still has to be right
		memset(buffer + part.filled(), ' ', room - part.filled());
		return room;
	}));
}

// The start of each of the portal's pages, before anything for its head alone and HTTP_HEAD_END
void AsyncWiFiManager::_printHead(Print &out, const char *title, bool script) {
	printTemplate(out, WFM_HTTP_HEAD, [title](Print &out, char) { out.print(title); });
	if (script) {
		out.print(FPSTR(HTTP_SCRIPT));
	}
	out.print(FPSTR(HTTP_STYLE));
	_claim();	// So it can't be replaced as it's printed
	if (_customHeadHTML != NULL) {
		out.print(_customHeadHTML);
	}
	_release();
}

void AsyncWiFiManager::_printPage(Print &out, const Page &page) {
	switch (page.kind) {
	case ROOT_PAGE:
		_printHead(out, "Options", true);
		out.print(FPSTR(HTTP_HEAD_END));
		out.print(F("<h1>"));
		_claim();
		printEscaped(out, _ap_ssid.c_str(), _ap_ssid.length());
		_release();
		out.print(F("</h1>"));
		out.print(FPSTR(HTTP_PORTAL_OPTIONS));
		_claim();
		if (_customOptionsHTML != NULL) {
			out.print(_customOptionsHTML);
		}
		_release();
		break;

	case SCANNING_PAGE:
		_printHead(out, "Configure WiFi", false);
		printTemplate(out, HTTP_SCAN_REFRESH, [&page](Print &out, char) { out.print(page.useStatic ? '1' : '0'); });
		out.print(FPSTR(HTTP_HEAD_END));
		out.print(F("Scanning..."));
		break;

	case WIFI_PAGE: {
		_printHead(out, "Configure WiFi", true);
		out.print(FPSTR(HTTP_HEAD_END));
		_printNetworks(out, page);
		out.print(F("<br/>"));

		out.print(FPSTR(HTTP_FORM_START));
		// add the extra parameters to the form
		for (int i = 0; i < _paramsCount && _params[i] != NULL; i++) {
			AsyncWiFiManagerParameter *param = _params[i];
			if (param->getID() != NULL) {
				printField(out, param->getID(), param->getPlaceholder(), param->getValueLength(), param->getCustomHTML(),
						[param](Print &out) { printEscaped(out, param->getValue()); });
			} else {
				out.print(param->getCustomHTML());
			}
		}
		if (_paramsCount > 0) {
			out.print(F("<br/>"));
		}

		if (page.useStatic) {
			const char *const ids[] = {"ip", "gw", "sn", "dns1", "dns2"};
			const char *const labels[] = {"Static IP", "Static Gateway", "Subnet", "DNS1", "DNS2"};
			const IPAddress *addresses[] = {&_sta_static_ip, &_sta_static_gw, &_sta_static_sn, &_sta_static_dns1, &_sta_static_dns2};
			for (int i = 0; i < 5; i++) {
				const IPAddress *address = addresses[i];
				printField(out, ids[i], labels[i], 15, "", [address](Print &out) { out.print(*address); });
			}
			out.print(F("<br/>"));
		}

		out.print(FPSTR(HTTP_FORM_END));
		printTemplate(out, HTTP_SCAN_LINK, [&page](Print &out, char) { out.print(page.useStatic ? '1' : '0'); });
		break;
	}

	case SAVED_PAGE:
		_printHead(out, "Saved", true);
		out.print(F("<meta http-equiv=\"refresh\" content=\"8; url=/i\">"));
		out.print(FPSTR(HTTP_HEAD_END));
		out.print(FPSTR(HTTP_SAVED));
		break;

	case INFO_PAGE: {
		const char *problem = _problemText(page.problem);
		_printHead(out, "Info", true);
		if (!page.connected && page.ssid[0] != 0) {
			out.print(F("<meta http-equiv=\"refresh\" content=\"5; url=/i\">"));	// Until there's good news
		}
		out.print(FPSTR(HTTP_HEAD_END));
		out.print(F("<dl><dt>WiFi</dt><dd>"));
		if (page.connected) {
			out.print(F("Connected to "));
			printEscaped(out, page.ssid);
			out.print(F(" at "));
			out.print(IPAddress(page.ip));
		} else if (page.ssid[0] == 0) {
			out.print(F("No network chosen"));
		} else if (problem != NULL) {
			out.print(F("Can't join "));
			printEscaped(out, page.ssid);
			out.print(F(": "));
			out.print(problem);
			out.print(F("<br>It keeps trying."));
		} else {
			out.print(F("Connecting to "));
			printEscaped(out, page.ssid);
			out.print(F("..."));
		}
		out.print(F("</dd>"));
		_printInfo(out, page);
		break;
	}

	case RESET_PAGE:
		_printHead(out, "Reset", true);
		out.print(FPSTR(HTTP_HEAD_END));
		out.print(F("The device will restart in a few seconds."));
		break;
	}

	out.print(FPSTR(HTTP_END));
}

void AsyncWiFiManager::_printNetworks(Print &out, const Page &page) {
	if (page.scanFailed) {
		out.print(F("<p>The scan failed, so this list may be out of date. Try again.</p>"));
	}

	const char *network = page.networks.get();
	if (network == NULL || *network == NETWORKS_END) {
		out.print(F("No networks found"));
		return;
	}

	while (*network != NETWORKS_END) {
		uint8_t about = *network++;
		size_t length = strlen(network);
		out.print(F("<div><a href='#p' onclick='c(this)'>"));
		printEscaped(out, network, length);
		out.print(about & LOCKED_NETWORK ? F("</a>&nbsp;<span class='q l'>") : F("</a>&nbsp;<span class='q'>"));
		out.print(about & ~LOCKED_NETWORK);
		out.print(F("%</span></div>"));
		network += length + 1;
	}
}

/** Handle root or redirect to captive portal */
void AsyncWiFiManager::handleRoot(AsyncWebServerRequest *request) {
	DEBUG_WM(F("Handle root"));
	if (captivePortal(request)) { // If captive portal redirect instead of displaying the page.
		return;
	}

	DEBUG_WM(F("Sending Captive Portal"));
	Page page = {};
	page.kind = ROOT_PAGE;
	_sendPage(request, page);
	DEBUG_WM(F("Sent..."));
}

/** Wifi config page handler */
void AsyncWiFiManager::handleWifi(AsyncWebServerRequest *request) {
	DEBUG_WM(F("Handle wifi"));

	Page page = {};
	page.useStatic = request->arg("static") == "1";

	_claim();
	if (request->hasParam("scan")) {
		_loop_scan = true;
	}
	page.kind = _loop_scan ? SCANNING_PAGE : WIFI_PAGE;	// The list is only worth showing once it's finished
	page.networks = _networks;
	page.scanFailed = _scanFailed;
	_release();

	_sendPage(request, page);
	DEBUG_WM(F("Sent config page"));
}

void AsyncWiFiManager::setRouterCredentials(const char *ssid, const char *pass) {
	_claim();
	_router_ssid = ssid;
	_router_pass = pass;
	_release();
}

void AsyncWiFiManager::setAPCredentials(const char *ssid, const char *pass) {
	_claim();
	_ap_ssid = ssid;
	_ap_pass = pass;
	_release();
}

/** Handle the WLAN save form and redirect to WLAN config page again */
void AsyncWiFiManager::handleWifiSave(AsyncWebServerRequest *request) {
	DEBUG_WM(F("WiFi save"));

	if (_saveformcallback != NULL) {
		_saveformcallback(request);
	}

	//parameters
	for (int i = 0; i < _paramsCount && _params[i] != NULL; i++) {
		AsyncWiFiManagerParameter *param = _params[i];
		if (param->getID() == NULL) {
			continue;	// Only HTML
		}
		//read parameter and store it
		const String &value = request->arg(param->getID());
		value.toCharArray(param->_value, param->_length + 1);
		DEBUG_WM(F("Parameter"));
		DEBUG_WM(param->getID());
		DEBUG_WM(value);
	}

	const char *const ids[] = {"ip", "gw", "sn", "dns1", "dns2"};
	IPAddress *addresses[] = {&_sta_static_ip, &_sta_static_gw, &_sta_static_sn, &_sta_static_dns1, &_sta_static_dns2};
	for (int i = 0; i < 5; i++) {
		if (request->hasArg(ids[i])) {
			DEBUG_WM(ids[i]);
			DEBUG_WM(request->arg(ids[i]));
			optionalIPFromString(addresses[i], request->arg(ids[i]).c_str());
		}
	}

	//SAVE/connect here
	_claim();
	_router_ssid = request->arg("s");
	_router_pass = request->arg("p");
	_lastDisconnectReason = 0;	// Whatever went wrong before, it's about to try something new
	_release();

	Page page = {};
	page.kind = SAVED_PAGE;
	_sendPage(request, page);
	DEBUG_WM(F("Sent wifi save page"));

	_claim();
	_connect = true; //signal ready to connect/reset
	_release();
}

/** Handle the info page */
void AsyncWiFiManager::_printInfo(Print &out, const Page &page) {
	out.print(F("<dt>Chip ID</dt><dd>"));
#if defined(ESP8266)
	out.print(ESP.getChipId());
#else
	printChipID(out);
#endif
	out.print(F("</dd>"));
#if defined(ESP8266)
	out.print(F("<dt>Flash Chip ID</dt><dd>"));
	out.print(ESP.getFlashChipId());
	out.print(F("</dd>"));
	out.print(F("<dt>Real Flash Size</dt><dd>"));
	out.print(ESP.getFlashChipRealSize());
	out.print(F(" bytes</dd>"));
#endif
	out.print(F("<dt>Flash Size</dt><dd>"));
	out.print(ESP.getFlashChipSize());
	out.print(F(" bytes</dd>"));
	out.print(F("<dt>Soft AP IP</dt><dd>"));
	out.print(WiFi.softAPIP());
	out.print(F("</dd>"));
	out.print(F("<dt>Soft AP MAC</dt><dd>"));
	out.print(WiFi.softAPmacAddress());
	out.print(F("</dd>"));
	out.print(F("<dt>AP SSID</dt><dd>"));
	// The name's length is given if it's the longest there is, as it isn't ended then
#if defined(ESP8266)
	struct softap_config conf;
	if (wifi_softap_get_config(&conf)) {
		const char *ssid = reinterpret_cast<const char*>(conf.ssid);
		printEscaped(out, ssid, conf.ssid_len > 0 ? std::min((size_t)conf.ssid_len, sizeof(conf.ssid)) : strnlen(ssid, sizeof(conf.ssid)));
	}
#else
	wifi_config_t conf;
	if (esp_wifi_get_config(WIFI_IF_AP, &conf) == ESP_OK) {
		const char *ssid = reinterpret_cast<const char*>(conf.ap.ssid);
		printEscaped(out, ssid, conf.ap.ssid_len > 0 ? std::min((size_t)conf.ap.ssid_len, sizeof(conf.ap.ssid)) : strnlen(ssid, sizeof(conf.ap.ssid)));
	}
#endif
	out.print(F("</dd>"));
	out.print(F("<dt>Network SSID</dt><dd>"));
	if (page.connected) {
		printEscaped(out, page.ssid);
	}
	out.print(F("</dd>"));
	out.print(F("<dt>Station IP</dt><dd>"));
	out.print(IPAddress(page.ip));
	out.print(F("</dd>"));
	out.print(F("<dt>Station MAC</dt><dd>"));
	out.print(WiFi.macAddress());
	out.print(F("</dd>"));
	out.print(F("</dl>"));
}

void AsyncWiFiManager::handleInfo(AsyncWebServerRequest *request) {
	DEBUG_WM(F("Info"));

	// As it is now, so each part of the page says the same
	Page page = {};
	page.kind = INFO_PAGE;
	page.connected = WiFi.isConnected();
	page.problem = _lastDisconnectReason;
	page.ip = WiFi.localIP();
	String ssid = page.connected ? WiFi.SSID() : _savedSSID();	// The network it's on, or trying to join
	strncpy(page.ssid, ssid.c_str(), sizeof(page.ssid) - 1);

	_sendPage(request, page);
	DEBUG_WM(F("Sent info page"));
}

/** Handle the reset page */
void AsyncWiFiManager::handleReset(AsyncWebServerRequest *request) {
	DEBUG_WM(F("Reset"));

	Page page = {};
	page.kind = RESET_PAGE;
	_sendPage(request, page);
	DEBUG_WM(F("Sent reset page"));

	// loop() restarts, as waiting here would hold up the web server, and the page with it
	_claim();
	_restart = true;
	_restartTime = millis();
	_release();
}

void AsyncWiFiManager::handleNotFound(AsyncWebServerRequest *request) {
	DEBUG_WM(F("Handle not found"));

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
		AsyncWebServerResponse *response = request->beginResponse(302, "text/html", "");
		String location = "http://";
		location += request->client()->localIP().toString();
		response->addHeader("Location", location);
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

void AsyncWiFiManager::setSaveFormCallback(void (*func)(AsyncWebServerRequest*)) {
	_saveformcallback = func;
}

// The network it's been told to join, whether it has joined it or not
String AsyncWiFiManager::_savedSSID() {
	_claim();
	String ssid = _router_ssid;
	_release();
	if (ssid.length() > 0) {
		return ssid;
	}
#ifdef ESP32
	wifi_config_t conf;
	char name[sizeof(conf.sta.ssid) + 1] = "";
	if (esp_wifi_get_config((wifi_interface_t)ESP_IF_WIFI_STA, &conf) == ESP_OK) {
		strncpy(name, reinterpret_cast<char*>(conf.sta.ssid), sizeof(conf.sta.ssid));	// Not ended if it's the longest there is
	}
	return String(name);
#else
	return WiFi.SSID();
#endif
}

// The ESP8266 and ESP32 number these the same, but name them differently
#ifdef ESP8266
#define DISCONNECT_REASON(name) WIFI_DISCONNECT_REASON_##name
#else
#define DISCONNECT_REASON(name) WIFI_REASON_##name
#endif

#ifdef ESP8266
void AsyncWiFiManager::onStationIP(const WiFiEventStationModeGotIP& evt) {
	_claim();
	_loop_call_connected = true;
	_release();
	DEBUG_WM(evt.ip);
}

void AsyncWiFiManager::onConnected(const WiFiEventStationModeConnected& evt) {
	DEBUG_WM(F("Connected"));
	_claim();
	_connectRetryTimeout = 0;
	_lastDisconnectReason = 0;
	_release();
}

void AsyncWiFiManager::onDisconnected(const WiFiEventStationModeDisconnected& evt) {
	DEBUG_WM(F("Disconnected"));
	_claim();
	_lastConnectTime = millis();
	_connectRetryTimeout = 10000;	// ms
	if (evt.reason != DISCONNECT_REASON(ASSOC_LEAVE)) {	// That's this library giving up on a try
		_lastDisconnectReason = evt.reason;
	}
	_release();
}
#else
void AsyncWiFiManager::onStationIP(WiFiEvent_t event, WiFiEventInfo_t info) {
	_claim();
	_loop_call_connected = true;
	_release();
	DEBUG_WM(WiFi.localIP());
}

void AsyncWiFiManager::onConnected(WiFiEvent_t event, WiFiEventInfo_t info) {
	DEBUG_WM(F("Connected"));
	_claim();
	_connectRetryTimeout = 0;
	_lastDisconnectReason = 0;
	_release();
}

void AsyncWiFiManager::onDisconnected(WiFiEvent_t event, WiFiEventInfo_t info) {
	DEBUG_WM(F("Disconnected"));
#if ESP_ARDUINO_VERSION_MAJOR >= 2
	uint8_t reason = info.wifi_sta_disconnected.reason;
#else
	uint8_t reason = info.disconnected.reason;
#endif
	_claim();
	_lastConnectTime = millis();
	_connectRetryTimeout = 10000;	// ms
	if (reason != DISCONNECT_REASON(ASSOC_LEAVE)) {	// That's this library giving up on a try
		_lastDisconnectReason = reason;
	}
	_release();
}
#endif

const char* AsyncWiFiManager::connectProblem() {
	return _problemText(_lastDisconnectReason);
}

const char* AsyncWiFiManager::_problemText(uint8_t reason) {
	switch (reason) {
		case 0:
			return NULL;
		case DISCONNECT_REASON(NO_AP_FOUND):
			return "Network not found";
		case DISCONNECT_REASON(AUTH_FAIL):
		case DISCONNECT_REASON(MIC_FAILURE):
		case DISCONNECT_REASON(4WAY_HANDSHAKE_TIMEOUT):
		case DISCONNECT_REASON(HANDSHAKE_TIMEOUT):
			return "Wrong password?";
		default:
			return "Can't connect";
	}
}

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
	const char *html = keepText(element);
	_claim();
	std::swap(html, _customHeadHTML);
	_release();
	forgetText(html);	// Pages print it with the lock held, so none is using it
}

//sets a custom element to add to options page
void AsyncWiFiManager::setCustomOptionsHTML(const char *element) {
	const char *html = keepText(element);
	_claim();
	std::swap(html, _customOptionsHTML);
	_release();
	forgetText(html);	// Pages print it with the lock held, so none is using it
}

//if this is true, remove duplicated Access Points - defaut true
void AsyncWiFiManager::setRemoveDuplicateAPs(bool removeDuplicates) {
	_removeDuplicateAPs = removeDuplicates;
}

template<typename Generic>
void AsyncWiFiManager::_debugPrint(Generic text) {
	Serial.print("*WM: ");
	Serial.println(text);
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
bool AsyncWiFiManager::isIp(const String &str) {
	for (unsigned int i = 0; i < str.length(); i++) {
		int c = str.charAt(i);
		if (c != '.' && (c < '0' || c > '9')) {
			return false;
		}
	}
	return true;
}
