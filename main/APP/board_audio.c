#include "board_audio.h"

#include "es8388.h"
#include "myiic.h"
#include "xl9555.h"

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>

#define TAG "BOARD_AUDIO"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define I2S_MCLK_GPIO GPIO_NUM_3
#define I2S_BCLK_GPIO GPIO_NUM_46
#define I2S_LRCK_GPIO GPIO_NUM_9
#define I2S_DOUT_GPIO GPIO_NUM_10 /* ESP32 TX -> ES8388 DSDIN */
#define I2S_DIN_GPIO GPIO_NUM_14  /* ESP32 RX <- ES8388 ASDOUT */

static i2s_chan_handle_t s_tx_handle;
static bool s_i2s_started;
static bool s_expander_ready;
static int s_i2s_rate;
static uint32_t s_i2s_write_log_count;

esp_err_t board_audio_init_i2c_and_expander(void)
{
    if (bus_handle == NULL) {
        ESP_RETURN_ON_ERROR(myiic_init(), TAG, "myiic_init failed");
    }
    ESP_LOGI(TAG, "I2C0_INIT_OK SDA=41 SCL=42");

    ESP_RETURN_ON_ERROR(xl9555_init(), TAG, "xl9555_init failed");
    s_expander_ready = true;
    ESP_LOGI(TAG, "XL9555_INIT_OK");
    return ESP_OK;
}

esp_err_t board_audio_speaker_enable(bool enable)
{
    if (!s_expander_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xl9555_pin_write(SPK_EN_IO, enable ? 0 : 1);
    ESP_LOGI(TAG, "%s", enable ? "SPK_EN_LOW_OK" : "SPK_EN_HIGH_OK");
    return ESP_OK;
}

esp_err_t board_audio_codec_start(uint8_t volume)
{
    ESP_RETURN_ON_ERROR(es8388_init(), TAG, "es8388_init failed");
    ESP_LOGI(TAG, "ES8388_INIT_OK");

    ESP_RETURN_ON_ERROR(es8388_i2s_cfg(0, 3), TAG, "es8388_i2s_cfg failed");
    ESP_RETURN_ON_ERROR(es8388_adda_cfg(1, 0), TAG, "es8388_adda_cfg failed");
    ESP_RETURN_ON_ERROR(es8388_output_cfg(1, 1), TAG, "es8388_output_cfg failed");
    ESP_RETURN_ON_ERROR(es8388_dac_mute(false), TAG, "es8388_dac_unmute failed");
    ESP_LOGI(TAG, "ES8388_DAC_ON_OK");

    ESP_RETURN_ON_ERROR(es8388_spkvol_set(volume), TAG, "es8388_spkvol_set failed");
    ESP_RETURN_ON_ERROR(es8388_hpvol_set(volume), TAG, "es8388_hpvol_set failed");
    ESP_LOGI(TAG, "ES8388_VOL_SET_OK vol=%u", (unsigned)volume);
    return ESP_OK;
}

void board_audio_codec_stop(void)
{
    es8388_dac_mute(true);
    es8388_output_cfg(0, 0);
    es8388_adda_cfg(0, 0);
    es8388_deinit();
}

esp_err_t board_audio_i2s_start(int sample_rate)
{
    if (s_i2s_started && s_i2s_rate == sample_rate) {
        return ESP_OK;
    }

    if (s_tx_handle && s_i2s_started) {
        ESP_RETURN_ON_ERROR(i2s_channel_disable(s_tx_handle),
                            TAG, "i2s_channel_disable failed");
        s_i2s_started = false;
    }

    if (s_tx_handle) {
        i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
        ESP_RETURN_ON_ERROR(i2s_channel_reconfig_std_clock(s_tx_handle, &clk_cfg),
                            TAG, "i2s clock reconfig failed");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "i2s_channel_enable failed");
    } else {
        i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
        chan_cfg.auto_clear = true;
        ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, NULL),
                            TAG, "i2s_new_channel failed");

        i2s_std_config_t std_cfg = {
            .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
            .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                            I2S_SLOT_MODE_STEREO),
            .gpio_cfg = {
                .mclk = I2S_MCLK_GPIO,
                .bclk = I2S_BCLK_GPIO,
                .ws = I2S_LRCK_GPIO,
                .dout = I2S_DOUT_GPIO,
                .din = I2S_DIN_GPIO,
                .invert_flags = {
                    .mclk_inv = false,
                    .bclk_inv = false,
                    .ws_inv = false,
                },
            },
        };
        ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg),
                            TAG, "i2s_channel_init_std_mode failed");
        ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "i2s_channel_enable failed");
    }

    s_i2s_started = true;
    s_i2s_rate = sample_rate;
    s_i2s_write_log_count = 0;
    ESP_LOGI(TAG, "I2S_INIT_OK mclk=3 bclk=46 lrck=9 dout=10(ES8388_DSDIN) din=14(ES8388_ASDOUT)");
    return ESP_OK;
}

void board_audio_i2s_stop(void)
{
    if (s_tx_handle) {
        if (s_i2s_started) {
            i2s_channel_disable(s_tx_handle);
        }
        i2s_del_channel(s_tx_handle);
        s_tx_handle = NULL;
    }
    s_i2s_started = false;
    s_i2s_rate = 0;
}

bool board_audio_i2s_is_started(void)
{
    return s_i2s_started;
}

int board_audio_i2s_sample_rate(void)
{
    return s_i2s_rate;
}

bool board_audio_i2s_write(const int16_t *samples, size_t sample_count)
{
    if (!s_i2s_started || !s_tx_handle || !samples || sample_count == 0) {
        return false;
    }

    size_t bytes_written = 0;
    esp_err_t err = i2s_channel_write(s_tx_handle, samples, sample_count * sizeof(int16_t),
                                      &bytes_written, pdMS_TO_TICKS(500));
    if (err != ESP_OK || bytes_written == 0) {
        ESP_LOGE(TAG, "I2S write failed err=%s bytes=%u",
                 esp_err_to_name(err), (unsigned)bytes_written);
        return false;
    }
    s_i2s_write_log_count++;
    if (s_i2s_write_log_count == 1 || s_i2s_write_log_count % 500 == 0) {
        ESP_LOGI(TAG, "I2S_WRITE_OK bytes=%u", (unsigned)bytes_written);
    }
    return true;
}

bool board_audio_play_beep_440hz_500ms(void)
{
    ESP_LOGI(TAG, "BEEP_START");
    if (board_audio_i2s_start(BOARD_AUDIO_SAMPLE_RATE_HZ) != ESP_OK) {
        ESP_LOGE(TAG, "BEEP_FAILED reason=I2S init failed");
        return false;
    }

    int16_t frame[128 * 2];
    uint32_t total = (BOARD_AUDIO_SAMPLE_RATE_HZ * 500) / 1000;
    uint32_t pos = 0;
    while (pos < total) {
        uint32_t n = total - pos;
        if (n > 128) {
            n = 128;
        }
        for (uint32_t i = 0; i < n; ++i) {
            float phase = 2.0f * (float)M_PI * 440.0f * (float)(pos + i) /
                          (float)BOARD_AUDIO_SAMPLE_RATE_HZ;
            int16_t sample = (int16_t)(sinf(phase) * 9000.0f);
            frame[i * 2] = sample;
            frame[i * 2 + 1] = sample;
        }
        if (!board_audio_i2s_write(frame, n * 2)) {
            ESP_LOGE(TAG, "BEEP_FAILED reason=Beep write failed");
            return false;
        }
        pos += n;
    }
    ESP_LOGI(TAG, "BEEP_OK");
    return true;
}
