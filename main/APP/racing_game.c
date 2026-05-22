/**
 ****************************************************************************************************
 * @file        racing_game.c
 * @brief       Pixel Road Rush racing game for DNESP32S3 800x480 RGB LCD.
 ****************************************************************************************************
 */

#include "racing_game.h"
#include "menu.h"
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

#define RG_W                 800
#define RG_H                 480
#define RG_HORIZON_Y         132
#define RG_ROAD_BOTTOM_Y     480
#define RG_PLAYER_Y          385
#define RG_PLAYER_W          70
#define RG_PLAYER_H          82
#define RG_MAX_ENEMIES       6
#define RG_ROAD_STRIPS       18
#define RG_LANE_MARKERS      14
#define RG_FRAME_MS          16

#define RG_COLOR_SKY_TOP     0x7EC8F5
#define RG_COLOR_SKY_LOW     0xBFE8FF
#define RG_COLOR_MOUNTAIN    0x6F8E9B
#define RG_COLOR_GRASS       0x3DA35D
#define RG_COLOR_GRASS_DARK  0x2F7D45
#define RG_COLOR_ROAD        0x343942
#define RG_COLOR_ROAD_ALT    0x2B3038
#define RG_COLOR_LINE        0xF2E7A1
#define RG_COLOR_HUD         0x16212B
#define RG_COLOR_TEXT        0xF8FAFC
#define RG_COLOR_PLAYER      0xE94560
#define RG_COLOR_PLAYER_TOP  0xF7A8B8
#define RG_COLOR_ENEMY       0x2D9CDB
#define RG_COLOR_BLOCK       0xF39C12

typedef enum {
    RG_STATE_PLAYING = 0,
    RG_STATE_GAME_OVER
} rg_state_t;

typedef struct {
    lv_obj_t *body;
    lv_obj_t *shine;
    bool active;
    float z;
    int lane;
    int kind;
} rg_enemy_t;

static lv_obj_t *g_scr;
static lv_obj_t *g_touch;
static lv_obj_t *g_score_label;
static lv_obj_t *g_speed_label;
static lv_obj_t *g_time_label;
static lv_obj_t *g_msg_panel;
static lv_obj_t *g_msg_title;
static lv_obj_t *g_msg_score;
static lv_obj_t *g_player_body;
static lv_obj_t *g_player_cabin;
static lv_obj_t *g_player_lw;
static lv_obj_t *g_player_rw;
static lv_obj_t *g_road_strip[RG_ROAD_STRIPS];
static lv_obj_t *g_lane_marker[RG_LANE_MARKERS];
static lv_timer_t *g_timer;
static rg_enemy_t g_enemies[RG_MAX_ENEMIES];

static rg_state_t g_state;
static int g_steer;
static float g_player_x;
static float g_marker_phase;
static int g_score;
static int g_speed;
static uint32_t g_start_tick;
static uint32_t g_last_tick;
static uint32_t g_next_spawn_tick;

static float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static float road_half_width(float z)
{
    return 38.0f + z * z * 335.0f;
}

static int road_y(float z)
{
    return RG_HORIZON_Y + (int)(z * z * (RG_ROAD_BOTTOM_Y - RG_HORIZON_Y));
}

static int lane_x(int lane, float z)
{
    return (int)(RG_W / 2 + lane * road_half_width(z) * 0.38f);
}

static void cleanup_game(void)
{
    if (g_timer) {
        lv_timer_del(g_timer);
        g_timer = NULL;
    }
}

static void back_to_menu(void)
{
    cleanup_game();
    menu_go_back();
}

static void update_hud(void)
{
    char buf[32];
    uint32_t elapsed = (lv_tick_get() - g_start_tick) / 1000;

    snprintf(buf, sizeof(buf), "SCORE %d", g_score);
    lv_label_set_text(g_score_label, buf);

    snprintf(buf, sizeof(buf), "SPEED %d", g_speed);
    lv_label_set_text(g_speed_label, buf);

    snprintf(buf, sizeof(buf), "TIME %lu", (unsigned long)elapsed);
    lv_label_set_text(g_time_label, buf);
}

static void hide_game_over(void)
{
    if (g_msg_panel) {
        lv_obj_add_flag(g_msg_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

static void show_game_over(void)
{
    char buf[40];
    g_state = RG_STATE_GAME_OVER;
    snprintf(buf, sizeof(buf), "Score: %d", g_score);
    lv_label_set_text(g_msg_score, buf);
    lv_obj_clear_flag(g_msg_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_msg_panel);
    lv_obj_move_foreground(g_touch);
}

static void reset_game(void)
{
    g_state = RG_STATE_PLAYING;
    g_steer = 0;
    g_player_x = RG_W / 2;
    g_marker_phase = 0.0f;
    g_score = 0;
    g_speed = 95;
    g_start_tick = lv_tick_get();
    g_last_tick = g_start_tick;
    g_next_spawn_tick = g_start_tick + 900;

    for (int i = 0; i < RG_MAX_ENEMIES; i++) {
        g_enemies[i].active = false;
        g_enemies[i].z = 0.0f;
        lv_obj_add_flag(g_enemies[i].body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_enemies[i].shine, LV_OBJ_FLAG_HIDDEN);
    }

    hide_game_over();
    update_hud();
}

static void place_player(void)
{
    int x = (int)g_player_x - RG_PLAYER_W / 2;
    lv_obj_set_pos(g_player_body, x, RG_PLAYER_Y);
    lv_obj_set_pos(g_player_cabin, x + 17, RG_PLAYER_Y + 13);
    lv_obj_set_pos(g_player_lw, x - 7, RG_PLAYER_Y + 53);
    lv_obj_set_pos(g_player_rw, x + RG_PLAYER_W - 7, RG_PLAYER_Y + 53);
}

static void spawn_enemy(void)
{
    for (int i = 0; i < RG_MAX_ENEMIES; i++) {
        if (g_enemies[i].active) continue;
        g_enemies[i].active = true;
        g_enemies[i].z = 0.03f;
        g_enemies[i].lane = (rand() % 3) - 1;
        g_enemies[i].kind = rand() % 3;
        lv_obj_clear_flag(g_enemies[i].body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(g_enemies[i].shine, LV_OBJ_FLAG_HIDDEN);
        return;
    }
}

static void update_enemy(rg_enemy_t *enemy, float dz)
{
    enemy->z += dz;
    if (enemy->z > 1.08f) {
        enemy->active = false;
        lv_obj_add_flag(enemy->body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(enemy->shine, LV_OBJ_FLAG_HIDDEN);
        g_score += 20;
        return;
    }

    int w = 18 + (int)(enemy->z * enemy->z * 70.0f);
    int h = 18 + (int)(enemy->z * enemy->z * 82.0f);
    int x = lane_x(enemy->lane, enemy->z) - w / 2;
    int y = road_y(enemy->z) - h;
    uint32_t color = (enemy->kind == 0) ? RG_COLOR_ENEMY :
                     (enemy->kind == 1) ? RG_COLOR_BLOCK : 0x9B59B6;

    lv_obj_set_size(enemy->body, w, h);
    lv_obj_set_pos(enemy->body, x, y);
    lv_obj_set_style_bg_color(enemy->body, lv_color_hex(color), 0);
    lv_obj_set_size(enemy->shine, w / 2, h / 4);
    lv_obj_set_pos(enemy->shine, x + w / 4, y + h / 5);

    if (enemy->z > 0.72f) {
        int px1 = (int)g_player_x - RG_PLAYER_W / 2 + 10;
        int px2 = (int)g_player_x + RG_PLAYER_W / 2 - 10;
        int py1 = RG_PLAYER_Y + 8;
        int py2 = RG_PLAYER_Y + RG_PLAYER_H - 4;
        int ex1 = x + 6;
        int ex2 = x + w - 6;
        int ey1 = y + 6;
        int ey2 = y + h - 4;
        if (px1 < ex2 && px2 > ex1 && py1 < ey2 && py2 > ey1) {
            show_game_over();
        }
    }
}

static void update_lanes(float dz)
{
    g_marker_phase += dz * 2.8f;
    while (g_marker_phase > 1.0f) {
        g_marker_phase -= 1.0f;
    }

    for (int i = 0; i < RG_LANE_MARKERS; i++) {
        float z = ((float)i + g_marker_phase) / RG_LANE_MARKERS;
        z = clampf(z, 0.02f, 0.98f);
        int y = road_y(z);
        int h = 8 + (int)(z * z * 32.0f);
        int w = 3 + (int)(z * 9.0f);

        int left_x = (int)(RG_W / 2 - road_half_width(z) * 0.19f);
        int right_x = (int)(RG_W / 2 + road_half_width(z) * 0.19f);
        lv_obj_t *m = g_lane_marker[i];

        lv_obj_set_size(m, w, h);
        lv_obj_set_pos(m, (i & 1) ? right_x : left_x, y);
        lv_obj_set_style_bg_opa(m, (lv_opa_t)(LV_OPA_50 + z * 90), 0);
    }
}

static void game_tick(lv_timer_t *timer)
{
    (void)timer;
    uint32_t now = lv_tick_get();
    uint32_t elapsed_ms = now - g_last_tick;
    g_last_tick = now;
    float dt = elapsed_ms / 16.0f;

    if (g_state != RG_STATE_PLAYING) return;

    g_speed = 95 + (int)((now - g_start_tick) / 260);
    if (g_speed > 220) g_speed = 220;

    g_player_x += (float)g_steer * (4.2f + g_speed * 0.015f) * dt;
    g_player_x = clampf(g_player_x, 165.0f, 635.0f);
    place_player();

    float dz = (0.0065f + g_speed * 0.000022f) * dt;
    update_lanes(dz);

    if (now >= g_next_spawn_tick) {
        spawn_enemy();
        uint32_t gap = 1050 - (uint32_t)((g_speed - 95) * 3);
        if (gap < 560) gap = 560;
        g_next_spawn_tick = now + gap + (rand() % 420);
    }

    for (int i = 0; i < RG_MAX_ENEMIES; i++) {
        if (g_enemies[i].active) {
            update_enemy(&g_enemies[i], dz);
        }
    }

    g_score += 1 + (g_speed / 120);
    update_hud();
}

static void touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);

    if (code == LV_EVENT_RELEASED) {
        if (pt.x <= 112 && pt.y <= 58) {
            back_to_menu();
            return;
        }
        if (g_state == RG_STATE_GAME_OVER) {
            if (pt.x >= 255 && pt.x <= 375 && pt.y >= 292 && pt.y <= 344) {
                reset_game();
                return;
            }
            if (pt.x >= 425 && pt.x <= 545 && pt.y >= 292 && pt.y <= 344) {
                back_to_menu();
                return;
            }
        }
        g_steer = 0;
        return;
    }

    if (g_state != RG_STATE_PLAYING) return;
    if (pt.y < 64) {
        g_steer = 0;
    } else {
        g_steer = (pt.x < RG_W / 2) ? -1 : 1;
    }
}

static lv_obj_t *rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                       uint32_t color, lv_align_t align, int x, int y)
{
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, text);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, lv_color_hex(color), 0);
    lv_obj_align(obj, align, x, y);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    return obj;
}

static void create_scene(void)
{
    rect(g_scr, 0, 0, RG_W, 80, RG_COLOR_SKY_TOP, 0);
    rect(g_scr, 0, 80, RG_W, 70, RG_COLOR_SKY_LOW, 0);
    rect(g_scr, 0, RG_HORIZON_Y, RG_W, RG_H - RG_HORIZON_Y, RG_COLOR_GRASS, 0);

    static lv_point_t mountains[] = {
        {0, 146}, {90, 94}, {170, 145}, {260, 96}, {365, 148},
        {470, 88}, {575, 146}, {680, 101}, {800, 150}
    };
    lv_obj_t *line = lv_line_create(g_scr);
    lv_line_set_points(line, mountains, sizeof(mountains) / sizeof(mountains[0]));
    lv_obj_set_style_line_width(line, 34, 0);
    lv_obj_set_style_line_color(line, lv_color_hex(RG_COLOR_MOUNTAIN), 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < RG_ROAD_STRIPS; i++) {
        float z1 = (float)i / RG_ROAD_STRIPS;
        float z2 = (float)(i + 1) / RG_ROAD_STRIPS;
        int y1 = road_y(z1);
        int y2 = road_y(z2);
        int w = (int)(road_half_width(z2) * 2.0f);
        int x = RG_W / 2 - w / 2;
        uint32_t color = (i & 1) ? RG_COLOR_ROAD : RG_COLOR_ROAD_ALT;
        g_road_strip[i] = rect(g_scr, x, y1, w, y2 - y1 + 2, color, 0);
    }

    for (int i = 0; i < RG_LANE_MARKERS; i++) {
        g_lane_marker[i] = rect(g_scr, -20, -20, 4, 12, RG_COLOR_LINE, 2);
    }
}

static void create_hud(void)
{
    rect(g_scr, 0, 0, RG_W, 58, RG_COLOR_HUD, 0);
    rect(g_scr, 12, 10, 92, 38, 0x263445, 8);
    label(g_scr, "Back", &lv_font_montserrat_16, RG_COLOR_TEXT, LV_ALIGN_TOP_LEFT, 34, 19);
    g_score_label = label(g_scr, "SCORE 0", &lv_font_montserrat_20,
                          RG_COLOR_TEXT, LV_ALIGN_TOP_LEFT, 145, 18);
    g_speed_label = label(g_scr, "SPEED 95", &lv_font_montserrat_20,
                          RG_COLOR_TEXT, LV_ALIGN_TOP_MID, 0, 18);
    g_time_label = label(g_scr, "TIME 0", &lv_font_montserrat_20,
                         RG_COLOR_TEXT, LV_ALIGN_TOP_RIGHT, -145, 18);
}

static void create_player(void)
{
    g_player_body = rect(g_scr, 0, 0, RG_PLAYER_W, RG_PLAYER_H, RG_COLOR_PLAYER, 12);
    g_player_cabin = rect(g_scr, 0, 0, 36, 30, RG_COLOR_PLAYER_TOP, 8);
    g_player_lw = rect(g_scr, 0, 0, 14, 30, 0x151A22, 5);
    g_player_rw = rect(g_scr, 0, 0, 14, 30, 0x151A22, 5);
    place_player();
}

static void create_enemies(void)
{
    for (int i = 0; i < RG_MAX_ENEMIES; i++) {
        g_enemies[i].body = rect(g_scr, -100, -100, 24, 24, RG_COLOR_ENEMY, 6);
        g_enemies[i].shine = rect(g_scr, -100, -100, 10, 5, 0xFFFFFF, 3);
        lv_obj_set_style_bg_opa(g_enemies[i].shine, LV_OPA_50, 0);
        lv_obj_add_flag(g_enemies[i].body, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_enemies[i].shine, LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_game_over_panel(void)
{
    g_msg_panel = rect(g_scr, 215, 170, 370, 190, 0x111827, 12);
    lv_obj_set_style_bg_opa(g_msg_panel, LV_OPA_90, 0);
    g_msg_title = label(g_msg_panel, "GAME OVER", &lv_font_montserrat_28,
                        0xFFFFFF, LV_ALIGN_TOP_MID, 0, 26);
    g_msg_score = label(g_msg_panel, "Score: 0", &lv_font_montserrat_20,
                        0xD1D5DB, LV_ALIGN_TOP_MID, 0, 72);
    (void)g_msg_title;

    rect(g_msg_panel, 40, 122, 120, 52, 0x27AE60, 8);
    label(g_msg_panel, "Restart", &lv_font_montserrat_16,
          0xFFFFFF, LV_ALIGN_BOTTOM_LEFT, 68, -17);
    rect(g_msg_panel, 210, 122, 120, 52, 0x34495E, 8);
    label(g_msg_panel, "Back", &lv_font_montserrat_16,
          0xFFFFFF, LV_ALIGN_BOTTOM_RIGHT, -252, -17);
    lv_obj_add_flag(g_msg_panel, LV_OBJ_FLAG_HIDDEN);
}

void racing_game_start(void)
{
    g_scr = lv_obj_create(NULL);
    lv_scr_load(g_scr);
    lv_obj_set_style_bg_color(g_scr, lv_color_hex(RG_COLOR_SKY_TOP), 0);
    lv_obj_clear_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);

    srand((unsigned int)lv_tick_get());
    g_timer = NULL;
    create_scene();
    create_hud();
    create_enemies();
    create_player();
    create_game_over_panel();

    g_touch = lv_obj_create(g_scr);
    lv_obj_set_size(g_touch, RG_W, RG_H);
    lv_obj_set_pos(g_touch, 0, 0);
    lv_obj_set_style_bg_opa(g_touch, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_touch, 0, 0);
    lv_obj_clear_flag(g_touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_touch, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(g_touch, touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(g_touch, touch_cb, LV_EVENT_RELEASED, NULL);

    reset_game();
    g_timer = lv_timer_create(game_tick, RG_FRAME_MS, NULL);
}
