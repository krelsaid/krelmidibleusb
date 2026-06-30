#ifndef WEBSERVER_HANDLER_H
#define WEBSERVER_HANDLER_H

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
void handleSystemInfo();
void handleListFs();
void handleDeleteFile();
void handleUpload();
void handleDownload();
void handleUpdateSwitch();
void handleUpdateEncoder();

#endif
