KRELFSW: BLE / USB-Host MIDI Controller

A MIDI foot switch controller designed explicitly to interface with the Sonicake PocketMaster modeling device and other MIDI-compatible hardware/software. Built on the ESP32-S3 platform using a hybrid ESP-IDF/Arduino framework architecture, this controller combines wireless BLE-MIDI capability with a hardware USB-Host processing pipeline for simultaneous low-latency sound design and preset manipulation.


🚀 Key Features
•	Dual Transport Engine: Simultaneous operation of Native BLE-MIDI (via an optimized Control Surface namespace implementation) and physical USB Host MIDI parsing to control the Sonicake PocketMaster.
•	Comprehensive Tactile Interface: Equipped with 4 customizable hardware switches, 5 high-resolution rotary encoders (including step acceleration), and a multi-function long-press navigation menu stack.
•	Polished UI Navigation: Features a clean, scannable SH1106 / SSD1306 OLED (128x64) interface with real-time status banners (battery metrics, connection states, active USB parsing signals).
•	Storage & Memory Automation: Saves configuration structures directly to local non-volatile storage using LittleFS / JSON mappings to instantly reload custom CC toggle states and encoder tracking ranges upon boot.
•	Wireless Infrastructure: Features standalone WiFi profile scanning, an embedded alphanumeric keyboard engine for credential entry, and OTA (Over-The-Air) firmware updates.
•	Power Optimization: Integrated multi-sample analog smoothing framework for precise real-time battery voltage monitoring and scaling.


🛠️ Hardware Component Map
•	Microcontroller: ESP32-S3
•	Display: SH1106 / SSD1306 $128 \times 64$ OLED Screen via I2C (SDA: GPIO 8, SCL: GPIO 9)
•	Primary Navigation Encoder: Pins DT: 41, CLK: 40, SW: 39
•	Auxiliary Tone Encoders: 4 discrete encoder lines on GPIOs 4, 5, 6, 7, 15, 16, 17, 18
•	Hardware Switches: 4 momentary foot switches mapped to GPIOs 14, 13, 12, 11
•	Power Monitor: Analog Battery Voltage ADC line on GPIO 2


📦 Project Dependencies (platformio.ini)
This project implements an optimized compilation pattern that pulls in specific sub-modules from Control Surface to completely bypass missing USB type definitions when working under custom ESP-IDF environments.


[env:esp32s3]
platform = espressif32
board = esp32-s3-devkitc-1
framework = arduino, espidf

lib_deps =
    adafruit/Adafruit SSD1306@^2.5.7
    adafruit/Adafruit GFX Library@^1.11.5
    bblanchon/ArduinoJson@^7.0.4
    igorantolic/Ai Esp32 Rotary Encoder@^1.7
    tttapa/Control Surface@^2.1.0
	
	
📂 Configuration Architecture
Preset parameters are automatically structured and saved as a JSON object inside the ESP32-S3's local filesystem (/settings.json):
JSON
{
  "mappings": [
    { "mode": "toggle", "cc": 10, "val": 127, "ch": 1, "altVal": 0 }
  ],
  "encoders": [
    {
      "mode": "range",
      "left": { "cc": 20, "val": 127, "ch": 1 },
      "right": { "cc": 20, "val": 127, "ch": 1 },
      "rangeMin": 0,
      "rangeMax": 127,
      "steps": 1,
      "accel": 250
    }
  ],
  "wifi": {
    "ssid": "Your_Network_Name",
    "pass": "Your_Secure_Password",
    "enabled": false
  }
}

🏗️ Architecture Blueprint
       +--------------------------------------------+
       |                  ESP32-S3                  |
       |  Core 0: UI / LittleFS / Encoders / BLE    |
       |  Core 1: Dedicated USB Host Worker Thread  |
       +--------------------+-----------------------+
                            |
         +------------------+------------------+
         |                                     |
         v                                     v
+------------------+                 +--------------------+
|  Control Surface |                 |   Esp32UsbHost     |
|   (BLE-MIDI)     |                 |  (Physical USB)    |
+--------+---------+                 +---------+----------+
         |                                     |
         v                                     v
   Wireless Host                     Sonicake PocketMaster
   
   
📝 Third-Party Credits & Acknowledgments
This controller relies heavily on the following open-source frameworks:
•	adafruit/Adafruit SSD1306 & Adafruit GFX – Drives the high-performance pixel drawing and low-level block structures for our customized UI menus.
•	bblanchon/ArduinoJson – Handles dynamic memory buffer allocation and configuration parsing.
•	igorantolic/Ai Esp32 Rotary Encoder – Manages high-speed nested hardware interrupt routines (ISR) for clean tracking across all 5 encoder inputs simultaneously.
•	tttapa/Control Surface – Provides the low-latency BLE-MIDI structural engine. (Note: This project targets a specific version layer to maintain deep stability across customized hybrid compiler environments).

⚖️ License
This project is open-source software licensed under the MIT License. You are free to modify, distribute, and utilize the codebase in commercial and private applications provided the original copyright and permission notice are included.

