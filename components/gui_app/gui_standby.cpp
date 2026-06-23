#include "lvgl.h"
#include "gui_app.h"
#include "gui_styles.h"
#include "gui_theme.h"
#include "gui_icons.h"
#include <time.h>

static lv_obj_t *s_time_label = NULL;
static lv_obj_t *s_date_label = NULL;

static void standby_click_cb(lv_event_t * e) {
    gui_app_wake_from_standby();
}

static void standby_clock_timer_cb(lv_timer_t *timer) {
    if (!s_time_label) return;
    time_t now;
    struct tm timeinfo;
    time(&now);
    localtime_r(&now, &timeinfo);

    char buf[32];
    if (timeinfo.tm_year < (2020 - 1900)) {
        lv_label_set_text(s_time_label, "--:--");
        if (s_date_label) lv_label_set_text(s_date_label, "");
    } else {
        strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
        lv_label_set_text(s_time_label, buf);
        if (s_date_label) {
            strftime(buf, sizeof(buf), "%Y年%m月%d日 %A", &timeinfo);
            lv_label_set_text(s_date_label, buf);
        }
    }
}

void gui_standby_init(lv_obj_t* parent) {
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x111418), 0);

    lv_obj_add_flag(parent, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(parent, standby_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *center_col = lv_obj_create(parent);
    lv_obj_set_size(center_col, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(center_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(center_col, 0, 0);
    lv_obj_set_flex_flow(center_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(center_col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(center_col, THEME_SPACE_M, 0);
    lv_obj_clear_flag(center_col, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_center(center_col);

    s_time_label = lv_label_create(center_col);
    lv_label_set_text(s_time_label, "14:30");
    lv_obj_set_style_text_color(s_time_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_time_label, font_cn_36, 0);

    s_date_label = lv_label_create(center_col);
    lv_label_set_text(s_date_label, "2026年06月22日 星期一");
    lv_obj_set_style_text_color(s_date_label, THEME_TEXT_SUB, 0);
    lv_obj_set_style_text_font(s_date_label, font_cn_18, 0);

    lv_obj_t *hint_label = lv_label_create(center_col);
    lv_label_set_text(hint_label, "轻触屏幕以唤醒");
    lv_obj_set_style_text_color(hint_label, THEME_TEXT_SUB, 0);
    lv_obj_set_style_text_font(hint_label, font_cn_18, 0);

    lv_timer_create(standby_clock_timer_cb, 1000, NULL);
}
