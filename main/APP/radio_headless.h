#ifndef RADIO_HEADLESS_H
#define RADIO_HEADLESS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void radio_headless_start(void);
bool radio_headless_display_released(void);

#ifdef __cplusplus
}
#endif

#endif
