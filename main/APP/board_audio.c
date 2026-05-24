#include "board_audio.h"

#include "myiic.h"
#include "xl9555.h"

#include "driver/gpio.h"
#include "driver/i2s.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <math.h>

#define TAG "BOARD_AUDIO"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ES8388_ADDR_7BIT 0x10
#define I2S_PORT I2S_NUM_0

#define I2S_MCLK_GPIO GPIO_NUM_3
#define I2S_BCLK_GPIO GPIO_NUM_46
#define I2S_LRCK_GPIO GPIO_NUM_9
#define I2S_DOUT_GPIO GPIO_NUM_14
#define I2S_DIN_GPIO GPIO_NUM_10

static i2c_master_dev_handle_t s_es8388;
static bool s_i2s_started;
static int s_i2s_rate;
static uint32_t s_i2s_write_log_count;

static esp_err_t es8388_write(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(s_es8388, data, sizeof(data), pdMS_TO_TICKS(100));
}

esp_err_t board_audio_init_i2c_and_expander(void)
{
    if (bus_handle == NULL) {
        ESP_RETURN_ON_ERROR(myiic_init(), TAG, "myiic_init failed");
    }
    ESP_LOGI(TAG, "I2C0_INIT_OK SDA=41 SCL=42");

    ESP_RETURN_ON_ERROR(xl9555_init(), TAG, "xl9555_init failed");
    ESP_LOGI(TAG, "XL9555_INIT_OK");
    return ESP_OK;
}

esp_err_t board_audio_speaker_enable(bool enable)
{
    xl9555_pin_write(SPK_EN_IO, enable ? 0 : 1);
    ESP_LOGI(TAG, "%s", enable ? "SPK_EN_LOW_OK" : "SPK_EN_HIGH_OK");
    return ESP_OK;
}

esp_err_t board_audio_codec_start(uint8_t volume)
{
    if (s_es8388 == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ES8388_ADDR_7BIT,
            .scl_speed_hz = IIC_SPEED_CLK,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_es8388),
                            TAG, "add ES8388 failed");
    }

    ESP_RETURN_ON_ERROR(es8388_write(0x00, 0x80), TAG, "ES8388 reset failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x00, 0x00), TAG, "ES8388 reset release failed");
    vTaskDelay(pdMS_TO_TICKS(80));

    ESP_RETURN_ON_ERROR(es8388_write(0x01, 0x58), TAG, "ES8388 control2 step1 failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x01, 0x50), TAG, "ES8388 control2 step2 failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x02, 0xF3), TAG, "ES8388 chip power step1 failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x02, 0xF0), TAG, "ES8388 chip power step2 failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x03, 0x09), TAG, "ES8388 adc power failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x00, 0x06), TAG, "ES8388 control1 failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x04, 0x00), TAG, "ES8388 dac power preconfig failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x08, 0x00), TAG, "ES8388 mclk divider failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x2B, 0x80), TAG, "ES8388 dac lrck source failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x09, 0x88), TAG, "ES8388 adc gain failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x0C, 0x4C), TAG, "ES8388 adc serial failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x0D, 0x02), TAG, "ES8388 adc clock failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x10, 0x00), TAG, "ES8388 adc left volume failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x11, 0x00), TAG, "ES8388 adc right volume failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x17, 0x18), TAG, "ES8388 dac i2s 16bit failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x18, 0x02), TAG, "ES8388 dac clock failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x1A, 0x00), TAG, "ES8388 dac left digital volume failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x1B, 0x00), TAG, "ES8388 dac right digital volume failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x27, 0xB8), TAG, "ES8388 left mixer failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x2A, 0xB8), TAG, "ES8388 right mixer failed");
    ESP_LOGI(TAG, "ES8388_INIT_OK");

    ESP_RETURN_ON_ERROR(es8388_write(0x02, 0x0A), TAG, "ES8388 DAC on failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x04, 0x3C), TAG, "ES8388 DAC output failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x19, 0x00), TAG, "ES8388 DAC unmute failed");
    ESP_LOGI(TAG, "ES8388_DAC_ON_OK");

    ESP_RETURN_ON_ERROR(es8388_write(0x2E, volume), TAG, "ES8388 hp left volume failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x2F, volume), TAG, "ES8388 hp right volume failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x30, volume), TAG, "ES8388 speaker left volume failed");
    ESP_RETURN_ON_ERROR(es8388_write(0x31, volume), TAG, "ES8388 speaker right volume failed");
    ESP_LOGI(TAG, "ES8388_VOL_SET_OK vol=%u", (unsigned)volume);
    return ESP_OK;
}

void board_audio_codec_stop(void)
{
    if (s_es8388) {
        es8388_write(0x19, 0x04);
        es8388_write(0x04, 0xC0);
        es8388_write(0x02, 0xF3);
    }
}

esp_err_t board_audio_i2s_start(int sample_rate)
{
    if (s_i2s_started && s_i2s_rate == sample_rate) {
        return ESP_OK;
    }
    if (s_i2s_started) {
        board_audio_i2s_stop();
    }

    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX,
        .sample_rate = sample_rate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = true,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0,
    };
    i2s_pin_config_t pin_config = {
        .mck_io_num = I2S_MCLK_GPIO,
        .bck_io_num = I2S_BCLK_GPIO,
        .ws_io_num = I2S_LRCK_GPIO,
        .data_out_num = I2S_DOUT_GPIO,
        .data_in_num = I2S_DIN_GPIO,
    };

    ESP_RETURN_ON_ERROR(i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL),
                        TAG, "i2s_driver_install failed");
    s_i2s_started = true;
    s_i2s_rate = sample_rate;
    s_i2s_write_log_count = 0;

    ESP_RETURN_ON_ERROR(i2s_set_pin(I2S_PORT, &pin_config), TAG, "i2s_set_pin failed");
    ESP_RETURN_ON_ERROR(i2s_zero_dma_buffer(I2S_PORT), TAG, "i2s_zero_dma_buffer failed");
    ESP_RETURN_ON_ERROR(i2s_start(I2S_PORT), TAG, "i2s_start failed");
    ESP_LOGI(TAG, "I2S_INIT_OK mclk=3 bclk=46 lrck=9 dout=14 din=10");
    return ESP_OK;
}

void board_audio_i2s_stop(void)
{
    if (s_i2s_started) {
        i2s_stop(I2S_PORT);
        i2s_driver_uninstall(I2S_PORT);
        s_i2s_started = false;
        s_i2s_rate = 0;
    }
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
    if (!s_i2s_started || !samples || sample_count == 0) {
        return false;
    }

    size_t bytes_written = 0;
    esp_err_t err = i2s_write(I2S_PORT, samples, sample_count * sizeof(int16_t),
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
            return false;
        }
        pos += n;
    }
    ESP_LOGI(TAG, "BEEP_OK");
    return true;
}
