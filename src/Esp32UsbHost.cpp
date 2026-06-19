#include "Esp32UsbHost.h"

// ============================================================================
// SYSTEM LIFECYCLE & CORE CONFIGURATION
// ============================================================================

void Esp32UsbHost::begin(void) {
    Serial.println("[USB HOST] Initializing low-level Espressif subsystem...");
    
    usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    esp_err_t err = usb_host_install(&host_config);
    if (err != ESP_OK) {
        Serial.printf("[USB HOST] ERROR: System installation failed (0x%X)\n", err);
        return;
    }

    // FIXED: Switched to explicit structural assignment to eliminate anonymous union bugs
    usb_host_client_config_t client_config;
    client_config.is_synchronous = false;
    client_config.max_num_event_msg = 5;
    client_config.async.client_event_callback = Esp32UsbHost::_clientEventCallback;
    client_config.async.callback_arg = this;

    usb_host_client_register(&client_config, &clientHandle);
}

void Esp32UsbHost::task(void) {
    // 1. Handle global USB library events (such as port daemon events)
    uint32_t lib_event_flags = 0;
    usb_host_lib_handle_events(0, &lib_event_flags);
    
    // 2. FIXED: Process client events whenever the handle exists.
    // This allows the "New Device" registration events to be drained and processed!
    if (clientHandle != NULL) {
        usb_host_client_handle_events(clientHandle, 0);
    }
}

void Esp32UsbHost::testUsbHost(void) {
    Serial.println("[DIAGNOSTIC] Register layers checking out nominal.");
}

// ============================================================================
// METHOD 2: FORCE MIDI INTERFACE OVERRIDE
// ============================================================================

void Esp32UsbHost::_configCallback(const usb_config_desc_t *config_desc) {
    Serial.println("[PARSER] Configuration packet intercept triggered.");
    
    if (findMidiEndpoints(config_desc)) {
        if (setupEndpoints()) {
            _isConnected = true;
            if (_onConnectedCallback != NULL) {
                _onConnectedCallback();
            }
        }
    }
}

bool Esp32UsbHost::findMidiEndpoints(const usb_config_desc_t *config_desc) {
    Serial.println("\n==================================================");
    Serial.println("[METHOD 2.7] Scanning for Class-Compliant MIDI Streaming Interface...");

    _interfaceNumber = 0xFF; // Start invalid to ensure we find it
    _alternateSetting = 0; 

    int offset = 0;
    uint8_t active_interface = 0xFF;
    uint8_t current_alt_setting = 0;
    bool isMidiInterface = false;
    bool foundIn = false;
    bool foundOut = false;

    // Cast away const to allow mid-flight adjustment of oversized MaxPacketSizes
    usb_config_desc_t *modifiable_config = const_cast<usb_config_desc_t *>(config_desc);

    while (offset < modifiable_config->wTotalLength) {
        usb_standard_desc_t *desc = (usb_standard_desc_t *)((uint8_t *)modifiable_config + offset);
        
        if (desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_INTERFACE) {
            const usb_intf_desc_t *intf = (const usb_intf_desc_t *)desc;
            active_interface = intf->bInterfaceNumber;
            current_alt_setting = intf->bAlternateSetting;
            
            // Check for Audio Class (0x01) + MIDI Streaming Subclass (0x03)
            if (intf->bInterfaceClass == 0x01 && intf->bInterfaceSubClass == 0x03) {
                _interfaceNumber = active_interface;
                _alternateSetting = current_alt_setting;
                isMidiInterface = true;
                Serial.printf("[MATCH] Found True MIDI Interface at Index: %d, Alt Setting: %d\n", 
                              _interfaceNumber, _alternateSetting);
            } else {
                isMidiInterface = false; // Different interface type (e.g. Audio Control or Audio Stream)
            }
        }
        
        // Only capture endpoints if they belong inside our matched MIDI Streaming interface boundaries
        if (isMidiInterface && desc->bDescriptorType == USB_B_DESCRIPTOR_TYPE_ENDPOINT) {
            usb_ep_desc_t *ep = (usb_ep_desc_t *)desc;
            
            if (ep->wMaxPacketSize > 128) {
                Serial.printf("[MPS OVERRIDE] Clamping Endpoint 0x%02X packet size from %d down to 64\n", 
                              ep->bEndpointAddress, ep->wMaxPacketSize);
                ep->wMaxPacketSize = 64; 
            }
            
            if (ep->bEndpointAddress & USB_B_ENDPOINT_ADDRESS_EP_DIR_MASK) {
                _inEndpoint = ep->bEndpointAddress;
                foundIn = true;
                Serial.printf("[MIDI] Found IN Endpoint: 0x%02X\n", _inEndpoint);
            } else {
                _outEndpoint = ep->bEndpointAddress;
                foundOut = true;
                Serial.printf("[MIDI] Found OUT Endpoint: 0x%02X\n", _outEndpoint);
            }
        }
        offset += desc->bLength;
    }

    if (_interfaceNumber == 0xFF) {
        Serial.println("[ERROR] No explicit MIDI Streaming interface signature detected!");
        return false;
    }

    Serial.printf("[SUCCESS] Final Mapping -> Interface: %d | Alt: %d | IN: 0x%02X | OUT: 0x%02X\n", 
                  _interfaceNumber, _alternateSetting, _inEndpoint, _outEndpoint);
    Serial.println("==================================================");
    
    return true; 
}

// ============================================================================
// ASYNCHRONOUS TRANSFER COMPLETION CALLBACK
// ============================================================================

static void usb_midi_tx_callback(usb_transfer_t *transfer) {
    // This executes automatically on Core 1 when the physical USB transmission completes
    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        // Safe diagnostic logging if a specific packet fails mid-flight
        ets_printf("[USB TX] Warning: Transfer finished with status status %d\n", transfer->status);
    }
}

// ============================================================================
// UPDATED SETUP ENDPOINTS
// ============================================================================

bool Esp32UsbHost::setupEndpoints() {
    if (claimInterface(_interfaceNumber)) {
        // Allocate a 4-byte buffer for standard class-compliant USB MIDI Event Packets
        esp_err_t err = usb_host_transfer_alloc(4, 0, &usbTransfer);
        
        if (err == ESP_OK && usbTransfer != NULL) {
            // FIXED: Register the required completion callback and pass the context pointer
            usbTransfer->callback = usb_midi_tx_callback;
            usbTransfer->context = this;
            
            Serial.println("[SETUP] Hardware endpoint pipes and async callback registered successfully.");
            return true;
        }
        Serial.printf("[SETUP] Transfer allocation failed (0x%X)\n", err);
    }
    return false;
}

bool Esp32UsbHost::claimInterface(uint8_t interfaceNumber) {
    // FIXED: Switched from hardcoded '0' to the dynamically discovered '_alternateSetting'
    esp_err_t err = usb_host_interface_claim(clientHandle, deviceHandle, interfaceNumber, _alternateSetting);
    if (err == ESP_OK) {
        _interfaceClaimed = true;
        return true;
    }
    Serial.printf("[INTERFACE] Error claiming interface %d, alt %d: 0x%X\n", interfaceNumber, _alternateSetting, err);
    return false;
}

bool Esp32UsbHost::releaseInterface(uint8_t interfaceNumber) {
    if (_interfaceClaimed) {
        usb_host_interface_release(clientHandle, deviceHandle, interfaceNumber);
        _interfaceClaimed = false;
        return true;
    }
    return false;
}

void Esp32UsbHost::_clientEventCallback(const usb_host_client_event_msg_t *eventMsg, void *arg) {
    Esp32UsbHost *instance = (Esp32UsbHost *)arg;
    
    if (eventMsg->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        Serial.printf("[CLIENT] New device detected at address %d\n", eventMsg->new_dev.address);
        
        // FIXED: Open the device using its actual assigned address, not 0
        esp_err_t err = usb_host_device_open(instance->clientHandle, eventMsg->new_dev.address, &instance->deviceHandle);
        if (err != ESP_OK) {
            Serial.printf("[CLIENT] Failed to open device handle (0x%X)\n", err);
            return;
        }
        
        // Fetch descriptors and execute our Method 2 bypass intercept
        const usb_config_desc_t *config_desc;
        err = usb_host_get_active_config_descriptor(instance->deviceHandle, &config_desc);
        if (err == ESP_OK) {
            instance->_configCallback(config_desc);
        } else {
            Serial.printf("[CLIENT] Failed to get config descriptor (0x%X)\n", err);
        }
        
    } else if (eventMsg->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        Serial.println("[CLIENT] Device removed.");
        instance->_isConnected = false;
        instance->releaseInterface(instance->_interfaceNumber);
        
        // Clean up device handle allocation safely
        if (instance->deviceHandle != NULL) {
            usb_host_device_close(instance->clientHandle, instance->deviceHandle);
            instance->deviceHandle = NULL;
        }
        
        if (instance->_onDisconnectedCallback != NULL) {
            instance->_onDisconnectedCallback();
        }
    }
}

void Esp32UsbHost::onConfig(const uint8_t bDescriptorType, const uint8_t *p) {
    // Virtual hook template base
}

// ============================================================================
// OUTBOUND MIDI EXECUTION ENGINES
// ============================================================================

void Esp32UsbHost::sendMIDI_CC(uint8_t controlNumber, uint8_t value, uint8_t channel) {
    if (!_isConnected || usbTransfer == NULL) return;

    // Standard 4-byte USB MIDI Event Packet Frame Format
    usbTransfer->data_buffer[0] = 0x0B; // Cable 0, CIN 0x0B (Control Change)
    usbTransfer->data_buffer[1] = 0xB0 | ((channel - 1) & 0x0F); 
    usbTransfer->data_buffer[2] = controlNumber & 0x7F;          
    usbTransfer->data_buffer[3] = value & 0x7F;                  
    
    // Configure transfer parameters
    usbTransfer->num_bytes = 4;
    usbTransfer->device_handle = deviceHandle;
    usbTransfer->bEndpointAddress = _outEndpoint;

    // Submit the transfer frame to the hardware queue
    esp_err_t err = usb_host_transfer_submit(usbTransfer);
    if (err != ESP_OK) {
        Serial.printf("[MIDI TX] Hardware submission failed error: 0x%X\n", err);
    }
}