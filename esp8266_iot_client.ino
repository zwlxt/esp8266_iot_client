#include <ESP8266WiFi.h>
#include <ESP8266httpUpdate.h>
#include <ESP8266HTTPClient.h>
#include <AsyncMqttClient.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <FS.h>
#include "webpage.h"

#define DBG_SERIAL Serial
#define _LOG(...) DBG_SERIAL.print(__VA_ARGS__)
#define _LOGLN(...) DBG_SERIAL.println(__VA_ARGS__)

const uint8_t PIN_BLINK = LED_BUILTIN;
const uint8_t PIN_VALVE_EN = 4;
const int VALVE_TRIGGER_LEVEL = HIGH;

const int CONFIG_JSON_SIZE = JSON_OBJECT_SIZE(6) + 440;
const char *CONFIG_FILE_PATH = "/config.json";

AsyncMqttClient mqttClient;
WiFiEventHandler gotIPHandler;
WiFiEventHandler stationModeDisconnectedHandler;
AsyncWebServer webServer(80);
int scanResultCount;
int restartCount = 0;
unsigned long wifiLastConnection;
unsigned long valveLastOff;
bool requestFirmwareUpdate;
bool requestReboot;
bool requestSyncClock;
AsyncMqttClientDisconnectReason mqttClientDisconnectReason;

struct Config {
	String server;
	uint16_t port;
	String clientId;
	String username;
	String password;
	String topic;
	String otaSource;
} config;

struct State {
	bool valvePower;
	String lastResetReason;
	uint32_t freeHeap;
	IPAddress localIP;
} state;

void setup() {
	DBG_SERIAL.begin(115200);
	_LOGLN();
	_LOG("[SYS] Reset reason: ");
	state.lastResetReason = ESP.getResetReason();
	_LOGLN(state.lastResetReason);
	if (state.lastResetReason == "Exception")
		ESP.restart();

	pinMode(PIN_BLINK, OUTPUT);
	pinMode(PIN_VALVE_EN, OUTPUT);

	updateState();

	_LOGLN("[SYS] Mounting FS...");
	SPIFFS.begin();

	if (!loadConfig()) {
		loadDefaultConfig();
	} else {
		_LOGLN("[CONFIG] Found config file");
	}
	_LOG("[CONFIG] Server: ");
	_LOGLN(config.server);
	_LOG("[CONFIG] port: ");
	_LOGLN(config.port);

	_LOGLN("[SYS] Starting web server");
	webServer.on("/", HTTP_GET, indexPageHandler);
	webServer.on("/server", HTTP_GET, serverConfigPageHandler);
	webServer.on("/wifi", HTTP_GET, wifiConfigPageHandler);
	webServer.on("/scanwifi", HTTP_GET, scanWiFiPageHandler);
	webServer.on("/update", HTTP_GET, updatePageHandler);
	webServer.on("/dbg/config.json", HTTP_GET, configFileHandler);
	webServer.on("/action/serverconfig", HTTP_POST, serverConfigActionHandler);
	webServer.on("/action/wificonfig", HTTP_POST, wifiConfigActionHandler);
	webServer.on("/action/update", HTTP_POST, updateActionHandler);
	webServer.on("/action/restart", HTTP_GET, restartActionHandler);
	webServer.onNotFound([](AsyncWebServerRequest *request) {
		request->send(404);
	});
	webServer.begin();

	setupMQTTClient();

	gotIPHandler = WiFi.onStationModeGotIP(onGotIP);
	stationModeDisconnectedHandler = WiFi.onStationModeDisconnected(onWiFiDisconnected);
	WiFi.persistent(true);
	WiFi.setAutoReconnect(true);
	if (WiFi.SSID().length() == 0) {
		_LOGLN("[WiFi] No credential");
		setupNetwork();
	} else {
		_LOG("[WIFI] Connecting to ");
		_LOGLN(WiFi.SSID());
		WiFi.mode(WIFI_STA);
		WiFi.begin();
	}

	scanResultCount = WiFi.scanNetworks(false, false);
}

void loop() {
	digitalWrite(LED_BUILTIN, LOW);
	delay(1000);
	digitalWrite(LED_BUILTIN, HIGH);
	delay(2000);

	updateState();

	// Auto close valve
	if (!state.valvePower) {
		valveLastOff = millis();
	} else {
		if (millis() - valveLastOff > 300 * 1000) { // if the valve is on for more than 5min
			_LOGLN("[SYS] Auto closing");
			handleEmergency();
		}
	}

	// Handling sync clock request
	if (requestSyncClock) {
		syncClock();
		requestSyncClock = false;
	}

	// Handling reboot request
	if (requestReboot || restartCount > 10) {
		_LOGLN("[SYS] Rebooting...");
		handleEmergency();
		ESP.restart();
	}

	// Reconnect
	if (!WiFi.isConnected()) {
		_LOGLN("[WIFI] Reconnecting...");
		handleEmergency();
		WiFi.reconnect();
	}

	if (WiFi.isConnected() && !mqttClient.connected()) {
		_LOGLN("[MQTT] Reconnecting ...");
		mqttClient.connect();
	}

	// This will restart the chip when WiFi reconnecting doesn't work
	if (WiFi.SSID().length() != 0) { // WiFi credential exists
		if (WiFi.isConnected()) {
			wifiLastConnection = millis();
		} else {
			if (millis() - wifiLastConnection > 10000 && WiFi.getMode() != WIFI_AP_STA) { // Disconnected for 10s
				setupNetwork();
			}
		}
	}

	// OTA
	if (requestFirmwareUpdate) {
		handleEmergency();
		saveConfig();
		updateFirmware();
	}
}

void setupNetwork() {
	_LOGLN("[WIFI] Starting network configuration");
	WiFi.mode(WIFI_AP_STA);
	char ssid[16];
	snprintf(ssid, sizeof(ssid), "DEVICE_%d", ESP.getChipId());
	WiFi.softAP(ssid);
	_LOGLN("[WIFI] Starting smart config");
	WiFi.beginSmartConfig();

}

void setupMQTTClient() {
	_LOGLN("[SYS] Setting up MQTT client");
	mqttClient.setServer(config.server.c_str(), config.port);
	mqttClient.setClientId(config.clientId.c_str());
	mqttClient.setCredentials(config.username.c_str(), config.password.c_str());
	mqttClient.onConnect(onMqttConnected);
	mqttClient.onMessage(onMqttMessage);
	mqttClient.onDisconnect(onMqttDisconnected);
}

void onGotIP(const WiFiEventStationModeGotIP event) {
	_LOG("[WIFI] Assigned IP: ");
	_LOGLN(event.ip);
	WiFi.stopSmartConfig();
	WiFi.softAPdisconnect(false);
	WiFi.mode(WIFI_STA);
	_LOG("[MQTT] Connecting to ");
	_LOG(config.server);
	_LOG(":");
	_LOGLN(config.port);
	mqttClient.connect();
	requestSyncClock = true;
}

void onWiFiDisconnected(const WiFiEventStationModeDisconnected event) {
	_LOG("[WIFI] Disconnected, reason: ");
	_LOGLN(event.reason);

	if (event.reason <= 200)
		restartCount++;

	if (WiFi.getMode() != WIFI_AP_STA && WiFi.status() != WL_CONNECTED) {
		setupNetwork();
	}
}

void onMqttConnected(bool /*sessionPresent*/) {
	_LOGLN("[MQTT] Connected");
	mqttResponse(2, "");
	mqttClient.subscribe(config.topic.c_str(), 1);
	controlValve(false);
}

void onMqttMessage(char* /*topic*/, char* payload, AsyncMqttClientMessageProperties /*properties*/, size_t len, size_t /*index*/, size_t /*total*/) {
	_LOG("[MQTT] Receiving: ");
	String plStr(payload);
	plStr = plStr.substring(0, len);
	_LOGLN(plStr);
	mqttParseMessage(plStr);
}

void onMqttDisconnected(AsyncMqttClientDisconnectReason reason) {
	_LOG("[MQTT] Disconnected, reason: ");
	_LOGLN((int8_t) reason);
	if (reason != AsyncMqttClientDisconnectReason::TCP_DISCONNECTED)
		mqttClientDisconnectReason = reason;
}

void indexPageHandler(AsyncWebServerRequest *request) {
	request->send_P(200, "text/html", INDEX_PAGE, indexTemplateProcessor);
}

String indexTemplateProcessor(const String &var) {
	if (var == "heap") {
		uint32_t heap = state.freeHeap;
		char result[6];
		ltoa((long) heap, result, 10);
		return result;
	}
	if (var == "reset_reason")
		return state.lastResetReason;
	if (var == "ssid")
		return WiFi.SSID();
	if (var == "rssi") {
		uint32_t rssi = WiFi.RSSI();
		char result[6];
		ltoa((long) rssi, result, 10);
		return result;
	}
	if (var == "mqtt_status")
		return mqttClient.connected() ? "Connected" : "Disconnected " + String((int)mqttClientDisconnectReason);
	return String();
}

void configFileHandler(AsyncWebServerRequest *request) {
	request->send(SPIFFS, "/config.json", "application/json", false);
}

void serverConfigPageHandler(AsyncWebServerRequest *request) {
	request->send_P(200, "text/html", SERVER_CONFIG_PAGE, serverConfigTemplateProcessor);
}

String serverConfigTemplateProcessor(const String &var) {
	if (var == "server")
		return config.server;
	if (var == "port") {
		char result[6];
		itoa(config.port, result, 10);
		return result;
	}
	if (var == "clid")
		return config.clientId;
	if (var == "username")
		return config.username;
	if (var == "password")
		return String();
	if (var == "nopass")
		if (config.password.length() == 0)
			return "checked";
	if (var == "topic")
		return config.topic;
	return String();
}

void serverConfigActionHandler(AsyncWebServerRequest *request) {
	AsyncWebParameter *param;
	String v;
	if (request->hasParam("server", true)) {
		param = request->getParam("server", true);
		v = param->value();
		if (v.length() > 0 && v.length() < 64) {
			config.server = v;
		} else {
			actionFailedHandler(request);
			return;
		}
	}
	if (request->hasParam("port", true)) {
		param = request->getParam("port", true);
		if (int port = atoi(param->value().c_str())) {
			if (port > 0 && port <= 65535) {
				config.port = port;
			}
		} else {
			actionFailedHandler(request);
			return;
		}
	}
	if (request->hasParam("client_id", true)) {
		param = request->getParam("client_id", true);
		v = param->value();
		if (v.length() > 0 && v.length() < 64) {
			config.clientId = v;
		} else {
			actionFailedHandler(request);
			return;
		}
	}
	if (request->hasParam("username", true)) {
		param = request->getParam("username", true);
		v = param->value();
		if (v.length() < 64) { // Username can be empty. If so, no credential is used
			config.username = v;
		} else {
			actionFailedHandler(request);
			return;
		}
	}
	bool nopass = false;
	if (request->hasParam("nopass", true)) {
		param = request->getParam("nopass", true);
		if (param->value() == "on") {
			nopass = true;
			config.password = "";
		}
	}
	if (!nopass) { // nopass is unchecked
		if (request->hasParam("password", true)) {
			param = request->getParam("password", true);
			v = param->value();
			if (v.length() > 0 && v.length() < 64) {
				config.password = v;
			}
		}
	}
	if (request->hasParam("topic", true)) {
		param = request->getParam("topic", true);
		v = param->value();
		if (v.length() > 0 && v.length() < 64) {
			config.topic = v;
		} else {
			actionFailedHandler(request);
			return;
		}
	}
	if (saveConfig()) {
		actionSuccessHandler(request);
		requestReboot = true;
	} else
		actionFailedHandler(request);
}

void wifiConfigPageHandler(AsyncWebServerRequest *request) {
	request->send_P(200, "text/html", WIFI_CONFIG_PAGE, wifiConfigTemplateProcessor);
}

String wifiConfigTemplateProcessor(const String &var) {
	if (var == "ssid")
		return WiFi.SSID();
	if (var == "password")
		return String();
	if (var == "nopass") {
		if (WiFi.psk().length() == 0) {
			return "checked";
		}
	}
	return String();
}

void wifiConfigActionHandler(AsyncWebServerRequest *request) {
	AsyncWebParameter *param;
	String v, ssid, password;
	bool nopass = false;
	if (request->hasParam("ssid", true)) {
		param = request->getParam("ssid", true);
		v = param->value();
		if (v.length() > 0) {
			ssid = v;
		} else {
			actionFailedHandler(request);
			return;
		}
	}
	if (request->hasParam("nopass", true)) {
		param = request->getParam("nopass", true);
		if (param->value() == "on") {
			nopass = true;
		}
	}
	if (!nopass) {
		if (request->hasParam("password", true)) {
			param = request->getParam("password", true);
			password = param->value();
		}
	}
	actionSuccessHandler(request);
	WiFi.begin(ssid.c_str(), password.c_str());
}

void scanWiFiPageHandler(AsyncWebServerRequest *request) {
	request->send_P(200, "text/html", SCAN_WIFI_PAGE, scanWiFiTemplateProcessor);
}

String scanWiFiTemplateProcessor(const String var) {
	if (var == "wifi") {
		String scanResult = "";
		if (scanResultCount > 5)
			scanResultCount = 5;
		for (int i = 0; i < scanResultCount; i++) {
			scanResult += "<tr><td>";
			scanResult += WiFi.SSID(i);
			scanResult += "</td><td>";
			scanResult += (WiFi.encryptionType(i) == ENC_TYPE_NONE) ? " " : "*";
			scanResult += "</td><td>";
			scanResult += WiFi.RSSI(i);
			scanResult += "</td><td>";
			scanResult += WiFi.BSSIDstr(i);
			scanResult += "</td></tr>";
		}
		return scanResult;
	}
	return String();
}

void updatePageHandler(AsyncWebServerRequest *request) {
	request->send_P(200, "text/html", UPDATE_PAGE, updatePageTemplateProcessor);
}

String updatePageTemplateProcessor(const String var) {
	if (var == "fw_src") {
		return config.otaSource;
	}
	return String();
}

void updateActionHandler(AsyncWebServerRequest *request) {
	if (request->hasParam("fw_src", true)) {
		AsyncWebParameter *param = request->getParam("fw_src", true);
		config.otaSource = param->value();
		actionSuccessHandler(request);
		requestFirmwareUpdate = true;
		return;
	}
	actionFailedHandler(request);
}

void restartActionHandler(AsyncWebServerRequest *request) {
	actionSuccessHandler(request);
	requestReboot = true;
}

void actionSuccessHandler(AsyncWebServerRequest *request) {
	request->send_P(200, "text/html", SUCCESS_PAGE);
}

void actionFailedHandler(AsyncWebServerRequest *request) {
	request->send_P(200, "text/html", FAILED_PAGE);
}

bool saveConfig() {
	_LOGLN("[CONFIG] Saving config");
	File file = SPIFFS.open(CONFIG_FILE_PATH, "w+");
	if (!file) {
		_LOGLN("[CONFIG] Unable to open config file");
		return false;
	}
	StaticJsonBuffer<CONFIG_JSON_SIZE> jsonBuffer;
	JsonObject &configJson = jsonBuffer.createObject();
	configJson["server"] = config.server;
	configJson["port"] = config.port;
	configJson["clid"] = config.clientId;
	configJson["username"] = config.username;
	configJson["password"] = config.password;
	configJson["topic"] = config.topic;
	configJson["ota_src"] = config.otaSource;
	configJson.printTo(file);
	file.close();
	return true;
}

bool loadConfig() {
	_LOGLN("[CONFIG] Loading config");
	if (!SPIFFS.exists(CONFIG_FILE_PATH)) {
		_LOGLN("[CONFIG] Config file does not exist");
		return false;
	}
	File file = SPIFFS.open(CONFIG_FILE_PATH, "r");
	if (!file) {
		_LOGLN("[CONFIG] Cannot read config file");
		return false;
	}

	StaticJsonBuffer<CONFIG_JSON_SIZE> jsonBuffer;
	JsonObject &configJson = jsonBuffer.parseObject(file.readString());
	file.close();
	if (!configJson.success()) {
		_LOGLN("[CONFIG] Cannot decode config file");
		return false;
	}
	config.server = (const char*)configJson["server"];
	config.port = configJson["port"];
	config.clientId = (const char*)configJson["clid"];
	config.username = (const char*)configJson["username"];
	config.password = (const char*)configJson["password"];
	config.topic = (const char*)configJson["topic"];
	config.otaSource = (const char*)configJson["ota_src"];
	return true;
}

void loadDefaultConfig() {
	_LOGLN("[CONFIG] Loading default config");
	config.server = "iot.eclipse.org";
	config.port = 1883;
	config.clientId = "id_example";
	config.username = "exampleuser";
	config.password = "examplepassword";
	config.topic = "/test";
	config.otaSource = "";
	saveConfig();
}

void mqttParseMessage(String &msg) {
	StaticJsonBuffer<CONFIG_JSON_SIZE> jsonBuffer;
	JsonObject &msgObject = jsonBuffer.parseObject(msg);
	if (!msgObject.success())
		return;
	String cmd = msgObject["cmd"];
	String hash = msgObject["hash"];
	if (hash.length() == 0)
		return;
	if (cmd == "on") {
		_LOGLN("== POWER ON ==");
		controlValve(true);
		mqttResponse(1, hash);
	}
	if (cmd == "off") {
		_LOGLN("== POWER OFF ==");
		controlValve(false);
		mqttResponse(1, hash);
	}
	if (cmd == "report") {
		_LOGLN("== Reporting ==");
		mqttResponse(2, hash);
	}
}

void mqttResponse(int msg, String hash) {
	String payload;
	switch (msg) {
	case 1:
		payload = String("{\"response\":\"ok\",") + "\"hash\":" + hash + "}";
		break;
	case 2: {
		const char *template_ = "{\"response\":\"ok\","
				"\"hash\":\"%s\""
				"\"entity\":{"
				"\"reset_reason\":\"%s\","
				"\"heap\":%d,"
				"\"power\":\"%s\""
				"}}";
		char buffer[120];
		snprintf(buffer, sizeof(buffer), template_,
				hash.c_str(),
				state.lastResetReason.c_str(),
				state.freeHeap,
				state.valvePower ? "true" : "false");
		payload = buffer;
		break;
	}
	default:
		payload = String("{\"response\":\"undefined command\",") + "\"hash\":" + hash + "}";
		break;
	}
	mqttClient.publish(config.topic.c_str(), 1, true,
			payload.c_str(), payload.length());
}

void mqttUploadIntDataPoint(String k, int v) {
	String dataPoint = "{\"" + k + "\":" + v + "}";
	char lenHigh = (dataPoint.length() & 0xff00) >> 8;
	char lenLow = dataPoint.length() & 0xff;
	String payload = "\x03";
	payload += lenHigh;
	payload += lenLow;
	payload += dataPoint;
	mqttClient.publish("$dp", 1, false, payload.c_str(), payload.length());
}

void updateFirmware() {
	_LOG("[OTA] Getting OTA updates from URL: ");
	_LOGLN(config.otaSource);
	ESPhttpUpdate.rebootOnUpdate(false);
	switch (ESPhttpUpdate.update(config.otaSource)) {
	case HTTP_UPDATE_FAILED:
		_LOGLN("[OTA] Failed");
		requestFirmwareUpdate = false;
		break;
	case HTTP_UPDATE_NO_UPDATES:
		_LOGLN("[OTA] No updates");
		requestFirmwareUpdate = false;
		break;
	case HTTP_UPDATE_OK:
		_LOGLN("[OTA] OK");
		requestReboot = true;
	}
}

void updateState() {
	state.freeHeap = ESP.getFreeHeap();
	state.lastResetReason = ESP.getResetReason();

	if (VALVE_TRIGGER_LEVEL == LOW)
		digitalWrite(PIN_VALVE_EN, state.valvePower ? LOW : HIGH);
	else if (VALVE_TRIGGER_LEVEL == HIGH)
		digitalWrite(PIN_VALVE_EN, state.valvePower ? HIGH : LOW);
}

void controlValve(bool power) {
	state.valvePower = power;
	mqttUploadIntDataPoint("power", power ? 1 : 0);
}

void handleEmergency() {
	// close the valve
	controlValve(false);
	updateState();
}

void syncClock() {
	_LOGLN("[Clock] Synchronizing");
	const char *HEADERS[] = {"Date"};
	const unsigned int SIZE = 1;
	HTTPClient httpClient;
	httpClient.setTimeout(2000);
	httpClient.begin("http://baidu.com");
	httpClient.collectHeaders(HEADERS, SIZE);
	int code = httpClient.sendRequest("HEAD");
	if (code == 200) {
		if (httpClient.hasHeader("Date")) {
			String date = httpClient.header("Date");
			_LOG("[Clock] Got date from http: ");
			_LOGLN(date);
		} else {
			_LOGLN("[Clock] No Header named Date");
		}
	} else {
		_LOG("[Clock] Failed to sync clock, code: ");
		_LOGLN(code);
	}
	httpClient.end();
}
