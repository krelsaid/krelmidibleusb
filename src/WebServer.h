#ifndef WEBSERVER_H
#define WEBSERVER_H

#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include "LittleFS.h"

extern WebServer server;

void setupWebServer();
void handleRoot();
void handleNotFound();
void handleSettings();
void handleUpdateSettings();
void handleScanWifi();
void handleConnectWifi();
void handleDisconnectWifi();

#endif