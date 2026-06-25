#include "gui_components.h"
#include "gui_icons.h"

lv_obj_t *gui_card_create(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_add_style(card, &style_card, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t *gui_pill_create(lv_obj_t *parent, const char *text,
                          lv_color_t bg_color, lv_color_t text_color) {
    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_set_size(pill, LV_SIZE_CONTENT, 24);
    lv_obj_set_style_bg_color(pill, bg_color, 0);
    lv_obj_set_style_radius(pill, THEME_RADIUS_PILL, 0);
    lv_obj_set_style_pad_left(pill, 10, 0);
    lv_obj_set_style_pad_right(pill, 10, 0);
    lv_obj_set_style_pad_top(pill, 2, 0);
    lv_obj_set_style_pad_bottom(pill, 2, 0);
    lv_obj_set_style_border_width(pill, 0, 0);
    lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(pill);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font_cn_16, 0);
    lv_obj_set_style_text_color(label, text_color, 0);
    lv_obj_center(label);

    return pill;
}

lv_obj_t *gui_icon_button_create(lv_obj_t *parent, const char *icon,
                                 const char *label_text,
                                 lv_color_t bg_start, lv_color_t bg_end,
                                 lv_event_cb_t cb, void *user_data) {
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 130, 130);
    lv_obj_set_style_bg_color(btn, bg_start, 0);
    lv_obj_set_style_bg_grad_color(btn, bg_end, 0);
    lv_obj_set_style_bg_grad_dir(btn, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_radius(btn, THEME_RADIUS_M, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 6, 0);
    lv_obj_set_style_shadow_ofs_y(btn, 3, 0);
    lv_obj_set_style_shadow_opa(btn, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(btn, bg_start, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    theme_apply_press_effect(btn);

    lv_obj_t *icon_label = lv_label_create(btn);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, font_icon_36, 0);
    lv_obj_set_style_text_color(icon_label, lv_color_white(), 0);
    lv_obj_align(icon_label, LV_ALIGN_CENTER, 0, -18);

    lv_obj_t *text_label = lv_label_create(btn);
    lv_label_set_text(text_label, label_text);
    lv_obj_set_style_text_font(text_label, font_cn_18, 0);
    lv_obj_set_style_text_color(text_label, lv_color_white(), 0);
    lv_obj_align(text_label, LV_ALIGN_CENTER, 0, 28);

    return btn;
}

lv_obj_t *gui_list_item_create(lv_obj_t *parent, const char *icon,
                               const char *title, const char *subtitle) {
    lv_obj_t *item = lv_obj_create(parent);
    lv_obj_set_size(item, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(item, THEME_SURFACE, 0);
    lv_obj_set_style_radius(item, THEME_RADIUS_S, 0);
    lv_obj_set_style_pad_all(item, 12, 0);
    lv_obj_set_style_pad_row(item, 4, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_set_flex_flow(item, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(item, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(item, 12, 0);

    if (icon) {
        lv_obj_t *icon_label = lv_label_create(item);
        lv_label_set_text(icon_label, icon);
        lv_obj_set_style_text_font(icon_label, font_icon_24, 0);
        lv_obj_set_style_text_color(icon_label, THEME_PRIMARY, 0);
    }

    lv_obj_t *text_col = lv_obj_create(item);
    lv_obj_set_flex_grow(text_col, 1);
    lv_obj_set_style_bg_opa(text_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(text_col, 0, 0);
    lv_obj_set_style_pad_all(text_col, 0, 0);
    lv_obj_set_flex_flow(text_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(text_col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = lv_label_create(text_col);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, font_cn_18, 0);
    lv_obj_set_style_text_color(title_label, THEME_TEXT_MAIN, 0);

    if (subtitle) {
        lv_obj_t *sub_label = lv_label_create(text_col);
        lv_label_set_text(sub_label, subtitle);
        lv_obj_set_style_text_font(sub_label, font_cn_16, 0);
        lv_obj_set_style_text_color(sub_label, THEME_TEXT_SUB, 0);
    }

    return item;
}

lv_obj_t *gui_empty_state_create(lv_obj_t *parent, const char *icon,
                                 const char *title, const char *hint) {
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cont, 12, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon_label = lv_label_create(cont);
    lv_label_set_text(icon_label, icon ? icon : ICON_INFO);
    lv_obj_set_style_text_font(icon_label, font_icon_36, 0);
    lv_obj_set_style_text_color(icon_label, THEME_PRIMARY, 0);

    lv_obj_t *title_label = lv_label_create(cont);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, font_cn_24, 0);
    lv_obj_set_style_text_color(title_label, THEME_TEXT_MAIN, 0);

    lv_obj_t *hint_label = lv_label_create(cont);
    lv_label_set_text(hint_label, hint);
    lv_obj_set_style_text_font(hint_label, font_cn_16, 0);
    lv_obj_set_style_text_color(hint_label, THEME_TEXT_SUB, 0);

    return cont;
}

lv_obj_t *gui_modal_create(lv_obj_t *parent, const char *title,
                           const char *body, const char *btn_text,
                           lv_event_cb_t btn_cb) {
    lv_obj_t *modal = lv_obj_create(parent);
    lv_obj_set_size(modal, 420, LV_SIZE_CONTENT);
    lv_obj_center(modal);
    lv_obj_add_style(modal, &style_card, 0);
    lv_obj_set_style_pad_all(modal, THEME_SPACE_L, 0);
    lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(modal, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(modal, THEME_SPACE_M, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = lv_label_create(modal);
    lv_label_set_text(title_label, title);
    lv_obj_set_style_text_font(title_label, font_cn_24, 0);
    lv_obj_set_style_text_color(title_label, THEME_TEXT_MAIN, 0);

    lv_obj_t *body_label = lv_label_create(modal);
    lv_label_set_text(body_label, body);
    lv_obj_set_style_text_font(body_label, font_cn_18, 0);
    lv_obj_set_style_text_color(body_label, THEME_TEXT_SUB, 0);
    lv_obj_set_width(body_label, 360);
    lv_label_set_long_mode(body_label, LV_LABEL_LONG_WRAP);

    lv_obj_t *btn = lv_btn_create(modal);
    lv_obj_set_size(btn, 160, 44);
    lv_obj_set_style_bg_color(btn, THEME_PRIMARY, 0);
    lv_obj_set_style_radius(btn, THEME_RADIUS_PILL, 0);
    if (btn_cb) {
        lv_obj_add_event_cb(btn, btn_cb, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, btn_text ? btn_text : "知道了");
    lv_obj_set_style_text_font(btn_label, font_cn_18, 0);
    lv_obj_set_style_text_color(btn_label, lv_color_white(), 0);
    lv_obj_center(btn_label);

    return modal;
}

static void fade_anim_cb(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, v, 0);
    int32_t zoom = 250 + (v * 6) / 256; // 250 -> 256
    lv_obj_set_style_transform_zoom((lv_obj_t *)obj, zoom, 0);
}

void gui_component_fade_in(lv_obj_t *obj, uint32_t delay_ms) {
    lv_obj_set_style_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_transform_zoom(obj, 250, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_exec_cb(&a, fade_anim_cb);
    lv_anim_set_time(&a, 250);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_start(&a);
}
