#ifndef MAIN_H
#define MAIN_H

#include <Arduino.h>

// Define your existing enums and structs here
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

enum SwitchMode { SWITCH_MODE_MOMENTARY, SWITCH_MODE_TOGGLE };

struct SwitchConfig {
    SwitchMode mode = SWITCH_MODE_MOMENTARY;
    int cc = 10;
    int val = 127;
    int ch = 1;
    int altVal = 0;
    bool state = false; // for toggle mode
};

enum Screen { HOME, MENU_MAIN, MENU_SWITCH_SELECT, MENU_SWITCH, MENU_ENCODER_SELECT, MENU_ENCODER_EDIT, MENU_WIFI, MENU_WIFI_SCAN, MENU_WIFI_INFO, KEYBOARD, ABOUT, MENU_CONFIRM_RESET };

// Declare global variables as extern
extern SwitchConfig switchConfigs[5];
extern EncoderSettings encoderSettings[5];
extern char wifiSsid[33];
extern char wifiPass[65];
extern bool wifiEnabled;
extern Screen screen;

void saveSettings(); // Declare saveSettings as extern as well

#endif
