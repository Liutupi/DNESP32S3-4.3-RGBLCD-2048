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

static volatile bool s_display_released;
static volatile bool s_player_failed;
static volatile bool s_player_playing;
static int s_current_station = -1;
static char s_last_fail_reason[128] = "unknown";

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

    pump_lvgl_for_ms(hold_ms);
    lv_obj_del(box);
    lv_timer_handler();
}

static void result_touch_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) {
        menu_start();
    }
}

static void show_result_screen(const char *title, const char *detail)
{
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
    lv_obj_add_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, result_touch_cb, LV_EVENT_CLICKED, NULL);

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

static bool enter_headless_audio(const char **failed_stage)
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

    if (board_audio_codec_start(BOARD_AUDIO_VOLUME_DEFAULT) != ESP_OK) {
        *failed_stage = "ES8388 init failed";
        return false;
    }
    if (board_audio_speaker_enable(true) != ESP_OK) {
        *failed_stage = "XL9555 speaker enable failed";
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(100));

    if (!board_audio_play_beep_440hz_500ms()) {
        *failed_stage = "Beep write failed";
        return false;
    }
    return true;
}

static void restore_display(bool return_to_menu)
{
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

static radio_loop_exit_t run_headless_loop(void)
{
    bool was_pressed = false;
    bool long_fired = false;
    int64_t press_start_ms = 0;
    int failed_stations = 0;

    radio_stations_init();
    snprintf(s_last_fail_reason, sizeof(s_last_fail_reason), "%s", "unknown");
    s_current_station = first_preferred_station_index();
    if (s_current_station < 0) {
        ESP_LOGE(TAG, "RADIO_URL_FAILED reason=no_enabled_mp3_stations");
        snprintf(s_last_fail_reason, sizeof(s_last_fail_reason), "%s", "no_enabled_mp3_stations");
        return RADIO_LOOP_EXIT_NO_STATIONS;
    }
    start_station(s_current_station);

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

void radio_headless_start(void)
{
    show_message("Radio Audio Test\nChecking WiFi...", 600);

    if (!wifi_is_connected()) {
        ESP_LOGW(TAG, "WIFI_NOT_CONNECTED");
        show_message("WiFi not connected\nPlease connect WiFi in Tomato Clock first", 2000);
        return;
    }

    ESP_LOGI(TAG, "WIFI_CONNECTED_OK");
#if RADIO_AUDIO_SELF_TEST_ONLY
    show_message("WiFi OK\nTesting speaker...", 1000);
#else
    show_message("WiFi OK\nEntering Headless Radio...", 1000);
#endif

    boot_button_init();
#if RADIO_AUDIO_SELF_TEST_ONLY
    const char *failed_stage = "Unknown";
    bool beep_ok = enter_headless_audio(&failed_stage);
    stop_audio_path();
    restore_display(false);
    if (beep_ok) {
        show_result_screen("Beep OK", "Speaker path works");
    } else {
        ESP_LOGE(TAG, "BEEP_FAILED reason=%s", failed_stage);
        show_result_screen("Audio test failed", failed_stage);
    }
#else
    const char *failed_stage = "Unknown";
    radio_loop_exit_t exit_reason = RADIO_LOOP_EXIT_BOOT;
    if (enter_headless_audio(&failed_stage)) {
        failed_stage = "Unknown";
        exit_reason = run_headless_loop();
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
