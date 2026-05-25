#ifndef UI_TEXT_H
#define UI_TEXT_H

#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_text_set(lv_obj_t *label, const char *utf8_text);
void ui_text_set_filename(lv_obj_t *label, const char *name);
bool ui_text_is_valid_utf8(const char *s);
void ui_text_sanitize_utf8(char *s);

#ifdef __cplusplus
}
#endif

#endif /* UI_TEXT_H */
