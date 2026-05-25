#ifndef UI_FONTS_H
#define UI_FONTS_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const lv_font_t *UI_FONT_CN_16;
extern const lv_font_t *UI_FONT_CN_20;
extern const lv_font_t *UI_FONT_CN_24;
extern const lv_font_t *UI_FONT_CN_32;
extern const lv_font_t *UI_FONT_DIGIT_48;

void ui_fonts_init(void);
void ui_fonts_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_FONTS_H */
