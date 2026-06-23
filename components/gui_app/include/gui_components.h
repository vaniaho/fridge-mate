#pragma once

#include "lvgl.h"
#include "gui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Standard white card with shadow and rounded corners.
 */
lv_obj_t *gui_card_create(lv_obj_t *parent);

/**
 * @brief Rounded pill tag.
 */
lv_obj_t *gui_pill_create(lv_obj_t *parent, const char *text,
                          lv_color_t bg_color, lv_color_t text_color);

/**
 * @brief Large circular icon button with gradient background.
 */
lv_obj_t *gui_icon_button_create(lv_obj_t *parent, const char *icon,
                                 const char *label_text,
                                 lv_color_t bg_start, lv_color_t bg_end,
                                 lv_event_cb_t cb, void *user_data);

/**
 * @brief Horizontal list item with icon + title + optional subtitle.
 */
lv_obj_t *gui_list_item_create(lv_obj_t *parent, const char *icon,
                               const char *title, const char *subtitle);

/**
 * @brief Centered empty-state placeholder with icon and text.
 */
lv_obj_t *gui_empty_state_create(lv_obj_t *parent, const char *icon,
                                 const char *title, const char *hint);

/**
 * @brief Simple modal popup with title, body and one confirm button.
 */
lv_obj_t *gui_modal_create(lv_obj_t *parent, const char *title,
                           const char *body, const char *btn_text,
                           lv_event_cb_t btn_cb);

/**
 * @brief Apply a fade-in + slight zoom entrance animation to an object.
 */
void gui_component_fade_in(lv_obj_t *obj, uint32_t delay_ms);

#ifdef __cplusplus
}
#endif
