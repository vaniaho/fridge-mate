#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

// ======== Colors ========
#define COLOR_BG         lv_color_hex(0xF4F6F8)
#define COLOR_CARD       lv_color_hex(0xFFFFFF)
#define COLOR_PRIMARY    lv_color_hex(0x4A90E2)
#define COLOR_TEXT_MAIN  lv_color_hex(0x333333)
#define COLOR_TEXT_SUB   lv_color_hex(0x888888)
#define COLOR_SUCCESS    lv_color_hex(0x2ECC71)
#define COLOR_WARNING    lv_color_hex(0xF1C40F)
#define COLOR_DANGER     lv_color_hex(0xE74C3C)
#define COLOR_DIVIDER    lv_color_hex(0xE0E0E0)
#define COLOR_TAG_BG     lv_color_hex(0xEBF2FC)

// ======== Fonts ========
// Assuming these are generated or loaded via FreeType
extern const lv_font_t * font_cn_16;
extern const lv_font_t * font_cn_18;
extern const lv_font_t * font_cn_24;
extern const lv_font_t * font_cn_36;
extern const lv_font_t * font_icon_24;

// ======== Global Styles ========
extern lv_style_t style_screen_bg;
extern lv_style_t style_card;
extern lv_style_t style_text_main;
extern lv_style_t style_text_sub;
extern lv_style_t style_text_title;

/**
 * @brief Initialize all global styles and fonts
 */
void gui_styles_init(void);

#ifdef __cplusplus
}
#endif
