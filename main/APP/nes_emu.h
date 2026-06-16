/**
 ****************************************************************************************************
 * @file        nes_emu.h
 * @brief       FC/NES 模拟器 for DNESP32S3 (4.3" RGB LCD 800x480, LVGL v8)
 *
 *  基于 Nofrendo 轻量级 NES 模拟器移植
 *  支持从 SD 卡加载 .nes ROM 文件
 *  触摸屏虚拟按键控制
 ****************************************************************************************************
 */

#ifndef NES_EMU_H
#define NES_EMU_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* NES 屏幕分辨率 */
#define NES_SCREEN_W    256
#define NES_SCREEN_H    240

/* LCD 屏幕分辨率 */
#define LCD_W           800
#define LCD_H           480

/* 缩放后的游戏区域（保持 4:3 比例） */
#define NES_SCALE       2
#define GAME_W          (NES_SCREEN_W * NES_SCALE)  /* 512 */
#define GAME_H          (NES_SCREEN_H * NES_SCALE)  /* 480 */
#define GAME_X          ((LCD_W - GAME_W) / 2)      /* 144 */
#define GAME_Y          0

/* NES 按键定义 */
typedef enum {
    NES_BTN_A      = 0,
    NES_BTN_B      = 1,
    NES_BTN_SELECT = 2,
    NES_BTN_START  = 3,
    NES_BTN_UP     = 4,
    NES_BTN_DOWN   = 5,
    NES_BTN_LEFT   = 6,
    NES_BTN_RIGHT  = 7,
    NES_BTN_COUNT  = 8
} nes_btn_t;

/**
 * @brief       启动 NES 模拟器
 * @param       rom_path: SD 卡上的 ROM 文件路径（如 "/sdcard/FC/mario.nes"）
 * @retval      无
 */
void nes_emu_start(const char *rom_path);

/**
 * @brief       停止 NES 模拟器并返回菜单
 * @retval      无
 */
void nes_emu_stop(void);

/**
 * @brief       显示 ROM 选择界面
 * @retval      无
 */
void nes_rom_browser_start(void);

#ifdef __cplusplus
}
#endif

#endif /* NES_EMU_H */
