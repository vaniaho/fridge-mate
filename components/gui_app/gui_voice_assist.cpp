#include "lvgl.h"
#include "gui_app.h"
#include "gui_styles.h"
#include "gui_theme.h"
#include "gui_icons.h"
#include "audio_api.h"
#include "system_events.h"
#include <stdio.h>
#include <string.h>
#include <cmath>

static lv_obj_t* s_voice_overlay = NULL;
static lv_obj_t* s_mic_icon = NULL;
static lv_obj_t* s_status_label = NULL;
static lv_obj_t* s_chat_list = NULL;
static lv_timer_t* s_wave_timer = NULL;
static lv_obj_t* s_mode_label = NULL;

namespace {

static void wave_timer_cb(lv_timer_t* timer) {
    if (!s_mic_icon) return;
    static int phase = 0;
    phase += 1;
    int scale = 256 + 40 * std::sin(phase * 0.2f);
    if (scale < 256) scale = 256;
    lv_obj_set_style_transform_zoom(s_mic_icon, scale, 0);
}

static void close_btn_cb(lv_event_t* e) {
    (void)e;
    if (s_voice_overlay) {
        lv_obj_del(s_voice_overlay);
        s_voice_overlay = NULL;
        s_mic_icon = NULL;
        s_status_label = NULL;
        s_chat_list = NULL;
        s_mode_label = NULL;
        if (s_wave_timer) {
            lv_timer_del(s_wave_timer);
            s_wave_timer = NULL;
        }
    }
    voice_session_stop();
}

static void interrupt_btn_cb(lv_event_t*) {
    voice_session_interrupt();
}

static void mode_btn_cb(lv_event_t*) {
    audio_voice_mode_t next =
        audio_hal_get_voice_mode() == AUDIO_VOICE_MODE_CASCADE
            ? AUDIO_VOICE_MODE_REALTIME
            : AUDIO_VOICE_MODE_CASCADE;
    if (s_mode_label) {
        lv_label_set_text(
            s_mode_label,
            next == AUDIO_VOICE_MODE_REALTIME
                ? "模式：端到端实时语音"
                : "模式：ASR → LLM → TTS");
    }
    voice_session_set_mode((int)next);
}

static const char* state_to_text(voice_assist_state_t state, const char* custom) {
    if (custom && custom[0]) return custom;
    switch (state) {
        case VOICE_STATE_LISTENING: return "正在聆听...";
        case VOICE_STATE_THINKING:  return "正在思考...";
        case VOICE_STATE_SPEAKING:  return "正在播报...";
        default: return "说“小鲜小鲜”唤醒我";
    }
}

} // anonymous namespace

void gui_voice_assist_show(void) {
    if (s_voice_overlay) return;

    lv_obj_t* top = lv_layer_top();

    s_voice_overlay = lv_obj_create(top);
    lv_obj_set_size(s_voice_overlay, LV_PCT(100), LV_PCT(100));
    theme_apply_gradient_bg(s_voice_overlay, lv_color_hex(0x0f172a), lv_color_hex(0x1e293b));
    lv_obj_set_style_pad_all(s_voice_overlay, 0, 0);
    lv_obj_clear_flag(s_voice_overlay, LV_OBJ_FLAG_SCROLLABLE);

    // 返回按钮 (Icon + Text combined in a flex row)
    lv_obj_t* close_btn = lv_btn_create(s_voice_overlay);
    lv_obj_align(close_btn, LV_ALIGN_TOP_LEFT, 20, 20);
    lv_obj_set_style_bg_opa(close_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(close_btn, 0, 0);
    lv_obj_add_event_cb(close_btn, close_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_set_flex_flow(close_btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(close_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(close_btn, 8, 0);

    lv_obj_t* close_icon = lv_label_create(close_btn);
    lv_label_set_text(close_icon, ICON_BACK);
    lv_obj_set_style_text_font(close_icon, font_icon_24, 0);
    lv_obj_set_style_text_color(close_icon, lv_color_white(), 0);

    lv_obj_t* close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, "返回");
    lv_obj_set_style_text_color(close_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(close_lbl, font_cn_18, 0);

    // 标题
    lv_obj_t* title = lv_label_create(s_voice_overlay);
    lv_label_set_text(title, "语音助手");
    lv_obj_set_style_text_font(title, font_cn_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    s_mode_label = lv_label_create(s_voice_overlay);
    lv_label_set_text(
        s_mode_label,
        audio_hal_get_voice_mode() == AUDIO_VOICE_MODE_REALTIME
            ? "模式：端到端实时语音"
            : "模式：ASR → LLM → TTS");
    lv_obj_set_style_text_font(s_mode_label, font_cn_16, 0);
    lv_obj_set_style_text_color(s_mode_label, THEME_PRIMARY_LIGHT, 0);
    lv_obj_align(s_mode_label, LV_ALIGN_TOP_MID, 0, 68);

    lv_obj_t* mode_btn = lv_btn_create(s_voice_overlay);
    lv_obj_set_size(mode_btn, 130, 44);
    lv_obj_align(mode_btn, LV_ALIGN_TOP_RIGHT, -170, 20);
    lv_obj_set_style_bg_color(mode_btn, THEME_SURFACE, 0);
    lv_obj_set_style_bg_opa(mode_btn, LV_OPA_30, 0);
    lv_obj_add_event_cb(mode_btn, mode_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* mode_btn_label = lv_label_create(mode_btn);
    lv_label_set_text(mode_btn_label, "切换模式");
    lv_obj_set_style_text_font(mode_btn_label, font_cn_16, 0);
    lv_obj_center(mode_btn_label);

    lv_obj_t* interrupt_btn = lv_btn_create(s_voice_overlay);
    lv_obj_set_size(interrupt_btn, 130, 44);
    lv_obj_align(interrupt_btn, LV_ALIGN_TOP_RIGHT, -24, 20);
    lv_obj_set_style_bg_color(interrupt_btn, lv_color_hex(0xEF5350), 0);
    lv_obj_add_event_cb(interrupt_btn, interrupt_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* interrupt_label = lv_label_create(interrupt_btn);
    lv_label_set_text(interrupt_label, "打断 / 重听");
    lv_obj_set_style_text_font(interrupt_label, font_cn_16, 0);
    lv_obj_center(interrupt_label);

    // 麦克风脉冲动画容器
    s_mic_icon = lv_btn_create(s_voice_overlay);
    lv_obj_set_size(s_mic_icon, 80, 80);
    lv_obj_set_style_radius(s_mic_icon, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_mic_icon, THEME_PRIMARY, 0);
    lv_obj_set_style_shadow_color(s_mic_icon, THEME_PRIMARY, 0);
    lv_obj_set_style_shadow_width(s_mic_icon, 40, 0);
    lv_obj_set_style_shadow_opa(s_mic_icon, LV_OPA_50, 0);
    lv_obj_align(s_mic_icon, LV_ALIGN_CENTER, 0, -60);
    
    lv_obj_t* mic_glyph = lv_label_create(s_mic_icon);
    lv_label_set_text(mic_glyph, ICON_MIC);
    lv_obj_set_style_text_font(mic_glyph, font_icon_36, 0);
    lv_obj_set_style_text_color(mic_glyph, lv_color_white(), 0);
    lv_obj_center(mic_glyph);

    // 状态文字
    s_status_label = lv_label_create(s_voice_overlay);
    lv_label_set_text(s_status_label, state_to_text(VOICE_STATE_IDLE, NULL));
    lv_obj_set_style_text_font(s_status_label, font_cn_18, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_white(), 0);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 20);

    // 对话历史
    s_chat_list = lv_obj_create(s_voice_overlay);
    lv_obj_set_size(s_chat_list, 760, 200);
    lv_obj_align(s_chat_list, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_bg_opa(s_chat_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chat_list, 0, 0);
    lv_obj_set_flex_flow(s_chat_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_chat_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(s_chat_list, 12, 0);
    lv_obj_set_scrollbar_mode(s_chat_list, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_scroll_dir(s_chat_list, LV_DIR_VER);

    // 启动波形动画
    s_wave_timer = lv_timer_create(wave_timer_cb, 50, NULL);
}

void gui_voice_assist_hide(void) {
    if (!s_voice_overlay) return;
    lv_obj_del(s_voice_overlay);
    s_voice_overlay = NULL;
    s_mic_icon = NULL;
    s_status_label = NULL;
    s_chat_list = NULL;
    s_mode_label = NULL;
    if (s_wave_timer) {
        lv_timer_del(s_wave_timer);
        s_wave_timer = NULL;
    }
}

void gui_voice_assist_set_state(voice_assist_state_t state, const char* text) {
    if (s_status_label) {
        lv_label_set_text(s_status_label, state_to_text(state, text));
    }
}

void gui_voice_assist_add_message(const char* text, bool is_user) {
    if (!s_chat_list || !text) return;

    lv_obj_t* bubble = lv_obj_create(s_chat_list);
    lv_obj_set_width(bubble, LV_PCT(80));
    lv_obj_set_style_bg_color(bubble, is_user ? THEME_PRIMARY : THEME_SURFACE, 0);
    lv_obj_set_style_bg_opa(bubble, LV_OPA_90, 0);
    lv_obj_set_style_radius(bubble, 20, 0);
    lv_obj_set_style_border_width(bubble, 0, 0);
    lv_obj_set_style_pad_all(bubble, 16, 0);
    lv_obj_align(bubble, is_user ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(bubble, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(bubble);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, is_user ? lv_color_white() : THEME_TEXT_MAIN, 0);
    lv_obj_set_style_text_font(lbl, font_cn_18, 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, LV_PCT(100));

    // 滚动到底部
    lv_obj_scroll_to_y(s_chat_list, LV_COORD_MAX, LV_ANIM_ON);
}

// C 接口封装
extern "C" void gui_app_show_voice_assist(void) {
    gui_voice_assist_show();
}
