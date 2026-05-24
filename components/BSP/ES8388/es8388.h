#ifndef BSP_ES8388_H
#define BSP_ES8388_H

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ES8388_ADDR_7BIT 0x10

esp_err_t es8388_init(void);
esp_err_t es8388_deinit(void);
esp_err_t es8388_write_reg(uint8_t reg, uint8_t val);
esp_err_t es8388_read_reg(uint8_t reg, uint8_t *val);
esp_err_t es8388_sai_cfg(uint8_t fmt, uint8_t len);
esp_err_t es8388_i2s_cfg(uint8_t fmt, uint8_t len);
esp_err_t es8388_hpvol_set(uint8_t volume);
esp_err_t es8388_spkvol_set(uint8_t volume);
esp_err_t es8388_adda_cfg(uint8_t dacen, uint8_t adcen);
esp_err_t es8388_output_cfg(uint8_t o1en, uint8_t o2en);
esp_err_t es8388_dac_mute(bool mute);
esp_err_t es8388_mixer_cfg(void);

#ifdef __cplusplus
}
#endif

#endif
