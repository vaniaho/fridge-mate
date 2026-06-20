#include "gui_bridge.h"
#include "gui_app.h"
#include "esp_log.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "lvgl.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "GuiBridge";

// -------- Internal Structs for Async Calls --------

typedef struct {
    char* title;
    char* message;
} notif_data_t;

typedef struct {
    char* text;
} string_data_t;

// -------- Async Callbacks (Execute in LVGL Thread) --------

static void async_refresh_inventory_cb(void * user_data) {
    gui_app_refresh_inventory();
}

static void async_refresh_dashboard_cb(void * user_data) {
    gui_launcher_refresh_dashboard();
}

static void async_show_notification_cb(void * user_data) {
    notif_data_t* data = (notif_data_t*)user_data;
    if (data) {
        gui_app_show_notification(data->title, data->message);
        if (data->title) free(data->title);
        if (data->message) free(data->message);
        free(data);
    }
}

static void async_listening_indicator_cb(void * user_data) {
    bool is_listening = (bool)(intptr_t)user_data;
    gui_app_show_listening_indicator(is_listening);
}

static void async_tts_text_cb(void * user_data) {
    string_data_t* data = (string_data_t*)user_data;
    if (data) {
        gui_app_show_tts_text(data->text);
        if (data->text) free(data->text);
        free(data);
    }
}

static void async_navigate_cb(void * user_data) {
    gui_app_id_t app_id = (gui_app_id_t)(intptr_t)user_data;
    gui_app_navigate_to(app_id);
}

static void async_go_back_cb(void * user_data) {
    gui_app_go_back();
}

static void async_camera_preview_cb(void * user_data) {
    gui_app_show_camera_preview();
}

static void async_voice_assist_cb(void * user_data) {
    gui_app_show_voice_assist();
}

static void async_wake_cb(void * user_data) {
    gui_app_wake_from_standby();
}

static void async_standby_cb(void * user_data) {
    gui_app_show_standby();
}

// -------- Public API --------

void gui_bridge_init(void) {
    ESP_LOGI(TAG, "GUI Bridge initialized");
}

void gui_bridge_wake(void) {
    ESP_LOGI(TAG, "Wake requested");
    // Hardware wake up
    if (bsp_display_lock(0)) {
        bsp_display_backlight_on();
        bsp_display_unlock();
    }
    // Also notify GUI app to exit standby
    lv_async_call(async_wake_cb, NULL);
}

void gui_bridge_show_standby(void) {
    ESP_LOGI(TAG, "Standby requested");
    // Notify GUI app to enter standby screen
    lv_async_call(async_standby_cb, NULL);

    // 关闭背光以降低待机功耗（gui_bridge_wake() 中会重新点亮）
    if (bsp_display_lock(0)) {
        bsp_display_backlight_off();
        bsp_display_unlock();
    }
}

void gui_bridge_refresh_inventory(void) {
    ESP_LOGI(TAG, "Inventory list refresh requested");
    lv_async_call(async_refresh_inventory_cb, NULL);
}

void gui_bridge_refresh_dashboard(void) {
    ESP_LOGI(TAG, "Dashboard refresh requested");
    lv_async_call(async_refresh_dashboard_cb, NULL);
}

void gui_bridge_show_notification(const char* title, const char* message) {
    ESP_LOGI(TAG, "Notification: [%s] %s", title ? title : "", message ? message : "");
    notif_data_t* data = (notif_data_t*)malloc(sizeof(notif_data_t));
    if (data) {
        data->title = title ? strdup(title) : NULL;
        data->message = message ? strdup(message) : NULL;
        lv_async_call(async_show_notification_cb, data);
    }
}

void gui_bridge_show_listening_indicator(bool is_listening) {
    ESP_LOGI(TAG, "Listening indicator: %s", is_listening ? "ON" : "OFF");
    lv_async_call(async_listening_indicator_cb, (void*)(intptr_t)is_listening);
}

void gui_bridge_show_tts_text(const char* text) {
    ESP_LOGI(TAG, "TTS display: %s", text ? text : "");
    string_data_t* data = (string_data_t*)malloc(sizeof(string_data_t));
    if (data) {
        data->text = text ? strdup(text) : NULL;
        lv_async_call(async_tts_text_cb, data);
    }
}

void gui_bridge_navigate_to(gui_app_id_t app_id) {
    ESP_LOGI(TAG, "Navigate to App ID: %d", (int)app_id);
    lv_async_call(async_navigate_cb, (void*)(intptr_t)app_id);
}

void gui_bridge_go_back(void) {
    ESP_LOGI(TAG, "Navigate Back");
    lv_async_call(async_go_back_cb, NULL);
}

void gui_bridge_show_camera_preview(void) {
    ESP_LOGI(TAG, "Show Camera Preview");
    lv_async_call(async_camera_preview_cb, NULL);
}

void gui_bridge_show_voice_assist(void) {
    ESP_LOGI(TAG, "Show Voice Assist Fullscreen");
    lv_async_call(async_voice_assist_cb, NULL);
}
