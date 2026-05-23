/**
 ****************************************************************************************************
 * @file        menu.c
 * @brief       WarmOS Launch home screen for DNESP32S3
 ****************************************************************************************************
 */

#include "menu.h"
#include "boot_ui.h"
#include "game2048.h"
#include "reaction_test.h"
#include "bird_launcher.h"
#include "photoviewer.h"
#include "tomato_timer.h"
#include "racing_game.h"
#include "flip_clock.h"
#include "radio_app.h"
#include <stdio.h>

#define SCREEN_W        800
#define SCREEN_H        480

#define COL_BG          0x140D0A
#define COL_BG_2        0x24140E
#define COL_GLOW        0xC46A2A
#define COL_GLOW_2      0x8E3B22
#define COL_CARD        0x2A1A14
#define COL_CARD_2      0x362219
#define COL_TEXT        0xFFF2DC
#define COL_TEXT_SOFT   0xB98A68
#define COL_ACCENT      0xE58A3A
#define COL_ACCENT_2    0xD95E3F

#define HEADER_X         48
#define HEADER_Y         32

#define PHOTO_X          70
#define PHOTO_Y         135
#define PHOTO_W         340
#define PHOTO_H         190

#define FLIP_X          450
#define FLIP_Y          135
#define FLIP_W          280
#define FLIP_H          190

#define DOCK_Y          360
#define DOCK_W          106
#define DOCK_H           72

#define DOCK_2048_X      31
#define DOCK_BIRD_X     154
#define DOCK_REACT_X    277
#define DOCK_TOMATO_X   400
#define DOCK_RACING_X   523
#define DOCK_RADIO_X    646

#define TAP_MOVE_MAX     30

typedef enum {
    APP_NONE = 0,
    APP_2048,
    APP_REACTION,
    APP_BIRD,
    APP_PHOTO,
    APP_TOMATO,
    APP_RACING,
    APP_FLIP_CLOCK,
    APP_RADIO,
} menu_app_t;

static lv_obj_t *g_menu_scr = NULL;
static lv_obj_t *g_menu_root = NULL;
static lv_obj_t *g_touch_layer = NULL;
static lv_obj_t *g_photo_card = NULL;
static lv_obj_t *g_2048_card = NULL;
static lv_obj_t *g_bird_card = NULL;
static lv_obj_t *g_react_card = NULL;
static lv_obj_t *g_tomato_card = NULL;
static lv_obj_t *g_racing_card = NULL;
static lv_obj_t *g_flip_card = NULL;
static lv_obj_t *g_radio_card = NULL;
static lv_coord_t g_press_x = 0;
static lv_coord_t g_press_y = 0;
static menu_app_t g_pressed_app = APP_NONE;
static bool g_menu_ready = false;

extern lv_obj_t *debug_label;

static void make_passive(lv_obj_t *obj)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *create_panel(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                              lv_coord_t w, lv_coord_t h, uint32_t color,
                              lv_opa_t opa, lv_coord_t radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
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

static lv_obj_t *create_label(lv_obj_t *parent, const char *text,
                              const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_style_text_letter_space(label, 0, 0);
    make_passive(label);
    return label;
}

static void anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)v);
}

static void fade_in_obj(lv_obj_t *obj, uint32_t delay, uint32_t time)
{
    (void)obj;
    (void)delay;
    (void)time;
}

static void fade_slide_in_obj(lv_obj_t *obj, lv_coord_t start_y, lv_coord_t end_y,
                              uint32_t delay, uint32_t time)
{
    (void)start_y;
    (void)delay;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, end_y + 10, end_y);
    lv_anim_set_delay(&a, 150);
    lv_anim_set_time(&a, time > 260 ? 260 : time);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void menu_ready_timer_cb(lv_timer_t *timer)
{
    g_menu_ready = true;
    lv_timer_del(timer);
}

static void create_background(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(COL_BG), 0);

    create_panel(parent, 210, 150, 390, 210, COL_BG_2, LV_OPA_30, 18);

    create_panel(parent, 560, -150, 300, 210, COL_GLOW, LV_OPA_20, 42);

    create_panel(parent, -120, 330, 280, 130, COL_GLOW_2, LV_OPA_20, 42);
}

static lv_obj_t *create_header(lv_obj_t *parent)
{
    lv_obj_t *header = create_panel(parent, 0, 0, SCREEN_W, 116, COL_BG, LV_OPA_TRANSP, 0);

    lv_obj_t *title = create_label(header, "WarmOS", &lv_font_montserrat_48, COL_TEXT);
    lv_obj_set_pos(title, HEADER_X, HEADER_Y);

    lv_obj_t *sub = create_label(header, "Good evening, Tupi", &lv_font_montserrat_16, COL_TEXT_SOFT);
    lv_obj_set_pos(sub, 50, 82);

    lv_obj_t *state = create_label(header, "800x480 . TOUCH . SD", &lv_font_montserrat_14, COL_TEXT_SOFT);
    lv_obj_set_pos(state, 560, 48);

    return header;
}

static lv_obj_t *create_main_photo_card(lv_obj_t *parent)
{
    lv_obj_t *card = create_panel(parent, PHOTO_X, PHOTO_Y, PHOTO_W, PHOTO_H, COL_CARD, LV_OPA_COVER, 32);
    g_photo_card = card;

    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_30, 0);

    create_panel(card, 224, -34, 118, 118, COL_GLOW, LV_OPA_20, 36);

    lv_obj_t *title = create_label(card, "PHOTO", &lv_font_montserrat_28, COL_TEXT);
    lv_obj_set_pos(title, 34, 48);

    lv_obj_t *sub = create_label(card, "Family album from SD card", &lv_font_montserrat_16, COL_TEXT_SOFT);
    lv_obj_set_pos(sub, 36, 94);

    lv_obj_t *hint = create_label(card, "Tap to open", &lv_font_montserrat_14, COL_ACCENT);
    lv_obj_set_pos(hint, 36, 134);

    return card;
}

static lv_obj_t *create_flip_clock_card(lv_obj_t *parent)
{
    lv_obj_t *card = create_panel(parent, FLIP_X, FLIP_Y, FLIP_W, FLIP_H, COL_CARD, LV_OPA_COVER, 32);
    g_flip_card = card;

    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_ACCENT_2), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_30, 0);

    create_panel(card, 186, -26, 94, 94, COL_ACCENT_2, LV_OPA_20, 34);
    create_panel(card, 28, 30, 10, 10, COL_ACCENT, LV_OPA_COVER, 5);

    lv_obj_t *title = create_label(card, "Flip Clock", &lv_font_montserrat_28, COL_TEXT);
    lv_obj_set_pos(title, 34, 46);

    lv_obj_t *sub = create_label(card, "Warm ambient desk clock", &lv_font_montserrat_16, COL_TEXT_SOFT);
    lv_obj_set_pos(sub, 36, 94);

    lv_obj_t *hint = create_label(card, "Tap to open", &lv_font_montserrat_14, COL_ACCENT);
    lv_obj_set_pos(hint, 36, 138);

    return card;
}

static lv_obj_t *create_dock_card(lv_obj_t *parent, lv_coord_t x, const char *title,
                                  const char *subtitle, uint32_t accent)
{
    lv_obj_t *card = create_panel(parent, x, DOCK_Y, DOCK_W, DOCK_H, COL_CARD, LV_OPA_80, 24);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x6B3A25), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_30, 0);

    create_panel(card, 18, 15, 8, 8, accent, LV_OPA_COVER, 4);

    lv_obj_t *title_label = create_label(card, title, &lv_font_montserrat_16, COL_TEXT);
    lv_obj_set_pos(title_label, 18, 27);
    lv_obj_set_width(title_label, DOCK_W - 28);
    lv_label_set_long_mode(title_label, LV_LABEL_LONG_CLIP);

    lv_obj_t *sub_label = create_label(card, subtitle, &lv_font_montserrat_12, COL_TEXT_SOFT);
    lv_obj_set_pos(sub_label, 20, 52);
    lv_obj_set_width(sub_label, DOCK_W - 32);
    lv_label_set_long_mode(sub_label, LV_LABEL_LONG_CLIP);

    return card;
}

static void create_dock(lv_obj_t *parent)
{
    g_2048_card = create_dock_card(parent, DOCK_2048_X, "2048", "Puzzle", COL_ACCENT);
    g_bird_card = create_dock_card(parent, DOCK_BIRD_X, "Bird", "Launch", COL_ACCENT);
    g_react_card = create_dock_card(parent, DOCK_REACT_X, "Reaction", "Tap test", COL_ACCENT_2);
    g_tomato_card = create_dock_card(parent, DOCK_TOMATO_X, "Tomato", "25:00", COL_ACCENT_2);
    g_racing_card = create_dock_card(parent, DOCK_RACING_X, "Racing", "Road rush", COL_ACCENT);
    g_radio_card = create_dock_card(parent, DOCK_RADIO_X, "Radio", "Streams", COL_ACCENT_2);
}

static lv_obj_t *create_footer(lv_obj_t *parent)
{
    lv_obj_t *footer = create_panel(parent, 0, 432, SCREEN_W, 48, COL_BG, LV_OPA_TRANSP, 0);

    lv_obj_t *left = create_label(footer, "Slide gently into play", &lv_font_montserrat_12, COL_TEXT_SOFT);
    lv_obj_set_pos(left, 48, 10);

    lv_obj_t *right = create_label(footer, "DNESP32S3 . Made by Tupi", &lv_font_montserrat_12, COL_TEXT_SOFT);
    lv_obj_set_pos(right, 584, 10);

    return footer;
}

static menu_app_t hit_test(lv_coord_t x, lv_coord_t y)
{
    if (x >= PHOTO_X && x <= PHOTO_X + PHOTO_W &&
        y >= PHOTO_Y && y <= PHOTO_Y + PHOTO_H) {
        return APP_PHOTO;
    }

    if (x >= FLIP_X && x <= FLIP_X + FLIP_W &&
        y >= FLIP_Y && y <= FLIP_Y + FLIP_H) {
        return APP_FLIP_CLOCK;
    }

    if (x >= DOCK_2048_X && x <= DOCK_2048_X + DOCK_W &&
        y >= DOCK_Y && y <= DOCK_Y + DOCK_H) {
        return APP_2048;
    }

    if (x >= DOCK_BIRD_X && x <= DOCK_BIRD_X + DOCK_W &&
        y >= DOCK_Y && y <= DOCK_Y + DOCK_H) {
        return APP_BIRD;
    }

    if (x >= DOCK_REACT_X && x <= DOCK_REACT_X + DOCK_W &&
        y >= DOCK_Y && y <= DOCK_Y + DOCK_H) {
        return APP_REACTION;
    }

    if (x >= DOCK_TOMATO_X && x <= DOCK_TOMATO_X + DOCK_W &&
        y >= DOCK_Y && y <= DOCK_Y + DOCK_H) {
        return APP_TOMATO;
    }

    if (x >= DOCK_RACING_X && x <= DOCK_RACING_X + DOCK_W &&
        y >= DOCK_Y && y <= DOCK_Y + DOCK_H) {
        return APP_RACING;
    }

    if (x >= DOCK_RADIO_X && x <= DOCK_RADIO_X + DOCK_W &&
        y >= DOCK_Y && y <= DOCK_Y + DOCK_H) {
        return APP_RADIO;
    }

    return APP_NONE;
}

static lv_obj_t *card_for_app(menu_app_t app)
{
    switch (app) {
        case APP_PHOTO: return g_photo_card;
        case APP_2048: return g_2048_card;
        case APP_BIRD: return g_bird_card;
        case APP_REACTION: return g_react_card;
        case APP_TOMATO: return g_tomato_card;
        case APP_RACING: return g_racing_card;
        case APP_FLIP_CLOCK: return g_flip_card;
        case APP_RADIO: return g_radio_card;
        default: return NULL;
    }
}

static void set_card_feedback(menu_app_t app, bool active)
{
    lv_obj_t *card = card_for_app(app);
    if (!card) return;

    lv_obj_set_style_bg_color(card, lv_color_hex(active ? COL_CARD_2 : COL_CARD), 0);
    lv_obj_set_style_border_opa(card, active ? LV_OPA_60 : LV_OPA_30, 0);
}

static void launch_app(menu_app_t app)
{
    switch (app) {
        case APP_PHOTO:
            photoviewer_start();
            break;
        case APP_2048:
            game2048_start();
            break;
        case APP_BIRD:
            bird_launcher_start();
            break;
        case APP_REACTION:
            reaction_test_start();
            break;
        case APP_TOMATO:
            tomato_timer_start();
            break;
        case APP_RACING:
            racing_game_start();
            break;
        case APP_FLIP_CLOCK:
            flip_clock_start();
            break;
        case APP_RADIO:
            radio_app_start();
            break;
        default:
            break;
    }
}

static void touch_layer_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t p;

    if (!g_menu_ready || !indev) return;

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &p);
        g_press_x = p.x;
        g_press_y = p.y;
        g_pressed_app = hit_test(p.x, p.y);
        set_card_feedback(g_pressed_app, true);
    } else if (code == LV_EVENT_RELEASED) {
        lv_indev_get_point(indev, &p);
        lv_coord_t dx = p.x - g_press_x;
        lv_coord_t dy = p.y - g_press_y;
        menu_app_t app = hit_test(p.x, p.y);

        set_card_feedback(g_pressed_app, false);
        if (LV_ABS(dx) > TAP_MOVE_MAX || LV_ABS(dy) > TAP_MOVE_MAX) {
            g_pressed_app = APP_NONE;
            return;
        }

        if (app == g_pressed_app) {
            launch_app(app);
        }
        g_pressed_app = APP_NONE;
    }
}

static void create_touch_layer(lv_obj_t *parent)
{
    g_touch_layer = lv_obj_create(parent);
    lv_obj_set_size(g_touch_layer, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(g_touch_layer, 0, 0);
    lv_obj_set_style_bg_opa(g_touch_layer, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_touch_layer, 0, 0);
    lv_obj_set_style_pad_all(g_touch_layer, 0, 0);
    lv_obj_add_flag(g_touch_layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(g_touch_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_touch_layer, touch_layer_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g_touch_layer, touch_layer_event_cb, LV_EVENT_RELEASED, NULL);
}

void menu_start(void)
{
    lv_obj_t *scr = lv_scr_act();
    g_menu_scr = scr;
    g_menu_ready = false;
    g_pressed_app = APP_NONE;

    if (debug_label) {
        lv_obj_add_flag(debug_label, LV_OBJ_FLAG_HIDDEN);
    }

    if (g_menu_root) {
        lv_obj_del(g_menu_root);
        g_menu_root = NULL;
    }

    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);

    g_menu_root = lv_obj_create(scr);
    lv_obj_set_size(g_menu_root, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(g_menu_root, 0, 0);
    lv_obj_set_style_bg_color(g_menu_root, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(g_menu_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_menu_root, 0, 0);
    lv_obj_set_style_pad_all(g_menu_root, 0, 0);
    make_passive(g_menu_root);

    create_background(g_menu_root);
    lv_obj_t *header = create_header(g_menu_root);
    lv_obj_t *photo = create_main_photo_card(g_menu_root);
    lv_obj_t *flip = create_flip_clock_card(g_menu_root);
    create_dock(g_menu_root);
    lv_obj_t *footer = create_footer(g_menu_root);
    create_touch_layer(g_menu_root);

    fade_in_obj(header, 0, 400);
    fade_slide_in_obj(photo, 150, PHOTO_Y, 250, 450);
    fade_slide_in_obj(flip, 150, FLIP_Y, 320, 450);
    fade_in_obj(g_2048_card, 500, 280);
    fade_in_obj(g_bird_card, 600, 280);
    fade_in_obj(g_react_card, 700, 280);
    fade_in_obj(g_tomato_card, 800, 280);
    fade_in_obj(g_racing_card, 900, 280);
    fade_in_obj(g_radio_card, 960, 280);
    fade_in_obj(footer, 760, 240);

    lv_timer_t *ready_timer = lv_timer_create(menu_ready_timer_cb, 1000, NULL);
    lv_timer_set_repeat_count(ready_timer, 1);
}

void menu_go_back(void)
{
    lv_obj_t *game_scr = lv_scr_act();
    if (g_menu_scr && game_scr != g_menu_scr) {
        lv_scr_load(g_menu_scr);
        lv_obj_del(game_scr);
    } else {
        boot_ui_return_home();
    }
}
