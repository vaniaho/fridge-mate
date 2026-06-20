#include "lvgl.h"
#include "gui_app.h"
#include "gui_styles.h"
#include "system_manager.hpp"
#include "notes_board.hpp"
#include "weather.hpp"
#include <stdio.h>
#include <time.h>
#include <vector>
#include <set>

using namespace smart_fridge::system;
extern lv_obj_t * wifi_icon_ptr;

static lv_obj_t * time_label_ptr = NULL;

// 桌面天气卡片 / 留言板控件指针（供后台数据刷新使用）
static lv_obj_t * weather_temp_ptr = NULL;
static lv_obj_t * weather_desc_ptr = NULL;

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
    } else {
        char buf[16];
        strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
        lv_label_set_text(time_label_ptr, buf);
    }
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
    // 1. Top Status Bar (50px height)
    // ------------------------------------------------------------------------
    lv_obj_t * status_bar = lv_obj_create(parent);
    lv_obj_set_size(status_bar, LV_PCT(100), 50);
    lv_obj_align(status_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(status_bar, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(status_bar, 0, 0);
    lv_obj_set_style_radius(status_bar, 0, 0);
    lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Bottom border for status bar
    lv_obj_set_style_border_width(status_bar, 1, 0);
    lv_obj_set_style_border_side(status_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(status_bar, COLOR_DIVIDER, 0);

    lv_obj_t * device_name = lv_label_create(status_bar);
    lv_label_set_text(device_name, "小鲜智能冰箱");
    lv_obj_add_style(device_name, &style_text_main, 0);
    lv_obj_align(device_name, LV_ALIGN_LEFT_MID, 20, 0);

    time_label_ptr = lv_label_create(status_bar);
    lv_label_set_text(time_label_ptr, "--:--");
    lv_obj_add_style(time_label_ptr, &style_text_main, 0);
    lv_obj_align(time_label_ptr, LV_ALIGN_RIGHT_MID, -20, 0);

    lv_obj_t * wifi_icon = lv_label_create(status_bar);
    lv_obj_set_style_text_font(wifi_icon, &lv_font_montserrat_18, 0);
    lv_label_set_text(wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_add_style(wifi_icon, &style_text_sub, 0);
    lv_obj_align(wifi_icon, LV_ALIGN_RIGHT_MID, -80, 0);

    wifi_icon_ptr = wifi_icon;
    gui_app_set_wifi_status(SystemManager::get_wifi_status() == WifiStatus::CONNECTED);

    // Create timer for clock update (every 1 second)
    lv_timer_create(clock_timer_cb, 1000, NULL);

    // ------------------------------------------------------------------------
    // 2. Left Column: Core Info Area (~320px)
    // ------------------------------------------------------------------------
    lv_obj_t * left_col = lv_obj_create(parent);
    lv_obj_set_size(left_col, 320, 440); // Total height minus top bar (50) and bottom banner (80+margins)
    lv_obj_align(left_col, LV_ALIGN_TOP_LEFT, 20, 60);
    lv_obj_set_style_bg_opa(left_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_col, 0, 0);
    lv_obj_set_style_pad_all(left_col, 0, 0);

    // Weather widget
    lv_obj_t * weather_card = lv_obj_create(left_col);
    lv_obj_set_size(weather_card, LV_PCT(100), 120);
    lv_obj_add_style(weather_card, &style_card, 0);
    
    lv_obj_t * weather_temp = lv_label_create(weather_card);
    lv_label_set_text(weather_temp, "--°");
    lv_obj_add_style(weather_temp, &style_text_title, 0);
    lv_obj_set_style_text_font(weather_temp, font_cn_36, 0);
    lv_obj_align(weather_temp, LV_ALIGN_TOP_LEFT, 10, 10);
    weather_temp_ptr = weather_temp;

    lv_obj_t * weather_desc = lv_label_create(weather_card);
    lv_label_set_text(weather_desc, "-- · --");
    lv_obj_add_style(weather_desc, &style_text_sub, 0);
    lv_obj_align(weather_desc, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    weather_desc_ptr = weather_desc;

    // ================================================================
    //  Message Board widget (留言板) — 支持逐条渲染 + 新留言高亮
    // ================================================================
    msg_card = lv_obj_create(left_col);
    lv_obj_set_size(msg_card, LV_PCT(100), 200);
    lv_obj_align(msg_card, LV_ALIGN_TOP_LEFT, 0, 140);
    lv_obj_add_style(msg_card, &style_card, 0);
    lv_obj_clear_flag(msg_card, LV_OBJ_FLAG_SCROLLABLE);
    // 点击空白区域 → 刷新
    lv_obj_add_flag(msg_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(msg_card, msg_card_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * msg_title = lv_label_create(msg_card);
    lv_label_set_text(msg_title, "留言板");
    lv_obj_add_style(msg_title, &style_text_main, 0);
    lv_obj_align(msg_title, LV_ALIGN_TOP_LEFT, 10, 5);

    // 留言列表容器（可滚动，位于标题下方）
    msg_list = lv_obj_create(msg_card);
    lv_obj_set_size(msg_list, LV_PCT(100) - 10, 160);
    lv_obj_align(msg_list, LV_ALIGN_TOP_LEFT, 5, 35);
    lv_obj_set_style_bg_opa(msg_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(msg_list, 0, 0);
    lv_obj_set_style_pad_all(msg_list, 0, 0);
    lv_obj_set_style_pad_row(msg_list, 6, 0);
    lv_obj_set_flex_flow(msg_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(msg_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(msg_list, LV_DIR_VER);

    // ------------------------------------------------------------------------
    // 3. Right Column: App Grid (~640px)
    // ------------------------------------------------------------------------
    lv_obj_t * grid = lv_obj_create(parent);
    lv_obj_set_size(grid, 640, 360);
    lv_obj_align(grid, LV_ALIGN_TOP_RIGHT, -20, 60);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    
    // Grid Layout (4 columns x 2 rows)
    static lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_style_grid_column_dsc_array(grid, col_dsc, 0);
    lv_obj_set_style_grid_row_dsc_array(grid, row_dsc, 0);
    lv_obj_set_layout(grid, LV_LAYOUT_GRID);

    struct AppItem {
        const char *icon;
        const char *name;
        gui_app_id_t id;
    };

    AppItem apps[] = {
        {LV_SYMBOL_LIST, "库存管理", GUI_APP_INVENTORY},
        {LV_SYMBOL_FILE, "我的食谱", GUI_APP_RECIPES},
        {LV_SYMBOL_SETTINGS, "系统设置", GUI_APP_SETTINGS},
        {LV_SYMBOL_AUDIO, "语音助手", GUI_APP_VOICE_ASSIST},
        {LV_SYMBOL_EDIT, "购物清单", GUI_APP_SHOPPING}
    };

    int num_apps = sizeof(apps) / sizeof(apps[0]);

    for(int i = 0; i < num_apps; i++) {
        int col = i % 4;
        int row = i / 4;

        lv_obj_t * btn = lv_btn_create(grid);
        lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, col, 1,
                                  LV_GRID_ALIGN_STRETCH, row, 1);
        lv_obj_add_style(btn, &style_card, 0);
        lv_obj_add_event_cb(btn, app_btn_event_cb, LV_EVENT_CLICKED, (void*)(intptr_t)apps[i].id);

        lv_obj_t * icon = lv_label_create(btn);
        lv_label_set_text(icon, apps[i].icon);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(icon, COLOR_TEXT_MAIN, 0);
        lv_obj_align(icon, LV_ALIGN_CENTER, 0, -15);

        lv_obj_t * label = lv_label_create(btn);
        lv_label_set_text(label, apps[i].name);
        lv_obj_set_style_text_color(label, COLOR_TEXT_MAIN, 0);
        lv_obj_set_style_text_font(label, font_cn_18, 0);
        lv_obj_align(label, LV_ALIGN_CENTER, 0, 25);
    }

    // ------------------------------------------------------------------------
    // 4. Bottom Full-width Action Banner (Camera Access)
    // ------------------------------------------------------------------------
    lv_obj_t * banner_btn = lv_btn_create(parent);
    lv_obj_set_size(banner_btn, LV_PCT(100) - 40, 80); // Width: 100% minus padding
    lv_obj_align(banner_btn, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_color(banner_btn, COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(banner_btn, 12, 0);
    lv_obj_add_event_cb(banner_btn, camera_btn_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * banner_icon = lv_label_create(banner_btn);
    lv_label_set_text(banner_icon, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_font(banner_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(banner_icon, lv_color_white(), 0);
    lv_obj_align(banner_icon, LV_ALIGN_LEFT_MID, 20, 0);

    lv_obj_t * banner_title = lv_label_create(banner_btn);
    lv_label_set_text(banner_title, "拍照存取");
    lv_obj_set_style_text_font(banner_title, font_cn_24, 0);
    lv_obj_set_style_text_color(banner_title, lv_color_white(), 0);
    lv_obj_align(banner_title, LV_ALIGN_LEFT_MID, 60, 0);

    lv_obj_t * banner_desc = lv_label_create(banner_btn);
    lv_label_set_text(banner_desc, "对准食材拍照，自动识别并存入");
    lv_obj_set_style_text_font(banner_desc, font_cn_16, 0);
    lv_obj_set_style_text_color(banner_desc, lv_color_white(), 0);
    lv_obj_set_style_text_opa(banner_desc, LV_OPA_80, 0);
    lv_obj_align(banner_desc, LV_ALIGN_RIGHT_MID, -30, 0);
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
