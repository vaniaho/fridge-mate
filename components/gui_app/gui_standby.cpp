#include "lvgl.h"
#include "gui_app.h"
#include "gui_styles.h"

static void standby_click_cb(lv_event_t * e) {
    gui_app_wake_from_standby();
}

void gui_standby_init(lv_obj_t* parent) {
    // 黑屏背景
    lv_obj_set_style_bg_color(parent, lv_color_black(), 0);
    
    // 点击任意位置唤醒
    lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(parent, standby_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t * time_label = lv_label_create(parent);
    lv_label_set_text(time_label, "14:30");
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(time_label, font_cn_36, 0);
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t * hint_label = lv_label_create(parent);
    lv_label_set_text(hint_label, "轻触屏幕以唤醒");
    lv_obj_set_style_text_color(hint_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(hint_label, font_cn_18, 0);
    lv_obj_align(hint_label, LV_ALIGN_CENTER, 0, 40);
}
