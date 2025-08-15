#include <iostream>
#include <vector>
#include <algorithm>
#include <cctype>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

#include "utils.hpp"
#include "simpleble/SimpleBLE.h"


// String to identify the Simionic G1000 units during Bluetooth device scanning
const std::string SIMIONIC_G1000_IDENTIFIER = "SHB1000";

// Simionic-specific characterisitic UUID for the 'Indication' characterisitic
const std::string BLE_CHARACTERISTIC_UUID = "f62a9f56-f29e-48a8-a317-47ee37a58999";

// How long to scan for BLE devices (5 seconds doesn't always capture both G1000 units)
constexpr int BLUETOOTH_SCANNING_TIMEOUT = 10;



// Helper: lower-case a UUID string for comparison.
static std::string to_lower(std::string v) {
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return v;
}


static void on_receive_bytes(SimpleBLE::ByteArray bytes) { 
    // Perform SimConnect/FSUIPC magic here

    print_hex_bytes(bytes);
}

static void print_hex_bytes(const SimpleBLE::ByteArray& data) {
    std::cout << "Indication (" << data.size() << " bytes): ";
    for (unsigned char c : data) {
        std::cout << std::hex << std::uppercase << std::setw(2) << std::setfill('0') << (int)c << " ";
    }
    std::cout << std::dec << std::nouppercase << std::endl;
}

int main() {
    
    const std::string desired_characteristic_uuid = to_lower(BLE_CHARACTERISTIC_UUID);

    auto adapter_optional = Utils::getAdapter();
    if (!adapter_optional.has_value()) {
        std::cerr << "No Bluetooth adapter found." << std::endl;
        return EXIT_FAILURE;
    }
    auto adapter = adapter_optional.value();

    std::vector<SimpleBLE::Peripheral> peripherals;

    adapter.set_callback_on_scan_found([&](SimpleBLE::Peripheral peripheral) {
        std::cout << "Found device: " << peripheral.identifier() << " [" << peripheral.address() << "]" << std::endl;
        if (peripheral.is_connectable()) {
            peripherals.push_back(peripheral);
        }
    });
    adapter.set_callback_on_scan_start([]() { std::cout << "Scan started." << std::endl; });
    adapter.set_callback_on_scan_stop([]() { std::cout << "Scan stopped." << std::endl; });

    adapter.scan_for(BLUETOOTH_SCANNING_TIMEOUT * 1000);

    if (peripherals.empty()) {
        std::cerr << "No connectable peripherals discovered." << std::endl;
        return EXIT_FAILURE;
    }

    std::cout << "The following connectable Simionic G1000 devices were found:" << std::endl;
    for (size_t i = 0; i < peripherals.size(); i++) {
        std::string peripheralIdentifier = peripherals[i].identifier();
        if (peripheralIdentifier == SIMIONIC_G1000_IDENTIFIER) {
            std::cout << "[" << i << "] " << peripheralIdentifier << " [" << peripherals[i].address() << "]"
                      << std::endl;
        }
    }

    auto selection = Utils::getUserInputInt("Please select a device to connect to", peripherals.size() - 1);
    if (!selection.has_value()) {
        std::cerr << "Invalid selection." << std::endl;
        return EXIT_FAILURE;
    }

    auto peripheral = peripherals[selection.value()];
    std::cout << "Connecting to " << peripheral.identifier() << " [" << peripheral.address() << "]" << std::endl;
    try {
        peripheral.connect();
    } catch (const std::exception& e) {
        std::cerr << "Connection failed: " << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    // Find the target characteristic & its service.
    SimpleBLE::BluetoothUUID found_service_uuid;
    SimpleBLE::BluetoothUUID found_characterisitic_uuid;
    bool found = false;

    // Find the service UUID to which the desired characteristic belongs.
    for (auto& service : peripheral.services()) {
        for (auto& characteristic : service.characteristics()) {
            if (to_lower(characteristic.uuid()) == desired_characteristic_uuid) {
                found_service_uuid = service.uuid();
                found_characterisitic_uuid = characteristic.uuid();
                found = true;
                break;
            }
        }
        if (found) break;
    }

    if (!found) {
        std::cerr << "Target characteristic UUID " << BLE_CHARACTERISTIC_UUID
                  << " not found on this device. Listing all characteristics for inspection:" << std::endl;
        for (auto& service : peripheral.services()) {
            std::cout << "Service: " << service.uuid() << std::endl;
            for (auto& characteristic : service.characteristics()) {
                std::cout << "  Char: " << characteristic.uuid() << " (";
                if (characteristic.can_notify()) std::cout << "N";
                if (characteristic.can_indicate()) std::cout << "I";
                std::cout << ")" << std::endl;
            }
        }
        peripheral.disconnect();
        return EXIT_FAILURE;
    }

    // Subscribe using indicate() if supported, else fallback to notify().
    bool used_indicate = false;
    try {
        // Quick capabilities re-check
        for (auto& service : peripheral.services()) {
            if (service.uuid() == found_service_uuid) {
                for (auto& characteristic : service.characteristics()) {
                    if (characteristic.uuid() == found_characterisitic_uuid) {
                        if (characteristic.can_indicate()) {
                            peripheral.indicate(found_service_uuid, found_characterisitic_uuid,
                                                [&](SimpleBLE::ByteArray bytes) { on_receive_bytes(bytes); });
                            used_indicate = true;
                        } else if (characteristic.can_notify()) {
                            std::cout << "Characteristic does not support indicate; using notify instead." << std::endl;
                            peripheral.notify(found_service_uuid, found_characterisitic_uuid,
                                              [&](SimpleBLE::ByteArray bytes) { on_receive_bytes(bytes); }); 
                        } else {
                            std::cerr << "Characteristic does not support indicate or notify." << std::endl;
                            peripheral.disconnect();
                            return EXIT_FAILURE;
                        }
                        break;
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Failed to subscribe: " << e.what() << std::endl;
        peripheral.disconnect();
        return EXIT_FAILURE;
    }

    std::cout << (used_indicate ? "Indication" : "Notification")
              << " subscription active. Press Enter to stop..." << std::endl;

    // Keep alive until user presses Enter.
    std::string dummy;
    std::getline(std::cin, dummy); // If a leftover newline exists, call again.
    if (dummy.empty() && std::cin.good()) {
        std::getline(std::cin, dummy);
    }

    // Unsubscribe before disconnecting.
    try {
        peripheral.unsubscribe(found_service_uuid, found_characterisitic_uuid);
    } catch (const std::exception& e) {
        std::cerr << "Unsubscribe failed (continuing): " << e.what() << std::endl;
    }

    peripheral.disconnect();
    std::cout << "Disconnected. Exiting." << std::endl;
    return EXIT_SUCCESS;
}
