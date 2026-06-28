/*
  ESP32 SH1106 BLE-MIDI Controller (Polished UI, no save icon)
  13.8.2025
  Khairil Said
  Using Ai Code Assist
  - WiFi functionality added
  - OLED SH1106 (I2C @ 0x3C): SDA=21, SCL=22
  - Rotary Encoder: DT=32, CLK=33, SW=25 (to GND, INPUT_PULLUP)
  - Switches: SW1=13, SW2=14, SW3=26, SW4=27 (to GND, INPUT_PULLUP)
  - Battery ADC: GPIO 35 via divider (R1=100k to batt+, R2=100k to GND) for 1S Li-ion
  - BLE-MIDI (NimBLE): sends CC on press only
    CC = Data1 (1..50), Value = Data2 (1..100), Channel = Data3 (1..10)
 - Update to version 1.1 @ 14/6/2026
  - implement Usb Host Midi
  - change to esp32s3
  - Update to version 1.2 @ 20/6/2026
  - Refactor to use new cs::BluetoothMIDI_Interface wrapper for better BLE-MIDI handling and status tracking.
*/

#include <Arduino.h>
#include <Wire.h>
#include "LittleFS.h" // Use #include "SPIFFS.h" if using legacy SPIFFS
// Fast Conversion Aliases
#define SPIFFS LittleFS
#include <WiFi.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include "Esp32UsbHost.h"

#include <Adafruit_GFX.h>
//#include <Adafruit_SH110X.h>
#include <Adafruit_SSD1306.h>

#include <AiEsp32RotaryEncoder.h>

//#include <MIDI.h>
//#include <BLEMIDI_Transport.h>
//#include <hardware/BLEMIDI_ESP32_NimBLE.h>
//#include <Control_Surface.h>
#include <MIDI_Interfaces/BluetoothMIDI_Interface.hpp>

/* ------------- MIDI Value Ranges ------------- */
#define SWITCH_CC_MIN 0
#define SWITCH_CC_MAX 127
#define SWITCH_VAL_MIN 0
#define SWITCH_VAL_MAX 127
#define SWITCH_CH_MIN 1
#define SWITCH_CH_MAX 16

#define ENCODER_CC_MIN 0
#define ENCODER_CC_MAX 127
#define ENCODER_VAL_MIN 0
#define ENCODER_VAL_MAX 127
#define ENCODER_CH_MIN 1
#define ENCODER_CH_MAX 16
#define ENCODER_STEPS_MIN 1
#define ENCODER_STEPS_MAX 10
#define ENCODER_RANGE_MIN 0
#define ENCODER_RANGE_MAX 127

/* ---------------- Esp32s3 Pins ---------------- */
#define OLED_SDA 8//21
#define OLED_SCL 9//22

#define ROT_DT   41//33
#define ROT_CLK  40//25
#define ROT_SW   39//26

#define SW1 14//13
#define SW2 13//12
#define SW3 12//14
#define SW4 11//27
const int SW_PINS[4] = {SW1, SW2, SW3, SW4};

// Placeholder pins for 4 new encoders (these are examples, adjust to your wiring)
#define ENC2_DT 4  // Moved from 15 (ADC2 pin, conflicts with WiFi)
#define ENC2_CLK 5//16 // Moved from 2 (ADC2 pin, conflicts with WiFi)
#define ENC3_DT 6//17
#define ENC3_CLK 7//5
#define ENC4_DT 15//18
// GPIO 19 is native USB D- on ESP32-S3. Moving ENC4_CLK to Pin 2.
#define ENC4_CLK 16//2
#define ENC5_DT 17//32
#define ENC5_CLK 18//23 // Moved from 35 (input-only pin, no internal pull-up)

#define BATTERY_PIN 2//34   // ADC (GPIO34)

/* --------------- OLED ----------------- */
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
//#define i2c_Address 0x3c
//#define WHITE SH110X_WHITE
//#define BLACK SH110X_BLACK
//Adafruit_SH1106 display(OLED_RESET);   // display.begin(SH1106_SWITCHCAPVCC, 0x3C);
//Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
/* ------------- Rotary ----------------- */
AiEsp32RotaryEncoder rotary(ROT_DT, ROT_CLK, ROT_SW, -1, 4);
AiEsp32RotaryEncoder encoder2(ENC2_DT, ENC2_CLK, -1, -1, 4);
AiEsp32RotaryEncoder encoder3(ENC3_DT, ENC3_CLK, -1, -1, 4);
AiEsp32RotaryEncoder encoder4(ENC4_DT, ENC4_CLK, -1, -1, 4);
AiEsp32RotaryEncoder encoder5(ENC5_DT, ENC5_CLK, -1, -1, 4);
AiEsp32RotaryEncoder* midiEncoders[5] = {&rotary, &encoder2, &encoder3, &encoder4, &encoder5};

void IRAM_ATTR rotaryISR(){ 
  for (int i=0; i<5; i++) {
    midiEncoders[i]->readEncoder_ISR();
  }
}

/* ------------- Battery ---------------- */
#define R1 100000.0f
#define R2 100000.0f
float batteryVoltage = 0.f;
int   batteryPercent = 0;
uint32_t lastBatteryRead = 0;
const int BATTERY_SMOOTHING_SAMPLES = 20; // Number of samples to average for a stable reading
float batteryReadings[BATTERY_SMOOTHING_SAMPLES];
int batteryReadingIndex = 0;
bool batteryBufferFilled = false;

bool usbMidiConnected = false;
Esp32UsbHost usbHost;

// Your custom connection callback function
void onUsbMidiConnected() {
  Serial.println("\n[USB HOST] >>> PocketMaster connected successfully! <<< \n");
  usbMidiConnected = true;
}

// Your custom disconnection callback function
void onUsbMidiDisconnected() {
  Serial.println("\n[USB HOST] !!! PocketMaster disconnected !!! \n");
  usbMidiConnected = false;
}

// Dedicated FreeRTOS thread worker function for USB Host processing
void usbHostCoreTask(void *pvParameters) {
  Serial.printf("[CORE] USB Daemon Thread successfully booted on Core: %d\n", xPortGetCoreID());

  while (1) {
    // Frequently pump the internal USB system event loop handler on Core 1
    usbHost.task();
    
    // Yield execution to prevent FreeRTOS Watchdog Timer (WDT) starvation
    vTaskDelay(pdMS_TO_TICKS(1)); 
  }
}

/* ------------- BLE-MIDI -------------- */
//BLEMIDI_CREATE_DEFAULT_INSTANCE();
//MIDI_CREATE_INSTANCE(BLEMIDI, BLEMIDI, MIDI);
cs::BluetoothMIDI_Interface midi_ble;

#define MIDI_DEVICE_NAME "KRELFSW"  //name of BT Device
const char* BLE_NAME = MIDI_DEVICE_NAME;

//BLEMIDI_CREATE_INSTANCE(MIDI_DEVICE_NAME, MIDI);
bool btConnected = false;
bool lastBtConnected = false;
char connectedDeviceAddress[18] = "Not Connected";
bool isCharging = false;
bool lastIsCharging = false;

/* ------------- Encoder MIDI Config ------------- */
enum EncoderMode { ENCODER_MODE_SINGLE, ENCODER_MODE_RANGE };

struct EncoderMidiAction {
    int cc = 20;
    int val = 127;
    int ch = 1;
};

struct EncoderSettings {
    EncoderMode mode = ENCODER_MODE_SINGLE;
    EncoderMidiAction left;
    EncoderMidiAction right; // In Range mode, only left's cc/ch are used
    int rangeMin = 0;
    int rangeMax = 127;
    int currentValue = 64;
    int singleModeSteps = 1; // Steps needed for single CC send
    int acceleration = 250; // Time in ms for acceleration (lower is faster), 0 is off.
};

EncoderSettings encoderSettings[5];
int encoderAccumulatedSteps[5] = {0, 0, 0, 0, 0}; // For single mode step counting

/* ------------- Switch MIDI Config ------------- */
enum SwitchMode { SWITCH_MODE_MOMENTARY, SWITCH_MODE_TOGGLE };

struct SwitchConfig {
    SwitchMode mode = SWITCH_MODE_MOMENTARY;
    int cc = 10;
    int val = 127;
    int ch = 1;
    int altVal = 0;
    bool state = false; // for toggle mode
};
SwitchConfig switchConfigs[5]; // 0..3 = Switch1..4, 4 = Encoder Button

enum Screen { HOME, MENU_MAIN, MENU_SWITCH_SELECT, MENU_SWITCH, MENU_ENCODER_SELECT, MENU_ENCODER_EDIT, MENU_WIFI, MENU_WIFI_SCAN, MENU_WIFI_INFO, KEYBOARD, ABOUT, MENU_CONFIRM_RESET };
Screen screen = HOME;

/* ------------- WiFi ----------------- */ 
bool wifiEnabled = false;
char wifiSsid[33] = "";
char wifiPass[65] = "";
uint8_t wifiStatus = WL_IDLE_STATUS;
int scanResultCount = 0;
int wifiScanIndex = 0;
int wifiMenuIndex = 0;
int wifiMenuTop = 0;
bool otaRunning = false;

/* ------------- Keyboard -------------- */
enum KeyboardMode { K_LOWER, K_UPPER, K_NUM, K_SYM };
KeyboardMode kbdMode = K_LOWER;
Screen screenBeforeKeyboard = HOME;
char kbdOriginalBuffer[65]; // to store text before editing
char* kbdInputBuffer = nullptr;
int kbdInputMaxLength = 0;
int kbdCursorX = 0, kbdCursorY = 0;
const char kbd_lower[] = "abcdefghijklmnopqrstuvwxyz"; 
const char kbd_upper[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
const char kbd_num[]   = "1234567890-=";
const char kbd_sym[]   = "!@#$%^&*()_+[]{}|;:'\"<>,.?/";

int mainMenuIndex = 0;      // 0..7 -> Sw1..Sw4, Encoder Btn, WiFi, About, Exit
int switchSelectMenuIndex = 0;
int switchSelectMenuTop = 0;
int switchEditIndex = 0;    // which item we’re editing (0..4)
int switchMenuIndex = 0;    // 0..5 -> CC, VAL, CH, BACK, SAVE, EXIT
int encoderSelectMenuIndex = 0;
int encoderSelectMenuTop = 0;
int encoderEditIndex = 0; // which encoder we're editing (0-4)
int encoderMenuItemIndex = 0; // which parameter in the encoder menu
int mainMenuTop = 0;        // for scrolling in main menu
int switchMenuTop = 0;      // for scrolling in switch menu
bool editingValue = false;

int confirmResetSelection = 0; // 0=NO, 1=YES

int lastPressedIndex = -1;  // -1 none → show "00"

/* ------------- Menu Timeout ------------- */
const uint32_t MENU_TIMEOUT_MS = 15000; // 15 seconds
uint32_t lastActivityTime = 0;

/* ------------- Long-press ------------- */
const uint32_t LONG_MS = 1000;
bool encBtnWasDown = false;
uint32_t encBtnDownAt = 0;
bool longPressHandled = false;

/* ------------- Debounce ---------------- */
const uint16_t DEBOUNCE_MS = 25;
bool swPrev[4] = {true,true,true,true};
uint32_t swLastChange[4] = {0,0,0,0};

/* ------------- Blinking ---------------- */
uint32_t lastBlinkTime = 0;
bool btIconVisible = true;
const uint32_t BLINK_INTERVAL_MS = 500;
uint32_t lastIndicatorBlinkTime = 0;
bool indicatorVisible = true;
const uint32_t INDICATOR_BLINK_INTERVAL_MS = 750; // Slightly slower than status icon blink

/* ------------- Bitmaps ----------------- */
// 8x8 Bluetooth icons (filled vs outline)
const uint8_t PROGMEM BT_FILLED[8] = {0x8c,0x4e,0x2f,0x1e,0x1e,0x2f,0x4e,0x8c};
const uint8_t PROGMEM BT_OUTLINE[8] = {0x8c,0x4a,0x29,0x1a,0x1a,0x29,0x4a,0x8c};
// 8x8 WiFi icons
const uint8_t PROGMEM WIFI_DOT[8]  = {0x00,0x00,0x81,0x42,0x24,0x18,0x18,0x18};
const uint8_t PROGMEM WIFI_LOW[8]  = {0x00,0x00,0x81,0x5a,0x24,0x18,0x18,0x18};
const uint8_t PROGMEM WIFI_MED[8]  = {0x00,0x3c,0x81,0x5a,0x24,0x18,0x18,0x18};
const uint8_t PROGMEM WIFI_HIGH[8] = {0x18,0x3c,0xdb,0x66,0x3c,0x18,0x18,0x18};
const uint8_t PROGMEM ARROW_TL[8] = { // Top Left: 
  0b11111110,
  0b11111100,
  0b11111000,
  0b11111100,
  0b11111110,
  0b11011100,
  0b10001000,
  0b00000000
};
const uint8_t PROGMEM ARROW_BL[8] = { // Bottom Left:
  0b00000000,
  0b10001000,
  0b11011100,
  0b11111110,
  0b11111100,
  0b11111000,
  0b11111100,
  0b11111110
};
const uint8_t PROGMEM ARROW_TR[8] = { // Top Right: 
  0b01111111,
  0b00111111,
  0b00011111,
  0b00111111,
  0b01111111,
  0b00111011,
  0b00010001,
  0b00000000
};
const uint8_t PROGMEM ARROW_BR[8] = { // Bottom Right: 
  0b00000000,
  0b00010001,
  0b00111011,
  0b01111111,
  0b00111111,
  0b00011111,
  0b00111111,
  0b01111111
};
const uint8_t PROGMEM USB_ICON[8]  = {0x10,0x54,0x38,0x10,0x10,0x10,0x38,0x00};

/* ------------- Spiff ----------------- */
const char* FILE_PATH = "/settings.json";

/* ------------- Decls ------------------- */
void drawStatusBar();
void drawHome();
void drawMainMenu();
void drawSwitchSelectMenu();
void drawEncoderSelectMenu();
void drawEncoderEditMenu();
void drawWifiMenu();
void drawWifiScan();
void drawWifiInfo();
void drawKeyboard();
void drawSwitchMenu();
void drawAbout();
void drawConfirmReset();
void readBattery();
void saveSettings();
void loadSettings();
void startKeyboard(char* buffer, int maxLength);
void sendCC(byte cc, byte val, byte ch); // Forward declaration added
void sendCCForIndex(int idx);
void midiTask(void*);
void startOTA();
void bootAnimation();
int wrapValue(int value, int dir, int minVal, int maxVal);
void drawMenuList(const char* title, const char* items[], int itemCount, int selectedIndex, int topIndex);
void redrawCurrentScreen();

/* ========================== Animation ========================= */
void bootAnimation() {
  const char* line1 = "KHAIRIL";
  const char* line2 = "BLE MIDI SWITCH";

  int16_t x1, y1;
  uint16_t w1, h1, w2, h2;

  // --- 1. Calculate text layout ---
  display.setTextSize(2);
  display.getTextBounds(line1, 0, 0, &x1, &y1, &w1, &h1);
  
  display.setTextSize(1);
  display.getTextBounds(line2, 0, 0, &x1, &y1, &w2, &h2);

  const int targetX1 = (SCREEN_WIDTH - w1) / 2;
  const int targetX2 = (SCREEN_WIDTH - w2) / 2;
  // Center the text block vertically
  const int text_gap = 8; // Pixels between the two lines
  const int total_text_height = h1 + text_gap + h2;
  const int y1_pos = (SCREEN_HEIGHT - total_text_height) / 2;
  const int y2_pos = y1_pos + h1 + text_gap;

  // --- 2. Raster In animation (reveal from center) ---
  for (int i = SCREEN_HEIGHT / 2; i >= 0; i -= 2) {
    display.clearDisplay();
    display.setTextColor(WHITE);

    // Draw the text to the buffer in every frame
    display.setTextSize(2);
    display.setCursor(targetX1, y1_pos);
    display.print(line1);
    display.setTextSize(1);
    display.setCursor(targetX2, y2_pos);
    display.print(line2);

    // Mask top and bottom parts to create the reveal effect
    display.fillRect(0, 0, SCREEN_WIDTH, i, BLACK);
    display.fillRect(0, SCREEN_HEIGHT - i, SCREEN_WIDTH, i, BLACK);
    
    display.display();
    delay(10);
  }

  // --- 3. Hold the logo ---
  delay(1500);

  // --- 4. Raster Out animation (hide from outside-in) ---
  for (int i = 0; i <= SCREEN_HEIGHT / 2; i += 2) {
    display.clearDisplay();
    display.setTextColor(WHITE);
    display.setTextSize(2);
    display.setCursor(targetX1, y1_pos);
    display.print(line1);
    display.setTextSize(1);
    display.setCursor(targetX2, y2_pos);
    display.print(line2);
    display.fillRect(0, 0, SCREEN_WIDTH, i, BLACK);
    display.fillRect(0, SCREEN_HEIGHT - i, SCREEN_WIDTH, i, BLACK);
    display.display();
    delay(10);
  }
}

/* ======================== Value Wrapping ====================== */
int wrapValue(int value, int dir, int minVal, int maxVal) {
  value += dir;
  if (value < minVal) return maxVal;
  if (value > maxVal) return minVal;
  return value;
}

/* ============================ Setup ============================ */
void setup() {
  Serial.begin(115200);
  delay(50);

  WiFi.mode(WIFI_OFF);

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS mount failed");
  }

  //Wire.begin(OLED_SDA, OLED_SCL);
  //display.begin(SH1106_SWITCHCAPVCC, 0x3C);
  delay(250); // wait for the OLED to power up
  //display.begin(i2c_Address, true); // Address 0x3C default
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3D)) { // Address 0x3D for 128x64
    Serial.println(F("SSD1306 allocation failed"));
    for(;;); // Don't proceed, loop forever
  }

  bootAnimation();

  pinMode(ROT_DT, INPUT_PULLUP);
  pinMode(ROT_CLK, INPUT_PULLUP);
  pinMode(ROT_SW, INPUT_PULLUP);
  rotary.begin();
  rotary.setup(rotaryISR);
  
  // Initialize all MIDI encoders
  int enc_pins[] = {ENC2_DT, ENC2_CLK, ENC3_DT, ENC3_CLK, ENC4_DT, ENC4_CLK, ENC5_DT, ENC5_CLK};
  for (int i=0; i<8; i++) {
    pinMode(enc_pins[i], INPUT_PULLUP);
  }
  for (int i=1; i<5; i++) { // Start from 1, 0 is the main rotary
    midiEncoders[i]->begin();
    midiEncoders[i]->setup(rotaryISR);
  }
  for (int i=0;i<4;i++){
    pinMode(SW_PINS[i], INPUT_PULLUP);
    swPrev[i] = true;
    swLastChange[i] = 0;
  }
  // Initialize the battery reading buffer
  for (int i = 0; i < BATTERY_SMOOTHING_SAMPLES; i++) {
    batteryReadings[i] = 0.0f;
  }
  pinMode(BATTERY_PIN, INPUT);

  loadSettings();

  // Initialize USB Host MIDI first (priority)
  usbHost.onDeviceConnected(onUsbMidiConnected);
  usbHost.onDeviceDisconnected(onUsbMidiDisconnected);
  usbHost.begin();
  
    // --- SPAWN BACKGROUND TASK ON CORE 1 ---
  xTaskCreatePinnedToCore(
    usbHostCoreTask,     // Function to run
    "usb_core1_worker",  // Name of task
    4096,                // Stack size allocation
    NULL,                // Parameter input pointer
    3,                   // Priority rank (higher handles interrupts better)
    NULL,                // Task handle
    1                    // Pin explicitly to Core 1
  );


  // Apply loaded/default settings to all encoders
  for (int i=0; i<5; i++) {
    midiEncoders[i]->setAcceleration(encoderSettings[i].acceleration);
    midiEncoders[i]->setBoundaries(-30000, 30000, false); // Use a large range for relative reading
  }
    
  if (wifiEnabled) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(BLE_NAME);
    if (strlen(wifiSsid) > 0) {
      WiFi.begin(wifiSsid, wifiPass);
    }
  }
 /*
  //BLEMIDI.begin(BLE_NAME);
  MIDI.begin(MIDI_CHANNEL_OMNI);
  
  BLEMIDI.setHandleConnected([]() {
    btConnected = true;
    Serial.println("BLE-MIDI connected");
    NimBLEServer* pServer = NimBLEDevice::getServer();
    if (pServer && pServer->getConnectedCount() > 0) {
      // Using the method from your working test sketch.
      // This assumes the newly connected device has connection ID 0.
      NimBLEConnInfo conn_info = pServer->getPeerInfo(0);
      strlcpy(connectedDeviceAddress, conn_info.getAddress().toString().c_str(), sizeof(connectedDeviceAddress));
      Serial.print("Connected to: ");
      Serial.println(connectedDeviceAddress);
    }
  });
  BLEMIDI.setHandleDisconnected([]() {
    btConnected = false;
    Serial.println("BLE-MIDI disconnected");
    strlcpy(connectedDeviceAddress, "Not Connected", sizeof(connectedDeviceAddress));
  });
  */
  enableLoopWDT(); // Registers the main loop to the default watchdog thread
  xTaskCreatePinnedToCore(midiTask, "BLEtask", 4096, NULL, 1, NULL, 1);


//BLE-MIDI setup moved to after USB Host init in case of any conflicts. Also added connection callbacks for status bar updates.
  midi_ble.setName(MIDI_DEVICE_NAME);
  cs::MIDI_Interface::beginAll();
   
  drawHome();
}

/* ============================= Loop =========================== */
void loop() {
 //BLE-MIDI connection status check for status bar updates
  btConnected = midi_ble.isConnected();
  //rotary.loop();
  readBattery();
  
  // OTA Handling
  if (wifiEnabled) {
    if (WiFi.status() == WL_CONNECTED) {
      if (!otaRunning) {
        startOTA();
        otaRunning = true;
      }
      ArduinoOTA.handle();
    } else {
      otaRunning = false; // handles case where connection is lost
    }
  } else {
    otaRunning = false; // handles case where wifi is manually disabled
  }

  // Blinking state for BT icon
  bool needsRedrawForBlink = false;
  if (millis() - lastBlinkTime > BLINK_INTERVAL_MS) { // Always update blink timer
    lastBlinkTime = millis();
    if (!btConnected) btIconVisible = !btIconVisible;
    needsRedrawForBlink = true;
  }

  // Blinking state for Switch Indicators
  if (millis() - lastIndicatorBlinkTime > INDICATOR_BLINK_INTERVAL_MS) {
    lastIndicatorBlinkTime = millis();
    indicatorVisible = !indicatorVisible;
    needsRedrawForBlink = true;
  }

  // Real-time icon updates for status bar
  uint8_t currentWifiStatus = wifiEnabled ? WiFi.status() : 255; // Use 255 for off
  if (btConnected != lastBtConnected || currentWifiStatus != wifiStatus || needsRedrawForBlink || isCharging != lastIsCharging) {
    if (btConnected != lastBtConnected) { // If BT connection state changed
      lastBtConnected = btConnected;
      lastBlinkTime = millis(); // Reset blink timer on any state change
      btIconVisible = true;     // Always show icon immediately after change
    }
    wifiStatus = currentWifiStatus;
    lastIsCharging = isCharging; // Update charging state tracker
    redrawCurrentScreen();
  }

  // Marquee redraw for long SSID in WiFi menu
  static uint32_t lastMarqueeDraw = 0;
  if (screen == MENU_WIFI && millis() - lastMarqueeDraw > 300) {
      lastMarqueeDraw = millis();
      // Only redraw if an SSID is actually long enough to scroll
      int max_len = (SCREEN_WIDTH - 75) / 6;
      if (strlen(wifiSsid) > max_len) {
          drawWifiMenu();
      }
  }

  // Menu timeout check
  if ((screen == MENU_MAIN || screen == MENU_SWITCH || screen == MENU_SWITCH_SELECT || screen == MENU_ENCODER_SELECT || screen == MENU_ENCODER_EDIT) && (millis() - lastActivityTime > MENU_TIMEOUT_MS)) {
    screen = HOME;
    editingValue = false; // Reset editing state
    drawHome();
  }

  // Encoder button long/short press handling
  bool encDown = (digitalRead(ROT_SW) == LOW);

    // Encoder rotation handling
  static long lastEncMain = 0;
  static long lastEncSwitch = 0;

  // 1. Button just pressed down
  if (encDown && !encBtnWasDown) {
    encBtnWasDown = true;
    //rotary.reset();
    encBtnDownAt = millis();
    longPressHandled = false;
  }

  // 2. Button is being held down: check for long press
  if (encDown && encBtnWasDown && !longPressHandled) {
    if (millis() - encBtnDownAt >= LONG_MS) {
      // --- LONG PRESS ACTION ---
      // Enter main menu immediately
      screen = MENU_MAIN;
      editingValue = false;
      mainMenuIndex = 0;
      mainMenuTop = 0;
      rotary.setEncoderValue(0);
      lastEncMain = 0;
      lastActivityTime = millis();
      drawMainMenu();
      longPressHandled = true; // Mark as handled to prevent short press on release
    }
  }

  // 3. Button was just released
  if (!encDown && encBtnWasDown) {
    encBtnWasDown = false;
    if (!longPressHandled) {
      // --- SHORT PRESS ACTION ---
      if (screen != HOME) lastActivityTime = millis();
      if (screen == HOME) {
        sendCCForIndex(4);           // encoder button mapping
        lastPressedIndex = 4;
        for(int i=0; i<5; i++) {
          midiEncoders[i]->setEncoderValue(0);
          encoderAccumulatedSteps[i] = 0; // Also reset my accumulator
        }
        drawHome();
      } else if (screen == MENU_WIFI_INFO) {
        // any short press returns to wifi menu
        screen = MENU_WIFI;
        drawWifiMenu();
      } else if (screen == MENU_SWITCH) {
        SwitchConfig &cfg = switchConfigs[switchEditIndex];
        int numItems = (cfg.mode == SWITCH_MODE_TOGGLE) ? 8 : 7;
        int backIndex = numItems - 3;
        int saveIndex = numItems - 2;
        int exitIndex = numItems - 1;

        if (switchMenuIndex < backIndex) { // Any item before Back/Save/Exit
            editingValue = !editingValue;
            // Special case for mode toggle
            if (switchMenuIndex == 0 && editingValue) {
                cfg.mode = (cfg.mode == SWITCH_MODE_MOMENTARY) ? SWITCH_MODE_TOGGLE : SWITCH_MODE_MOMENTARY;
                // If we switch to momentary, ensure the menu index is not out of bounds
                if (cfg.mode == SWITCH_MODE_MOMENTARY && switchMenuIndex >= numItems) {
                    switchMenuIndex = numItems - 1; // Point to Exit
                }
                editingValue = false; // It's a toggle, not an edit mode
            }
            drawSwitchMenu();
        } else if (switchMenuIndex == backIndex) { // BACK
            screen = MENU_SWITCH_SELECT;
            editingValue = false;
            drawSwitchSelectMenu();
        } else if (switchMenuIndex == saveIndex) { // SAVE
            saveSettings();
            drawSwitchMenu();
        } else if (switchMenuIndex == exitIndex) { // EXIT
            screen = HOME;
            for(int i=0; i<5; i++) { 
                midiEncoders[i]->setEncoderValue(0); 
                encoderAccumulatedSteps[i] = 0;
            }
            editingValue = false;
            drawHome();
        }
      } else if (screen == MENU_SWITCH_SELECT) {
        if (switchSelectMenuIndex <= 4) { // Switch 1-4 or Encoder Btn
          switchEditIndex = switchSelectMenuIndex;
          switchMenuIndex = 0;
          switchMenuTop = 0;
          editingValue = false;
          screen = MENU_SWITCH;
          rotary.setEncoderValue(0);
          lastEncSwitch = 0;
          drawSwitchMenu();
        } else if (switchSelectMenuIndex == 5) { // Back
          screen = MENU_MAIN;
          drawMainMenu();
        }
      } else if (screen == MENU_ENCODER_EDIT) {
        EncoderSettings &cfg = encoderSettings[encoderEditIndex];
        int numItems = (cfg.mode == ENCODER_MODE_SINGLE) ? 12 : 9;

        if (encoderMenuItemIndex < numItems - 3) { // Any item before Back/Save/Exit (Mode, CC, Val, Ch, Steps, Accel)
          editingValue = !editingValue;
          // Special case for mode toggle
          if (encoderMenuItemIndex == 0 && editingValue) {
              cfg.mode = (cfg.mode == ENCODER_MODE_SINGLE) ? ENCODER_MODE_RANGE : ENCODER_MODE_SINGLE;
              editingValue = false; // It's a toggle, not an edit mode
          }
          drawEncoderEditMenu();
        } else if (encoderMenuItemIndex == numItems - 3) { // Back
          screen = MENU_ENCODER_SELECT;
          editingValue = false;
          drawEncoderSelectMenu();
        } else if (encoderMenuItemIndex == numItems - 2) { // Save
          saveSettings();
          drawEncoderEditMenu();
        } else if (encoderMenuItemIndex == numItems - 1) { // Exit
          screen = HOME;
          for(int i=0; i<5; i++) { 
            midiEncoders[i]->setEncoderValue(0); 
            encoderAccumulatedSteps[i] = 0;
          }
          editingValue = false;
          drawHome();
        }
      } else if (screen == MENU_ENCODER_SELECT) {
        if (encoderSelectMenuIndex <= 4) { // Encoder 1-5
          encoderEditIndex = encoderSelectMenuIndex;
          encoderMenuItemIndex = 0;
          switchMenuTop = 0; // reuse for encoder edit menu scroll
          editingValue = false;
          screen = MENU_ENCODER_EDIT;
          rotary.setEncoderValue(0);
          lastEncSwitch = 0;
          drawEncoderEditMenu();
        } else { // Back
          screen = MENU_MAIN;
          drawMainMenu();
        }

      } else if (screen == MENU_MAIN) {
        // select item
        if (mainMenuIndex == 0) {         // Switch Config
          screen = MENU_SWITCH_SELECT;
          switchSelectMenuIndex = 0;
          switchSelectMenuTop = 0;
          drawSwitchSelectMenu();
        } else if (mainMenuIndex == 1) {    // Encoder Config
          screen = MENU_ENCODER_SELECT;
          encoderSelectMenuIndex = 0;
          encoderSelectMenuTop = 0;
          drawEncoderSelectMenu();
        } else if (mainMenuIndex == 2) {    // WiFi Settings
          screen = MENU_WIFI;
          wifiMenuIndex = 0;
          wifiMenuTop = 0;
          drawWifiMenu();
        } else if (mainMenuIndex == 3) {    // About
          screen = ABOUT;
          drawAbout();
        } else if (mainMenuIndex == 4) {    // Factory Reset
          screen = MENU_CONFIRM_RESET;
          confirmResetSelection = 0; // Default to NO
          drawConfirmReset();
        } else if (mainMenuIndex == 5) {    // Reboot
          display.clearDisplay();
          display.setTextSize(1);
          display.setCursor(10, 25);
          display.print("Rebooting...");
          display.display();
          delay(1000);
          ESP.restart();
        } else if (mainMenuIndex == 6) {    // Exit
          screen = HOME;
          for(int i=0; i<5; i++) { 
            midiEncoders[i]->setEncoderValue(0); 
            encoderAccumulatedSteps[i] = 0;
          }
          drawHome();
        }
      } else if (screen == MENU_WIFI) {
        if (wifiMenuIndex == 0) { // Enable
          wifiEnabled = !wifiEnabled;
          if (wifiEnabled) {
            WiFi.mode(WIFI_STA);
            WiFi.setHostname(BLE_NAME);
            if (strlen(wifiSsid) > 0) WiFi.begin(wifiSsid, wifiPass);
          } else {
            WiFi.disconnect(true);
            WiFi.mode(WIFI_OFF);
          }
          drawWifiMenu();
        } else if (wifiMenuIndex == 1) { // Connect/Disconnect
          if (WiFi.status() == WL_CONNECTED) {
            WiFi.disconnect();
          } else if (wifiEnabled && strlen(wifiSsid) > 0) {
            WiFi.begin(wifiSsid, wifiPass);
          }
          drawWifiMenu();
        } else if (wifiMenuIndex == 2) { // Info
          screen = MENU_WIFI_INFO;
          drawWifiInfo();
        } else if (wifiMenuIndex == 3) { // Edit SSID
          startKeyboard(wifiSsid, sizeof(wifiSsid));
        } else if (wifiMenuIndex == 4) { // Edit Password
          startKeyboard(wifiPass, sizeof(wifiPass));
        } else if (wifiMenuIndex == 5) { // Scan for Networks
          screen = MENU_WIFI_SCAN;
          display.clearDisplay();
          drawStatusBar();
          display.setTextSize(1);
          display.setCursor(2, 20);
          display.print("Scanning for WiFi...");
          display.display();
          scanResultCount = WiFi.scanNetworks();
          wifiScanIndex = 0;
          drawWifiScan();
        } else if (wifiMenuIndex == 6) { // Save
          saveSettings();
          if (wifiEnabled) {
            WiFi.disconnect(true);
            delay(100);
            WiFi.begin(wifiSsid, wifiPass);
          }
          drawWifiMenu();
        } else if (wifiMenuIndex == 7) { // Forget
          wifiSsid[0] = '\0';
          wifiPass[0] = '\0';
          saveSettings();
          if (wifiEnabled) {
            WiFi.disconnect(true);
          }
          drawWifiMenu();
        } else if (wifiMenuIndex == 8) { // Back
          screen = MENU_MAIN;
          drawMainMenu();
        }
      } else if (screen == MENU_WIFI_SCAN) {
        if (scanResultCount > 0) {
          String selectedSsid = WiFi.SSID(wifiScanIndex);

          // If a new SSID is selected, update it and clear the old password.
          if (selectedSsid != wifiSsid) {
            strncpy(wifiSsid, selectedSsid.c_str(), sizeof(wifiSsid) - 1);
            wifiSsid[sizeof(wifiSsid) - 1] = '\0';
            wifiPass[0] = '\0'; // Clear password for new SSID
          }

          // If the selected network is open and WiFi is enabled, try to connect automatically.
          if (wifiEnabled && WiFi.encryptionType(wifiScanIndex) == WIFI_AUTH_OPEN) {
            WiFi.disconnect(true);
            delay(100);
            WiFi.begin((const char*)wifiSsid, (const char*)""); // Connect with empty password
          }
        }
        WiFi.scanDelete();
        screen = MENU_WIFI;
        drawWifiMenu();
      } else if (screen == KEYBOARD) {
        int len = strlen(kbdInputBuffer);
        if (kbdCursorY == 3) { // Special keys row
          if (kbdCursorX == 0) { // Cancel
            strlcpy(kbdInputBuffer, kbdOriginalBuffer, kbdInputMaxLength);
            screen = screenBeforeKeyboard;
            if (screen == MENU_WIFI) drawWifiMenu();
          }
          else if (kbdCursorX == 9) { // OK
            screen = screenBeforeKeyboard;
            // Redraw the screen we returned to
            if (screen == MENU_WIFI) drawWifiMenu();
            else drawHome(); // Fallback
          }
          else if (kbdCursorX == 8) { kbdMode = (kbdMode == K_SYM) ? K_LOWER : K_SYM; } // SYM
          else if (kbdCursorX == 7) { kbdMode = (kbdMode == K_NUM) ? K_LOWER : K_NUM; } // 123
          else if (kbdCursorX == 6) { kbdMode = (kbdMode == K_UPPER) ? K_LOWER : K_UPPER; } // CAPS
          else if (kbdCursorX == 5) { if (len > 0) kbdInputBuffer[len - 1] = '\0'; } // Backspace
          else if (kbdCursorX == 4) { if (len < kbdInputMaxLength - 1) strcat(kbdInputBuffer, " "); } // Space
        } else { // Character keys
          if (len < kbdInputMaxLength - 1) {
            const char* charset;
            if (kbdMode == K_UPPER) charset = kbd_upper;
            else if (kbdMode == K_NUM) charset = kbd_num;
            else if (kbdMode == K_SYM) charset = kbd_sym;
            else charset = kbd_lower;
            int char_idx = kbdCursorY * 10 + kbdCursorX;
            if (char_idx < strlen(charset)) {
              char c[2] = {charset[char_idx], '\0'};
              strcat(kbdInputBuffer, c);
            }
          }
        }
        drawKeyboard();
      } else if (screen == ABOUT) {
        // any short press returns to main menu
        screen = MENU_MAIN;
        drawMainMenu();
      } else if (screen == MENU_CONFIRM_RESET) {
        if (confirmResetSelection == 1) { // YES was selected
          display.clearDisplay();
          display.setTextSize(1);
          display.setTextColor(WHITE);
          display.setCursor(10, 20);
          display.println("Factory Reset...");
          display.display();
          if (SPIFFS.exists(FILE_PATH)) {
            SPIFFS.remove(FILE_PATH);
          }
          delay(1000);
          display.setCursor(10, 35);
          display.println("Rebooting now.");
          display.display();
          delay(1500);
          ESP.restart();
        } else { // NO was selected
          screen = MENU_MAIN;
          drawMainMenu();
        }
      }
    }
  }

  // --- MIDI Encoder Handling (only on HOME screen) ---
  if (screen == HOME) {
    for (int i = 0; i < 5; i++) {
      long value = midiEncoders[i]->readEncoder();
      if (value != 0) { // A change has occurred
        EncoderSettings &cfg = encoderSettings[i];
        if (cfg.mode == ENCODER_MODE_SINGLE) {
          encoderAccumulatedSteps[i] += value;
        if (encoderAccumulatedSteps[i] >= cfg.singleModeSteps) {
          // Rotated right enough
          sendCC(cfg.right.cc, cfg.right.val, cfg.right.ch);
          encoderAccumulatedSteps[i] = 0;
        } else if (encoderAccumulatedSteps[i] <= -cfg.singleModeSteps) {
          // Rotated left enough
          sendCC(cfg.left.cc, cfg.left.val, cfg.left.ch);
          encoderAccumulatedSteps[i] = 0;
          }
        } else if (cfg.mode == ENCODER_MODE_RANGE) { // ENCODER_MODE_RANGE
          cfg.currentValue += value; // value is the change from the encoder
          cfg.currentValue = constrain(cfg.currentValue, cfg.rangeMin, cfg.rangeMax);
          sendCC(cfg.left.cc, cfg.currentValue, cfg.left.ch);
        }
        midiEncoders[i]->setEncoderValue(0); // Reset for next relative read
      }
    }
  }

  if (screen == MENU_MAIN) {
    long v = rotary.readEncoder();
    if (v != lastEncMain) {
      lastActivityTime = millis();
      int dir = (v > lastEncMain) ? 1 : -1;
      lastEncMain = v;
      mainMenuIndex += dir;
      mainMenuIndex = constrain(mainMenuIndex, 0, 6); // Switch Cfg, Enc Cfg, WiFi, About, Reset, Reboot, Exit

      // Scrolling logic for main menu
      const int visibleItems = 5;
      if (mainMenuIndex < mainMenuTop) {
        mainMenuTop = mainMenuIndex;
      } else if (mainMenuIndex >= mainMenuTop + visibleItems) {
        mainMenuTop = mainMenuIndex - visibleItems + 1;
      }
      drawMainMenu();
    }
  } else if (screen == MENU_SWITCH_SELECT) {
    long v = rotary.readEncoder();
    if (v != lastEncMain) {
      lastActivityTime = millis();
      int dir = (v > lastEncMain) ? 1 : -1;
      lastEncMain = v;
      switchSelectMenuIndex += dir;
      switchSelectMenuIndex = constrain(switchSelectMenuIndex, 0, 5); // Sw1..4, Enc, Back

      const int visibleItems = 4;
      if (switchSelectMenuIndex < switchSelectMenuTop) {
        switchSelectMenuTop = switchSelectMenuIndex;
      } else if (switchSelectMenuIndex >= switchSelectMenuTop + visibleItems) {
        switchSelectMenuTop = switchSelectMenuIndex - visibleItems + 1;
      }
      drawSwitchSelectMenu();
    }
  } else if (screen == MENU_ENCODER_SELECT) {
    long v = rotary.readEncoder();
    if (v != lastEncMain) {
      lastActivityTime = millis();
      int dir = (v > lastEncMain) ? 1 : -1;
      lastEncMain = v;
      encoderSelectMenuIndex += dir;
      encoderSelectMenuIndex = constrain(encoderSelectMenuIndex, 0, 5); // Enc 1-5, Back

      const int visibleItems = 4;
      if (encoderSelectMenuIndex < encoderSelectMenuTop) {
        encoderSelectMenuTop = encoderSelectMenuIndex;
      } else if (encoderSelectMenuIndex >= encoderSelectMenuTop + visibleItems) {
        encoderSelectMenuTop = encoderSelectMenuIndex - visibleItems + 1;
      }
      drawEncoderSelectMenu();
    }
  } else if (screen == MENU_ENCODER_EDIT) {
    long v = rotary.readEncoder();
    if (v != lastEncSwitch) {
      lastActivityTime = millis();
      int dir = (v > lastEncSwitch) ? 1 : -1;
      lastEncSwitch = v;
      EncoderSettings &cfg = encoderSettings[encoderEditIndex];
      int numItems = (cfg.mode == ENCODER_MODE_SINGLE) ? 12 : 9;

      if (!editingValue) {
        encoderMenuItemIndex += dir;
        encoderMenuItemIndex = constrain(encoderMenuItemIndex, 0, numItems - 1);

        // Scrolling logic
        const int visibleItems = 4;
        if (encoderMenuItemIndex < switchMenuTop) { // switchMenuTop is reused for encoder menu scroll
          switchMenuTop = encoderMenuItemIndex;
        } else if (encoderMenuItemIndex >= switchMenuTop + visibleItems) {
          switchMenuTop = encoderMenuItemIndex - visibleItems + 1;
        }
      } else {
        if (cfg.mode == ENCODER_MODE_SINGLE) {
          if      (encoderMenuItemIndex == 1) { cfg.left.cc = wrapValue(cfg.left.cc, dir, ENCODER_CC_MIN, ENCODER_CC_MAX); }
          else if (encoderMenuItemIndex == 2) { cfg.left.val = wrapValue(cfg.left.val, dir, ENCODER_VAL_MIN, ENCODER_VAL_MAX); }
          else if (encoderMenuItemIndex == 3) { cfg.left.ch = wrapValue(cfg.left.ch, dir, ENCODER_CH_MIN, ENCODER_CH_MAX); }
          else if (encoderMenuItemIndex == 4) { cfg.right.cc = wrapValue(cfg.right.cc, dir, ENCODER_CC_MIN, ENCODER_CC_MAX); }
          else if (encoderMenuItemIndex == 5) { cfg.right.val = wrapValue(cfg.right.val, dir, ENCODER_VAL_MIN, ENCODER_VAL_MAX); }
          else if (encoderMenuItemIndex == 6) { cfg.right.ch = wrapValue(cfg.right.ch, dir, ENCODER_CH_MIN, ENCODER_CH_MAX); }
          else if (encoderMenuItemIndex == 7) { cfg.singleModeSteps = wrapValue(cfg.singleModeSteps, dir, ENCODER_STEPS_MIN, ENCODER_STEPS_MAX); }
          else if (encoderMenuItemIndex == 8) { // Accel
            cfg.acceleration += (dir * 10);
            cfg.acceleration = constrain(cfg.acceleration, 0, 1000);
            midiEncoders[encoderEditIndex]->setAcceleration(cfg.acceleration);
          }
        } else { // Range Mode
          if      (encoderMenuItemIndex == 1) { cfg.left.cc = wrapValue(cfg.left.cc, dir, ENCODER_CC_MIN, ENCODER_CC_MAX); }
          else if (encoderMenuItemIndex == 2) { cfg.left.ch = wrapValue(cfg.left.ch, dir, ENCODER_CH_MIN, ENCODER_CH_MAX); }
          else if (encoderMenuItemIndex == 3) { // Editing rangeMin
            cfg.rangeMin += dir;
            cfg.rangeMin = constrain(cfg.rangeMin, ENCODER_RANGE_MIN, cfg.rangeMax);
          }
          else if (encoderMenuItemIndex == 4) { // Editing rangeMax
            cfg.rangeMax += dir;
            cfg.rangeMax = constrain(cfg.rangeMax, cfg.rangeMin, ENCODER_RANGE_MAX);
          }
          else if (encoderMenuItemIndex == 5) { // Accel
            cfg.acceleration += (dir * 10);
            cfg.acceleration = constrain(cfg.acceleration, 0, 1000);
            midiEncoders[encoderEditIndex]->setAcceleration(cfg.acceleration);
          }
        }
      }
      drawEncoderEditMenu();
    }
  } else if (screen == MENU_SWITCH) {
    long v = rotary.readEncoder();
    if (v != lastEncSwitch) {
      lastActivityTime = millis();
      int dir = (v > lastEncSwitch) ? 1 : -1;
      lastEncSwitch = v;
      SwitchConfig &cfg = switchConfigs[switchEditIndex];
      int numItems = (cfg.mode == SWITCH_MODE_TOGGLE) ? 8 : 7;

      if (!editingValue) {
        switchMenuIndex += dir;
        switchMenuIndex = constrain(switchMenuIndex, 0, numItems - 1);

        // Scrolling logic
        const int visibleItems = 4; // How many items fit on screen
        if (switchMenuIndex < switchMenuTop) {
          switchMenuTop = switchMenuIndex;
        } else if (switchMenuIndex >= switchMenuTop + visibleItems) {
          switchMenuTop = switchMenuIndex - visibleItems + 1;
        }
      } else {
        // adjust value
        if (cfg.mode == SWITCH_MODE_MOMENTARY) {
            if      (switchMenuIndex == 1) { cfg.cc = wrapValue(cfg.cc, dir, SWITCH_CC_MIN, SWITCH_CC_MAX); }
            else if (switchMenuIndex == 2) { cfg.val = wrapValue(cfg.val, dir, SWITCH_VAL_MIN, SWITCH_VAL_MAX); }
            else if (switchMenuIndex == 3) { cfg.ch = wrapValue(cfg.ch, dir, SWITCH_CH_MIN, SWITCH_CH_MAX); }
        } else { // TOGGLE
            if      (switchMenuIndex == 1) { cfg.cc = wrapValue(cfg.cc, dir, SWITCH_CC_MIN, SWITCH_CC_MAX); }
            else if (switchMenuIndex == 2) { cfg.val = wrapValue(cfg.val, dir, SWITCH_VAL_MIN, SWITCH_VAL_MAX); }
            else if (switchMenuIndex == 3) { cfg.altVal = wrapValue(cfg.altVal, dir, SWITCH_VAL_MIN, SWITCH_VAL_MAX); }
            else if (switchMenuIndex == 4) { cfg.ch = wrapValue(cfg.ch, dir, SWITCH_CH_MIN, SWITCH_CH_MAX); }
        }
      }
      drawSwitchMenu();
    }
  } else if (screen == MENU_WIFI) {
    long v = rotary.readEncoder();
    if (v != lastEncMain) {
      lastActivityTime = millis();
      int dir = (v > lastEncMain) ? 1 : -1;
      lastEncMain = v;
      wifiMenuIndex += dir;
      wifiMenuIndex = constrain(wifiMenuIndex, 0, 8); // Enable, Connect, Info, SSID, Pass, Scan, Save, Forget, Back
      
      const int visibleItems = 4;
      if (wifiMenuIndex < wifiMenuTop) {
        wifiMenuTop = wifiMenuIndex;
      } else if (wifiMenuIndex >= wifiMenuTop + visibleItems) {
        wifiMenuTop = wifiMenuIndex - visibleItems + 1;
      }
      drawWifiMenu();
    }
  } else if (screen == MENU_WIFI_SCAN) {
    long v = rotary.readEncoder();
    if (v != lastEncMain) {
      lastActivityTime = millis();
      int dir = (v > lastEncMain) ? 1 : -1;
      lastEncMain = v;
      if (scanResultCount > 0) {
        wifiScanIndex += dir;
        wifiScanIndex = constrain(wifiScanIndex, 0, scanResultCount - 1);
      }
      drawWifiScan();
    }
  } else if (screen == KEYBOARD) {
    long v = rotary.readEncoder();
    if (v != lastEncMain) {
      lastActivityTime = millis();
      int dir = (v > lastEncMain) ? 1 : -1;
      lastEncMain = v;
      int current_pos = kbdCursorY * 10 + kbdCursorX;
      current_pos += dir;
      int max_pos = 39; // 4 rows * 10 cols - 1
      if (current_pos < 0) current_pos = max_pos;
      if (current_pos > max_pos) current_pos = 0;
      kbdCursorY = current_pos / 10;
      kbdCursorX = current_pos % 10;
      drawKeyboard();
    }
  } else if (screen == MENU_CONFIRM_RESET) {
    long v = rotary.readEncoder();
    if (v != lastEncMain) {
      lastActivityTime = millis();
      lastEncMain = v; // any change toggles
      confirmResetSelection = 1 - confirmResetSelection; // Toggle between 0 and 1
      drawConfirmReset();
    }
  }

  // Physical switches (debounced, falling edge)
  uint32_t now = millis();
  for (int i=0;i<4;i++){
    bool curHigh = digitalRead(SW_PINS[i]);  // HIGH idle
    if (curHigh != swPrev[i] && (now - swLastChange[i] >= DEBOUNCE_MS)) {
      swPrev[i] = curHigh; swLastChange[i] = now;
      if (curHigh == LOW) {
        sendCCForIndex(i);
        lastPressedIndex = i;
        if (screen == HOME) drawHome();
      }
    }
  }

  delay(2);
}

/* ========================== Rendering ========================= */
void drawStatusBar(){
  // Top strip contents: BT icon + battery icon at right
  // Icons from right to left: Battery, WiFi, Bluetooth
  int bx = SCREEN_WIDTH - 16; // Battery icon at far right (14x8 body + 2x4 tip)
  int wifiX = bx - 10;        // WiFi icon (8px) with 2px padding
  int btX = wifiX - 10;       // BT icon (8px) with 2px padding
  int usbX = btX - 12;       // USB status position

  // Bluetooth
  if (btConnected) {
    display.drawBitmap(btX, 0, BT_FILLED, 8, 8, WHITE);
  } else if (btIconVisible) { // Not connected, draw if visible (for blinking)
    display.drawBitmap(btX, 0, BT_OUTLINE, 8, 8, WHITE);
  }

  // USB Host Status
  if (usbMidiConnected) {
    display.drawBitmap(usbX, 0, USB_ICON, 8, 8, WHITE);
  }

  // WiFi
  if (wifiEnabled) {
    const uint8_t* wifiIcon = WIFI_DOT;
    if (wifiStatus == WL_CONNECTED) {
        long rssi = WiFi.RSSI();
        if (rssi >= -67) wifiIcon = WIFI_HIGH;       // Excellent
        else if (rssi >= -70) wifiIcon = WIFI_MED;   // Good
        else wifiIcon = WIFI_LOW;                    // Fair
    }
    display.drawBitmap(wifiX, 0, wifiIcon, 8, 8, WHITE);
  }

  // Battery
  int by = 0;
  display.drawRect(bx, by, 14, 8, WHITE);
  display.drawRect(bx + 14, by + 2, 2, 4, WHITE);

  if (isCharging) {
    // Draw a lightning bolt icon inside the battery
    display.fillRect(bx + 1, by + 1, 12, 6, BLACK); // Clear inside
    display.drawLine(bx + 7, by + 2, bx + 5, by + 4, WHITE);
    display.drawLine(bx + 5, by + 4, bx + 8, by + 4, WHITE);
    display.drawLine(bx + 8, by + 4, bx + 6, by + 6, WHITE);
  } else {
    int fillW = map(batteryPercent, 0, 100, 0, 12);
    display.fillRect(bx + 1, by + 1, fillW, 6, WHITE);
  }
}

void redrawCurrentScreen() {
    switch(screen) {
      case HOME:               drawHome();             break;
      case MENU_MAIN:          drawMainMenu();         break;
      case MENU_SWITCH_SELECT: drawSwitchSelectMenu(); break;
      case MENU_SWITCH:        drawSwitchMenu();       break;
      case MENU_ENCODER_SELECT:drawEncoderSelectMenu();break;
      case MENU_ENCODER_EDIT:  drawEncoderEditMenu();  break;
      case MENU_WIFI:          drawWifiMenu();         break;
      case MENU_WIFI_SCAN:     drawWifiScan();         break;
      case MENU_WIFI_INFO:     drawWifiInfo();         break;
      case KEYBOARD:           drawKeyboard();         break;
      case ABOUT:              drawAbout();            break;
      case MENU_CONFIRM_RESET: drawConfirmReset();     break;
    }
}

void drawMenuList(const char* title, const char* items[], int itemCount, int selectedIndex, int topIndex) {
  display.clearDisplay();
  drawStatusBar();

  display.setTextSize(1);
  display.setTextColor(WHITE);

  int startY = 12;
  if (title) {
    display.setCursor(2, 10);
    display.print(title);
    startY = 22;
  }

  const int visibleItems = 5;
  const int itemHeight = 10;

  for (int i = 0; i < visibleItems; i++) {
    int itemIndex = topIndex + i;
    if (itemIndex >= itemCount) break;

    int currentY = startY + i * itemHeight;
    
    if (itemIndex == selectedIndex) {
      display.fillRect(0, currentY - 1, SCREEN_WIDTH, itemHeight, WHITE);
      display.setTextColor(BLACK);
    } else {
      display.setTextColor(WHITE);
    }
    display.setCursor(2, currentY);
    display.print(items[itemIndex]);
  }
  display.display();
}

void drawHome(){
  display.clearDisplay();
  drawStatusBar();
  
  display.setTextSize(6);

  char txt[4] = "00";
  bool isInverted = false;

  if (lastPressedIndex >= 0) {
    SwitchConfig &cfg = switchConfigs[lastPressedIndex];
    int ch = cfg.ch;
    ch = constrain(ch, 0, 99);
    snprintf(txt, sizeof(txt), "%02d", ch);

    if (cfg.mode == SWITCH_MODE_TOGGLE && cfg.state) {
      isInverted = true;
    }
  }

  int16_t x1,y1; uint16_t w,h;
  display.getTextBounds(txt, 0, 0, &x1, &y1, &w, &h);
  int cx = (SCREEN_WIDTH - w)/2;
  int cy = ((SCREEN_HEIGHT - h)/2) + 6;  // below status bar

  if (isInverted) {
    const int padding = 4;
    display.fillRect(cx - padding, cy - padding, w + (padding - 2), h + (padding - 2), WHITE);
    display.setTextColor(BLACK);
  } else {
    display.setTextColor(WHITE);
  }

  display.setCursor(cx, cy);
  display.print(txt);

  // Draw blinking indicators for physical switches
  if (lastPressedIndex >= 0 && indicatorVisible) { // Only draw if an index is set and it's visible (blinking)
    const int sz = 8;     // Arrow size
    const int pad = 2;    // Padding from edge
    const int topY = 10;  // Top limit (below status bar)
    const int botY = SCREEN_HEIGHT - 1;
    const int rightX = SCREEN_WIDTH - 1 - pad;

    if (lastPressedIndex == 0) { // Switch 1: Top Left
      display.drawBitmap(pad, topY, ARROW_TL, sz, sz, WHITE);
    } 
    else if (lastPressedIndex == 1) { // Switch 2: Bottom Left
      display.drawBitmap(pad, botY - sz, ARROW_BL, sz, sz, WHITE);
    } 
    else if (lastPressedIndex == 3) { // Switch 4: Top Right
      display.drawBitmap(rightX - sz, topY, ARROW_TR, sz, sz, WHITE);
    } 
    else if (lastPressedIndex == 2) { // Switch 3: Bottom Right
      display.drawBitmap(rightX - sz, botY - sz, ARROW_BR, sz, sz, WHITE);
    } 
    else if (lastPressedIndex == 4) { // Encoder Btn: Bottom Center
      // Blinking line at bottom center to avoid overlapping with the number box
      int lineW = 20;
      display.drawFastHLine((SCREEN_WIDTH - lineW) / 2, botY, lineW, WHITE);
    }
  }

  display.display();
}

void drawMainMenu(){
  const char* items[] = {
    "Switch Config", "Encoder Config", "WiFi Settings", "About", "Factory Reset", "Reboot", "Exit"
  };
  drawMenuList(nullptr, items, 7, mainMenuIndex, mainMenuTop);
}

void drawSwitchSelectMenu() {
  const char* items[] = {
    "Switch 1", "Switch 2", "Switch 3", "Switch 4", "Encoder Btn", "Back"
  };
  drawMenuList("Select Switch", items, 6, switchSelectMenuIndex, switchSelectMenuTop);
}

void drawEncoderSelectMenu() {
  const char* items[] = {
    "Encoder 3 (UI)", "Encoder 1", "Encoder 2", "Encoder 4", "Encoder 5", "Back"
  };
  drawMenuList("Select Encoder", items, 6, encoderSelectMenuIndex, encoderSelectMenuTop);
}

void drawEncoderEditMenu() {
  display.clearDisplay();
  drawStatusBar();

  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(2, 10);
  display.print("Encoder "); display.print(encoderEditIndex + 1); display.print(" Config");

  EncoderSettings &cfg = encoderSettings[encoderEditIndex];
  const char** labels;
  int numItems;
 
  static const char* single_labels[] = {"Mode", "L CC", "L Val", "L Ch", "R CC", "R Val", "R Ch", "Steps", "Accel", "Back", "Save", "Exit"};
  static const char* range_labels[] = {"Mode", "CC", "Ch", "Min", "Max", "Accel", "Back", "Save", "Exit"};

  if (cfg.mode == ENCODER_MODE_SINGLE) {
    labels = single_labels;
    numItems = 12;
  } else {
    labels = range_labels;
    numItems = 9;
  }

  const int visibleItems = 4;
  const int itemHeight = 11;
  int startY = 20;

  for (int i = 0; i < visibleItems; i++) {
    int itemIndex = switchMenuTop + i; // Re-use switchMenuTop for scrolling
    if (itemIndex >= numItems) break;

    int currentY = startY + i * itemHeight;
    bool highlight = (itemIndex == encoderMenuItemIndex);

    if (highlight && !editingValue) {
      display.fillRect(0, currentY - 2, SCREEN_WIDTH, itemHeight, WHITE);
      display.setTextColor(BLACK);
    } else {
      display.setTextColor(WHITE);
    }

    display.setCursor(2, currentY);
    display.print(labels[itemIndex]);

    // Draw values
    display.setCursor(70, currentY);
    if (highlight && editingValue) {
        display.fillRect(68, currentY - 2, 40, itemHeight, WHITE);
        display.setTextColor(BLACK);
    } else if (highlight) {
        display.setTextColor(BLACK);
    }

    if (itemIndex == 0) { display.print(cfg.mode == ENCODER_MODE_SINGLE ? "Single" : "Range"); }
    else if (cfg.mode == ENCODER_MODE_SINGLE) {
      if      (itemIndex == 1) display.print(cfg.left.cc);
      else if (itemIndex == 2) display.print(cfg.left.val);
      else if (itemIndex == 3) display.print(cfg.left.ch);
      else if (itemIndex == 4) display.print(cfg.right.cc);
      else if (itemIndex == 5) display.print(cfg.right.val);
      else if (itemIndex == 6) display.print(cfg.right.ch);
      else if (itemIndex == 7) display.print(cfg.singleModeSteps);
      else if (itemIndex == 8) display.print(cfg.acceleration);
    } else { // Range Mode
      if      (itemIndex == 1) display.print(cfg.left.cc);
      else if (itemIndex == 2) display.print(cfg.left.ch);
      else if (itemIndex == 3) display.print(cfg.rangeMin);
      else if (itemIndex == 4) display.print(cfg.rangeMax);
      else if (itemIndex == 5) display.print(cfg.acceleration);
    }
  }
  display.display();
}

void drawWifiMenu() {
  display.clearDisplay();
  drawStatusBar();

  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(2, 10);
  display.print("WiFi Settings");

  //display.setCursor(70, 10);
  //if (wifiEnabled) {
  //  uint8_t s = WiFi.status();
  //  if (s == WL_CONNECTED) {
  //    display.print(WiFi.localIP());
  //  } else if (s == WL_NO_SSID_AVAIL || s == WL_CONNECT_FAILED || s == WL_CONNECTION_LOST) {
  //    display.print("Failed");
  //  } else { // IDLE, DISCONNECTED
  //    display.print("Connecting...");
  //  }
  //} else {
  //  display.print("Disabled");
  //}

  const char* labels[9] = {"Enable", (WiFi.status() == WL_CONNECTED ? "Disconnect" : "Connect"), "Info", "SSID", "Password", "Scan for Networks", "Save", "Forget", "Back"};

  const int visibleItems = 4;
  const int itemHeight = 11;
  int startY = 22;

  for (int i = 0; i < visibleItems; i++) {
    int itemIndex = wifiMenuTop + i;
    if (itemIndex > 8) break;

    int currentY = startY + i * itemHeight;
    bool highlight = (itemIndex == wifiMenuIndex);

    if (highlight) {
      display.fillRect(0, currentY - 2, SCREEN_WIDTH, itemHeight, WHITE);
      display.setTextColor(BLACK);
    } else {
      display.setTextColor(WHITE);
    }

    display.setCursor(2, currentY);
    display.print(labels[itemIndex]);

    // Draw values
    display.setCursor(70, currentY);
    if (itemIndex == 0) { // Enable
      display.print(wifiEnabled ? "ON" : "OFF");
    } else if (itemIndex == 1 || itemIndex == 2) { // Connect/Disconnect or Info
      // No value, just a navigation item
    } else if (itemIndex == 3) { // SSID
      int max_len = (SCREEN_WIDTH - 75) / 6;
      int ssid_len = strlen(wifiSsid);
      if (ssid_len > max_len) {
          static int scroll_offset = 0;
          scroll_offset++;

          char temp[sizeof(wifiSsid) + 4 + sizeof(wifiSsid)];
          snprintf(temp, sizeof(temp), "%s   %s", wifiSsid, wifiSsid);

          if (scroll_offset >= ssid_len + 3) {
              scroll_offset = 0;
          }

          char sub[max_len + 1];
          strncpy(sub, temp + scroll_offset, max_len);
          sub[max_len] = '\0';
          display.print(sub);
      } else {
          display.print(wifiSsid);
      }
    } else if (itemIndex == 4) { // Password
      //for(int j=0; j<strlen(wifiPass); j++) display.print('*');
      for(int j=0; j<8; j++) display.print('*');  
    }
  }
  display.display();
}

void drawWifiScan() {
  display.clearDisplay();
  drawStatusBar();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(2, 10);
  display.print("Select WiFi Network");

  if (scanResultCount <= 0) {
    display.setCursor(2, 30);
    display.print("No networks found.");
  } else {
    const int visibleItems = 5;
    const int itemHeight = 10;
    int startY = 20;
    int topItem = wifiScanIndex - (visibleItems / 2);
    if (topItem < 0) topItem = 0;
    if (topItem > scanResultCount - visibleItems) topItem = scanResultCount - visibleItems;
    if (topItem < 0) topItem = 0;

    for (int i = 0; i < visibleItems; i++) {
      int itemIndex = topItem + i;
      if (itemIndex >= scanResultCount) break;

      int currentY = startY + i * itemHeight;
      if (itemIndex == wifiScanIndex) {
        display.fillRect(0, currentY - 1, SCREEN_WIDTH, itemHeight, WHITE);
        display.setTextColor(BLACK);
      } else {
        display.setTextColor(WHITE);
      }
      display.setCursor(2, currentY);
      display.print(WiFi.SSID(itemIndex));
    }
  }
  display.display();
}

void drawWifiInfo(){
  display.clearDisplay();
  drawStatusBar();

  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(2, 14);
  display.println("WiFi Information");
  
  display.setCursor(2, 28);
  display.print("Status: ");
  if (wifiEnabled) {
    uint8_t s = WiFi.status();
    if (s == WL_CONNECTED) {
      display.println("Connected");
      display.setCursor(2, 40);
      display.print("IP: ");
      display.print(WiFi.localIP());
      display.setCursor(2, 52);
      display.print("SSID: ");
      display.print(WiFi.SSID());
    } else if (s == WL_NO_SSID_AVAIL) {
      display.println("SSID Not Found");
    } else if (s == WL_CONNECT_FAILED) {
      display.println("Connection Failed");
    } else if (s == WL_CONNECTION_LOST) {
      display.println("Connection Lost");
    } else if (s == WL_DISCONNECTED) {
      display.println("Disconnected");
    } else { // IDLE
      display.println("Connecting...");
    }
  } else {
    display.println("Disabled");
  }

  display.display();
}

void drawKeyboard() {
  display.clearDisplay();
  display.setTextSize(1);

  // Draw input buffer
  display.setTextColor(WHITE);
  display.drawRect(0, 0, SCREEN_WIDTH, 12, WHITE);
  display.setCursor(2, 2);
  display.print(kbdInputBuffer);

  // Draw keyboard characters
  const char* charset;
  if (kbdMode == K_UPPER) charset = kbd_upper;
  else if (kbdMode == K_NUM) charset = kbd_num;
  else if (kbdMode == K_SYM) charset = kbd_sym;
  else charset = kbd_lower;

  int char_w = 12;
  int char_h = 12;
  int start_y = 14;

  for (int y = 0; y < 4; y++) {
    for (int x = 0; x < 10; x++) {
      int char_idx = y * 10 + x;
      int cur_x = x * char_w;
      int cur_y = start_y + y * char_h;

      bool is_cursor = (x == kbdCursorX && y == kbdCursorY);
      if (is_cursor) {
        display.fillRect(cur_x, cur_y, char_w, char_h, WHITE);
        display.setTextColor(BLACK);
      } else {
        display.setTextColor(WHITE);
      }

      display.setCursor(cur_x + 3, cur_y + 2);

      if (y == 3) { // Special keys row
        if (x == 0) display.print("X"); // Cancel
        else if (x == 4) display.print("_"); // Space
        else if (x == 5) display.print("<");
        else if (x == 6) display.print("C");
        else if (x == 7) display.print("1");
        else if (x == 8) display.print("@");
        else if (x == 9) display.print("OK");
      } else {
        if (char_idx < strlen(charset)) {
          display.print(charset[char_idx]);
        }
      }
    }
  }

  display.display();
}

void drawSwitchMenu(){
  display.clearDisplay();
  drawStatusBar();

  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(2, 10);
  if (switchEditIndex <= 3) {
    display.print("Switch "); display.print(switchEditIndex + 1);
  } else {
    display.print("Encoder Button");
  }

  SwitchConfig &cfg = switchConfigs[switchEditIndex];

  const char* labels_mom[] = {"Mode", "CC", "Value", "Channel", "Back", "Save", "Exit"};
  const char* labels_tgl[] = {"Mode", "CC", "Value", "Alt Value", "Channel", "Back", "Save", "Exit"};
  const char** labels = (cfg.mode == SWITCH_MODE_TOGGLE) ? labels_tgl : labels_mom;
  int numItems = (cfg.mode == SWITCH_MODE_TOGGLE) ? 8 : 7;

  const int visibleItems = 4;
  const int itemHeight = 11;
  int startY = 20;

  for (int i = 0; i < visibleItems; i++) {
    int itemIndex = switchMenuTop + i;
    if (itemIndex >= numItems) break;

    int currentY = startY + i * itemHeight;
    bool highlight = (itemIndex == switchMenuIndex);

    // Draw highlight bar for selected item (when not editing a value)
    if (highlight && !editingValue) {
        display.fillRect(0, currentY - 2, SCREEN_WIDTH, itemHeight, WHITE);
        display.setTextColor(BLACK);
    } else {
        display.setTextColor(WHITE);
    }

    display.setCursor(2, currentY);
    display.print(labels[itemIndex]);

    // Draw values
    if (itemIndex < numItems - 3) { // Not Back, Save, Exit
        display.setCursor(70, currentY);
        if (highlight && editingValue) {
            display.fillRect(68, currentY - 2, 40, itemHeight, WHITE);
            display.setTextColor(BLACK);
        } else if (highlight) {
            display.setTextColor(BLACK);
        }

        if (itemIndex == 0) { // Mode
            display.print(cfg.mode == SWITCH_MODE_TOGGLE ? "Toggle" : "Momentary");
        } else if (cfg.mode == SWITCH_MODE_MOMENTARY) {
            if      (itemIndex == 1) display.print(cfg.cc);
            else if (itemIndex == 2) display.print(cfg.val);
            else if (itemIndex == 3) display.print(cfg.ch);
        } else { // TOGGLE
            if      (itemIndex == 1) display.print(cfg.cc);
            else if (itemIndex == 2) display.print(cfg.val);
            else if (itemIndex == 3) display.print(cfg.altVal);
            else if (itemIndex == 4) display.print(cfg.ch);
        }
    }
  }
  display.display();
}

void drawAbout(){
  display.clearDisplay();
  drawStatusBar();

  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(2, 14);
  display.println("KHAIRIL MIDI SWITCH");
  display.println("v1.2.2026");
  display.print("Device: "); display.println(BLE_NAME);
  display.print("Bt: "); display.println(connectedDeviceAddress);
  display.print("Usb: "); display.println(usbMidiConnected ? "Connected" : "Not Connected");
  display.print("Batt: "); display.print(batteryPercent); display.println("%");

  display.display();
}

void drawConfirmReset() {
  display.clearDisplay();
  drawStatusBar();

  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(10, 20);
  display.println("Reset all settings?");
  display.setCursor(10, 30);
  display.println("This cannot be undone.");

  // Options: NO / YES
  display.setTextSize(2);
  if (confirmResetSelection == 0) { // NO is selected
    display.fillRect(5, 45, 55, 20, WHITE);
    display.setTextColor(BLACK);
    display.setCursor(15, 47);
    display.print("NO");

    display.setTextColor(WHITE);
    display.setCursor(75, 47);
    display.print("YES");
  } else { // YES is selected
    display.setTextColor(WHITE);
    display.setCursor(15, 47);
    display.print("NO");

    display.fillRect(65, 45, 55, 20, WHITE);
    display.setTextColor(BLACK);
    display.setCursor(75, 47);
    display.print("YES");
  }
  display.display();
}

void startKeyboard(char* buffer, int maxLength) {
  screenBeforeKeyboard = screen;
  screen = KEYBOARD;
  strlcpy(kbdOriginalBuffer, buffer, sizeof(kbdOriginalBuffer));
  kbdInputBuffer = buffer;
  kbdInputMaxLength = maxLength;
  kbdCursorX = 0;
  kbdCursorY = 0;
  kbdMode = K_LOWER;
  lastActivityTime = millis();
  rotary.setEncoderValue(0);
  drawKeyboard();
}

void startOTA() {
  ArduinoOTA.setHostname(BLE_NAME);

  // Optional: Set a password for updates
  // ArduinoOTA.setPassword("admin");

  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH) {
        type = "sketch";
      } else { // U_SPIFFS
        type = "filesystem";
        SPIFFS.end(); // Unmount SPIFFS to prevent corruption during update
      }
      Serial.println("Start updating " + type);

      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(WHITE);
      display.setCursor(10, 20);
      display.println("OTA Update Start");
      display.display();
    })
    .onEnd([]() {
      Serial.println("\nEnd");
      display.clearDisplay();
      display.setCursor(10, 20);
      display.println("OTA Update End");
      display.setCursor(10, 35);
      display.println("Rebooting...");
      display.display();
      delay(1000);
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      int percent = (progress / (total / 100));
      Serial.printf("Progress: %u%%\r", percent);
      
      display.clearDisplay();
      display.setTextSize(1);
      display.setTextColor(WHITE);
      display.setCursor(10, 20);
      display.println("OTA Update...");
      display.setCursor(10, 35);
      display.printf("Progress: %d%%", percent);
      display.display();
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");

      // Display error on screen
      display.clearDisplay();
      display.setCursor(10, 20);
      display.println("OTA Error!");
      display.display();
      delay(2000);
    });

  ArduinoOTA.begin();
  Serial.println("OTA Ready");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());
}

/* ========================== Battery ========================== */
void readBattery(){
  // Read more frequently to populate the smoothing buffer, but not on every loop
  if (millis() - lastBatteryRead < 100) return;
  lastBatteryRead = millis();

  // Perform a single raw reading
  float vAdc = analogRead(BATTERY_PIN) * (3.3f / 4095.0f);
  float currentVoltage = vAdc * ((R1 + R2) / R2);  // Apply voltage divider formula

  // Store the reading in a circular buffer for smoothing
  batteryReadings[batteryReadingIndex] = currentVoltage;
  batteryReadingIndex++;
  if (batteryReadingIndex >= BATTERY_SMOOTHING_SAMPLES) {
    batteryReadingIndex = 0;
    batteryBufferFilled = true; // The buffer is now full, use all samples for averaging
  }

  // Calculate the moving average of the readings
  float totalVoltage = 0;
  int samplesToAverage = batteryBufferFilled ? BATTERY_SMOOTHING_SAMPLES : batteryReadingIndex;
  if (samplesToAverage == 0) return; // Avoid division by zero

  for (int i = 0; i < samplesToAverage; i++) {
    totalVoltage += batteryReadings[i];
  }
  batteryVoltage = totalVoltage / samplesToAverage; // This is our smoothed voltage
  
  // A simple heuristic for charging detection: if voltage is very near max, assume it's on a charger.
  // This is not perfect but a common way without a dedicated charging status pin.
  // A Li-ion charger holds the voltage at ~4.2V.
  const float CHARGING_THRESHOLD_V = 4.18f; 
  isCharging = (batteryVoltage > CHARGING_THRESHOLD_V);
  
  int pct = map((int)(batteryVoltage * 100), 300, 420, 0, 100); // Map 3.00V-4.20V to 0-100%
  batteryPercent = constrain(pct, 0, 100);
}

/* =========================== Storage ========================= */
//const char* FILE_PATH = "/settings.json";

void setDefaultSettings() {
  for (int i=0;i<5;i++){
    switchConfigs[i] = SwitchConfig(); // Reset to defaults
    switchConfigs[i].cc = 10+i;
    switchConfigs[i].ch = i+1;
  }
  for (int i=0;i<5;i++){ encoderSettings[i] = EncoderSettings(); }
}

void loadSettings(){
  if (!SPIFFS.exists(FILE_PATH)) {
    Serial.println("Settings file not found, using defaults.");
    // When no file, the SwitchConfig array will have the default values from its definition.
    // We could call setDefaultSettings() here if we wanted different defaults than the struct provides.
    // setDefaultSettings();
    return;
  }

  File f = SPIFFS.open(FILE_PATH, FILE_READ);
  if (!f) {
    Serial.println("Failed to open settings file for reading.");
    return;
  }
  
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, f);
  f.close(); // Close file regardless of parsing outcome
  if (error) {
    Serial.print("deserializeJson() failed: ");
    Serial.println(error.c_str());
    return;
  }

  JsonArray mappings = doc["mappings"];
  if (!mappings.isNull()) {
    int i = 0;
    for(JsonObject map_obj : mappings) {
      if (i >= 5) break;
      const char* modeStr = map_obj["mode"] | "momentary";
      switchConfigs[i].mode = (strcmp(modeStr, "toggle") == 0) ? SWITCH_MODE_TOGGLE : SWITCH_MODE_MOMENTARY;
      switchConfigs[i].cc = map_obj["cc"] | switchConfigs[i].cc;
      switchConfigs[i].val = map_obj["val"] | switchConfigs[i].val;
      switchConfigs[i].ch  = map_obj["ch"]  | switchConfigs[i].ch;
      switchConfigs[i].altVal = map_obj["altVal"] | 0;
      i++;
    }
  }

  JsonArray encoders = doc["encoders"];
  if (!encoders.isNull()) {
    int i = 0;
    for (JsonObject enc_obj : encoders) {
      if (i >= 5) break;
      const char* modeStr = enc_obj["mode"] | "single";
      encoderSettings[i].mode = (strcmp(modeStr, "range") == 0) ? ENCODER_MODE_RANGE : ENCODER_MODE_SINGLE;
      
      JsonObject left_obj = enc_obj["left"];
      if (!left_obj.isNull()) {
        encoderSettings[i].left.cc = left_obj["cc"] | 20;
        encoderSettings[i].left.val = left_obj["val"] | 127;
        encoderSettings[i].left.ch = left_obj["ch"] | 1;
      }
      JsonObject right_obj = enc_obj["right"];
      if (!right_obj.isNull()) {
        encoderSettings[i].right.cc = right_obj["cc"] | 21;
        encoderSettings[i].right.val = right_obj["val"] | 127;
        encoderSettings[i].right.ch = right_obj["ch"] | 1;
      }
      encoderSettings[i].rangeMin = enc_obj["rangeMin"] | 0;
      encoderSettings[i].rangeMax = enc_obj["rangeMax"] | 127;
      encoderSettings[i].singleModeSteps = enc_obj["steps"] | 1;
      encoderSettings[i].acceleration = enc_obj["accel"] | 250;
      i++;
    }
  }
  JsonObject wifi_obj = doc["wifi"];
  if (!wifi_obj.isNull()) {
    wifiEnabled = wifi_obj["enabled"] | false;
    strlcpy(wifiSsid, wifi_obj["ssid"] | "", sizeof(wifiSsid));
    strlcpy(wifiPass, wifi_obj["pass"] | "", sizeof(wifiPass));
  }
}


void saveSettings(){
  JsonDocument doc;
  JsonArray mappings = doc["mappings"].to<JsonArray>();
  for (int i=0;i<5;i++){
    JsonObject map_obj = mappings.add<JsonObject>();
    map_obj["mode"] = (switchConfigs[i].mode == SWITCH_MODE_TOGGLE) ? "toggle" : "momentary";
    map_obj["cc"]  = switchConfigs[i].cc;
    map_obj["val"] = switchConfigs[i].val;
    map_obj["ch"]  = switchConfigs[i].ch;
    map_obj["altVal"] = switchConfigs[i].altVal;
  }
  JsonArray encoders = doc["encoders"].to<JsonArray>();
  for (int i=0; i<5; i++) {
    JsonObject enc_obj = encoders.add<JsonObject>();
    enc_obj["mode"] = (encoderSettings[i].mode == ENCODER_MODE_SINGLE) ? "single" : "range";
    
    JsonObject left_obj = enc_obj["left"].to<JsonObject>();
    left_obj["cc"] = encoderSettings[i].left.cc;
    left_obj["val"] = encoderSettings[i].left.val;
    left_obj["ch"] = encoderSettings[i].left.ch;

    JsonObject right_obj = enc_obj["right"].to<JsonObject>();
    right_obj["cc"] = encoderSettings[i].right.cc;
    right_obj["val"] = encoderSettings[i].right.val;
    right_obj["ch"] = encoderSettings[i].right.ch;

    enc_obj["rangeMin"] = encoderSettings[i].rangeMin;
    enc_obj["rangeMax"] = encoderSettings[i].rangeMax;
    enc_obj["steps"] = encoderSettings[i].singleModeSteps;
    enc_obj["accel"] = encoderSettings[i].acceleration;
  }
  JsonObject wifi_obj = doc["wifi"].to<JsonObject>();
  wifi_obj["ssid"] = wifiSsid;
  wifi_obj["pass"] = wifiPass;
  wifi_obj["enabled"] = wifiEnabled;

  File f = SPIFFS.open(FILE_PATH, FILE_WRITE);
  if (!f) {
    Serial.println("Failed to open settings file for writing");
    return;
  }
  if (serializeJson(doc, f) == 0) {
    Serial.println("Failed to write to settings file");
  }
  f.close();

  display.fillRect(10, 22, SCREEN_WIDTH-20, 20, BLACK);
  display.drawRect(10, 22, SCREEN_WIDTH-20, 20, WHITE);
  display.setTextColor(WHITE);
  display.setCursor(40, 30);
  display.print("Saved!");
  display.display();
  delay(1000);
}

/* ============================ MIDI =========================== */
void sendCC(byte cc, byte val, byte ch) {
    // Check USB connection first as requested
    if (usbMidiConnected) {
      
      // Send CC 10, Value 64 on Channel 1
      usbHost.sendMIDI_CC(cc, val, ch);
    } 
    else if (btConnected) {
      // Fallback to BLE if USB is not connected
      //MIDI.sendControlChange(cc, val, ch);
      midi_ble.sendControlChange(cs::MIDIAddress(cc, cs::Channel(ch)), val);
    }
}

void sendCCForIndex(int idx){
  SwitchConfig &cfg = switchConfigs[idx];
  if (cfg.mode == SWITCH_MODE_TOGGLE) {
    cfg.state = !cfg.state;
    int valueToSend = cfg.state ? cfg.val : cfg.altVal;
    sendCC((byte)cfg.cc, (byte)valueToSend, (byte)cfg.ch);
  } else { // Momentary
    sendCC((byte)cfg.cc, (byte)cfg.val, (byte)cfg.ch);
  }
}

void midiTask(void*){
  for(;;){
    // ESP32_Host_MIDI uses the ESP-IDF USB Host background task, 
    // so explicit polling is not required here.
    //MIDI.read();     
    cs::MIDI_Interface::updateAll(); 
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}