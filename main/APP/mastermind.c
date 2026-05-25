#include "mastermind.h"
#include "menu.h"
#include "ui_fonts.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE_LEN      4
#define MAX_DIGIT     8
#define MAX_ATTEMPTS  10

#define COL_BG        0x3D2B18
#define COL_CARD      0x4E3720
#define COL_CARD_L    0x5E4228
#define COL_TEXT      0xFFF1D6
#define COL_TEXT_SOFT 0xD4B896
#define COL_ACCENT    0xF0B050
#define COL_PEG_OK    0xF0B050
#define COL_PEG_MATCH 0xD4B896
#define COL_ACCENT_2  0xE89840

static const uint32_t digit_colors[8] = {
    0xE06050, 0x50A0E0, 0x50C878, 0xF0B050,
    0xC878E0, 0xF08050, 0x60D0D0, 0xD0D060
};

#define DD_COLOR(d) digit_colors[(d)-1]

static lv_obj_t *g_scr;
static lv_obj_t *g_attempt_label;
static lv_obj_t *g_status_label;
static lv_obj_t *g_history_cont;
static lv_obj_t *g_input_slots[CODE_LEN];
static lv_obj_t *g_input_labels[CODE_LEN];

static int g_secret[CODE_LEN];
static int g_current[CODE_LEN];
static int g_pos;
static int g_attempt;
static int g_hist[CODE_LEN * MAX_ATTEMPTS];
static int g_black[MAX_ATTEMPTS];
static int g_white[MAX_ATTEMPTS];
static bool g_over;

static lv_obj_t *mk_panel(lv_obj_t *p, lv_coord_t x, lv_coord_t y,
                           lv_coord_t w, lv_coord_t h, uint32_t c, lv_coord_t r)
{
    lv_obj_t *o = lv_obj_create(p);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(c), 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, r, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *p, const char *t, lv_coord_t x, lv_coord_t y,
                           const lv_font_t *f, uint32_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *mk_btn(lv_obj_t *p, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                         const char *t, uint32_t bg, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(p);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_border_opa(btn, LV_OPA_20, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, UI_FONT_CN_20, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(COL_TEXT), 0);
    lv_obj_center(l);
    lv_obj_add_event_cb(l, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static void gen_secret(void)
{
    for (int i = 0; i < CODE_LEN; i++)
        g_secret[i] = 1 + (rand() % MAX_DIGIT);
}

static void calc(int *g, int *bk, int *wh)
{
    int su[CODE_LEN] = {0}, gu[CODE_LEN] = {0};
    *bk = 0; *wh = 0;
    for (int i = 0; i < CODE_LEN; i++)
        if (g[i] == g_secret[i]) { (*bk)++; su[i] = 1; gu[i] = 1; }
    for (int i = 0; i < CODE_LEN; i++) {
        if (gu[i]) continue;
        for (int j = 0; j < CODE_LEN; j++) {
            if (su[j]) continue;
            if (g[i] == g_secret[j]) { (*wh)++; su[j] = 1; break; }
        }
    }
}

static void refresh_input(void)
{
    for (int i = 0; i < CODE_LEN; i++) {
        if (g_current[i] > 0) {
            char d[12]; snprintf(d, sizeof(d), "%d", g_current[i]);
            lv_label_set_text(g_input_labels[i], d);
            lv_obj_set_style_bg_color(g_input_slots[i], lv_color_hex(DD_COLOR(g_current[i])), 0);
        } else {
            lv_label_set_text(g_input_labels[i], "");
            lv_obj_set_style_bg_color(g_input_slots[i], lv_color_hex(COL_CARD_L), 0);
        }
    }
}

static void refresh_hist(void)
{
    lv_obj_clean(g_history_cont);
    for (int a = 0; a < g_attempt; a++) {
        int y = a * 40 + 4;
        char buf[12];
        snprintf(buf, sizeof(buf), "%d", a + 1);
        mk_label(g_history_cont, buf, 4, y + 8, UI_FONT_CN_20, COL_TEXT_SOFT);
        for (int i = 0; i < CODE_LEN; i++) {
            int d = g_hist[a * CODE_LEN + i];
            mk_panel(g_history_cont, 30 + i * 38, y + 4, 30, 30, DD_COLOR(d), LV_RADIUS_CIRCLE);
                         char ds[12]; snprintf(ds, sizeof(ds), "%d", d);
            mk_label(g_history_cont, ds, 40 + i * 38, y + 9, UI_FONT_CN_20, COL_TEXT);
        }
        for (int i = 0; i < CODE_LEN; i++) {
            uint32_t pc = (i < g_black[a]) ? COL_PEG_OK :
                          (i < g_black[a] + g_white[a]) ? COL_PEG_MATCH : COL_CARD_L;
            mk_panel(g_history_cont, 200 + i * 18, y + 8, 14, 14, pc, LV_RADIUS_CIRCLE);
        }
    }
}

static void touch_digit(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_over) return;
    g_current[g_pos] = d;
    g_pos = (g_pos + 1) % CODE_LEN;
    refresh_input();
}

static void touch_slot(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_over) return;
    g_pos = i;
}

static void touch_clear(lv_event_t *e)
{
    (void)e;
    if (g_over) return;
    if (g_pos > 0) g_pos--;
    g_current[g_pos] = 0;
    refresh_input();
}

static void touch_submit(lv_event_t *e)
{
    (void)e;
    if (g_over) return;
    for (int i = 0; i < CODE_LEN; i++)
        if (g_current[i] == 0) return;

    memcpy(&g_hist[g_attempt * CODE_LEN], g_current, sizeof(g_current));
    calc(g_current, &g_black[g_attempt], &g_white[g_attempt]);
    g_attempt++;
    refresh_hist();

    if (g_black[g_attempt - 1] == CODE_LEN) {
        g_over = true;
        lv_label_set_text(g_status_label, "Cracked it! You win!");
        lv_obj_set_style_text_color(g_status_label, lv_color_hex(COL_ACCENT), 0);
        for (int i = 0; i < CODE_LEN; i++) {
            char d[12]; snprintf(d, sizeof(d), "%d", g_secret[i]);
            lv_label_set_text(g_input_labels[i], d);
            lv_obj_set_style_bg_color(g_input_slots[i], lv_color_hex(DD_COLOR(g_secret[i])), 0);
        }
        return;
    }
    if (g_attempt >= MAX_ATTEMPTS) {
        g_over = true;
        lv_label_set_text(g_status_label, "Out of attempts! Revealed above.");
        lv_obj_set_style_text_color(g_status_label, lv_color_hex(0xE06050), 0);
        for (int i = 0; i < CODE_LEN; i++) {
            char d[12]; snprintf(d, sizeof(d), "%d", g_secret[i]);
            lv_label_set_text(g_input_labels[i], d);
            lv_obj_set_style_bg_color(g_input_slots[i], lv_color_hex(DD_COLOR(g_secret[i])), 0);
        }
        return;
    }
    memset(g_current, 0, sizeof(g_current));
    g_pos = 0;
    refresh_input();
    char buf[32];
    snprintf(buf, sizeof(buf), "%d / %d", g_attempt + 1, MAX_ATTEMPTS);
    lv_label_set_text(g_attempt_label, buf);
}

static void touch_new(lv_event_t *e)
{
    (void)e;
    gen_secret();
    g_attempt = 0; g_over = false; g_pos = 0;
    memset(g_hist, 0, sizeof(g_hist));
    memset(g_black, 0, sizeof(g_black));
    memset(g_white, 0, sizeof(g_white));
    memset(g_current, 0, sizeof(g_current));
    refresh_hist();
    refresh_input();
    lv_label_set_text(g_attempt_label, "1 / 10");
    lv_label_set_text(g_status_label, "Enter 4 digits, then Submit");
    lv_obj_set_style_text_color(g_status_label, lv_color_hex(COL_TEXT_SOFT), 0);
}

static void touch_back(lv_event_t *e)
{
    (void)e;
    menu_go_back();
}

void mastermind_start(void)
{
    g_scr = lv_obj_create(NULL);
    lv_scr_load(g_scr);
    lv_obj_set_style_bg_color(g_scr, lv_color_hex(COL_BG), 0);
    srand((unsigned int)lv_tick_get());

    gen_secret();
    g_attempt = 0; g_over = false; g_pos = 0;
    memset(g_hist, 0, sizeof(g_hist));
    memset(g_black, 0, sizeof(g_black));
    memset(g_white, 0, sizeof(g_white));
    memset(g_current, 0, sizeof(g_current));

    mk_btn(g_scr, 8, 6, 80, 40, "Back", COL_CARD, touch_back);

    lv_obj_t *t = lv_label_create(g_scr);
    lv_label_set_text(t, "Mastermind");
    lv_obj_set_style_text_font(t, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(t, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_pos(t, 100, 10);

    g_attempt_label = lv_label_create(g_scr);
    lv_label_set_text(g_attempt_label, "1 / 10");
    lv_obj_set_style_text_font(g_attempt_label, UI_FONT_CN_20, 0);
    lv_obj_set_style_text_color(g_attempt_label, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_pos(g_attempt_label, 700, 14);

    lv_obj_t *lp = mk_panel(g_scr, 12, 54, 280, 380, COL_CARD, 12);
    g_history_cont = lv_obj_create(lp);
    lv_obj_set_pos(g_history_cont, 0, 0);
    lv_obj_set_size(g_history_cont, 280, 380);
    lv_obj_set_style_bg_opa(g_history_cont, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_history_cont, 0, 0);
    lv_obj_set_style_pad_all(g_history_cont, 0, 0);
    lv_obj_set_scroll_dir(g_history_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(g_history_cont, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *rp = mk_panel(g_scr, 304, 54, 484, 380, COL_CARD, 12);

    mk_label(rp, "Your guess:", 20, 12, UI_FONT_CN_20, COL_TEXT_SOFT);

    for (int i = 0; i < CODE_LEN; i++) {
        int sx = 20 + i * 62;
        g_input_slots[i] = mk_panel(rp, sx, 40, 50, 50, COL_CARD_L, 12);
        g_input_labels[i] = mk_label(rp, "", sx + 16, 56, UI_FONT_CN_24, COL_TEXT);

        lv_obj_t *tap = lv_obj_create(rp);
        lv_obj_set_pos(tap, sx, 40);
        lv_obj_set_size(tap, 50, 50);
        lv_obj_set_style_bg_opa(tap, LV_OPA_0, 0);
        lv_obj_set_style_border_width(tap, 0, 0);
        lv_obj_add_flag(tap, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(tap, touch_slot, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    for (int d = 0; d < MAX_DIGIT; d++) {
        int bx = 20 + (d % 4) * 63, by = 106 + (d / 4) * 64;
                     char ds[12]; snprintf(ds, sizeof(ds), "%d", d + 1);
        lv_obj_t *btn = lv_btn_create(rp);
        lv_obj_set_pos(btn, bx, by);
        lv_obj_set_size(btn, 58, 52);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COL_CARD_L), 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(COL_ACCENT), 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_20, 0);
        lv_obj_add_event_cb(btn, touch_digit, LV_EVENT_CLICKED, (void *)(intptr_t)(d + 1));
        lv_obj_t *l = lv_label_create(btn);
        lv_label_set_text(l, ds);
        lv_obj_set_style_text_font(l, UI_FONT_CN_24, 0);
        lv_obj_set_style_text_color(l, lv_color_hex(DD_COLOR(d + 1)), 0);
        lv_obj_center(l);
        lv_obj_add_event_cb(l, touch_digit, LV_EVENT_CLICKED, (void *)(intptr_t)(d + 1));
    }

    lv_obj_t *rule = mk_label(rp, "Gold dot  = right digit, right place", 20, 246, UI_FONT_CN_20, COL_TEXT_SOFT);
    mk_label(rp, "Beige dot = right digit, wrong place", 20, 274, UI_FONT_CN_20, COL_TEXT_SOFT);
    mk_label(rp, "Guess 4-digit code using 1~8", 20, 302, UI_FONT_CN_20, COL_ACCENT);

    mk_btn(rp, 290, 106, 100, 52, "Clear", COL_CARD_L, touch_clear);
    mk_btn(rp, 290, 170, 100, 52, "Submit", COL_ACCENT, touch_submit);

    g_status_label = mk_label(g_scr, "Enter 4 digits, then Submit", 20, 444, UI_FONT_CN_20, COL_TEXT_SOFT);
    mk_btn(g_scr, 360, 438, 130, 36, "New Game", COL_CARD, touch_new);

    refresh_hist();
}
