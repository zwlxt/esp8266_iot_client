#include <ESP8266WiFi.h>
#include <ESP8266httpUpdate.h>
#include <AsyncMqttClient.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <FS.h>
#include "webpage.h"

const uint8_t PIN_BLINK = LED_BUILTIN;
const uint8_t PIN_VALVE_EN = 12;

const int CONFIG_JSON_SIZE = JSON_OBJECT_SIZE(6) + 440;
const char *CONFIG_FILE_PATH = "/config.json";

AsyncMqttClient mqttClient;
WiFiEventHandler gotIPHandler;
WiFiEventHandler stationModeDisconnectedHandler;
AsyncWebServer webServer(80);
int scanResultCount;
int mqttReconnectionAttempts = 0;
unsigned long mqttLastReconnection;
unsigned long wifiLastConnection;
String lastResetReason;
bool requestFirmwareUpdate;
bool requestReboot;
String firmwareURL;

struct Config {
	char server[64];
	uint16_t port;
	char clientId[23];
	char username[64];
	char password[64];
	char topic[64];
} config;

struct State {
	bool power;
	String lastResetReason;
	uint32_t freeHeap;

} state;

void setup() {
	Serial.begin(115200);
	Serial.println();
	Serial.print("[SYS] Reset reason: ");
	state.lastResetReason = ESP.getResetReason();
	Serial.println(lastResetReason);
	if (state.lastResetReason == "Exception")
		ESP.restart();

	pinMode(PIN_BLINK, OUTPUT);
	pinMode(PIN_VALVE_EN, OUTPUT);

	updateState();

	Serial.println("[SYS] Mounting FS...");
	SPIFFS.begin();

	if (!loadConfig()) {
		loadDefaultConfig();
	} else {
		Serial.println("[CONFIG] Found config file");
	}
	Serial.print("[CONFIG] Server: ");
	Serial.println(config.server);
	Serial.print("[CONFIG] port: ");
	Serial.println(config.port);

	Serial.println("[SYS] Starting web server");
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
		Serial.println("[WiFi] No credential");
		setupNetwork();
	} else {
		Serial.print("[WIFI] Connecting to ");
		Serial.println(WiFi.SSID());
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

	if (requestReboot) {
		handleEmergency();
		ESP.restart();
	}

	if (WiFi.status() != WL_CONNECTED) {
		handleEmergency();
	}

	// This will restart the chip when WiFi reconnecting doesn't work
	if (WiFi.SSID().length() != 0) { // WiFi credential exists
		if (WiFi.status() == WL_CONNECTED) {
			wifiLastConnection = millis();
		} else {
			if (millis() - wifiLastConnection > 300000) { // Disconnected for 10min
				ESP.restart();
			}
		}
	}

	// OTA
	if (requestFirmwareUpdate) {
		handleEmergency();
		updateFirmware();
	}
}

void setupNetwork() {
	Serial.println("[WIFI] Starting network configuration");
	WiFi.mode(WIFI_AP_STA);
	char ssid[16];
	snprintf(ssid, sizeof(ssid), "DEVICE_%d", ESP.getChipId());
	WiFi.softAP(ssid);
	Serial.println("[WIFI] Starting smart config");
	WiFi.beginSmartConfig();

}

void setupMQTTClient() {
	Serial.println("[SYS] Setting up MQTT client");
	mqttClient.setServer(config.server, config.port);
	mqttClient.setClientId(config.clientId);
	mqttClient.setCredentials(config.username, config.password);
	mqttClient.onConnect(onMqttConnected);
	mqttClient.onMessage(onMqttMessage);
	mqttClient.onDisconnect(onMqttDisconnected);
}

void onGotIP(const WiFiEventStationModeGotIP event) {
	Serial.print("[WIFI] Assigned IP: ");
	Serial.println(event.ip);
	WiFi.stopSmartConfig();
	WiFi.softAPdisconnect(false);
	WiFi.mode(WIFI_STA);
	Serial.print("[MQTT] Connecting to ");
	Serial.print(config.server);
	Serial.print(":");
	Serial.println(config.port);
	mqttClient.connect();
}

void onWiFiDisconnected(const WiFiEventStationModeDisconnected event) {
	Serial.print("[WIFI] Disconnected, reason: ");
	Serial.println(event.reason);

	if (WiFi.getMode() != WIFI_AP_STA && WiFi.status() != WL_CONNECTED) {
		setupNetwork();
	}
	Serial.println("[WIFI] Reconnecting...");
}

void onMqttConnected(bool sessionPresent) {
	Serial.println("[MQTT] Connected");
	mqttClient.subscribe(config.topic, 1);
	mqttReconnectionAttempts = 0;
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
	Serial.print("[MQTT] Receiving: ");
	String plStr(payload);
	plStr = plStr.substring(0, len);
	Serial.println(plStr);
	mqttParseMessage(plStr);
}

void onMqttDisconnected(AsyncMqttClientDisconnectReason reason) {
	Serial.print("[MQTT] Disconnected, reason: ");
	Serial.println((int8_t) reason);
	if (WiFi.status() == WL_CONNECTED) {
		mqttReconnectionAttempts++;
		if (mqttReconnectionAttempts > 10) { // After 10 times of reconnection
			if (millis() - mqttLastReconnection > 10000) { // Must wait 10s before next reconnectio
				mqttClient.connect();
				mqttLastReconnection = millis();
			}
		} else {
			Serial.println("[MQTT] Reconnecting...");
			mqttClient.connect();
			mqttLastReconnection = millis();
		}
	}
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
		return lastResetReason;
	if (var == "ssid")
		return WiFi.SSID();
	if (var == "rssi") {
		uint32_t rssi = WiFi.RSSI();
		char result[6];
		ltoa((long) rssi, result, 10);
		return result;
	}
	if (var == "mqtt_status")
		return mqttClient.connected() ? "Connected" : "Disconnected";
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
		if (strlen(config.password) == 0)
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
			strlcpy(config.server, v.c_str(), sizeof(config.server));
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
			strlcpy(config.clientId, v.c_str(), sizeof(config.clientId));
		} else {
			actionFailedHandler(request);
			return;
		}
	}
	if (request->hasParam("username", true)) {
		param = request->getParam("username", true);
		v = param->value();
		if (v.length() < 64) { // Username can be empty. If so, no credential is used
			strlcpy(config.username, v.c_str(), sizeof(config.username));
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
			memset(config.password, '\0', sizeof(config.password));
		}
	}
	if (!nopass) { // nopass is unchecked
		if (request->hasParam("password", true)) {
			param = request->getParam("password", true);
			v = param->value();
			if (v.length() < 64) {
				strlcpy(config.password, v.c_str(), sizeof(config.password));
			} else {
				actionFailedHandler(request);
				return;
			}
		}
	}
	if (request->hasParam("topic", true)) {
		param = request->getParam("topic", true);
		v = param->value();
		if (v.length() > 0 && v.length() < 64) {
			strlcpy(config.topic, v.c_str(), sizeof(config.topic));
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
	request->send_P(200, "text/html", UPDATE_PAGE);
}

void updateActionHandler(AsyncWebServerRequest *request) {
	if (request->hasParam("fw_src", true)) {
		AsyncWebParameter *param = request->getParam("fw_src", true);
		firmwareURL = param->value();
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
	Serial.println("[CONFIG] Saving config");
	File file = SPIFFS.open(CONFIG_FILE_PATH, "w+");
	if (!file) {
		Serial.println("[CONFIG] Unable to open config file");
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
	configJson.printTo(file);
	file.close();
	return true;
}

bool loadConfig() {
	Serial.println("[CONFIG] Loading config");
	if (!SPIFFS.exists(CONFIG_FILE_PATH)) {
		Serial.println("[CONFIG] Config file does not exist");
		return false;
	}
	File file = SPIFFS.open(CONFIG_FILE_PATH, "r");
	if (!file) {
		Serial.println("[CONFIG] Cannot read config file");
		return false;
	}

	StaticJsonBuffer<CONFIG_JSON_SIZE> jsonBuffer;
	JsonObject &configJson = jsonBuffer.parseObject(file.readString());
	file.close();
	if (!configJson.success()) {
		Serial.println("[CONFIG] Cannot decode config file");
		return false;
	}
	strlcpy(config.server, configJson["server"], sizeof(config.server));
	config.port = configJson["port"];
	strlcpy(config.clientId, configJson["clid"], sizeof(config.clientId));
	strlcpy(config.username, configJson["username"], sizeof(config.username));
	strlcpy(config.password, configJson["password"], sizeof(config.password));
	strlcpy(config.topic, configJson["topic"], sizeof(config.topic));
	return true;
}

void loadDefaultConfig() {
	Serial.println("[CONFIG] Loading default config");
	strlcpy(config.server, "iot.eclipse.org", sizeof(config.server));
	config.port = 1883;
	strlcpy(config.clientId, "id_example", sizeof(config.clientId));
	strlcpy(config.username, "exampleuser", sizeof(config.username));
	strlcpy(config.password, "examplepassword", sizeof(config.password));
	strlcpy(config.topic, "/test", sizeof(config.topic));
	saveConfig();
}

void mqttParseMessage(String &msg) {
	StaticJsonBuffer<CONFIG_JSON_SIZE> jsonBuffer;
	JsonObject &msgObject = jsonBuffer.parseObject(msg);
	if (!msgObject.success())
		return;
	String cmd = msgObject["cmd"];
	if (cmd == "on") {
		Serial.println("== POWER ON ==");
		state.power = true;
		mqttResponse(1);
	}
	if (cmd == "off") {
		Serial.println("== POWER OFF ==");
		state.power = false;
		mqttResponse(1);
	}
	if (cmd == "report") {
		Serial.println("== Reporting ==");
		mqttResponse(2);
	}
}

void mqttResponse(int msg) {
	String payload;
	switch (msg) {
	case 1:
		payload = "{\"response\":\"ok\"}";
		break;
	case 2:
		const char *template_ = "{\"response\":\"ok\",\"entity\":{"
				"\"reset_reason\":\"%s\","
				"\"heap\":%d,"
				"\"power\":\"%s\""
				"}}";
		char buffer[120];
		snprintf(buffer, sizeof(buffer), template_,
				state.lastResetReason.c_str(),
				state.freeHeap,
				state.power ? "true" : "false");
		payload = buffer;
		break;
	}
	mqttClient.publish(config.topic, 1, true, payload.c_str(), payload.length());
}

void updateFirmware() {
	Serial.println("[OTA] Begin OTA");
	ESPhttpUpdate.rebootOnUpdate(false);
	switch (ESPhttpUpdate.update(firmwareURL)) {
	case HTTP_UPDATE_FAILED:
		Serial.println("[OTA] Failed");
		break;
	case HTTP_UPDATE_NO_UPDATES:
		Serial.println("[OTA] No updates");
		break;
	case HTTP_UPDATE_OK:
		Serial.println("[OTA] OK");
		requestReboot = true;
	}
}

void updateState() {
	state.freeHeap = ESP.getFreeHeap();
	state.lastResetReason = ESP.getResetReason();

	digitalWrite(PIN_VALVE_EN, state.power ? HIGH : LOW);
}

void handleEmergency() {
	// close the valve
	state.power = false;
	updateState();
}
