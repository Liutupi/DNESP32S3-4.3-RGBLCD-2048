/**
 ****************************************************************************************************
 * @file        reaction_test.c
 * @brief       Reaction Time Test game for LVGL on 4.3inch RGB LCD (800x480)
 *              Tap when the screen turns green to test your reaction speed.
 ****************************************************************************************************
 */

#include "reaction_test.h"
#include "menu.h"
#include <stdio.h>
#include <stdlib.h>

typedef enum {
    RT_IDLE = 0,
    RT_WAITING,
    RT_TRIGGERED,
    RT_RESULT,
    RT_TOO_EARLY
} rt_state_t;

static rt_state_t g_state;
static lv_obj_t *g_screen;
static lv_obj_t *g_title_label;
static lv_obj_t *g_main_label;
static lv_obj_t *g_info_label;
static lv_obj_t *g_menu_btn;
static int64_t g_trigger_ms;
static int64_t g_reaction_ms;
static int64_t g_best_ms;
static lv_timer_t *g_wait_timer;

static void update_ui(void);
static void screen_cb(lv_event_t *e);
static void wait_timer_cb(lv_timer_t *t);

static void menu_cb(lv_event_t *e)
{
    (void)e;
    if (g_wait_timer) {
        lv_timer_del(g_wait_timer);
        g_wait_timer = NULL;
    }
    menu_go_back();
}

static void wait_timer_cb(lv_timer_t *t)
{
    (void)t;
    g_wait_timer = NULL;
    g_state = RT_TRIGGERED;
    g_trigger_ms = lv_tick_get();
    update_ui();
}

static void screen_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_RELEASED) return;

    switch (g_state) {
        case RT_IDLE:
            g_state = RT_WAITING;
            update_ui();
            g_wait_timer = lv_timer_create(wait_timer_cb,
                                           1500 + (rand() % 3000), NULL);
            lv_timer_set_repeat_count(g_wait_timer, 1);
            break;

        case RT_WAITING:
            lv_timer_del(g_wait_timer);
            g_wait_timer = NULL;
            g_state = RT_TOO_EARLY;
            update_ui();
            break;

        case RT_TRIGGERED:
            g_reaction_ms = lv_tick_get() - g_trigger_ms;
            if (g_best_ms == 0 || g_reaction_ms < g_best_ms) {
                g_best_ms = g_reaction_ms;
            }
            g_state = RT_RESULT;
            update_ui();
            break;

        case RT_RESULT:
        case RT_TOO_EARLY:
            g_state = RT_IDLE;
            update_ui();
            break;
    }
}

static void update_ui(void)
{
    char buf[64];

    switch (g_state) {
        case RT_IDLE:
            lv_obj_set_style_bg_color(g_screen, lv_color_hex(0x2C3E50), 0);
            lv_label_set_text(g_title_label, "Reaction Test");
            lv_label_set_text(g_main_label, "READY");
            if (g_best_ms > 0) {
                snprintf(buf, sizeof(buf), "Best: %lld ms\nTap anywhere to start",
                         g_best_ms);
            } else {
                snprintf(buf, sizeof(buf), "Tap anywhere to start");
            }
            lv_label_set_text(g_info_label, buf);
            lv_obj_clear_flag(g_menu_btn, LV_OBJ_FLAG_HIDDEN);
            break;

        case RT_WAITING:
            lv_obj_set_style_bg_color(g_screen, lv_color_hex(0xC0392B), 0);
            lv_label_set_text(g_title_label, "Wait...");
            lv_label_set_text(g_main_label, "");
            lv_label_set_text(g_info_label, "Wait for green, then tap!");
            lv_obj_add_flag(g_menu_btn, LV_OBJ_FLAG_HIDDEN);
            break;

        case RT_TRIGGERED:
            lv_obj_set_style_bg_color(g_screen, lv_color_hex(0x27AE60), 0);
            lv_label_set_text(g_title_label, "TAP NOW!");
            lv_label_set_text(g_main_label, "");
            lv_label_set_text(g_info_label, "");
            lv_obj_add_flag(g_menu_btn, LV_OBJ_FLAG_HIDDEN);
            break;

        case RT_RESULT:
            lv_obj_set_style_bg_color(g_screen, lv_color_hex(0x2980B9), 0);
            snprintf(buf, sizeof(buf), "%lld ms", g_reaction_ms);
            lv_label_set_text(g_title_label, "Your Time");
            lv_label_set_text(g_main_label, buf);
            snprintf(buf, sizeof(buf), "Best: %lld ms\nTap to try again",
                     g_best_ms);
            lv_label_set_text(g_info_label, buf);
            lv_obj_clear_flag(g_menu_btn, LV_OBJ_FLAG_HIDDEN);
            break;

        case RT_TOO_EARLY:
            lv_obj_set_style_bg_color(g_screen, lv_color_hex(0xE67E22), 0);
            lv_label_set_text(g_title_label, "Too Early!");
            lv_label_set_text(g_main_label, "X");
            lv_label_set_text(g_info_label,
                              "Don't tap before green!\nTap to try again.");
            lv_obj_clear_flag(g_menu_btn, LV_OBJ_FLAG_HIDDEN);
            break;
    }
}

void reaction_test_start(void)
{
    g_screen = lv_obj_create(NULL);
    lv_scr_load(g_screen);
    lv_obj_set_style_bg_color(g_screen, lv_color_hex(0x2C3E50), 0);

    srand((unsigned int)lv_tick_get());
    g_best_ms = 0;
    g_wait_timer = NULL;

    /* Invisible touch layer covering full screen */
    lv_obj_t *touch = lv_obj_create(g_screen);
    lv_obj_set_size(touch, 800, 480);
    lv_obj_set_pos(touch, 0, 0);
    lv_obj_set_style_bg_opa(touch, LV_OPA_0, 0);
    lv_obj_set_style_border_width(touch, 0, 0);
    lv_obj_clear_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(touch, screen_cb, LV_EVENT_RELEASED, NULL);

    /* Title label (top) */
    g_title_label = lv_label_create(g_screen);
    lv_obj_set_style_text_font(g_title_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(g_title_label, lv_color_hex(0xECF0F1), 0);
    lv_obj_align(g_title_label, LV_ALIGN_TOP_MID, 0, 40);

    /* Main display (center) */
    g_main_label = lv_label_create(g_screen);
    lv_obj_set_style_text_font(g_main_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(g_main_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(g_main_label, LV_ALIGN_CENTER, 0, -20);

    /* Info label (bottom area) */
    g_info_label = lv_label_create(g_screen);
    lv_obj_set_style_text_font(g_info_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(g_info_label, lv_color_hex(0xBDC3C7), 0);
    lv_obj_set_style_text_align(g_info_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_info_label, LV_ALIGN_BOTTOM_MID, 0, -100);

    /* Menu button */
    g_menu_btn = lv_btn_create(g_screen);
    lv_obj_set_size(g_menu_btn, 120, 44);
    lv_obj_set_style_bg_color(g_menu_btn, lv_color_hex(0x34495E), 0);
    lv_obj_set_style_radius(g_menu_btn, 8, 0);
    lv_obj_align(g_menu_btn, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_add_event_cb(g_menu_btn, menu_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(g_menu_btn);
    lv_label_set_text(btn_lbl, "Menu");
    lv_obj_set_style_text_font(btn_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(btn_lbl);
    lv_obj_add_event_cb(btn_lbl, menu_cb, LV_EVENT_RELEASED, NULL);

    g_state = RT_IDLE;
    update_ui();
}
