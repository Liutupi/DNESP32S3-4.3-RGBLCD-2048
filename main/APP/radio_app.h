#ifndef __RADIO_APP_H
#define __RADIO_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void radio_app_start(void);

bool radio_audio_available(void);
bool radio_audio_start(const char *url);
size_t radio_audio_write_pcm(const int16_t *samples, size_t sample_count);
void radio_audio_stop(void);

#endif
