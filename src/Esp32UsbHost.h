#ifndef __Esp32UsbHost_H__
#define __Esp32UsbHost_H__

#include <Arduino.h>
#include <usb/usb_host.h>

class Esp32UsbHost {
private:
    bool _isConnected = false;
    bool _interfaceClaimed = false;
    uint8_t _interfaceNumber = 0;
    uint8_t _outEndpoint = 0;
    uint8_t _inEndpoint = 0;

    // Internal function pointers to track user-registered callback routines
    void (*_onConnectedCallback)() = NULL;
    void (*_onDisconnectedCallback)() = NULL;

public:
    usb_host_client_handle_t clientHandle;
    usb_device_handle_t deviceHandle;
    uint32_t eventFlags;
    usb_transfer_t *usbTransfer = NULL;

    virtual ~Esp32UsbHost() {}

    // ========================================================================
    // Lifecycle & Operational Flow Management
    // ========================================================================
    void begin(void);
    void task(void);
    void testUsbHost(void);
    
    // ========================================================================
    // External Callback Registrars
    // ========================================================================
    void onDeviceConnected(void (*callback)()) { _onConnectedCallback = callback; }
    void onDeviceDisconnected(void (*callback)()) { _onDisconnectedCallback = callback; }

    // ========================================================================
    // Outbound MIDI Execution Engines
    // ========================================================================
    void sendMIDI_CC(uint8_t controlNumber, uint8_t value, uint8_t channel);
    
    // ========================================================================
    // Core State Evaluation Utilities
    // ========================================================================
    bool isConnected(void) { return _isConnected; }

    // ========================================================================
    // Low-Level Espressif Hardware System Bridges
    // ========================================================================
    static void _clientEventCallback(const usb_host_client_event_msg_t *eventMsg, void *arg);
    void _configCallback(const usb_config_desc_t *config_desc);
    virtual void onConfig(const uint8_t bDescriptorType, const uint8_t *p);
    
    // ========================================================================
    // USB Hardware Bus Interface Allocators
    // ========================================================================
    bool claimInterface(uint8_t interfaceNumber);
    bool releaseInterface(uint8_t interfaceNumber);

private:
    uint8_t _alternateSetting = 0; // Tracks whether endpoints live in Alt 0 or Alt 1
    // ========================================================================
    // Method 2: Hard-Coded Allocation Helpers
    // ========================================================================
    bool findMidiEndpoints(const usb_config_desc_t *config_desc);
    bool setupEndpoints();
};

#endif // __Esp32UsbHost_H__