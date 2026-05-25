/**
 ****************************************************************************************************
 * @file        tomato_timer.h
 * @brief       Pomodoro Timer for DNESP32S3 Game Center
 ****************************************************************************************************
 */

#ifndef __TOMATO_TIMER_H
#define __TOMATO_TIMER_H

#include "lvgl.h"
#include <stdbool.h>

void tomato_timer_start(void);
void tomato_network_start(void);
bool tomato_network_has_configured_wifi(void);

#endif
