/**
 ****************************************************************************************************
 * @file        boot_ui.c
 * @brief       Neon Game Deck boot screen and launcher for DNESP32S3
 ****************************************************************************************************
 */

#include "boot_ui.h"
#include "game2048.h"
#include "reaction_test.h"
#include "photoviewer.h"
#include "tomato_timer.h"
#include "lvgl.h"

#define BOOT_UI_W              800
#define BOOT_UI_HEIGHT         480
#define BOOT_UI_CARD_COUNT       4
#define BOOT_UI_CENTER_X       400
#define BOOT_UI_CARD_Y         132
#define BOOT_UI_CARD_W         444
#define BOOT_UI_CARD_H         230
#define BOOT_UI_BOOT_MS       1500
#define BOOT_UI_SWIPE_MIN       38
#define BOOT_UI_TAP_MAX         20
#define BOOT_UI_LAUNCH_X       206
#define BOOT_UI_LAUNCH_Y       298
#define BOOT_UI_LAUNCH_W       190
#define BOOT_UI_LAUNCH_H        46
#define BOOT_UI_LEFT_HIT_W     150
#define BOOT_UI_RIGHT_HIT_X    650

typedef void (*boot_ui_app_start_cb_t)(void);

typedef struct {
    const char *title;
    const char *kicker;
    const char *hint;
    lv_color_t color_a;
    lv_color_t color_b;
    boot_ui_app_start_cb_t start_cb;
} boot_ui_card_info_t;

static const boot_ui_card_info_t s_boot_ui_cards[BOOT_UI_CARD_COUNT] = {
    {"2048", "NUMBER CORE", "Merge tiles and chase 2048.",
     LV_COLOR_MAKE(0x20, 0xE7, 0xFF), LV_COLOR_MAKE(0x7B, 0x2C, 0xFF), game2048_start},
    {"Reaction Test", "REFLEX LAB", "Tap when the signal turns green.",
     LV_COLOR_MAKE(0x26, 0xFF, 0x90), LV_COLOR_MAKE(0x00, 0xA8, 0xFF), reaction_test_start},
    {"Photo Viewer", "SD GALLERY", "Browse photos from the SD card.",
     LV_COLOR_MAKE(0x6C, 0xFF, 0xE8), LV_COLOR_MAKE(0x00, 0x84, 0xFF), photoviewer_start},
    {"Tomato Glow", "FOCUS CLOCK", "Timer, weather, and desk glow.",
     LV_COLOR_MAKE(0xFF, 0x7A, 0x3D), LV_COLOR_MAKE(0xFF, 0x2E, 0x63), tomato_timer_start},
};

static lv_obj_t *s_boot_ui_splash_scr;
static lv_obj_t *s_boot_ui_deck_scr;
static lv_obj_t *s_boot_ui_card_layer;
static lv_obj_t *s_boot_ui_selected_card;
static lv_timer_t *s_boot_ui_splash_timer;
static lv_anim_t s_boot_ui_breathe_anim;
static int s_boot_ui_selected_index;
static lv_coord_t s_boot_ui_press_x;
static lv_coord_t s_boot_ui_press_y;
static uint32_t s_boot_ui_press_tick;
static bool s_boot_ui_gesture_done;

extern lv_obj_t *debug_label;

static void boot_ui_build_deck_screen(void);

static void boot_ui_set_obj_no_input(lv_obj_t *obj)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *boot_ui_make_label(lv_obj_t *parent, const char *text,
                                    const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    boot_ui_set_obj_no_input(label);
    return label;
}

static lv_obj_t *boot_ui_make_fixed_label(lv_obj_t *parent, const char *text,
                                          const lv_font_t *font, lv_color_t color,
                                          lv_coord_t x, lv_coord_t y, lv_coord_t w)
{
    lv_obj_t *label = boot_ui_make_label(parent, text, font, color);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    return label;
}

static lv_obj_t *boot_ui_make_panel(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                    lv_coord_t w, lv_coord_t h, lv_color_t bg,
                                    lv_opa_t opa, lv_coord_t radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, bg, 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    boot_ui_set_obj_no_input(obj);
    return obj;
}

static void boot_ui_add_background(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x030712), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(0x061A2D), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_VER, 0);

    boot_ui_make_panel(scr, 0, 0, BOOT_UI_W, 90, lv_color_hex(0x051124), LV_OPA_80, 0);
    boot_ui_make_panel(scr, 0, 404, BOOT_UI_W, 76, lv_color_hex(0x030813), LV_OPA_70, 0);
    boot_ui_make_panel(scr, 42, 106, 180, 2, lv_color_hex(0x1FF7FF), LV_OPA_80, 0);
    boot_ui_make_panel(scr, 578, 106, 180, 2, lv_color_hex(0xFF2E9D), LV_OPA_80, 0);
    boot_ui_make_panel(scr, 68, 404, 664, 1, lv_color_hex(0x123B61), LV_OPA_70, 0);
}

static void boot_ui_add_header(lv_obj_t *scr)
{
    boot_ui_make_fixed_label(scr, "DNESP32S3 GAME DECK", &lv_font_montserrat_28,
                             lv_color_hex(0xECFBFF), 42, 20, 380);
    boot_ui_make_fixed_label(scr, "Ready Player Tupi", &lv_font_montserrat_16,
                             lv_color_hex(0x6DF7FF), 44, 58, 240);

    lv_obj_t *state = boot_ui_make_panel(scr, 522, 28, 236, 32,
                                         lv_color_hex(0x071D30), LV_OPA_COVER, 8);
    lv_obj_set_style_border_width(state, 1, 0);
    lv_obj_set_style_border_color(state, lv_color_hex(0x29F3FF), 0);
    boot_ui_make_fixed_label(state, "LVGL v8   TOUCH   SD", &lv_font_montserrat_12,
                             lv_color_hex(0xDDFBFF), 18, 9, 200);
}

static void boot_ui_apply_breathe(void *obj, int32_t v)
{
    lv_obj_t *card = (lv_obj_t *)obj;
    lv_obj_set_style_border_opa(card, (lv_opa_t)v, 0);
    lv_obj_set_style_shadow_opa(card, (lv_opa_t)v, 0);
}

static void boot_ui_start_breathe(lv_obj_t *card)
{
    lv_anim_del(NULL, boot_ui_apply_breathe);
    lv_anim_init(&s_boot_ui_breathe_anim);
    lv_anim_set_var(&s_boot_ui_breathe_anim, card);
    lv_anim_set_exec_cb(&s_boot_ui_breathe_anim, boot_ui_apply_breathe);
    lv_anim_set_values(&s_boot_ui_breathe_anim, LV_OPA_50, LV_OPA_COVER);
    lv_anim_set_time(&s_boot_ui_breathe_anim, 760);
    lv_anim_set_playback_time(&s_boot_ui_breathe_anim, 760);
    lv_anim_set_repeat_count(&s_boot_ui_breathe_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&s_boot_ui_breathe_anim);
}

static void boot_ui_create_side_card(lv_obj_t *parent, int card_index, int rel)
{
    const boot_ui_card_info_t *info = &s_boot_ui_cards[card_index];
    lv_coord_t x = (rel < 0) ? 34 : 682;
    lv_obj_t *rail = boot_ui_make_panel(parent, x, 158, 84, 176,
                                        lv_color_hex(0x071B2D), LV_OPA_70, 14);
    lv_obj_set_style_border_width(rail, 1, 0);
    lv_obj_set_style_border_color(rail, info->color_a, 0);
    lv_obj_set_style_shadow_width(rail, 12, 0);
    lv_obj_set_style_shadow_color(rail, info->color_a, 0);

    lv_obj_t *arrow = boot_ui_make_label(rail, rel < 0 ? "<" : ">", &lv_font_montserrat_28,
                                         info->color_a);
    lv_obj_center(arrow);

    lv_obj_t *tap = boot_ui_make_label(rail, rel < 0 ? "PREV" : "NEXT", &lv_font_montserrat_12,
                                       lv_color_hex(0x8FB6D1));
    lv_obj_align(tap, LV_ALIGN_BOTTOM_MID, 0, -20);
}

static void boot_ui_create_selected_card(lv_obj_t *parent, int card_index)
{
    const boot_ui_card_info_t *info = &s_boot_ui_cards[card_index];
    lv_coord_t x = BOOT_UI_CENTER_X - (BOOT_UI_CARD_W / 2);

    lv_obj_t *glow = boot_ui_make_panel(parent, x - 8, BOOT_UI_CARD_Y - 8,
                                        BOOT_UI_CARD_W + 16, BOOT_UI_CARD_H + 16,
                                        info->color_b, LV_OPA_10, 18);
    lv_obj_set_style_shadow_width(glow, 20, 0);
    lv_obj_set_style_shadow_color(glow, info->color_b, 0);

    lv_obj_t *card = boot_ui_make_panel(parent, x, BOOT_UI_CARD_Y, BOOT_UI_CARD_W, BOOT_UI_CARD_H,
                                        lv_color_hex(0x081626), LV_OPA_COVER, 16);
    lv_obj_set_style_bg_grad_color(card, lv_color_hex(0x102B45), 0);
    lv_obj_set_style_bg_grad_dir(card, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_border_color(card, info->color_a, 0);
    lv_obj_set_style_shadow_width(card, 18, 0);
    lv_obj_set_style_shadow_color(card, info->color_a, 0);
    s_boot_ui_selected_card = card;

    boot_ui_make_panel(card, 0, 0, BOOT_UI_CARD_W, 5, info->color_a, LV_OPA_COVER, 0);
    boot_ui_make_panel(card, BOOT_UI_CARD_W - 84, 0, 84, BOOT_UI_CARD_H, info->color_b, LV_OPA_20, 0);

    boot_ui_make_fixed_label(card, info->kicker, &lv_font_montserrat_12, info->color_a, 30, 30, 250);
    boot_ui_make_fixed_label(card, info->title, &lv_font_montserrat_28,
                             lv_color_hex(0xF3FCFF), 30, 70, 310);
    boot_ui_make_fixed_label(card, info->hint, &lv_font_montserrat_16,
                             lv_color_hex(0xAFC7D9), 32, 122, 300);
    lv_obj_t *launch = boot_ui_make_panel(card, 28, 166, 190, 46,
                                          lv_color_hex(0x0A2437), LV_OPA_COVER, 10);
    lv_obj_set_style_border_width(launch, 2, 0);
    lv_obj_set_style_border_color(launch, info->color_a, 0);
    lv_obj_set_style_shadow_width(launch, 12, 0);
    lv_obj_set_style_shadow_color(launch, info->color_a, 0);
    lv_obj_t *launch_text = boot_ui_make_label(launch, "LAUNCH", &lv_font_montserrat_16,
                                               lv_color_hex(0xECFBFF));
    lv_obj_center(launch_text);

    char buf[8];
    lv_snprintf(buf, sizeof(buf), "%02d", card_index + 1);
    lv_obj_t *idx = boot_ui_make_label(card, buf, &lv_font_montserrat_48, lv_color_hex(0xFFFFFF));
    lv_obj_set_style_text_opa(idx, LV_OPA_70, 0);
    lv_obj_align(idx, LV_ALIGN_RIGHT_MID, -18, 0);

    boot_ui_start_breathe(card);
}

static void boot_ui_refresh_cards(void)
{
    if (!s_boot_ui_card_layer) return;

    lv_anim_del(NULL, boot_ui_apply_breathe);
    lv_obj_clean(s_boot_ui_card_layer);
    s_boot_ui_selected_card = NULL;

    int prev = (s_boot_ui_selected_index + BOOT_UI_CARD_COUNT - 1) % BOOT_UI_CARD_COUNT;
    int next = (s_boot_ui_selected_index + 1) % BOOT_UI_CARD_COUNT;

    boot_ui_create_side_card(s_boot_ui_card_layer, prev, -1);
    boot_ui_create_side_card(s_boot_ui_card_layer, next, 1);
    boot_ui_create_selected_card(s_boot_ui_card_layer, s_boot_ui_selected_index);

    lv_coord_t dot_x = 352;
    for (int i = 0; i < BOOT_UI_CARD_COUNT; ++i) {
        boot_ui_make_panel(s_boot_ui_card_layer, dot_x + i * 24, 382, 10, 10,
                           i == s_boot_ui_selected_index ? lv_color_hex(0x22E7FF) : lv_color_hex(0x24415B),
                           LV_OPA_COVER, LV_RADIUS_CIRCLE);
    }
}

static void boot_ui_select_next(void)
{
    s_boot_ui_selected_index = (s_boot_ui_selected_index + 1) % BOOT_UI_CARD_COUNT;
    boot_ui_refresh_cards();
}

static void boot_ui_select_prev(void)
{
    s_boot_ui_selected_index = (s_boot_ui_selected_index + BOOT_UI_CARD_COUNT - 1) % BOOT_UI_CARD_COUNT;
    boot_ui_refresh_cards();
}

static bool boot_ui_point_on_launch(lv_coord_t x, lv_coord_t y)
{
    return x >= BOOT_UI_LAUNCH_X && x <= BOOT_UI_LAUNCH_X + BOOT_UI_LAUNCH_W &&
           y >= BOOT_UI_LAUNCH_Y && y <= BOOT_UI_LAUNCH_Y + BOOT_UI_LAUNCH_H;
}

static bool boot_ui_point_on_prev(lv_coord_t x, lv_coord_t y)
{
    return x >= 0 && x <= BOOT_UI_LEFT_HIT_W && y >= 120 && y <= 374;
}

static bool boot_ui_point_on_next(lv_coord_t x, lv_coord_t y)
{
    return x >= BOOT_UI_RIGHT_HIT_X && x <= BOOT_UI_W && y >= 120 && y <= 374;
}

static void boot_ui_launch_selected(void)
{
    boot_ui_app_start_cb_t cb = s_boot_ui_cards[s_boot_ui_selected_index].start_cb;
    if (cb) cb();
}

static bool boot_ui_handle_swipe(lv_coord_t x, lv_coord_t y)
{
    lv_coord_t dx = x - s_boot_ui_press_x;
    lv_coord_t dy = y - s_boot_ui_press_y;

    if (LV_ABS(dx) < BOOT_UI_SWIPE_MIN) {
        return false;
    }
    if (LV_ABS(dx) <= LV_ABS(dy) + 16) {
        return false;
    }

    if (dx < 0) {
        boot_ui_select_next();
    } else {
        boot_ui_select_prev();
    }
    s_boot_ui_gesture_done = true;
    return true;
}

static void boot_ui_touch_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t point;

    if (!indev) return;

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &point);
        s_boot_ui_press_x = point.x;
        s_boot_ui_press_y = point.y;
        s_boot_ui_press_tick = lv_tick_get();
        s_boot_ui_gesture_done = false;
    } else if (code == LV_EVENT_PRESSING) {
        lv_indev_get_point(indev, &point);
        if (!s_boot_ui_gesture_done) {
            boot_ui_handle_swipe(point.x, point.y);
        }
    } else if (code == LV_EVENT_RELEASED) {
        lv_indev_get_point(indev, &point);
        lv_coord_t dx = point.x - s_boot_ui_press_x;
        lv_coord_t dy = point.y - s_boot_ui_press_y;
        uint32_t dt = lv_tick_elaps(s_boot_ui_press_tick);

        if (!s_boot_ui_gesture_done && boot_ui_handle_swipe(point.x, point.y)) {
            return;
        }

        if (s_boot_ui_gesture_done || LV_ABS(dx) > BOOT_UI_TAP_MAX || LV_ABS(dy) > BOOT_UI_TAP_MAX) {
            return;
        }

        if (dt < 80) {
            return;
        }

        if (boot_ui_point_on_launch(point.x, point.y) && boot_ui_point_on_launch(s_boot_ui_press_x, s_boot_ui_press_y)) {
            boot_ui_launch_selected();
        } else if (boot_ui_point_on_prev(point.x, point.y)) {
            boot_ui_select_prev();
        } else if (boot_ui_point_on_next(point.x, point.y)) {
            boot_ui_select_next();
        }
    }
}

static void boot_ui_add_touch_layer(lv_obj_t *scr)
{
    lv_obj_t *touch = lv_obj_create(scr);
    lv_obj_set_pos(touch, 0, 0);
    lv_obj_set_size(touch, BOOT_UI_W, BOOT_UI_HEIGHT);
    lv_obj_set_style_bg_opa(touch, LV_OPA_0, 0);
    lv_obj_set_style_border_width(touch, 0, 0);
    lv_obj_set_style_pad_all(touch, 0, 0);
    lv_obj_add_flag(touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(touch, boot_ui_touch_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(touch, boot_ui_touch_event_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(touch, boot_ui_touch_event_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_move_foreground(touch);
}

static void boot_ui_add_footer(lv_obj_t *scr)
{
    lv_obj_t *hint = boot_ui_make_label(scr, "Swipe or tap arrows to browse    |    Tap LAUNCH to enter",
                                        &lv_font_montserrat_14, lv_color_hex(0x8FB6D1));
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -30);

}

static void boot_ui_build_deck_screen(void)
{
    if (s_boot_ui_deck_scr) {
        return;
    }

    s_boot_ui_deck_scr = lv_obj_create(NULL);
    boot_ui_add_background(s_boot_ui_deck_scr);
    boot_ui_add_header(s_boot_ui_deck_scr);

    s_boot_ui_card_layer = lv_obj_create(s_boot_ui_deck_scr);
    lv_obj_set_pos(s_boot_ui_card_layer, 0, 0);
    lv_obj_set_size(s_boot_ui_card_layer, BOOT_UI_W, BOOT_UI_HEIGHT);
    lv_obj_set_style_bg_opa(s_boot_ui_card_layer, LV_OPA_0, 0);
    lv_obj_set_style_border_width(s_boot_ui_card_layer, 0, 0);
    lv_obj_set_style_pad_all(s_boot_ui_card_layer, 0, 0);
    boot_ui_set_obj_no_input(s_boot_ui_card_layer);

    boot_ui_refresh_cards();
    boot_ui_add_footer(s_boot_ui_deck_scr);
    boot_ui_add_touch_layer(s_boot_ui_deck_scr);
}

static void boot_ui_enter_deck_timer_cb(lv_timer_t *timer)
{
    s_boot_ui_splash_timer = NULL;
    lv_timer_del(timer);

    boot_ui_build_deck_screen();
    lv_scr_load(s_boot_ui_deck_scr);

    if (s_boot_ui_splash_scr) {
        lv_obj_del_async(s_boot_ui_splash_scr);
        s_boot_ui_splash_scr = NULL;
    }
}

static void boot_ui_bar_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_width((lv_obj_t *)obj, (lv_coord_t)v);
}

static void boot_ui_scan_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)v);
}

static void boot_ui_build_splash_screen(void)
{
    if (s_boot_ui_splash_scr) {
        lv_obj_del_async(s_boot_ui_splash_scr);
        s_boot_ui_splash_scr = NULL;
    }

    s_boot_ui_splash_scr = lv_obj_create(NULL);
    boot_ui_add_background(s_boot_ui_splash_scr);

    lv_obj_t *core = boot_ui_make_panel(s_boot_ui_splash_scr, 204, 108, 392, 238,
                                        lv_color_hex(0x071528), LV_OPA_90, 18);
    lv_obj_set_style_border_width(core, 2, 0);
    lv_obj_set_style_border_color(core, lv_color_hex(0x22E7FF), 0);
    lv_obj_set_style_shadow_width(core, 30, 0);
    lv_obj_set_style_shadow_color(core, lv_color_hex(0x00D9FF), 0);

    lv_obj_t *brand = boot_ui_make_label(core, "DNESP32S3", &lv_font_montserrat_28,
                                         lv_color_hex(0xF3FCFF));
    lv_obj_align(brand, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t *deck = boot_ui_make_label(core, "GAME DECK", &lv_font_montserrat_48,
                                        lv_color_hex(0x22E7FF));
    lv_obj_align(deck, LV_ALIGN_CENTER, 0, -12);

    lv_obj_t *sub = boot_ui_make_label(core, "BOOTING NEON INTERFACE", &lv_font_montserrat_14,
                                       lv_color_hex(0xFF75D8));
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 50);

    lv_obj_t *track = boot_ui_make_panel(core, 54, 188, 284, 8, lv_color_hex(0x102B45), LV_OPA_COVER, 4);
    lv_obj_t *bar = boot_ui_make_panel(track, 0, 0, 18, 8, lv_color_hex(0x26FF90), LV_OPA_COVER, 4);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, bar);
    lv_anim_set_exec_cb(&anim, boot_ui_bar_anim_cb);
    lv_anim_set_values(&anim, 18, 284);
    lv_anim_set_time(&anim, BOOT_UI_BOOT_MS);
    lv_anim_set_path_cb(&anim, lv_anim_path_ease_out);
    lv_anim_start(&anim);

    /* 副标题呼吸动画 */
    lv_anim_t ab;
    lv_anim_init(&ab);
    lv_anim_set_var(&ab, sub);
    lv_anim_set_exec_cb(&ab, (lv_anim_exec_xcb_t)lv_obj_set_style_opa);
    lv_anim_set_values(&ab, LV_OPA_30, LV_OPA_COVER);
    lv_anim_set_time(&ab, 800);
    lv_anim_set_playback_time(&ab, 800);
    lv_anim_set_repeat_count(&ab, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&ab, lv_anim_path_ease_in_out);
    lv_anim_start(&ab);

    /* 扫描线：单向从顶部扫到底部，营造系统初始化仪式感 */
    lv_obj_t *scan = boot_ui_make_panel(s_boot_ui_splash_scr, 0, 0, BOOT_UI_W, 2,
                                        lv_color_hex(0x22E7FF), LV_OPA_70, 0);
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, scan);
    lv_anim_set_exec_cb(&anim, boot_ui_scan_anim_cb);
    lv_anim_set_values(&anim, 0, BOOT_UI_HEIGHT);
    lv_anim_set_time(&anim, 900);
    lv_anim_set_delay(&anim, 0);
    lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&anim, lv_anim_path_linear);
    lv_anim_start(&anim);
}

void boot_ui_return_home(void)
{
    boot_ui_build_deck_screen();
    lv_obj_t *old_scr = lv_scr_act();
    lv_scr_load(s_boot_ui_deck_scr);

    if (old_scr && old_scr != s_boot_ui_deck_scr && old_scr != s_boot_ui_splash_scr) {
        lv_obj_del(old_scr);
    }
}

void boot_ui_start(void)
{
    if (debug_label) {
        lv_obj_add_flag(debug_label, LV_OBJ_FLAG_HIDDEN);
    }

    s_boot_ui_selected_index = 0;
    boot_ui_build_splash_screen();
    lv_scr_load(s_boot_ui_splash_scr);

    if (s_boot_ui_splash_timer) {
        lv_timer_del(s_boot_ui_splash_timer);
    }
    s_boot_ui_splash_timer = lv_timer_create(boot_ui_enter_deck_timer_cb, BOOT_UI_BOOT_MS, NULL);
}
