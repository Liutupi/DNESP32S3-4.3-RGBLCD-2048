/**
 ****************************************************************************************************
 * @file        menu.c
 * @brief       WarmOS Launch home screen for DNESP32S3
 ****************************************************************************************************
 */

#include "menu.h"
#include "app_network.h"
#include "boot_ui.h"
#include "game2048.h"
#include "reaction_test.h"
#include "photoviewer.h"
#include "tomato_timer.h"
#include "flip_clock.h"
#include "radio_headless.h"
#include "mastermind.h"
#include "xiaozhi_headless.h"
#include "nes_emu.h"
#include "ui_fonts.h"
#include "ui_text.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <time.h>

#define SCREEN_W        800
#define SCREEN_H        480

#define COL_BG          0xF2E8D5
#define COL_BG_GRAD     0xE5DBC8
#define COL_CARD        0xFFFDF5
#define COL_CARD_HOVER  0xFCE8CC
#define COL_SHADOW      0xD0C4B0
#define COL_TEXT        0x3A2510
#define COL_TEXT_SOFT   0x7D6850
#define COL_ACCENT      0xD07020
#define COL_ACCENT_2    0xB86218
#define COL_CLOCK       0xD07020

#define HEADER_X         48
#define HEADER_Y         10

#define CARD_X_L         32
#define CARD_X_R        412
#define CARD_Y           96
#define CARD_W          356
#define CARD_H          244

#define DOCK_Y          356
#define DOCK_W          96
#define DOCK_H          106

#define DOCK_2048_X      32
#define DOCK_REACT_X    138
#define DOCK_TOMATO_X   244
#define DOCK_RADIO_X    350
#define DOCK_MM_X       456
#define DOCK_XIAOZHI_X  562
#define DOCK_NES_X      668

#define FONT_TITLE     &lv_font_montserrat_32
#define FONT_BODY      &lv_font_montserrat_24
#define FONT_HINT      &lv_font_montserrat_20
#define FONT_GREET     &lv_font_montserrat_28
#define FONT_WIFI      &lv_font_montserrat_20

#define TAP_MOVE_MAX     30

typedef enum {
    APP_NONE = 0,
    APP_2048,
    APP_REACTION,
    APP_PHOTO,
    APP_TOMATO,
    APP_FLIP_CLOCK,
    APP_RADIO,
    APP_MASTERMIND,
    APP_XIAOZHI,
    APP_NES,
} menu_app_t;

static lv_obj_t *g_menu_scr = NULL;
static lv_obj_t *g_menu_root = NULL;
static lv_obj_t *g_touch_layer = NULL;
static lv_obj_t *g_photo_card = NULL;
static lv_obj_t *g_2048_card = NULL;
static lv_obj_t *g_react_card = NULL;
static lv_obj_t *g_tomato_card = NULL;
static lv_obj_t *g_flip_card = NULL;
static lv_obj_t *g_radio_card = NULL;
static lv_obj_t *g_mastermind_card = NULL;
static lv_obj_t *g_xiaozhi_card = NULL;
static lv_obj_t *g_nes_card = NULL;
static lv_obj_t *g_network_label = NULL;
static lv_timer_t *g_network_timer = NULL;
static lv_coord_t g_press_x = 0;
static lv_coord_t g_press_y = 0;
static menu_app_t g_pressed_app = APP_NONE;
static bool g_menu_ready = false;
static lv_obj_t *g_greeting_label = NULL;
static lv_timer_t *g_greeting_timer = NULL;
static lv_obj_t *g_flip_clock_preview = NULL;
static lv_obj_t *g_header_time_label = NULL;

extern lv_obj_t *debug_label;

static void make_passive(lv_obj_t *obj)
{
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *mk_panel(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                          lv_coord_t w, lv_coord_t h, uint32_t color,
                          lv_opa_t opa, lv_coord_t radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    
    // 注入亮银半透明的 1px 细微玻璃态边框，在深色背景下营造悬浮质感
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_20, 0);
    
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    make_passive(obj);
    return obj;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *text,
                          const lv_font_t *font, uint32_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    ui_text_set(label, text);
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

static void set_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void fade_in_obj(lv_obj_t *obj, uint32_t delay, uint32_t time)
{
    lv_obj_set_style_opa(obj, LV_OPA_0, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, set_opa_cb);
    lv_anim_set_values(&a, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_time(&a, time);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void fade_slide_in_obj(lv_obj_t *obj, lv_coord_t end_y,
                              uint32_t delay, uint32_t time)
{
    lv_obj_set_style_opa(obj, LV_OPA_0, 0);
    lv_anim_t ay;
    lv_anim_init(&ay);
    lv_anim_set_var(&ay, obj);
    lv_anim_set_exec_cb(&ay, anim_y_cb);
    lv_anim_set_values(&ay, end_y + 16, end_y);
    lv_anim_set_delay(&ay, delay);
    lv_anim_set_time(&ay, time > 300 ? 300 : time);
    lv_anim_set_path_cb(&ay, lv_anim_path_ease_out);
    lv_anim_start(&ay);

    lv_anim_t ao;
    lv_anim_init(&ao);
    lv_anim_set_var(&ao, obj);
    lv_anim_set_exec_cb(&ao, set_opa_cb);
    lv_anim_set_values(&ao, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_delay(&ao, delay);
    lv_anim_set_time(&ao, time > 350 ? 350 : time);
    lv_anim_set_path_cb(&ao, lv_anim_path_ease_out);
    lv_anim_start(&ao);
}

static void breath_bg_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void start_breath(lv_obj_t *obj, lv_opa_t lo, lv_opa_t hi, uint32_t ms, uint32_t delay)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, breath_bg_opa_cb);
    lv_anim_set_values(&a, lo, hi);
    lv_anim_set_time(&a, ms);
    lv_anim_set_playback_time(&a, ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void menu_ready_timer_cb(lv_timer_t *timer)
{
    g_menu_ready = true;
    lv_timer_del(timer);
}

static const char *get_greeting(int hour)
{
    if (hour >= 6 && hour < 12)  return "Morning, Tupi";
    if (hour >= 12 && hour < 18) return "Afternoon, Tupi";
    return "Evening, Tupi";
}

static const char *g_cur_greet = "Hi, Tupi";

static void greet_text_cb(lv_anim_t *anim)
{
    (void)anim;
    if (!g_greeting_label) return;
    ui_text_set(g_greeting_label, g_cur_greet);
}

static void update_greeting_label(void)
{
    if (!g_greeting_label) return;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    if (tm_now.tm_year < 124) {
        ui_text_set(g_greeting_label, "Hi, Tupi");
        return;
    }

    const char *new_greet = get_greeting(tm_now.tm_hour);
    if (g_cur_greet == new_greet) return;
    g_cur_greet = new_greet;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_greeting_label);
    lv_anim_set_exec_cb(&a, set_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_0);
    lv_anim_set_time(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, greet_text_cb);
    lv_anim_start(&a);

    lv_anim_t b;
    lv_anim_init(&b);
    lv_anim_set_var(&b, g_greeting_label);
    lv_anim_set_exec_cb(&b, set_opa_cb);
    lv_anim_set_values(&b, LV_OPA_0, LV_OPA_COVER);
    lv_anim_set_time(&b, 300);
    lv_anim_set_delay(&b, 280);
    lv_anim_set_path_cb(&b, lv_anim_path_ease_out);
    lv_anim_start(&b);
}

static void greeting_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_greeting_label();
}

static void update_clock_preview(void)
{
    if (!g_flip_clock_preview) return;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    if (tm_now.tm_year < 124) {
        ui_text_set(g_flip_clock_preview, "--:--");
        return;
    }

    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    ui_text_set(g_flip_clock_preview, buf);
}

static void update_network_status_label(void)
{
    if (!g_network_label) return;

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        char buf[40];
        snprintf(buf, sizeof(buf), "WiFi %ddBm", (int)ap.rssi);
        ui_text_set(g_network_label, buf);
        lv_obj_set_style_text_color(g_network_label, lv_color_hex(0x5A8A5A), 0);
        return;
    }

    if (app_network_has_configured_wifi()) {
        ui_text_set(g_network_label, "WiFi Connect..");
        lv_obj_set_style_text_color(g_network_label, lv_color_hex(COL_ACCENT), 0);
    } else {
        ui_text_set(g_network_label, "WiFi Setup");
        lv_obj_set_style_text_color(g_network_label, lv_color_hex(COL_TEXT_SOFT), 0);
    }
}

static void update_header_time_label(void)
{
    if (!g_header_time_label) return;

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);

    if (tm_now.tm_year < 124) {
        ui_text_set(g_header_time_label, "--:--");
        return;
    }

    char buf[8];
    snprintf(buf, sizeof(buf), "%02d:%02d", tm_now.tm_hour, tm_now.tm_min);
    ui_text_set(g_header_time_label, buf);
}

static void network_status_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    update_network_status_label();
    update_clock_preview();
    update_header_time_label();
}

static void create_background(lv_obj_t *parent)
{
    lv_obj_set_style_bg_color(parent, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_grad_color(parent, lv_color_hex(COL_BG_GRAD), 0);
    lv_obj_set_style_bg_grad_dir(parent, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_bg_main_stop(parent, 0, 0);
    lv_obj_set_style_bg_grad_stop(parent, 255, 0);
}

static lv_obj_t *create_header(lv_obj_t *parent)
{
    lv_obj_t *header = mk_panel(parent, 0, 0, SCREEN_W, 84, COL_BG, LV_OPA_TRANSP, 0);

    lv_obj_t *title = mk_label(header, "Nothing impossible", &lv_font_montserrat_48, COL_TEXT);
    lv_obj_set_pos(title, HEADER_X, HEADER_Y);

    g_greeting_label = mk_label(header, "Hi, Tupi", FONT_GREET, COL_TEXT_SOFT);
    lv_obj_set_pos(g_greeting_label, HEADER_X, 56);
    update_greeting_label();

    g_network_label = mk_label(header, "WiFi ..", FONT_WIFI, COL_TEXT_SOFT);
    lv_obj_set_width(g_network_label, 190);
    lv_obj_set_style_text_align(g_network_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(g_network_label, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(g_network_label, 570, 58);
    update_network_status_label();

    /* Header 实时时钟，右上角显示 HH:MM */
    g_header_time_label = mk_label(header, "--:--", FONT_TITLE, COL_CLOCK);
    lv_obj_set_pos(g_header_time_label, 614, 8);
    update_header_time_label();

    return header;
}

static lv_obj_t *create_photo_card(lv_obj_t *parent)
{
    mk_panel(parent, CARD_X_L + 3, CARD_Y + 4,
             CARD_W, CARD_H, COL_SHADOW, LV_OPA_40, 28);
    lv_obj_t *card = mk_panel(parent, CARD_X_L, CARD_Y, CARD_W, CARD_H,
                              COL_CARD, LV_OPA_COVER, 28);
    g_photo_card = card;

    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_20, 0);

    /* 右侧暖橙渐变色带装饰 */
    mk_panel(card, 276, 0, 80, CARD_H, COL_ACCENT, 20, 28);

    lv_obj_t *d1 = mk_panel(card, 220, 60, 108, 80, COL_ACCENT, LV_OPA_10, 16);
    lv_obj_t *d2 = mk_panel(card, 238, 78,  88, 62, COL_ACCENT, LV_OPA_10, 12);
    lv_obj_t *d3 = mk_panel(card, 256, 96,  68, 44, COL_ACCENT, LV_OPA_20, 8);

    start_breath(d1, LV_OPA_10, LV_OPA_20, 3500, 0);
    start_breath(d2, LV_OPA_10, LV_OPA_20, 3500, 600);
    start_breath(d3, LV_OPA_20, LV_OPA_30, 3500, 1200);

    lv_obj_t *t = mk_label(card, "Photos",       FONT_TITLE, COL_TEXT);
    lv_obj_set_pos(t, 36, 44);

    lv_obj_t *s = mk_label(card, "View gallery", FONT_BODY,  COL_TEXT_SOFT);
    lv_obj_set_pos(s, 38, 100);

    lv_obj_t *h = mk_label(card, "Tap to open",  FONT_HINT,  COL_ACCENT);
    lv_obj_set_pos(h, 38, 168);

    return card;
}

static lv_obj_t *create_flip_card(lv_obj_t *parent)
{
    mk_panel(parent, CARD_X_R + 3, CARD_Y + 4,
             CARD_W, CARD_H, COL_SHADOW, LV_OPA_40, 28);
    lv_obj_t *card = mk_panel(parent, CARD_X_R, CARD_Y, CARD_W, CARD_H,
                              COL_CARD, LV_OPA_COVER, 28);
    g_flip_card = card;

    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_ACCENT_2), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_20, 0);

    /* 右侧琥珀色装饰色带 */
    mk_panel(card, 276, 0, 80, CARD_H, COL_ACCENT_2, 20, 28);

    lv_obj_t *df = mk_panel(card, 218, 54, 114, 114, COL_ACCENT_2, LV_OPA_10, 24);
    start_breath(df, LV_OPA_10, LV_OPA_20, 4000, 0);

    lv_obj_t *t = mk_label(card, "Flip Clock",      FONT_TITLE, COL_TEXT);
    lv_obj_set_pos(t, 36, 44);

    lv_obj_t *s = mk_label(card, "Desk clock",      FONT_BODY,  COL_TEXT_SOFT);
    lv_obj_set_pos(s, 38, 100);

    lv_obj_t *h = mk_label(card, "Tap to open",     FONT_HINT,  COL_ACCENT);
    lv_obj_set_pos(h, 38, 168);

    g_flip_clock_preview = mk_label(card, "--:--", UI_FONT_DIGIT_48, COL_CLOCK);
    lv_obj_set_pos(g_flip_clock_preview, 188, 80);
    update_clock_preview();

    return card;
}

static lv_obj_t *create_dock_card(lv_obj_t *parent, lv_coord_t x,
                                  const char *title, const char *sub,
                                  const char *icon_char,
                                  uint32_t accent, uint32_t bar_color)
{
    mk_panel(parent, x + 2, DOCK_Y + 3,
             DOCK_W, DOCK_H, COL_SHADOW, LV_OPA_40, 20);
    lv_obj_t *card = mk_panel(parent, x, DOCK_Y, DOCK_W, DOCK_H,
                              COL_CARD, LV_OPA_COVER, 20);

    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(COL_SHADOW), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_50, 0);

    /* 顶部彩色标识条 */
    mk_panel(card, 0, 0, DOCK_W, 3, bar_color, LV_OPA_COVER, 20);

    /* 图标底盘（24x24） */
    mk_panel(card, 10, 14, 24, 24, accent, 64, 12);
    lv_obj_t *ic = mk_label(card, icon_char, FONT_HINT, accent);
    lv_obj_set_pos(ic, 16, 16);

    lv_obj_t *t = mk_label(card, title, FONT_HINT, COL_TEXT);
    lv_obj_set_pos(t, 12, 44);
    lv_obj_set_width(t, DOCK_W - 20);
    lv_label_set_long_mode(t, LV_LABEL_LONG_CLIP);

    lv_obj_t *s = mk_label(card, sub, FONT_HINT, COL_TEXT_SOFT);
    lv_obj_set_pos(s, 14, 70);
    lv_obj_set_width(s, DOCK_W - 28);
    lv_label_set_long_mode(s, LV_LABEL_LONG_CLIP);

    return card;
}

static void create_dock(lv_obj_t *parent)
{
    /* 大卡片与 Dock 之间的分隔线 */
    mk_panel(parent, 32, 348, 736, 1, COL_SHADOW, LV_OPA_50, 0);

    /* 每个 App 独立色条：2048=暖橙 React=冷蓝 Tomato=番茄红 Radio=薄荷绿 Master=紫 */
    g_2048_card      = create_dock_card(parent, DOCK_2048_X,  "2048",   "Puzzle", "2", COL_ACCENT,   0xD07020);
    g_react_card     = create_dock_card(parent, DOCK_REACT_X, "React",  "Test",   "R", COL_ACCENT_2, 0x4A9FD5);
    g_tomato_card    = create_dock_card(parent, DOCK_TOMATO_X,"Tomato", "Timer",  "T", COL_ACCENT_2, 0xE05050);
    g_radio_card     = create_dock_card(parent, DOCK_RADIO_X, "Radio",  "Stream", "S", COL_ACCENT_2, 0x50A060);
    g_mastermind_card= create_dock_card(parent, DOCK_MM_X,    "Master", "Mind",   "M", COL_ACCENT,   0x9060D0);
    g_xiaozhi_card   = create_dock_card(parent, DOCK_XIAOZHI_X,"XiaoZhi","AI",    "X", COL_ACCENT,   0xE67E22);
    g_nes_card       = create_dock_card(parent, DOCK_NES_X,    "FC",     "Games",  "N", COL_ACCENT,   0x20B040);
}

static lv_obj_t *create_footer(lv_obj_t *parent)
{
    lv_obj_t *l = mk_label(parent, "DNESP32S3 by Tupi", FONT_HINT, COL_TEXT_SOFT);
    lv_obj_set_pos(l, 572, 462);
    return l;
}

static menu_app_t hit_test(lv_coord_t x, lv_coord_t y)
{
    if (x >= CARD_X_L && x <= CARD_X_L + CARD_W &&
        y >= CARD_Y    && y <= CARD_Y + CARD_H)    return APP_PHOTO;
    if (x >= CARD_X_R && x <= CARD_X_R + CARD_W &&
        y >= CARD_Y    && y <= CARD_Y + CARD_H)    return APP_FLIP_CLOCK;

    if (x >= DOCK_2048_X && x <= DOCK_2048_X + DOCK_W &&
        y >= DOCK_Y      && y <= DOCK_Y + DOCK_H)       return APP_2048;
    if (x >= DOCK_REACT_X && x <= DOCK_REACT_X + DOCK_W &&
        y >= DOCK_Y       && y <= DOCK_Y + DOCK_H)      return APP_REACTION;
    if (x >= DOCK_TOMATO_X && x <= DOCK_TOMATO_X + DOCK_W &&
        y >= DOCK_Y        && y <= DOCK_Y + DOCK_H)      return APP_TOMATO;
    if (x >= DOCK_RADIO_X && x <= DOCK_RADIO_X + DOCK_W &&
        y >= DOCK_Y       && y <= DOCK_Y + DOCK_H)       return APP_RADIO;
    if (x >= DOCK_MM_X && x <= DOCK_MM_X + DOCK_W &&
        y >= DOCK_Y    && y <= DOCK_Y + DOCK_H)          return APP_MASTERMIND;
    if (x >= DOCK_XIAOZHI_X && x <= DOCK_XIAOZHI_X + DOCK_W &&
        y >= DOCK_Y         && y <= DOCK_Y + DOCK_H)        return APP_XIAOZHI;
    if (x >= DOCK_NES_X && x <= DOCK_NES_X + DOCK_W &&
        y >= DOCK_Y     && y <= DOCK_Y + DOCK_H)           return APP_NES;

    return APP_NONE;
}

static lv_obj_t *card_for_app(menu_app_t app)
{
    switch (app) {
        case APP_PHOTO:      return g_photo_card;
        case APP_2048:       return g_2048_card;
        case APP_REACTION:   return g_react_card;
        case APP_TOMATO:     return g_tomato_card;
        case APP_FLIP_CLOCK: return g_flip_card;
        case APP_RADIO:      return g_radio_card;
        case APP_MASTERMIND: return g_mastermind_card;
        case APP_XIAOZHI:    return g_xiaozhi_card;
        case APP_NES:        return g_nes_card;
        default:             return NULL;
    }
}

static void set_card_feedback(menu_app_t app, bool active)
{
    lv_obj_t *card = card_for_app(app);
    if (!card) return;

    lv_obj_set_style_bg_color(card, lv_color_hex(active ? COL_CARD_HOVER : COL_CARD), 0);
    lv_obj_set_style_border_opa(card, active ? LV_OPA_COVER : LV_OPA_20, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(active ? COL_ACCENT : COL_SHADOW), 0);
}

static void launch_app(menu_app_t app)
{
    switch (app) {
        case APP_PHOTO:      photoviewer_start();   break;
        case APP_2048:       game2048_start();      break;
        case APP_REACTION:   reaction_test_start(); break;
        case APP_TOMATO:     tomato_timer_start();  break;
        case APP_FLIP_CLOCK: flip_clock_start();    break;
        case APP_RADIO:      radio_headless_start();break;
        case APP_MASTERMIND: mastermind_start();    break;
        case APP_XIAOZHI:    xiaozhi_headless_start(); break;
        case APP_NES:        nes_rom_browser_start(); break;
        default: break;
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
    if (g_network_timer) {
        lv_timer_del(g_network_timer);
        g_network_timer = NULL;
    }
    if (g_greeting_timer) {
        lv_timer_del(g_greeting_timer);
        g_greeting_timer = NULL;
    }
    g_network_label = NULL;
    g_flip_clock_preview = NULL;
    g_header_time_label = NULL;

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
    vTaskDelay(pdMS_TO_TICKS(1));
    lv_obj_t *header = create_header(g_menu_root);
    vTaskDelay(pdMS_TO_TICKS(1));
    lv_obj_t *photo = create_photo_card(g_menu_root);
    vTaskDelay(pdMS_TO_TICKS(1));
    lv_obj_t *flip  = create_flip_card(g_menu_root);
    vTaskDelay(pdMS_TO_TICKS(1));
    create_dock(g_menu_root);
    vTaskDelay(pdMS_TO_TICKS(1));
    lv_obj_t *footer = create_footer(g_menu_root);
    create_touch_layer(g_menu_root);

    fade_in_obj(header, 0, 400);
    fade_slide_in_obj(photo, CARD_Y, 160, 420);
    fade_slide_in_obj(flip,  CARD_Y, 280, 420);
    fade_in_obj(g_2048_card,      500, 260);
    fade_in_obj(g_react_card,     660, 260);
    fade_in_obj(g_tomato_card,    760, 260);
    fade_in_obj(g_radio_card,     880, 260);
    fade_in_obj(g_mastermind_card,960, 260);
    fade_in_obj(g_xiaozhi_card,1080, 260);
    fade_in_obj(g_nes_card,    1200, 260);
    fade_in_obj(footer, 800, 220);

    lv_timer_t *ready_timer = lv_timer_create(menu_ready_timer_cb, 1000, NULL);
    lv_timer_set_repeat_count(ready_timer, 1);
    g_network_timer = lv_timer_create(network_status_timer_cb, 1000, NULL);
    g_greeting_timer = lv_timer_create(greeting_timer_cb, 30000, NULL);
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
