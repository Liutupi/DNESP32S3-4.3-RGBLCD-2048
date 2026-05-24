#include "es8388.h"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "myiic.h"

#define TAG "ES8388"

static i2c_master_dev_handle_t s_es8388;

static esp_err_t es8388_ensure_device(void)
{
    if (bus_handle == NULL) {
        ESP_RETURN_ON_ERROR(myiic_init(), TAG, "myiic_init failed");
    }

    if (s_es8388 == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ES8388_ADDR_7BIT,
            .scl_speed_hz = IIC_SPEED_CLK,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_es8388),
                            TAG, "add ES8388 failed");
    }
    return ESP_OK;
}

esp_err_t es8388_write_reg(uint8_t reg, uint8_t val)
{
    ESP_RETURN_ON_ERROR(es8388_ensure_device(), TAG, "ES8388 device not ready");
    uint8_t data[2] = {reg, val};
    return i2c_master_transmit(s_es8388, data, sizeof(data), pdMS_TO_TICKS(100));
}

esp_err_t es8388_read_reg(uint8_t reg, uint8_t *val)
{
    if (!val) {
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(es8388_ensure_device(), TAG, "ES8388 device not ready");
    return i2c_master_transmit_receive(s_es8388, &reg, 1, val, 1, pdMS_TO_TICKS(100));
}

esp_err_t es8388_init(void)
{
    ESP_RETURN_ON_ERROR(es8388_ensure_device(), TAG, "ES8388 device init failed");

    ESP_RETURN_ON_ERROR(es8388_write_reg(0x00, 0x80), TAG, "reset failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x00, 0x00), TAG, "reset release failed");
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_RETURN_ON_ERROR(es8388_write_reg(0x01, 0x58), TAG, "control2 step1 failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x01, 0x50), TAG, "control2 step2 failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x02, 0xF3), TAG, "chip power step1 failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x02, 0xF0), TAG, "chip power step2 failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x03, 0x09), TAG, "adc power failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x00, 0x06), TAG, "control1 failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x04, 0x00), TAG, "dac power preconfig failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x08, 0x00), TAG, "mclk divider failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x2B, 0x80), TAG, "dac lrck source failed");

    ESP_RETURN_ON_ERROR(es8388_write_reg(0x09, 0x88), TAG, "adc gain failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x0C, 0x4C), TAG, "adc serial failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x0D, 0x02), TAG, "adc clock failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x10, 0x00), TAG, "adc left volume failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x11, 0x00), TAG, "adc right volume failed");

    ESP_RETURN_ON_ERROR(es8388_write_reg(0x17, 0x18), TAG, "dac i2s 16bit failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x18, 0x02), TAG, "dac clock failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x1A, 0x00), TAG, "dac left volume failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x1B, 0x00), TAG, "dac right volume failed");
    ESP_RETURN_ON_ERROR(es8388_mixer_cfg(), TAG, "mixer config failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

esp_err_t es8388_deinit(void)
{
    if (s_es8388 == NULL) {
        return ESP_OK;
    }
    return es8388_write_reg(0x02, 0xFF);
}

esp_err_t es8388_sai_cfg(uint8_t fmt, uint8_t len)
{
    fmt &= 0x03;
    len &= 0x07;
    return es8388_write_reg(0x17, (fmt << 1) | (len << 3));
}

esp_err_t es8388_i2s_cfg(uint8_t fmt, uint8_t len)
{
    return es8388_sai_cfg(fmt, len);
}

esp_err_t es8388_hpvol_set(uint8_t volume)
{
    if (volume > 33) {
        volume = 33;
    }
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x2E, volume), TAG, "hp left volume failed");
    return es8388_write_reg(0x2F, volume);
}

esp_err_t es8388_spkvol_set(uint8_t volume)
{
    if (volume > 33) {
        volume = 33;
    }
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x30, volume), TAG, "spk left volume failed");
    return es8388_write_reg(0x31, volume);
}

esp_err_t es8388_adda_cfg(uint8_t dacen, uint8_t adcen)
{
    uint8_t tempreg = 0;
    tempreg |= (!dacen) << 0;
    tempreg |= (!adcen) << 1;
    tempreg |= (!dacen) << 2;
    tempreg |= (!adcen) << 3;
    return es8388_write_reg(0x02, tempreg);
}

esp_err_t es8388_output_cfg(uint8_t o1en, uint8_t o2en)
{
    uint8_t tempreg = 0;
    tempreg |= o1en ? (3 << 4) : 0;
    tempreg |= o2en ? (3 << 2) : 0;
    return es8388_write_reg(0x04, tempreg);
}

esp_err_t es8388_dac_mute(bool mute)
{
    return es8388_write_reg(0x19, mute ? 0x04 : 0x00);
}

esp_err_t es8388_mixer_cfg(void)
{
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x27, 0xB8), TAG, "left mixer failed");
    return es8388_write_reg(0x2A, 0xB8);
}
