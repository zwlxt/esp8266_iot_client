#ifdef __IN_ECLIPSE__
//This is a automatic generated file
//Please do not modify this file
//If you touch this file your change will be overwritten during the next build
//This file has been generated on 2018-03-02 03:40:58

#include "Arduino.h"
#include <ESP8266WiFi.h>
#include <ESP8266httpUpdate.h>
#include <AsyncMqttClient.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <FS.h>
#include "webpage.h"

void setup() ;
void loop() ;
void setupNetwork() ;
void setupMQTTClient() ;
void onGotIP(const WiFiEventStationModeGotIP event) ;
void onWiFiDisconnected(const WiFiEventStationModeDisconnected event) ;
void onMqttConnected(bool sessionPresent) ;
void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) ;
void onMqttDisconnected(AsyncMqttClientDisconnectReason reason) ;
void indexPageHandler(AsyncWebServerRequest *request) ;
String indexTemplateProcessor(const String &var) ;
void configFileHandler(AsyncWebServerRequest *request) ;
void serverConfigPageHandler(AsyncWebServerRequest *request) ;
String serverConfigTemplateProcessor(const String &var) ;
void serverConfigActionHandler(AsyncWebServerRequest *request) ;
void wifiConfigPageHandler(AsyncWebServerRequest *request) ;
String wifiConfigTemplateProcessor(const String &var) ;
void wifiConfigActionHandler(AsyncWebServerRequest *request) ;
void scanWiFiPageHandler(AsyncWebServerRequest *request) ;
String scanWiFiTemplateProcessor(const String var) ;
void updatePageHandler(AsyncWebServerRequest *request) ;
void updateActionHandler(AsyncWebServerRequest *request) ;
void actionSuccessHandler(AsyncWebServerRequest *request) ;
void actionFailedHandler(AsyncWebServerRequest *request) ;
bool saveConfig() ;
bool loadConfig() ;
void loadDefaultConfig() ;
void mqttParseMessage(String &msg) ;
void mqttResponse(int msg) ;

#include "esp8266_iot_client.ino"


#endif
