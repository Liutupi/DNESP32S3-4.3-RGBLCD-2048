#include "ui_text.h"

#include <stdint.h>
#include <string.h>

#define UI_TEXT_INVALID_FILENAME "无法显示文件名"

bool ui_text_is_valid_utf8(const char *s)
{
    if (!s) {
        return false;
    }

    const uint8_t *p = (const uint8_t *)s;
    while (*p) {
        if (*p < 0x80) {
            p++;
        } else if ((*p & 0xE0) == 0xC0) {
            if ((p[1] & 0xC0) != 0x80 || *p < 0xC2) return false;
            p += 2;
        } else if ((*p & 0xF0) == 0xE0) {
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80) return false;
            if (*p == 0xE0 && p[1] < 0xA0) return false;
            if (*p == 0xED && p[1] >= 0xA0) return false;
            p += 3;
        } else if ((*p & 0xF8) == 0xF0) {
            if ((p[1] & 0xC0) != 0x80 || (p[2] & 0xC0) != 0x80 || (p[3] & 0xC0) != 0x80) return false;
            if (*p == 0xF0 && p[1] < 0x90) return false;
            if (*p > 0xF4 || (*p == 0xF4 && p[1] > 0x8F)) return false;
            p += 4;
        } else {
            return false;
        }
    }

    return true;
}

void ui_text_sanitize_utf8(char *s)
{
    if (!s || ui_text_is_valid_utf8(s)) {
        return;
    }

    size_t len = strlen(s);
    size_t fallback_len = strlen(UI_TEXT_INVALID_FILENAME);
    if (len >= fallback_len) {
        memcpy(s, UI_TEXT_INVALID_FILENAME, fallback_len + 1);
    } else {
        s[0] = '\0';
    }
}

void ui_text_set(lv_obj_t *label, const char *utf8_text)
{
    if (!label) {
        return;
    }

    lv_label_set_text(label, utf8_text ? utf8_text : "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
}

void ui_text_set_filename(lv_obj_t *label, const char *name)
{
    if (!label) {
        return;
    }

    if (ui_text_is_valid_utf8(name)) {
        ui_text_set(label, name);
    } else {
        ui_text_set(label, UI_TEXT_INVALID_FILENAME);
    }
}
