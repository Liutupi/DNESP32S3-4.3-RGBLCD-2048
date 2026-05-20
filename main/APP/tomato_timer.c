/**
 ****************************************************************************************************
 * @file        tomato_timer.c
 * @brief       Tomato Glow Clock for DNESP32S3 800x480 RGB LCD
 ****************************************************************************************************
 */

#include "tomato_timer.h"
#include "menu.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "psa/crypto.h"
#if __has_include("qweather_jwt_secret.h")
#include "qweather_jwt_secret.h"
#define TOMATO_QWEATHER_JWT_SECRET_AVAILABLE 1
#else
#define TOMATO_QWEATHER_JWT_SECRET_AVAILABLE 0
#define TOMATO_QWEATHER_JWT_CREDENTIAL_ID ""
#define TOMATO_QWEATHER_JWT_PROJECT_ID ""
static const unsigned char TOMATO_QWEATHER_ED25519_SEED[32] = {0};
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef TOMATO_WIFI_SSID
#ifdef CONFIG_TOMATO_WIFI_SSID
#define TOMATO_WIFI_SSID CONFIG_TOMATO_WIFI_SSID
#else
#define TOMATO_WIFI_SSID ""
#endif
#endif

#ifndef TOMATO_WIFI_PASS
#ifdef CONFIG_TOMATO_WIFI_PASS
#define TOMATO_WIFI_PASS CONFIG_TOMATO_WIFI_PASS
#else
#define TOMATO_WIFI_PASS ""
#endif
#endif

#ifndef TOMATO_QWEATHER_API_KEY
#ifdef CONFIG_TOMATO_QWEATHER_API_KEY
#define TOMATO_QWEATHER_API_KEY CONFIG_TOMATO_QWEATHER_API_KEY
#else
#define TOMATO_QWEATHER_API_KEY ""
#endif
#endif

#ifndef TOMATO_QWEATHER_LOCATION
#ifdef CONFIG_TOMATO_QWEATHER_LOCATION
#define TOMATO_QWEATHER_LOCATION CONFIG_TOMATO_QWEATHER_LOCATION
#else
#define TOMATO_QWEATHER_LOCATION "113.53,22.49"
#endif
#endif

#define TAG "TomatoGlow"

#define SCREEN_W 800
#define SCREEN_H 480

#define DEFAULT_WORK_MIN        25
#define DEFAULT_SHORT_BREAK_MIN 5
#define DEFAULT_LONG_BREAK_MIN  15
#define DEFAULT_ROUNDS          4
#define DEFAULT_WEATHER_MIN     30

#define WEATHER_BUF_SIZE 1536
#define WIFI_CONNECTED_BIT BIT0
#define QWEATHER_JWT_TTL_S (60 * 60)
#define MIN_VALID_TIME_EPOCH 1704067200

#define C_BG_DARK       0x2B140D
#define C_BG_WARM       0x6B2C18
#define C_BG_GLOW       0xB85B2A
#define C_CARD_DARK     0x36130C
#define C_CARD_SOFT     0x5B2415
#define C_HIGHLIGHT     0xFFB35C
#define C_HIGHLIGHT_2   0xFFE0A8
#define C_TEXT          0xFFF3D7
#define C_TEXT_DIM      0xFFD19A
#define C_MUTED         0x9C6044
#define C_BUTTON_DARK   0x512012

typedef enum {
    TIMER_IDLE,
    TIMER_RUNNING,
    TIMER_PAUSED
} timer_state_t;

typedef enum {
    MODE_FOCUS,
    MODE_SHORT_BREAK,
    MODE_LONG_BREAK
} timer_mode_t;

typedef enum {
    PAGE_MAIN,
    PAGE_SETTINGS,
    PAGE_DONE,
    PAGE_WIFI
} page_t;

typedef struct {
    char ssid[33];
    int8_t rssi;
    wifi_auth_mode_t authmode;
} wifi_ap_item_t;

typedef struct {
    char city[32];
    char temp[8];
    char text[32];
    char humidity[8];
    char status[48];
    bool online;
} weather_info_t;

typedef struct {
    lv_obj_t *btn;
    lv_obj_t *label;
} glow_button_t;

static lv_obj_t *g_scr;
static lv_obj_t *g_time_label;
static lv_obj_t *g_date_label;
static lv_obj_t *g_timer_arc;
static lv_obj_t *g_mode_label;
static lv_obj_t *g_countdown_label;
static lv_obj_t *g_tomato_label;
static lv_obj_t *g_weather_city_label;
static lv_obj_t *g_weather_temp_label;
static lv_obj_t *g_weather_detail_label;
static lv_obj_t *g_weather_status_label;
static lv_obj_t *g_rhythm_label;
static lv_obj_t *g_round_dots[DEFAULT_ROUNDS];
static lv_obj_t *g_start_label;
static lv_obj_t *g_pause_label;
static lv_obj_t *g_page_title;
static lv_obj_t *g_settings_list;
static lv_obj_t *g_wifi_status_label;
static lv_obj_t *g_wifi_list;
static lv_obj_t *g_wifi_ssid_ta;
static lv_obj_t *g_wifi_password_ta;
static lv_obj_t *g_wifi_keyboard;

static lv_timer_t *g_tick_timer;
static EventGroupHandle_t g_wifi_events;
static SemaphoreHandle_t g_weather_mutex;

static timer_state_t g_state = TIMER_IDLE;
static timer_mode_t g_mode = MODE_FOCUS;
static page_t g_page = PAGE_MAIN;
static int g_focus_min = DEFAULT_WORK_MIN;
static int g_short_break_min = DEFAULT_SHORT_BREAK_MIN;
static int g_long_break_min = DEFAULT_LONG_BREAK_MIN;
static int g_rounds = DEFAULT_ROUNDS;
static int g_weather_refresh_min = DEFAULT_WEATHER_MIN;
static int g_brightness_pct = 70;
static int g_completed_focus = 0;
static int g_remaining_s = DEFAULT_WORK_MIN * 60;
static int g_total_s = DEFAULT_WORK_MIN * 60;
static bool g_net_started;
static bool g_sntp_started;
static bool g_weather_task_started;
static volatile bool g_weather_dirty;
static volatile bool g_wifi_scan_dirty;
static volatile bool g_wifi_scan_busy;
static char g_wifi_ssid[33];
static char g_wifi_pass[65];
static char g_selected_ssid[33];
static wifi_ap_item_t g_wifi_scan_results[8];
static int g_wifi_scan_count;
static weather_info_t g_weather = {
    .city = "Zhongshan Nanlang",
    .temp = "26",
    .text = "Cloudy",
    .humidity = "68",
    .status = "Local sample",
    .online = false,
};

static const char *const g_weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static void show_main_page(void);
static void show_settings_page(void);
static void show_done_page(void);
static void show_wifi_page(void);
static void set_label(lv_obj_t *obj, const char *text);
static void weather_set_local_status(const char *status);
static void start_wifi_once(void);
static void start_wifi_scan(void);
static void save_and_connect_wifi(const char *ssid, const char *pass);
static void update_wifi_scan_list(void);

static bool has_text(const char *s)
{
    return s && s[0] != '\0';
}

static const char *wifi_auth_name(wifi_auth_mode_t authmode)
{
    if (authmode == WIFI_AUTH_OPEN) return "Open";
    if (authmode == WIFI_AUTH_WEP) return "WEP";
    if (authmode == WIFI_AUTH_WPA_PSK) return "WPA";
    if (authmode == WIFI_AUTH_WPA2_PSK) return "WPA2";
    if (authmode == WIFI_AUTH_WPA_WPA2_PSK) return "WPA/WPA2";
    if (authmode == WIFI_AUTH_WPA3_PSK) return "WPA3";
    if (authmode == WIFI_AUTH_WPA2_WPA3_PSK) return "WPA2/WPA3";
    return "Secure";
}

static void wifi_set_status(const char *status)
{
    set_label(g_wifi_status_label, status);
    weather_set_local_status(status);
}

static void clear_wifi_page_refs(void)
{
    g_wifi_status_label = NULL;
    g_wifi_list = NULL;
    g_wifi_ssid_ta = NULL;
    g_wifi_password_ta = NULL;
    g_wifi_keyboard = NULL;
}

static void clear_page_refs(void)
{
    g_time_label = NULL;
    g_date_label = NULL;
    g_timer_arc = NULL;
    g_mode_label = NULL;
    g_countdown_label = NULL;
    g_tomato_label = NULL;
    g_weather_city_label = NULL;
    g_weather_temp_label = NULL;
    g_weather_detail_label = NULL;
    g_weather_status_label = NULL;
    g_rhythm_label = NULL;
    for (int i = 0; i < DEFAULT_ROUNDS; ++i) {
        g_round_dots[i] = NULL;
    }
    g_start_label = NULL;
    g_pause_label = NULL;
    g_page_title = NULL;
    g_settings_list = NULL;
    clear_wifi_page_refs();
}

static int current_duration_for_mode(timer_mode_t mode)
{
    if (mode == MODE_SHORT_BREAK) return g_short_break_min * 60;
    if (mode == MODE_LONG_BREAK) return g_long_break_min * 60;
    return g_focus_min * 60;
}

static const char *mode_title(void)
{
    if (g_mode == MODE_SHORT_BREAK) return "Short Break";
    if (g_mode == MODE_LONG_BREAK) return "Long Break";
    return "Focus Mode";
}

static void reset_mode(timer_mode_t mode)
{
    g_mode = mode;
    g_total_s = current_duration_for_mode(mode);
    g_remaining_s = g_total_s;
    g_state = TIMER_IDLE;
}

static void set_label(lv_obj_t *obj, const char *text)
{
    if (obj) lv_label_set_text(obj, text);
}

static void style_panel(lv_obj_t *obj, uint32_t color, lv_opa_t opa, lv_coord_t radius)
{
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, opa, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(C_HIGHLIGHT_2), 0);
    lv_obj_set_style_border_opa(obj, LV_OPA_20, 0);
    lv_obj_set_style_border_width(obj, 1, 0);
    lv_obj_set_style_shadow_width(obj, 14, 0);
    lv_obj_set_style_shadow_opa(obj, LV_OPA_20, 0);
    lv_obj_set_style_shadow_color(obj, lv_color_hex(0x120503), 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            uint32_t color, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    lv_obj_set_pos(label, x, y);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

static void button_event_proxy(lv_event_t *e)
{
    lv_event_cb_t cb = (lv_event_cb_t)lv_event_get_user_data(e);
    if (cb) cb(e);
}

static glow_button_t make_button(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y,
                                 lv_coord_t w, lv_coord_t h, uint32_t color, uint32_t text_color,
                                 lv_event_cb_t cb)
{
    glow_button_t b;
    b.btn = lv_btn_create(parent);
    lv_obj_set_size(b.btn, w, h);
    lv_obj_set_pos(b.btn, x, y);
    lv_obj_set_style_bg_color(b.btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(b.btn, color == C_HIGHLIGHT ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_set_style_radius(b.btn, h / 2, 0);
    lv_obj_set_style_border_width(b.btn, 1, 0);
    lv_obj_set_style_border_color(b.btn, lv_color_hex(C_HIGHLIGHT_2), 0);
    lv_obj_set_style_border_opa(b.btn, LV_OPA_30, 0);
    lv_obj_set_style_shadow_width(b.btn, 0, 0);
    lv_obj_clear_flag(b.btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(b.btn, button_event_proxy, LV_EVENT_CLICKED, (void *)cb);

    b.label = lv_label_create(b.btn);
    lv_label_set_text(b.label, text);
    lv_obj_set_style_text_font(b.label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(b.label, lv_color_hex(text_color), 0);
    lv_obj_center(b.label);
    lv_obj_clear_flag(b.label, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b.label, button_event_proxy, LV_EVENT_CLICKED, (void *)cb);
    return b;
}

static void update_clock_labels(void)
{
    time_t now = time(NULL);
    struct tm tm_now;
    char buf[32];

    localtime_r(&now, &tm_now);
    if (tm_now.tm_year < 124) {
        set_label(g_time_label, "--:--");
        set_label(g_date_label, "Time syncing");
        return;
    }

    strftime(buf, sizeof(buf), "%H:%M", &tm_now);
    set_label(g_time_label, buf);
    snprintf(buf, sizeof(buf), "%02d/%02d %s", tm_now.tm_mon + 1, tm_now.tm_mday,
             g_weekdays[tm_now.tm_wday]);
    set_label(g_date_label, buf);
}

static void update_weather_labels(void)
{
    weather_info_t copy;
    if (!g_weather_mutex) return;
    if (g_page != PAGE_MAIN || !g_weather_city_label || !g_weather_temp_label ||
        !g_weather_detail_label || !g_weather_status_label) {
        return;
    }

    if (xSemaphoreTake(g_weather_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        copy = g_weather;
        g_weather_dirty = false;
        xSemaphoreGive(g_weather_mutex);
    } else {
        return;
    }

    char temp_buf[20];
    char detail_buf[64];
    snprintf(temp_buf, sizeof(temp_buf), "%s deg", copy.temp);
    snprintf(detail_buf, sizeof(detail_buf), "%s / Humidity %s%%", copy.text, copy.humidity);
    set_label(g_weather_city_label, copy.city);
    set_label(g_weather_temp_label, temp_buf);
    set_label(g_weather_detail_label, detail_buf);
    set_label(g_weather_status_label, copy.status);
}

static void update_round_dots(void)
{
    for (int i = 0; i < DEFAULT_ROUNDS; ++i) {
        uint32_t c = (i < g_completed_focus) ? C_HIGHLIGHT : C_MUTED;
        lv_opa_t opa = (i < g_rounds) ? LV_OPA_COVER : LV_OPA_20;
        if (g_round_dots[i]) {
            lv_obj_set_style_bg_color(g_round_dots[i], lv_color_hex(c), 0);
            lv_obj_set_style_bg_opa(g_round_dots[i], opa, 0);
        }
    }
}

static void update_timer_labels(void)
{
    char buf[48];
    int minutes = g_remaining_s / 60;
    int seconds = g_remaining_s % 60;
    int progress = 0;

    if (g_total_s > 0) {
        progress = (g_remaining_s * 1000) / g_total_s;
    }

    snprintf(buf, sizeof(buf), "%02d:%02d", minutes, seconds);
    set_label(g_countdown_label, buf);
    set_label(g_mode_label, mode_title());

    snprintf(buf, sizeof(buf), "Tomato %d / %d", g_completed_focus + 1, g_rounds);
    if (g_mode != MODE_FOCUS) {
        snprintf(buf, sizeof(buf), "Break after %d / %d", g_completed_focus, g_rounds);
    }
    set_label(g_tomato_label, buf);

    snprintf(buf, sizeof(buf), "%d min focus + %d min break", g_focus_min, g_short_break_min);
    set_label(g_rhythm_label, buf);

    if (g_timer_arc) lv_arc_set_value(g_timer_arc, progress);
    set_label(g_start_label, g_state == TIMER_RUNNING ? "Running" : "Start");
    set_label(g_pause_label, g_state == TIMER_PAUSED ? "Resume" : "Pause");
    update_round_dots();
}

static void change_to_next_mode_after_completion(void)
{
    if (g_mode == MODE_FOCUS) {
        g_completed_focus++;
        if (g_completed_focus >= g_rounds) {
            g_completed_focus = 0;
            reset_mode(MODE_LONG_BREAK);
        } else {
            reset_mode(MODE_SHORT_BREAK);
        }
        show_done_page();
        return;
    }

    reset_mode(MODE_FOCUS);
    show_main_page();
}

static void tick_cb(lv_timer_t *timer)
{
    (void)timer;

    update_clock_labels();
    if (g_page == PAGE_MAIN && g_weather_dirty) update_weather_labels();
    if (g_page == PAGE_WIFI && g_wifi_scan_dirty) update_wifi_scan_list();

    if (g_state == TIMER_RUNNING) {
        if (g_remaining_s > 0) {
            g_remaining_s--;
        }
        if (g_remaining_s <= 0) {
            g_state = TIMER_IDLE;
            change_to_next_mode_after_completion();
            return;
        }
        if (g_page == PAGE_MAIN) update_timer_labels();
    }
}

static void on_start(lv_event_t *e)
{
    (void)e;
    if (g_page == PAGE_DONE) {
        show_main_page();
    }
    g_state = TIMER_RUNNING;
    show_main_page();
}

static void on_pause(lv_event_t *e)
{
    (void)e;
    if (g_state == TIMER_RUNNING) {
        g_state = TIMER_PAUSED;
    } else if (g_state == TIMER_PAUSED) {
        g_state = TIMER_RUNNING;
    }
    update_timer_labels();
}

static void on_skip(lv_event_t *e)
{
    (void)e;
    g_state = TIMER_IDLE;
    change_to_next_mode_after_completion();
}

static void on_settings(lv_event_t *e)
{
    (void)e;
    show_settings_page();
}

static void on_wifi_settings(lv_event_t *e)
{
    (void)e;
    show_wifi_page();
}

static void on_back(lv_event_t *e)
{
    (void)e;
    if (g_tick_timer) {
        lv_timer_del(g_tick_timer);
        g_tick_timer = NULL;
    }
    menu_go_back();
}

static void on_main(lv_event_t *e)
{
    (void)e;
    show_main_page();
}

static void on_wifi_scan(lv_event_t *e)
{
    (void)e;
    start_wifi_scan();
}

static void on_wifi_textarea_focus(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    if (!g_wifi_keyboard && g_scr) {
        g_wifi_keyboard = lv_keyboard_create(g_scr);
        lv_obj_set_size(g_wifi_keyboard, 800, 168);
        lv_obj_set_pos(g_wifi_keyboard, 0, 312);
        lv_obj_set_style_bg_color(g_wifi_keyboard, lv_color_hex(C_CARD_DARK), 0);
        lv_obj_set_style_bg_opa(g_wifi_keyboard, LV_OPA_COVER, 0);
    }
    if (!g_wifi_keyboard) return;
    lv_keyboard_set_textarea(g_wifi_keyboard, ta);
    lv_obj_clear_flag(g_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
}

static void on_wifi_ap_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= g_wifi_scan_count) return;
    strncpy(g_selected_ssid, g_wifi_scan_results[idx].ssid, sizeof(g_selected_ssid) - 1);
    g_selected_ssid[sizeof(g_selected_ssid) - 1] = '\0';
    if (g_wifi_ssid_ta) lv_textarea_set_text(g_wifi_ssid_ta, g_selected_ssid);
    if (g_wifi_password_ta) lv_textarea_set_text(g_wifi_password_ta, "");
    wifi_set_status("SSID selected");
}

static void on_wifi_connect(lv_event_t *e)
{
    (void)e;
    const char *ssid = g_wifi_ssid_ta ? lv_textarea_get_text(g_wifi_ssid_ta) : "";
    const char *pass = g_wifi_password_ta ? lv_textarea_get_text(g_wifi_password_ta) : "";
    save_and_connect_wifi(ssid, pass);
}

static void on_focus_minus(lv_event_t *e)
{
    (void)e;
    if (g_focus_min > 5) g_focus_min -= 5;
    if (g_mode == MODE_FOCUS && g_state == TIMER_IDLE) reset_mode(MODE_FOCUS);
    show_settings_page();
}

static void on_focus_plus(lv_event_t *e)
{
    (void)e;
    if (g_focus_min < 60) g_focus_min += 5;
    if (g_mode == MODE_FOCUS && g_state == TIMER_IDLE) reset_mode(MODE_FOCUS);
    show_settings_page();
}

static void on_break_minus(lv_event_t *e)
{
    (void)e;
    if (g_short_break_min > 3) g_short_break_min--;
    if (g_mode == MODE_SHORT_BREAK && g_state == TIMER_IDLE) reset_mode(MODE_SHORT_BREAK);
    show_settings_page();
}

static void on_break_plus(lv_event_t *e)
{
    (void)e;
    if (g_short_break_min < 20) g_short_break_min++;
    if (g_mode == MODE_SHORT_BREAK && g_state == TIMER_IDLE) reset_mode(MODE_SHORT_BREAK);
    show_settings_page();
}

static void on_long_break_minus(lv_event_t *e)
{
    (void)e;
    if (g_long_break_min > 10) g_long_break_min -= 5;
    if (g_mode == MODE_LONG_BREAK && g_state == TIMER_IDLE) reset_mode(MODE_LONG_BREAK);
    show_settings_page();
}

static void on_long_break_plus(lv_event_t *e)
{
    (void)e;
    if (g_long_break_min < 30) g_long_break_min += 5;
    if (g_mode == MODE_LONG_BREAK && g_state == TIMER_IDLE) reset_mode(MODE_LONG_BREAK);
    show_settings_page();
}

static void on_round_minus(lv_event_t *e)
{
    (void)e;
    if (g_rounds > 1) g_rounds--;
    if (g_completed_focus >= g_rounds) g_completed_focus = g_rounds - 1;
    show_settings_page();
}

static void on_round_plus(lv_event_t *e)
{
    (void)e;
    if (g_rounds < DEFAULT_ROUNDS) g_rounds++;
    show_settings_page();
}

static void on_weather_minus(lv_event_t *e)
{
    (void)e;
    if (g_weather_refresh_min > 15) g_weather_refresh_min -= 15;
    show_settings_page();
}

static void on_weather_plus(lv_event_t *e)
{
    (void)e;
    if (g_weather_refresh_min < 60) g_weather_refresh_min += 15;
    show_settings_page();
}

static void on_brightness_minus(lv_event_t *e)
{
    (void)e;
    if (g_brightness_pct > 30) g_brightness_pct -= 10;
    show_settings_page();
}

static void on_brightness_plus(lv_event_t *e)
{
    (void)e;
    if (g_brightness_pct < 100) g_brightness_pct += 10;
    show_settings_page();
}

static void create_background(lv_obj_t *scr)
{
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG_DARK), 0);
    lv_obj_set_style_bg_grad_color(scr, lv_color_hex(C_BG_GLOW), 0);
    lv_obj_set_style_bg_grad_dir(scr, LV_GRAD_DIR_HOR, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
}

static void create_top_bar(lv_obj_t *scr)
{
    g_page_title = make_label(scr, "Tomato Glow Clock", &lv_font_montserrat_24, C_TEXT, 38, 28);
    make_label(scr, "Focus / Break / Warm desk clock", &lv_font_montserrat_14, C_TEXT_DIM, 39, 62);
    g_time_label = make_label(scr, "--:--", &lv_font_montserrat_28, C_TEXT, 610, 24);
    g_date_label = make_label(scr, "Time syncing", &lv_font_montserrat_14, C_TEXT_DIM, 612, 60);
    make_button(scr, "Menu", 700, 24, 66, 36, C_BUTTON_DARK, C_TEXT, on_back);
}

static void create_main_card(lv_obj_t *scr)
{
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, 732, 316);
    lv_obj_set_pos(card, 34, 92);
    style_panel(card, C_CARD_SOFT, LV_OPA_70, 28);

    lv_obj_t *circle_bg = lv_obj_create(card);
    lv_obj_set_size(circle_bg, 248, 248);
    lv_obj_set_pos(circle_bg, 86, 34);
    lv_obj_set_style_radius(circle_bg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle_bg, lv_color_hex(C_CARD_DARK), 0);
    lv_obj_set_style_bg_opa(circle_bg, LV_OPA_70, 0);
    lv_obj_set_style_border_width(circle_bg, 0, 0);
    lv_obj_clear_flag(circle_bg, LV_OBJ_FLAG_SCROLLABLE);

    g_timer_arc = lv_arc_create(card);
    lv_obj_set_size(g_timer_arc, 238, 238);
    lv_obj_set_pos(g_timer_arc, 91, 39);
    lv_arc_set_range(g_timer_arc, 0, 1000);
    lv_arc_set_value(g_timer_arc, 1000);
    lv_arc_set_bg_angles(g_timer_arc, 120, 60);
    lv_obj_set_style_arc_width(g_timer_arc, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_timer_arc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_timer_arc, lv_color_hex(0x5B2A19), LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_timer_arc, lv_color_hex(C_HIGHLIGHT), LV_PART_INDICATOR);
    lv_obj_remove_style(g_timer_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(g_timer_arc, LV_OBJ_FLAG_CLICKABLE);

    g_mode_label = make_label(card, "Focus Mode", &lv_font_montserrat_20, C_TEXT, 167, 118);
    lv_obj_set_style_text_align(g_mode_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_mode_label, 120);

    g_countdown_label = make_label(card, "25:00", &lv_font_montserrat_48, 0xFFFFFF, 143, 151);
    lv_obj_set_style_text_align(g_countdown_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_countdown_label, 168);

    g_tomato_label = make_label(card, "Tomato 1 / 4", &lv_font_montserrat_14, C_TEXT_DIM, 160, 220);
    lv_obj_set_style_text_align(g_tomato_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_tomato_label, 135);

    lv_obj_t *weather_card = lv_obj_create(card);
    lv_obj_set_size(weather_card, 300, 132);
    lv_obj_set_pos(weather_card, 386, 34);
    style_panel(weather_card, C_CARD_DARK, LV_OPA_50, 22);
    g_weather_city_label = make_label(weather_card, "Zhongshan Nanlang", &lv_font_montserrat_16, C_TEXT, 24, 18);
    lv_obj_set_width(g_weather_city_label, 250);
    lv_label_set_long_mode(g_weather_city_label, LV_LABEL_LONG_DOT);
    g_weather_temp_label = make_label(weather_card, "26 deg", &lv_font_montserrat_28, 0xFFFFFF, 24, 48);
    g_weather_detail_label = make_label(weather_card, "Cloudy / Humidity 68%", &lv_font_montserrat_16, C_TEXT_DIM, 24, 86);
    lv_obj_set_width(g_weather_detail_label, 250);
    lv_label_set_long_mode(g_weather_detail_label, LV_LABEL_LONG_DOT);
    g_weather_status_label = make_label(weather_card, "Local sample", &lv_font_montserrat_12, C_MUTED, 24, 112);
    lv_obj_set_width(g_weather_status_label, 250);
    lv_label_set_long_mode(g_weather_status_label, LV_LABEL_LONG_DOT);

    lv_obj_t *rhythm_card = lv_obj_create(card);
    lv_obj_set_size(rhythm_card, 300, 78);
    lv_obj_set_pos(rhythm_card, 386, 184);
    style_panel(rhythm_card, C_CARD_DARK, LV_OPA_40, 22);
    make_label(rhythm_card, "Today Rhythm", &lv_font_montserrat_16, C_TEXT, 24, 12);
    g_rhythm_label = make_label(rhythm_card, "25 min focus + 5 min break", &lv_font_montserrat_14, C_TEXT_DIM, 24, 48);
    lv_obj_set_width(g_rhythm_label, 250);
    lv_label_set_long_mode(g_rhythm_label, LV_LABEL_LONG_DOT);

    lv_obj_t *bar_bg = lv_obj_create(rhythm_card);
    lv_obj_set_size(bar_bg, 210, 8);
    lv_obj_set_pos(bar_bg, 24, 36);
    lv_obj_set_style_radius(bar_bg, 5, 0);
    lv_obj_set_style_bg_color(bar_bg, lv_color_hex(0x4B1D12), 0);
    lv_obj_set_style_border_width(bar_bg, 0, 0);
    lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < DEFAULT_ROUNDS; ++i) {
        g_round_dots[i] = lv_obj_create(rhythm_card);
        lv_obj_set_size(g_round_dots[i], 14, 14);
        lv_obj_set_pos(g_round_dots[i], 26 + i * 54, 33);
        lv_obj_set_style_radius(g_round_dots[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(g_round_dots[i], 0, 0);
        lv_obj_clear_flag(g_round_dots[i], LV_OBJ_FLAG_SCROLLABLE);
    }
}

static void create_bottom_buttons(lv_obj_t *scr)
{
    glow_button_t pause = make_button(scr, "Pause", 88, 424, 140, 40, C_BUTTON_DARK, C_TEXT, on_pause);
    glow_button_t start = make_button(scr, "Start", 255, 424, 140, 40, C_HIGHLIGHT, C_CARD_DARK, on_start);
    make_button(scr, "Skip", 422, 424, 140, 40, C_BUTTON_DARK, C_TEXT, on_skip);
    make_button(scr, "Settings", 589, 424, 140, 40, C_BUTTON_DARK, C_TEXT, on_settings);
    g_pause_label = pause.label;
    g_start_label = start.label;
}

static void show_main_page(void)
{
    g_page = PAGE_MAIN;
    if (g_scr) lv_obj_clean(g_scr);
    clear_page_refs();
    create_background(g_scr);
    create_top_bar(g_scr);
    create_main_card(g_scr);
    create_bottom_buttons(g_scr);
    update_clock_labels();
    update_weather_labels();
    update_timer_labels();
}

static void add_setting_tile(lv_obj_t *parent, const char *name, const char *value,
                             int col, int row_idx, lv_event_cb_t minus_cb, lv_event_cb_t plus_cb)
{
    const int tile_w = 322;
    const int tile_h = 62;
    const int gap = 24;
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_size(tile, tile_w, tile_h);
    lv_obj_set_pos(tile, col * (tile_w + gap), row_idx * (tile_h + 14));
    style_panel(tile, C_CARD_DARK, LV_OPA_40, 16);

    lv_obj_t *title = make_label(tile, name, &lv_font_montserrat_14, C_TEXT_DIM, 18, 10);
    lv_obj_set_width(title, 150);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    lv_obj_t *v = make_label(tile, value, &lv_font_montserrat_20, C_TEXT, 154, 20);
    lv_obj_set_width(v, 78);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);

    make_button(tile, "-", 232, 14, 36, 34, C_BUTTON_DARK, C_TEXT, minus_cb);
    make_button(tile, "+", 274, 14, 36, 34, C_HIGHLIGHT, C_CARD_DARK, plus_cb);
}

static void show_settings_page(void)
{
    char buf[32];
    g_page = PAGE_SETTINGS;
    lv_obj_clean(g_scr);
    clear_page_refs();
    create_background(g_scr);
    create_top_bar(g_scr);
    lv_label_set_text(g_page_title, "Settings");

    lv_obj_t *panel = lv_obj_create(g_scr);
    lv_obj_set_size(panel, 732, 316);
    lv_obj_set_pos(panel, 34, 92);
    style_panel(panel, C_CARD_SOFT, LV_OPA_70, 24);
    make_label(panel, "Pomodoro Settings", &lv_font_montserrat_20, C_TEXT, 28, 20);
    make_label(panel, "Large touch controls, no crowded rows.", &lv_font_montserrat_14,
               C_TEXT_DIM, 252, 24);
    make_button(panel, "WiFi", 598, 16, 96, 36, C_HIGHLIGHT, C_CARD_DARK, on_wifi_settings);

    g_settings_list = lv_obj_create(panel);
    lv_obj_set_size(g_settings_list, 682, 228);
    lv_obj_set_pos(g_settings_list, 25, 72);
    lv_obj_set_style_bg_opa(g_settings_list, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_settings_list, 0, 0);
    lv_obj_clear_flag(g_settings_list, LV_OBJ_FLAG_SCROLLABLE);

    snprintf(buf, sizeof(buf), "%d min", g_focus_min);
    add_setting_tile(g_settings_list, "Focus", buf, 0, 0, on_focus_minus, on_focus_plus);
    snprintf(buf, sizeof(buf), "%d min", g_short_break_min);
    add_setting_tile(g_settings_list, "Short Break", buf, 1, 0, on_break_minus, on_break_plus);
    snprintf(buf, sizeof(buf), "%d min", g_long_break_min);
    add_setting_tile(g_settings_list, "Long Break", buf, 0, 1, on_long_break_minus, on_long_break_plus);
    snprintf(buf, sizeof(buf), "%d rounds", g_rounds);
    add_setting_tile(g_settings_list, "Rounds", buf, 1, 1, on_round_minus, on_round_plus);
    snprintf(buf, sizeof(buf), "%d min", g_weather_refresh_min);
    add_setting_tile(g_settings_list, "Weather", buf, 0, 2, on_weather_minus, on_weather_plus);
    snprintf(buf, sizeof(buf), "%d%%", g_brightness_pct);
    add_setting_tile(g_settings_list, "Brightness", buf, 1, 2, on_brightness_minus, on_brightness_plus);

    make_button(g_scr, "Back", 330, 424, 140, 40, C_HIGHLIGHT, C_CARD_DARK, on_main);
}

static void update_wifi_scan_list(void)
{
    if (!g_wifi_list) return;
    lv_obj_clean(g_wifi_list);
    g_wifi_scan_dirty = false;

    if (g_wifi_scan_busy) {
        make_label(g_wifi_list, "Scanning nearby WiFi...", &lv_font_montserrat_14, C_TEXT_DIM, 14, 14);
        return;
    }

    if (g_wifi_scan_count <= 0) {
        make_label(g_wifi_list, "No networks yet. Tap Scan.", &lv_font_montserrat_14, C_TEXT_DIM, 14, 14);
        return;
    }

    for (int i = 0; i < g_wifi_scan_count; ++i) {
        char row_text[64];
        snprintf(row_text, sizeof(row_text), "%s  %ddBm  %s",
                 g_wifi_scan_results[i].ssid,
                 (int)g_wifi_scan_results[i].rssi,
                 wifi_auth_name(g_wifi_scan_results[i].authmode));
        lv_obj_t *row = lv_btn_create(g_wifi_list);
        lv_obj_set_size(row, 300, 32);
        lv_obj_set_pos(row, 0, i * 36);
        lv_obj_set_style_bg_color(row, lv_color_hex(C_BUTTON_DARK), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_50, 0);
        lv_obj_set_style_radius(row, 18, 0);
        lv_obj_set_style_shadow_width(row, 0, 0);
        lv_obj_add_event_cb(row, on_wifi_ap_click, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *label = lv_label_create(row);
        lv_label_set_text(label, row_text);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(C_TEXT), 0);
        lv_obj_set_width(label, 270);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_center(label);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    }
}

static lv_obj_t *make_textarea(lv_obj_t *parent, const char *placeholder,
                               lv_coord_t x, lv_coord_t y, bool password)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, 300, 36);
    lv_obj_set_pos(ta, x, y);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_max_length(ta, password ? 64 : 32);
    lv_textarea_set_password_mode(ta, password);
    lv_obj_set_style_bg_color(ta, lv_color_hex(C_CARD_DARK), 0);
    lv_obj_set_style_bg_opa(ta, LV_OPA_70, 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(C_HIGHLIGHT_2), 0);
    lv_obj_set_style_border_opa(ta, LV_OPA_30, 0);
    lv_obj_set_style_border_width(ta, 1, 0);
    lv_obj_set_style_radius(ta, 12, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_TEXT), 0);
    lv_obj_set_style_text_font(ta, &lv_font_montserrat_16, 0);
    lv_obj_add_event_cb(ta, on_wifi_textarea_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, on_wifi_textarea_focus, LV_EVENT_CLICKED, NULL);
    return ta;
}

static void show_wifi_page(void)
{
    g_page = PAGE_WIFI;
    lv_obj_clean(g_scr);
    clear_page_refs();
    create_background(g_scr);
    create_top_bar(g_scr);
    lv_label_set_text(g_page_title, "WiFi Setup");

    lv_obj_t *panel = lv_obj_create(g_scr);
    lv_obj_set_size(panel, 732, 204);
    lv_obj_set_pos(panel, 34, 92);
    style_panel(panel, C_CARD_SOFT, LV_OPA_70, 24);

    make_label(panel, "Nearby networks", &lv_font_montserrat_16, C_TEXT, 28, 20);
    make_button(panel, "Scan", 242, 16, 82, 34, C_HIGHLIGHT, C_CARD_DARK, on_wifi_scan);

    g_wifi_list = lv_obj_create(panel);
    lv_obj_set_size(g_wifi_list, 312, 132);
    lv_obj_set_pos(g_wifi_list, 24, 60);
    lv_obj_set_style_bg_opa(g_wifi_list, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_wifi_list, 0, 0);
    lv_obj_set_scroll_dir(g_wifi_list, LV_DIR_VER);

    make_label(panel, "SSID", &lv_font_montserrat_14, C_TEXT_DIM, 382, 20);
    g_wifi_ssid_ta = make_textarea(panel, "Select or type SSID", 382, 42, false);
    lv_textarea_set_text(g_wifi_ssid_ta, has_text(g_wifi_ssid) ? g_wifi_ssid : "");

    make_label(panel, "Password", &lv_font_montserrat_14, C_TEXT_DIM, 382, 84);
    g_wifi_password_ta = make_textarea(panel, "Leave blank for open WiFi", 382, 106, true);

    g_wifi_status_label = make_label(panel, has_text(g_wifi_ssid) ? "Saved network loaded" : "No saved WiFi",
                                     &lv_font_montserrat_14, C_TEXT_DIM, 382, 146);
    lv_obj_set_width(g_wifi_status_label, 292);
    lv_label_set_long_mode(g_wifi_status_label, LV_LABEL_LONG_DOT);
    make_button(panel, "Connect", 382, 168, 132, 34, C_HIGHLIGHT, C_CARD_DARK, on_wifi_connect);
    make_button(panel, "Back", 534, 168, 118, 34, C_BUTTON_DARK, C_TEXT, on_settings);

    update_wifi_scan_list();
    if (g_wifi_scan_count == 0) start_wifi_scan();
}

static void show_done_page(void)
{
    g_page = PAGE_DONE;
    lv_obj_clean(g_scr);
    clear_page_refs();
    create_background(g_scr);
    create_top_bar(g_scr);
    lv_label_set_text(g_page_title, "Focus Complete");

    lv_obj_t *panel = lv_obj_create(g_scr);
    lv_obj_set_size(panel, 520, 260);
    lv_obj_set_pos(panel, 140, 116);
    style_panel(panel, C_CARD_SOFT, LV_OPA_70, 28);

    lv_obj_t *tomato = lv_obj_create(panel);
    lv_obj_set_size(tomato, 86, 86);
    lv_obj_set_pos(tomato, 217, 36);
    lv_obj_set_style_radius(tomato, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(tomato, lv_color_hex(0xE95A3B), 0);
    lv_obj_set_style_border_width(tomato, 0, 0);
    lv_obj_clear_flag(tomato, LV_OBJ_FLAG_SCROLLABLE);

    make_label(panel, "One tomato is done!", &lv_font_montserrat_28, C_TEXT, 130, 140);
    make_label(panel, g_mode == MODE_LONG_BREAK ? "Long break is ready." : "Take a warm 5 minute break.",
               &lv_font_montserrat_16, C_TEXT_DIM, 164, 184);
    make_button(g_scr, "Start Break", 255, 424, 140, 40, C_HIGHLIGHT, C_CARD_DARK, on_start);
    make_button(g_scr, "Skip", 422, 424, 140, 40, C_BUTTON_DARK, C_TEXT, on_skip);
}

static void weather_set_local_status(const char *status)
{
    if (!g_weather_mutex) return;
    if (xSemaphoreTake(g_weather_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        strncpy(g_weather.status, status, sizeof(g_weather.status) - 1);
        g_weather.status[sizeof(g_weather.status) - 1] = '\0';
        g_weather.online = false;
        g_weather_dirty = true;
        xSemaphoreGive(g_weather_mutex);
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (has_text(g_wifi_ssid)) {
            esp_wifi_connect();
        } else {
            weather_set_local_status("WiFi setup needed");
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (g_wifi_events) xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT);
        if (has_text(g_wifi_ssid)) {
            esp_wifi_connect();
            weather_set_local_status("WiFi reconnecting");
        } else {
            weather_set_local_status("WiFi setup needed");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (g_wifi_events) xEventGroupSetBits(g_wifi_events, WIFI_CONNECTED_BIT);
        weather_set_local_status("WiFi online");
    }
}

static void start_sntp_once(void)
{
    if (g_sntp_started) return;
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    g_sntp_started = true;
}

static void load_wifi_credentials_once(void)
{
    static bool loaded;
    if (loaded) return;
    loaded = true;

    strncpy(g_wifi_ssid, TOMATO_WIFI_SSID, sizeof(g_wifi_ssid) - 1);
    g_wifi_ssid[sizeof(g_wifi_ssid) - 1] = '\0';
    strncpy(g_wifi_pass, TOMATO_WIFI_PASS, sizeof(g_wifi_pass) - 1);
    g_wifi_pass[sizeof(g_wifi_pass) - 1] = '\0';

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("tomato_wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) return;

    size_t len = sizeof(g_wifi_ssid);
    if (nvs_get_str(nvs, "ssid", g_wifi_ssid, &len) != ESP_OK) {
        g_wifi_ssid[0] = '\0';
    }
    len = sizeof(g_wifi_pass);
    if (nvs_get_str(nvs, "pass", g_wifi_pass, &len) != ESP_OK) {
        g_wifi_pass[0] = '\0';
    }
    nvs_close(nvs);
}

static bool save_wifi_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("tomato_wifi", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }

    err = nvs_set_str(nvs, "ssid", ssid);
    if (err == ESP_OK) err = nvs_set_str(nvs, "pass", pass ? pass : "");
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save wifi failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static esp_err_t apply_wifi_config(void)
{
    if (!has_text(g_wifi_ssid)) return ESP_ERR_INVALID_ARG;

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.sta.ssid, g_wifi_ssid, sizeof(wifi_config.sta.ssid));
    strncpy((char *)wifi_config.sta.password, g_wifi_pass, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = has_text(g_wifi_pass) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;

    return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

static void save_and_connect_wifi(const char *ssid, const char *pass)
{
    if (!has_text(ssid)) {
        wifi_set_status("SSID is empty");
        return;
    }

    strncpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid) - 1);
    g_wifi_ssid[sizeof(g_wifi_ssid) - 1] = '\0';
    strncpy(g_wifi_pass, pass ? pass : "", sizeof(g_wifi_pass) - 1);
    g_wifi_pass[sizeof(g_wifi_pass) - 1] = '\0';

    if (!save_wifi_credentials(g_wifi_ssid, g_wifi_pass)) {
        wifi_set_status("WiFi save failed");
        return;
    }

    wifi_set_status("Saved. Connecting");
    start_wifi_once();
    if (g_net_started) {
        if (g_wifi_events) xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_disconnect();
        esp_err_t err = apply_wifi_config();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "apply wifi failed: %s", esp_err_to_name(err));
            wifi_set_status("WiFi config failed");
            return;
        }
        esp_wifi_connect();
    }
}

static void start_wifi_once(void)
{
    load_wifi_credentials_once();
    if (g_net_started) return;

    g_wifi_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (has_text(g_wifi_ssid)) {
        ESP_ERROR_CHECK(apply_wifi_config());
    } else {
        weather_set_local_status("WiFi setup needed");
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    start_sntp_once();
    g_net_started = true;
}

static void wifi_scan_task(void *arg)
{
    (void)arg;
    start_wifi_once();
    g_wifi_scan_busy = true;
    g_wifi_scan_dirty = true;

    wifi_scan_config_t scan_config = {
        .show_hidden = true,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi scan failed: %s", esp_err_to_name(err));
        g_wifi_scan_count = 0;
        g_wifi_scan_busy = false;
        g_wifi_scan_dirty = true;
        if (has_text(g_wifi_ssid)) esp_wifi_connect();
        vTaskDelete(NULL);
        return;
    }

    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    if (count > 16) count = 16;
    wifi_ap_record_t records[16] = {0};
    esp_wifi_scan_get_ap_records(&count, records);

    for (uint16_t i = 0; i < count; ++i) {
        for (uint16_t j = i + 1; j < count; ++j) {
            if (records[j].rssi > records[i].rssi) {
                wifi_ap_record_t tmp = records[i];
                records[i] = records[j];
                records[j] = tmp;
            }
        }
    }

    int copied = 0;
    for (uint16_t i = 0; i < count && copied < (int)(sizeof(g_wifi_scan_results) / sizeof(g_wifi_scan_results[0])); ++i) {
        if (records[i].ssid[0] == '\0') continue;
        strncpy(g_wifi_scan_results[copied].ssid, (const char *)records[i].ssid,
                sizeof(g_wifi_scan_results[copied].ssid) - 1);
        g_wifi_scan_results[copied].ssid[sizeof(g_wifi_scan_results[copied].ssid) - 1] = '\0';
        g_wifi_scan_results[copied].rssi = records[i].rssi;
        g_wifi_scan_results[copied].authmode = records[i].authmode;
        copied++;
    }
    g_wifi_scan_count = copied;
    g_wifi_scan_busy = false;
    g_wifi_scan_dirty = true;
    if (has_text(g_wifi_ssid)) esp_wifi_connect();
    vTaskDelete(NULL);
}

static void start_wifi_scan(void)
{
    if (g_wifi_scan_busy) return;
    g_wifi_scan_busy = true;
    g_wifi_scan_dirty = true;
    wifi_set_status("Scanning WiFi");
    xTaskCreate(wifi_scan_task, "tomato_wifi_scan", 4096, NULL, 4, NULL);
}

typedef struct {
    char data[WEATHER_BUF_SIZE];
    int len;
} http_capture_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_capture_t *cap = (http_capture_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && cap && evt->data && evt->data_len > 0) {
        int copy_len = evt->data_len;
        if (cap->len + copy_len >= WEATHER_BUF_SIZE) {
            copy_len = WEATHER_BUF_SIZE - cap->len - 1;
        }
        if (copy_len > 0) {
            memcpy(cap->data + cap->len, evt->data, copy_len);
            cap->len += copy_len;
            cap->data[cap->len] = '\0';
        }
    }
    return ESP_OK;
}

static bool json_get_string(cJSON *obj, const char *name, char *out, size_t out_len)
{
    cJSON *item = cJSON_GetObjectItem(obj, name);
    if (!cJSON_IsString(item) || !item->valuestring) return false;
    strncpy(out, item->valuestring, out_len - 1);
    out[out_len - 1] = '\0';
    return true;
}

static bool base64url_encode(const uint8_t *src, size_t src_len, char *out, size_t out_len)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t needed = ((src_len + 2) / 3) * 4;
    if (src_len % 3) needed -= 3 - (src_len % 3);
    if (out_len < needed + 1) return false;

    size_t pos = 0;
    for (size_t i = 0; i < src_len; i += 3) {
        size_t remain = src_len - i;
        uint32_t triple = ((uint32_t)src[i]) << 16;
        if (remain > 1) triple |= ((uint32_t)src[i + 1]) << 8;
        if (remain > 2) triple |= src[i + 2];

        out[pos++] = table[(triple >> 18) & 0x3F];
        out[pos++] = table[(triple >> 12) & 0x3F];
        if (remain > 1) out[pos++] = table[(triple >> 6) & 0x3F];
        if (remain > 2) out[pos++] = table[triple & 0x3F];
    }

    out[pos] = '\0';
    return true;
}

static bool qweather_sign_jwt_input(const char *input, uint8_t *signature,
                                    size_t signature_len, size_t *signature_actual_len)
{
    psa_status_t status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        ESP_LOGW(TAG, "psa_crypto_init failed: %ld", (long)status);
        return false;
    }

    psa_key_attributes_t attributes = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attributes, PSA_KEY_USAGE_SIGN_MESSAGE);
    psa_set_key_algorithm(&attributes, PSA_ALG_PURE_EDDSA);
    psa_set_key_type(&attributes, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_TWISTED_EDWARDS));
    psa_set_key_bits(&attributes, 255);

    psa_key_id_t key_id = 0;
    status = psa_import_key(&attributes, TOMATO_QWEATHER_ED25519_SEED,
                            sizeof(TOMATO_QWEATHER_ED25519_SEED), &key_id);
    psa_reset_key_attributes(&attributes);
    if (status != PSA_SUCCESS) {
        ESP_LOGW(TAG, "psa_import_key failed: %ld", (long)status);
        return false;
    }

    status = psa_sign_message(key_id, PSA_ALG_PURE_EDDSA, (const uint8_t *)input,
                              strlen(input), signature, signature_len, signature_actual_len);
    psa_destroy_key(key_id);
    if (status != PSA_SUCCESS) {
        ESP_LOGW(TAG, "psa_sign_message failed: %ld", (long)status);
        return false;
    }

    return true;
}

static bool build_qweather_jwt(char *jwt, size_t jwt_len)
{
    if (!TOMATO_QWEATHER_JWT_SECRET_AVAILABLE ||
        !has_text(TOMATO_QWEATHER_JWT_CREDENTIAL_ID) ||
        !has_text(TOMATO_QWEATHER_JWT_PROJECT_ID)) {
        weather_set_local_status("Set QWeather JWT");
        return false;
    }

    time_t now = time(NULL);
    if (now < MIN_VALID_TIME_EPOCH) {
        weather_set_local_status("Time syncing");
        return false;
    }

    char header_json[96];
    char payload_json[128];
    snprintf(header_json, sizeof(header_json), "{\"alg\":\"EdDSA\",\"kid\":\"%s\"}",
             TOMATO_QWEATHER_JWT_CREDENTIAL_ID);
    snprintf(payload_json, sizeof(payload_json), "{\"sub\":\"%s\",\"iat\":%ld,\"exp\":%ld}",
             TOMATO_QWEATHER_JWT_PROJECT_ID, (long)now, (long)(now + QWEATHER_JWT_TTL_S));

    char header_b64[128];
    char payload_b64[192];
    if (!base64url_encode((const uint8_t *)header_json, strlen(header_json),
                          header_b64, sizeof(header_b64)) ||
        !base64url_encode((const uint8_t *)payload_json, strlen(payload_json),
                          payload_b64, sizeof(payload_b64))) {
        weather_set_local_status("JWT encode failed");
        return false;
    }

    char signing_input[320];
    int n = snprintf(signing_input, sizeof(signing_input), "%s.%s", header_b64, payload_b64);
    if (n < 0 || (size_t)n >= sizeof(signing_input)) {
        weather_set_local_status("JWT too long");
        return false;
    }

    uint8_t signature[64];
    size_t signature_len = 0;
    if (!qweather_sign_jwt_input(signing_input, signature, sizeof(signature), &signature_len)) {
        weather_set_local_status("JWT sign failed");
        return false;
    }

    char signature_b64[96];
    if (!base64url_encode(signature, signature_len, signature_b64, sizeof(signature_b64))) {
        weather_set_local_status("JWT sig encode failed");
        return false;
    }

    n = snprintf(jwt, jwt_len, "%s.%s", signing_input, signature_b64);
    if (n < 0 || (size_t)n >= jwt_len) {
        weather_set_local_status("JWT output too long");
        return false;
    }

    return true;
}

static bool wait_for_valid_time(void)
{
    for (int i = 0; i < 30; ++i) {
        if (time(NULL) >= MIN_VALID_TIME_EPOCH) return true;
        weather_set_local_status("Time syncing");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return time(NULL) >= MIN_VALID_TIME_EPOCH;
}

static bool fetch_weather_once(void)
{
    if (!wait_for_valid_time()) {
        weather_set_local_status("Time sync failed");
        return false;
    }

    char jwt[512];
    if (!build_qweather_jwt(jwt, sizeof(jwt))) {
        return false;
    }

    http_capture_t cap = {0};
    char url[160];
    snprintf(url, sizeof(url),
             "https://devapi.qweather.com/v7/weather/now?location=%s&lang=en&unit=m",
             TOMATO_QWEATHER_LOCATION);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &cap,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 8000,
        .buffer_size = 1024,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        weather_set_local_status("HTTP init failed");
        return false;
    }

    char auth_header[544];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", jwt);
    esp_http_client_set_header(client, "Authorization", auth_header);
    esp_err_t err = esp_http_client_perform(client);
    int http_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || http_code != 200) {
        ESP_LOGW(TAG, "weather request failed: err=%s code=%d", esp_err_to_name(err), http_code);
        weather_set_local_status(http_code == 401 ? "QWeather auth failed" : "Weather offline");
        return false;
    }

    cJSON *root = cJSON_Parse(cap.data);
    if (!root) {
        weather_set_local_status("Weather parse failed");
        return false;
    }

    cJSON *code = cJSON_GetObjectItem(root, "code");
    cJSON *now = cJSON_GetObjectItem(root, "now");
    if (!cJSON_IsString(code) || strcmp(code->valuestring, "200") != 0 || !cJSON_IsObject(now)) {
        cJSON_Delete(root);
        weather_set_local_status("QWeather rejected");
        return false;
    }

    if (xSemaphoreTake(g_weather_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        strncpy(g_weather.city, "Zhongshan Nanlang", sizeof(g_weather.city) - 1);
        json_get_string(now, "temp", g_weather.temp, sizeof(g_weather.temp));
        json_get_string(now, "text", g_weather.text, sizeof(g_weather.text));
        json_get_string(now, "humidity", g_weather.humidity, sizeof(g_weather.humidity));
        strncpy(g_weather.status, "QWeather updated", sizeof(g_weather.status) - 1);
        g_weather.online = true;
        g_weather_dirty = true;
        xSemaphoreGive(g_weather_mutex);
    }

    cJSON_Delete(root);
    return true;
}

static void weather_task(void *arg)
{
    (void)arg;
    start_wifi_once();

    while (true) {
        if (!g_wifi_events || !has_text(g_wifi_ssid)) {
            weather_set_local_status("WiFi setup needed");
            vTaskDelay(pdMS_TO_TICKS(60000));
            continue;
        }

        EventBits_t bits = xEventGroupWaitBits(g_wifi_events, WIFI_CONNECTED_BIT,
                                               pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
        if ((bits & WIFI_CONNECTED_BIT) == 0) {
            weather_set_local_status("Waiting for WiFi");
            vTaskDelay(pdMS_TO_TICKS(30000));
            continue;
        }

        fetch_weather_once();
        vTaskDelay(pdMS_TO_TICKS(g_weather_refresh_min * 60 * 1000));
    }
}

static void start_weather_task_once(void)
{
    if (!g_weather_mutex) g_weather_mutex = xSemaphoreCreateMutex();
    if (g_weather_task_started) return;
    g_weather_task_started = true;
    xTaskCreate(weather_task, "tomato_weather", 12288, NULL, 4, NULL);
}

void tomato_timer_start(void)
{
    if (!g_weather_mutex) g_weather_mutex = xSemaphoreCreateMutex();
    load_wifi_credentials_once();

    g_scr = lv_obj_create(NULL);
    lv_scr_load(g_scr);

    reset_mode(g_mode);
    show_main_page();

    if (g_tick_timer) lv_timer_del(g_tick_timer);
    g_tick_timer = lv_timer_create(tick_cb, 1000, NULL);

    start_weather_task_once();
}
