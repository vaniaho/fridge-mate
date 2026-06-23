#include "gui_icons.h"

static char s_icon_buf[4];

const char *gui_icon_codepoint(uint32_t cp) {
    s_icon_buf[0] = (char)(0xE0 | ((cp >> 12) & 0x0F));
    s_icon_buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
    s_icon_buf[2] = (char)(0x80 | (cp & 0x3F));
    s_icon_buf[3] = '\0';
    return s_icon_buf;
}
