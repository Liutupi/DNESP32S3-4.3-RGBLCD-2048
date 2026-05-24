#ifndef BOARD_AUDIO_H
#define BOARD_AUDIO_H

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_AUDIO_SAMPLE_RATE_HZ 44100
#define BOARD_AUDIO_VOLUME_DEFAULT 28

esp_err_t board_audio_init_i2c_and_expander(void);
esp_err_t board_audio_speaker_enable(bool enable);
esp_err_t board_audio_codec_start(uint8_t volume);
void board_audio_codec_stop(void);
esp_err_t board_audio_i2s_start(int sample_rate);
void board_audio_i2s_stop(void);
bool board_audio_i2s_is_started(void);
int board_audio_i2s_sample_rate(void);
bool board_audio_i2s_write(const int16_t *samples, size_t sample_count);
bool board_audio_play_beep_440hz_500ms(void);

#ifdef __cplusplus
}
#endif

#endif
