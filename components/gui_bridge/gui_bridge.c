#include "gui_bridge.h"
#include "esp_log.h"

static const char *TAG = "GuiBridge";

void gui_bridge_init(void) {
    ESP_LOGI(TAG, "GUI Bridge initialized (stub mode - no GUI framework connected)");
    // TODO: Initialize connection to LVGL/ESP-Brookesia once the framework is decided
}

void gui_bridge_screen_on(void) {
    ESP_LOGI(TAG, "[Stub] Screen ON requested");
    // TODO: Call display backlight on + wake LVGL task
}

void gui_bridge_screen_off(void) {
    ESP_LOGI(TAG, "[Stub] Screen OFF requested");
    // TODO: Call display backlight off
}

void gui_bridge_refresh_inventory(void) {
    ESP_LOGI(TAG, "[Stub] Inventory list refresh requested");
    // TODO: Signal LVGL to re-render the inventory list widget
}

void gui_bridge_show_notification(const char* title, const char* message) {
    ESP_LOGI(TAG, "[Stub] Notification: [%s] %s", title ? title : "", message ? message : "");
    // TODO: Create and show an LVGL notification popup
}

void gui_bridge_show_listening_indicator(bool is_listening) {
    ESP_LOGI(TAG, "[Stub] Listening indicator: %s", is_listening ? "ON" : "OFF");
    // TODO: Show/hide the microphone animation widget
}

void gui_bridge_show_tts_text(const char* text) {
    ESP_LOGI(TAG, "[Stub] TTS display: %s", text ? text : "");
    // TODO: Update the TTS text label on screen
}

void gui_bridge_open_settings(void) {
    ESP_LOGI(TAG, "[Stub] Settings page requested");
    // TODO: Navigate to the settings screen
}
