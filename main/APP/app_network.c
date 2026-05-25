/**
 ****************************************************************************************************
 * @file        app_network.c
 * @brief       Shared WiFi bootstrap for WarmOS apps
 ****************************************************************************************************
 */

#include "app_network.h"
#include "tomato_timer.h"

void app_network_start(void)
{
    tomato_network_start();
}

bool app_network_has_configured_wifi(void)
{
    return tomato_network_has_configured_wifi();
}
