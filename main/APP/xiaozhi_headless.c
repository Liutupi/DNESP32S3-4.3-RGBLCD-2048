/**
 * 小智AI无头模式实现
 * 基于WebSocket协议与小智服务器通信
 */

#include "xiaozhi_headless.h"

#include "board_audio.h"
#include "es8388.h"
#include "lcd.h"
#include "lvgl_demo.h"
#include "menu.h"
#include "ui_fonts.h"
#include "ui_text.h"

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_http_client.h"
#include "esp_websocket_client.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#include <stdbool.h>
#include <string.h>
#include <math.h>

#define TAG "XIAOZHI"

/* 小智服务器配置 */
#define XIAOZHI_OTA_URL "http://api.tenclass.net/xiaozhi/ota/"

/* 设备信息 */
static char s_device_id[32] = {0};
static bool s_is_bound = false;

/* WebSocket配置 */
static char s_ws_url[256] = {0};
static char s_ws_token[256] = {0};
static esp_websocket_client_handle_t s_ws_client = NULL;

/* 设备MAC地址 */
#define DEVICE_MAC "14:c1:9f:42:2f:94"

/* 音频配置 */
#define AUDIO_SAMPLE_RATE 16000
#define AUDIO_FRAME_SIZE 1024
#define AUDIO_BUFFER_SIZE (AUDIO_FRAME_SIZE * 2) /* 双声道 */

/* 提示音频率 */
#define TONE_START_FREQ 1000  /* 开始录音提示音 */
#define TONE_STOP_FREQ 800    /* 停止录音提示音 */
#define TONE_ERROR_FREQ 400   /* 错误提示音 */
#define TONE_READY_FREQ 1200  /* 就绪提示音 */

/* 音频缓冲区 */
static int16_t s_audio_buffer[AUDIO_BUFFER_SIZE];
static bool s_audio_initialized = false;

/* 状态枚举 */
typedef enum {
    XIAOZHI_STATE_IDLE,
    XIAOZHI_STATE_CONNECTING,
    XIAOZHI_STATE_LISTENING,
    XIAOZHI_STATE_THINKING,
    XIAOZHI_STATE_SPEAKING,
} xiaozhi_state_t;

/* 全局变量 */
static bool s_is_running = false;
static xiaozhi_state_t s_state = XIAOZHI_STATE_IDLE;
static SemaphoreHandle_t s_state_mutex = NULL;
static lv_obj_t *s_xiaozhi_scr = NULL;
static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_response_label = NULL;
static char s_session_id[64] = {0};

/* 复位冲突的GPIO引脚 */
static void reset_conflict_gpios(void)
{
    gpio_reset_pin(GPIO_NUM_3);
    gpio_reset_pin(GPIO_NUM_46);
    gpio_reset_pin(GPIO_NUM_9);
    gpio_reset_pin(GPIO_NUM_10);
    gpio_reset_pin(GPIO_NUM_14);
    ESP_LOGI(TAG, "CONFLICT_GPIOS_RESET_OK");
}

/* 进入无头音频模式 */
static bool enter_headless_audio(void)
{
    ESP_LOGI(TAG, "ENTER_HEADLESS_AUDIO");

    /* 1. 暂停LVGL */
    lvgl_demo_suspend();
    ESP_LOGI(TAG, "LVGL_SUSPEND_OK");

    /* 2. 关闭RGB LCD */
    esp_err_t lcd_ret = lcd_deinit();
    if (lcd_ret == ESP_OK || lcd_ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "LCD_DEINIT_OK");
    } else {
        ESP_LOGE(TAG, "LCD_DEINIT_FAILED: %s", esp_err_to_name(lcd_ret));
        return false;
    }

    /* 3. 关闭背光 */
    LCD_BL(0);
    ESP_LOGI(TAG, "LCD_BACKLIGHT_OFF_OK");

    /* 4. 复位冲突的GPIO */
    reset_conflict_gpios();

    /* 5. 初始化I2C和扩展芯片 */
    if (board_audio_init_i2c_and_expander() != ESP_OK) {
        ESP_LOGE(TAG, "I2C_INIT_FAILED");
        return false;
    }

    /* 6. 初始化I2S */
    if (board_audio_i2s_start(AUDIO_SAMPLE_RATE) != ESP_OK) {
        ESP_LOGE(TAG, "I2S_INIT_FAILED");
        return false;
    }

    /* 7. 初始化ES8388编解码器 */
    if (board_audio_codec_start(28) != ESP_OK) {
        ESP_LOGE(TAG, "ES8388_INIT_FAILED");
        return false;
    }

    /* 8. 启用ADC（麦克风输入）和DAC（扬声器输出） */
    if (es8388_adda_cfg(1, 1) != ESP_OK) {
        ESP_LOGE(TAG, "ADC_DAC_ENABLE_FAILED");
        return false;
    }

    /* 9. 打开扬声器 */
    if (board_audio_speaker_enable(true) != ESP_OK) {
        ESP_LOGE(TAG, "SPEAKER_ENABLE_FAILED");
        return false;
    }

    ESP_LOGI(TAG, "HEADLESS_AUDIO_OK");
    return true;
}

/* 恢复显示 */
static void restore_display(void)
{
    ESP_LOGI(TAG, "RESTORE_DISPLAY");

    /* 复位冲突的GPIO */
    reset_conflict_gpios();

    /* 重新初始化LCD */
    lcd_init();
    ESP_LOGI(TAG, "LCD_INIT_OK");

    /* 恢复LVGL */
    lvgl_demo_rebind_display();
    lvgl_demo_resume();
    ESP_LOGI(TAG, "LVGL_RESUME_OK");

    /* 打开背光 */
    LCD_BL(1);
    ESP_LOGI(TAG, "LCD_BACKLIGHT_ON_OK");
}

/* 读取麦克风数据 */
static size_t read_microphone(int16_t *buffer, size_t samples)
{
    if (!s_audio_initialized || !buffer || samples == 0) {
        return 0;
    }

    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(board_audio_i2s_get_handle(),
                                     buffer,
                                     samples * sizeof(int16_t),
                                     &bytes_read,
                                     pdMS_TO_TICKS(100));

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S_READ_FAILED: %s", esp_err_to_name(err));
        return 0;
    }

    return bytes_read / sizeof(int16_t);
}

/* 停止音频 */
static void stop_audio(void)
{
    if (s_audio_initialized) {
        board_audio_speaker_enable(false);
        board_audio_codec_stop();
        board_audio_i2s_stop();
        s_audio_initialized = false;
        ESP_LOGI(TAG, "AUDIO_STOPPED");
    }
}

/* 播放提示音 */
static void play_tone(int freq, int duration_ms)
{
    if (!s_audio_initialized) return;

    ESP_LOGI(TAG, "PLAY_TONE freq=%d duration=%d", freq, duration_ms);

    int16_t buffer[128 * 2];
    uint32_t total = (AUDIO_SAMPLE_RATE * duration_ms) / 1000;
    uint32_t pos = 0;

    while (pos < total) {
        uint32_t n = total - pos;
        if (n > 128) n = 128;

        for (uint32_t i = 0; i < n; ++i) {
            float phase = 2.0f * 3.14159f * freq * (pos + i) / AUDIO_SAMPLE_RATE;
            int16_t sample = (int16_t)(sinf(phase) * 3000.0f);
            buffer[i * 2] = sample;
            buffer[i * 2 + 1] = sample;
        }

        board_audio_i2s_write(buffer, n * 2);
        pos += n;
    }
}

/* 播放开始录音提示音 */
static void play_start_tone(void)
{
    play_tone(TONE_START_FREQ, 200);
}

/* 播放停止录音提示音 */
static void play_stop_tone(void)
{
    play_tone(TONE_STOP_FREQ, 300);
}

/* 播放错误提示音 */
static void play_error_tone(void)
{
    play_tone(TONE_ERROR_FREQ, 500);
}

/* 播放就绪提示音 */
static void play_ready_tone(void)
{
    play_tone(TONE_READY_FREQ, 150);
    vTaskDelay(pdMS_TO_TICKS(100));
    play_tone(TONE_READY_FREQ, 150);
}

/* 状态管理 */
static void set_state(xiaozhi_state_t state)
{
    if (s_state_mutex) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        s_state = state;
        xSemaphoreGive(s_state_mutex);
    }
}

static xiaozhi_state_t get_state(void)
{
    xiaozhi_state_t state = XIAOZHI_STATE_IDLE;
    if (s_state_mutex) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        state = s_state;
        xSemaphoreGive(s_state_mutex);
    }
    return state;
}

/* WiFi状态检查 */
static bool wifi_is_connected(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

/* 更新UI显示 */
static void update_ui(const char *status, const char *response)
{
    if (s_status_label && status) {
        lv_label_set_text(s_status_label, status);
    }
    if (s_response_label && response) {
        lv_label_set_text(s_response_label, response);
    }
    if (s_xiaozhi_scr) {
        lv_scr_load(s_xiaozhi_scr);
    }
}

/* 发送WebSocket JSON消息 */
static bool ws_send_json(cJSON *json)
{
    if (!s_ws_client || !json) return false;

    char *str = cJSON_PrintUnformatted(json);
    if (!str) return false;

    ESP_LOGI(TAG, "WS_SEND: %s", str);
    int sent = esp_websocket_client_send_text(s_ws_client, str, strlen(str), pdMS_TO_TICKS(1000));
    free(str);

    return sent > 0;
}

/* 发送hello消息 */
static void send_hello(void)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "hello");
    cJSON_AddStringToObject(msg, "version", "1.0.0");
    cJSON_AddStringToObject(msg, "transport", "websocket");
    cJSON_AddStringToObject(msg, "device_id", DEVICE_MAC);

    /* 音频参数 */
    cJSON *audio_params = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_params, "format", "opus");
    cJSON_AddNumberToObject(audio_params, "sample_rate", AUDIO_SAMPLE_RATE);
    cJSON_AddNumberToObject(audio_params, "frame_duration", 60);
    cJSON_AddItemToObject(msg, "audio_params", audio_params);

    /* 特性 */
    cJSON *features = cJSON_CreateArray();
    cJSON_AddItemToArray(features, cJSON_CreateString("aec"));
    cJSON_AddItemToArray(features, cJSON_CreateString("mcp"));
    cJSON_AddItemToObject(msg, "features", features);

    ws_send_json(msg);
    cJSON_Delete(msg);
}

/* 发送开始监听消息 */
static void send_start_listening(void)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "start_listening");
    cJSON_AddStringToObject(msg, "mode", "auto");
    cJSON_AddStringToObject(msg, "session_id", s_session_id);

    ws_send_json(msg);
    cJSON_Delete(msg);
}

/* 发送停止监听消息 */
static void send_stop_listening(void)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "stop_listening");
    cJSON_AddStringToObject(msg, "session_id", s_session_id);

    ws_send_json(msg);
    cJSON_Delete(msg);
}

/* 发送唤醒词检测消息 */
static void send_wake_word_detected(const char *wake_word)
{
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "wake_word_detected");
    cJSON_AddStringToObject(msg, "session_id", s_session_id);
    cJSON_AddStringToObject(msg, "wake_word", wake_word);

    ws_send_json(msg);
    cJSON_Delete(msg);
}

/* 处理服务器hello响应 */
static void handle_server_hello(cJSON *root)
{
    cJSON *session_id = cJSON_GetObjectItem(root, "session_id");
    if (session_id && cJSON_IsString(session_id)) {
        strncpy(s_session_id, session_id->valuestring, sizeof(s_session_id) - 1);
        ESP_LOGI(TAG, "Session ID: %s", s_session_id);
    }

    cJSON *transport = cJSON_GetObjectItem(root, "transport");
    if (transport && cJSON_IsString(transport)) {
        ESP_LOGI(TAG, "Transport: %s", transport->valuestring);
    }

    /* 服务器hello成功，进入空闲状态 */
    set_state(XIAOZHI_STATE_IDLE);
    update_ui("Connected", "Ready to chat!\nPress BOOT to talk");
}

/* 处理TTS消息 */
static void handle_tts_message(cJSON *root)
{
    cJSON *state = cJSON_GetObjectItem(root, "state");
    if (state && cJSON_IsString(state)) {
        if (strcmp(state->valuestring, "start") == 0) {
            ESP_LOGI(TAG, "TTS start");
            set_state(XIAOZHI_STATE_SPEAKING);
            update_ui("AI Speaking:", "");
        } else if (strcmp(state->valuestring, "stop") == 0) {
            ESP_LOGI(TAG, "TTS stop");
            set_state(XIAOZHI_STATE_IDLE);
            update_ui("Ready", "Press BOOT to talk");
        }
    }

    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (text && cJSON_IsString(text)) {
        ESP_LOGI(TAG, "TTS text: %s", text->valuestring);
        update_ui("AI:", text->valuestring);
    }
}

/* 处理STT消息 */
static void handle_stt_message(cJSON *root)
{
    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (text && cJSON_IsString(text)) {
        ESP_LOGI(TAG, "STT text: %s", text->valuestring);
        update_ui("You said:", text->valuestring);
    }
}

/* 处理LLM消息 */
static void handle_llm_message(cJSON *root)
{
    cJSON *text = cJSON_GetObjectItem(root, "text");
    if (text && cJSON_IsString(text)) {
        ESP_LOGI(TAG, "LLM text: %s", text->valuestring);
        update_ui("AI:", text->valuestring);
    }
}

/* 处理emotion消息 */
static void handle_emotion_message(cJSON *root)
{
    cJSON *emotion = cJSON_GetObjectItem(root, "emotion");
    if (emotion && cJSON_IsString(emotion)) {
        ESP_LOGI(TAG, "Emotion: %s", emotion->valuestring);
    }
}

/* 处理JSON消息 */
static void handle_json_message(const char *data, int len)
{
    char *str = strndup(data, len);
    ESP_LOGI(TAG, "WS_RECV: %s", str);

    cJSON *root = cJSON_Parse(str);
    free(str);

    if (!root) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        return;
    }

    cJSON *type = cJSON_GetObjectItem(root, "type");
    if (!type || !cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    const char *type_str = type->valuestring;

    if (strcmp(type_str, "hello") == 0) {
        handle_server_hello(root);
    } else if (strcmp(type_str, "tts") == 0) {
        handle_tts_message(root);
    } else if (strcmp(type_str, "stt") == 0) {
        handle_stt_message(root);
    } else if (strcmp(type_str, "llm") == 0) {
        handle_llm_message(root);
    } else if (strcmp(type_str, "emotion") == 0) {
        handle_emotion_message(root);
    } else if (strcmp(type_str, "audio") == 0) {
        /* 处理音频响应 */
        cJSON *audio_data = cJSON_GetObjectItem(root, "data");
        if (audio_data && cJSON_IsString(audio_data)) {
            ESP_LOGI(TAG, "Received audio data");
        }
    } else {
        ESP_LOGW(TAG, "Unknown message type: %s", type_str);
    }

    cJSON_Delete(root);
}

/* WebSocket事件处理 */
static void websocket_event_handler(void *handler_args, esp_event_base_t base,
                                    int32_t event_id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;

    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WS_CONNECTED");
            update_ui("Connected", "Sending hello...");
            send_hello();
            break;

        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "WS_DISCONNECTED");
            set_state(XIAOZHI_STATE_IDLE);
            update_ui("Disconnected", "Reconnecting...");
            break;

        case WEBSOCKET_EVENT_DATA:
            if (data->op_code == 0x01) { /* Text frame */
                handle_json_message(data->data_ptr, data->data_len);
            } else if (data->op_code == 0x02) { /* Binary frame - audio data */
                ESP_LOGD(TAG, "WS_BINARY len=%d", data->data_len);
            }
            break;

        case WEBSOCKET_EVENT_ERROR:
            ESP_LOGE(TAG, "WS_ERROR");
            update_ui("Error", "WebSocket error");
            break;

        default:
            break;
    }
}

/* 初始化WebSocket */
static bool init_websocket(void)
{
    ESP_LOGI(TAG, "INIT_WS url=%s", s_ws_url);

    if (s_ws_url[0] == '\0') {
        ESP_LOGE(TAG, "No WebSocket URL");
        return false;
    }

    /* 构建带token的URL */
    char url_with_token[1024];
    snprintf(url_with_token, sizeof(url_with_token), "%s?token=%s", s_ws_url, s_ws_token);

    esp_websocket_client_config_t ws_cfg = {
        .uri = url_with_token,
        .buffer_size = 4096,
        .task_stack = 4096,
        .disable_auto_reconnect = false,
        .reconnect_timeout_ms = 5000,
    };

    s_ws_client = esp_websocket_client_init(&ws_cfg);
    if (!s_ws_client) {
        ESP_LOGE(TAG, "WS_INIT_FAILED");
        return false;
    }

    /* 注册事件处理 */
    esp_websocket_register_events(s_ws_client, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL);

    /* 设置自定义头部 */
    esp_websocket_client_append_header(s_ws_client, "Device-Id", s_device_id);

    /* 设置Authorization header */
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", s_ws_token);
    esp_websocket_client_append_header(s_ws_client, "Authorization", auth_header);

    esp_err_t err = esp_websocket_client_start(s_ws_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WS_START_FAILED: %s", esp_err_to_name(err));
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
        return false;
    }

    return true;
}

/* 停止WebSocket */
static void stop_websocket(void)
{
    if (s_ws_client) {
        esp_websocket_client_stop(s_ws_client);
        esp_websocket_client_destroy(s_ws_client);
        s_ws_client = NULL;
    }
}

/* 发送音频数据 */
static bool send_audio_data(const void *data, size_t len)
{
    if (!s_ws_client || get_state() != XIAOZHI_STATE_LISTENING) {
        return false;
    }

    int sent = esp_websocket_client_send_bin(s_ws_client, data, len, pdMS_TO_TICKS(100));
    return sent > 0;
}

/* 音频录制任务 */
static void audio_record_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "AUDIO_RECORD_TASK_START");

    while (s_is_running) {
        if (get_state() == XIAOZHI_STATE_LISTENING) {
            /* 读取麦克风数据 */
            size_t samples_read = read_microphone(s_audio_buffer, AUDIO_FRAME_SIZE);
            if (samples_read > 0) {
                /* 发送音频数据到服务器 */
                send_audio_data(s_audio_buffer, samples_read * sizeof(int16_t));
                ESP_LOGD(TAG, "AUDIO_SENT samples=%d", samples_read);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "AUDIO_RECORD_TASK_END");
    vTaskDelete(NULL);
}

/* 开始录音 */
static void start_listening(void)
{
    ESP_LOGI(TAG, "START_LISTENING");
    play_start_tone(); /* 播放开始提示音 */
    set_state(XIAOZHI_STATE_LISTENING);
    send_start_listening();

    /* 创建音频录制任务 */
    TaskHandle_t audio_task = NULL;
    if (xTaskCreate(audio_record_task, "audio_record", 4096, NULL, 7,
                    &audio_task) != pdPASS) {
        ESP_LOGE(TAG, "CREATE_AUDIO_TASK_FAILED");
    }
}

/* 停止录音 */
static void stop_listening(void)
{
    ESP_LOGI(TAG, "STOP_LISTENING");
    play_stop_tone(); /* 播放停止提示音 */
    set_state(XIAOZHI_STATE_THINKING);
    send_stop_listening();
}

/* 创建配置界面 */
static void create_config_ui(void)
{
    /* 创建新屏幕 */
    s_xiaozhi_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_xiaozhi_scr, lv_color_hex(0x1A1A2E), 0);

    /* 标题 */
    lv_obj_t *title = lv_label_create(s_xiaozhi_scr);
    lv_label_set_text(title, "XiaoZhi AI");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE67E22), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    /* 状态显示 */
    s_status_label = lv_label_create(s_xiaozhi_scr);
    lv_label_set_text(s_status_label, "Connecting...");
    lv_obj_set_style_text_font(s_status_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x3498DB), 0);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, -60);

    /* 响应显示 */
    s_response_label = lv_label_create(s_xiaozhi_scr);
    lv_label_set_text(s_response_label, "Waiting for connection...");
    lv_obj_set_style_text_font(s_response_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_response_label, lv_color_hex(0xECF0F1), 0);
    lv_obj_set_width(s_response_label, 700);
    lv_obj_set_style_text_align(s_response_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_response_label, LV_ALIGN_CENTER, 0, 20);

    /* 操作提示 */
    lv_obj_t *hint = lv_label_create(s_xiaozhi_scr);
    lv_label_set_text(hint, "Press BOOT to talk\nLong press to exit");
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x95A5A6), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -40);

    /* 加载屏幕 */
    lv_scr_load(s_xiaozhi_scr);
}

/* 获取设备ID（MAC地址） */
static void get_device_id(void)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    snprintf(s_device_id, sizeof(s_device_id), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "Device ID: %s", s_device_id);
}

/* 使用手动配置的WebSocket */
static bool fetch_server_config(void)
{
    ESP_LOGI(TAG, "FETCH_CONFIG (MANUAL_WS)");

    /* 获取设备ID */
    get_device_id();

    /* 手动配置WebSocket - 从服务器获取的配置 */
    strncpy(s_ws_url, "wss://api.tenclass.net/xiaozhi/v1/", sizeof(s_ws_url) - 1);
    strncpy(s_ws_token, "test-token", sizeof(s_ws_token) - 1);

    ESP_LOGI(TAG, "WebSocket URL: %s", s_ws_url);
    ESP_LOGI(TAG, "WebSocket token: %s", s_ws_token);

    /* 设备已绑定 */
    s_is_bound = true;
    ESP_LOGI(TAG, "Device is already bound (manual WebSocket config)");

    return true;
}

/* 重试标志 */
static volatile bool s_retry_requested = false;

/* 请求重试 */
static void request_retry(void)
{
    s_retry_requested = true;
}

/* 主任务 */
static void xiaozhi_main_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "MAIN_TASK_START");

    /* 创建配置界面 */
    create_config_ui();

    bool config_success = false;

    /* 重试循环 */
    while (s_is_running && !config_success) {
        s_retry_requested = false;

        update_ui("Connecting to WiFi...", "");
        vTaskDelay(pdMS_TO_TICKS(500));

        /* 检查WiFi */
        if (!wifi_is_connected()) {
            update_ui("WiFi not connected", "Please connect WiFi first\nPress BOOT to retry\nLong press to exit");
            ESP_LOGE(TAG, "WiFi not connected");

            /* 等待用户操作 */
            while (s_is_running && !s_retry_requested) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue; /* 重试 */
        }

        update_ui("WiFi connected", "Fetching config...");
        ESP_LOGI(TAG, "WiFi connected, fetching config...");

        /* 获取服务器配置 */
        if (!fetch_server_config()) {
            update_ui("Config Failed", "Cannot get server config\nPress BOOT to retry\nLong press to exit");
            ESP_LOGE(TAG, "Failed to fetch config");

            /* 等待用户操作 */
            while (s_is_running && !s_retry_requested) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            continue; /* 重试 */
        }

        config_success = true;
    }

    if (!s_is_running || !config_success) {
        menu_start();
        vTaskDelete(NULL);
        return;
    }

    update_ui("Connecting to AI...", "");

    /* 连接到WebSocket服务器 */
    if (!init_websocket()) {
        update_ui("Connection Failed", "Cannot connect to AI server\nPress BOOT to retry\nLong press to exit");
        ESP_LOGE(TAG, "WebSocket connection failed");

        /* 等待用户操作 */
        while (s_is_running) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        menu_start();
        vTaskDelete(NULL);
        return;
    }

    /* 进入无头音频模式（关闭屏幕，初始化音频） */
    if (!enter_headless_audio()) {
        update_ui("Audio init failed", "Cannot initialize audio");
        vTaskDelay(pdMS_TO_TICKS(2000));
        s_is_running = false;
        stop_websocket();
        restore_display();
        menu_start();
        vTaskDelete(NULL);
        return;
    }

    s_audio_initialized = true;
    ESP_LOGI(TAG, "READY_TO_TALK");
    play_ready_tone(); /* 播放就绪提示音 */

    /* 主循环 - 检查退出标志 */
    while (s_is_running) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    /* 清理 */
    ESP_LOGI(TAG, "MAIN_TASK_CLEANUP");
    stop_audio();
    stop_websocket();
    restore_display();
    s_is_running = false;
    menu_start();
    vTaskDelete(NULL);
}

/* BOOT按钮监控任务 */
static void boot_monitor_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "BOOT_MONITOR_START");

    /* 配置BOOT按钮 */
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    bool was_pressed = false;
    int64_t press_start_ms = 0;

    while (s_is_running) {
        bool pressed = gpio_get_level(GPIO_NUM_0) == 0;
        int64_t now_ms = esp_timer_get_time() / 1000;

        /* 按下检测 */
        if (pressed && !was_pressed) {
            was_pressed = true;
            press_start_ms = now_ms;
            ESP_LOGI(TAG, "BOOT_PRESSED");
        }

        /* 长按检测 - 1.5秒 */
        if (pressed && was_pressed && (now_ms - press_start_ms >= 1500)) {
            ESP_LOGI(TAG, "BOOT_LONG_PRESS_EXIT");
            s_is_running = false;
            was_pressed = false;
            vTaskDelay(pdMS_TO_TICKS(500)); /* 防止重复触发 */
            continue;
        }

        /* 释放检测 */
        if (!pressed && was_pressed) {
            int64_t press_duration = now_ms - press_start_ms;
            was_pressed = false;

            if (press_duration < 1500) {
                /* 短按 */
                ESP_LOGI(TAG, "BOOT_SHORT_PRESS duration=%lldms", press_duration);

                /* 如果在错误/等待状态，触发重试 */
                if (get_state() == XIAOZHI_STATE_IDLE && s_status_label) {
                    const char *current_text = lv_label_get_text(s_status_label);
                    if (current_text && (
                        strstr(current_text, "Failed") ||
                        strstr(current_text, "not connected") ||
                        strstr(current_text, "Error"))) {
                        ESP_LOGI(TAG, "RETRY_REQUESTED");
                        request_retry();
                        continue;
                    }
                }

                /* 正常的录音控制 */
                if (get_state() == XIAOZHI_STATE_IDLE) {
                    start_listening();
                } else if (get_state() == XIAOZHI_STATE_LISTENING) {
                    stop_listening();
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }

    ESP_LOGI(TAG, "BOOT_MONITOR_EXIT");
    vTaskDelete(NULL);
}

/* 公共接口 */
void xiaozhi_headless_start(void)
{
    ESP_LOGI(TAG, "HEADLESS_START");

    if (s_is_running) {
        ESP_LOGW(TAG, "ALREADY_RUNNING");
        return;
    }

    s_is_running = true;
    s_state_mutex = xSemaphoreCreateMutex();

    /* 创建主任务 */
    TaskHandle_t main_task = NULL;
    if (xTaskCreate(xiaozhi_main_task, "xiaozhi_main", 8192, NULL, 5,
                    &main_task) != pdPASS) {
        ESP_LOGE(TAG, "CREATE_MAIN_TASK_FAILED");
        s_is_running = false;
        return;
    }

    /* 创建BOOT监控任务 */
    TaskHandle_t boot_task = NULL;
    if (xTaskCreate(boot_monitor_task, "xiaozhi_boot", 4096, NULL, 6,
                    &boot_task) != pdPASS) {
        ESP_LOGE(TAG, "CREATE_BOOT_TASK_FAILED");
        s_is_running = false;
    }
}

bool xiaozhi_headless_is_running(void)
{
    return s_is_running;
}

void xiaozhi_headless_stop(void)
{
    ESP_LOGI(TAG, "HEADLESS_STOP");
    s_is_running = false;
}
