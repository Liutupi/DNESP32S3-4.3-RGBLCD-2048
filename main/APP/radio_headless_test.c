#include "radio_headless_test.h"
#include "radio_stations.h"
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
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "esp_mp3_dec.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
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

#define RAW_BUFFER_SIZE             (16 * 1024)
#define PCM_BUFFER_SIZE             (8 * 1024)
#define HTTP_READ_CHUNK             2048
#define USER_AGENT                  "DNESP32S3-RGBLCD-Radio/1.0"

#define DEBOUNCE_CLICK_MS           300
#define DEBOUNCE_DOUBLE_MS          400

typedef enum {
    BTN_EVENT_NONE = 0,
    BTN_EVENT_CLICK,
    BTN_EVENT_DOUBLE_CLICK,
    BTN_EVENT_LONG_PRESS,
} button_event_t;

static bool s_i2s_installed = false;
static i2c_master_dev_handle_t s_es8388 = NULL;
static bool s_radio_running = false;
static bool s_radio_paused = false;
static volatile int s_current_station = 0;
static volatile int s_station_changed = 0;
static int s_current_url = 0;
static RingbufHandle_t s_pcm_ring = NULL;

static bool wifi_is_connected(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

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
    lv_label_set_text(title, "Entering Headless Radio");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFF2DC), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 105);

    lv_obj_t *body = lv_label_create(box);
    lv_label_set_text(body,
                      "Screen off, speaker on\n"
                      "BOOT click: next\n"
                      "BOOT double: pause\n"
                      "BOOT hold: exit");
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

static button_event_t poll_button(void)
{
    static int64_t s_last_click = 0;
    static int64_t s_last_press = 0;
    static bool s_was_pressed = false;
    static int64_t s_double_window = 0;
    static bool s_waiting_double = false;

    int level = gpio_get_level(BOOT_GPIO);
    bool pressed = (level == 0);
    int64_t now = esp_timer_get_time() / 1000;

    if (pressed && !s_was_pressed) {
        s_last_press = now;
        s_was_pressed = true;
    }

    if (!pressed && s_was_pressed) {
        s_was_pressed = false;
        int64_t hold_ms = now - s_last_press;

        if (hold_ms >= EXIT_HOLD_MS) {
            ESP_LOGI(TAG, "button long press: exit");
            return BTN_EVENT_LONG_PRESS;
        }

        if (s_waiting_double && (now - s_double_window) < DEBOUNCE_DOUBLE_MS) {
            s_waiting_double = false;
            ESP_LOGI(TAG, "button double click: pause/resume");
            return BTN_EVENT_DOUBLE_CLICK;
        }

        s_waiting_double = true;
        s_double_window = now;
    }

    if (s_waiting_double && (now - s_double_window) >= DEBOUNCE_DOUBLE_MS) {
        s_waiting_double = false;
        ESP_LOGI(TAG, "button click: next station");
        return BTN_EVENT_CLICK;
    }

    return BTN_EVENT_NONE;
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
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x2E, 30), TAG, "ES8388 headphone L volume failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x2F, 30), TAG, "ES8388 headphone R volume failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x30, 30), TAG, "ES8388 speaker L volume failed");
    ESP_RETURN_ON_ERROR(es8388_write_reg(0x31, 30), TAG, "ES8388 speaker R volume failed");

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

static size_t radio_audio_write_pcm(const int16_t *samples, size_t sample_count)
{
    if (!s_i2s_installed || !samples || sample_count == 0) {
        ESP_LOGW(TAG, "radio_audio_write_pcm: invalid params (i2s=%d, samples=%p, count=%u)",
                 s_i2s_installed, samples, (unsigned)sample_count);
        return 0;
    }

    size_t bytes_written = 0;
    esp_err_t ret = i2s_write(I2S_NUM_0, samples, sample_count * sizeof(int16_t),
                              &bytes_written, pdMS_TO_TICKS(200));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2s write failed: %s", esp_err_to_name(ret));
        return 0;
    }
    return bytes_written / sizeof(int16_t);
}

static bool url_looks_playable(const char *url)
{
    if (!url || !url[0]) {
        return false;
    }
    char lower[RADIO_MAX_URL];
    size_t i;
    for (i = 0; url[i] && i < sizeof(lower) - 1; i++) {
        lower[i] = (char)tolower((unsigned char)url[i]);
    }
    lower[i] = '\0';

    return strstr(lower, ".mp3") != NULL &&
           strstr(lower, ".m3u8") == NULL &&
           strstr(lower, ".aac") == NULL &&
           strstr(lower, ".flv") == NULL &&
           strstr(lower, "token=") == NULL;
}

static bool content_type_is_mp3(const char *content_type)
{
    if (!content_type || !content_type[0]) {
        return true;
    }
    return strstr(content_type, "audio/mpeg") != NULL ||
           strstr(content_type, "audio/mp3") != NULL ||
           strstr(content_type, "application/octet-stream") != NULL;
}

typedef struct {
    int status_code;
    int64_t content_length;
    char content_type[64];
    bool ok;
} probe_result_t;

static probe_result_t probe_url(const char *url, int station_index, int url_index)
{
    probe_result_t result = {
        .status_code = 0,
        .content_length = -1,
        .content_type = {0},
        .ok = false,
    };

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 2500,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .disable_auto_redirect = false,
        .max_redirection_count = 5,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGW(TAG, "probe init failed: url=%s", url);
        return result;
    }

    esp_http_client_set_header(client, "User-Agent", USER_AGENT);
    esp_http_client_set_header(client, "Accept", "audio/mpeg,*/*");
    esp_http_client_set_header(client, "Icy-MetaData", "0");

    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        result.status_code = esp_http_client_get_status_code(client);
        result.content_length = esp_http_client_get_content_length(client);
        char *ct = NULL;
        esp_http_client_get_header(client, "Content-Type", &ct);
        if (ct) {
            strncpy(result.content_type, ct, sizeof(result.content_type) - 1);
        }
        result.ok = result.status_code >= 200 && result.status_code < 400 &&
                    content_type_is_mp3(result.content_type);

        ESP_LOGI(TAG, "probe station=%d url_index=%d using_fallback=%s HTTP status=%d "
                      "content-type=%s content-length=%lld ok=%s",
                 station_index + 1, url_index + 1, url_index > 0 ? "yes" : "no",
                 result.status_code,
                 result.content_type[0] ? result.content_type : "(none)",
                 (long long)result.content_length, result.ok ? "yes" : "no");
    } else {
        ESP_LOGW(TAG, "probe open failed: station=%d url=%s err=%s",
                 station_index + 1, url, esp_err_to_name(err));
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

static void pcm_ring_init(void)
{
    if (!s_pcm_ring) {
        s_pcm_ring = xRingbufferCreate(64 * 1024, RINGBUF_TYPE_BYTEBUF);
        if (!s_pcm_ring) {
            ESP_LOGE(TAG, "Failed to create PCM ring buffer");
        }
    }
}

static void pcm_ring_flush(void)
{
    if (!s_pcm_ring) return;
    size_t size = 0;
    void *item = NULL;
    while ((item = xRingbufferReceive(s_pcm_ring, &size, 0)) != NULL) {
        vRingbufferReturnItem(s_pcm_ring, item);
    }
}

static void pcm_ring_deinit(void)
{
    if (s_pcm_ring) {
        pcm_ring_flush();
        vRingbufferDelete(s_pcm_ring);
        s_pcm_ring = NULL;
    }
}

static void play_task(void *arg)
{
    ESP_LOGI(TAG, "play task started");
    uint32_t play_count = 0;
    while (s_radio_running) {
        size_t rx_size = 0;
        void *item = xRingbufferReceiveUpTo(s_pcm_ring, &rx_size, pdMS_TO_TICKS(50), 4096);
        if (!item || rx_size == 0) {
            continue;
        }

        if (!s_radio_paused) {
            size_t samples = rx_size / sizeof(int16_t);
            size_t written = radio_audio_write_pcm((int16_t *)item, samples);
            play_count++;
            if (play_count <= 5 || play_count % 100 == 0) {
                ESP_LOGI(TAG, "play: rx_size=%u samples=%u written=%u", (unsigned)rx_size, (unsigned)samples, (unsigned)written);
            }
        }
        vRingbufferReturnItem(s_pcm_ring, item);
    }
    ESP_LOGI(TAG, "play task stopped");
    vTaskDelete(NULL);
}

static bool stream_station(const radio_station_t *station, int station_index)
{
    int total_urls = radio_stations_count();
    bool played = false;

    ESP_LOGI(TAG, "stream_station: name=%s index=%d", station->name, station_index);

    for (int url_idx = 0; url_idx < RADIO_MAX_URLS; url_idx++) {
        const char *url = radio_station_url_get(station_index, url_idx);
        if (!url || !url[0]) {
            ESP_LOGD(TAG, "  url[%d]: empty", url_idx);
            continue;
        }
        ESP_LOGI(TAG, "  url[%d]: %s", url_idx, url);

        if (!url_looks_playable(url)) {
            ESP_LOGW(TAG, "Skip non-MP3 url: %s", url);
            continue;
        }

        probe_result_t probe = probe_url(url, station_index, url_idx);
        if (!probe.ok) {
            ESP_LOGW(TAG, "Probe failed for url: %s", url);
            continue;
        }

        ESP_LOGI(TAG, "Opening station=%d/%d name=%s selected_url=%s using_fallback=%s "
                      "HTTP status=%d content-type=%s",
                 station_index + 1, total_urls, station->name, url,
                 url_idx > 0 ? "yes" : "no", probe.status_code,
                 probe.content_type[0] ? probe.content_type : "(none)");

        esp_http_client_config_t config = {
            .url = url,
            .timeout_ms = 4000,
            .buffer_size = 2048,
            .buffer_size_tx = 512,
            .disable_auto_redirect = false,
            .max_redirection_count = 5,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGW(TAG, "HTTP client init failed: %s", url);
            continue;
        }

        esp_http_client_set_header(client, "User-Agent", USER_AGENT);
        esp_http_client_set_header(client, "Accept", "audio/mpeg,*/*");
        esp_http_client_set_header(client, "Icy-MetaData", "0");

        esp_err_t open_err = esp_http_client_open(client, 0);
        if (open_err != ESP_OK) {
            ESP_LOGW(TAG, "open failed: %s err=%s", url, esp_err_to_name(open_err));
            esp_http_client_cleanup(client);
            continue;
        }

        esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        char *content_type = NULL;
        esp_http_client_get_header(client, "Content-Type", &content_type);
        int64_t content_length = esp_http_client_get_content_length(client);

        ESP_LOGI(TAG, "HTTP status=%d station=%s content-type=%s content-length=%lld",
                 status_code, station->name,
                 content_type ? content_type : "(none)",
                 (long long)content_length);

        if (status_code < 200 || status_code >= 400 || !content_type_is_mp3(content_type)) {
            ESP_LOGW(TAG, "skip URL: status=%d content-type=%s",
                     status_code, content_type ? content_type : "(none)");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        void *decoder = NULL;
        esp_audio_err_t dec_err = esp_mp3_dec_open(NULL, 0, &decoder);
        if (dec_err != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "MP3 decoder open failed: error=%d", dec_err);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        uint8_t *raw_buffer = malloc(RAW_BUFFER_SIZE);
        uint8_t *pcm_buffer = malloc(PCM_BUFFER_SIZE);
        if (!raw_buffer || !pcm_buffer) {
            ESP_LOGE(TAG, "Failed to allocate buffers");
            free(raw_buffer);
            free(pcm_buffer);
            esp_mp3_dec_close(decoder);
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            continue;
        }

        int raw_len = 0;
        int no_decode_count = 0;
        uint32_t decoded_frames = 0;
        bool stream_ok = true;

        ESP_LOGI(TAG, "Start streaming: %s", station->name);
        ESP_LOGI(TAG, "  raw_buffer=%p pcm_buffer=%p", raw_buffer, pcm_buffer);
        ESP_LOGI(TAG, "  decoder=%p", decoder);

        int my_station = station_index;
        while (s_radio_running && stream_ok) {
            if (s_station_changed || my_station != s_current_station) {
                ESP_LOGI(TAG, "Station changed, breaking stream");
                break;
            }

            if (raw_len < (int)(RAW_BUFFER_SIZE - HTTP_READ_CHUNK)) {
                int read = esp_http_client_read(client, (char *)(raw_buffer + raw_len),
                                                 RAW_BUFFER_SIZE - raw_len);
                if (read <= 0) {
                    ESP_LOGW(TAG, "stream read ended: read=%d", read);
                    break;
                }
                raw_len += read;
            }

            bool decoded_any = false;
            while (raw_len > 0 && s_radio_running) {
                esp_audio_dec_in_raw_t raw = {
                    .buffer = raw_buffer,
                    .len = raw_len,
                };
                esp_audio_dec_out_frame_t frame = {
                    .buffer = pcm_buffer,
                    .len = PCM_BUFFER_SIZE,
                };
                esp_audio_dec_info_t info = {0};

                dec_err = esp_mp3_dec_decode(decoder, &raw, &frame, &info);
                ESP_LOGD(TAG, "decode error=%d raw.consumed=%u frame.decoded_size=%u "
                              "sample_rate=%d channels=%d bitrate=%d",
                         dec_err, (unsigned)raw.consumed, (unsigned)frame.decoded_size,
                         info.sample_rate, info.channel, info.bitrate);

                if (dec_err != ESP_AUDIO_ERR_OK || raw.consumed == 0) {
                    ESP_LOGW(TAG, "decoder error=%d consumed=%u", dec_err, (unsigned)raw.consumed);
                    break;
                }

                decoded_any = true;
                no_decode_count = 0;

                if (raw.consumed < (uint32_t)raw_len) {
                    memmove(raw_buffer, raw_buffer + raw.consumed, raw_len - raw.consumed);
                }
                raw_len -= raw.consumed;

                if (frame.decoded_size == 0) continue;

                decoded_frames++;
                if (decoded_frames <= 3 || decoded_frames % 200 == 0) {
                    ESP_LOGI(TAG, "decoded frame=%u consumed=%u decoded_size=%u "
                                  "sample_rate=%d channels=%d bitrate=%d",
                             (unsigned)decoded_frames, (unsigned)raw.consumed,
                             (unsigned)frame.decoded_size,
                             info.sample_rate, info.channel, info.bitrate);
                }

                if (s_pcm_ring && frame.decoded_size > 0) {
                    esp_err_t send_ret = xRingbufferSend(s_pcm_ring, pcm_buffer, frame.decoded_size, pdMS_TO_TICKS(100));
                    if (send_ret != pdTRUE) {
                        ESP_LOGW(TAG, "PCM ring buffer send failed");
                    }
                }
            }

            if (!decoded_any && raw_len >= (int)(RAW_BUFFER_SIZE - HTTP_READ_CHUNK)) {
                no_decode_count++;
                ESP_LOGW(TAG, "no MP3 frame decoded: raw_len=%d failures=%d", raw_len, no_decode_count);
                raw_len = 0;
                if (no_decode_count >= 3) {
                    ESP_LOGW(TAG, "skip undecodable URL");
                    stream_ok = false;
                }
            }

            vTaskDelay(pdMS_TO_TICKS(2));
        }

        free(raw_buffer);
        free(pcm_buffer);
        esp_mp3_dec_close(decoder);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        pcm_ring_flush();

        if (decoded_frames > 0) {
            played = true;
            break;
        }
    }

    return played;
}

static void stream_task(void *arg)
{
    ESP_LOGI(TAG, "stream task started");

    while (s_radio_running) {
        if (!wifi_is_connected()) {
            ESP_LOGW(TAG, "WiFi not connected, waiting...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        s_station_changed = 0;
        int station_count = radio_stations_count();
        if (station_count <= 0) {
            ESP_LOGE(TAG, "No stations available");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        if (s_current_station >= station_count) {
            s_current_station = 0;
        }

        const radio_station_t *station = radio_station_get(s_current_station);
        if (!station) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "station=%d/%d name=%s category=%s",
                 s_current_station + 1, station_count, station->name, station->category);

        bool played = stream_station(station, s_current_station);
        if (!played && s_radio_running) {
            ESP_LOGW(TAG, "Source failed for station: %s", station->name);
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    ESP_LOGI(TAG, "stream task stopped");
    vTaskDelete(NULL);
}

static void run_radio(void)
{
    ESP_LOGI(TAG, "run_radio: start");

    if (!wifi_is_connected()) {
        ESP_LOGE(TAG, "WiFi not connected! Please connect to WiFi first.");
        ESP_LOGE(TAG, "Exiting radio mode.");
        return;
    }
    ESP_LOGI(TAG, "WiFi connected: OK");

    esp_err_t codec_ret = es8388_init_minimal();
    if (codec_ret != ESP_OK) {
        ESP_LOGE(TAG, "Init ES8388: fail (%s)", esp_err_to_name(codec_ret));
        return;
    }
    ESP_LOGI(TAG, "ES8388 init result: success");

    esp_err_t i2s_ret = i2s_init();
    if (i2s_ret != ESP_OK) {
        ESP_LOGE(TAG, "Init I2S: fail (%s)", esp_err_to_name(i2s_ret));
        return;
    }
    ESP_LOGI(TAG, "I2S init result: success");

    pcm_ring_init();
    radio_stations_init();

    int station_count = radio_stations_count();
    ESP_LOGI(TAG, "Total stations: %d", station_count);
    for (int i = 0; i < station_count; i++) {
        const radio_station_t *s = radio_station_get(i);
        if (s) {
            ESP_LOGI(TAG, "  Station %d: %s (%s)", i + 1, s->name, s->category);
        }
    }

    s_radio_running = true;
    s_radio_paused = false;
    s_current_station = 0;

    TaskHandle_t stream_handle = NULL;
    TaskHandle_t play_handle = NULL;

    if (xTaskCreate(stream_task, "radio_stream", 12288, NULL, 5, &stream_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create stream task");
        s_radio_running = false;
    }
    if (xTaskCreate(play_task, "radio_play", 4096, NULL, 6, &play_handle) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create play task");
        s_radio_running = false;
    }

    while (s_radio_running) {
        button_event_t event = poll_button();
        switch (event) {
            case BTN_EVENT_CLICK:
                ESP_LOGI(TAG, "button click: next station");
                s_current_station = (s_current_station + 1) % radio_stations_count();
                s_station_changed++;
                pcm_ring_flush();
                break;
            case BTN_EVENT_DOUBLE_CLICK:
                s_radio_paused = !s_radio_paused;
                ESP_LOGI(TAG, "button double click: %s", s_radio_paused ? "paused" : "resumed");
                break;
            case BTN_EVENT_LONG_PRESS:
                ESP_LOGI(TAG, "button long press: exit");
                s_radio_running = false;
                break;
            default:
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
    }

    for (int i = 0; i < 50 && (stream_handle || play_handle); i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    pcm_ring_deinit();
    radio_i2s_stop();
    es8388_stop();
}

#ifdef RADIO_BEEP_TEST_ENABLED
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

static void run_beep_test(void)
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
        button_event_t event = poll_button();
        if (event == BTN_EVENT_LONG_PRESS) {
            ESP_LOGI(TAG, "BOOT long press detected");
            break;
        }

        ESP_LOGI(TAG, "beep loop alive");
        play_beep_once();
        vTaskDelay(pdMS_TO_TICKS(BEEP_PERIOD_MS - BEEP_MS));
        if (++loops % BEEP_LOOPS_BEFORE_IDLE_LOG == 0) {
            ESP_LOGI(TAG, "beep loops: %u", (unsigned)loops);
        }
    }

    radio_i2s_stop();
    es8388_stop();
}
#endif

void radio_headless_test_start(void)
{
    ESP_LOGI(TAG, "Enter headless radio");
    warning_page_show();

    ESP_LOGI(TAG, "Stop LVGL tick/flush");
    lvgl_demo_suspend();

    ESP_LOGI(TAG, "Turn off backlight");
    LCD_BL(0);
    vTaskDelay(pdMS_TO_TICKS(80));

    esp_err_t lcd_ret = lcd_deinit();
    if (lcd_ret == ESP_OK) {
        ESP_LOGI(TAG, "LCD deinit result: success");
    } else {
        ESP_LOGW(TAG, "LCD deinit result: fail (%s)", esp_err_to_name(lcd_ret));
    }

    reset_conflict_gpios();
    boot_button_init();

#ifdef RADIO_BEEP_TEST_ENABLED
    ESP_LOGI(TAG, "Start beep test");
    run_beep_test();
#else
    ESP_LOGI(TAG, "Start MP3 radio");
    run_radio();
#endif

    ESP_LOGI(TAG, "Exit radio");

    ESP_LOGI(TAG, "Restore GPIO");
    reset_conflict_gpios();

    ESP_LOGI(TAG, "Reinit RGB LCD: start");
    lcd_init();
    if (lcddev.lcd_panel_handle) {
        ESP_LOGI(TAG, "LCD restore result: success");
        ESP_LOGI(TAG, "Restore LVGL");
        lvgl_demo_rebind_display();
        lvgl_demo_resume();
        ESP_LOGI(TAG, "Restore backlight");
        LCD_BL(1);
        ESP_LOGI(TAG, "Return menu");
        menu_start();
    } else {
        ESP_LOGE(TAG, "LCD restore result: fail");
        ESP_LOGE(TAG, "LCD restore not implemented, please reboot to return to UI.");
    }
}
