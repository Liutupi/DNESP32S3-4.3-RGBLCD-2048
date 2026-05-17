/**
 ****************************************************************************************************
 * @file        game2048.c
 * @author      ALIENTEK / Modified
 * @version     V1.0
 * @date        2025-05-17
 * @brief       2048 Game for LVGL on 4.3inch RGB LCD (800x480)
 * @license     Copyright (c) 2020-2032
 ****************************************************************************************************
 */

#include "game2048.h"
#include "menu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Game board */
static int g_board[4][4];
static int g_score = 0;
static int g_best_score = 0;

/* UI handles */
static lv_obj_t *g_score_label = NULL;
static lv_obj_t *g_best_label = NULL;
static lv_obj_t *g_overlay = NULL;
static lv_obj_t *g_overlay_text = NULL;
static lv_obj_t *g_tile_conts[4][4];
static lv_obj_t *g_tile_labels[4][4];

/* Touch tracking */
static lv_coord_t g_press_x = 0;
static lv_coord_t g_press_y = 0;
static bool g_pressed = false;

/* Classic 2048 color palette */
static const uint32_t TILE_BG_COLORS[12] = {
    0xCDC1B4, /* 0     empty */
    0xEEE4DA, /* 2     */
    0xEDE0C8, /* 4     */
    0xF2B179, /* 8     */
    0xF59563, /* 16    */
    0xF67C5F, /* 32    */
    0xF65E3B, /* 64    */
    0xEDCF72, /* 128   */
    0xEDCC61, /* 256   */
    0xEDC850, /* 512   */
    0xEDC53F, /* 1024  */
    0xEDC22E, /* 2048  */
};

static const lv_font_t *FONT_SMALL  = &lv_font_montserrat_14;
static const lv_font_t *FONT_NORMAL = &lv_font_montserrat_16;

#define BOARD_SIZE   410
#define TILE_GAP     10
#define TILE_SIZE    90   /* (410 - 5*10) / 4 = 90 */

/**
 * @brief Get color index from tile value
 */
static int tile_color_index(int value)
{
    if (value == 0) return 0;
    int idx = 0;
    int v = value;
    while (v > 2) {
        v >>= 1;
        idx++;
    }
    if (idx > 11) idx = 11;
    return idx;
}

/**
 * @brief Spawn a new tile (2 or 4) at random empty position
 */
static void spawn_tile(void)
{
    int empty[16][2];
    int count = 0;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (g_board[r][c] == 0) {
                empty[count][0] = r;
                empty[count][1] = c;
                count++;
            }
        }
    }
    if (count > 0) {
        int idx = rand() % count;
        int r = empty[idx][0];
        int c = empty[idx][1];
        g_board[r][c] = (rand() % 10 < 9) ? 2 : 4;
    }
}

/**
 * @brief Initialize / reset the board
 */
static void init_board(void)
{
    memset(g_board, 0, sizeof(g_board));
    g_score = 0;
    spawn_tile();
    spawn_tile();
}

/**
 * @brief Check if any move is possible
 */
static bool can_move(void)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (g_board[r][c] == 0) return true;
            if (c < 3 && g_board[r][c] == g_board[r][c + 1]) return true;
            if (r < 3 && g_board[r][c] == g_board[r + 1][c]) return true;
        }
    }
    return false;
}

/**
 * @brief Check if 2048 is reached
 */
static bool check_win(void)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (g_board[r][c] == 2048) return true;
        }
    }
    return false;
}

/**
 * @brief Move left (core logic)
 * @return true if board changed
 */
static bool move_left(void)
{
    bool moved = false;
    for (int r = 0; r < 4; r++) {
        int row[4] = {0};
        int pos = 0;
        /* Compress */
        for (int c = 0; c < 4; c++) {
            if (g_board[r][c] != 0) {
                row[pos++] = g_board[r][c];
            }
        }
        /* Merge */
        for (int c = 0; c < 3; c++) {
            if (row[c] != 0 && row[c] == row[c + 1]) {
                row[c] <<= 1;
                g_score += row[c];
                row[c + 1] = 0;
                c++; /* skip next */
            }
        }
        /* Compress again */
        int final[4] = {0};
        pos = 0;
        for (int c = 0; c < 4; c++) {
            if (row[c] != 0) {
                final[pos++] = row[c];
            }
        }
        /* Write back */
        for (int c = 0; c < 4; c++) {
            if (g_board[r][c] != final[c]) moved = true;
            g_board[r][c] = final[c];
        }
    }
    return moved;
}

/**
 * @brief Rotate board clockwise by 90 degrees
 */
static void rotate_board(void)
{
    int tmp[4][4];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            tmp[c][3 - r] = g_board[r][c];
        }
    }
    memcpy(g_board, tmp, sizeof(g_board));
}

static bool move_right(void)
{
    rotate_board(); rotate_board();
    bool m = move_left();
    rotate_board(); rotate_board();
    return m;
}

static bool move_up(void)
{
    rotate_board();
    bool m = move_left();
    rotate_board(); rotate_board(); rotate_board();
    return m;
}

static bool move_down(void)
{
    rotate_board(); rotate_board(); rotate_board();
    bool m = move_left();
    rotate_board();
    return m;
}

/**
 * @brief Update all UI elements from board state
 */
static void update_ui(void)
{
    char buf[16];

    /* Score */
    snprintf(buf, sizeof(buf), "%d", g_score);
    lv_label_set_text(g_score_label, buf);

    if (g_score > g_best_score) {
        g_best_score = g_score;
        snprintf(buf, sizeof(buf), "%d", g_best_score);
        lv_label_set_text(g_best_label, buf);
    }

    /* Tiles */
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            int val = g_board[r][c];
            int idx = tile_color_index(val);
            lv_obj_set_style_bg_color(g_tile_conts[r][c], lv_color_hex(TILE_BG_COLORS[idx]), 0);

            if (val == 0) {
                lv_label_set_text(g_tile_labels[r][c], "");
            } else {
                snprintf(buf, sizeof(buf), "%d", val);
                lv_label_set_text(g_tile_labels[r][c], buf);
            }

            /* Text color: dark for small, white for large */
            if (idx < 3) {
                lv_obj_set_style_text_color(g_tile_labels[r][c], lv_color_hex(0x776E65), 0);
            } else {
                lv_obj_set_style_text_color(g_tile_labels[r][c], lv_color_hex(0xF9F6F2), 0);
            }

            /* Font size based on digits */
            int digits = (val == 0) ? 0 : snprintf(buf, sizeof(buf), "%d", val);
            if (digits >= 4) {
                lv_obj_set_style_text_font(g_tile_labels[r][c], FONT_SMALL, 0);
            } else {
                lv_obj_set_style_text_font(g_tile_labels[r][c], FONT_NORMAL, 0);
            }
        }
    }
}

/**
 * @brief Execute a move direction
 */
static void do_move(lv_dir_t dir)
{
    bool moved = false;
    switch (dir) {
        case LV_DIR_LEFT:  moved = move_left();  break;
        case LV_DIR_RIGHT: moved = move_right(); break;
        case LV_DIR_TOP:   moved = move_up();    break;
        case LV_DIR_BOTTOM:moved = move_down();  break;
        default: break;
    }

    if (moved) {
        spawn_tile();
        update_ui();

        if (check_win()) {
            lv_label_set_text(g_overlay_text, "You Win!");
            lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
        } else if (!can_move()) {
            lv_label_set_text(g_overlay_text, "Game Over");
            lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/**
 * @brief Touch event callback for the game board
 */
static void board_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    lv_point_t pt;

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &pt);
        g_press_x = pt.x;
        g_press_y = pt.y;
        g_pressed = true;
    } else if (code == LV_EVENT_RELEASED && g_pressed) {
        lv_indev_get_point(indev, &pt);
        lv_coord_t dx = pt.x - g_press_x;
        lv_coord_t dy = pt.y - g_press_y;
        g_pressed = false;

        if (lv_obj_has_flag(g_overlay, LV_OBJ_FLAG_HIDDEN) == false) {
            /* If overlay visible, a tap dismisses it if game over, or restarts if win */
            if (check_win() && !can_move()) {
                /* Already won and stuck - ignore */
            } else if (!can_move()) {
                init_board();
                update_ui();
                lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
            } else if (check_win()) {
                /* Continue playing after win */
                lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
            }
            return;
        }

        /* Minimum swipe distance */
        if (LV_ABS(dx) < 20 && LV_ABS(dy) < 20) return;

        if (LV_ABS(dx) > LV_ABS(dy)) {
            do_move((dx > 0) ? LV_DIR_RIGHT : LV_DIR_LEFT);
        } else {
            do_move((dy > 0) ? LV_DIR_BOTTOM : LV_DIR_TOP);
        }
    }
}

/**
 * @brief New Game button callback
 */
static void new_game_event_cb(lv_event_t *e)
{
    (void)e;
    init_board();
    update_ui();
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief Menu button callback
 */
static void menu_btn_cb(lv_event_t *e)
{
    (void)e;
    menu_go_back();
}

/**
 * @brief Create score box (SCORE or BEST)
 */
static lv_obj_t *create_score_box(lv_obj_t *parent, const char *title, int x, int y)
{
    lv_obj_t *cont = lv_obj_create(parent);
    lv_obj_set_size(cont, 110, 60);
    lv_obj_set_pos(cont, x, y);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0xBBADA0), 0);
    lv_obj_set_style_radius(cont, 6, 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_pad_all(cont, 4, 0);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl_title = lv_label_create(cont);
    lv_label_set_text(lbl_title, title);
    lv_obj_set_style_text_font(lbl_title, FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl_title, lv_color_hex(0xEEE4DA), 0);
    lv_obj_align(lbl_title, LV_ALIGN_TOP_MID, 0, 2);

    lv_obj_t *lbl_val = lv_label_create(cont);
    lv_label_set_text(lbl_val, "0");
    lv_obj_set_style_text_font(lbl_val, FONT_NORMAL, 0);
    lv_obj_set_style_text_color(lbl_val, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(lbl_val, LV_ALIGN_BOTTOM_MID, 0, -2);

    return lbl_val;
}

/**
 * @brief Start the 2048 game
 */
void game2048_start(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_scr_load(scr);
    srand((unsigned int)lv_tick_get());
    lv_obj_set_style_bg_color(scr, lv_color_hex(0xFAF8EF), 0);

    /* ---------- Left side: Game board ---------- */
    lv_obj_t *board_bg = lv_obj_create(scr);
    lv_obj_set_size(board_bg, BOARD_SIZE, BOARD_SIZE);
    lv_obj_set_pos(board_bg, 20, 60);
    lv_obj_set_style_bg_color(board_bg, lv_color_hex(0xBBADA0), 0);
    lv_obj_set_style_radius(board_bg, 10, 0);
    lv_obj_set_style_border_width(board_bg, 0, 0);
    lv_obj_clear_flag(board_bg, LV_OBJ_FLAG_SCROLLABLE);

    /* Create 4x4 tiles */
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            lv_obj_t *tile = lv_obj_create(board_bg);
            lv_obj_set_size(tile, TILE_SIZE, TILE_SIZE);
            lv_obj_set_pos(tile, TILE_GAP + c * (TILE_SIZE + TILE_GAP),
                                 TILE_GAP + r * (TILE_SIZE + TILE_GAP));
            lv_obj_set_style_radius(tile, 6, 0);
            lv_obj_set_style_border_width(tile, 0, 0);
            lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *lbl = lv_label_create(tile);
            lv_obj_set_style_text_font(lbl, FONT_NORMAL, 0);
            lv_obj_center(lbl);

            g_tile_conts[r][c] = tile;
            g_tile_labels[r][c] = lbl;
        }
    }

    /* Transparent touch layer over the board */
    lv_obj_t *touch_layer = lv_obj_create(board_bg);
    lv_obj_set_size(touch_layer, BOARD_SIZE, BOARD_SIZE);
    lv_obj_align(touch_layer, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(touch_layer, LV_OPA_0, 0);
    lv_obj_set_style_border_width(touch_layer, 0, 0);
    lv_obj_clear_flag(touch_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(touch_layer, board_event_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(touch_layer, board_event_cb, LV_EVENT_RELEASED, NULL);

    /* Overlay for Game Over / Win */
    g_overlay = lv_obj_create(board_bg);
    lv_obj_set_size(g_overlay, BOARD_SIZE, BOARD_SIZE);
    lv_obj_align(g_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(g_overlay, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g_overlay, LV_OPA_80, 0);
    lv_obj_set_style_radius(g_overlay, 10, 0);
    lv_obj_set_style_border_width(g_overlay, 0, 0);
    lv_obj_clear_flag(g_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g_overlay, LV_OBJ_FLAG_HIDDEN);

    g_overlay_text = lv_label_create(g_overlay);
    lv_label_set_text(g_overlay_text, "Game Over");
    lv_obj_set_style_text_font(g_overlay_text, FONT_NORMAL, 0);
    lv_obj_set_style_text_color(g_overlay_text, lv_color_hex(0x776E65), 0);
    lv_obj_center(g_overlay_text);

    /* ---------- Right side: Info panel ---------- */
    int info_x = 460;
    int info_y = 60;

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "2048");
    lv_obj_set_style_text_font(title, FONT_NORMAL, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x776E65), 0);
    lv_obj_set_pos(title, info_x, info_y);

    /* Score boxes */
    g_score_label = create_score_box(scr, "SCORE", info_x, info_y + 40);
    g_best_label  = create_score_box(scr, "BEST",  info_x + 120, info_y + 40);

    /* New Game button */
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 120, 40);
    lv_obj_set_pos(btn, info_x, info_y + 120);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x8F7A66), 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_add_event_cb(btn, new_game_event_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *btn_lbl = lv_label_create(btn);
    lv_label_set_text(btn_lbl, "New Game");
    lv_obj_set_style_text_color(btn_lbl, lv_color_hex(0xF9F6F2), 0);
    lv_obj_center(btn_lbl);
    lv_obj_add_event_cb(btn_lbl, new_game_event_cb, LV_EVENT_RELEASED, NULL);

    /* Menu button */
    lv_obj_t *menu_btn = lv_btn_create(scr);
    lv_obj_set_size(menu_btn, 120, 40);
    lv_obj_set_pos(menu_btn, info_x, info_y + 170);
    lv_obj_set_style_bg_color(menu_btn, lv_color_hex(0xBBADA0), 0);
    lv_obj_set_style_radius(menu_btn, 6, 0);
    lv_obj_add_event_cb(menu_btn, menu_btn_cb, LV_EVENT_RELEASED, NULL);

    lv_obj_t *menu_btn_lbl = lv_label_create(menu_btn);
    lv_label_set_text(menu_btn_lbl, "Menu");
    lv_obj_set_style_text_color(menu_btn_lbl, lv_color_hex(0xF9F6F2), 0);
    lv_obj_center(menu_btn_lbl);
    lv_obj_add_event_cb(menu_btn_lbl, menu_btn_cb, LV_EVENT_RELEASED, NULL);

    /* Instructions */
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "Swipe on the board\nto move tiles.\n\nMerge numbers\nto reach 2048!");
    lv_obj_set_style_text_font(hint, FONT_SMALL, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x8F7A66), 0);
    lv_obj_set_pos(hint, info_x, info_y + 180);

    /* Initialize game */
    init_board();
    update_ui();
}
