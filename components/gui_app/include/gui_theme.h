#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *  Smart Fridge — Unified Light Theme Tokens
 *  Style: light background + brand blue-green gradient accent
 * ============================================================ */

/* ---------- Colors ---------- */
#define THEME_BG                lv_color_hex(0xF4F6F8)
#define THEME_SURFACE           lv_color_hex(0xFFFFFF)
#define THEME_PRIMARY           lv_color_hex(0x4A90E2)
#define THEME_PRIMARY_LIGHT     lv_color_hex(0xEBF2FC)
#define THEME_ACCENT            lv_color_hex(0x2ECC71)
#define THEME_ACCENT_LIGHT      lv_color_hex(0xE6FAF0)

#define THEME_TEXT_MAIN         lv_color_hex(0x333333)
#define THEME_TEXT_SUB          lv_color_hex(0x888888)
#define THEME_TEXT_ON_PRIMARY   lv_color_white()
#define THEME_TEXT_ON_ACCENT    lv_color_white()

#define THEME_SUCCESS           lv_color_hex(0x2ECC71)
#define THEME_WARNING           lv_color_hex(0xFF9800)
#define THEME_DANGER            lv_color_hex(0xE74C3C)
#define THEME_INFO              lv_color_hex(0x4A90E2)

#define THEME_DIVIDER           lv_color_hex(0xE8ECF0)
#define THEME_SHADOW            lv_color_hex(0x4A90E2)

/* ---------- Spacing ---------- */
#define THEME_SPACE_XS          4
#define THEME_SPACE_S           8
#define THEME_SPACE_M           16
#define THEME_SPACE_L           24
#define THEME_SPACE_XL          32

/* ---------- Radius ---------- */
#define THEME_RADIUS_S          8
#define THEME_RADIUS_M          16
#define THEME_RADIUS_L          24
#define THEME_RADIUS_PILL       100

/* ---------- Fonts ---------- */
extern const lv_font_t *font_cn_16;
extern const lv_font_t *font_cn_18;
extern const lv_font_t *font_cn_24;
extern const lv_font_t *font_cn_36;
extern const lv_font_t *font_icon_24;
extern const lv_font_t *font_icon_36;

/* ---------- Global reusable styles ---------- */
extern lv_style_t style_screen_bg;
extern lv_style_t style_card;
extern lv_style_t style_text_main;
extern lv_style_t style_text_sub;
extern lv_style_t style_text_title;
extern lv_style_t style_text_caption;

/**
 * @brief Initialize theme styles and load fonts.
 */
void gui_theme_init(void);

/**
 * @brief Apply a horizontal gradient background to an object.
 */
void theme_apply_gradient_bg(lv_obj_t *obj, lv_color_t start, lv_color_t end);

/**
 * @brief Apply simple press animation (scale 0.96 + opacity) to a clickable object.
 */
void theme_apply_press_effect(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif
