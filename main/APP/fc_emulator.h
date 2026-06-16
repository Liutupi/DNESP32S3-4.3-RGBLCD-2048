/**
 ****************************************************************************************************
 * @file        fc_emulator.h
 * @brief       FC/NES Emulator Integration for LVGL
 ****************************************************************************************************
 */

#ifndef __FC_EMULATOR_H
#define __FC_EMULATOR_H

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 函数声明 */
void fc_emulator_init(void);
void fc_emulator_start(const char *rom_path);
void fc_emulator_stop(void);
void fc_emulator_show_rom_list(void);

#ifdef __cplusplus
}
#endif

#endif /* __FC_EMULATOR_H */
