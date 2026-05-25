/**
 ****************************************************************************************************
 * @file        app_network.h
 * @brief       Shared WiFi bootstrap for WarmOS apps
 ****************************************************************************************************
 */

#ifndef __APP_NETWORK_H
#define __APP_NETWORK_H

#include <stdbool.h>

void app_network_start(void);
bool app_network_has_configured_wifi(void);

#endif
