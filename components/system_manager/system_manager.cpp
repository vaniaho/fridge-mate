#include "system_manager.hpp"
#include <string.h>
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "bsp/display.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"
#include <stdio.h>

extern "C" {
#include "credentials_manager.h"
#include "wifi_manager.h"
}
extern "C" void gui_app_set_wifi_status(bool connected);

namespace smart_fridge {
namespace system {

static const char *TAG = "SystemManager";

int32_t SystemManager::current_brightness = 80;
int32_t SystemManager::current_volume = 50;
WifiStatus SystemManager::wifi_status = WifiStatus::DISCONNECTED;
std::string SystemManager::current_ssid = "";
std::string SystemManager::current_ip = "";
std::vector<WifiNetwork> SystemManager::scanned_networks;
bool SystemManager::is_connecting_active = false;
int SystemManager::auto_reconnect_count = 0;
SystemManager::NetworkStateCallback SystemManager::network_state_callback = nullptr;

void SystemManager::notify_network_state(bool connected) {
    if (SystemManager::network_state_callback) {
        SystemManager::network_state_callback(connected);
    }
}

void SystemManager::wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        // Wi-Fi started, can connect now
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_status = WifiStatus::CONNECTING;
        wifi_event_sta_connected_t* event = (wifi_event_sta_connected_t*)event_data;
        if (event && event->ssid[0] != '\0') {
            current_ssid = std::string((char*)event->ssid, event->ssid_len);
        }
    } else if (event_base == WIFI_EVENT &&
               (event_id == WIFI_EVENT_STA_DISCONNECTED ||
                event_id == WIFI_EVENT_STA_STOP)) {
        wifi_status = WifiStatus::DISCONNECTED;
        current_ip.clear();
        current_ssid.clear();
        gui_app_set_wifi_status(false);
        SystemManager::notify_network_state(false);

        if (is_connecting_active) {
            ESP_LOGW(TAG, "Wi-Fi connection attempt failed or interrupted. Stopping retries.");
            is_connecting_active = false;
        } else {
            if (auto_reconnect_count < 5) {
                auto_reconnect_count++;
                ESP_LOGI(TAG, "Wi-Fi disconnected. Auto-reconnecting background (%d/5)...", auto_reconnect_count);
                esp_wifi_connect();
            } else {
                ESP_LOGW(TAG, "Wi-Fi disconnected. Auto-reconnect limit reached. Stopping.");
            }
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        char ip_str[32];
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&event->ip_info.ip));
        current_ip = std::string(ip_str);
        wifi_status = WifiStatus::CONNECTED;
        is_connecting_active = false;
        auto_reconnect_count = 0;

        wifi_config_t wifi_config = {};
        if (esp_wifi_get_config(WIFI_IF_STA, &wifi_config) == ESP_OK) {
            current_ssid = std::string((char*)wifi_config.sta.ssid);
        }

        ESP_LOGI(TAG, "Got IP: %s (SSID: %s)", current_ip.c_str(), current_ssid.c_str());
        gui_app_set_wifi_status(true);
        SystemManager::notify_network_state(true);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
        wifi_status = WifiStatus::DISCONNECTED;
        current_ip.clear();
        gui_app_set_wifi_status(false);
        SystemManager::notify_network_state(false);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 0) {
            wifi_ap_record_t* ap_info = new wifi_ap_record_t[ap_count];
            ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&ap_count, ap_info));
            scanned_networks.clear();
            for (int i = 0; i < ap_count; i++) {
                WifiNetwork net;
                net.ssid = std::string((char*)ap_info[i].ssid);
                net.rssi = ap_info[i].rssi;
                net.security_type = ap_info[i].authmode;
                
                // Avoid duplicates
                bool duplicate = false;
                for (const auto& existing : scanned_networks) {
                    if (existing.ssid == net.ssid) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate && !net.ssid.empty()) {
                    scanned_networks.push_back(net);
                }
            }
            delete[] ap_info;
        }
        ESP_LOGI(TAG, "Wi-Fi scan done. Found %d networks.", scanned_networks.size());
    }
}

void SystemManager::init() {
    const bool boot_wifi_connected = wifi_manager_is_connected();

    wifi_manager_disable_retry();      // 禁用 boot 阶段的重试逻辑
    wifi_manager_unregister_handlers(); // 注销 boot 阶段的事件 handler，避免与 SystemManager 重复处理
    load_settings();

    // Ensure core network and event loop are initialized
    // (if not already done by wifi_manager in Phase 3)
    esp_netif_init();
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    // Register our own handler to listen for scan results and IP events
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));

    if (boot_wifi_connected) {
        esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
        esp_netif_ip_info_t ip_info = {};
        if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK &&
            ip_info.ip.addr != 0) {
            char ip_str[32];
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
            current_ip = std::string(ip_str);

            wifi_config_t wifi_config = {};
            if (esp_wifi_get_config(WIFI_IF_STA, &wifi_config) == ESP_OK) {
                current_ssid = std::string((char*)wifi_config.sta.ssid);
            }

            wifi_status = WifiStatus::CONNECTED;
            gui_app_set_wifi_status(true);
            SystemManager::notify_network_state(true);
        }
    } else {
        wifi_status = WifiStatus::DISCONNECTED;
        current_ip.clear();
        current_ssid.clear();
        gui_app_set_wifi_status(false);
        SystemManager::notify_network_state(false);
    }

    // The BSP already owns and initializes the backlight LEDC channel while
    // creating the display. Reinitializing it here causes a GPIO 32 conflict.
    // The saved value is applied by gui_port only after a stable boot frame.
}

void SystemManager::load_settings() {
    nvs_handle_t my_handle;
    if (nvs_open("settings", NVS_READONLY, &my_handle) == ESP_OK) {
        nvs_get_i32(my_handle, "brightness", &current_brightness);
        nvs_get_i32(my_handle, "volume", &current_volume);
        nvs_close(my_handle);
    }
}

void SystemManager::save_settings() {
    nvs_handle_t my_handle;
    if (nvs_open("settings", NVS_READWRITE, &my_handle) == ESP_OK) {
        nvs_set_i32(my_handle, "brightness", current_brightness);
        nvs_set_i32(my_handle, "volume", current_volume);
        nvs_commit(my_handle);
        nvs_close(my_handle);
    }
}

void SystemManager::scan_wifi_async() {
    wifi_scan_config_t scan_config = {};
    scan_config.show_hidden = false;
    // Non-blocking scan
    esp_wifi_scan_start(&scan_config, false); 
}

std::vector<WifiNetwork> SystemManager::get_scanned_networks() {
    return scanned_networks;
}

void SystemManager::connect_wifi(const std::string& ssid, const std::string& password) {
    // Save new WiFi configurations to NVS
    credentials_set_wifi(ssid.c_str(), password.c_str());

    wifi_config_t wifi_config = {};
    memset(&wifi_config, 0, sizeof(wifi_config));
    strncpy((char*)wifi_config.sta.ssid, ssid.c_str(), sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char*)wifi_config.sta.password, password.c_str(), sizeof(wifi_config.sta.password) - 1);

    // 根据密码 / 扫描结果动态选择认证阈值，支持 OPEN / WPA / WPA2 / WPA3
    if (password.empty()) {
        wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    } else {
        // 优先参考扫描结果中该 SSID 的实际认证模式，未命中则使用 WPA2 作为通用兼容阈值
        wifi_auth_mode_t target_auth = WIFI_AUTH_WPA2_PSK;
        for (const auto& net : scanned_networks) {
            if (net.ssid == ssid) {
                target_auth = (wifi_auth_mode_t)net.security_type;
                break;
            }
        }
        // WPA3 兼容：若 AP 使用 WPA3，仍以 WPA2 为阈值可同时兼容 WPA2/WPA3 混合模式
        if (target_auth > WIFI_AUTH_WPA2_PSK) {
            wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        } else {
            wifi_config.sta.threshold.authmode = target_auth;
        }
    }
    
    wifi_status = WifiStatus::CONNECTING;
    current_ssid = ssid;
    current_ip.clear();
    is_connecting_active = false;
    auto_reconnect_count = 0;

    ESP_LOGI(TAG, "UI requested Wi-Fi connection. Target SSID: %s", ssid.c_str());

    // Disconnect current to reset the Wi-Fi state machine
    esp_wifi_disconnect();
    
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write new wifi config: %s", esp_err_to_name(err));
        wifi_status = WifiStatus::DISCONNECTED;
        is_connecting_active = false;
        return;
    }

    is_connecting_active = true;
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start wifi connection: %s", esp_err_to_name(err));
        wifi_status = WifiStatus::DISCONNECTED;
        is_connecting_active = false;
    }
}

void SystemManager::disconnect_wifi() {
    esp_wifi_disconnect();
}

WifiStatus SystemManager::get_wifi_status() {
    return wifi_status;
}

std::string SystemManager::get_wifi_ip() {
    return current_ip;
}

std::string SystemManager::get_current_ssid() {
    return current_ssid;
}

void SystemManager::set_network_state_callback(NetworkStateCallback callback) {
    network_state_callback = callback;
    if (network_state_callback) {
        network_state_callback(wifi_status == WifiStatus::CONNECTED);
    }
}

void SystemManager::set_brightness(int percent) {
    if (percent < 1) percent = 1;
    if (percent > 100) percent = 100;
    current_brightness = percent;
    bsp_display_brightness_set(percent);
    save_settings();
}

int SystemManager::get_brightness() {
    return current_brightness;
}

void SystemManager::set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    current_volume = percent;
    // Volume control hardware implementation to be added
    save_settings();
}

int SystemManager::get_volume() {
    return current_volume;
}

std::string SystemManager::get_firmware_version() {
    return "v1.0.0";
}

uint32_t SystemManager::get_free_heap() {
    return esp_get_free_heap_size();
}

uint32_t SystemManager::get_total_heap() {
    // 返回内部 SRAM + PSRAM 的可分配堆总量
    // heap_caps_get_total_size 在 esp_heap_caps.h 中声明，IDF 各版本稳定可用
    uint32_t internal = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    uint32_t spiram   = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    return internal + spiram;
}

} // namespace system
} // namespace smart_fridge
