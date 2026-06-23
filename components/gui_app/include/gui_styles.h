#pragma once

#include "gui_theme.h"

#ifdef __cplusplus
extern "C" {
#endif

// Legacy color aliases — kept for backward compatibility with existing app code
#define COLOR_BG         THEME_BG
#define COLOR_CARD       THEME_SURFACE
#define COLOR_PRIMARY    THEME_PRIMARY
#define COLOR_TEXT_MAIN  THEME_TEXT_MAIN
#define COLOR_TEXT_SUB   THEME_TEXT_SUB
#define COLOR_SUCCESS    THEME_SUCCESS
#define COLOR_WARNING    THEME_WARNING
#define COLOR_DANGER     THEME_DANGER
#define COLOR_DIVIDER    THEME_DIVIDER
#define COLOR_TAG_BG     THEME_PRIMARY_LIGHT

// Fonts and styles are declared in gui_theme.h
static inline void gui_styles_init(void) {
    gui_theme_init();
}

#ifdef __cplusplus
}
#endif
