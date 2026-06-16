/**
 ****************************************************************************************************
 * @file        fc_emulator.c
 * @brief       FC/NES Emulator - Exclusive screen mode (no LVGL during gameplay)
 *              ROM browser uses LVGL, gameplay directly writes LCD framebuffer.
 ****************************************************************************************************
 */

#include "fc_emulator.h"
#include "menu.h"
#include "lvgl_demo.h"
#include "nofrendo.h"
#include "nes.h"
#include "spi_sdcard.h"
#include "spi.h"
#include "touch.h"
#include "lcd.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

static const char *TAG = "fc_emulator";

/* Screen */
#define SCREEN_W    800
#define SCREEN_H    480

/* NES 1x scale, centered */
#define NES_W       NES_SCREEN_WIDTH     /* 256 */
#define NES_H       NES_SCREEN_HEIGHT    /* 240 */
#define GAME_X      ((SCREEN_W - NES_W) / 2)  /* 272 */
#define GAME_Y      ((SCREEN_H - NES_H) / 2)  /* 120 */

/* ROM scanning */
#define MAX_ROMS    50
#define ROM_PATH_LEN 320
#define ROM_DIR     "/sdcard/FC"
#define ROM_DIR_ALT "/sdcard"

/* Colors (RGB565) */
#define COL_BG_RGB565   0x10A2  /* dark blue-gray */
#define COL_RED_RGB565  0xF800
#define COL_WHITE_RGB565 0xFFFF

/* ROM info */
typedef struct {
    char name[64];
    char path[320];
} rom_info_t;

/* State */
typedef enum {
    EMU_STATE_IDLE = 0,
    EMU_STATE_ROM_LIST,
    EMU_STATE_RUNNING,
} emu_state_t;

/* ── Globals ──────────────────────────────────────────── */
static emu_state_t g_state = EMU_STATE_IDLE;

/* nofrendo */
static uint16_t *g_palette565 = NULL;
static uint8_t  *g_vidbuf     = NULL;

/* Render buffer (local, then flush to LCD) */
static uint16_t *g_lcd_fb0    = NULL;  /* LCD framebuffer 0 */
static uint16_t *g_lcd_fb1    = NULL;  /* LCD framebuffer 1 */
static esp_lcd_panel_handle_t g_panel = NULL;

/* Emulator task */
static TaskHandle_t g_emu_task = NULL;
static bool g_emu_running = false;

/* Input */
static volatile uint32_t g_joystick = 0;
static volatile bool g_back_pressed = false;

/* ROM list (LVGL) */
static rom_info_t g_roms[MAX_ROMS];
static int g_rom_count = 0;

/* ── ROM scanning ─────────────────────────────────────── */
static int scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) && g_rom_count < MAX_ROMS) {
        size_t len = strlen(e->d_name);
        if (len > 4 && strcasecmp(e->d_name + len - 4, ".nes") == 0) {
            strncpy(g_roms[g_rom_count].name, e->d_name, 63);
            g_roms[g_rom_count].name[63] = '\0';
            snprintf(g_roms[g_rom_count].path, ROM_PATH_LEN, "%s/%s", dir, e->d_name);
            g_rom_count++;
            found++;
        }
    }
    closedir(d);
    return found;
}

static int scan_roms(void)
{
    g_rom_count = 0;
    int n = scan_dir(ROM_DIR);
    if (n == 0) n = scan_dir(ROM_DIR_ALT);
    ESP_LOGI(TAG, "Found %d ROMs", g_rom_count);
    return g_rom_count;
}

/* ── Draw helpers (write to BOTH LCD framebuffers) ──── */
static inline void fb_put_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;
    if (g_lcd_fb0) g_lcd_fb0[y * SCREEN_W + x] = color;
    if (g_lcd_fb1) g_lcd_fb1[y * SCREEN_W + x] = color;
}

static void fb_fill_rect(int x0, int y0, int w, int h, uint16_t color)
{
    for (int y = y0; y < y0 + h && y < SCREEN_H; y++) {
        for (int x = x0; x < x0 + w && x < SCREEN_W; x++) {
            if (g_lcd_fb0) g_lcd_fb0[y * SCREEN_W + x] = color;
            if (g_lcd_fb1) g_lcd_fb1[y * SCREEN_W + x] = color;
        }
    }
}

static void fb_clear(uint16_t color)
{
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        if (g_lcd_fb0) g_lcd_fb0[i] = color;
        if (g_lcd_fb1) g_lcd_fb1[i] = color;
    }
}

/* Simple 8x16 font for "BACK" and "FPS" text */
static const uint8_t font8x16_basic[][16] = {
    /* A */ {0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* B */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0xFC,0x00,0x00,0x00,0x00},
    /* C */ {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xC0,0xC0,0xC2,0x66,0x3C,0x00,0x00,0x00,0x00},
    /* D */ {0x00,0x00,0xF8,0x6C,0x66,0x66,0x66,0x66,0x66,0x66,0x6C,0xF8,0x00,0x00,0x00,0x00},
    /* E */ {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    /* F */ {0x00,0x00,0xFE,0x66,0x62,0x68,0x78,0x68,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* G */ {0x00,0x00,0x3C,0x66,0xC2,0xC0,0xC0,0xDE,0xC6,0xC6,0x66,0x3A,0x00,0x00,0x00,0x00},
    /* H */ {0x00,0x00,0xC6,0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* I */ {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00},
    /* K */ {0x00,0x00,0xC6,0xCC,0xD8,0xF0,0xE0,0xF0,0xD8,0xCC,0xC6,0xC6,0x00,0x00,0x00,0x00},
    /* L */ {0x00,0x00,0xF0,0x60,0x60,0x60,0x60,0x60,0x60,0x62,0x66,0xFE,0x00,0x00,0x00,0x00},
    /* P */ {0x00,0x00,0xFC,0x66,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0xF0,0x00,0x00,0x00,0x00},
    /* S */ {0x00,0x00,0x7C,0xC6,0xC6,0x60,0x38,0x0C,0xC6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 0-9 */
    /* 0 */ {0x00,0x00,0x7C,0xC6,0xC6,0xCE,0xDE,0xF6,0xE6,0xC6,0xC6,0x7C,0x00,0x00,0x00,0x00},
    /* 1 */ {0x00,0x00,0x30,0x70,0xF0,0x30,0x30,0x30,0x30,0x30,0x30,0xFC,0x00,0x00,0x00,0x00},
    /* 2 */ {0x00,0x00,0x78,0xCC,0xCC,0x0C,0x18,0x30,0x60,0xC0,0xCC,0xFC,0x00,0x00,0x00,0x00},
    /* 3 */ {0x00,0x00,0x78,0xCC,0x0C,0x0C,0x38,0x0C,0x0C,0x0C,0xCC,0x78,0x00,0x00,0x00,0x00},
    /* 4 */ {0x00,0x00,0x1C,0x3C,0x6C,0xCC,0xCC,0xFE,0x0C,0x0C,0x0C,0x1E,0x00,0x00,0x00,0x00},
    /* 5 */ {0x00,0x00,0xFC,0xC0,0xC0,0xC0,0xF8,0x0C,0x0C,0x0C,0xCC,0x78,0x00,0x00,0x00,0x00},
    /* 6 */ {0x00,0x00,0x38,0x60,0xC0,0xC0,0xF8,0xCC,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00},
    /* 7 */ {0x00,0x00,0xFE,0xCC,0x0C,0x18,0x18,0x30,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00},
    /* 8 */ {0x00,0x00,0x78,0xCC,0xCC,0xCC,0x78,0xCC,0xCC,0xCC,0xCC,0x78,0x00,0x00,0x00,0x00},
    /* 9 */ {0x00,0x00,0x78,0xCC,0xCC,0xCC,0x7C,0x0C,0x0C,0x0C,0x18,0x70,0x00,0x00,0x00,0x00},
};

static int char_to_font_idx(char c)
{
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';  /* tolower */
    switch (c) {
        case 'a': return 0;  case 'b': return 1;  case 'c': return 2;
        case 'd': return 3;  case 'e': return 4;  case 'f': return 5;
        case 'g': return 6;  case 'h': return 7;  case 'i': return 8;
        case 'k': return 9;  case 'l': return 10; case 'p': return 11;
        case 's': return 12;
        case '0': return 13; case '1': return 14; case '2': return 15;
        case '3': return 16; case '4': return 17; case '5': return 18;
        case '6': return 19; case '7': return 20; case '8': return 21;
        case '9': return 22;
        case ':': return -2; /* special */
        default: return -1;
    }
}

static void fb_draw_char(int x, int y, char c, uint16_t color)
{
    if (c == ' ') return;
    if (c == ':') {
        /* draw colon as two dots */
        fb_put_pixel(x + 3, y + 5, color);
        fb_put_pixel(x + 3, y + 10, color);
        return;
    }
    int idx = char_to_font_idx(c);
    if (idx < 0) return;
    for (int row = 0; row < 16; row++) {
        uint8_t line = font8x16_basic[idx][row];
        for (int col = 0; col < 8; col++) {
            if (line & (0x80 >> col)) {
                fb_put_pixel(x + col, y + row, color);
            }
        }
    }
}

static void fb_draw_string(int x, int y, const char *str, uint16_t color)
{
    while (*str) {
        fb_draw_char(x, y, *str, color);
        x += 9;
        str++;
    }
}

/* ── Blit callback (called by nofrendo per frame) ───── */
/* Write to BOTH framebuffers to prevent DMA alternating between old/new content */
static void IRAM_ATTR blit_screen(uint8_t *bmp)
{
    if (!bmp || !g_palette565 || !g_lcd_fb0 || !g_lcd_fb1) return;

    for (int y = 0; y < NES_H; y++) {
        uint8_t  *src = bmp + y * NES_SCREEN_PITCH + NES_SCREEN_OVERDRAW;
        uint16_t *dst0 = g_lcd_fb0 + (GAME_Y + y) * SCREEN_W + GAME_X;
        uint16_t *dst1 = g_lcd_fb1 + (GAME_Y + y) * SCREEN_W + GAME_X;
        for (int x = 0; x < NES_W; x++) {
            uint16_t c = g_palette565[src[x]];
            dst0[x] = c;
            dst1[x] = c;
        }
    }
}

/* ── Touch input (direct hardware read) ──────────────── */
static void read_touch_input(void)
{
    tp_dev.scan(0);

    if (!(tp_dev.sta & TP_PRES_DOWN)) {
        g_joystick = 0;
        return;
    }

    int x = tp_dev.x[0];
    int y = tp_dev.y[0];

    /* BACK: top-right corner */
    if (x >= SCREEN_W - 120 && y <= 60) {
        g_back_pressed = true;
        g_joystick = 0;
        return;
    }

    uint32_t pad = 0;

    /* D-pad: left area, cross layout centered at (100, 300) */
    if (x >= 60 && x <= 140 && y >= 230 && y <= 280) pad |= NES_PAD_UP;
    if (x >= 60 && x <= 140 && y >= 320 && y <= 370) pad |= NES_PAD_DOWN;
    if (x >= 20 && x <= 80  && y >= 270 && y <= 330) pad |= NES_PAD_LEFT;
    if (x >= 120 && x <= 180 && y >= 270 && y <= 330) pad |= NES_PAD_RIGHT;

    /* A button: right area at (700, 280) */
    if (x >= 660 && x <= 740 && y >= 240 && y <= 320) pad |= NES_PAD_A;

    /* B button: right area at (700, 380) */
    if (x >= 660 && x <= 740 && y >= 340 && y <= 420) pad |= NES_PAD_B;

    /* START: bottom center-right */
    if (x >= 480 && x <= 560 && y >= 440 && y <= 480) pad |= NES_PAD_START;

    /* SELECT: bottom center-left */
    if (x >= 240 && x <= 320 && y >= 440 && y <= 480) pad |= NES_PAD_SELECT;

    g_joystick = pad;
}

/* ── Draw game screen UI (buttons, labels) ───────────── */
static void draw_game_ui(void)
{
    /* Clear entire framebuffer to dark */
    fb_clear(COL_BG_RGB565);

    /* BACK label top-right */
    fb_draw_string(SCREEN_W - 80, 10, "back", COL_WHITE_RGB565);

    /* D-pad area: draw cross guides */
    int dcx = 100, dcy = 300;
    fb_fill_rect(dcx - 40, dcy - 5, 80, 10, 0x4208);  /* horizontal bar */
    fb_fill_rect(dcx - 5, dcy - 40, 10, 80, 0x4208);  /* vertical bar */

    /* A button circle indicator */
    fb_fill_rect(700 - 30, 280 - 30, 60, 60, 0x3800);
    fb_draw_string(700 - 5, 280 - 8, "a", COL_RED_RGB565);

    /* B button circle indicator */
    fb_fill_rect(700 - 30, 380 - 30, 60, 60, 0x0120);
    fb_draw_string(700 - 5, 380 - 8, "b", 0x07E0);

    /* START label */
    fb_fill_rect(480, 445, 70, 25, 0x4208);
    fb_draw_string(485, 448, "start", COL_WHITE_RGB565);

    /* SELECT label */
    fb_fill_rect(240, 445, 70, 25, 0x4208);
    fb_draw_string(245, 448, "select", COL_WHITE_RGB565);
}

/* ── Emulator task (runs on core 1) ───────────────────── */
static void emu_task_func(void *arg)
{
    (void)arg;
    uint32_t frame_count = 0;
    uint32_t fps_timer = xTaskGetTickCount();

    ESP_LOGI(TAG, "Emu task started on core %d", xPortGetCoreID());

    while (g_emu_running) {
        /* Read touch input directly */
        read_touch_input();

        /* Check BACK */
        if (g_back_pressed) {
            ESP_LOGI(TAG, "BACK pressed, stopping emulator");
            break;
        }

        /* Update NES input */
        input_update(0, g_joystick);

        /* Run one NES frame - blit_screen writes to both LCD framebuffers */
        nes_emulate(true);

        /* Draw UI overlay directly to LCD framebuffer */
        draw_game_ui();

        /* Draw FPS */
        frame_count++;
        uint32_t now = xTaskGetTickCount();
        if (now - fps_timer >= pdMS_TO_TICKS(1000)) {
            char fps_str[16];
            snprintf(fps_str, sizeof(fps_str), "fps:%lu", frame_count);
            fb_draw_string(10, 10, fps_str, COL_WHITE_RGB565);
            ESP_LOGI(TAG, "FPS: %lu", frame_count);
            frame_count = 0;
            fps_timer = now;
        }
    }

    ESP_LOGI(TAG, "Emu task ended");
    g_emu_task = NULL;
    vTaskDelete(NULL);
}

/* ── Stop emulator ───────────────────────────────────── */
static void stop_emu(void)
{
    g_emu_running = false;

    /* Wait for task to finish */
    while (g_emu_task) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    nes_shutdown();

    if (g_palette565) { free(g_palette565); g_palette565 = NULL; }
    if (g_vidbuf)     { free(g_vidbuf);     g_vidbuf     = NULL; }

    g_lcd_fb0 = NULL;
    g_lcd_fb1 = NULL;
    g_panel = NULL;
    g_state = EMU_STATE_IDLE;
}

/* ── Start emulator (exclusive screen mode) ──────────── */
static void start_emu(const char *rom_path)
{
    ESP_LOGI(TAG, "Starting emulator: %s", rom_path);
    ESP_LOGI(TAG, "Free heap: %lu, PSRAM: %lu",
             esp_get_free_heap_size(), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    /* 1. Get LCD panel handle and both framebuffers */
    g_panel = lcddev.lcd_panel_handle;
    if (!g_panel) {
        ESP_LOGE(TAG, "LCD panel handle is NULL");
        return;
    }

    void *fb0 = NULL, *fb1 = NULL;
    if (!lvgl_demo_get_framebuffers(&fb0, &fb1, NULL, NULL)) {
        ESP_LOGE(TAG, "Failed to get LCD framebuffers");
        return;
    }
    g_lcd_fb0 = (uint16_t *)fb0;
    g_lcd_fb1 = (uint16_t *)fb1;
    ESP_LOGI(TAG, "LCD FB0: %p, FB1: %p", g_lcd_fb0, g_lcd_fb1);

    /* 2. Suspend LVGL (stops tick and flush) */
    lvgl_demo_suspend();
    ESP_LOGI(TAG, "LVGL suspended");

    /* 3. Clear both framebuffers */
    memset(g_lcd_fb0, 0, SCREEN_W * SCREEN_H * 2);
    memset(g_lcd_fb1, 0, SCREEN_W * SCREEN_H * 2);

    /* 4. Build RGB565 palette */
    g_palette565 = nofrendo_buildpalette(NES_PALETTE_NOFRENDO, 16);
    if (!g_palette565) {
        ESP_LOGE(TAG, "Failed to build palette");
        lvgl_demo_resume();
        return;
    }

    /* 4. Allocate vidbuf (8-bit indexed) */
    g_vidbuf = malloc(NES_SCREEN_PITCH * NES_H);
    if (!g_vidbuf) {
        ESP_LOGE(TAG, "Failed to alloc vidbuf");
        goto fail;
    }
    memset(g_vidbuf, 0, NES_SCREEN_PITCH * NES_H);

    /* 5. Init nofrendo */
    if (nofrendo_init(SYS_DETECT, 44100, false, (void *)blit_screen, NULL, NULL) != 0) {
        ESP_LOGE(TAG, "nofrendo_init failed");
        goto fail;
    }

    /* 6. Load ROM */
    if (nes_loadfile(rom_path) < 0) {
        ESP_LOGE(TAG, "nes_loadfile failed");
        goto fail;
    }

    /* 7. Set vidbuf AFTER nes_loadfile */
    nes_setvidbuf(g_vidbuf);

    /* 8. Connect input */
    input_connect(0, NES_JOYPAD);

    ESP_LOGI(TAG, "NES init OK, vidbuf=%p", g_vidbuf);

    /* 9. Draw initial UI */
    draw_game_ui();

    /* 10. Start emulator task on core 1 */
    g_state = EMU_STATE_RUNNING;
    g_emu_running = true;
    g_back_pressed = false;

    xTaskCreatePinnedToCore(
        emu_task_func,
        "nes_emu",
        8192,
        NULL,
        5,
        &g_emu_task,
        1  /* core 1 */
    );

    /* Wait for task to exit */
    while (g_emu_task) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    /* 11. Cleanup and resume LVGL */
    stop_emu();
    lvgl_demo_resume();
    ESP_LOGI(TAG, "LVGL resumed");

    /* Return to menu */
    menu_start();
    return;

fail:
    if (g_palette565) { free(g_palette565); g_palette565 = NULL; }
    if (g_vidbuf)     { free(g_vidbuf);     g_vidbuf     = NULL; }
    lvgl_demo_resume();
}

/* ── ROM list (LVGL) ─────────────────────────────────── */
static void rom_item_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx >= 0 && idx < g_rom_count) {
        start_emu(g_roms[idx].path);
    }
}

static void rom_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        menu_go_back();
    }
}

void fc_emulator_show_rom_list(void)
{
    g_state = EMU_STATE_ROM_LIST;

    /* Init SPI + SD if needed */
    struct stat st;
    if (stat("/sdcard", &st) != 0) {
        spi2_init();
        sd_spi_init();
    }

    scan_roms();

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x1A1A2E), 0);

    /* Title */
    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_pos(title, 20, 15);
    lv_label_set_text(title, "FC/NES Emulator");
    lv_obj_set_style_text_color(title, lv_color_hex(0xECF0F1), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);

    /* Status */
    char status[64];
    bool sd_ok = (stat("/sdcard", &st) == 0);
    snprintf(status, sizeof(status), "SD:%s  ROMs:%d", sd_ok ? "OK" : "FAIL", g_rom_count);
    lv_obj_t *stlbl = lv_label_create(scr);
    lv_obj_set_pos(stlbl, 20, 45);
    lv_label_set_text(stlbl, status);
    lv_obj_set_style_text_color(stlbl, lv_color_hex(sd_ok ? 0x00FF00 : 0xFF0000), 0);
    lv_obj_set_style_text_font(stlbl, &lv_font_montserrat_14, 0);

    /* Back button */
    lv_obj_t *btn = lv_btn_create(scr);
    lv_obj_set_size(btn, 90, 36);
    lv_obj_set_pos(btn, SCREEN_W - 110, 15);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x444444), 0);
    lv_obj_add_event_cb(btn, rom_back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, "Back");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xECF0F1), 0);
    lv_obj_center(lbl);

    /* ROM list */
    if (g_rom_count == 0) {
        lv_obj_t *msg = lv_label_create(scr);
        lv_obj_set_pos(msg, 20, 80);
        lv_label_set_text(msg, "No .nes files found.\n\nPut ROMs in:\n  /sdcard/FC/  or  /sdcard/");
        lv_obj_set_style_text_color(msg, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_text_font(msg, &lv_font_montserrat_16, 0);
    } else {
        lv_obj_t *list = lv_list_create(scr);
        lv_obj_set_size(list, SCREEN_W - 40, SCREEN_H - 80);
        lv_obj_set_pos(list, 20, 70);
        lv_obj_set_style_bg_color(list, lv_color_hex(0x222244), 0);

        for (int i = 0; i < g_rom_count && i < MAX_ROMS; i++) {
            lv_obj_t *b = lv_list_add_btn(list, LV_SYMBOL_PLAY, g_roms[i].name);
            lv_obj_set_style_bg_color(b, lv_color_hex(0x333366), 0);
            lv_obj_set_style_text_color(b, lv_color_hex(0xECF0F1), 0);
            lv_obj_add_event_cb(b, rom_item_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }

    lv_scr_load(scr);
}

/* ── Init ─────────────────────────────────────────────── */
void fc_emulator_init(void)
{
    ESP_LOGI(TAG, "FC Emulator initialized");
}
