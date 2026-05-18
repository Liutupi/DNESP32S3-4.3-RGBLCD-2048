/**
 ****************************************************************************************************
 * @file        menu.c
 * @brief       Main menu screen for game selection on DNESP32S3
 ****************************************************************************************************
 */

#include "menu.h"
#include "game2048.h"
#include "reaction_test.h"
#include "bird_launcher.h"
#include "photoviewer.h"
#include <stdio.h>

static lv_obj_t *g_menu_scr = NULL;
static lv_coord_t g_press_x = 0;
static lv_coord_t g_press_y = 0;

extern lv_obj_t * debug_label;

#define CARD_X      200
#define CARD_W      400
#define CARD1_Y      95
#define CARD2_Y     190
#define CARD3_Y     285
#define CARD4_Y     375
#define CARD_H       85

static int hit_test(lv_coord_t x, lv_coord_t y)
{
    if (x >= CARD_X && x <= CARD_X + CARD_W) {
        if (y >= CARD1_Y && y <= CARD1_Y + CARD_H) return 1;
        if (y >= CARD2_Y && y <= CARD2_Y + CARD_H) return 2;
        if (y >= CARD3_Y && y <= CARD3_Y + CARD_H) return 3;
        if (y >= CARD4_Y && y <= CARD4_Y + CARD_H) return 4;
    }
    return 0;
}

static void touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t pt;

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &pt);
        g_press_x = pt.x;
        g_press_y = pt.y;
    } else if (code == LV_EVENT_RELEASED) {
        lv_indev_get_point(indev, &pt);
        lv_coord_t dx = pt.x - g_press_x;
        lv_coord_t dy = pt.y - g_press_y;
        if (LV_ABS(dx) > 30 || LV_ABS(dy) > 30) return;

        int card = hit_test(pt.x, pt.y);
        if (card == 1) {
            game2048_start();
        } else if (card == 2) {
            reaction_test_start();
        } else if (card == 3) {
            bird_launcher_start();
        } else if (card == 4) {
            photoviewer_start();
        }
    }
}

void menu_start(void)
{
    lv_obj_t *scr = lv_scr_act();
    g_menu_scr = scr;

    if (debug_label) {
        lv_obj_add_flag(debug_label, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1A1A2E), 0);

    /* Header */
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_set_size(header, 800, 90);
    lv_obj_set_pos(header, 0, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(0x16213E), 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hl = lv_label_create(header);
    lv_label_set_text(hl, LV_SYMBOL_PLAY "  Game  &  Photo Center");
    lv_obj_set_style_text_font(hl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(hl, lv_color_hex(0xECF0F1), 0);
    lv_obj_center(hl);

    /* Card 1: 2048 */
    lv_obj_t *c1 = lv_obj_create(scr);
    lv_obj_set_size(c1, CARD_W, CARD_H);
    lv_obj_set_pos(c1, CARD_X, CARD1_Y);
    lv_obj_set_style_bg_color(c1, lv_color_hex(0xE94560), 0);
    lv_obj_set_style_radius(c1, 12, 0);
    lv_obj_set_style_border_width(c1, 0, 0);
    lv_obj_set_style_pad_all(c1, 0, 0);
    lv_obj_clear_flag(c1, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *c1t = lv_label_create(c1);
    lv_label_set_text(c1t, LV_SYMBOL_SHUFFLE "  2048");
    lv_obj_set_style_text_font(c1t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(c1t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(c1t, 20, 14);

    lv_obj_t *c1s = lv_label_create(c1);
    lv_label_set_text(c1s, "Swipe to merge tiles & reach 2048");
    lv_obj_set_style_text_font(c1s, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(c1s, lv_color_hex(0xECF0F1), 0);
    lv_obj_set_pos(c1s, 20, 52);

    /* Card 2: Reaction Test */
    lv_obj_t *c2 = lv_obj_create(scr);
    lv_obj_set_size(c2, CARD_W, CARD_H);
    lv_obj_set_pos(c2, CARD_X, CARD2_Y);
    lv_obj_set_style_bg_color(c2, lv_color_hex(0x0F3460), 0);
    lv_obj_set_style_radius(c2, 12, 0);
    lv_obj_set_style_border_width(c2, 0, 0);
    lv_obj_set_style_pad_all(c2, 0, 0);
    lv_obj_clear_flag(c2, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *c2t = lv_label_create(c2);
    lv_label_set_text(c2t, LV_SYMBOL_LOOP "  Reaction Test");
    lv_obj_set_style_text_font(c2t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(c2t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(c2t, 20, 14);

    lv_obj_t *c2s = lv_label_create(c2);
    lv_label_set_text(c2s, "Tap when green to test your speed");
    lv_obj_set_style_text_font(c2s, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(c2s, lv_color_hex(0xECF0F1), 0);
    lv_obj_set_pos(c2s, 20, 52);

    /* Card 3: Bird Launcher */
    lv_obj_t *c3 = lv_obj_create(scr);
    lv_obj_set_size(c3, CARD_W, CARD_H);
    lv_obj_set_pos(c3, CARD_X, CARD3_Y);
    lv_obj_set_style_bg_color(c3, lv_color_hex(0x6C3483), 0);
    lv_obj_set_style_radius(c3, 12, 0);
    lv_obj_set_style_border_width(c3, 0, 0);
    lv_obj_set_style_pad_all(c3, 0, 0);
    lv_obj_clear_flag(c3, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *c3t = lv_label_create(c3);
    lv_label_set_text(c3t, LV_SYMBOL_CHARGE "  Bird Launcher");
    lv_obj_set_style_text_font(c3t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(c3t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(c3t, 20, 14);

    lv_obj_t *c3s = lv_label_create(c3);
    lv_label_set_text(c3s, "Drag to aim, release to launch!");
    lv_obj_set_style_text_font(c3s, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(c3s, lv_color_hex(0xECF0F1), 0);
    lv_obj_set_pos(c3s, 20, 52);

    /* Card 4: Photo Viewer */
    lv_obj_t *c4 = lv_obj_create(scr);
    lv_obj_set_size(c4, CARD_W, CARD_H);
    lv_obj_set_pos(c4, CARD_X, CARD4_Y);
    lv_obj_set_style_bg_color(c4, lv_color_hex(0x145A32), 0);
    lv_obj_set_style_radius(c4, 12, 0);
    lv_obj_set_style_border_width(c4, 0, 0);
    lv_obj_set_style_pad_all(c4, 0, 0);
    lv_obj_clear_flag(c4, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *c4t = lv_label_create(c4);
    lv_label_set_text(c4t, LV_SYMBOL_IMAGE "  Photo Viewer");
    lv_obj_set_style_text_font(c4t, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(c4t, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_pos(c4t, 20, 14);

    lv_obj_t *c4s = lv_label_create(c4);
    lv_label_set_text(c4s, "Auto slideshow from SD card (/PHOTOS/)");
    lv_obj_set_style_text_font(c4s, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(c4s, lv_color_hex(0xA9DFBF), 0);
    lv_obj_set_pos(c4s, 20, 52);

    /* Transparent touch layer on top */
    lv_obj_t *touch = lv_obj_create(scr);
    lv_obj_set_size(touch, 800, 480);
    lv_obj_set_pos(touch, 0, 0);
    lv_obj_set_style_bg_opa(touch, LV_OPA_0, 0);
    lv_obj_set_style_border_width(touch, 0, 0);
    lv_obj_clear_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(touch, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(touch, touch_cb, LV_EVENT_RELEASED, NULL);

    /* Footer */
    lv_obj_t *foot = lv_label_create(scr);
    lv_label_set_text(foot, "DNESP32S3  |  Touch to play");
    lv_obj_set_style_text_font(foot, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(foot, lv_color_hex(0x533483), 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -20);
}

void menu_go_back(void)
{
    lv_obj_t *game_scr = lv_scr_act();
    if (g_menu_scr && game_scr != g_menu_scr) {
        lv_scr_load(g_menu_scr);
        lv_obj_del(game_scr);
    }
}
