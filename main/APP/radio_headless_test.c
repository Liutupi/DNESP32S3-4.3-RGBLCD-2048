#include "radio_headless_test.h"
#include "lvgl_demo.h"
#include "menu.h"
#include "lcd.h"
#include "myiic.h"
#include "xl9555.h"

#include "driver/gpio.h"
#include "driver/i2s.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define TAG                         "RADIO_HEADLESS"

#define ES8388_ADDR                 0x10
#define SAMPLE_RATE_HZ              44100
#define BEEP_FREQ_HZ                1000
#define BEEP_MS                     500
#define BEEP_PERIOD_MS              1000
#define EXIT_HOLD_MS                2000
#define BUTTON_POLL_MS              20
#define BEEP_LOOPS_BEFORE_IDLE_LOG  5

#define I2S_MCLK_GPIO               GPIO_NUM_3
#define I2S_BCLK_GPIO               GPIO_NUM_46
#define I2S_LRCK_GPIO               GPIO_NUM_9
#define I2S_DOUT_GPIO               GPIO_NUM_10
#define I2S_DIN_GPIO                GPIO_NUM_14
#define BOOT_GPIO                   GPIO_NUM_0

static bool s_i2s_installed = false;
static i2c_master_dev_handle_t s_es8388 = NULL;

static void warning_page_show(void)
{
    ESP_LOGI(TAG, "Show warning page");

    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(box, 800, 480);
    lv_obj_set_pos(box, 0, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x100A08), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "Entering Headless Radio Test");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFF2DC), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 105);

    lv_obj_t *body = lv_label_create(box);
    lv_label_set_text(body,
                      "LCD will turn off\n"
                      "Hold BOOT to exit\n"
                      "Testing ES8388 beep");
    lv_obj_set_style_text_font(body, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(0xE58A3A), 0);
    lv_obj_set_style_text_line_space(body, 12, 0);
    lv_obj_set_style_text_align(body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(body, LV_ALIGN_CENTER, 0, 40);

    int64_t until = esp_timer_get_time() + 2000000;
    while (esp_timer_get_time() < until) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    lv_obj_del(box);
    lv_timer_handler();
}

static void reset_conflict_gpios(void)
{
    ESP_LOGI(TAG, "Reset GPIO 3/46/9/10/14");
    gpio_reset_pin(I2S_MCLK_GPIO);
    gpio_reset_pin(I2S_BCLK_GPIO);
    gpio_reset_pin(I2S_LRCK_GPIO);
    gpio_reset_pin(I2S_DIN_GPIO);
    gpio_reset_pin(I2S_DOUT_GPIO);
}

static void boot_button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
}

static bool boot_button_long_pressed(void)
{
    static int64_t pressed_since_us = 0;
    int level = gpio_get_level(BOOT_GPIO);

    if (level == 0) {
        if (pressed_since_us == 0) {
            pressed_since_us = esp_timer_get_time();
        }
        return (esp_timer_get_time() - pressed_since_us) >= (EXIT_HOLD_MS * 1000);
    }

    pressed_since_us = 0;
    return false;
}

static esp_err_t es8388_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t data[2] = {reg, value};
    return i2c_master_transmit(s_es8388, data, sizeof(data), pdMS_TO_TICKS(100));
}

static esp_err_t es8388_read_reg(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_es8388, &reg, 1, value, 1, pdMS_TO_TICKS(100));
}

static void es8388_log_reg(uint8_t reg)
{
    uint8_t value = 0;
    esp_err_t ret = es8388_read_reg(reg, &value);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ES8388 reg 0x%02x = 0x%02x", reg, value);
    } else {
        ESP_LOGW(TAG, "ES8388 reg 0x%02x read failed: %s", reg, esp_err_to_name(ret));
    }
}

static esp_err_t es8388_init_minimal(void)
{
    ESP_LOGI(TAG, "Init ES8388: start");

    if (bus_handle == NULL) {
        ESP_RETURN_ON_ERROR(myiic_init(), TAG, "myiic_init failed");
    }

    if (s_es8388 == NULL) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = ES8388_ADDR,
            .scl_speed_hz = IIC_SPEED_CLK,
        };
        ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bus_handle, &dev_cfg, &s_es8388),
                            TAG, "add ES8388 device failed");
    }

    ESP_RETURN_ON_ERROR(es8388_write_reg(0x00, 0x80), TAG, "ES8388 reset failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x00, 0x00), TAG, "ES8388 reset release failed");
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_RETURN_ON_ERROR(es8388_write_reg(0x01, 0x58), TAG, "ES8388 control2 step 1 failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x01, 0x50), TAG, "ES8388 control2 step 2 failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x02, 0xF3), TAG, "ES8388 chip power step 1 failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x02, 0xF0), TAG, "ES8388 chip power step 2 failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x03, 0x09), TAG, "ES8388 ADC power failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x00, 0x06), TAG, "ES8388 control1 failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x04, 0x00), TAG, "ES8388 DAC power preconfigure failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x08, 0x00), TAG, "ES8388 MCLK divider failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x2B, 0x80), TAG, "ES8388 DAC LRCK source failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x09, 0x88), TAG, "ES8388 ADC gain failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x0C, 0x4C), TAG, "ES8388 ADC serial failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x0D, 0x02), TAG, "ES8388 ADC clock failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x10, 0x00), TAG, "ES8388 ADC left volume failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x11, 0x00), TAG, "ES8388 ADC right volume failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x17, 0x18), TAG, "ES8388 DAC I2S 16-bit failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x18, 0x02), TAG, "ES8388 DAC clock ratio failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x1A, 0x00), TAG, "ES8388 DAC left digital volume failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x1B, 0x00), TAG, "ES8388 DAC right digital volume failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x27, 0xB8), TAG, "ES8388 left mixer failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x2A, 0xB8), TAG, "ES8388 right mixer failed");
    vTaskDelay(pdMS_TO_TICKS(100));

    ESP_RETURN_ON_ERROR(es8388_write_reg(0x02, 0x0A), TAG, "ES8388 DAC on ADC off failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x04, 0x3C), TAG, "ES8388 DAC outputs failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x2E, 20), TAG, "ES8388 headphone L volume failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x2F, 20), TAG, "ES8388 headphone R volume failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x30, 20), TAG, "ES8388 speaker L volume failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x31, 20), TAG, "ES8388 speaker R volume failed");

    xl9555_pin_write(SPK_EN_IO, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    es8388_log_reg(0x02);
    es8388_log_reg(0x04);
    es8388_log_reg(0x17);
    es8388_log_reg(0x19);
    es8388_log_reg(0x2B);
    es8388_log_reg(0x2E);
    es8388_log_reg(0x2F);
    es8388_log_reg(0x30);
    es8388_log_reg(0x31);
    ESP_LOGI(TAG, "Init ES8388: success");
    return ESP_OK;
}

static void es8388_stop(void)
{
    ESP_LOGI(TAG, "Stop ES8388");
    if (s_es8388) {
        es8388_write_reg(0x19, 0x04);
        es8388_write_reg(0x04, 0xC0);
    }
    xl9555_pin_write(SPK_EN_IO, 1);
}

static esp_err_t i2s_init(void)
{
    ESP_LOGI(TAG, "Init I2S: start, DOUT GPIO%d, DIN GPIO%d", I2S_DOUT_GPIO, I2S_DIN_GPIO);
    ESP_LOGI(TAG, "sample rate: %d", SAMPLE_RATE_HZ);
    ESP_LOGI(TAG, "channels: stereo");
    ESP_LOGI(TAG, "bits per sample: 16");

    i2s_config_t i2s_config = {
        .mode = I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX,
        .sample_rate = SAMPLE_RATE_HZ,
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

    ESP_RETURN_ON_ERROR(i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL), TAG, "i2s driver install failed");
    s_i2s_installed = true;
    ESP_RETURN_ON_ERROR(i2s_set_pin(I2S_NUM_0, &pin_config), TAG, "i2s set pin failed");
    ESP_RETURN_ON_ERROR(i2s_zero_dma_buffer(I2S_NUM_0), TAG, "i2s zero dma failed");
    ESP_RETURN_ON_ERROR(i2s_start(I2S_NUM_0), TAG, "i2s start failed");
    ESP_LOGI(TAG, "Init I2S: success");
    return ESP_OK;
}

static void radio_i2s_stop(void)
{
    ESP_LOGI(TAG, "Stop I2S");
    if (s_i2s_installed) {
        i2s_stop(I2S_NUM_0);
        i2s_driver_uninstall(I2S_NUM_0);
        s_i2s_installed = false;
    }
}

static void write_silence(uint32_t ms)
{
    int16_t frame[128 * 2] = {0};
    uint32_t samples_left = (SAMPLE_RATE_HZ * ms) / 1000;
    while (s_i2s_installed && samples_left > 0) {
        uint32_t frames = samples_left > 128 ? 128 : samples_left;
        size_t bytes_written = 0;
        esp_err_t ret = i2s_write(I2S_NUM_0, frame, frames * 2 * sizeof(int16_t),
                                  &bytes_written, pdMS_TO_TICKS(100));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s write silence failed: %s", esp_err_to_name(ret));
            return;
        }
        samples_left -= frames;
    }
}

static void play_beep_once(void)
{
    int16_t frame[128 * 2];
    uint32_t total_samples = (SAMPLE_RATE_HZ * BEEP_MS) / 1000;
    uint32_t sample_index = 0;

    size_t total_bytes_written = 0;
    while (s_i2s_installed && sample_index < total_samples) {
        uint32_t frames = (total_samples - sample_index) > 128 ? 128 : (total_samples - sample_index);
        for (uint32_t i = 0; i < frames; ++i) {
            float phase = 2.0f * (float)M_PI * (float)BEEP_FREQ_HZ * (float)(sample_index + i) / (float)SAMPLE_RATE_HZ;
            int16_t sample = (int16_t)(sinf(phase) * 9000.0f);
            frame[i * 2] = sample;
            frame[i * 2 + 1] = sample;
        }

        size_t bytes_written = 0;
        esp_err_t ret = i2s_write(I2S_NUM_0, frame, frames * 2 * sizeof(int16_t),
                                  &bytes_written, pdMS_TO_TICKS(200));
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(ret));
            return;
        }
        total_bytes_written += bytes_written;
        sample_index += frames;
    }
    ESP_LOGI(TAG, "i2s write bytes: %u", (unsigned)total_bytes_written);
}

static void run_audio_until_exit(void)
{
    esp_err_t codec_ret = es8388_init_minimal();
    if (codec_ret != ESP_OK) {
        ESP_LOGE(TAG, "Init ES8388: fail (%s)", esp_err_to_name(codec_ret));
        return;
    }

    esp_err_t i2s_ret = i2s_init();
    if (i2s_ret != ESP_OK) {
        ESP_LOGE(TAG, "Init I2S: fail (%s)", esp_err_to_name(i2s_ret));
        return;
    }

    uint32_t loops = 0;
    while (true) {
        if (boot_button_long_pressed()) {
            ESP_LOGI(TAG, "BOOT long press detected");
            break;
        }

        ESP_LOGI(TAG, "beep loop alive");
        play_beep_once();
        write_silence(BEEP_PERIOD_MS - BEEP_MS);
        if (++loops % BEEP_LOOPS_BEFORE_IDLE_LOG == 0) {
            ESP_LOGI(TAG, "beep loops: %u", (unsigned)loops);
        }
    }
}

void radio_headless_test_start(void)
{
    ESP_LOGI(TAG, "Enter headless radio test");
    warning_page_show();

    ESP_LOGI(TAG, "Stop LVGL tick/flush");
    lvgl_demo_suspend();

    ESP_LOGI(TAG, "Turn off backlight");
    LCD_BL(0);
    vTaskDelay(pdMS_TO_TICKS(80));

    esp_err_t lcd_ret = lcd_deinit();
    if (lcd_ret == ESP_OK) {
        ESP_LOGI(TAG, "Deinit RGB LCD: success");
    } else {
        ESP_LOGW(TAG, "Deinit RGB LCD: fail (%s)", esp_err_to_name(lcd_ret));
    }

    reset_conflict_gpios();
    boot_button_init();

    ESP_LOGI(TAG, "Start beep");
    run_audio_until_exit();

    ESP_LOGI(TAG, "Stop beep");
    radio_i2s_stop();
    es8388_stop();

    ESP_LOGI(TAG, "Restore GPIO");
    reset_conflict_gpios();

    ESP_LOGI(TAG, "Reinit RGB LCD: start");
    lcd_init();
    if (lcddev.lcd_panel_handle) {
        ESP_LOGI(TAG, "Reinit RGB LCD: success");
        ESP_LOGI(TAG, "Restore LVGL");
        lvgl_demo_rebind_display();
        lvgl_demo_resume();
        ESP_LOGI(TAG, "Restore backlight");
        LCD_BL(1);
        ESP_LOGI(TAG, "Return menu");
        menu_start();
    } else {
        ESP_LOGE(TAG, "Reinit RGB LCD: fail");
        ESP_LOGE(TAG, "LCD restore not implemented, please reboot to return to UI.");
    }
}
