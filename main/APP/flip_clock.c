/**
 ****************************************************************************************************
 * @file        flip_clock.c
 * @brief       Warm ambient Flip Clock with animated digit flipping.
 ****************************************************************************************************
 */

#include "flip_clock.h"

#include "esp_sntp.h"
#include "esp_wifi.h"
#include "menu.h"
#include "ui_fonts.h"
#include "ui_text.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FC_W                    800
#define FC_H                    480

#define COL_BG                  0x140F0B
#define COL_BG_GLOW             0x3A2214
#define COL_GLOW                0xB86C2B
#define COL_CARD                0x2A1B13
#define COL_CARD_TOP            0x3A2418
#define COL_CARD_BOTTOM         0x251710
#define COL_CARD_ACTIVE         0x42291A
#define COL_DIVIDER             0x6E482B
#define COL_DIVIDER_HOT         0xD99A4E
#define COL_EDGE                0xB8793B
#define COL_TEXT                0xF4E6D0
#define COL_TEXT_SOFT           0xC89B6A
#define COL_TEXT_DIM            0xA88B72
#define COL_OK                  0xD99A4E

#define DIGIT_COUNT               6
#define DIGIT_W                  84
#define DIGIT_H                 158
#define DIGIT_Y                 150
#define DIGIT_PAIR_GAP            8
#define DIGIT_COLON_GAP          26
#define DIGIT_START_X            98
#define DIGIT_RADIUS             14
#define SEG_COUNT                 7
#define SEG_H_X                  20
#define SEG_H_W                  44
#define SEG_H_H                   9
#define SEG_V_W                   9
#define SEG_V_H                  36
#define SEG_LEFT_X               13
#define SEG_RIGHT_X              62
#define SEG_TOP_Y                23
#define SEG_UPPER_Y              35
#define SEG_MID_Y                74
#define SEG_LOWER_Y              85
#define SEG_BOTTOM_Y            123

typedef struct {
    lv_obj_t *shadow;
    lv_obj_t *card;
    lv_obj_t *top;
    lv_obj_t *bottom;
    lv_obj_t *divider;
    lv_obj_t *hinge_left;
    lv_obj_t *hinge_right;
    lv_obj_t *top_flap;
    lv_obj_t *bottom_flap;
    lv_obj_t *stable_layer;
    lv_obj_t *next_layer;
    lv_obj_t *stable_seg[SEG_COUNT];
    lv_obj_t *next_seg[SEG_COUNT];
    char value;
    bool animating;
} flip_digit_t;

static lv_obj_t *g_scr;
static lv_obj_t *g_status_label;
static lv_obj_t *g_date_label;
static lv_obj_t *g_wifi_label;
static lv_obj_t *g_colon_top;
static lv_obj_t *g_colon_bottom;
static lv_obj_t *g_second_colon_top;
static lv_obj_t *g_second_colon_bottom;
static lv_timer_t *g_timer;
static flip_digit_t g_digits[DIGIT_COUNT];
static char g_last_digits[DIGIT_COUNT + 1] = "------";
static bool g_last_synced;
static bool g_active;

static const char *const g_weekdays[] = {
    "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
};

static void flip_clock_cleanup(void);

static void make_passive(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *panel(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                       lv_coord_t w, lv_coord_t h, uint32_t color,
                       lv_opa_t opa, lv_coord_t radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    if (!obj) return NULL;
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    make_passive(obj);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text,
                       const lv_font_t *font, uint32_t color)
{
    lv_obj_t *obj = lv_label_create(parent);
    if (!obj) return NULL;
    ui_text_set(obj, text ? text : "");
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(obj, 0, 0);
    make_passive(obj);
    return obj;
}

static void set_obj_y_cb(void *obj, int32_t v)
{
    if (!g_active) return;
    if (obj) lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)v);
}

static void set_opa_cb(void *obj, int32_t v)
{
    if (!g_active) return;
    if (obj) lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void set_divider_opa_cb(void *obj, int32_t v)
{
    if (!g_active) return;
    if (obj) lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void set_top_flap_h_cb(void *obj, int32_t v)
{
    if (!g_active || !obj) return;
    lv_coord_t h = (lv_coord_t)v;
    lv_obj_set_y((lv_obj_t *)obj, DIGIT_H / 2 - h);
    lv_obj_set_height((lv_obj_t *)obj, h);
}

static void set_bottom_flap_h_cb(void *obj, int32_t v)
{
    if (!g_active || !obj) return;
    lv_obj_set_y((lv_obj_t *)obj, DIGIT_H / 2 + 1);
    lv_obj_set_height((lv_obj_t *)obj, (lv_coord_t)v);
}

static void set_digit_segments(lv_obj_t *seg[SEG_COUNT], char value);

static void anim_ready_cb(lv_anim_t *a)
{
    if (!g_active) return;
    flip_digit_t *digit = (flip_digit_t *)lv_anim_get_user_data(a);
    if (!digit) return;

    set_digit_segments(digit->stable_seg, digit->value);
    if (digit->stable_layer) {
        lv_obj_set_y(digit->stable_layer, 0);
        lv_obj_set_style_opa(digit->stable_layer, LV_OPA_COVER, 0);
    }
    if (digit->next_layer) {
        lv_obj_set_y(digit->next_layer, -28);
        lv_obj_set_style_opa(digit->next_layer, LV_OPA_TRANSP, 0);
    }
    if (digit->top) lv_obj_set_style_bg_color(digit->top, lv_color_hex(COL_CARD_TOP), 0);
    if (digit->bottom) lv_obj_set_style_bg_color(digit->bottom, lv_color_hex(COL_CARD_BOTTOM), 0);
    if (digit->divider) {
        lv_obj_set_style_bg_color(digit->divider, lv_color_hex(COL_DIVIDER), 0);
        lv_obj_set_style_bg_opa(digit->divider, LV_OPA_80, 0);
    }
    if (digit->top_flap) {
        lv_obj_set_style_opa(digit->top_flap, LV_OPA_TRANSP, 0);
        set_top_flap_h_cb(digit->top_flap, 6);
    }
    if (digit->bottom_flap) {
        lv_obj_set_style_opa(digit->bottom_flap, LV_OPA_TRANSP, 0);
        set_bottom_flap_h_cb(digit->bottom_flap, 6);
    }
    digit->animating = false;
}

static void anim_start(lv_obj_t *obj, lv_anim_exec_xcb_t cb,
                       int32_t from, int32_t to, uint32_t delay,
                       uint32_t time_ms, lv_anim_path_cb_t path,
                       lv_anim_ready_cb_t ready_cb, void *user_data)
{
    if (!obj || !g_active) return;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_time(&a, time_ms);
    lv_anim_set_path_cb(&a, path ? path : lv_anim_path_ease_in_out);
    if (ready_cb) lv_anim_set_ready_cb(&a, ready_cb);
    if (user_data) lv_anim_set_user_data(&a, user_data);
    lv_anim_start(&a);
}

static uint8_t digit_mask(char value)
{
    enum {
        SEG_A = 1 << 0,
        SEG_B = 1 << 1,
        SEG_C = 1 << 2,
        SEG_D = 1 << 3,
        SEG_E = 1 << 4,
        SEG_F = 1 << 5,
        SEG_G = 1 << 6,
    };

    switch (value) {
        case '0': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F;
        case '1': return SEG_B | SEG_C;
        case '2': return SEG_A | SEG_B | SEG_G | SEG_E | SEG_D;
        case '3': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_G;
        case '4': return SEG_F | SEG_G | SEG_B | SEG_C;
        case '5': return SEG_A | SEG_F | SEG_G | SEG_C | SEG_D;
        case '6': return SEG_A | SEG_F | SEG_E | SEG_D | SEG_C | SEG_G;
        case '7': return SEG_A | SEG_B | SEG_C;
        case '8': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G;
        case '9': return SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G;
        case '-': return SEG_G;
        default: return 0;
    }
}

static void set_segment_on(lv_obj_t *seg, bool on)
{
    if (!seg) return;
    lv_obj_set_style_bg_opa(seg, on ? LV_OPA_COVER : LV_OPA_0, 0);
    lv_obj_set_style_shadow_opa(seg, on ? LV_OPA_30 : LV_OPA_0, 0);
}

static lv_obj_t *create_segment(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *seg = panel(parent, x, y, w, h, COL_TEXT, LV_OPA_COVER, h / 2);
    if (!seg) return NULL;
    lv_obj_set_style_bg_grad_color(seg, lv_color_hex(0xD9B98A), 0);
    lv_obj_set_style_bg_grad_dir(seg, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_shadow_width(seg, 8, 0);
    lv_obj_set_style_shadow_color(seg, lv_color_hex(0xE2A15C), 0);
    lv_obj_set_style_shadow_opa(seg, LV_OPA_30, 0);
    return seg;
}

static void create_digit_segments(lv_obj_t *layer, lv_obj_t *seg[SEG_COUNT])
{
    if (!layer || !seg) return;
    seg[0] = create_segment(layer, SEG_H_X, SEG_TOP_Y, SEG_H_W, SEG_H_H);
    seg[1] = create_segment(layer, SEG_RIGHT_X, SEG_UPPER_Y, SEG_V_W, SEG_V_H);
    seg[2] = create_segment(layer, SEG_RIGHT_X, SEG_LOWER_Y, SEG_V_W, SEG_V_H);
    seg[3] = create_segment(layer, SEG_H_X, SEG_BOTTOM_Y, SEG_H_W, SEG_H_H);
    seg[4] = create_segment(layer, SEG_LEFT_X, SEG_LOWER_Y, SEG_V_W, SEG_V_H);
    seg[5] = create_segment(layer, SEG_LEFT_X, SEG_UPPER_Y, SEG_V_W, SEG_V_H);
    seg[6] = create_segment(layer, SEG_H_X, SEG_MID_Y, SEG_H_W, SEG_H_H);
}

static void set_digit_segments(lv_obj_t *seg[SEG_COUNT], char value)
{
    uint8_t mask = digit_mask(value);
    for (size_t i = 0; i < SEG_COUNT; ++i) {
        set_segment_on(seg[i], (mask & (1U << i)) != 0);
    }
}

static lv_obj_t *create_digit_layer(lv_obj_t *parent)
{
    lv_obj_t *layer = panel(parent, 0, 0, DIGIT_W, DIGIT_H, COL_CARD, LV_OPA_TRANSP, 0);
    if (!layer) return NULL;
    lv_obj_set_style_opa(layer, LV_OPA_COVER, 0);
    return layer;
}

static void flip_clock_create_digit_card(lv_obj_t *parent, flip_digit_t *digit,
                                         lv_coord_t x, lv_coord_t y)
{
    if (!parent || !digit) return;
    memset(digit, 0, sizeof(*digit));
    digit->value = '-';

    digit->shadow = panel(parent, x + 6, y + 12, DIGIT_W, DIGIT_H, 0x070403, LV_OPA_60, DIGIT_RADIUS);
    digit->card = panel(parent, x, y, DIGIT_W, DIGIT_H, COL_CARD, LV_OPA_COVER, DIGIT_RADIUS);
    if (!digit->card) return;

    lv_obj_set_style_border_width(digit->card, 1, 0);
    lv_obj_set_style_border_color(digit->card, lv_color_hex(COL_EDGE), 0);
    lv_obj_set_style_border_opa(digit->card, LV_OPA_40, 0);
    lv_obj_set_style_shadow_width(digit->card, 18, 0);
    lv_obj_set_style_shadow_color(digit->card, lv_color_hex(0x050302), 0);
    lv_obj_set_style_shadow_opa(digit->card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_ofs_y(digit->card, 8, 0);

    digit->top = panel(digit->card, 0, 0, DIGIT_W, DIGIT_H / 2, COL_CARD_TOP, LV_OPA_COVER, DIGIT_RADIUS);
    digit->bottom = panel(digit->card, 0, DIGIT_H / 2, DIGIT_W, DIGIT_H / 2, COL_CARD_BOTTOM, LV_OPA_COVER, DIGIT_RADIUS);
    if (digit->top) {
        lv_obj_set_style_bg_grad_color(digit->top, lv_color_hex(0x24150E), 0);
        lv_obj_set_style_bg_grad_dir(digit->top, LV_GRAD_DIR_VER, 0);
    }
    if (digit->bottom) {
        lv_obj_set_style_bg_grad_color(digit->bottom, lv_color_hex(0x1A100C), 0);
        lv_obj_set_style_bg_grad_dir(digit->bottom, LV_GRAD_DIR_VER, 0);
    }

    digit->divider = panel(digit->card, 10, DIGIT_H / 2 - 1, DIGIT_W - 20, 2,
                           COL_DIVIDER, LV_OPA_80, 1);

    digit->stable_layer = create_digit_layer(digit->card);
    digit->next_layer = create_digit_layer(digit->card);
    create_digit_segments(digit->stable_layer, digit->stable_seg);
    create_digit_segments(digit->next_layer, digit->next_seg);
    set_digit_segments(digit->stable_seg, '-');
    set_digit_segments(digit->next_seg, '-');
    if (digit->next_layer) {
        lv_obj_set_style_opa(digit->next_layer, LV_OPA_TRANSP, 0);
        lv_obj_set_y(digit->next_layer, -28);
    }

    lv_obj_move_foreground(digit->divider);
    digit->hinge_left = panel(digit->card, 5, DIGIT_H / 2 - 4, 8, 8, COL_DIVIDER_HOT, LV_OPA_70, 4);
    digit->hinge_right = panel(digit->card, DIGIT_W - 13, DIGIT_H / 2 - 4, 8, 8,
                               COL_DIVIDER_HOT, LV_OPA_70, 4);
    if (digit->hinge_left) lv_obj_move_foreground(digit->hinge_left);
    if (digit->hinge_right) lv_obj_move_foreground(digit->hinge_right);

    digit->top_flap = panel(digit->card, 5, DIGIT_H / 2 - 6, DIGIT_W - 10, 6,
                            0x1B100C, LV_OPA_COVER, 8);
    digit->bottom_flap = panel(digit->card, 5, DIGIT_H / 2 + 1, DIGIT_W - 10, 6,
                               0x4A2C1B, LV_OPA_COVER, 8);
    if (digit->top_flap) {
        lv_obj_set_style_opa(digit->top_flap, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(digit->top_flap, 1, 0);
        lv_obj_set_style_border_color(digit->top_flap, lv_color_hex(COL_DIVIDER_HOT), 0);
        lv_obj_set_style_border_opa(digit->top_flap, LV_OPA_40, 0);
        lv_obj_move_foreground(digit->top_flap);
    }
    if (digit->bottom_flap) {
        lv_obj_set_style_opa(digit->bottom_flap, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(digit->bottom_flap, 1, 0);
        lv_obj_set_style_border_color(digit->bottom_flap, lv_color_hex(COL_DIVIDER_HOT), 0);
        lv_obj_set_style_border_opa(digit->bottom_flap, LV_OPA_40, 0);
        lv_obj_move_foreground(digit->bottom_flap);
    }
    lv_obj_move_foreground(digit->divider);
    if (digit->hinge_left) lv_obj_move_foreground(digit->hinge_left);
    if (digit->hinge_right) lv_obj_move_foreground(digit->hinge_right);
}

static void flip_clock_animate_digit(flip_digit_t *digit, char next, uint32_t stagger)
{
    if (!digit || !digit->card || digit->value == next || !g_active) return;

    lv_anim_del(digit->stable_layer, set_obj_y_cb);
    lv_anim_del(digit->stable_layer, set_opa_cb);
    lv_anim_del(digit->next_layer, set_obj_y_cb);
    lv_anim_del(digit->next_layer, set_opa_cb);
    lv_anim_del(digit->divider, set_divider_opa_cb);
    lv_anim_del(digit->top_flap, set_top_flap_h_cb);
    lv_anim_del(digit->top_flap, set_opa_cb);
    lv_anim_del(digit->bottom_flap, set_bottom_flap_h_cb);
    lv_anim_del(digit->bottom_flap, set_opa_cb);

    digit->value = next;
    digit->animating = true;

    set_digit_segments(digit->next_seg, next);
    if (digit->next_layer) {
        lv_obj_set_y(digit->next_layer, -30);
        lv_obj_set_style_opa(digit->next_layer, LV_OPA_TRANSP, 0);
    }
    if (digit->stable_layer) {
        lv_obj_set_y(digit->stable_layer, 0);
        lv_obj_set_style_opa(digit->stable_layer, LV_OPA_COVER, 0);
    }
    if (digit->top) lv_obj_set_style_bg_color(digit->top, lv_color_hex(COL_CARD_ACTIVE), 0);
    if (digit->bottom) lv_obj_set_style_bg_color(digit->bottom, lv_color_hex(0x1F120D), 0);
    if (digit->divider) {
        lv_obj_set_style_bg_color(digit->divider, lv_color_hex(COL_DIVIDER_HOT), 0);
        lv_obj_set_style_bg_opa(digit->divider, LV_OPA_COVER, 0);
    }
    if (digit->top_flap) {
        lv_obj_set_style_opa(digit->top_flap, LV_OPA_90, 0);
        set_top_flap_h_cb(digit->top_flap, DIGIT_H / 2 - 8);
        lv_obj_move_foreground(digit->top_flap);
    }
    if (digit->bottom_flap) {
        lv_obj_set_style_opa(digit->bottom_flap, LV_OPA_TRANSP, 0);
        set_bottom_flap_h_cb(digit->bottom_flap, 6);
        lv_obj_move_foreground(digit->bottom_flap);
    }
    lv_obj_move_foreground(digit->divider);
    if (digit->hinge_left) lv_obj_move_foreground(digit->hinge_left);
    if (digit->hinge_right) lv_obj_move_foreground(digit->hinge_right);

    uint32_t d = stagger;

    anim_start(digit->stable_layer, set_obj_y_cb, 0, 18,
               d, 145, lv_anim_path_ease_in_out, NULL, NULL);
    anim_start(digit->stable_layer, set_opa_cb, LV_OPA_COVER, LV_OPA_10,
               d, 145, lv_anim_path_ease_in_out, NULL, NULL);
    anim_start(digit->top_flap, set_top_flap_h_cb, DIGIT_H / 2 - 8, 7,
               d, 170, lv_anim_path_ease_in_out, NULL, NULL);
    anim_start(digit->top_flap, set_opa_cb, LV_OPA_90, LV_OPA_20,
               d + 70, 150, lv_anim_path_ease_in_out, NULL, NULL);
    anim_start(digit->next_layer, set_obj_y_cb, -42, 0,
               d + 150, 300, lv_anim_path_ease_in_out, NULL, NULL);
    anim_start(digit->next_layer, set_opa_cb, LV_OPA_TRANSP, LV_OPA_COVER,
               d + 150, 300, lv_anim_path_ease_in_out, anim_ready_cb, digit);
    anim_start(digit->bottom_flap, set_bottom_flap_h_cb, 6, DIGIT_H / 2 - 8,
               d + 135, 180, lv_anim_path_ease_in_out, NULL, NULL);
    anim_start(digit->bottom_flap, set_opa_cb, LV_OPA_80, LV_OPA_TRANSP,
               d + 235, 180, lv_anim_path_ease_in_out, NULL, NULL);
    anim_start(digit->divider, set_divider_opa_cb, LV_OPA_COVER, LV_OPA_80,
               d + 220, 230, lv_anim_path_ease_in_out, NULL, NULL);
}

static void flip_clock_set_digit(size_t index, char value, bool animate, uint32_t stagger)
{
    if (index >= DIGIT_COUNT) return;
    flip_digit_t *digit = &g_digits[index];
    if (digit->value == value) return;

    if (!animate) {
        digit->value = value;
        set_digit_segments(digit->stable_seg, value);
        set_digit_segments(digit->next_seg, value);
        if (digit->stable_layer) {
            lv_obj_set_y(digit->stable_layer, 0);
            lv_obj_set_style_opa(digit->stable_layer, LV_OPA_COVER, 0);
        }
        if (digit->next_layer) {
            lv_obj_set_y(digit->next_layer, -28);
            lv_obj_set_style_opa(digit->next_layer, LV_OPA_TRANSP, 0);
        }
        return;
    }

    flip_clock_animate_digit(digit, value, stagger);
}

static bool flip_clock_is_time_synced(const struct tm *tm_now)
{
    return tm_now && tm_now->tm_year >= 124;
}

static void flip_clock_update_date_status(const struct tm *tm_now, bool synced)
{
    if (!g_active) return;

    wifi_ap_record_t ap_info;
    bool wifi_connected = (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK);
    bool sntp_synced = synced || esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;

    if (g_status_label) {
        ui_text_set(g_status_label, synced ? "SYNCED" : "SYNCING");
        lv_obj_set_style_text_color(g_status_label, lv_color_hex(synced ? COL_OK : COL_TEXT_DIM), 0);
    }
    if (g_wifi_label) {
        ui_text_set(g_wifi_label,
                    wifi_connected ? (sntp_synced ? "WiFi Connected   SNTP Synced" :
                                       "WiFi Connected   SNTP Syncing") :
                                     "WiFi Offline   SNTP Waiting");
    }
    if (g_date_label) {
        char buf[64];
        if (synced && tm_now) {
            snprintf(buf, sizeof(buf), "%s %02d/%02d",
                     g_weekdays[tm_now->tm_wday],
                     tm_now->tm_mon + 1,
                     tm_now->tm_mday);
        } else {
            snprintf(buf, sizeof(buf), "Time syncing");
        }
        ui_text_set(g_date_label, buf);
    }
}

static void flip_clock_update_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!g_active || !g_scr) return;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    bool synced = flip_clock_is_time_synced(&tm_now);
    char digits[DIGIT_COUNT + 1];
    if (synced) {
        snprintf(digits, sizeof(digits), "%02d%02d%02d",
                 tm_now.tm_hour, tm_now.tm_min, tm_now.tm_sec);
    } else {
        snprintf(digits, sizeof(digits), "------");
    }

    bool animate = g_last_digits[0] != '\0' && g_last_synced == synced;
    for (size_t i = 0; i < DIGIT_COUNT; ++i) {
        if (digits[i] != g_last_digits[i]) {
            flip_clock_set_digit(i, digits[i], animate, (uint32_t)i * 40);
        }
    }
    memcpy(g_last_digits, digits, sizeof(g_last_digits));
    g_last_synced = synced;

    uint8_t phase = (uint8_t)(tm_now.tm_sec & 1);
    if (g_colon_top && g_colon_bottom && g_second_colon_top && g_second_colon_bottom) {
        lv_opa_t opa = phase ? LV_OPA_30 : LV_OPA_COVER;
        lv_obj_set_style_bg_opa(g_colon_top, opa, 0);
        lv_obj_set_style_bg_opa(g_colon_bottom, opa, 0);
        lv_obj_set_style_bg_opa(g_second_colon_top, opa, 0);
        lv_obj_set_style_bg_opa(g_second_colon_bottom, opa, 0);
    }

    flip_clock_update_date_status(&tm_now, synced);
}

static void flip_clock_back_event_cb(lv_event_t *e)
{
    (void)e;
    flip_clock_cleanup();
    menu_go_back();
}

static lv_obj_t *make_back_button(lv_obj_t *parent)
{
    lv_obj_t *btn = lv_btn_create(parent);
    if (!btn) return NULL;
    lv_obj_set_pos(btn, 28, 24);
    lv_obj_set_size(btn, 86, 38);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x2A1B13), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(COL_EDGE), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_30, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(btn, flip_clock_back_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *txt = label(btn, "Back", &lv_font_montserrat_16, COL_TEXT);
    if (txt) {
        lv_obj_center(txt);
        lv_obj_clear_flag(txt, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(txt, flip_clock_back_event_cb, LV_EVENT_CLICKED, NULL);
    }
    return btn;
}

static void flip_clock_create_ui(void)
{
    g_scr = lv_obj_create(NULL);
    if (!g_scr) return;

    lv_obj_set_size(g_scr, FC_W, FC_H);
    lv_obj_set_style_bg_color(g_scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_grad_color(g_scr, lv_color_hex(0x21130D), 0);
    lv_obj_set_style_bg_grad_dir(g_scr, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(g_scr, 0, 0);
    lv_obj_set_style_pad_all(g_scr, 0, 0);

    panel(g_scr, 175, 120, 450, 230, COL_BG_GLOW, LV_OPA_30, 54);
    panel(g_scr, 268, 70, 264, 82, COL_GLOW, LV_OPA_20, 38);
    panel(g_scr, 62, 370, 210, 58, 0x3B2115, LV_OPA_30, 26);
    panel(g_scr, 526, 366, 220, 62, 0x5A2D18, LV_OPA_30, 28);

    make_back_button(g_scr);

    lv_obj_t *title = label(g_scr, "FLIP CLOCK", &lv_font_montserrat_20, COL_TEXT_SOFT);
    if (title) {
        lv_obj_set_width(title, 260);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 32);
    }

    g_status_label = label(g_scr, "SYNCING", &lv_font_montserrat_14, COL_TEXT_DIM);
    if (g_status_label) {
        lv_obj_set_width(g_status_label, 150);
        lv_obj_set_style_text_align(g_status_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(g_status_label, 616, 36);
    }

    lv_coord_t x0 = DIGIT_START_X;
    lv_coord_t x1 = x0 + DIGIT_W + DIGIT_PAIR_GAP;
    lv_coord_t x2 = x1 + DIGIT_W + DIGIT_COLON_GAP;
    lv_coord_t x3 = x2 + DIGIT_W + DIGIT_PAIR_GAP;
    lv_coord_t x4 = x3 + DIGIT_W + DIGIT_COLON_GAP;
    lv_coord_t x5 = x4 + DIGIT_W + DIGIT_PAIR_GAP;

    flip_clock_create_digit_card(g_scr, &g_digits[0], x0, DIGIT_Y);
    flip_clock_create_digit_card(g_scr, &g_digits[1], x1, DIGIT_Y);
    flip_clock_create_digit_card(g_scr, &g_digits[2], x2, DIGIT_Y);
    flip_clock_create_digit_card(g_scr, &g_digits[3], x3, DIGIT_Y);
    flip_clock_create_digit_card(g_scr, &g_digits[4], x4, DIGIT_Y);
    flip_clock_create_digit_card(g_scr, &g_digits[5], x5, DIGIT_Y);

    lv_coord_t colon_x = x1 + DIGIT_W + (DIGIT_COLON_GAP / 2) - 4;
    lv_coord_t second_colon_x = x3 + DIGIT_W + (DIGIT_COLON_GAP / 2) - 4;
    g_colon_top = panel(g_scr, colon_x, DIGIT_Y + 55, 8, 8, COL_DIVIDER_HOT, LV_OPA_COVER, 4);
    g_colon_bottom = panel(g_scr, colon_x, DIGIT_Y + 94, 8, 8, COL_DIVIDER_HOT, LV_OPA_COVER, 4);
    g_second_colon_top = panel(g_scr, second_colon_x, DIGIT_Y + 55, 8, 8,
                               COL_DIVIDER_HOT, LV_OPA_COVER, 4);
    g_second_colon_bottom = panel(g_scr, second_colon_x, DIGIT_Y + 94, 8, 8,
                                  COL_DIVIDER_HOT, LV_OPA_COVER, 4);

    panel(g_scr, 154, 354, 492, 1, COL_DIVIDER, LV_OPA_50, 0);

    g_date_label = label(g_scr, "Time syncing", &lv_font_montserrat_20, COL_TEXT_SOFT);
    if (g_date_label) {
        lv_obj_set_width(g_date_label, 240);
        lv_obj_set_pos(g_date_label, 178, 386);
    }

    g_wifi_label = label(g_scr, "WiFi Offline   SNTP Waiting", &lv_font_montserrat_16, COL_TEXT_DIM);
    if (g_wifi_label) {
        lv_obj_set_width(g_wifi_label, 360);
        lv_obj_set_style_text_align(g_wifi_label, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_pos(g_wifi_label, 272, 390);
    }

    lv_scr_load(g_scr);
}

static void flip_clock_cleanup(void)
{
    g_active = false;

    if (g_timer) {
        lv_timer_del(g_timer);
        g_timer = NULL;
    }

    for (size_t i = 0; i < DIGIT_COUNT; ++i) {
        flip_digit_t *d = &g_digits[i];
        if (d->card) {
            lv_anim_del(d->stable_layer, NULL);
            lv_anim_del(d->next_layer, NULL);
            lv_anim_del(d->divider, NULL);
            lv_anim_del(d->top_flap, NULL);
            lv_anim_del(d->bottom_flap, NULL);
        }
    }

    if (g_scr) {
        lv_obj_del(g_scr);
        g_scr = NULL;
    }
}

void flip_clock_start(void)
{
    flip_clock_cleanup();
    memset(g_digits, 0, sizeof(g_digits));
    g_status_label = NULL;
    g_date_label = NULL;
    g_wifi_label = NULL;
    g_colon_top = NULL;
    g_colon_bottom = NULL;
    g_second_colon_top = NULL;
    g_second_colon_bottom = NULL;
    memcpy(g_last_digits, "------", sizeof(g_last_digits));
    g_last_synced = false;

    setenv("TZ", "CST-8", 1);
    tzset();

    g_active = true;
    flip_clock_create_ui();
    flip_clock_update_timer_cb(NULL);
    g_timer = lv_timer_create(flip_clock_update_timer_cb, 1000, NULL);
    if (g_timer) lv_timer_set_repeat_count(g_timer, -1);
}
