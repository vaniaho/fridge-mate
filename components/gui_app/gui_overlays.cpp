#include "lvgl.h"
#include "gui_app.h"
#include "gui_styles.h"
#include <stdio.h>

static lv_obj_t * overlay_layer = NULL;
static lv_obj_t * notif_label = NULL;
static lv_obj_t * listening_indicator = NULL;
static lv_timer_t * notif_timer = NULL;

// TTS 播报文本指示器（对应 UI 设计方案 §1.2.3 的「播报中」状态）
static lv_obj_t * tts_indicator = NULL;
static lv_timer_t * tts_timer = NULL;

void gui_overlays_init(void) {
    // LVGL has a top layer `lv_layer_top()` for overlays
    overlay_layer = lv_layer_top();

    // 语音监听指示器
    listening_indicator = lv_obj_create(overlay_layer);
    lv_obj_set_size(listening_indicator, 60, 60);
    lv_obj_align(listening_indicator, LV_ALIGN_TOP_RIGHT, -20, 20);
    lv_obj_set_style_bg_color(listening_indicator, lv_color_hex(0x3498DB), 0);
    lv_obj_set_style_radius(listening_indicator, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(listening_indicator, LV_OBJ_FLAG_HIDDEN); // 默认隐藏

    lv_obj_t * mic_icon = lv_label_create(listening_indicator);
    lv_label_set_text(mic_icon, "Mic"); // Fallback
    lv_obj_align(mic_icon, LV_ALIGN_CENTER, 0, 0);

    // 通知浮层
    notif_label = lv_label_create(overlay_layer);
    lv_obj_add_style(notif_label, &style_card, 0);
    lv_obj_set_style_bg_color(notif_label, COLOR_WARNING, 0);
    lv_obj_set_style_text_color(notif_label, lv_color_white(), 0);
    lv_obj_align(notif_label, LV_ALIGN_TOP_MID, 0, -100); // 默认在屏幕外

    // TTS 播报文本指示器（底部悬浮条，绿色「播报中」状态）
    tts_indicator = lv_obj_create(overlay_layer);
    lv_obj_set_size(tts_indicator, 600, 60);
    lv_obj_set_style_radius(tts_indicator, 16, 0);
    lv_obj_set_style_bg_color(tts_indicator, COLOR_SUCCESS, 0);
    lv_obj_set_style_bg_opa(tts_indicator, LV_OPA_80, 0);
    lv_obj_set_style_border_width(tts_indicator, 0, 0);
    lv_obj_set_style_pad_hor(tts_indicator, 20, 0);
    lv_obj_clear_flag(tts_indicator, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(tts_indicator, LV_ALIGN_BOTTOM_MID, 0, 100); // 默认在屏幕外（下方）

    lv_obj_t * tts_text = lv_label_create(tts_indicator);
    lv_obj_set_width(tts_text, 560);
    lv_label_set_long_mode(tts_text, LV_LABEL_LONG_DOT);
    lv_label_set_text(tts_text, "");
    lv_obj_set_style_text_color(tts_text, lv_color_white(), 0);
    lv_obj_set_style_text_font(tts_text, font_cn_18, 0);
    lv_obj_align(tts_text, LV_ALIGN_LEFT_MID, 0, 0);
}

// 隐藏 TTS 指示器（淡出回屏幕外）
static void tts_hide_timer_cb(lv_timer_t * timer) {
    if (!tts_indicator) return;
    lv_obj_align(tts_indicator, LV_ALIGN_BOTTOM_MID, 0, 100);
    lv_obj_add_flag(tts_indicator, LV_OBJ_FLAG_HIDDEN);
}

void gui_app_show_tts_text(const char* text) {
    if (!tts_indicator) return;

    // 更新文本内容
    lv_obj_t * txt = lv_obj_get_child(tts_indicator, 0);
    if (txt) {
        lv_label_set_text(txt, text ? text : "");
    }

    // 显示在屏幕底部
    lv_obj_clear_flag(tts_indicator, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(tts_indicator, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_move_foreground(tts_indicator);

    // 重置自动隐藏定时器（5 秒后淡出，足够覆盖大多数 LLM 播报内容）
    if (tts_timer) {
        lv_timer_del(tts_timer);
    }
    tts_timer = lv_timer_create(tts_hide_timer_cb, 5000, NULL);
    lv_timer_set_repeat_count(tts_timer, 1);
}

void gui_app_show_listening_indicator(bool is_listening) {
    if (!listening_indicator) return;
    if (is_listening) {
        lv_obj_clear_flag(listening_indicator, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(listening_indicator, LV_OBJ_FLAG_HIDDEN);
    }
}

// 动画 y 坐标设置回调
static void notif_anim_cb(void * var, int32_t v) {
    lv_obj_set_y((lv_obj_t*)var, v);
}

// 隐藏通知
static void notif_hide_timer_cb(lv_timer_t * timer) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, notif_label);
    lv_anim_set_values(&a, lv_obj_get_y(notif_label), -100);
    lv_anim_set_time(&a, 300);
    lv_anim_set_exec_cb(&a, notif_anim_cb);
    lv_anim_start(&a);
}

void gui_app_show_notification(const char* title, const char* message) {
    if (!notif_label) return;
    
    char buf[256];
    snprintf(buf, sizeof(buf), "%s\n%s", title ? title : "", message ? message : "");
    lv_label_set_text(notif_label, buf);

    // 滑入动画
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, notif_label);
    lv_anim_set_values(&a, -100, 20);
    lv_anim_set_time(&a, 300);
    lv_anim_set_exec_cb(&a, notif_anim_cb);
    lv_anim_start(&a);

    // 重置并启动隐藏定时器 (3秒后)
    if (notif_timer) {
        lv_timer_del(notif_timer);
    }
    notif_timer = lv_timer_create(notif_hide_timer_cb, 3000, NULL);
    lv_timer_set_repeat_count(notif_timer, 1);
}

void gui_app_show_voice_assist(void) {
    if (!overlay_layer) return;

    lv_obj_t * voice_bg = lv_obj_create(overlay_layer);
    lv_obj_set_size(voice_bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(voice_bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(voice_bg, LV_OPA_80, 0);

    lv_obj_t * close_btn = lv_btn_create(voice_bg);
    lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 20, 20);
    lv_obj_t * close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "<- Back");
    
    lv_obj_add_event_cb(close_btn, [](lv_event_t * e) {
        lv_obj_del((lv_obj_t*)lv_event_get_user_data(e));
    }, LV_EVENT_CLICKED, voice_bg);

    lv_obj_t * title = lv_label_create(voice_bg);
    lv_label_set_text(title, "Listening...");
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);
}

void gui_app_show_camera_preview(void) {
    if (!overlay_layer) return;

    lv_obj_t * cam_bg = lv_obj_create(overlay_layer);
    lv_obj_set_size(cam_bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(cam_bg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(cam_bg, LV_OPA_90, 0);

    lv_obj_t * close_btn = lv_btn_create(cam_bg);
    lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 20, 20);
    lv_obj_t * close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "<- Cancel");
    
    lv_obj_add_event_cb(close_btn, [](lv_event_t * e) {
        lv_obj_del((lv_obj_t*)lv_event_get_user_data(e));
    }, LV_EVENT_CLICKED, cam_bg);

    lv_obj_t * cam_placeholder = lv_obj_create(cam_bg);
    lv_obj_set_size(cam_placeholder, 320, 240);
    lv_obj_align(cam_placeholder, LV_ALIGN_CENTER, 0, -40);
    lv_obj_set_style_bg_color(cam_placeholder, lv_color_hex(0x333333), 0);
    lv_obj_t * ph_lbl = lv_label_create(cam_placeholder);
    lv_label_set_text(ph_lbl, "[Camera Preview]");
    lv_obj_set_style_text_color(ph_lbl, lv_color_white(), 0);
    lv_obj_align(ph_lbl, LV_ALIGN_CENTER, 0, 0);
}
