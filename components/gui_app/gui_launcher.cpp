#include "lvgl.h"
#include "gui_app.h"
#include "gui_styles.h"
#include "gui_theme.h"
#include "gui_components.h"
#include "gui_icons.h"
#include "system_manager.hpp"
#include "notes_board.hpp"
#include "weather.hpp"
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <vector>
#include <set>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

using namespace smart_fridge::system;
extern lv_obj_t * wifi_icon_ptr;

static lv_obj_t * time_label_ptr = NULL;
static lv_obj_t * date_label_ptr = NULL;

// 桌面天气卡片 / 留言板控件指针（供后台数据刷新使用）
static lv_obj_t * weather_temp_ptr = NULL;
static lv_obj_t * weather_desc_ptr = NULL;
static lv_obj_t * weather_icon_ptr = NULL;

// 留言板容器 (msg_card 是外层白色卡片, msg_list 是可滚动的内部列表)
static lv_obj_t * msg_card = NULL;
static lv_obj_t * msg_list = NULL;

// 新留言高亮追踪：存储「尚未被用户点击确认」的 timestamp
static std::set<time_t> new_note_timestamps;

// 新留言高亮颜色: 柔和的暖橙渐变底色 (类似 Material amber-50)
#define COLOR_NOTE_NEW_BG     lv_color_hex(0xFFF3E0)
// 新留言左侧色条: 活力橙
#define COLOR_NOTE_NEW_ACCENT lv_color_hex(0xFF9800)
// 普通留言底色: 白色
#define COLOR_NOTE_NORMAL_BG  lv_color_hex(0xFFFFFF)

static void clock_timer_cb(lv_timer_t * timer) {
    if (time_label_ptr == NULL) return;
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    // Default system time if not synced is 1970, don't show invalid time
    if (timeinfo.tm_year < (2020 - 1900)) {
        lv_label_set_text(time_label_ptr, "--:--");
        if (date_label_ptr) lv_label_set_text(date_label_ptr, "");
    } else {
        char buf[16];
        strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
        lv_label_set_text(time_label_ptr, buf);
        if (date_label_ptr) {
            strftime(buf, sizeof(buf), "%m月%d日", &timeinfo);
            lv_label_set_text(date_label_ptr, buf);
        }
    }
}

static const char* weather_icon_for(const char* text) {
    if (!text) return ICON_SUNNY;
    if (strstr(text, "雷") || strstr(text, "暴雨")) return ICON_STORM;
    if (strstr(text, "雨")) return ICON_RAIN;
    if (strstr(text, "雪")) return ICON_SNOW;
    if (strstr(text, "雾") || strstr(text, "霾")) return ICON_FOG;
    if (strstr(text, "云") || strstr(text, "阴")) return ICON_CLOUDY;
    return ICON_SUNNY;
}

static void app_btn_event_cb(lv_event_t * e) {
    gui_app_id_t app_id = (gui_app_id_t)(intptr_t)lv_event_get_user_data(e);
    gui_app_navigate_to(app_id);
}

static void camera_btn_event_cb(lv_event_t * e) {
    // 触发拍照存取
    gui_app_show_camera_preview();
}

// 点击留言板卡片（空白区域）→ 刷新留言列表
static void msg_card_click_cb(lv_event_t * e) {
    gui_launcher_refresh_dashboard();
}

// 点击单条留言 → 取消该条的高亮
static void note_item_click_cb(lv_event_t * e) {
    time_t ts = (time_t)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t * item = lv_event_get_target(e);

    if (new_note_timestamps.count(ts)) {
        new_note_timestamps.erase(ts);
        // 恢复普通样式
        lv_obj_set_style_bg_color(item, COLOR_NOTE_NORMAL_BG, 0);
        // 移除左侧色条
        lv_obj_set_style_border_width(item, 0, 0);
    }
}

void gui_launcher_init(lv_obj_t* parent) {
    // ------------------------------------------------------------------------
    // 1. Top Status Bar (60px height, clean white)
    // ------------------------------------------------------------------------
    // Status bar: use absolute alignment so the right-side items cannot be
    // pushed out of bounds by the left-side label.
    lv_obj_t *status_bar = lv_obj_create(parent);
    lv_obj_set_size(status_bar, LV_PCT(100), 72);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(status_bar, THEME_SURFACE, 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_set_style_pad_all(status_bar, 0, 0);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *device_name = lv_label_create(status_bar);
    lv_label_set_text(device_name, "小鲜智能冰箱");
    lv_obj_set_style_text_font(device_name, font_cn_24, 0);
    lv_obj_set_style_text_color(device_name, THEME_TEXT_MAIN, 0);
    lv_obj_align(device_name, LV_ALIGN_LEFT_MID, THEME_SPACE_L, 0);

    lv_obj_t *right_group = lv_obj_create(status_bar);
    lv_obj_set_size(right_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(right_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right_group, 0, 0);
    lv_obj_set_style_pad_all(right_group, 0, 0);
    lv_obj_set_flex_flow(right_group, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right_group, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right_group, 16, 0);
    lv_obj_clear_flag(right_group, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(right_group, LV_ALIGN_RIGHT_MID, -THEME_SPACE_L, 0);

    // Built-in LVGL Wi-Fi symbol (reliable, included in Montserrat 24)
    wifi_icon_ptr = lv_label_create(right_group);
    lv_label_set_text(wifi_icon_ptr, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(wifi_icon_ptr, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(wifi_icon_ptr,
        SystemManager::get_wifi_status() == WifiStatus::CONNECTED ? THEME_PRIMARY : THEME_TEXT_SUB, 0);

    lv_obj_t *time_group = lv_obj_create(right_group);
    lv_obj_set_size(time_group, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(time_group, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(time_group, 0, 0);
    lv_obj_set_style_pad_top(time_group, 6, 0);
    lv_obj_set_style_pad_bottom(time_group, 6, 0);
    lv_obj_set_flex_flow(time_group, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(time_group, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(time_group, LV_OBJ_FLAG_SCROLLABLE);

    time_label_ptr = lv_label_create(time_group);
    lv_label_set_text(time_label_ptr, "--:--");
    lv_obj_set_style_text_font(time_label_ptr, font_cn_24, 0);
    lv_obj_set_style_text_color(time_label_ptr, THEME_TEXT_MAIN, 0);

    date_label_ptr = lv_label_create(time_group);
    lv_label_set_text(date_label_ptr, "");
    lv_obj_set_style_text_font(date_label_ptr, font_cn_16, 0);
    lv_obj_set_style_text_color(date_label_ptr, THEME_TEXT_SUB, 0);

    // Create timer for clock update (every 1 second)
    lv_timer_create(clock_timer_cb, 1000, NULL);
    vTaskDelay(1);

    // ------------------------------------------------------------------------
    // 2. Left Column: Weather + Message Board
    // ------------------------------------------------------------------------
    lv_obj_t *left_col = lv_obj_create(parent);
    lv_obj_set_size(left_col, 320, 440);
    lv_obj_align(left_col, LV_ALIGN_TOP_LEFT, THEME_SPACE_L, 78);
    lv_obj_set_style_bg_opa(left_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_col, 0, 0);
    lv_obj_set_style_pad_all(left_col, 0, 0);

    // Weather card
    lv_obj_t *weather_card = gui_card_create(left_col);
    lv_obj_set_size(weather_card, LV_PCT(100), 140);
    lv_obj_align(weather_card, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_pad_all(weather_card, THEME_SPACE_M, 0);
    lv_obj_set_flex_flow(weather_card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(weather_card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *weather_text_col = lv_obj_create(weather_card);
    lv_obj_set_size(weather_text_col, LV_SIZE_CONTENT, LV_PCT(100));
    lv_obj_set_style_bg_opa(weather_text_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(weather_text_col, 0, 0);
    lv_obj_set_flex_flow(weather_text_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(weather_text_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(weather_text_col, LV_OBJ_FLAG_SCROLLABLE);

    weather_temp_ptr = lv_label_create(weather_text_col);
    lv_label_set_text(weather_temp_ptr, "--°");
    lv_obj_set_style_text_font(weather_temp_ptr, font_cn_36, 0);
    lv_obj_set_style_text_color(weather_temp_ptr, THEME_TEXT_MAIN, 0);

    weather_desc_ptr = lv_label_create(weather_text_col);
    lv_label_set_text(weather_desc_ptr, "-- · --");
    lv_obj_set_style_text_font(weather_desc_ptr, font_cn_16, 0);
    lv_obj_set_style_text_color(weather_desc_ptr, THEME_TEXT_SUB, 0);

    weather_icon_ptr = lv_label_create(weather_card);
    lv_label_set_text(weather_icon_ptr, ICON_SUNNY);
    lv_obj_set_style_text_font(weather_icon_ptr, font_icon_36, 0);
    lv_obj_set_style_text_color(weather_icon_ptr, THEME_WARNING, 0);

    // Message board card
    msg_card = gui_card_create(left_col);
    lv_obj_set_size(msg_card, LV_PCT(100), 280);
    lv_obj_align(msg_card, LV_ALIGN_TOP_LEFT, 0, 160);
    lv_obj_set_style_pad_all(msg_card, THEME_SPACE_M, 0);
    lv_obj_set_flex_flow(msg_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(msg_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *msg_title_row = lv_obj_create(msg_card);
    lv_obj_set_width(msg_title_row, LV_PCT(100));
    lv_obj_set_height(msg_title_row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(msg_title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(msg_title_row, 0, 0);
    lv_obj_set_flex_flow(msg_title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(msg_title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(msg_title_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *msg_title = lv_label_create(msg_title_row);
    lv_label_set_text(msg_title, "留言板");
    lv_obj_set_style_text_font(msg_title, font_cn_24, 0);
    lv_obj_set_style_text_color(msg_title, THEME_TEXT_MAIN, 0);

    lv_obj_t *msg_refresh = lv_label_create(msg_title_row);
    lv_label_set_text(msg_refresh, ICON_NOTIFICATION);
    lv_obj_set_style_text_font(msg_refresh, font_icon_24, 0);
    lv_obj_set_style_text_color(msg_refresh, THEME_PRIMARY, 0);

    msg_list = lv_obj_create(msg_card);
    lv_obj_set_width(msg_list, LV_PCT(100));
    lv_obj_set_flex_grow(msg_list, 1);
    lv_obj_set_style_bg_opa(msg_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(msg_list, 0, 0);
    lv_obj_set_style_pad_all(msg_list, 0, 0);
    lv_obj_set_style_pad_row(msg_list, THEME_SPACE_S, 0);
    lv_obj_set_flex_flow(msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(msg_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(msg_list, LV_DIR_VER);

    // Click empty area to refresh
    lv_obj_add_flag(msg_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(msg_card, msg_card_click_cb, LV_EVENT_CLICKED, NULL);
    vTaskDelay(1);

    // ------------------------------------------------------------------------
    // 3. Right Column: App Grid
    // ------------------------------------------------------------------------
    lv_obj_t *grid = lv_obj_create(parent);
    lv_obj_set_size(grid, 640, 380);
    lv_obj_align(grid, LV_ALIGN_TOP_RIGHT, -THEME_SPACE_L, 78);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(grid, THEME_SPACE_L, 0);
    lv_obj_set_style_pad_row(grid, THEME_SPACE_L, 0);

    struct AppItem {
        const char *icon;
        const char *name;
        gui_app_id_t id;
        lv_color_t grad_start;
        lv_color_t grad_end;
    };

    AppItem apps[] = {
        {ICON_INVENTORY, "库存管理", GUI_APP_INVENTORY, lv_color_hex(0x4A90E2), lv_color_hex(0x357ABD)},
        {ICON_RECIPES,   "我的食谱", GUI_APP_RECIPES,   lv_color_hex(0xFF9800), lv_color_hex(0xF57C00)},
        {ICON_VOICE,     "语音助手", GUI_APP_VOICE_ASSIST, lv_color_hex(0x9C27B0), lv_color_hex(0x7B1FA2)},
        {ICON_SHOPPING,  "购物清单", GUI_APP_SHOPPING,  lv_color_hex(0x2ECC71), lv_color_hex(0x27AE60)},
        {ICON_SETTINGS,  "系统设置", GUI_APP_SETTINGS,  lv_color_hex(0x607D8B), lv_color_hex(0x455A64)},
    };

    int num_apps = sizeof(apps) / sizeof(apps[0]);
    for (int i = 0; i < num_apps; i++) {
        gui_icon_button_create(grid, apps[i].icon, apps[i].name,
                               apps[i].grad_start, apps[i].grad_end,
                               app_btn_event_cb, (void*)(intptr_t)apps[i].id);
        vTaskDelay(1);
    }

    // ------------------------------------------------------------------------
    // 4. Bottom Full-width Action Banner (Camera Access)
    // ------------------------------------------------------------------------
    // Banner aligns with the right-side app grid (width 640, right margin 24)
    lv_obj_t *banner_btn = lv_btn_create(parent);
    lv_obj_set_size(banner_btn, 640, 84);
    lv_obj_align(banner_btn, LV_ALIGN_BOTTOM_RIGHT, -THEME_SPACE_L, -THEME_SPACE_L);
    theme_apply_gradient_bg(banner_btn, THEME_PRIMARY, THEME_ACCENT);
    lv_obj_set_style_radius(banner_btn, THEME_RADIUS_M, 0);
    lv_obj_set_style_border_width(banner_btn, 0, 0);
    lv_obj_set_style_shadow_width(banner_btn, 8, 0);
    lv_obj_set_style_shadow_ofs_y(banner_btn, 3, 0);
    lv_obj_set_style_shadow_opa(banner_btn, LV_OPA_20, 0);
    lv_obj_set_style_pad_left(banner_btn, THEME_SPACE_L, 0);
    lv_obj_set_style_pad_right(banner_btn, THEME_SPACE_L, 0);
    lv_obj_set_flex_flow(banner_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(banner_btn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(banner_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(banner_btn, camera_btn_event_cb, LV_EVENT_CLICKED, NULL);
    theme_apply_press_effect(banner_btn);

    lv_obj_t *banner_left = lv_obj_create(banner_btn);
    lv_obj_set_size(banner_left, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(banner_left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(banner_left, 0, 0);
    lv_obj_set_flex_flow(banner_left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(banner_left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(banner_left, THEME_SPACE_M, 0);
    lv_obj_clear_flag(banner_left, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *banner_icon = lv_label_create(banner_left);
    lv_label_set_text(banner_icon, ICON_CAMERA);
    lv_obj_set_style_text_font(banner_icon, font_icon_36, 0);
    lv_obj_set_style_text_color(banner_icon, lv_color_white(), 0);

    lv_obj_t *banner_title = lv_label_create(banner_left);
    lv_label_set_text(banner_title, "拍照存取");
    lv_obj_set_style_text_font(banner_title, font_cn_24, 0);
    lv_obj_set_style_text_color(banner_title, lv_color_white(), 0);

    lv_obj_t *banner_desc = lv_label_create(banner_btn);
    lv_label_set_text(banner_desc, "对准食材拍照，自动识别并存入");
    lv_obj_set_style_text_font(banner_desc, font_cn_16, 0);
    lv_obj_set_style_text_color(banner_desc, lv_color_white(), 0);
    lv_obj_set_style_text_opa(banner_desc, LV_OPA_80, 0);
    vTaskDelay(1);

    // No entrance animation to keep Launcher snappy
}

// ============================================================
// 桌面实时数据刷新（由 gui_bridge_refresh_dashboard 触发）
// ============================================================

void gui_launcher_update_weather(float temp, const char* city, const char* text) {
    if (weather_temp_ptr) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%.0f°", temp);
        lv_label_set_text(weather_temp_ptr, buf);
    }
    if (weather_desc_ptr) {
        char buf[64];
        const char* c = city ? city : "--";
        const char* t = (text && text[0]) ? text : "--";
        snprintf(buf, sizeof(buf), "%s · %s", c, t);
        lv_label_set_text(weather_desc_ptr, buf);
    }
    if (weather_icon_ptr) {
        lv_label_set_text(weather_icon_ptr, weather_icon_for(text));
        // Tint icon by weather type for quick visual recognition
        lv_color_t icon_color = THEME_WARNING; // sunny default
        if (text) {
            if (strstr(text, "雨")) icon_color = THEME_PRIMARY;
            else if (strstr(text, "雪") || strstr(text, "雾") || strstr(text, "霾")) icon_color = THEME_TEXT_SUB;
            else if (strstr(text, "雷") || strstr(text, "暴雨")) icon_color = THEME_DANGER;
            else if (strstr(text, "云") || strstr(text, "阴")) icon_color = THEME_TEXT_SUB;
        }
        lv_obj_set_style_text_color(weather_icon_ptr, icon_color, 0);
    }
}

void gui_launcher_update_notes(const void* notes_vec_ptr) {
    if (!msg_list || !lv_obj_is_valid(msg_list)) return;

    // 清空旧的留言条目
    lv_obj_clean(msg_list);

    if (!notes_vec_ptr) {
        lv_obj_t * empty = lv_label_create(msg_list);
        lv_label_set_text(empty, "暂无留言");
        lv_obj_add_style(empty, &style_text_sub, 0);
        return;
    }

    const std::vector<smart_fridge::dashboard::Note>* notes =
        static_cast<const std::vector<smart_fridge::dashboard::Note>*>(notes_vec_ptr);

    if (notes->empty()) {
        lv_obj_t * empty = lv_label_create(msg_list);
        lv_label_set_text(empty, "暂无留言");
        lv_obj_add_style(empty, &style_text_sub, 0);
        return;
    }

    for (const auto& note : *notes) {

        bool is_new = new_note_timestamps.count(note.timestamp) > 0;

        lv_obj_t * item = lv_obj_create(msg_list);
        lv_obj_set_size(item, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(item, 6, 0);
        lv_obj_set_style_radius(item, 8, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

        if (is_new) {
            // 新留言：暖橙高亮底色 + 左侧 3px 橙色色条
            lv_obj_set_style_bg_color(item, COLOR_NOTE_NEW_BG, 0);
            lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(item, 3, 0);
            lv_obj_set_style_border_side(item, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_color(item, COLOR_NOTE_NEW_ACCENT, 0);
        } else {
            // 普通留言
            lv_obj_set_style_bg_color(item, COLOR_NOTE_NORMAL_BG, 0);
            lv_obj_set_style_bg_opa(item, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(item, 0, 0);
        }

        // 点击取消高亮
        lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(item, note_item_click_cb, LV_EVENT_CLICKED,
                            (void*)(intptr_t)note.timestamp);

        lv_obj_t * label = lv_label_create(item);
        lv_label_set_text(label, note.text.c_str());
        lv_obj_set_style_text_font(label, font_cn_18, 0);
        lv_obj_set_style_text_color(label, COLOR_TEXT_MAIN, 0);
        lv_obj_set_width(label, LV_PCT(100));
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    }
}

void gui_launcher_mark_note_new(long long timestamp) {
    new_note_timestamps.insert((time_t)timestamp);
}

void gui_launcher_refresh_dashboard(void) {
    // 从 dashboard 缓存读取并刷新桌面天气与留言
    auto w = smart_fridge::dashboard::weather_get();
    if (w.valid) {
        gui_launcher_update_weather(w.temp, w.city.c_str(), w.text.c_str());
    } else {
        gui_launcher_update_weather(0.0f, NULL, NULL);
    }

    auto notes = smart_fridge::dashboard::notes_get_all();
    gui_launcher_update_notes(&notes);
}
