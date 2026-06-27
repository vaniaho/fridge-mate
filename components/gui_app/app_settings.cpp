#include "lvgl.h"
#include "gui_app.h"
#include "gui_styles.h"
#include "gui_theme.h"
#include "gui_components.h"
#include "gui_icons.h"
#include "system_manager.hpp"
#include "credentials_manager.h"
#include "rtc_time.h"
#include "audio_api.h"
#include "system_events.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

using namespace smart_fridge::system;

static lv_obj_t * content_area;
static lv_timer_t * wifi_timer = NULL;
static lv_obj_t * wifi_list = NULL;
static lv_obj_t * pwd_ta = NULL;
static lv_obj_t * pwd_card = NULL;
static std::string selected_ssid = "";
static lv_obj_t * kb = NULL;
static lv_obj_t * wifi_status_lbl = NULL;
static lv_timer_t * time_timer = NULL;
static lv_obj_t * time_status_lbl = NULL;
static lv_obj_t * time_value_lbl = NULL;
static lv_obj_t * time_input_ta = NULL;
static volatile bool time_sync_in_progress = false;

static const char *TAG = "AppSettings";

static void cleanup_active_tab() {
    if (kb) {
        lv_obj_del(kb);
        kb = NULL;
    }
    if (wifi_timer) {
        lv_timer_del(wifi_timer);
        wifi_timer = NULL;
        wifi_status_lbl = NULL;
    }
    if (time_timer) {
        lv_timer_del(time_timer);
        time_timer = NULL;
        time_status_lbl = NULL;
        time_value_lbl = NULL;
        time_input_ta = NULL;
    }
}

static void back_btn_event_cb(lv_event_t * e) {
    cleanup_active_tab();
    gui_app_navigate_to(GUI_APP_LAUNCHER);
}

// -----------------------------------------------------------------------------
// Display Tab
// -----------------------------------------------------------------------------
static void brightness_slider_cb(lv_event_t * e) {
    lv_obj_t * slider = lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    SystemManager::set_brightness(v);
}

static void render_display_tab() {
    lv_obj_clean(content_area);

    lv_obj_t * title = lv_label_create(content_area);
    lv_label_set_text(title, "显示设置");
    lv_obj_add_style(title, &style_text_title, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 40, 20);

    lv_obj_t * card = lv_obj_create(content_area);
    lv_obj_set_size(card, LV_PCT(90), 120);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 40, 80);
    lv_obj_add_style(card, &style_card, 0);

    lv_obj_t * label = lv_label_create(card);
    lv_label_set_text(label, "屏幕亮度");
    lv_obj_add_style(label, &style_text_main, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t * slider = lv_slider_create(card);
    lv_obj_set_size(slider, 300, 20);
    lv_obj_align(slider, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_slider_set_range(slider, 5, 100);
    lv_slider_set_value(slider, SystemManager::get_brightness(), LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, brightness_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

// -----------------------------------------------------------------------------
// Sound Tab
// -----------------------------------------------------------------------------
static void volume_slider_cb(lv_event_t * e) {
    lv_obj_t * slider = lv_event_get_target(e);
    int v = lv_slider_get_value(slider);
    SystemManager::set_volume(v);
    audio_hal_set_output_volume(v);
}

static void render_sound_tab() {
    lv_obj_clean(content_area);

    lv_obj_t * title = lv_label_create(content_area);
    lv_label_set_text(title, "声音设置");
    lv_obj_add_style(title, &style_text_title, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 40, 20);

    lv_obj_t * card = lv_obj_create(content_area);
    lv_obj_set_size(card, LV_PCT(90), 120);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 40, 80);
    lv_obj_add_style(card, &style_card, 0);

    lv_obj_t * label = lv_label_create(card);
    lv_label_set_text(label, "系统音量");
    lv_obj_add_style(label, &style_text_main, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t * slider = lv_slider_create(card);
    lv_obj_set_size(slider, 300, 20);
    lv_obj_align(slider, LV_ALIGN_RIGHT_MID, -20, 0);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, SystemManager::get_volume(), LV_ANIM_OFF);
    lv_obj_add_event_cb(slider, volume_slider_cb, LV_EVENT_VALUE_CHANGED, NULL);
}

// -----------------------------------------------------------------------------
// Wi-Fi Tab
// -----------------------------------------------------------------------------
static void ta_event_cb(lv_event_t * e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t * ta = lv_event_get_target(e);
    if(code == LV_EVENT_FOCUSED) {
        if(kb == NULL) {
            kb = lv_keyboard_create(lv_scr_act());
            lv_obj_set_style_text_font(kb, font_cn_18, 0);
            lv_obj_move_foreground(kb);
        }
        lv_keyboard_set_textarea(kb, ta);
    } else if(code == LV_EVENT_DEFOCUSED) {
        if(kb) {
            lv_keyboard_set_textarea(kb, NULL);
            lv_obj_del(kb);
            kb = NULL;
        }
    }
}

static void wifi_connect_btn_cb(lv_event_t * e) {
    if (selected_ssid.empty()) return;
    const char * pwd = lv_textarea_get_text(pwd_ta);
    SystemManager::connect_wifi(selected_ssid, pwd);
    if(kb) {
        lv_obj_del(kb);
        kb = NULL;
    }
}

static void wifi_item_cb(lv_event_t * e) {
    lv_obj_t * btn = lv_event_get_target(e);
    const char * ssid = (const char*)lv_event_get_user_data(e);
    selected_ssid = std::string(ssid);
    // highlight selected
    uint32_t count = lv_obj_get_child_cnt(wifi_list);
    for(uint32_t i=0; i<count; i++) {
        lv_obj_set_style_bg_color(lv_obj_get_child(wifi_list, i), lv_color_white(), 0);
    }
    lv_obj_set_style_bg_color(btn, THEME_PRIMARY_LIGHT, 0);
    
    // Show password card when an SSID is selected
    if (pwd_card) {
        lv_obj_clear_flag(pwd_card, LV_OBJ_FLAG_HIDDEN);
    }
}

static void wifi_item_delete_cb(lv_event_t * e) {
    // 按钮被销毁时释放 strdup 分配的 SSID 字符串
    char* ssid = (char*)lv_event_get_user_data(e);
    if (ssid) {
        free(ssid);
    }
}

static void wifi_timer_handler(lv_timer_t * timer) {
    if (wifi_status_lbl) {
        WifiStatus s = SystemManager::get_wifi_status();
        if (s == WifiStatus::CONNECTED) {
            lv_label_set_text_fmt(wifi_status_lbl, "状态: 已连接 %s", SystemManager::get_current_ssid().c_str());
            lv_obj_set_style_text_color(wifi_status_lbl, THEME_SUCCESS, 0);
        } else if (s == WifiStatus::CONNECTING) {
            lv_label_set_text(wifi_status_lbl, "状态: 连接中...");
            lv_obj_set_style_text_color(wifi_status_lbl, THEME_WARNING, 0);
        } else {
            lv_label_set_text(wifi_status_lbl, "状态: 未连接");
            lv_obj_set_style_text_color(wifi_status_lbl, THEME_DANGER, 0);
        }
    }

    auto networks = SystemManager::get_scanned_networks();
    if (networks.size() > 0) {
        // 每次刷新前先清空旧列表，lv_obj_clean 会触发 LV_EVENT_DELETE 释放 SSID 内存
        lv_obj_clean(wifi_list);

        for (const auto& net : networks) {
            lv_obj_t * btn = lv_btn_create(wifi_list);
            lv_obj_set_width(btn, LV_PCT(100));
            lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
            lv_obj_set_style_text_color(btn, THEME_TEXT_MAIN, 0);
            
            // Allocate a stable string for user_data
            char* ssid_copy = strdup(net.ssid.c_str());
            lv_obj_add_event_cb(btn, wifi_item_cb, LV_EVENT_CLICKED, (void*)ssid_copy);
            lv_obj_add_event_cb(btn, wifi_item_delete_cb, LV_EVENT_DELETE, (void*)ssid_copy);

            lv_obj_t * label = lv_label_create(btn);
            lv_label_set_text(label, net.ssid.c_str());
            lv_obj_align(label, LV_ALIGN_LEFT_MID, 10, 0);
            
            lv_obj_t * rssi = lv_label_create(btn);
            lv_label_set_text_fmt(rssi, "%d dBm", net.rssi);
            lv_obj_align(rssi, LV_ALIGN_RIGHT_MID, -10, 0);
        }
    }
}

static void render_wifi_tab() {
    lv_obj_clean(content_area);

    lv_obj_t * title = lv_label_create(content_area);
    lv_label_set_text(title, "Wi-Fi 设置");
    lv_obj_add_style(title, &style_text_title, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 40, 20);

    // Status
    wifi_status_lbl = lv_label_create(content_area);
    WifiStatus s = SystemManager::get_wifi_status();
    if (s == WifiStatus::CONNECTED) {
        lv_label_set_text_fmt(wifi_status_lbl, "状态: 已连接 %s", SystemManager::get_current_ssid().c_str());
        lv_obj_set_style_text_color(wifi_status_lbl, THEME_SUCCESS, 0);
    } else if (s == WifiStatus::CONNECTING) {
        lv_label_set_text(wifi_status_lbl, "状态: 连接中...");
        lv_obj_set_style_text_color(wifi_status_lbl, THEME_WARNING, 0);
    } else {
        lv_label_set_text(wifi_status_lbl, "状态: 未连接");
        lv_obj_set_style_text_color(wifi_status_lbl, THEME_DANGER, 0);
    }
    lv_obj_add_style(wifi_status_lbl, &style_text_main, 0);
    lv_obj_align(wifi_status_lbl, LV_ALIGN_TOP_RIGHT, -40, 20);

    // Scan List
    wifi_list = lv_obj_create(content_area);
    lv_obj_set_size(wifi_list, 400, 300);
    lv_obj_align(wifi_list, LV_ALIGN_TOP_LEFT, 40, 80);
    lv_obj_set_flex_flow(wifi_list, LV_FLEX_FLOW_COLUMN);
    
    lv_obj_t * list_title = lv_label_create(wifi_list);
    lv_label_set_text(list_title, "扫描结果...");

    // Password area
    pwd_card = lv_obj_create(content_area);
    lv_obj_set_size(pwd_card, 300, 300);
    lv_obj_align(pwd_card, LV_ALIGN_TOP_RIGHT, -40, 80);
    lv_obj_add_flag(pwd_card, LV_OBJ_FLAG_HIDDEN);
    
    lv_obj_t * pwd_lbl = lv_label_create(pwd_card);
    lv_label_set_text(pwd_lbl, "输入密码:");
    lv_obj_align(pwd_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    pwd_ta = lv_textarea_create(pwd_card);
    lv_textarea_set_password_mode(pwd_ta, true);
    lv_textarea_set_one_line(pwd_ta, true);
    lv_obj_set_scrollbar_mode(pwd_ta, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(pwd_ta, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(pwd_ta, 260, 40);
    lv_obj_align(pwd_ta, LV_ALIGN_TOP_LEFT, 0, 40);
    lv_obj_add_event_cb(pwd_ta, ta_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t * conn_btn = lv_btn_create(pwd_card);
    lv_obj_set_size(conn_btn, 260, 40);
    lv_obj_align(conn_btn, LV_ALIGN_TOP_LEFT, 0, 100);
    lv_obj_set_style_bg_color(conn_btn, THEME_PRIMARY, 0);
    lv_obj_add_event_cb(conn_btn, wifi_connect_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * conn_btn_lbl = lv_label_create(conn_btn);
    lv_label_set_text(conn_btn_lbl, "连接");
    lv_obj_set_style_text_color(conn_btn_lbl, lv_color_white(), 0);
    lv_obj_align(conn_btn_lbl, LV_ALIGN_CENTER, 0, 0);

    SystemManager::scan_wifi_async();

    if (wifi_timer) lv_timer_del(wifi_timer);
    wifi_timer = lv_timer_create(wifi_timer_handler, 1000, NULL);
}

// -----------------------------------------------------------------------------
// Time Tab
// -----------------------------------------------------------------------------
static void update_time_labels() {
    if (!time_status_lbl || !time_value_lbl) {
        return;
    }

    char buf[32];
    rtc_time_get_formatted(buf, sizeof(buf));
    lv_label_set_text_fmt(time_value_lbl, "当前时间: %s", buf);

    if (rtc_time_is_synced()) {
        lv_label_set_text(time_status_lbl, "状态: 已同步");
        lv_obj_set_style_text_color(time_status_lbl, THEME_SUCCESS, 0);
    } else {
        lv_label_set_text(time_status_lbl, "状态: 未同步");
        lv_obj_set_style_text_color(time_status_lbl, THEME_WARNING, 0);
    }
}

static void time_timer_handler(lv_timer_t * timer) {
    (void)timer;
    update_time_labels();
}

typedef struct {
    esp_err_t err;
} time_sync_result_t;

static void time_sync_done_async(void *arg) {
    time_sync_result_t *result = (time_sync_result_t*)arg;
    time_sync_in_progress = false;
    update_time_labels();

    if (result && result->err != ESP_OK && time_status_lbl) {
        lv_label_set_text_fmt(time_status_lbl, "状态: 同步失败 (%s)",
                              esp_err_to_name(result->err));
        lv_obj_set_style_text_color(time_status_lbl, THEME_DANGER, 0);
    }

    free(result);
}

static void time_sync_task(void *arg) {
    (void)arg;
    esp_err_t err = rtc_time_sync_now(30000);
    time_sync_result_t *result =
        (time_sync_result_t*)malloc(sizeof(time_sync_result_t));
    if (result) {
        result->err = err;
        lv_async_call(time_sync_done_async, result);
    } else {
        ESP_LOGE(TAG, "Failed to allocate time sync result");
        time_sync_in_progress = false;
    }
    vTaskDelete(NULL);
}

static void time_sync_btn_cb(lv_event_t * e) {
    (void)e;
    if (time_sync_in_progress) {
        return;
    }

    time_sync_in_progress = true;
    if (time_status_lbl) {
        lv_label_set_text(time_status_lbl, "状态: 同步中...");
        lv_obj_set_style_text_color(time_status_lbl, THEME_WARNING, 0);
    }

    if (xTaskCreate(time_sync_task, "time_sync_ui", 6144, NULL, 4, NULL) !=
        pdPASS) {
        time_sync_in_progress = false;
        if (time_status_lbl) {
            lv_label_set_text(time_status_lbl, "状态: 无法启动同步任务");
            lv_obj_set_style_text_color(time_status_lbl, THEME_DANGER, 0);
        }
    }
}

static void time_set_btn_cb(lv_event_t * e) {
    (void)e;
    if (!time_input_ta) {
        return;
    }

    const char *text = lv_textarea_get_text(time_input_ta);
    esp_err_t err = rtc_time_set_datetime(text);
    update_time_labels();

    if (time_status_lbl) {
        if (err == ESP_OK) {
            lv_label_set_text(time_status_lbl, "状态: 已手动设置");
            lv_obj_set_style_text_color(time_status_lbl, THEME_SUCCESS, 0);
        } else {
            lv_label_set_text(time_status_lbl,
                              "状态: 时间格式无效，请用 YYYY-MM-DD HH:MM:SS");
            lv_obj_set_style_text_color(time_status_lbl, THEME_DANGER, 0);
        }
    }

    if (kb) {
        lv_obj_del(kb);
        kb = NULL;
    }
}

static void render_time_tab() {
    lv_obj_clean(content_area);

    lv_obj_t * title = lv_label_create(content_area);
    lv_label_set_text(title, "时间日期设置");
    lv_obj_add_style(title, &style_text_title, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 40, 20);

    lv_obj_t * card = lv_obj_create(content_area);
    lv_obj_set_size(card, LV_PCT(90), 300);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 40, 80);
    lv_obj_add_style(card, &style_card, 0);

    time_status_lbl = lv_label_create(card);
    lv_obj_add_style(time_status_lbl, &style_text_main, 0);
    lv_obj_align(time_status_lbl, LV_ALIGN_TOP_LEFT, 20, 20);

    time_value_lbl = lv_label_create(card);
    lv_obj_add_style(time_value_lbl, &style_text_main, 0);
    lv_obj_align(time_value_lbl, LV_ALIGN_TOP_LEFT, 20, 60);

    lv_obj_t * sync_btn = lv_btn_create(card);
    lv_obj_set_size(sync_btn, 220, 44);
    lv_obj_align(sync_btn, LV_ALIGN_TOP_LEFT, 20, 110);
    lv_obj_set_style_bg_color(sync_btn, THEME_PRIMARY, 0);
    lv_obj_add_event_cb(sync_btn, time_sync_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * sync_lbl = lv_label_create(sync_btn);
    lv_label_set_text(sync_lbl, "立即同步网络时间");
    lv_obj_set_style_text_color(sync_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(sync_lbl, font_cn_18, 0);
    lv_obj_align(sync_lbl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t * input_lbl = lv_label_create(card);
    lv_label_set_text(input_lbl, "手动设置时间");
    lv_obj_add_style(input_lbl, &style_text_sub, 0);
    lv_obj_align(input_lbl, LV_ALIGN_TOP_LEFT, 20, 175);

    time_input_ta = lv_textarea_create(card);
    lv_textarea_set_one_line(time_input_ta, true);
    lv_textarea_set_placeholder_text(time_input_ta, "YYYY-MM-DD HH:MM:SS");
    lv_obj_set_size(time_input_ta, 300, 44);
    lv_obj_align(time_input_ta, LV_ALIGN_TOP_LEFT, 20, 205);
    lv_obj_add_event_cb(time_input_ta, ta_event_cb, LV_EVENT_ALL, NULL);

    lv_obj_t * set_btn = lv_btn_create(card);
    lv_obj_set_size(set_btn, 120, 44);
    lv_obj_align_to(set_btn, time_input_ta, LV_ALIGN_OUT_RIGHT_MID, 16, 0);
    lv_obj_set_style_bg_color(set_btn, THEME_PRIMARY, 0);
    lv_obj_add_event_cb(set_btn, time_set_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * set_lbl = lv_label_create(set_btn);
    lv_label_set_text(set_lbl, "设置");
    lv_obj_set_style_text_color(set_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(set_lbl, font_cn_18, 0);
    lv_obj_align(set_lbl, LV_ALIGN_CENTER, 0, 0);

    update_time_labels();
    if (time_timer) lv_timer_del(time_timer);
    time_timer = lv_timer_create(time_timer_handler, 1000, NULL);
}

// -----------------------------------------------------------------------------
// Device Info Tab
// -----------------------------------------------------------------------------
static void render_device_info_tab() {
    lv_obj_clean(content_area);

    lv_obj_t * title = lv_label_create(content_area);
    lv_label_set_text(title, "设备信息");
    lv_obj_add_style(title, &style_text_title, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 40, 20);

    lv_obj_t * card = lv_obj_create(content_area);
    lv_obj_set_size(card, LV_PCT(90), 300);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 40, 80);
    lv_obj_add_style(card, &style_card, 0);

    char buf[256];
    sprintf(buf, 
        "固件版本: %s\n\n"
        "内存空闲: %lu KB / %lu KB\n\n"
        "IP 地址: %s",
        SystemManager::get_firmware_version().c_str(),
        SystemManager::get_free_heap() / 1024,
        SystemManager::get_total_heap() / 1024,
        SystemManager::get_wifi_ip().empty() ? "未分配" : SystemManager::get_wifi_ip().c_str()
    );

    lv_obj_t * info = lv_label_create(card);
    lv_label_set_text(info, buf);
    lv_obj_add_style(info, &style_text_main, 0);
    lv_obj_align(info, LV_ALIGN_TOP_LEFT, 20, 20);
}

// -----------------------------------------------------------------------------
// Voice Model Tab
// -----------------------------------------------------------------------------
static void render_voice_tab() {
    lv_obj_clean(content_area);

    lv_obj_t * title = lv_label_create(content_area);
    lv_label_set_text(title, "语音模型配置");
    lv_obj_add_style(title, &style_text_title, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 40, 20);

    lv_obj_t * card = lv_obj_create(content_area);
    lv_obj_set_size(card, LV_PCT(90), 430);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 40, 80);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 16, 0);

    const char* labels[] = {"ASR 语音识别", "LLM 大模型",
                            "TTS 语音合成", "端到端实时语音"};
    voice_model_type_t types[] = {VOICE_MODEL_ASR, VOICE_MODEL_LLM,
                                  VOICE_MODEL_TTS,
                                  VOICE_MODEL_REALTIME};

    for (int i = 0; i < 4; i++) {
        const voice_model_config_t* cfg = credentials_get_voice_model_config(types[i]);

        lv_obj_t* row = lv_obj_create(card);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(row, 4, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t* lbl = lv_label_create(row);
        char buf[384];
        snprintf(buf, sizeof(buf), "%s: %s / %s", labels[i],
                 cfg && cfg->provider[0] ? cfg->provider : "未配置",
                 cfg && cfg->model[0] ? cfg->model : "--");
        lv_label_set_text(lbl, buf);
        lv_obj_set_style_text_font(lbl, font_cn_18, 0);
        lv_obj_set_style_text_color(lbl, THEME_TEXT_MAIN, 0);

        lv_obj_t* url_lbl = lv_label_create(row);
        snprintf(buf, sizeof(buf), "URL: %s", cfg && cfg->url[0] ? cfg->url : "--");
        lv_label_set_text(url_lbl, buf);
        lv_obj_set_style_text_font(url_lbl, font_cn_16, 0);
        lv_obj_set_style_text_color(url_lbl, THEME_TEXT_SUB, 0);
    }

    lv_obj_t* btn = lv_btn_create(content_area);
    lv_obj_set_size(btn, 200, 50);
    lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_set_style_bg_color(btn, THEME_PRIMARY, 0);
    lv_obj_add_event_cb(btn, [](lv_event_t*) {
        voice_session_start(false);
    }, LV_EVENT_CLICKED, NULL);

    lv_obj_t* btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "打开语音助手");
    lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(btn_lbl, font_cn_18, 0);
    lv_obj_align(btn_lbl, LV_ALIGN_CENTER, 0, 0);
}

// -----------------------------------------------------------------------------
// About Tab
// -----------------------------------------------------------------------------
static void render_about_tab() {
    lv_obj_clean(content_area);

    lv_obj_t * title = lv_label_create(content_area);
    lv_label_set_text(title, "关于");
    lv_obj_add_style(title, &style_text_title, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 40, 20);

    lv_obj_t * card = lv_obj_create(content_area);
    lv_obj_set_size(card, LV_PCT(90), 300);
    lv_obj_align(card, LV_ALIGN_TOP_LEFT, 40, 80);
    lv_obj_add_style(card, &style_card, 0);

    lv_obj_t * info = lv_label_create(card);
    lv_label_set_text(info, "产品：小鲜智能冰箱\n架构：ESP32-P4 + LVGL\n版权所有 (C) 2026");
    lv_obj_add_style(info, &style_text_main, 0);
    lv_obj_set_style_text_align(info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(info, LV_ALIGN_CENTER, 0, 0);
}


static void sidebar_btn_event_cb(lv_event_t * e) {
    cleanup_active_tab();
    
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (id == 0) render_wifi_tab();
    else if (id == 1) render_display_tab();
    else if (id == 2) render_sound_tab();
    else if (id == 3) render_voice_tab();
    else if (id == 4) render_time_tab();
    else if (id == 5) render_device_info_tab();
    else if (id == 6) render_about_tab();
}

lv_obj_t* app_settings_create(void) {
    lv_obj_t * screen = lv_obj_create(NULL);
    lv_obj_add_style(screen, &style_screen_bg, 0);

    // Sidebar
    lv_obj_t * sidebar = lv_obj_create(screen);
    lv_obj_set_size(sidebar, 200, LV_PCT(100));
    lv_obj_align(sidebar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(sidebar, THEME_SURFACE, 0);
    lv_obj_set_style_border_width(sidebar, 0, 0);
    lv_obj_set_style_radius(sidebar, 0, 0);

    lv_obj_t * back_btn = lv_btn_create(sidebar);
    lv_obj_set_size(back_btn, LV_PCT(90), 50);
    lv_obj_align(back_btn, LV_ALIGN_TOP_MID, 0, 10);
    lv_obj_set_style_bg_color(back_btn, THEME_BG, 0);
    lv_obj_add_event_cb(back_btn, back_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t * back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "← 返回桌面");
    lv_obj_set_style_text_color(back_label, THEME_TEXT_MAIN, 0);
    lv_obj_set_style_text_font(back_label, font_cn_18, 0);
    lv_obj_align(back_label, LV_ALIGN_CENTER, 0, 0);

    const char * menus[] = {"Wi-Fi 设置", "显示设置", "声音设置", "语音模型", "设备信息", "关于"};
    for(int i=0; i<7; i++) {
        lv_obj_t * menu_btn = lv_btn_create(sidebar);
        lv_obj_set_size(menu_btn, LV_PCT(100), 50);
        lv_obj_align(menu_btn, LV_ALIGN_TOP_MID, 0, 80 + i * 60);
        lv_obj_set_style_bg_opa(menu_btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(menu_btn, 0, 0);
        lv_obj_add_event_cb(menu_btn, sidebar_btn_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t * label = lv_label_create(menu_btn);
        const char *menu_text = (i == 4) ? "时间日期" :
                                (i < 4 ? menus[i] : menus[i - 1]);
        lv_label_set_text(label, menu_text);
        lv_obj_set_style_text_color(label, THEME_TEXT_MAIN, 0);
        lv_obj_set_style_text_font(label, font_cn_18, 0);
        lv_obj_align(label, LV_ALIGN_LEFT_MID, 20, 0);
    }

    // Main Content Area
    content_area = lv_obj_create(screen);
    lv_obj_set_size(content_area, 824, LV_PCT(100)); 
    lv_obj_align(content_area, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_opa(content_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content_area, 0, 0);

    // Render default tab
    render_wifi_tab();

    return screen;
}
