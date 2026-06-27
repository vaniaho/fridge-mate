#pragma once

#include <string>
#include <vector>
#include <stdint.h>
#include "esp_event.h"

namespace smart_fridge {
namespace system {

struct WifiNetwork {
    std::string ssid;
    int rssi;
    int security_type; // e.g. 0: open, 1: wpa/wpa2
};

enum class WifiStatus {
    DISCONNECTED,
    CONNECTING,
    CONNECTED
};

class SystemManager {
public:
    using NetworkStateCallback = void (*)(bool connected);

    static void init();

    // WiFi
    static void scan_wifi_async();
    static std::vector<WifiNetwork> get_scanned_networks();
    static void connect_wifi(const std::string& ssid, const std::string& password);
    static void disconnect_wifi();
    static WifiStatus get_wifi_status();
    static std::string get_wifi_ip();
    static std::string get_current_ssid();
    static void set_network_state_callback(NetworkStateCallback callback);

    // Display
    static void set_brightness(int percent);
    static int get_brightness();

    // Sound
    static void set_volume(int percent);
    static int get_volume();

    // System Info
    static std::string get_firmware_version();
    static uint32_t get_free_heap();
    static uint32_t get_total_heap();

private:
    static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
    static void notify_network_state(bool connected);
    static void load_settings();
    static void save_settings();
    static int32_t current_brightness;
    static int32_t current_volume;
    static WifiStatus wifi_status;
    static std::string current_ssid;
    static std::string current_ip;
    static std::vector<WifiNetwork> scanned_networks;
    static bool is_connecting_active;
    static int auto_reconnect_count;
    static NetworkStateCallback network_state_callback;
};

} // namespace system
} // namespace smart_fridge
