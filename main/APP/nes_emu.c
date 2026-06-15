/**
 ****************************************************************************************************
 * @file        nes_emu.c
 * @brief       FC/NES 模拟器 for DNESP32S3 (4.3" RGB LCD 800x480, LVGL v8)
 *
 *  基于 Nofrendo (retro-go 版本) 移植
 *  支持从 SD 卡加载 .nes ROM 文件
 *  触摸屏虚拟按键控制
 ****************************************************************************************************
 */

#include "nes_emu.h"
#include "menu.h"
#include "spi_sdcard.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Nofrendo 头文件 */
#include <nofrendo.h>
#include <nes/nes.h>
#include <nes/state.h>
#include <nes/ppu.h>
#include <nes/input.h>

static const char *TAG = "NES_EMU";

/* ── ROM 文件浏览器配置 ───────────────────────────── */
#define ROM_DIR_PRIMARY   "/sdcard/FC game"
#define ROM_DIR_FALLBACK  "/sdcard/FC"
#define MAX_ROMS          256
#define ROM_NAME_MAX      256

/* ── NES 屏幕配置 ───────────────────────────────── */
#define NES_W           256
#define NES_H           240
#define SCALE           2
#define NES_SCREEN_PITCH  (8 + 256 + 8)  /* NES_SCREEN_PITCH from nes.h */

/* 音频采样率 */
#define AUDIO_SAMPLE_RATE 44100

/* ── 模拟器状态 ─────────────────────────────────── */
typedef enum {
    EMU_STATE_IDLE = 0,
    EMU_STATE_RUNNING,
    EMU_STATE_PAUSED,
    EMU_STATE_STOPPING
} emu_state_t;

/* ── 内部变量 ───────────────────────────────────── */
static lv_obj_t    *g_nes_scr       = NULL;
static lv_obj_t    *g_canvas        = NULL;
static lv_timer_t  *g_emu_timer     = NULL;
static emu_state_t  g_emu_state     = EMU_STATE_IDLE;
static TaskHandle_t g_emu_task       = NULL;

/* NES 帧缓冲 (RGB565) */
static uint16_t    *g_framebuf      = NULL;
static uint16_t    *g_scalebuf      = NULL;  /* 缩放后的缓冲 */
static uint8       *g_vidbuf        = NULL;  /* nofrendo 视频缓冲 */

/* NES 实例 */
static nes_t       *g_nes           = NULL;

/* ROM 文件列表 */
static char *g_rom_list[MAX_ROMS];
static int   g_rom_count = 0;

/* 虚拟按键状态 */
static uint32_t g_joystick = 0;

/* ── 前向声明 ───────────────────────────────────── */
static void nes_emu_task_func(void *arg);
static void blit_screen(uint8 *bmp);
static void scale_frame(uint16_t *src, uint16_t *dst);

/* ── UI 回调 ────────────────────────────────────── */
static void nes_btn_cb(lv_event_t *e);
static void nes_back_cb(lv_event_t *e);
static void rom_item_cb(lv_event_t *e);

/* ══════════════════════════════════════════════════════════════════════════════
   ROM 文件浏览器
   ══════════════════════════════════════════════════════════════════════════════ */

static void scan_rom_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) {
        ESP_LOGW(TAG, "无法打开目录: %s", dir);
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && g_rom_count < MAX_ROMS) {
        if (ent->d_type != DT_REG) continue;

        char *ext = strrchr(ent->d_name, '.');
        if (!ext) continue;

        /* 支持 .nes 和 .zip 文件 */
        if (strcasecmp(ext, ".nes") != 0 && strcasecmp(ext, ".zip") != 0) continue;

        char *name = malloc(ROM_NAME_MAX);
        if (name) {
            snprintf(name, ROM_NAME_MAX, "%s", ent->d_name);
            g_rom_list[g_rom_count++] = name;
            ESP_LOGI(TAG, "找到 ROM: %s", name);
        }
    }
    closedir(d);
}

static void free_rom_list(void)
{
    for (int i = 0; i < g_rom_count; i++) {
        free(g_rom_list[i]);
        g_rom_list[i] = NULL;
    }
    g_rom_count = 0;
}

/* ROM 列表 UI */
static lv_obj_t *g_rom_scr = NULL;
static lv_obj_t *g_rom_list_obj = NULL;

static void create_rom_browser_ui(void)
{
    g_rom_scr = lv_obj_create(NULL);
    lv_scr_load(g_rom_scr);

    /* 背景 */
    lv_obj_set_style_bg_color(g_rom_scr, lv_color_hex(0x1A1A2E), 0);

    /* 标题 */
    lv_obj_t *title = lv_label_create(g_rom_scr);
    lv_label_set_text(title, "FC Game List");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE0E0E0), 0);
    lv_obj_set_pos(title, 20, 15);

    /* 返回按钮 */
    lv_obj_t *back_btn = lv_btn_create(g_rom_scr);
    lv_obj_set_size(back_btn, 100, 40);
    lv_obj_set_pos(back_btn, 680, 10);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xE05050), 0);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "Back");
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, nes_back_cb, LV_EVENT_CLICKED, NULL);

    /* ROM 列表 */
    g_rom_list_obj = lv_list_create(g_rom_scr);
    lv_obj_set_size(g_rom_list_obj, 760, 410);
    lv_obj_set_pos(g_rom_list_obj, 20, 60);
    lv_obj_set_style_bg_color(g_rom_list_obj, lv_color_hex(0x2D2D44), 0);
    lv_obj_set_style_border_color(g_rom_list_obj, lv_color_hex(0x4A4A6A), 0);

    /* 扫描 ROM 目录 */
    g_rom_count = 0;
    scan_rom_dir(ROM_DIR_PRIMARY);
    if (g_rom_count == 0) {
        scan_rom_dir(ROM_DIR_FALLBACK);
    }

    if (g_rom_count == 0) {
        lv_obj_t *empty = lv_label_create(g_rom_list_obj);
        lv_label_set_text(empty, "No .nes files found in /sdcard/FC game/");
        lv_obj_set_style_text_color(empty, lv_color_hex(0xAAAAAA), 0);
    } else {
        for (int i = 0; i < g_rom_count; i++) {
            lv_obj_t *btn = lv_list_add_btn(g_rom_list_obj, LV_SYMBOL_PLAY, g_rom_list[i]);
            lv_obj_set_style_text_color(btn, lv_color_hex(0xE0E0E0), 0);
            lv_obj_add_event_cb(btn, rom_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }

    /* 提示信息 */
    lv_obj_t *hint = lv_label_create(g_rom_scr);
    lv_label_set_text(hint, "Put .nes files in /sdcard/FC game/ folder");
    lv_obj_set_style_text_color(hint, lv_color_hex(0x888888), 0);
    lv_obj_set_pos(hint, 20, 450);
}

static void rom_item_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_rom_count) return;

    char rom_path[256];
    snprintf(rom_path, sizeof(rom_path), "%s/%s", ROM_DIR_PRIMARY, g_rom_list[idx]);

    /* 检查文件是否存在 */
    FILE *f = fopen(rom_path, "rb");
    if (!f) {
        /* 尝试 fallback 目录 */
        snprintf(rom_path, sizeof(rom_path), "%s/%s", ROM_DIR_FALLBACK, g_rom_list[idx]);
        f = fopen(rom_path, "rb");
        if (!f) {
            ESP_LOGE(TAG, "无法打开 ROM: %s", rom_path);
            return;
        }
    }
    fclose(f);

    ESP_LOGI(TAG, "加载 ROM: %s", rom_path);

    /* 清理 ROM 列表 UI */
    free_rom_list();
    if (g_rom_scr) {
        lv_obj_del(g_rom_scr);
        g_rom_scr = NULL;
    }

    /* 启动模拟器 */
    nes_emu_start(rom_path);
}

void nes_rom_browser_start(void)
{
    ESP_LOGI(TAG, "启动 ROM 浏览器");
    create_rom_browser_ui();
}

/* ══════════════════════════════════════════════════════════════════════════════
   Nofrendo 回调
   ══════════════════════════════════════════════════════════════════════════════ */

/* 显示回调 - 将 NES 画面写入帧缓冲 */
static void blit_screen(uint8 *bmp)
{
    if (!g_framebuf || !g_nes) return;

    /* nofrendo 的 vidbuf 是 RGB565 格式的帧缓冲 */
    /* 直接复制到我们的帧缓冲 */
    uint16_t *src = (uint16_t *)g_nes->vidbuf;
    uint16_t *dst = g_framebuf;

    for (int y = 0; y < NES_H; y++) {
        for (int x = 0; x < NES_W; x++) {
            /* 跳过 overscan 区域 (8 像素边距) */
            dst[y * NES_W + x] = src[y * NES_SCREEN_PITCH + x + 8];
        }
    }
}

/* 2x 缩放 */
static void scale_frame(uint16_t *src, uint16_t *dst)
{
    for (int y = 0; y < NES_H; y++) {
        for (int x = 0; x < NES_W; x++) {
            uint16_t pixel = src[y * NES_W + x];
            int dy = y * 2;
            int dx = x * 2;
            dst[dy * GAME_W + dx] = pixel;
            dst[dy * GAME_W + dx + 1] = pixel;
            dst[(dy + 1) * GAME_W + dx] = pixel;
            dst[(dy + 1) * GAME_W + dx + 1] = pixel;
        }
    }
}

/* ══════════════════════════════════════════════════════════════════════════════
   LVGL UI - 虚拟按键
   ══════════════════════════════════════════════════════════════════════════════ */

/* 虚拟按键布局 */
/*
 *  ┌─────────────────────────────────────────────┐
 *  │                    [游戏画面]                  │
 *  │                    512 x 480                  │
 *  │                                              │
 *  │  ┌─────┐                        ┌─────┐      │
 *  │  │  B  │      [START] [SELECT]  │  A  │      │
 *  │  └─────┘                        └─────┘      │
 *  │      ↑                           ↑          │
 *  │    ←   →                                       │
 *  │      ↓                                         │
 *  └─────────────────────────────────────────────┘
 */

#define DPAD_X      60
#define DPAD_Y      320
#define DPAD_SIZE   60
#define DPAD_GAP    5

#define BTN_A_X     680
#define BTN_A_Y     350
#define BTN_B_X     580
#define BTN_B_Y     380

#define BTN_START_X  350
#define BTN_START_Y  420
#define BTN_SELECT_X 250
#define BTN_SELECT_Y 420

static void create_virtual_buttons(lv_obj_t *parent)
{
    /* 方向键 - 上 */
    lv_obj_t *btn_up = lv_btn_create(parent);
    lv_obj_set_size(btn_up, DPAD_SIZE, DPAD_SIZE);
    lv_obj_set_pos(btn_up, DPAD_X + DPAD_SIZE + DPAD_GAP, DPAD_Y);
    lv_obj_set_style_bg_color(btn_up, lv_color_hex(0x4A4A6A), 0);
    lv_obj_t *lbl_up = lv_label_create(btn_up);
    lv_label_set_text(lbl_up, LV_SYMBOL_UP);
    lv_obj_center(lbl_up);
    lv_obj_add_flag(btn_up, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_up, nes_btn_cb, LV_EVENT_PRESSED, (void *)NES_PAD_UP);
    lv_obj_add_event_cb(btn_up, nes_btn_cb, LV_EVENT_RELEASED, (void *)NES_PAD_UP);

    /* 方向键 - 下 */
    lv_obj_t *btn_down = lv_btn_create(parent);
    lv_obj_set_size(btn_down, DPAD_SIZE, DPAD_SIZE);
    lv_obj_set_pos(btn_down, DPAD_X + DPAD_SIZE + DPAD_GAP, DPAD_Y + (DPAD_SIZE + DPAD_GAP) * 2);
    lv_obj_set_style_bg_color(btn_down, lv_color_hex(0x4A4A6A), 0);
    lv_obj_t *lbl_down = lv_label_create(btn_down);
    lv_label_set_text(lbl_down, LV_SYMBOL_DOWN);
    lv_obj_center(lbl_down);
    lv_obj_add_flag(btn_down, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_down, nes_btn_cb, LV_EVENT_PRESSED, (void *)NES_PAD_DOWN);
    lv_obj_add_event_cb(btn_down, nes_btn_cb, LV_EVENT_RELEASED, (void *)NES_PAD_DOWN);

    /* 方向键 - 左 */
    lv_obj_t *btn_left = lv_btn_create(parent);
    lv_obj_set_size(btn_left, DPAD_SIZE, DPAD_SIZE);
    lv_obj_set_pos(btn_left, DPAD_X, DPAD_Y + DPAD_SIZE + DPAD_GAP);
    lv_obj_set_style_bg_color(btn_left, lv_color_hex(0x4A4A6A), 0);
    lv_obj_t *lbl_left = lv_label_create(btn_left);
    lv_label_set_text(lbl_left, LV_SYMBOL_LEFT);
    lv_obj_center(lbl_left);
    lv_obj_add_flag(btn_left, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_left, nes_btn_cb, LV_EVENT_PRESSED, (void *)NES_PAD_LEFT);
    lv_obj_add_event_cb(btn_left, nes_btn_cb, LV_EVENT_RELEASED, (void *)NES_PAD_LEFT);

    /* 方向键 - 右 */
    lv_obj_t *btn_right = lv_btn_create(parent);
    lv_obj_set_size(btn_right, DPAD_SIZE, DPAD_SIZE);
    lv_obj_set_pos(btn_right, DPAD_X + (DPAD_SIZE + DPAD_GAP) * 2, DPAD_Y + DPAD_SIZE + DPAD_GAP);
    lv_obj_set_style_bg_color(btn_right, lv_color_hex(0x4A4A6A), 0);
    lv_obj_t *lbl_right = lv_label_create(btn_right);
    lv_label_set_text(lbl_right, LV_SYMBOL_RIGHT);
    lv_obj_center(lbl_right);
    lv_obj_add_flag(btn_right, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_right, nes_btn_cb, LV_EVENT_PRESSED, (void *)NES_PAD_RIGHT);
    lv_obj_add_event_cb(btn_right, nes_btn_cb, LV_EVENT_RELEASED, (void *)NES_PAD_RIGHT);

    /* A 按钮 */
    lv_obj_t *btn_a = lv_btn_create(parent);
    lv_obj_set_size(btn_a, 80, 80);
    lv_obj_set_pos(btn_a, BTN_A_X, BTN_A_Y);
    lv_obj_set_style_bg_color(btn_a, lv_color_hex(0xE05050), 0);
    lv_obj_set_style_radius(btn_a, 40, 0);
    lv_obj_t *lbl_a = lv_label_create(btn_a);
    lv_label_set_text(lbl_a, "A");
    lv_obj_set_style_text_font(lbl_a, &lv_font_montserrat_28, 0);
    lv_obj_center(lbl_a);
    lv_obj_add_flag(btn_a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_a, nes_btn_cb, LV_EVENT_PRESSED, (void *)NES_PAD_A);
    lv_obj_add_event_cb(btn_a, nes_btn_cb, LV_EVENT_RELEASED, (void *)NES_PAD_A);

    /* B 按钮 */
    lv_obj_t *btn_b = lv_btn_create(parent);
    lv_obj_set_size(btn_b, 80, 80);
    lv_obj_set_pos(btn_b, BTN_B_X, BTN_B_Y);
    lv_obj_set_style_bg_color(btn_b, lv_color_hex(0x50A060), 0);
    lv_obj_set_style_radius(btn_b, 40, 0);
    lv_obj_t *lbl_b = lv_label_create(btn_b);
    lv_label_set_text(lbl_b, "B");
    lv_obj_set_style_text_font(lbl_b, &lv_font_montserrat_28, 0);
    lv_obj_center(lbl_b);
    lv_obj_add_flag(btn_b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_b, nes_btn_cb, LV_EVENT_PRESSED, (void *)NES_PAD_B);
    lv_obj_add_event_cb(btn_b, nes_btn_cb, LV_EVENT_RELEASED, (void *)NES_PAD_B);

    /* START 按钮 */
    lv_obj_t *btn_start = lv_btn_create(parent);
    lv_obj_set_size(btn_start, 80, 40);
    lv_obj_set_pos(btn_start, BTN_START_X, BTN_START_Y);
    lv_obj_set_style_bg_color(btn_start, lv_color_hex(0x4A4A6A), 0);
    lv_obj_t *lbl_start = lv_label_create(btn_start);
    lv_label_set_text(lbl_start, "START");
    lv_obj_set_style_text_font(lbl_start, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl_start);
    lv_obj_add_flag(btn_start, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_start, nes_btn_cb, LV_EVENT_PRESSED, (void *)NES_PAD_START);
    lv_obj_add_event_cb(btn_start, nes_btn_cb, LV_EVENT_RELEASED, (void *)NES_PAD_START);

    /* SELECT 按钮 */
    lv_obj_t *btn_select = lv_btn_create(parent);
    lv_obj_set_size(btn_select, 80, 40);
    lv_obj_set_pos(btn_select, BTN_SELECT_X, BTN_SELECT_Y);
    lv_obj_set_style_bg_color(btn_select, lv_color_hex(0x4A4A6A), 0);
    lv_obj_t *lbl_select = lv_label_create(btn_select);
    lv_label_set_text(lbl_select, "SELECT");
    lv_obj_set_style_text_font(lbl_select, &lv_font_montserrat_12, 0);
    lv_obj_center(lbl_select);
    lv_obj_add_flag(btn_select, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_select, nes_btn_cb, LV_EVENT_PRESSED, (void *)NES_PAD_SELECT);
    lv_obj_add_event_cb(btn_select, nes_btn_cb, LV_EVENT_RELEASED, (void *)NES_PAD_SELECT);

    /* 返回按钮 */
    lv_obj_t *btn_back = lv_btn_create(parent);
    lv_obj_set_size(btn_back, 80, 40);
    lv_obj_set_pos(btn_back, 10, 10);
    lv_obj_set_style_bg_color(btn_back, lv_color_hex(0xE05050), 0);
    lv_obj_t *lbl_back = lv_label_create(btn_back);
    lv_label_set_text(lbl_back, "Back");
    lv_obj_center(lbl_back);
    lv_obj_add_event_cb(btn_back, nes_back_cb, LV_EVENT_CLICKED, NULL);
}

/* 按键回调 */
static void nes_btn_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    uint32_t btn = (uint32_t)(uintptr_t)lv_event_get_user_data(e);

    if (code == LV_EVENT_PRESSED) {
        g_joystick |= btn;
    } else if (code == LV_EVENT_RELEASED) {
        g_joystick &= ~btn;
    }
}

/* 返回按钮回调 */
static void nes_back_cb(lv_event_t *e)
{
    nes_emu_stop();
}

/* canvas 绘制回调 */
static void nes_canvas_draw_cb(lv_event_t *e)
{
    lv_draw_ctx_t *draw_ctx = lv_event_get_draw_ctx(e);
    if (!draw_ctx || !g_scalebuf) return;

    /* 创建图像描述符 */
    lv_img_dsc_t img_dsc;
    img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    img_dsc.header.w = GAME_W;
    img_dsc.header.h = GAME_H;
    img_dsc.data = (uint8_t *)g_scalebuf;
    img_dsc.data_size = GAME_W * GAME_H * 2;

    /* 绘制到 canvas */
    lv_draw_img_dsc_t img_draw_dsc;
    lv_draw_img_dsc_init(&img_draw_dsc);

    lv_area_t coords;
    coords.x1 = GAME_X;
    coords.y1 = GAME_Y;
    coords.x2 = GAME_X + GAME_W - 1;
    coords.y2 = GAME_Y + GAME_H - 1;

    lv_draw_img(draw_ctx, &img_draw_dsc, &coords, &img_dsc);
}

/* 创建模拟器 UI */
static void create_emu_ui(void)
{
    g_nes_scr = lv_obj_create(NULL);
    lv_scr_load(g_nes_scr);

    /* 黑色背景 */
    lv_obj_set_style_bg_color(g_nes_scr, lv_color_hex(0x000000), 0);

    /* 游戏画面区域 */
    g_canvas = lv_obj_create(g_nes_scr);
    lv_obj_set_size(g_canvas, GAME_W, GAME_H);
    lv_obj_set_pos(g_canvas, GAME_X, GAME_Y);
    lv_obj_set_style_bg_color(g_canvas, lv_color_hex(0x000000), 0);
    lv_obj_set_style_border_width(g_canvas, 0, 0);
    lv_obj_add_event_cb(g_canvas, nes_canvas_draw_cb, LV_EVENT_DRAW_MAIN, NULL);

    /* 创建虚拟按键 */
    create_virtual_buttons(g_nes_scr);
}

/* ══════════════════════════════════════════════════════════════════════════════
   模拟器任务
   ══════════════════════════════════════════════════════════════════════════════ */

static void nes_emu_task_func(void *arg)
{
    const char *rom_path = (const char *)arg;
    ESP_LOGI(TAG, "模拟器任务启动: %s", rom_path);

    /* 初始化 nofrendo */
    if (nofrendo_init(SYS_DETECT, AUDIO_SAMPLE_RATE, true, blit_screen, NULL, NULL) != 0) {
        ESP_LOGE(TAG, "nofrendo 初始化失败");
        goto cleanup;
    }

    /* 加载 ROM */
    int ret = nes_loadfile(rom_path);
    if (ret != 0) {
        ESP_LOGE(TAG, "加载 ROM 失败: %d", ret);
        goto cleanup;
    }

    g_nes = nes_getptr();

    /* 设置视频缓冲 */
    nes_setvidbuf(g_vidbuf);

    /* 运行模拟器 */
    ESP_LOGI(TAG, "开始模拟");
    while (g_emu_state == EMU_STATE_RUNNING) {
        /* 更新输入 */
        input_update(0, g_joystick);

        /* 执行一帧 */
        nes_emulate(true);

        /* 缩放帧 */
        if (g_framebuf && g_scalebuf) {
            scale_frame(g_framebuf, g_scalebuf);
            lv_obj_invalidate(g_canvas);
        }

        /* 控制帧率 */
        vTaskDelay(pdMS_TO_TICKS(16));
    }

cleanup:
    ESP_LOGI(TAG, "模拟器任务结束");
    g_nes = NULL;
    g_emu_task = NULL;
    vTaskDelete(NULL);
}

/* ══════════════════════════════════════════════════════════════════════════════
   公共接口
   ══════════════════════════════════════════════════════════════════════════════ */

void nes_emu_start(const char *rom_path)
{
    ESP_LOGI(TAG, "启动 NES 模拟器: %s", rom_path);

    /* 分配 nofrendo 视频缓冲 (需要 8 像素边距) */
    g_vidbuf = heap_caps_malloc(NES_SCREEN_PITCH * NES_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!g_vidbuf) {
        ESP_LOGE(TAG, "分配视频缓冲失败");
        return;
    }
    memset(g_vidbuf, 0, NES_SCREEN_PITCH * NES_H * sizeof(uint16_t));

    /* 分配帧缓冲 (PSRAM) */
    g_framebuf = heap_caps_malloc(NES_W * NES_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!g_framebuf) {
        ESP_LOGE(TAG, "分配帧缓冲失败");
        free(g_vidbuf);
        g_vidbuf = NULL;
        return;
    }
    memset(g_framebuf, 0, NES_W * NES_H * sizeof(uint16_t));

    /* 分配缩放缓冲 (PSRAM) */
    g_scalebuf = heap_caps_malloc(GAME_W * GAME_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!g_scalebuf) {
        ESP_LOGE(TAG, "分配缩放缓冲失败");
        free(g_vidbuf);
        free(g_framebuf);
        g_vidbuf = NULL;
        g_framebuf = NULL;
        return;
    }
    memset(g_scalebuf, 0, GAME_W * GAME_H * sizeof(uint16_t));

    /* 创建 UI */
    create_emu_ui();

    /* 启动模拟器任务 */
    g_emu_state = EMU_STATE_RUNNING;

    /* 复制 ROM 路径 */
    char *path_copy = strdup(rom_path);
    xTaskCreatePinnedToCore(nes_emu_task_func, "nes_emu", 16384, path_copy, 5, &g_emu_task, 1);
}

void nes_emu_stop(void)
{
    ESP_LOGI(TAG, "停止 NES 模拟器");

    /* 停止模拟器任务 */
    g_emu_state = EMU_STATE_STOPPING;
    while (g_emu_task != NULL) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    /* 释放资源 */
    if (g_vidbuf) {
        free(g_vidbuf);
        g_vidbuf = NULL;
    }
    if (g_framebuf) {
        free(g_framebuf);
        g_framebuf = NULL;
    }
    if (g_scalebuf) {
        free(g_scalebuf);
        g_scalebuf = NULL;
    }

    /* 清理 UI */
    if (g_nes_scr) {
        lv_obj_del(g_nes_scr);
        g_nes_scr = NULL;
        g_canvas = NULL;
    }

    /* 返回菜单 */
    menu_go_back();
}
