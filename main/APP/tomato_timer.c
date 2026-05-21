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
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "lwip/sockets.h"
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
static const unsigned char TOMATO_QWEATHER_ED25519_SEED[32] __attribute__((unused)) = {0};
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

typedef enum {
    WX_ANIM_CLOUDY,
    WX_ANIM_SUNNY,
    WX_ANIM_RAINY,
    WX_ANIM_SNOWY,
    WX_ANIM_STORMY,
    WX_ANIM_FOGGY
} weather_anim_t;

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
static lv_obj_t *g_weather_scene;
static lv_obj_t *g_weather_motes[8];
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

/* ---- Captive Portal globals ---- */
static httpd_handle_t g_portal_httpd = NULL;
static TaskHandle_t   g_dns_task_handle = NULL;
static esp_netif_t   *g_ap_netif = NULL;
static volatile bool  g_portal_active = false;
static volatile int   g_portal_connect_status = 0; /* 0=idle, 1=connecting, 2=ok, -1=fail */

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
static weather_anim_t g_weather_anim_kind = WX_ANIM_CLOUDY;
static bool g_wifi_connecting_manually;
static char g_wifi_ssid[33];
static char g_wifi_pass[65];

/* ---- Multi-network credential storage (up to 5) ---- */
#define WIFI_CRED_MAX 5
typedef struct {
    char ssid[33];
    char pass[65];
} wifi_cred_t;
static wifi_cred_t g_wifi_creds[WIFI_CRED_MAX];
static int g_wifi_cred_count = 0;
static int g_wifi_cred_idx = 0;      /* currently active credential index */
static int g_wifi_fail_count = 0;    /* consecutive disconnects for current cred */
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
static void start_captive_portal(void);
static void stop_captive_portal(void);
static void portal_shutdown_task(void *arg);
static esp_err_t apply_wifi_config(void);
static void on_settings(lv_event_t *e);
static lv_obj_t *make_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                            uint32_t color, lv_coord_t x, lv_coord_t y);

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
    if (g_wifi_keyboard) {
        ESP_LOGI(TAG, "Deleting top-layer keyboard...");
        lv_obj_del(g_wifi_keyboard);
        g_wifi_keyboard = NULL;
    }
    g_wifi_status_label = NULL;
    g_wifi_list = NULL;
    g_wifi_ssid_ta = NULL;
    g_wifi_password_ta = NULL;
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
    g_weather_scene = NULL;
    for (int i = 0; i < 8; ++i) {
        g_weather_motes[i] = NULL;
    }
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

static void anim_x_cb(void *obj, int32_t v)
{
    if (obj) lv_obj_set_x((lv_obj_t *)obj, v);
}

static void anim_y_cb(void *obj, int32_t v)
{
    if (obj) lv_obj_set_y((lv_obj_t *)obj, v);
}

static void anim_opa_cb(void *obj, int32_t v)
{
    if (obj) lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static weather_anim_t weather_kind_from_text(const char *text)
{
    if (!text) return WX_ANIM_CLOUDY;
    if (strstr(text, "Sunny")) return WX_ANIM_SUNNY;
    if (strstr(text, "Rain")) return WX_ANIM_RAINY;
    if (strstr(text, "Snow")) return WX_ANIM_SNOWY;
    if (strstr(text, "Storm")) return WX_ANIM_STORMY;
    if (strstr(text, "Fog")) return WX_ANIM_FOGGY;
    return WX_ANIM_CLOUDY;
}

static lv_obj_t *create_disc(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                             lv_coord_t size, uint32_t color, lv_opa_t opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, size, size);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t *create_mote(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                             lv_coord_t w, lv_coord_t h, uint32_t color)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_radius(o, h / 2, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_80, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_shadow_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static void start_loop_anim(lv_obj_t *obj, lv_anim_exec_xcb_t cb,
                            int32_t from, int32_t to, uint32_t time_ms,
                            uint32_t delay_ms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_time(&a, time_ms);
    lv_anim_set_delay(&a, delay_ms);
    lv_anim_set_playback_time(&a, time_ms);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void add_cloud_shape(lv_obj_t *parent)
{
    create_disc(parent, 30, 48, 44, 0xFFFFFF, LV_OPA_70);
    create_disc(parent, 58, 34, 58, 0xFFFFFF, LV_OPA_80);
    create_disc(parent, 98, 50, 38, 0xFFFFFF, LV_OPA_70);
    create_mote(parent, 38, 72, 88, 22, 0xFFFFFF);
}

static void rebuild_weather_animation(weather_anim_t kind)
{
    if (!g_weather_scene) return;
    lv_obj_clean(g_weather_scene);
    memset(g_weather_motes, 0, sizeof(g_weather_motes));
    g_weather_anim_kind = kind;

    if (kind == WX_ANIM_SUNNY) {
        lv_obj_t *sun = create_disc(g_weather_scene, 46, 30, 76, C_HIGHLIGHT, LV_OPA_COVER);
        start_loop_anim(sun, anim_opa_cb, LV_OPA_70, LV_OPA_COVER, 1100, 0);
        for (int i = 0; i < 8; ++i) {
            int x = 82 + (i % 4) * 18 - (i / 4) * 72;
            int y = (i < 4) ? 18 : 106;
            g_weather_motes[i] = create_mote(g_weather_scene, x, y, 26, 4, C_HIGHLIGHT_2);
            start_loop_anim(g_weather_motes[i], anim_opa_cb, LV_OPA_40, LV_OPA_90, 1200, i * 120);
        }
        return;
    }

    add_cloud_shape(g_weather_scene);

    if (kind == WX_ANIM_RAINY || kind == WX_ANIM_STORMY) {
        for (int i = 0; i < 6; ++i) {
            g_weather_motes[i] = create_mote(g_weather_scene, 38 + i * 17, 88, 5, 20, 0x8AD9FF);
            start_loop_anim(g_weather_motes[i], anim_y_cb, 88, 118, 650, i * 120);
            start_loop_anim(g_weather_motes[i], anim_opa_cb, LV_OPA_30, LV_OPA_COVER, 650, i * 120);
        }
        if (kind == WX_ANIM_STORMY) {
            lv_obj_t *bolt = make_label(g_weather_scene, "Z", &lv_font_montserrat_28, C_HIGHLIGHT, 76, 82);
            start_loop_anim(bolt, anim_opa_cb, LV_OPA_0, LV_OPA_COVER, 360, 0);
        }
        return;
    }

    if (kind == WX_ANIM_SNOWY) {
        for (int i = 0; i < 6; ++i) {
            g_weather_motes[i] = create_disc(g_weather_scene, 42 + i * 16, 90, 8, 0xFFFFFF, LV_OPA_90);
            start_loop_anim(g_weather_motes[i], anim_y_cb, 88, 120, 1200, i * 140);
            start_loop_anim(g_weather_motes[i], anim_x_cb, 42 + i * 16, 48 + i * 16, 1200, i * 140);
        }
        return;
    }

    if (kind == WX_ANIM_FOGGY) {
        for (int i = 0; i < 4; ++i) {
            g_weather_motes[i] = create_mote(g_weather_scene, 24, 88 + i * 10, 110, 5, 0xFFEBC9);
            start_loop_anim(g_weather_motes[i], anim_x_cb, 22, 42, 1700, i * 180);
            start_loop_anim(g_weather_motes[i], anim_opa_cb, LV_OPA_40, LV_OPA_80, 1700, i * 180);
        }
        return;
    }

    for (int i = 0; i < 3; ++i) {
        g_weather_motes[i] = create_disc(g_weather_scene, 26 + i * 40, 92, 9, 0xFFFFFF, LV_OPA_50);
        start_loop_anim(g_weather_motes[i], anim_y_cb, 90, 98, 1300, i * 180);
    }
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

    weather_anim_t next_kind = weather_kind_from_text(copy.text);
    if (g_weather_scene && next_kind != g_weather_anim_kind) {
        rebuild_weather_animation(next_kind);
    }
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

static void keyboard_event_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *kb = lv_event_get_target(e);
    ESP_LOGI(TAG, "keyboard_event_cb triggered! event_code=%d", (int)code);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        ESP_LOGI(TAG, "Keyboard Ready/Cancel event triggered. Hiding keyboard.");
        lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
        
        lv_obj_t *ta = lv_keyboard_get_textarea(kb);
        if (ta) {
            lv_obj_clear_state(ta, LV_STATE_FOCUSED);
        }
    }
}

static void on_wifi_textarea_changed(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target(e);
    const char *text = lv_textarea_get_text(ta);
    ESP_LOGI(TAG, "[TEXTAREA] ta=%p value changed: len=%d", ta, (int)strlen(text));
}


static void on_wifi_textarea_focus(lv_event_t *e)
{
    if (!e) return;
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *ta = lv_event_get_target(e);
    ESP_LOGI(TAG, "on_wifi_textarea_focus triggered! event_code=%d, ta=%p", (int)code, ta);

    if (!g_wifi_keyboard) {
        ESP_LOGI(TAG, "Creating LVGL keyboard on top-layer lv_layer_top()...");
        g_wifi_keyboard = lv_keyboard_create(lv_layer_top());
        if (g_wifi_keyboard) {
            ESP_LOGI(TAG, "Keyboard created successfully on top-layer: %p", g_wifi_keyboard);
            lv_obj_set_size(g_wifi_keyboard, 800, 168);
            lv_obj_set_pos(g_wifi_keyboard, 0, 312);
            lv_obj_set_style_bg_color(g_wifi_keyboard, lv_color_hex(C_CARD_DARK), 0);
            lv_obj_set_style_bg_opa(g_wifi_keyboard, LV_OPA_COVER, 0);
            
            /* Crucial: clear click focusable so clicking keys won't defocus the textarea */
            lv_obj_clear_flag(g_wifi_keyboard, LV_OBJ_FLAG_CLICK_FOCUSABLE);
            
            lv_obj_add_event_cb(g_wifi_keyboard, keyboard_event_cb, LV_EVENT_ALL, NULL);
        } else {
            ESP_LOGE(TAG, "FAILED to create keyboard on top-layer!");
        }
    }
    if (!g_wifi_keyboard) {
        ESP_LOGW(TAG, "Keyboard object is NULL, skipping association.");
        return;
    }
    ESP_LOGI(TAG, "Associating keyboard with textarea...");
    lv_keyboard_set_textarea(g_wifi_keyboard, ta);
    lv_obj_move_foreground(g_wifi_keyboard);
    lv_obj_clear_flag(g_wifi_keyboard, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "Keyboard shown and unhidden.");
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
    ESP_LOGI(TAG, "on_wifi_connect clicked! ssid='%s', pass_len=%d", ssid, (int)strlen(pass));
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
    lv_obj_set_size(card, 732, 306);
    lv_obj_set_pos(card, 34, 98);
    style_panel(card, C_CARD_SOFT, LV_OPA_70, 28);
    lv_obj_set_style_pad_all(card, 0, 0);

    lv_obj_t *circle_bg = lv_obj_create(card);
    lv_obj_set_size(circle_bg, 254, 254);
    lv_obj_set_pos(circle_bg, 66, 26);
    lv_obj_set_style_radius(circle_bg, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(circle_bg, lv_color_hex(C_CARD_DARK), 0);
    lv_obj_set_style_bg_opa(circle_bg, LV_OPA_70, 0);
    lv_obj_set_style_border_width(circle_bg, 0, 0);
    lv_obj_clear_flag(circle_bg, LV_OBJ_FLAG_SCROLLABLE);

    g_timer_arc = lv_arc_create(card);
    lv_obj_set_size(g_timer_arc, 244, 244);
    lv_obj_set_pos(g_timer_arc, 71, 31);
    lv_arc_set_range(g_timer_arc, 0, 1000);
    lv_arc_set_value(g_timer_arc, 1000);
    lv_arc_set_bg_angles(g_timer_arc, 120, 60);
    lv_obj_set_style_arc_width(g_timer_arc, 18, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_timer_arc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_timer_arc, lv_color_hex(0x5B2A19), LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_timer_arc, lv_color_hex(C_HIGHLIGHT), LV_PART_INDICATOR);
    lv_obj_remove_style(g_timer_arc, NULL, LV_PART_KNOB);
    lv_obj_clear_flag(g_timer_arc, LV_OBJ_FLAG_CLICKABLE);

    g_mode_label = make_label(card, "Focus Mode", &lv_font_montserrat_20, C_TEXT, 103, 110);
    lv_obj_set_style_text_align(g_mode_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_mode_label, 180);

    g_countdown_label = make_label(card, "25:00", &lv_font_montserrat_48, 0xFFFFFF, 92, 142);
    lv_obj_set_style_text_align(g_countdown_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_countdown_label, 200);

    g_tomato_label = make_label(card, "Tomato 1 / 4", &lv_font_montserrat_14, C_TEXT_DIM, 104, 204);
    lv_obj_set_style_text_align(g_tomato_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(g_tomato_label, 180);

    lv_obj_t *weather_card = lv_obj_create(card);
    lv_obj_set_size(weather_card, 336, 184);
    lv_obj_set_pos(weather_card, 366, 26);
    style_panel(weather_card, C_CARD_DARK, LV_OPA_50, 22);
    lv_obj_set_style_pad_all(weather_card, 0, 0);
    g_weather_city_label = make_label(weather_card, "Zhongshan Nanlang", &lv_font_montserrat_16, C_TEXT, 22, 20);
    lv_obj_set_width(g_weather_city_label, 154);
    lv_label_set_long_mode(g_weather_city_label, LV_LABEL_LONG_DOT);
    g_weather_temp_label = make_label(weather_card, "26 deg", &lv_font_montserrat_28, 0xFFFFFF, 22, 54);
    g_weather_detail_label = make_label(weather_card, "Cloudy / Humidity 68%", &lv_font_montserrat_16, C_TEXT_DIM, 22, 96);
    lv_obj_set_width(g_weather_detail_label, 150);
    lv_label_set_long_mode(g_weather_detail_label, LV_LABEL_LONG_DOT);
    g_weather_status_label = make_label(weather_card, "Local sample", &lv_font_montserrat_12, C_MUTED, 22, 136);
    lv_obj_set_width(g_weather_status_label, 150);
    lv_label_set_long_mode(g_weather_status_label, LV_LABEL_LONG_DOT);

    g_weather_scene = lv_obj_create(weather_card);
    lv_obj_set_size(g_weather_scene, 150, 136);
    lv_obj_set_pos(g_weather_scene, 176, 22);
    lv_obj_set_style_bg_opa(g_weather_scene, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_weather_scene, 0, 0);
    lv_obj_set_style_shadow_width(g_weather_scene, 0, 0);
    lv_obj_clear_flag(g_weather_scene, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(g_weather_scene, LV_OBJ_FLAG_CLICKABLE);
    rebuild_weather_animation(weather_kind_from_text(g_weather.text));

    lv_obj_t *rhythm_card = lv_obj_create(card);
    lv_obj_set_size(rhythm_card, 336, 60);
    lv_obj_set_pos(rhythm_card, 366, 224);
    style_panel(rhythm_card, C_CARD_DARK, LV_OPA_40, 22);
    lv_obj_set_style_pad_all(rhythm_card, 0, 0);
    make_label(rhythm_card, "Today Rhythm", &lv_font_montserrat_16, C_TEXT, 22, 8);
    g_rhythm_label = make_label(rhythm_card, "25 min focus + 5 min break", &lv_font_montserrat_14, C_TEXT_DIM, 146, 12);
    lv_obj_set_width(g_rhythm_label, 168);
    lv_label_set_long_mode(g_rhythm_label, LV_LABEL_LONG_DOT);

    lv_obj_t *bar_bg = lv_obj_create(rhythm_card);
    lv_obj_set_size(bar_bg, 120, 8);
    lv_obj_set_pos(bar_bg, 22, 38);
    lv_obj_set_style_radius(bar_bg, 5, 0);
    lv_obj_set_style_bg_color(bar_bg, lv_color_hex(0x4B1D12), 0);
    lv_obj_set_style_border_width(bar_bg, 0, 0);
    lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < DEFAULT_ROUNDS; ++i) {
        g_round_dots[i] = lv_obj_create(rhythm_card);
        lv_obj_set_size(g_round_dots[i], 14, 14);
        lv_obj_set_pos(g_round_dots[i], 24 + i * 30, 35);
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
    (void)col;
    const int tile_w = 650;
    const int tile_h = 58;
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_size(tile, tile_w, tile_h);
    lv_obj_set_pos(tile, 0, row_idx * (tile_h + 14));
    style_panel(tile, C_CARD_DARK, LV_OPA_40, 16);
    lv_obj_set_style_pad_all(tile, 0, 0);

    lv_obj_t *title = make_label(tile, name, &lv_font_montserrat_16, C_TEXT_DIM, 24, 18);
    lv_obj_set_width(title, 230);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);

    lv_obj_t *v = make_label(tile, value, &lv_font_montserrat_20, C_TEXT, 330, 17);
    lv_obj_set_width(v, 132);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);

    make_button(tile, "-", 500, 11, 48, 36, C_BUTTON_DARK, C_TEXT, minus_cb);
    make_button(tile, "+", 568, 11, 48, 36, C_HIGHLIGHT, C_CARD_DARK, plus_cb);
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
    lv_obj_set_style_pad_all(panel, 0, 0);
    make_label(panel, "Pomodoro Settings", &lv_font_montserrat_20, C_TEXT, 28, 20);
    make_button(panel, "WiFi", 598, 16, 96, 36, C_HIGHLIGHT, C_CARD_DARK, on_wifi_settings);

    g_settings_list = lv_obj_create(panel);
    lv_obj_set_size(g_settings_list, 660, 232);
    lv_obj_set_pos(g_settings_list, 36, 70);
    lv_obj_set_style_bg_opa(g_settings_list, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_settings_list, 0, 0);
    lv_obj_set_scroll_dir(g_settings_list, LV_DIR_VER);
    lv_obj_set_style_width(g_settings_list, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(g_settings_list, lv_color_hex(C_HIGHLIGHT), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(g_settings_list, LV_OPA_50, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_all(g_settings_list, 0, 0);

    snprintf(buf, sizeof(buf), "%d min", g_focus_min);
    add_setting_tile(g_settings_list, "Focus", buf, 0, 0, on_focus_minus, on_focus_plus);
    snprintf(buf, sizeof(buf), "%d min", g_short_break_min);
    add_setting_tile(g_settings_list, "Short Break", buf, 0, 1, on_break_minus, on_break_plus);
    snprintf(buf, sizeof(buf), "%d min", g_long_break_min);
    add_setting_tile(g_settings_list, "Long Break", buf, 0, 2, on_long_break_minus, on_long_break_plus);
    snprintf(buf, sizeof(buf), "%d rounds", g_rounds);
    add_setting_tile(g_settings_list, "Rounds", buf, 0, 3, on_round_minus, on_round_plus);
    snprintf(buf, sizeof(buf), "%d min", g_weather_refresh_min);
    add_setting_tile(g_settings_list, "Weather", buf, 0, 4, on_weather_minus, on_weather_plus);
    snprintf(buf, sizeof(buf), "%d%%", g_brightness_pct);
    add_setting_tile(g_settings_list, "Brightness", buf, 0, 5, on_brightness_minus, on_brightness_plus);

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

static void on_wifi_screen_click(lv_event_t *e)
{
    if (!e) return;
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_CLICKED) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev) {
            lv_point_t pt;
            lv_indev_get_point(indev, &pt);
            ESP_LOGI("WIFI_CLICK", "[TOUCH] Click detected at X=%d, Y=%d (on screen / panel)", (int)pt.x, (int)pt.y);
        }
    }
}

static lv_obj_t *make_textarea(lv_obj_t *parent, const char *placeholder,
                               lv_coord_t x, lv_coord_t y, bool password)
{
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_obj_set_size(ta, 300, 46);
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
    
    /* Center the input text vertically inside 46px height */
    lv_obj_set_style_pad_top(ta, 11, 0);
    lv_obj_set_style_pad_bottom(ta, 11, 0);
    lv_obj_set_style_pad_left(ta, 12, 0);
    
    lv_obj_add_event_cb(ta, on_wifi_textarea_focus, LV_EVENT_FOCUSED, NULL);
    lv_obj_add_event_cb(ta, on_wifi_textarea_focus, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(ta, on_wifi_textarea_changed, LV_EVENT_VALUE_CHANGED, NULL);
    return ta;
}

/* ===========================================================================
 *  CAPTIVE PORTAL - phone-based WiFi provisioning
 * =========================================================================== */

/* Minimal HTML page for WiFi config (embedded as C string) */
static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Tomato Clock WiFi</title>"
    "<style>"
    "body{font-family:sans-serif;background:#1a1a2e;color:#eee;max-width:400px;margin:40px auto;padding:20px}"
    "h2{color:#a78bfa;text-align:center}"
    "select,input{width:100%;padding:12px;margin:8px 0;border-radius:8px;border:1px solid #555;"
    "background:#16213e;color:#fff;font-size:16px;box-sizing:border-box}"
    "button{width:100%;padding:14px;background:#7c3aed;color:#fff;border:none;border-radius:8px;"
    "font-size:18px;cursor:pointer;margin-top:8px}"
    "button:active{background:#6d28d9}"
    "#st{text-align:center;margin-top:12px;color:#34d399;font-size:15px}"
    ".info{text-align:center;color:#888;font-size:13px;margin-top:16px}"
    "</style></head><body>"
    "<h2>Tomato Clock WiFi</h2>"
    "<select id='s'><option>Scanning...</option></select>"
    "<input type='password' id='p' placeholder='WiFi Password (blank=open)'>"
    "<button onclick='go()'>Connect WiFi</button>"
    "<div id='st'></div>"
    "<div class='info'>After success the hotspot will close automatically.</div>"
    "<script>"
    "fetch('/scan').then(r=>r.json()).then(a=>{"
    "let e=document.getElementById('s');"
    "e.innerHTML=a.map(n=>'<option>'+n+'</option>').join('')"
    "});"
    "function go(){"
    "let s=document.getElementById('s').value,p=document.getElementById('p').value;"
    "document.getElementById('st').innerText='Connecting...';"
    "fetch('/connect',{method:'POST',"
    "body:'ssid='+encodeURIComponent(s)+'&pass='+encodeURIComponent(p),"
    "headers:{'Content-Type':'application/x-www-form-urlencoded'}})"
    ".then(r=>r.json()).then(j=>{"
    "if(j.ok){document.getElementById('st').innerText='Success! Device reconnecting...'}"
    "else{document.getElementById('st').innerText='Failed: '+j.msg}"
    "})"
    "}"
    "</script></body></html>";

/* --- DNS hijack task: replies to ALL DNS queries with 192.168.4.1 --- */
static void captive_dns_task(void *arg)
{
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { ESP_LOGE(TAG, "DNS socket failed"); vTaskDelete(NULL); return; }

    struct sockaddr_in saddr = { .sin_family = AF_INET, .sin_port = htons(53), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(sock, (struct sockaddr *)&saddr, sizeof(saddr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "[Portal] DNS hijack listening on :53");
    uint8_t buf[512];
    struct sockaddr_in caddr;
    socklen_t clen;

    while (g_portal_active) {
        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        clen = sizeof(caddr);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&caddr, &clen);
        if (n < 12) continue;

        /* Parse QTYPE: skip DNS name starting at offset 12 */
        int qoff = 12;
        while (qoff < n) {
            uint8_t lablen = buf[qoff];
            if (lablen == 0) { qoff++; break; }
            if ((lablen & 0xC0) == 0xC0) { qoff += 2; break; }
            qoff += 1 + lablen;
        }
        if (qoff + 4 > n) continue;
        uint16_t qtype  = (buf[qoff] << 8) | buf[qoff + 1];
        uint16_t qclass = (buf[qoff + 2] << 8) | buf[qoff + 3];

        /* Only respond to A-record / IN-class queries */
        if (qtype != 1 || qclass != 1) continue;

        /* Build DNS response */
        uint8_t resp[512];
        if (n > (int)(sizeof(resp) - 16)) continue;
        memcpy(resp, buf, n);
        resp[2] = 0x81; resp[3] = 0x80;   /* QR=1, AA=1, RCODE=0 */
        resp[4] = 0x00; resp[5] = 0x01;   /* QDCOUNT = 1 */
        resp[6] = 0x00; resp[7] = 0x01;   /* ANCOUNT = 1 */
        resp[8] = 0x00; resp[9] = 0x00;   /* NSCOUNT = 0 */
        resp[10] = 0x00; resp[11] = 0x00; /* ARCOUNT = 0 */

        /* Append A record: pointer to name (0xC00C), type A, class IN, TTL=60, rdlen=4, IP=192.168.4.1 */
        int pos = n;
        resp[pos++] = 0xC0; resp[pos++] = 0x0C;
        resp[pos++] = 0x00; resp[pos++] = 0x01;   /* type A */
        resp[pos++] = 0x00; resp[pos++] = 0x01;   /* class IN */
        resp[pos++] = 0x00; resp[pos++] = 0x00; resp[pos++] = 0x00; resp[pos++] = 0x3C; /* TTL=60 */
        resp[pos++] = 0x00; resp[pos++] = 0x04;   /* rdlen=4 */
        resp[pos++] = 192; resp[pos++] = 168; resp[pos++] = 4; resp[pos++] = 1;

        sendto(sock, resp, pos, 0, (struct sockaddr *)&caddr, clen);
    }

    close(sock);
    ESP_LOGI(TAG, "[Portal] DNS task exiting");
    vTaskDelete(NULL);
}

/* --- HTTP handlers --- */
static esp_err_t portal_root_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_scan_handler(httpd_req_t *req)
{
    /* Trigger a quick scan */
    start_wifi_once();
    g_wifi_connecting_manually = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));

    wifi_scan_config_t scan_cfg = { .show_hidden = true };
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);

    char json[1024] = "[";
    int jlen = 1;

    if (err == ESP_OK) {
        uint16_t ap_count = 0;
        esp_wifi_scan_get_ap_num(&ap_count);
        if (ap_count > 16) ap_count = 16;
        wifi_ap_record_t *ap_list = calloc(ap_count, sizeof(wifi_ap_record_t));
        if (ap_list) {
            esp_wifi_scan_get_ap_records(&ap_count, ap_list);
            for (int i = 0; i < ap_count && jlen < (int)sizeof(json) - 60; i++) {
                if (i > 0) json[jlen++] = ',';
                jlen += snprintf(json + jlen, sizeof(json) - jlen, "\"%s\"", (char *)ap_list[i].ssid);
            }
            free(ap_list);
        }
    }
    json[jlen++] = ']';
    json[jlen] = '\0';

    g_wifi_connecting_manually = false;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, jlen);
}

static esp_err_t portal_connect_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No body");
        return ESP_FAIL;
    }
    body[len] = '\0';

    /* Parse ssid= and pass= from URL-encoded body */
    char ssid[33] = {0}, pass[65] = {0};
    char *sp = strstr(body, "ssid=");
    char *pp = strstr(body, "pass=");
    if (sp) {
        sp += 5;
        char *end = strchr(sp, '&');
        if (end) *end = '\0';
        /* Simple URL decode: just handle %XX for common chars */
        int j = 0;
        for (int i = 0; sp[i] && j < 32; i++) {
            if (sp[i] == '%' && sp[i+1] && sp[i+2]) {
                char hex[3] = {sp[i+1], sp[i+2], 0};
                ssid[j++] = (char)strtol(hex, NULL, 16);
                i += 2;
            } else if (sp[i] == '+') {
                ssid[j++] = ' ';
            } else {
                ssid[j++] = sp[i];
            }
        }
        if (end) *end = '&'; /* restore */
    }
    if (pp) {
        pp += 5;
        char *end = strchr(pp, '&');
        if (end) *end = '\0';
        int j = 0;
        for (int i = 0; pp[i] && j < 64; i++) {
            if (pp[i] == '%' && pp[i+1] && pp[i+2]) {
                char hex[3] = {pp[i+1], pp[i+2], 0};
                pass[j++] = (char)strtol(hex, NULL, 16);
                i += 2;
            } else if (pp[i] == '+') {
                pass[j++] = ' ';
            } else {
                pass[j++] = pp[i];
            }
        }
        if (end) *end = '&';
    }

    ESP_LOGI(TAG, "[Portal] Connect request: ssid='%s' pass_len=%d", ssid, (int)strlen(pass));

    if (!ssid[0]) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"SSID is empty\"}");
        return ESP_OK;
    }

    /* Save and connect */
    g_portal_connect_status = 1;
    wifi_set_status("Portal: connecting...");
    save_and_connect_wifi(ssid, pass);

    /* Wait up to 10 seconds for connection */
    bool connected = false;
    for (int i = 0; i < 20; i++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (g_wifi_events) {
            EventBits_t bits = xEventGroupGetBits(g_wifi_events);
            if (bits & WIFI_CONNECTED_BIT) {
                connected = true;
                break;
            }
        }
    }

    char resp[128];
    if (connected) {
        g_portal_connect_status = 2;
        snprintf(resp, sizeof(resp), "{\"ok\":true,\"msg\":\"Connected!\"}");
        wifi_set_status("Portal: WiFi connected!");
        ESP_LOGI(TAG, "[Portal] WiFi connected successfully!");
    } else {
        g_portal_connect_status = -1;
        snprintf(resp, sizeof(resp), "{\"ok\":false,\"msg\":\"Connection failed. Check password.\"}");
        wifi_set_status("Portal: connection failed");
        ESP_LOGW(TAG, "[Portal] WiFi connection failed");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, resp);

    /* If connected, schedule portal shutdown from a separate task */
    if (connected) {
        xTaskCreate(portal_shutdown_task, "portal_shutdown", 4096, NULL, 5, NULL);
    }

    return ESP_OK;
}

static esp_err_t portal_status_handler(httpd_req_t *req)
{
    char resp[64];
    snprintf(resp, sizeof(resp), "{\"status\":%d}", g_portal_connect_status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, resp);
}

/* Catch Apple/Android captive portal detection URLs and redirect to root */
static esp_err_t portal_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

/* --- Start / Stop captive portal --- */
static void start_captive_portal(void)
{
    if (g_portal_active) return;
    ESP_LOGI(TAG, "[Portal] Starting captive portal...");
    g_portal_active = true;
    g_portal_connect_status = 0;

    /* Ensure netif and event loop are initialized */
    start_wifi_once();

    /* Create AP netif if not already */
    if (!g_ap_netif) {
        g_ap_netif = esp_netif_create_default_wifi_ap();
    }

    /* Switch to APSTA mode */
    esp_wifi_set_mode(WIFI_MODE_APSTA);

    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, "TomatoClock-Setup", sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen("TomatoClock-Setup");
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.ssid_hidden = 0;
    ap_config.ap.beacon_interval = 100;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_LOGI(TAG, "[Portal] AP config set: SSID=%s, hidden=%d", ap_config.ap.ssid, ap_config.ap.ssid_hidden);

    /* Start DNS hijack task */
    xTaskCreate(captive_dns_task, "portal_dns", 4096, NULL, 5, &g_dns_task_handle);

    /* Start HTTP server */
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    config.stack_size = 8192;

    if (httpd_start(&g_portal_httpd, &config) == ESP_OK) {
        httpd_uri_t uri_root    = { .uri = "/",        .method = HTTP_GET,  .handler = portal_root_handler };
        httpd_uri_t uri_scan    = { .uri = "/scan",    .method = HTTP_GET,  .handler = portal_scan_handler };
        httpd_uri_t uri_connect = { .uri = "/connect", .method = HTTP_POST, .handler = portal_connect_handler };
        httpd_uri_t uri_status  = { .uri = "/status",  .method = HTTP_GET,  .handler = portal_status_handler };
        /* Apple captive portal detection */
        httpd_uri_t uri_apple   = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = portal_redirect_handler };
        /* Android captive portal detection */
        httpd_uri_t uri_android = { .uri = "/generate_204", .method = HTTP_GET, .handler = portal_redirect_handler };

        httpd_register_uri_handler(g_portal_httpd, &uri_root);
        httpd_register_uri_handler(g_portal_httpd, &uri_scan);
        httpd_register_uri_handler(g_portal_httpd, &uri_connect);
        httpd_register_uri_handler(g_portal_httpd, &uri_status);
        httpd_register_uri_handler(g_portal_httpd, &uri_apple);
        httpd_register_uri_handler(g_portal_httpd, &uri_android);
        ESP_LOGI(TAG, "[Portal] HTTP server started on port 80");
    } else {
        ESP_LOGE(TAG, "[Portal] Failed to start HTTP server");
    }

    /* Update screen status */
    wifi_set_status("Hotspot: TomatoClock-Setup");
}

static void portal_shutdown_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    stop_captive_portal();
    vTaskDelete(NULL);
}

static void stop_captive_portal(void)
{
    if (!g_portal_active) return;
    ESP_LOGI(TAG, "[Portal] Stopping captive portal...");
    g_portal_active = false;

    /* Stop HTTP server */
    if (g_portal_httpd) {
        httpd_stop(g_portal_httpd);
        g_portal_httpd = NULL;
    }

    /* DNS task will self-exit when g_portal_active becomes false */
    g_dns_task_handle = NULL;

    /* Switch back to STA only mode */
    esp_wifi_set_mode(WIFI_MODE_STA);

    /* Only reconnect if STA is not already connected */
    if (has_text(g_wifi_ssid)) {
        bool already_connected = false;
        if (g_wifi_events) {
            EventBits_t bits = xEventGroupGetBits(g_wifi_events);
            already_connected = (bits & WIFI_CONNECTED_BIT) != 0;
        }
        if (!already_connected) {
            ESP_LOGI(TAG, "[Portal] STA not connected, reconnecting...");
            apply_wifi_config();
            esp_wifi_connect();
        } else {
            ESP_LOGI(TAG, "[Portal] STA already connected, keeping link.");
        }
    }

    ESP_LOGI(TAG, "[Portal] Captive portal stopped.");
    wifi_set_status("Portal closed. WiFi online.");
}

static void on_phone_setup(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Phone Setup button pressed!");
    start_captive_portal();

    /* Replace WiFi page content with portal status screen */
    lv_obj_clean(g_scr);
    clear_page_refs();
    create_background(g_scr);
    create_top_bar(g_scr);
    lv_label_set_text(g_page_title, "Phone Setup");

    lv_obj_t *panel = lv_obj_create(g_scr);
    lv_obj_set_size(panel, 520, 260);
    lv_obj_set_pos(panel, 140, 116);
    style_panel(panel, C_CARD_SOFT, LV_OPA_70, 28);
    lv_obj_set_style_pad_all(panel, 0, 0);

    make_label(panel, LV_SYMBOL_WIFI "  Hotspot Active", &lv_font_montserrat_28, C_HIGHLIGHT, 110, 30);
    make_label(panel, "Connect your phone to WiFi:", &lv_font_montserrat_16, C_TEXT_DIM, 120, 80);
    make_label(panel, "TomatoClock-Setup", &lv_font_montserrat_28, C_TEXT, 90, 110);
    make_label(panel, "Then wait for the config page to appear.", &lv_font_montserrat_14, C_TEXT_DIM, 100, 160);

    g_wifi_status_label = make_label(panel, "Waiting for phone...", &lv_font_montserrat_16, C_HIGHLIGHT_2, 140, 200);

    make_button(g_scr, "Cancel", 330, 424, 140, 40, C_BUTTON_DARK, C_TEXT, on_settings);
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
    lv_obj_set_size(panel, 732, 270);
    lv_obj_set_pos(panel, 34, 92);
    style_panel(panel, C_CARD_SOFT, LV_OPA_70, 24);
    lv_obj_set_style_pad_all(panel, 0, 0);

    /* Attach touchscreen coordinate listener to both screen and panel layers */
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(panel, on_wifi_screen_click, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(g_scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_scr, on_wifi_screen_click, LV_EVENT_CLICKED, NULL);

    make_label(panel, "Nearby networks", &lv_font_montserrat_16, C_TEXT, 28, 20);
    make_button(panel, "Scan", 242, 16, 82, 34, C_HIGHLIGHT, C_CARD_DARK, on_wifi_scan);

    g_wifi_list = lv_obj_create(panel);
    lv_obj_set_size(g_wifi_list, 312, 180);
    lv_obj_set_pos(g_wifi_list, 24, 60);
    lv_obj_set_style_bg_opa(g_wifi_list, LV_OPA_0, 0);
    lv_obj_set_style_border_width(g_wifi_list, 0, 0);
    lv_obj_set_scroll_dir(g_wifi_list, LV_DIR_VER);
    lv_obj_set_style_pad_all(g_wifi_list, 0, 0);

    make_label(panel, "SSID", &lv_font_montserrat_14, C_TEXT_DIM, 382, 20);
    g_wifi_ssid_ta = make_textarea(panel, "Select or type SSID", 382, 44, false);
    lv_textarea_set_text(g_wifi_ssid_ta, has_text(g_wifi_ssid) ? g_wifi_ssid : "");

    make_label(panel, "Password", &lv_font_montserrat_14, C_TEXT_DIM, 382, 102);
    g_wifi_password_ta = make_textarea(panel, "Leave blank for open WiFi", 382, 126, true);

    g_wifi_status_label = make_label(panel, has_text(g_wifi_ssid) ? "Saved network loaded" : "No saved WiFi",
                                     &lv_font_montserrat_14, C_TEXT_DIM, 382, 184);
    lv_obj_set_width(g_wifi_status_label, 292);
    lv_label_set_long_mode(g_wifi_status_label, LV_LABEL_LONG_DOT);
    make_button(panel, "Connect", 382, 214, 100, 38, C_HIGHLIGHT, C_CARD_DARK, on_wifi_connect);
    make_button(panel, "Phone", 494, 214, 80, 38, C_BG_WARM, C_TEXT, on_phone_setup);
    make_button(panel, "Back", 586, 214, 80, 38, C_BUTTON_DARK, C_TEXT, on_settings);

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
    lv_obj_set_style_pad_all(panel, 0, 0);

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

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (has_text(g_wifi_ssid)) {
            esp_wifi_connect();
        } else {
            weather_set_local_status("WiFi setup needed");
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        uint8_t reason = disconn ? disconn->reason : 0;
        ESP_LOGW(TAG, "WiFi disconnected, reason code: %d", reason);

        if (g_wifi_events) xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT);

        char status_buf[48];
        if (reason == 201 || reason == 202 || reason == 3) {
            snprintf(status_buf, sizeof(status_buf), "WiFi failed: AP not found");
        } else if (reason == 15 || reason == 204 || reason == 23 || reason == 205) {
            snprintf(status_buf, sizeof(status_buf), "WiFi failed: Auth error");
        } else if (reason == 8) {
            snprintf(status_buf, sizeof(status_buf), "WiFi failed: Assoc timeout");
        } else {
            snprintf(status_buf, sizeof(status_buf), "WiFi disconnected (%d)", reason);
        }

        weather_set_local_status(status_buf);
        wifi_set_status(status_buf);

        if (g_wifi_connecting_manually) {
            ESP_LOGI(TAG, "Manual connection/scan active. Skip auto-reconnect.");
            return;
        }

        g_wifi_fail_count++;
        ESP_LOGI(TAG, "WiFi fail count=%d for cred[%d] '%s' (total=%d)",
                 g_wifi_fail_count, g_wifi_cred_idx, g_wifi_ssid, g_wifi_cred_count);

        /* If current network fails 3 times and we have other creds, try next */
        if (g_wifi_fail_count >= 3 && g_wifi_cred_count > 1) {
            int start_idx = g_wifi_cred_idx;
            int next_idx = -1;
            for (int i = 1; i < WIFI_CRED_MAX; i++) {
                int idx = (start_idx + i) % WIFI_CRED_MAX;
                if (g_wifi_creds[idx].ssid[0] != '\0') {
                    next_idx = idx;
                    break;
                }
            }
            if (next_idx >= 0 && next_idx != start_idx) {
                g_wifi_cred_idx = next_idx;
                g_wifi_fail_count = 0;
                strncpy(g_wifi_ssid, g_wifi_creds[next_idx].ssid, sizeof(g_wifi_ssid) - 1);
                g_wifi_ssid[sizeof(g_wifi_ssid) - 1] = '\0';
                strncpy(g_wifi_pass, g_wifi_creds[next_idx].pass, sizeof(g_wifi_pass) - 1);
                g_wifi_pass[sizeof(g_wifi_pass) - 1] = '\0';
                apply_wifi_config();
                ESP_LOGI(TAG, "Switching to next cred[%d]: %s", next_idx, g_wifi_ssid);
                wifi_set_status("Trying next network...");
            }
        }

        if (has_text(g_wifi_ssid)) {
            esp_wifi_connect();
        } else {
            weather_set_local_status("WiFi setup needed");
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        g_wifi_connecting_manually = false;
        g_wifi_fail_count = 0;
        if (g_wifi_events) xEventGroupSetBits(g_wifi_events, WIFI_CONNECTED_BIT);
        weather_set_local_status("WiFi online");
        wifi_set_status("WiFi online");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_START) {
        ESP_LOGI(TAG, "[Portal] AP started successfully (WIFI_EVENT_AP_START)");
        wifi_set_status("AP started - waiting for phone");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t *evt = (wifi_event_ap_staconnected_t *)event_data;
        ESP_LOGI(TAG, "[Portal] Phone connected: MAC=" MACSTR ", AID=%d",
                 MAC2STR(evt->mac), evt->aid);
        wifi_set_status("Phone connected!");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t *evt = (wifi_event_ap_stadisconnected_t *)event_data;
        ESP_LOGI(TAG, "[Portal] Phone disconnected: MAC=" MACSTR ", AID=%d",
                 MAC2STR(evt->mac), evt->aid);
    }
}

static void start_sntp_once(void)
{
    if (g_sntp_started) return;
    setenv("TZ", "CST-8", 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "cn.ntp.org.cn");
    esp_sntp_setservername(2, "pool.ntp.org");
    esp_sntp_init();
    g_sntp_started = true;
}

static void load_wifi_credentials_once(void)
{
    static bool loaded;
    if (loaded) return;
    loaded = true;

    /* Clear all slots */
    for (int i = 0; i < WIFI_CRED_MAX; i++) {
        g_wifi_creds[i].ssid[0] = '\0';
        g_wifi_creds[i].pass[0] = '\0';
    }
    g_wifi_cred_count = 0;
    g_wifi_cred_idx = 0;
    g_wifi_fail_count = 0;

    /* Default compile-time credentials go to slot 0 */
    if (TOMATO_WIFI_SSID[0] != '\0') {
        strncpy(g_wifi_creds[0].ssid, TOMATO_WIFI_SSID, sizeof(g_wifi_creds[0].ssid) - 1);
        g_wifi_creds[0].ssid[sizeof(g_wifi_creds[0].ssid) - 1] = '\0';
        strncpy(g_wifi_creds[0].pass, TOMATO_WIFI_PASS, sizeof(g_wifi_creds[0].pass) - 1);
        g_wifi_creds[0].pass[sizeof(g_wifi_creds[0].pass) - 1] = '\0';
        g_wifi_cred_count = 1;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open("tomato_wifi", NVS_READONLY, &nvs);
    if (err != ESP_OK) goto set_active;

    for (int i = 0; i < WIFI_CRED_MAX; i++) {
        char key_ssid[16], key_pass[16];
        snprintf(key_ssid, sizeof(key_ssid), "ssid%d", i);
        snprintf(key_pass, sizeof(key_pass), "pass%d", i);
        size_t len = sizeof(g_wifi_creds[i].ssid);
        if (nvs_get_str(nvs, key_ssid, g_wifi_creds[i].ssid, &len) != ESP_OK) {
            g_wifi_creds[i].ssid[0] = '\0';
        }
        len = sizeof(g_wifi_creds[i].pass);
        if (nvs_get_str(nvs, key_pass, g_wifi_creds[i].pass, &len) != ESP_OK) {
            g_wifi_creds[i].pass[0] = '\0';
        }
        if (g_wifi_creds[i].ssid[0] != '\0') g_wifi_cred_count++;
    }
    nvs_close(nvs);

set_active:
    /* Set current active credential to first non-empty slot */
    for (int i = 0; i < WIFI_CRED_MAX; i++) {
        if (g_wifi_creds[i].ssid[0] != '\0') {
            g_wifi_cred_idx = i;
            strncpy(g_wifi_ssid, g_wifi_creds[i].ssid, sizeof(g_wifi_ssid) - 1);
            g_wifi_ssid[sizeof(g_wifi_ssid) - 1] = '\0';
            strncpy(g_wifi_pass, g_wifi_creds[i].pass, sizeof(g_wifi_pass) - 1);
            g_wifi_pass[sizeof(g_wifi_pass) - 1] = '\0';
            break;
        }
    }
}

static bool save_wifi_credentials(const char *ssid, const char *pass)
{
    if (!ssid || !ssid[0]) return false;

    int slot = -1;
    /* Find existing slot with same SSID */
    for (int i = 0; i < WIFI_CRED_MAX; i++) {
        if (strcmp(g_wifi_creds[i].ssid, ssid) == 0) {
            slot = i;
            break;
        }
        if (slot < 0 && g_wifi_creds[i].ssid[0] == '\0') {
            slot = i;
        }
    }
    if (slot < 0) slot = WIFI_CRED_MAX - 1; /* Overwrite last if full */

    /* Update memory */
    strncpy(g_wifi_creds[slot].ssid, ssid, sizeof(g_wifi_creds[slot].ssid) - 1);
    g_wifi_creds[slot].ssid[sizeof(g_wifi_creds[slot].ssid) - 1] = '\0';
    strncpy(g_wifi_creds[slot].pass, pass ? pass : "", sizeof(g_wifi_creds[slot].pass) - 1);
    g_wifi_creds[slot].pass[sizeof(g_wifi_creds[slot].pass) - 1] = '\0';

    /* Update active credential */
    g_wifi_cred_idx = slot;
    g_wifi_fail_count = 0;
    strncpy(g_wifi_ssid, ssid, sizeof(g_wifi_ssid) - 1);
    g_wifi_ssid[sizeof(g_wifi_ssid) - 1] = '\0';
    strncpy(g_wifi_pass, pass ? pass : "", sizeof(g_wifi_pass) - 1);
    g_wifi_pass[sizeof(g_wifi_pass) - 1] = '\0';

    /* Recount non-empty slots */
    g_wifi_cred_count = 0;
    for (int i = 0; i < WIFI_CRED_MAX; i++) {
        if (g_wifi_creds[i].ssid[0] != '\0') g_wifi_cred_count++;
    }

    /* Write all slots to NVS */
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("tomato_wifi", NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return false;
    }

    for (int i = 0; i < WIFI_CRED_MAX; i++) {
        char key_ssid[16], key_pass[16];
        snprintf(key_ssid, sizeof(key_ssid), "ssid%d", i);
        snprintf(key_pass, sizeof(key_pass), "pass%d", i);
        err = nvs_set_str(nvs, key_ssid, g_wifi_creds[i].ssid);
        if (err == ESP_OK) err = nvs_set_str(nvs, key_pass, g_wifi_creds[i].pass);
        if (err != ESP_OK) break;
    }
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save wifi failed: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Saved WiFi cred[%d]: SSID=%s (total=%d)", slot, ssid, g_wifi_cred_count);
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

    g_wifi_connecting_manually = true;
    wifi_set_status("Saved. Connecting");
    start_wifi_once();
    if (g_net_started) {
        if (g_wifi_events) xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT);
        esp_wifi_disconnect();
        esp_err_t err = apply_wifi_config();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "apply wifi failed: %s", esp_err_to_name(err));
            wifi_set_status("WiFi config failed");
            g_wifi_connecting_manually = false;
            return;
        }
        esp_wifi_connect();
    }
}

static void start_wifi_once(void)
{
    load_wifi_credentials_once();
    if (g_net_started) return;

    ESP_LOGI(TAG, "Initializing Wi-Fi system...");
    wifi_set_status("Initializing Netif...");
    g_wifi_events = xEventGroupCreate();

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (has_text(g_wifi_ssid)) {
        ESP_LOGI(TAG, "Loading saved network: SSID=%s", g_wifi_ssid);
        ESP_ERROR_CHECK(apply_wifi_config());
    } else {
        weather_set_local_status("WiFi setup needed");
    }

    ESP_LOGI(TAG, "Starting Wi-Fi hardware driver...");
    wifi_set_status("Starting WiFi...");
    ESP_ERROR_CHECK(esp_wifi_start());
    
    start_sntp_once();
    g_net_started = true;
    ESP_LOGI(TAG, "Wi-Fi started successfully.");
}

static void wifi_scan_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "Starting WiFi scan task...");
    wifi_set_status("Initializing WiFi...");

    start_wifi_once();

    g_wifi_scan_busy = true;
    g_wifi_scan_dirty = true;

    g_wifi_connecting_manually = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(150));

    wifi_set_status("Scanning networks...");
    ESP_LOGI(TAG, "Calling esp_wifi_scan_start...");

    wifi_scan_config_t scan_config = {
        .show_hidden = true,
    };
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    g_wifi_connecting_manually = false;

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi scan failed: %s", esp_err_to_name(err));
        char err_buf[48];
        snprintf(err_buf, sizeof(err_buf), "Scan failed (%s)", esp_err_to_name(err));
        wifi_set_status(err_buf);

        g_wifi_scan_count = 0;
        g_wifi_scan_busy = false;
        g_wifi_scan_dirty = true;

        if (has_text(g_wifi_ssid)) esp_wifi_connect();
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "WiFi scan completed successfully!");
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

    char status_buf[32];
    snprintf(status_buf, sizeof(status_buf), "Found %d networks", copied);
    wifi_set_status(status_buf);

    ESP_LOGI(TAG, "Loaded %d AP records.", copied);

    /* If we have multiple saved credentials, pick the best known network */
    if (g_wifi_cred_count > 1 && copied > 0) {
        int best_idx = -1;
        int8_t best_rssi = -128;
        for (int c = 0; c < WIFI_CRED_MAX; c++) {
            if (g_wifi_creds[c].ssid[0] == '\0') continue;
            for (int s = 0; s < copied; s++) {
                if (strcmp(g_wifi_creds[c].ssid, g_wifi_scan_results[s].ssid) == 0) {
                    if (g_wifi_scan_results[s].rssi > best_rssi) {
                        best_rssi = g_wifi_scan_results[s].rssi;
                        best_idx = c;
                    }
                    break;
                }
            }
        }
        if (best_idx >= 0 && best_idx != g_wifi_cred_idx) {
            g_wifi_cred_idx = best_idx;
            g_wifi_fail_count = 0;
            strncpy(g_wifi_ssid, g_wifi_creds[best_idx].ssid, sizeof(g_wifi_ssid) - 1);
            g_wifi_ssid[sizeof(g_wifi_ssid) - 1] = '\0';
            strncpy(g_wifi_pass, g_wifi_creds[best_idx].pass, sizeof(g_wifi_pass) - 1);
            g_wifi_pass[sizeof(g_wifi_pass) - 1] = '\0';
            apply_wifi_config();
            ESP_LOGI(TAG, "Auto-picked best network: %s (RSSI %d)", g_wifi_ssid, (int)best_rssi);
        }
    }

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

static const char *weather_from_wmo_code(int code)
{
    if (code == 0) return "Sunny";
    if (code == 1 || code == 2 || code == 3) return "Cloudy";
    if (code == 45 || code == 48) return "Foggy";
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return "Rainy";
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return "Snowy";
    if (code >= 95 && code <= 99) return "Stormy";
    return "Cloudy";
}

static bool fetch_weather_once(void)
{
    http_capture_t cap = {0};
    const char *url = "http://api.open-meteo.com/v1/forecast?latitude=22.49&longitude=113.53&current=temperature_2m,relative_humidity_2m,weather_code";

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .user_data = &cap,
        .timeout_ms = 8000,
        .buffer_size = 1024,
    };

    ESP_LOGI(TAG, "Weather sync start");
    weather_set_local_status("Weather syncing");

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        weather_set_local_status("HTTP init failed");
        return false;
    }

    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0");
    esp_http_client_set_header(client, "Referer", "https://wis.qq.com/");

    esp_err_t err = esp_http_client_perform(client);
    int http_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || http_code != 200) {
        ESP_LOGW(TAG, "weather request failed: err=%s code=%d body=%s", esp_err_to_name(err), http_code, cap.data);
        weather_set_local_status("Weather offline");
        return false;
    }

    cJSON *root = cJSON_Parse(cap.data);
    if (!root) {
        weather_set_local_status("Weather parse failed");
        return false;
    }

    cJSON *current = cJSON_GetObjectItem(root, "current");
    if (!cJSON_IsObject(current)) {
        cJSON_Delete(root);
        weather_set_local_status("Weather data empty");
        return false;
    }

    cJSON *degree = cJSON_GetObjectItem(current, "temperature_2m");
    cJSON *humidity = cJSON_GetObjectItem(current, "relative_humidity_2m");
    cJSON *weather_code = cJSON_GetObjectItem(current, "weather_code");

    if (!cJSON_IsNumber(degree) || !cJSON_IsNumber(humidity) || !cJSON_IsNumber(weather_code)) {
        cJSON_Delete(root);
        weather_set_local_status("Weather value empty");
        return false;
    }

    if (xSemaphoreTake(g_weather_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        strncpy(g_weather.city, "Zhongshan", sizeof(g_weather.city) - 1);
        snprintf(g_weather.temp, sizeof(g_weather.temp), "%.0f", degree->valuedouble);
        snprintf(g_weather.humidity, sizeof(g_weather.humidity), "%.0f", humidity->valuedouble);

        const char *desc = weather_from_wmo_code(weather_code->valueint);
        strncpy(g_weather.text, desc, sizeof(g_weather.text) - 1);
        
        strncpy(g_weather.status, "Weather updated", sizeof(g_weather.status) - 1);
        g_weather.online = true;
        g_weather_dirty = true;
        xSemaphoreGive(g_weather_mutex);
        ESP_LOGI(TAG, "Weather sync success! Temp=%s Hum=%s Text=%s", g_weather.temp, g_weather.humidity, g_weather.text);
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

        bool ok = fetch_weather_once();
        vTaskDelay(pdMS_TO_TICKS((ok ? g_weather_refresh_min * 60 : 60) * 1000));
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
