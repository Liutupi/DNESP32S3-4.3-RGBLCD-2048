#include "radio_headless.h"

#include "board_audio.h"
#include "lcd.h"
#include "lvgl_demo.h"
#include "menu.h"
#include "radio_player.h"
#include "radio_stations.h"

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define TAG "RADIO_HEADLESS"
#define BOOT_GPIO GPIO_NUM_0
#define BOOT_LONG_EXIT_MS 1500
#define BOOT_POLL_MS 20
#define RADIO_MAX_FAILED_STATIONS 5
#define SOMAFM_GROOVE_SALAD_URL "https://ice5.somafm.com/groovesalad-128-mp3"
#define RADIO_VISIBLE_DIAG_MAX_STREAMS 5
#define RADIO_HEADLESS_VOLUME 33
#define RADIO_SELFTEST_TAG "SELFTEST V4"
#define RADIO_STREAM_DIAG_TAG "STREAMDIAG V4"
#define RADIO_STREAM_PLAY_TAG "STREAMPLAY V4"
#define RADIO_BOOT_STREAM_DIAG_AFTER_SELFTEST 1
#define RADIO_DIAG_SCREEN_POLL_MS 200

typedef enum {
    RADIO_BOOT_WAIT_NONE,
    RADIO_BOOT_WAIT_STREAM_DIAG,
    RADIO_BOOT_WAIT_HEADLESS_PLAY,
} radio_boot_wait_mode_t;

static volatile bool s_display_released;
static volatile bool s_player_failed;
static volatile bool s_player_playing;
static int s_current_station = -1;
static char s_last_fail_reason[128] = "unknown";
static lv_obj_t *s_diag_scr;
static lv_obj_t *s_diag_title;
static lv_obj_t *s_diag_detail;
static lv_timer_t *s_boot_diag_timer;
static lv_timer_t *s_diag_screen_timer;
static TaskHandle_t s_diag_task_handle;
static SemaphoreHandle_t s_diag_mutex;
static bool s_diag_update_pending;
static bool s_diag_done;
static bool s_diag_success;
static char s_diag_title_text[96];
static char s_diag_detail_text[256];
static bool s_boot_wait_pressed;
static bool s_boot_wait_long_fired;
static int64_t s_boot_wait_press_start_ms;
static radio_boot_wait_mode_t s_boot_wait_mode;
static bool s_verified_stream_ready;
static char s_verified_stream_name[64];
static char s_verified_stream_url[256];
static int s_verified_stream_station_index = -1;

typedef struct {
    const char *name;
    const char *url;
    int station_index;
} radio_diag_candidate_t;

typedef struct {
    const radio_diag_candidate_t *candidate;
    int index;
    int total;
} radio_diag_screen_ctx_t;

typedef enum {
    RADIO_LOOP_EXIT_BOOT,
    RADIO_LOOP_EXIT_ALL_FAILED,
    RADIO_LOOP_EXIT_NO_STATIONS,
} radio_loop_exit_t;

bool radio_headless_display_released(void)
{
    return s_display_released;
}

static bool wifi_is_connected(void)
{
    wifi_ap_record_t ap;
    return esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
}

static void pump_lvgl_for_ms(int ms)
{
    int64_t until = esp_timer_get_time() + (int64_t)ms * 1000;
    while (esp_timer_get_time() < until) {
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void show_message(const char *text, int hold_ms)
{
    ESP_LOGI(TAG, "SCREEN_MSG %s", text ? text : "");
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(box, 800, 480);
    lv_obj_set_pos(box, 0, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x120C09), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(box);
    lv_label_set_text(label, text);
    lv_obj_set_width(label, 720);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xFFF2DC), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, 0);

    lv_timer_handler();
    lv_refr_now(NULL);
    pump_lvgl_for_ms(hold_ms);
    lv_obj_del(box);
    lv_timer_handler();
    lv_refr_now(NULL);
}

static void show_result_screen(const char *title, const char *detail)
{
    ESP_LOGI(TAG, "RESULT_SCREEN title=%s detail=%s",
             title ? title : "", detail ? detail : "");
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x120C09), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_set_size(root, 800, 480);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_label = lv_label_create(root);
    lv_label_set_text(title_label, title);
    lv_obj_set_width(title_label, 720);
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0xFFF2DC), 0);
    lv_obj_set_style_text_align(title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title_label, LV_ALIGN_CENTER, 0, -42);

    lv_obj_t *detail_label = lv_label_create(root);
    lv_label_set_text(detail_label, detail ? detail : "");
    lv_obj_set_width(detail_label, 720);
    lv_obj_set_style_text_font(detail_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(detail_label, lv_color_hex(0xE5B36D), 0);
    lv_obj_set_style_text_align(detail_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(detail_label, LV_ALIGN_CENTER, 0, 24);

    lv_timer_handler();
    lv_refr_now(NULL);
}

static void ensure_diag_screen(void)
{
    if (!s_diag_scr) {
        s_diag_scr = lv_obj_create(NULL);
        lv_obj_set_style_bg_color(s_diag_scr, lv_color_hex(0x120C09), 0);
        lv_obj_set_style_bg_opa(s_diag_scr, LV_OPA_COVER, 0);
        lv_obj_clear_flag(s_diag_scr, LV_OBJ_FLAG_SCROLLABLE);

        s_diag_title = lv_label_create(s_diag_scr);
        lv_obj_set_width(s_diag_title, 720);
        lv_obj_set_style_text_font(s_diag_title, &lv_font_montserrat_28, 0);
        lv_obj_set_style_text_color(s_diag_title, lv_color_hex(0xFFF2DC), 0);
        lv_obj_set_style_text_align(s_diag_title, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(s_diag_title, LV_ALIGN_TOP_MID, 0, 58);

        s_diag_detail = lv_label_create(s_diag_scr);
        lv_obj_set_width(s_diag_detail, 720);
        lv_obj_set_style_text_font(s_diag_detail, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(s_diag_detail, lv_color_hex(0xE5B36D), 0);
        lv_obj_set_style_text_align(s_diag_detail, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(s_diag_detail, 6, 0);
        lv_obj_align(s_diag_detail, LV_ALIGN_CENTER, 0, 36);
    }
}

static void set_diag_screen_text(const char *title, const char *detail)
{
    ensure_diag_screen();
    lv_label_set_text(s_diag_title, title ? title : "");
    lv_label_set_text(s_diag_detail, detail ? detail : "");
    lv_scr_load(s_diag_scr);
}

static void show_diag_status(const char *title, const char *detail, int hold_ms)
{
    set_diag_screen_text(title, detail);
    pump_lvgl_for_ms(hold_ms);
}

static void diag_state_set(const char *title, const char *detail, bool done, bool success)
{
    if (!s_diag_mutex) {
        s_diag_mutex = xSemaphoreCreateMutex();
    }
    if (s_diag_mutex && xSemaphoreTake(s_diag_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        snprintf(s_diag_title_text, sizeof(s_diag_title_text), "%s", title ? title : "");
        snprintf(s_diag_detail_text, sizeof(s_diag_detail_text), "%s", detail ? detail : "");
        s_diag_done = done;
        s_diag_success = success;
        s_diag_update_pending = true;
        xSemaphoreGive(s_diag_mutex);
    }
}

static bool diag_state_get(char *title, size_t title_size, char *detail, size_t detail_size,
                           bool *done, bool *success)
{
    bool pending = false;
    if (s_diag_mutex && xSemaphoreTake(s_diag_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
        pending = s_diag_update_pending;
        if (pending) {
            snprintf(title, title_size, "%s", s_diag_title_text);
            snprintf(detail, detail_size, "%s", s_diag_detail_text);
            if (done) {
                *done = s_diag_done;
            }
            if (success) {
                *success = s_diag_success;
            }
            s_diag_update_pending = false;
        }
        xSemaphoreGive(s_diag_mutex);
    }
    return pending;
}

static void reset_conflict_gpios(void)
{
    gpio_reset_pin(GPIO_NUM_3);
    gpio_reset_pin(GPIO_NUM_46);
    gpio_reset_pin(GPIO_NUM_9);
    gpio_reset_pin(GPIO_NUM_10);
    gpio_reset_pin(GPIO_NUM_14);
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

static void player_status_cb(radio_player_state_t state, const char *message, void *user_ctx)
{
    (void)user_ctx;
    if (message) {
        ESP_LOGI(TAG, "%s", message);
    }
    if (state == RADIO_PLAYER_PLAYING) {
        s_player_playing = true;
        s_player_failed = false;
    } else if (state == RADIO_PLAYER_FAILED) {
        const char *reason = message ? strstr(message, "reason=") : NULL;
        reason = reason ? reason + strlen("reason=") : message;
        snprintf(s_last_fail_reason, sizeof(s_last_fail_reason), "%s",
                 (reason && reason[0]) ? reason : "unknown");
        s_player_failed = true;
        s_player_playing = false;
    } else if (state == RADIO_PLAYER_STOPPED) {
        s_player_playing = false;
    }
}

static bool start_station(int index)
{
    const radio_station_t *station = radio_stations_get(index);
    if (!station) {
        return false;
    }

    ESP_LOGI(TAG, "RADIO_STATION_SELECT index=%d/%d name=%s category=%s",
             index + 1, radio_stations_count(), station->name,
             station->category ? station->category : "");

    s_player_failed = false;
    s_player_playing = false;
    radio_player_request_t req = {
        .name = station->name,
        .url = station->url,
        .codec_hint = station->type,
        .status_cb = player_status_cb,
        .user_ctx = NULL,
    };
    int fallback_count = 0;
    for (int i = 0; i < RADIO_MAX_FALLBACK_URLS; ++i) {
        if (station->fallback_urls[i] && station->fallback_urls[i][0]) {
            req.fallback_urls[fallback_count++] = station->fallback_urls[i];
        }
    }
    req.fallback_count = fallback_count;
    if (!radio_player_play(&req)) {
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=player_start_failed station=%s", station->name);
        snprintf(s_last_fail_reason, sizeof(s_last_fail_reason), "%s", "player_start_failed");
        s_player_failed = true;
        return false;
    }
    return true;
}

static bool start_url_playback(const char *name, const char *url)
{
    if (!url || !url[0]) {
        return false;
    }
    ESP_LOGI(TAG, "RADIO_STATION_SELECT index=verified name=%s category=diag",
             name ? name : "Verified Stream");
    s_player_failed = false;
    s_player_playing = false;
    radio_player_request_t req = {
        .name = name ? name : "Verified Stream",
        .url = url,
        .codec_hint = "mp3",
        .status_cb = player_status_cb,
        .user_ctx = NULL,
    };
    if (!radio_player_play(&req)) {
        ESP_LOGW(TAG, "RADIO_URL_FAILED reason=player_start_failed station=%s",
                 name ? name : "Verified Stream");
        snprintf(s_last_fail_reason, sizeof(s_last_fail_reason), "%s", "player_start_failed");
        s_player_failed = true;
        return false;
    }
    return true;
}

static int next_station_index(void)
{
    int next = radio_stations_next_enabled_mp3(s_current_station);
    if (next >= 0) {
        s_current_station = next;
    }
    return next;
}

static int first_preferred_station_index(void)
{
    for (int i = 0; i < radio_stations_count(); ++i) {
        const radio_station_t *station = radio_stations_get(i);
        if (station && station->enabled && station->type &&
            strcmp(station->type, "mp3") == 0 && station->url &&
            strstr(station->url, SOMAFM_GROOVE_SALAD_URL)) {
            return i;
        }
    }
    return radio_stations_first_enabled_mp3();
}

static void stop_audio_path(void)
{
    radio_player_stop();
    board_audio_codec_stop();
    board_audio_speaker_enable(false);
    board_audio_i2s_stop();
}

static bool enter_headless_audio(const char **failed_stage, bool play_beep)
{
    ESP_LOGI(TAG, "HEADLESS_ENTER");

    lvgl_demo_suspend();
    ESP_LOGI(TAG, "LVGL_SUSPEND_OK");

    esp_err_t lcd_ret = lcd_deinit();
    if (lcd_ret == ESP_OK || lcd_ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "RGB_PANEL_STOP_OK");
    } else {
        ESP_LOGE(TAG, "RGB panel stop failed: %s", esp_err_to_name(lcd_ret));
        *failed_stage = "RGB panel stop failed";
        return false;
    }

    LCD_BL(0);
    ESP_LOGI(TAG, "LCD_BACKLIGHT_OFF_OK");

    reset_conflict_gpios();
    s_display_released = true;
    ESP_LOGI(TAG, "DISPLAY_RELEASED_OK");

    if (board_audio_init_i2c_and_expander() != ESP_OK) {
        *failed_stage = "I2C init failed";
        return false;
    }
    if (board_audio_speaker_enable(false) != ESP_OK) {
        *failed_stage = "XL9555 init failed";
        return false;
    }
    if (board_audio_i2s_start(BOARD_AUDIO_SAMPLE_RATE_HZ) != ESP_OK) {
        *failed_stage = "I2S init failed";
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    if (board_audio_codec_start(RADIO_HEADLESS_VOLUME) != ESP_OK) {
        *failed_stage = "ES8388 init failed";
        return false;
    }
    if (board_audio_speaker_enable(true) != ESP_OK) {
        *failed_stage = "XL9555 speaker enable failed";
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    if (play_beep) {
        ESP_LOGI(TAG, "BEEP_START pass=1");
        if (!board_audio_play_beep_440hz_500ms()) {
            *failed_stage = "Beep write failed";
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(120));
        ESP_LOGI(TAG, "BEEP_START pass=2");
        if (!board_audio_play_beep_440hz_500ms()) {
            *failed_stage = "Beep write failed";
            return false;
        }
    }
    return true;
}

static void restore_display(bool return_to_menu)
{
    if (!s_display_released) {
        LCD_BL(1);
        if (return_to_menu) {
            menu_start();
        }
        return;
    }

    reset_conflict_gpios();
    s_display_released = false;

    lcd_init();
    ESP_LOGI(TAG, "RGB_PANEL_RESTORE_OK");

    lvgl_demo_rebind_display();
    lvgl_demo_resume();
    ESP_LOGI(TAG, "LVGL_RESUME_OK");

    LCD_BL(1);
    if (return_to_menu) {
        menu_start();
    }
}

static bool same_url(const char *a, const char *b)
{
    return a && b && strcmp(a, b) == 0;
}

static int build_diag_candidates(radio_diag_candidate_t *candidates, int max_candidates)
{
    int count = 0;
    int soma_index = first_preferred_station_index();
    if (soma_index >= 0) {
        const radio_station_t *station = radio_stations_get(soma_index);
        if (station && station->url) {
            candidates[count++] = (radio_diag_candidate_t){
                .name = station->name,
                .url = station->url,
                .station_index = soma_index,
            };
        }
    } else if (max_candidates > 0) {
        candidates[count++] = (radio_diag_candidate_t){
            .name = "Groove Salad",
            .url = SOMAFM_GROOVE_SALAD_URL,
            .station_index = -1,
        };
    }

    for (int i = 0; i < radio_stations_count() && count < max_candidates; ++i) {
        const radio_station_t *station = radio_stations_get(i);
        if (!station || !station->enabled || !station->type ||
            strcmp(station->type, "mp3") != 0 || !station->url || !station->url[0]) {
            continue;
        }
        bool duplicate = false;
        for (int j = 0; j < count; ++j) {
            if (same_url(candidates[j].url, station->url)) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate) {
            candidates[count++] = (radio_diag_candidate_t){
                .name = station->name,
                .url = station->url,
                .station_index = i,
            };
        }
    }
    return count;
}

static void format_diag_detail(const radio_diag_result_t *result, char *detail, size_t detail_size)
{
    if (!result || !detail || detail_size == 0) {
        return;
    }
    if (result->playable && result->i2s_write_ok) {
        snprintf(detail, detail_size,
                 "HTTP OK\nstatus=%d\ncontent-type=%s\ndecode OK\nsample_rate=%d\nI2S_WRITE_OK",
                 result->http_status, result->content_type, result->sample_rate);
    } else if (result->decode_ok) {
        snprintf(detail, detail_size,
                 "HTTP OK\nstatus=%d\ncontent-type=%s\ndecode OK\nsample_rate=%d\nchannels=%d\ndecoded_size=%d",
                 result->http_status, result->content_type, result->sample_rate,
                 result->channels, result->decoded_size);
    } else if (result->http_open_ok) {
        snprintf(detail, detail_size, "HTTP OK\nstatus=%d\ncontent-type=%s\n%s",
                 result->http_status, result->content_type, result->reason);
    } else {
        snprintf(detail, detail_size, "%s", result->reason);
    }
}

static void visible_diag_cb(const radio_diag_result_t *result, void *user_ctx)
{
    radio_diag_screen_ctx_t *ctx = (radio_diag_screen_ctx_t *)user_ctx;
    if (!ctx || !ctx->candidate || !result) {
        return;
    }
    char title[96];
    char detail[256];
    snprintf(title, sizeof(title), "Testing %d/%d\n%s",
             ctx->index + 1, ctx->total, ctx->candidate->name);
    format_diag_detail(result, detail, sizeof(detail));
    show_diag_status(title, detail, 250);
}

static bool wait_boot_or_timeout_ms(int timeout_ms)
{
    int64_t end_ms = esp_timer_get_time() / 1000 + timeout_ms;
    while (esp_timer_get_time() / 1000 < end_ms) {
        if (gpio_get_level(BOOT_GPIO) == 0) {
            while (gpio_get_level(BOOT_GPIO) == 0) {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            return true;
        }
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

static bool wait_boot_for_stream_diag(void)
{
    bool was_pressed = false;
    bool long_fired = false;
    int64_t press_start_ms = 0;

    while (true) {
        bool pressed = gpio_get_level(BOOT_GPIO) == 0;
        int64_t now_ms = esp_timer_get_time() / 1000;

        if (pressed && !was_pressed) {
            was_pressed = true;
            long_fired = false;
            press_start_ms = now_ms;
        }
        if (pressed && was_pressed && !long_fired &&
            now_ms - press_start_ms >= BOOT_LONG_EXIT_MS) {
            ESP_LOGI(TAG, "BOOT_LONG_CANCEL_STREAM_DIAG");
            long_fired = true;
            return false;
        }
        if (!pressed && was_pressed) {
            was_pressed = false;
            if (!long_fired) {
                ESP_LOGI(TAG, "BOOT_SHORT_STREAM_DIAG");
                return true;
            }
        }

        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));
    }
}

static bool run_visible_stream_diag(radio_diag_candidate_t *verified,
                                    radio_diag_result_t *verified_result)
{
    radio_diag_candidate_t candidates[RADIO_VISIBLE_DIAG_MAX_STREAMS] = {0};
    radio_stations_init();
    int count = build_diag_candidates(candidates, RADIO_VISIBLE_DIAG_MAX_STREAMS);
    if (count <= 0) {
        snprintf(s_last_fail_reason, sizeof(s_last_fail_reason), "%s", "no_enabled_mp3_stations");
        return false;
    }

    radio_diag_result_t result = {0};
    for (int i = 0; i < count; ++i) {
        radio_diag_screen_ctx_t ctx = {
            .candidate = &candidates[i],
            .index = i,
            .total = count,
        };
        bool decode_ok = radio_player_diag_test_url(candidates[i].url, false,
                                                    visible_diag_cb, &ctx, &result);
        if (!decode_ok) {
            snprintf(s_last_fail_reason, sizeof(s_last_fail_reason), "%s", result.reason);
            char title[96];
            char detail[160];
            snprintf(title, sizeof(title), "Testing %d/%d\n%s", i + 1, count, candidates[i].name);
            snprintf(detail, sizeof(detail), "failed: %s", result.reason);
            show_diag_status(title, detail, 900);
            continue;
        }
        if (result.sample_rate != BOARD_AUDIO_SAMPLE_RATE_HZ) {
            snprintf(s_last_fail_reason, sizeof(s_last_fail_reason),
                     "unsupported sample_rate=%d", result.sample_rate);
            char title[96];
            char detail[160];
            snprintf(title, sizeof(title), "Testing %d/%d\n%s", i + 1, count, candidates[i].name);
            snprintf(detail, sizeof(detail), "unsupported sample_rate=%d", result.sample_rate);
            show_diag_status(title, detail, 900);
            continue;
        }

        char title[96];
        char detail[256];
        snprintf(title, sizeof(title), "Testing %d/%d\n%s", i + 1, count, candidates[i].name);
        format_diag_detail(&result, detail, sizeof(detail));
        show_diag_status(title, detail, 1200);
        *verified = candidates[i];
        if (verified_result) {
            *verified_result = result;
        }
        return true;
    }
    return false;
}

static void diag_task_cb(const radio_diag_result_t *result, void *user_ctx)
{
    radio_diag_screen_ctx_t *ctx = (radio_diag_screen_ctx_t *)user_ctx;
    if (!ctx || !ctx->candidate || !result) {
        return;
    }
    char title[96];
    char detail[256];
    snprintf(title, sizeof(title), "Testing %d/%d\n%s",
             ctx->index + 1, ctx->total, ctx->candidate->name);
    format_diag_detail(result, detail, sizeof(detail));
    diag_state_set(title, detail, false, false);
}

static bool run_stream_diag_tasksafe(radio_diag_candidate_t *verified,
                                     radio_diag_result_t *verified_result)
{
    radio_diag_candidate_t candidates[RADIO_VISIBLE_DIAG_MAX_STREAMS] = {0};
    radio_stations_init();
    int count = build_diag_candidates(candidates, RADIO_VISIBLE_DIAG_MAX_STREAMS);
    if (count <= 0) {
        snprintf(s_last_fail_reason, sizeof(s_last_fail_reason), "%s", "no_enabled_mp3_stations");
        diag_state_set("Radio failed\n" RADIO_STREAM_DIAG_TAG,
                       "No enabled MP3 stations", true, false);
        return false;
    }

    radio_diag_result_t result = {0};
    for (int i = 0; i < count; ++i) {
        radio_diag_screen_ctx_t ctx = {
            .candidate = &candidates[i],
            .index = i,
            .total = count,
        };
        char title[96];
        char detail[160];
        snprintf(title, sizeof(title), "Testing %d/%d\n%s",
                 i + 1, count, candidates[i].name);
        snprintf(detail, sizeof(detail), "Opening stream...\n%s", candidates[i].url);
        diag_state_set(title, detail, false, false);

        bool decode_ok = radio_player_diag_test_url(candidates[i].url, false,
                                                    diag_task_cb, &ctx, &result);
        if (!decode_ok) {
            snprintf(s_last_fail_reason, sizeof(s_last_fail_reason), "%s", result.reason);
            snprintf(detail, sizeof(detail), "failed: %s", result.reason);
            diag_state_set(title, detail, false, false);
            vTaskDelay(pdMS_TO_TICKS(900));
            continue;
        }
        if (result.sample_rate != BOARD_AUDIO_SAMPLE_RATE_HZ) {
            snprintf(s_last_fail_reason, sizeof(s_last_fail_reason),
                     "unsupported sample_rate=%d", result.sample_rate);
            snprintf(detail, sizeof(detail), "unsupported sample_rate=%d", result.sample_rate);
            diag_state_set(title, detail, false, false);
            vTaskDelay(pdMS_TO_TICKS(900));
            continue;
        }

        if (verified) {
            *verified = candidates[i];
        }
        if (verified_result) {
            *verified_result = result;
        }
        snprintf(s_verified_stream_name, sizeof(s_verified_stream_name), "%s",
                 candidates[i].name ? candidates[i].name : "Verified Stream");
        snprintf(s_verified_stream_url, sizeof(s_verified_stream_url), "%s",
                 candidates[i].url ? candidates[i].url : "");
        s_verified_stream_station_index = candidates[i].station_index;
        s_verified_stream_ready = s_verified_stream_url[0] != '\0';

        snprintf(detail, sizeof(detail),
                 "%s\nHTTP OK status=%d\nsample_rate=%d\nShort BOOT: headless play",
                 candidates[i].name, result.http_status, result.sample_rate);
        diag_state_set("Radio stream OK\n" RADIO_STREAM_DIAG_TAG, detail, true, true);
        return true;
    }

    char detail[176];
    snprintf(detail, sizeof(detail), "Last reason: %s\nTried %d streams",
             s_last_fail_reason, count);
    diag_state_set("Radio failed\n" RADIO_STREAM_DIAG_TAG, detail, true, false);
    return false;
}

static void start_boot_headless_play_wait(void);
static void start_verified_headless_play(void);

static void diag_screen_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    char title[96] = {0};
    char detail[256] = {0};
    bool done = false;
    bool success = false;
    if (!diag_state_get(title, sizeof(title), detail, sizeof(detail), &done, &success)) {
        return;
    }
    (void)success;
    set_diag_screen_text(title, detail);
    if (done && s_diag_screen_timer) {
        lv_timer_del(s_diag_screen_timer);
        s_diag_screen_timer = NULL;
    }
    if (done && success && s_verified_stream_ready) {
        start_boot_headless_play_wait();
    }
}

static void stream_diag_task(void *param)
{
    (void)param;
    ESP_LOGI(TAG, "STREAM_DIAG_TASK_START");
    diag_state_set("Testing radio stream\n" RADIO_STREAM_DIAG_TAG,
                   "Starting HTTP/MP3 probe...", false, false);
    radio_diag_candidate_t verified = {0};
    radio_diag_result_t result = {0};
    run_stream_diag_tasksafe(&verified, &result);
    ESP_LOGI(TAG, "STREAM_DIAG_TASK_DONE success=%d reason=%s",
             result.playable, result.reason);
    s_diag_task_handle = NULL;
    vTaskDelete(NULL);
}

static void start_stream_diag_task(void)
{
    if (s_diag_task_handle) {
        return;
    }
    s_verified_stream_ready = false;
    s_verified_stream_name[0] = '\0';
    s_verified_stream_url[0] = '\0';
    s_verified_stream_station_index = -1;
    if (!s_diag_screen_timer) {
        s_diag_screen_timer = lv_timer_create(diag_screen_timer_cb,
                                              RADIO_DIAG_SCREEN_POLL_MS, NULL);
    }
    diag_state_set("Testing radio stream\n" RADIO_STREAM_DIAG_TAG,
                   "Starting task...", false, false);
    if (xTaskCreate(stream_diag_task, "radio_stream_diag", 20480, NULL, 4,
                    &s_diag_task_handle) != pdPASS) {
        s_diag_task_handle = NULL;
        diag_state_set("Radio failed\n" RADIO_STREAM_DIAG_TAG,
                       "Failed to create diag task", true, false);
    }
}

static void boot_diag_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    bool pressed = gpio_get_level(BOOT_GPIO) == 0;
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (pressed && !s_boot_wait_pressed) {
        s_boot_wait_pressed = true;
        s_boot_wait_long_fired = false;
        s_boot_wait_press_start_ms = now_ms;
    }
    if (pressed && s_boot_wait_pressed && !s_boot_wait_long_fired &&
        now_ms - s_boot_wait_press_start_ms >= BOOT_LONG_EXIT_MS) {
        s_boot_wait_long_fired = true;
        if (s_boot_wait_mode == RADIO_BOOT_WAIT_HEADLESS_PLAY) {
            ESP_LOGI(TAG, "BOOT_LONG_CANCEL_HEADLESS_PLAY");
            set_diag_screen_text("Radio stream OK\n" RADIO_STREAM_DIAG_TAG,
                                 "Headless play not started\nShort BOOT: enter headless play");
        } else {
            ESP_LOGI(TAG, "BOOT_LONG_CANCEL_STREAM_DIAG");
            set_diag_screen_text("Beep OK\n" RADIO_SELFTEST_TAG,
                                 "Speaker path works\nStream test not started");
        }
    }
    if (!pressed && s_boot_wait_pressed) {
        s_boot_wait_pressed = false;
        if (!s_boot_wait_long_fired) {
            radio_boot_wait_mode_t mode = s_boot_wait_mode;
            if (s_boot_diag_timer) {
                lv_timer_del(s_boot_diag_timer);
                s_boot_diag_timer = NULL;
            }
            s_boot_wait_mode = RADIO_BOOT_WAIT_NONE;
            if (mode == RADIO_BOOT_WAIT_HEADLESS_PLAY) {
                ESP_LOGI(TAG, "BOOT_SHORT_HEADLESS_PLAY");
                start_verified_headless_play();
            } else {
                ESP_LOGI(TAG, "BOOT_SHORT_STREAM_DIAG");
                set_diag_screen_text("Testing radio stream\n" RADIO_STREAM_DIAG_TAG,
                                     "Starting HTTP/MP3 probe...");
                start_stream_diag_task();
            }
        }
    }
}

static void start_boot_wait(radio_boot_wait_mode_t mode)
{
    boot_button_init();
    s_boot_wait_pressed = false;
    s_boot_wait_long_fired = false;
    s_boot_wait_press_start_ms = 0;
    s_boot_wait_mode = mode;
    if (s_boot_diag_timer) {
        lv_timer_del(s_boot_diag_timer);
        s_boot_diag_timer = NULL;
    }
    s_boot_diag_timer = lv_timer_create(boot_diag_timer_cb, BOOT_POLL_MS, NULL);
}

static void start_boot_stream_diag_wait(void)
{
    start_boot_wait(RADIO_BOOT_WAIT_STREAM_DIAG);
}

static void start_boot_headless_play_wait(void)
{
    start_boot_wait(RADIO_BOOT_WAIT_HEADLESS_PLAY);
}

static radio_loop_exit_t run_headless_loop(const char *initial_name,
                                           const char *initial_url,
                                           int initial_station_index)
{
    bool was_pressed = false;
    bool long_fired = false;
    int64_t press_start_ms = 0;
    int failed_stations = 0;

    snprintf(s_last_fail_reason, sizeof(s_last_fail_reason), "%s", "unknown");
    if (initial_url && initial_url[0]) {
        s_current_station = initial_station_index >= 0 ? initial_station_index : first_preferred_station_index();
        start_url_playback(initial_name, initial_url);
    } else {
        radio_stations_init();
        s_current_station = first_preferred_station_index();
        if (s_current_station >= 0) {
            start_station(s_current_station);
        }
    }
    if (s_current_station < 0) {
        ESP_LOGE(TAG, "RADIO_URL_FAILED reason=no_enabled_mp3_stations");
        snprintf(s_last_fail_reason, sizeof(s_last_fail_reason), "%s", "no_enabled_mp3_stations");
        return RADIO_LOOP_EXIT_NO_STATIONS;
    }

    while (true) {
        bool pressed = gpio_get_level(BOOT_GPIO) == 0;
        int64_t now_ms = esp_timer_get_time() / 1000;

        if (pressed && !was_pressed) {
            was_pressed = true;
            long_fired = false;
            press_start_ms = now_ms;
        }
        if (pressed && was_pressed && !long_fired &&
            now_ms - press_start_ms >= BOOT_LONG_EXIT_MS) {
            ESP_LOGI(TAG, "BOOT_LONG_EXIT");
            long_fired = true;
            break;
        }
        if (!pressed && was_pressed) {
            was_pressed = false;
            if (!long_fired) {
                ESP_LOGI(TAG, "BOOT_SHORT_NEXT");
                radio_player_stop();
                if (radio_player_is_running()) {
                    snprintf(s_last_fail_reason, sizeof(s_last_fail_reason),
                             "%s", "player_stop_timeout");
                    ESP_LOGW(TAG, "BOOT_SHORT_NEXT ignored: player_stop_timeout");
                    vTaskDelay(pdMS_TO_TICKS(300));
                    continue;
                }
                int next = next_station_index();
                if (next >= 0) {
                    start_station(next);
                }
            }
        }

        if (s_player_failed && !radio_player_is_running()) {
            s_player_failed = false;
            failed_stations++;
            ESP_LOGI(TAG, "RADIO_AUTO_NEXT tried=%d", failed_stations);
            if (failed_stations >= RADIO_MAX_FAILED_STATIONS) {
                ESP_LOGE(TAG, "RADIO_ALL_FAILED tried=%d last_reason=%s",
                         failed_stations, s_last_fail_reason);
                return RADIO_LOOP_EXIT_ALL_FAILED;
            }
            int next = next_station_index();
            if (next >= 0) {
                start_station(next);
            } else {
                vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));
    }

    return RADIO_LOOP_EXIT_BOOT;
}

static void start_verified_headless_play(void)
{
    if (!s_verified_stream_ready || !s_verified_stream_url[0]) {
        set_diag_screen_text("Radio failed\n" RADIO_STREAM_PLAY_TAG,
                             "No verified stream URL");
        return;
    }

    char detail[192];
    snprintf(detail, sizeof(detail), "%s\nEntering headless play...",
             s_verified_stream_name[0] ? s_verified_stream_name : "Verified Stream");
    set_diag_screen_text("Entering Headless Radio\n" RADIO_STREAM_PLAY_TAG, detail);
    pump_lvgl_for_ms(900);

    const char *failed_stage = "Unknown";
    radio_loop_exit_t exit_reason = RADIO_LOOP_EXIT_BOOT;
    if (enter_headless_audio(&failed_stage, true)) {
        failed_stage = "Unknown";
        exit_reason = run_headless_loop(s_verified_stream_name,
                                        s_verified_stream_url,
                                        s_verified_stream_station_index);
    } else {
        ESP_LOGE(TAG, "RADIO_URL_FAILED reason=%s", failed_stage);
    }

    stop_audio_path();
    if (failed_stage && failed_stage[0] && strcmp(failed_stage, "Unknown") != 0) {
        restore_display(false);
        show_result_screen("Audio test failed", failed_stage);
    } else if (exit_reason == RADIO_LOOP_EXIT_ALL_FAILED) {
        char failed_detail[176];
        snprintf(failed_detail, sizeof(failed_detail), "Last reason: %s", s_last_fail_reason);
        restore_display(false);
        show_result_screen("Radio failed", failed_detail);
    } else if (exit_reason == RADIO_LOOP_EXIT_NO_STATIONS) {
        restore_display(false);
        show_result_screen("Radio failed", "No enabled MP3 stations");
    } else {
        restore_display(true);
    }
}

void radio_headless_start(void)
{
    ESP_LOGI(TAG, "RADIO_SELF_TEST_TAG=%s mode=%d visible_diag=%d",
             RADIO_SELFTEST_TAG, RADIO_AUDIO_SELF_TEST_ONLY, RADIO_VISIBLE_STREAM_DIAG);
    show_message("Radio Audio Test\n" RADIO_SELFTEST_TAG "\nChecking WiFi...", 1800);

    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "WIFI_NOT_CONNECTED");
        show_message("WiFi not connected\nPlease connect WiFi in Tomato Clock first", 2000);
        return;
    }

    ESP_LOGI(TAG, "WIFI_CONNECTED_OK");
    show_message("WiFi OK\n" RADIO_SELFTEST_TAG "\nTesting speaker...", 1800);

    boot_button_init();
#if RADIO_AUDIO_SELF_TEST_ONLY
    const char *failed_stage = "Unknown";
    bool beep_ok = enter_headless_audio(&failed_stage, true);
    stop_audio_path();
    restore_display(false);
    if (beep_ok) {
#if RADIO_BOOT_STREAM_DIAG_AFTER_SELFTEST
        show_diag_status("Beep OK\n" RADIO_SELFTEST_TAG,
                         "Speaker path works\nShort BOOT: test stream\nLong BOOT: stay here",
                         100);
        start_boot_stream_diag_wait();
#else
        show_result_screen("Beep OK", RADIO_SELFTEST_TAG "\nSpeaker path works");
#endif
    } else {
        ESP_LOGE(TAG, "BEEP_FAILED reason=%s", failed_stage);
        show_result_screen("Audio test failed", failed_stage);
    }
#else
    const char *failed_stage = "Unknown";
    radio_loop_exit_t exit_reason = RADIO_LOOP_EXIT_BOOT;
    if (enter_headless_audio(&failed_stage, true)) {
        failed_stage = "Unknown";
        stop_audio_path();
        restore_display(false);
#if RADIO_VISIBLE_STREAM_DIAG
        show_diag_status("Beep OK", "Speaker path works", 1200);
#if RADIO_REQUIRE_BOOT_FOR_STREAM_DIAG
        show_diag_status("Beep OK", "Press BOOT to test stream\nHold BOOT to cancel", 100);
        if (!wait_boot_for_stream_diag()) {
            show_result_screen("Beep OK", "Speaker path works\nStream test not started");
            return;
        }
#endif
        show_diag_status("Beep OK", "Testing radio stream...", 800);
        radio_diag_candidate_t verified = {0};
        radio_diag_result_t diag_result = {0};
        if (run_visible_stream_diag(&verified, &diag_result)) {
            show_diag_status("Radio stream OK", "Press BOOT to enter headless play", 100);
            wait_boot_or_timeout_ms(3000);
            if (enter_headless_audio(&failed_stage, false)) {
                failed_stage = "Unknown";
                exit_reason = run_headless_loop(verified.name, verified.url, verified.station_index);
            } else {
                ESP_LOGE(TAG, "RADIO_URL_FAILED reason=%s", failed_stage);
            }
        } else {
            exit_reason = RADIO_LOOP_EXIT_ALL_FAILED;
        }
#else
        if (enter_headless_audio(&failed_stage, false)) {
            failed_stage = "Unknown";
            exit_reason = run_headless_loop(NULL, NULL, -1);
        }
#endif
    } else {
        ESP_LOGE(TAG, "BEEP_FAILED reason=%s", failed_stage);
    }

    stop_audio_path();
    if (failed_stage && failed_stage[0] && strcmp(failed_stage, "Unknown") != 0) {
        restore_display(false);
        show_result_screen("Audio test failed", failed_stage);
    } else if (exit_reason == RADIO_LOOP_EXIT_ALL_FAILED) {
        char detail[176];
        snprintf(detail, sizeof(detail), "Last reason: %s", s_last_fail_reason);
        restore_display(false);
        show_result_screen("Radio failed", detail);
    } else if (exit_reason == RADIO_LOOP_EXIT_NO_STATIONS) {
        restore_display(false);
        show_result_screen("Radio failed", "No enabled MP3 stations");
    } else {
        restore_display(true);
    }
#endif
}
