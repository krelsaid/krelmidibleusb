#include "WebServer.h"
#include "main.h" // For access to switchConfigs, encoderSettings, wifiSsid, wifiPass, wifiEnabled, saveSettings

WebServer server(80);

extern Screen screen;

void setupWebServer() {
    server.on("/", HTTP_GET, handleRoot);
    server.on("/settings", HTTP_GET, handleSettings);
    server.on("/updateSettings", HTTP_POST, handleUpdateSettings);
    server.on("/scanWifi", HTTP_GET, handleScanWifi);
    server.on("/connectWifi", HTTP_POST, handleConnectWifi);
    server.on("/disconnectWifi", HTTP_POST, handleDisconnectWifi);
    server.onNotFound(handleNotFound);
    server.begin();
    Serial.println("HTTP server started");
}

String getContentType(String filename) {
    if (server.hasArg("download")) return "application/octet-stream";
    else if (filename.endsWith(".htm")) return "text/html";
    else if (filename.endsWith(".html")) return "text/html";
    else if (filename.endsWith(".css")) return "text/css";
    else if (filename.endsWith(".js")) return "application/javascript";
    else if (filename.endsWith(".png")) return "image/png";
    else if (filename.endsWith(".gif")) return "image/gif";
    else if (filename.endsWith(".jpg")) return "image/jpeg";
    else if (filename.endsWith(".ico")) return "image/x-icon";
    else if (filename.endsWith(".xml")) return "text/xml";
    else if (filename.endsWith(".pdf")) return "application/x-pdf";
    else if (filename.endsWith(".zip")) return "application/x-zip";
    else if (filename.endsWith(".gz")) return "application/x-gzip";
    return "text/plain";
}

bool handleFileRead(String path) {
    Serial.println("handleFileRead: " + path);
    if (path.endsWith("/")) path += "index.html";
    String contentType = getContentType(path);
    if (LittleFS.exists(path)) {
        File file = LittleFS.open(path, "r");
        server.streamFile(file, contentType);
        file.close();
        return true;
    }
    return false;
}

void handleRoot() {
    if (!handleFileRead("/index.html")) {
        server.send(404, "text/plain", "FileNotFound");
    }
}

void handleNotFound() {
    if (!handleFileRead(server.uri())) {
        server.send(404, "text/plain", "FileNotFound");
    }
}

void handleSettings() {
    JsonDocument doc;
    JsonArray switchMaps = doc["switchConfigs"].to<JsonArray>();
    for (int i = 0; i < 5; ++i) {
        JsonObject obj = switchMaps.add<JsonObject>();
        obj["mode"] = switchConfigs[i].mode == SWITCH_MODE_TOGGLE ? "toggle" : "momentary";
        obj["cc"] = switchConfigs[i].cc;
        obj["val"] = switchConfigs[i].val;
        obj["ch"] = switchConfigs[i].ch;
        obj["altVal"] = switchConfigs[i].altVal;
    }

    JsonArray encSettings = doc["encoderSettings"].to<JsonArray>();
    for (int i = 0; i < 5; ++i) {
        JsonObject obj = encSettings.add<JsonObject>();
        obj["mode"] = encoderSettings[i].mode == ENCODER_MODE_SINGLE ? "single" : "range";
        obj["left_cc"] = encoderSettings[i].left.cc;
        obj["left_val"] = encoderSettings[i].left.val;
        obj["left_ch"] = encoderSettings[i].left.ch;
        obj["right_cc"] = encoderSettings[i].right.cc;
        obj["right_val"] = encoderSettings[i].right.val;
        obj["right_ch"] = encoderSettings[i].right.ch;
        obj["rangeMin"] = encoderSettings[i].rangeMin;
        obj["rangeMax"] = encoderSettings[i].rangeMax;
        obj["currentValue"] = encoderSettings[i].currentValue;
        obj["singleModeSteps"] = encoderSettings[i].singleModeSteps;
        obj["acceleration"] = encoderSettings[i].acceleration;
    }

    JsonObject wifi = doc["wifi"].to<JsonObject>();
    wifi["ssid"] = wifiSsid;
    wifi["enabled"] = wifiEnabled;
    wifi["status"] = WiFi.status();
    if (WiFi.status() == WL_CONNECTED) {
        wifi["ip"] = WiFi.localIP().toString();
    }

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    server.send(200, "application/json", jsonResponse);
}

void handleUpdateSettings() {
    if (server.hasArg("plain")) {
        String body = server.arg("plain");
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, body);

        if (error) {
            server.send(400, "text/plain", "Invalid JSON");
            return;
        }

        JsonArray switchMaps = doc["switchConfigs"];
        if (!switchMaps.isNull()) {
            for (int i = 0; i < switchMaps.size() && i < 5; ++i) {
                JsonObject obj = switchMaps[i];
                switchConfigs[i].mode = strcmp(obj["mode"] | "momentary", "toggle") == 0 ? SWITCH_MODE_TOGGLE : SWITCH_MODE_MOMENTARY;
                switchConfigs[i].cc = obj["cc"] | switchConfigs[i].cc;
                switchConfigs[i].val = obj["val"] | switchConfigs[i].val;
                switchConfigs[i].ch = obj["ch"] | switchConfigs[i].ch;
                switchConfigs[i].altVal = obj["altVal"] | switchConfigs[i].altVal;
            }
        }

        JsonArray encSettings = doc["encoderSettings"];
        if (!encSettings.isNull()) {
            for (int i = 0; i < encSettings.size() && i < 5; ++i) {
                JsonObject obj = encSettings[i];
                encoderSettings[i].mode = strcmp(obj["mode"] | "single", "range") == 0 ? ENCODER_MODE_RANGE : ENCODER_MODE_SINGLE;
                encoderSettings[i].left.cc = obj["left_cc"] | encoderSettings[i].left.cc;
                encoderSettings[i].left.val = obj["left_val"] | encoderSettings[i].left.val;
                encoderSettings[i].left.ch = obj["left_ch"] | encoderSettings[i].left.ch;
                encoderSettings[i].right.cc = obj["right_cc"] | encoderSettings[i].right.cc;
                encoderSettings[i].right.val = obj["right_val"] | encoderSettings[i].right.val;
                encoderSettings[i].right.ch = obj["right_ch"] | encoderSettings[i].right.ch;
                encoderSettings[i].rangeMin = obj["rangeMin"] | encoderSettings[i].rangeMin;
                encoderSettings[i].rangeMax = obj["rangeMax"] | encoderSettings[i].rangeMax;
                encoderSettings[i].currentValue = obj["currentValue"] | encoderSettings[i].currentValue;
                encoderSettings[i].singleModeSteps = obj["singleModeSteps"] | encoderSettings[i].singleModeSteps;
                encoderSettings[i].acceleration = obj["acceleration"] | encoderSettings[i].acceleration;
            }
        }

        JsonObject wifiObj = doc["wifi"];
        if (!wifiObj.isNull()) {
            wifiEnabled = wifiObj["enabled"] | wifiEnabled;
            strlcpy(wifiSsid, wifiObj["ssid"] | "", sizeof(wifiSsid));
            strlcpy(wifiPass, wifiObj["pass"] | "", sizeof(wifiPass));
        }

        saveSettings();
        server.send(200, "text/plain", "Settings updated");
    } else {
        server.send(400, "text/plain", "No data received");
    }
}

void handleScanWifi() {
    JsonDocument doc;
    JsonArray networks = doc.to<JsonArray>();

    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; ++i) {
        JsonObject obj = networks.add<JsonObject>();
        obj["ssid"] = WiFi.SSID(i);
        obj["rssi"] = WiFi.RSSI(i);
        obj["encryption"] = WiFi.encryptionType(i);
    }
    WiFi.scanDelete();

    String jsonResponse;
    serializeJson(doc, jsonResponse);
    server.send(200, "application/json", jsonResponse);
}

void handleConnectWifi() {
    if (server.hasArg("ssid") && server.hasArg("password")) {
        String ssid = server.arg("ssid");
        String password = server.arg("password");

        strlcpy(wifiSsid, ssid.c_str(), sizeof(wifiSsid));
        strlcpy(wifiPass, password.c_str(), sizeof(wifiPass));
        wifiEnabled = true;
        saveSettings();

        WiFi.disconnect(true);
        delay(100);
        WiFi.begin(wifiSsid, wifiPass);

        server.send(200, "text/plain", "Connecting to WiFi...");
    } else {
        server.send(400, "text/plain", "Missing SSID or password");
    }
}

void handleDisconnectWifi() {
    wifiEnabled = false;
    saveSettings();
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    server.send(200, "text/plain", "Disconnected from WiFi");
}
