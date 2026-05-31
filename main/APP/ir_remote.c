/**
 ****************************************************************************************************
 * @file        ir_remote.c
 * @brief       Universal IR Remote App for DNESP32S3
 *              - 4 devices x 8 keys per device
 *              - Raw symbol learning & replay via RMT
 *              - NVS persistent storage
 ****************************************************************************************************
 */

#include "ir_remote.h"
#include "menu.h"
#include "remote_ir.h"
#include "lvgl.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "ir_remote";

#define IR_MAX_DEVICES      4
#define IR_KEYS_PER_DEVICE  8
/* IR_MAX_SYMBOLS defined in remote_ir.h (256) */
#define IR_KEY_NAME_LEN     20
#define IR_DEV_NAME_LEN     20
#define IR_NVS_NAMESPACE    "ir_remote"
#define IR_NVS_KEY_DB       "ir_db"

typedef struct {
    char name[IR_KEY_NAME_LEN];
    uint16_t num_symbols;
    rmt_symbol_word_t symbols[IR_MAX_SYMBOLS];
} ir_key_t;

typedef struct {
    char name[IR_DEV_NAME_LEN];
    uint8_t num_keys;
    ir_key_t keys[IR_KEYS_PER_DEVICE];
} ir_device_t;

typedef struct {
    uint8_t num_devices;
    ir_device_t devices[IR_MAX_DEVICES];
} ir_database_t;

static ir_database_t s_db;
static lv_obj_t *g_ir_scr = NULL;
static int g_current_dev = 0;
static bool g_learn_mode = false;
static lv_obj_t *g_key_btns[IR_KEYS_PER_DEVICE] = {NULL};
static lv_obj_t *g_status_label = NULL;
static lv_obj_t *g_mode_switch = NULL;
static lv_obj_t *g_mode_label = NULL;
static lv_timer_t *g_learn_timer = NULL;

/* Default names */
static const char *s_default_dev_names[IR_MAX_DEVICES] = {
    "TV", "AirCon", "Box", "Custom"
};
static const char *s_default_key_names[IR_KEYS_PER_DEVICE] = {
    "Power", "Vol+", "Vol-", "CH+", "CH-", "Menu", "OK", "Back"
};

/* NVS persistence */
static void ir_db_load(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(IR_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed, using defaults");
        goto use_defaults;
    }

    size_t len = sizeof(s_db);
    ret = nvs_get_blob(handle, IR_NVS_KEY_DB, &s_db, &len);
    nvs_close(handle);

    if (ret != ESP_OK || len != sizeof(s_db) || s_db.num_devices == 0) {
        ESP_LOGW(TAG, "NVS read failed or empty, using defaults");
        goto use_defaults;
    }
    ESP_LOGI(TAG, "Loaded IR DB from NVS");
    return;

use_defaults:
    memset(&s_db, 0, sizeof(s_db));
    s_db.num_devices = IR_MAX_DEVICES;
    for (int d = 0; d < IR_MAX_DEVICES; d++) {
        strncpy(s_db.devices[d].name, s_default_dev_names[d], IR_DEV_NAME_LEN - 1);
        s_db.devices[d].num_keys = IR_KEYS_PER_DEVICE;
        for (int k = 0; k < IR_KEYS_PER_DEVICE; k++) {
            strncpy(s_db.devices[d].keys[k].name, s_default_key_names[k], IR_KEY_NAME_LEN - 1);
            s_db.devices[d].keys[k].num_symbols = 0;
        }
    }
}

static void ir_db_save(void)
{
    nvs_handle_t handle;
    esp_err_t ret = nvs_open(IR_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS open RW failed");
        return;
    }
    ret = nvs_set_blob(handle, IR_NVS_KEY_DB, &s_db, sizeof(s_db));
    if (ret == ESP_OK) {
        nvs_commit(handle);
        ESP_LOGI(TAG, "IR DB saved to NVS");
    } else {
        ESP_LOGE(TAG, "NVS set_blob failed: %s", esp_err_to_name(ret));
    }
    nvs_close(handle);
}

/* Forward declarations */
static void show_device_list(void);
static void show_key_panel(int dev_idx);

/* Helper: create a styled button with label */
static lv_obj_t *create_key_btn(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                                 lv_color_t bg_color, lv_event_cb_t cb, void *user_data)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, bg_color, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(lbl);

    /* Ensure label doesn't steal touches (LVGL v8 bug workaround) */
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

    return btn;
}

/* Global learn result for polling */
static volatile bool s_learn_done = false;
static volatile bool s_learn_success = false;
static volatile uint16_t s_learn_symbols = 0;

static void ir_remote_set_learn_result(bool success, uint16_t num_symbols)
{
    s_learn_success = success;
    s_learn_symbols = num_symbols;
    s_learn_done = true;
}

/* Background task for learning (non-blocking) */
typedef struct {
    int dev_idx;
    int key_idx;
} learn_task_param_t;

static void learn_task(void *param)
{
    learn_task_param_t *p = (learn_task_param_t *)param;
    int dev_idx = p->dev_idx;
    int key_idx = p->key_idx;
    free(p);

    rmt_symbol_word_t symbols[IR_MAX_SYMBOLS];
    size_t num_symbols = 0;

    ESP_LOGI(TAG, "Learning key dev=%d key=%d", dev_idx, key_idx);

    bool ok = remote_ir_learn(symbols, &num_symbols, 8000); /* 8s timeout */

    if (ok && num_symbols > 0) {
        s_db.devices[dev_idx].keys[key_idx].num_symbols = (uint16_t)num_symbols;
        memcpy(s_db.devices[dev_idx].keys[key_idx].symbols, symbols,
               num_symbols * sizeof(rmt_symbol_word_t));
        ir_db_save();
        ESP_LOGI(TAG, "Learned %d symbols", (int)num_symbols);
    } else {
        ESP_LOGW(TAG, "Learn timeout or no signal");
    }

    /* Post result to UI (use lvgl direct call since we're in a task) */
    /* In LVGL v8, we can use lv_async_call if needed, but simpler: set a flag and update on next event */
    /* Actually we'll just update directly if LVGL lock is available, or use a simpler approach */
    /* For simplicity, we update UI state variables and rely on the status label refresh */
    /* But we need a way to signal the UI... let's use a simple global flag */
    /* Better: post an event to the screen */

    /* Simple approach: schedule a callback via lv_timer */
    /* Actually the simplest is to just update the status label if screen still exists */
    /* But we must ensure thread safety with LVGL */

    /* We'll use a message queue approach via lv_event_send? No, that's not thread safe either. */
    /* In this simple app, let's just update a global result and show it in the next UI refresh. */
    /* Actually, for 8s timeout it's fine to block the UI and show a modal. But the requirement was non-blocking? */

    /* Re-design: the learn button will show a modal that blocks interaction, and we run learn_task. */
    /* The task will call a UI update function when done. */

    /* For thread-safe LVGL update, we need a mutex or lv_timer. */
    /* In this project, LVGL is likely running on a single task. Let's check how lvgl_demo works. */

    /* Simpler approach: don't use a FreeRTOS task. Instead, use a non-blocking learn with a lv_timer poll. */
    /* But remote_ir_learn is blocking... */

    /* Let me redesign: just block in a task, and when done, set a global variable. */
    /* The main UI has a timer that checks this variable. */

    /* Actually, I realize I should redesign this part. Let me think again... */
    /* I'll add a global learn result struct and a timer-based poller in the UI. */

    /* For now, just save the result for polling */
    ir_remote_set_learn_result(ok && num_symbols > 0, (uint16_t)num_symbols);

    vTaskDelete(NULL);
}

static void check_learn_result(lv_timer_t *timer)
{
    if (!s_learn_done) return;
    s_learn_done = false;

    if (g_status_label) {
        if (s_learn_success) {
            lv_label_set_text_fmt(g_status_label, "Learn OK!  (%d symbols)", s_learn_symbols);
            lv_obj_set_style_text_color(g_status_label, lv_color_hex(0x00FF00), 0);
        } else {
            lv_label_set_text(g_status_label, "Learn failed: no signal");
            lv_obj_set_style_text_color(g_status_label, lv_color_hex(0xFF0000), 0);
        }
    }
    /* Re-enable mode switch */
    if (g_mode_switch) lv_obj_clear_state(g_mode_switch, LV_STATE_DISABLED);
}

/* Key button click handler */
static void key_btn_cb(lv_event_t *e)
{
    int key_idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (key_idx < 0 || key_idx >= IR_KEYS_PER_DEVICE) return;

    ir_device_t *dev = &s_db.devices[g_current_dev];
    ir_key_t *key = &dev->keys[key_idx];

    if (g_learn_mode) {
        /* Start learning */
        if (g_status_label) {
            lv_label_set_text(g_status_label, "Learning... Press remote now");
            lv_obj_set_style_text_color(g_status_label, lv_color_hex(0xFFFF00), 0);
        }
        if (g_mode_switch) lv_obj_add_state(g_mode_switch, LV_STATE_DISABLED);

        learn_task_param_t *param = malloc(sizeof(learn_task_param_t));
        param->dev_idx = g_current_dev;
        param->key_idx = key_idx;
        xTaskCreate(learn_task, "learn_task", 8192, param, 5, NULL);
    } else {
        /* Transmit */
        if (key->num_symbols == 0) {
            if (g_status_label) {
                lv_label_set_text(g_status_label, "Key not learned yet");
                lv_obj_set_style_text_color(g_status_label, lv_color_hex(0xFF8800), 0);
            }
            return;
        }
        bool ok = remote_ir_send(key->symbols, key->num_symbols);
        if (g_status_label) {
            if (ok) {
                lv_label_set_text_fmt(g_status_label, "TX OK  (%d symbols)", key->num_symbols);
                lv_obj_set_style_text_color(g_status_label, lv_color_hex(0x00FF00), 0);
            } else {
                lv_label_set_text(g_status_label, "TX failed (HW conflict?)");
                lv_obj_set_style_text_color(g_status_label, lv_color_hex(0xFF0000), 0);
            }
        }
    }
}

/* Mode switch handler */
static void mode_switch_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    g_learn_mode = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (g_mode_label) {
        lv_label_set_text(g_mode_label, g_learn_mode ? "LEARN" : "TX");
    }
    /* Update button colors */
    for (int i = 0; i < IR_KEYS_PER_DEVICE; i++) {
        if (g_key_btns[i]) {
            lv_color_t color = g_learn_mode ? lv_color_hex(0xE67E22) : lv_color_hex(0x0F3460);
            lv_obj_set_style_bg_color(g_key_btns[i], color, 0);
        }
    }
    if (g_status_label) {
        lv_label_set_text(g_status_label, g_learn_mode ? "Tap key to learn" : "Tap key to transmit");
        lv_obj_set_style_text_color(g_status_label, lv_color_hex(0xECF0F1), 0);
    }
}

/* Back to device list */
static void back_to_devices_cb(lv_event_t *e)
{
    (void)e;
    show_device_list();
}

/* Back to menu */
static void back_to_menu_cb(lv_event_t *e)
{
    (void)e;
    g_ir_scr = NULL;        /* prevent dangling pointer */
    g_status_label = NULL;
    g_mode_switch = NULL;
    g_mode_label = NULL;
    memset(g_key_btns, 0, sizeof(g_key_btns));
    if (g_learn_timer) {
        lv_timer_del(g_learn_timer);
        g_learn_timer = NULL;
    }
    menu_go_back();
}

/* Device selection handler */
static void dev_btn_cb(lv_event_t *e)
{
    int dev_idx = (int)(intptr_t)lv_event_get_user_data(e);
    g_current_dev = dev_idx;
    show_key_panel(dev_idx);
}

/* Show key panel for a device */
static void show_key_panel(int dev_idx)
{
    lv_obj_t *old_scr = g_ir_scr;
    g_ir_scr = lv_obj_create(NULL);
    lv_scr_load(g_ir_scr);
    lv_obj_set_style_bg_color(g_ir_scr, lv_color_hex(0x1A1A2E), 0);
    if (old_scr) {
        lv_obj_del_async(old_scr);
    }
    /* Clean up key panel globals when leaving it */
    g_status_label = NULL;
    g_mode_switch = NULL;
    g_mode_label = NULL;
    memset(g_key_btns, 0, sizeof(g_key_btns));
    if (g_learn_timer) {
        lv_timer_del(g_learn_timer);
        g_learn_timer = NULL;
    }

    ir_device_t *dev = &s_db.devices[dev_idx];

    /* Top bar */
    lv_obj_t *top = lv_obj_create(g_ir_scr);
    lv_obj_set_size(top, 800, 60);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x16213E), 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = lv_btn_create(top);
    lv_obj_set_size(back_btn, 70, 40);
    lv_obj_set_pos(back_btn, 10, 10);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xE94560), 0);
    lv_obj_add_event_cb(back_btn, back_to_devices_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_lbl);
    lv_obj_clear_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text_fmt(title, "%s", dev->name);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xECF0F1), 0);
    lv_obj_center(title);

    /* Mode switch */
    g_mode_switch = lv_switch_create(top);
    lv_obj_set_size(g_mode_switch, 50, 26);
    lv_obj_set_pos(g_mode_switch, 680, 17);
    lv_obj_add_event_cb(g_mode_switch, mode_switch_cb, LV_EVENT_VALUE_CHANGED, NULL);
    if (g_learn_mode) {
        lv_obj_add_state(g_mode_switch, LV_STATE_CHECKED);
    }

    g_mode_label = lv_label_create(top);
    lv_label_set_text(g_mode_label, g_learn_mode ? "LEARN" : "TX");
    lv_obj_set_style_text_font(g_mode_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_mode_label, lv_color_hex(0xECF0F1), 0);
    lv_obj_set_pos(g_mode_label, 735, 22);

    /* Key grid: 4 cols x 2 rows */
    int btn_w = 170;
    int btn_h = 130;
    int gap_x = 24;
    int gap_y = 30;
    int start_x = 35;
    int start_y = 80;

    memset(g_key_btns, 0, sizeof(g_key_btns));

    for (int i = 0; i < IR_KEYS_PER_DEVICE; i++) {
        int col = i % 4;
        int row = i / 4;
        int x = start_x + col * (btn_w + gap_x);
        int y = start_y + row * (btn_h + gap_y);

        ir_key_t *key = &dev->keys[i];
        lv_color_t bg = g_learn_mode ? lv_color_hex(0xE67E22) : lv_color_hex(0x0F3460);
        if (!g_learn_mode && key->num_symbols == 0) {
            bg = lv_color_hex(0x333333); /* dim if not learned */
        }

        g_key_btns[i] = create_key_btn(g_ir_scr, key->name, x, y, btn_w, btn_h,
                                       bg, key_btn_cb, (void *)(intptr_t)i);
    }

    /* Status label */
    g_status_label = lv_label_create(g_ir_scr);
    lv_obj_set_pos(g_status_label, 35, 420);
    lv_obj_set_size(g_status_label, 730, 30);
    lv_label_set_text(g_status_label, g_learn_mode ? "Tap key to learn" : "Tap key to transmit");
    lv_obj_set_style_text_font(g_status_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_status_label, lv_color_hex(0xECF0F1), 0);

    /* Poll timer for learn result (reuse or create) */
    if (g_learn_timer) {
        lv_timer_reset(g_learn_timer);
    } else {
        g_learn_timer = lv_timer_create(check_learn_result, 200, NULL);
    }
}

/* Show device list */
static void show_device_list(void)
{
    lv_obj_t *old_scr = g_ir_scr;
    g_ir_scr = lv_obj_create(NULL);
    lv_scr_load(g_ir_scr);
    lv_obj_set_style_bg_color(g_ir_scr, lv_color_hex(0x1A1A2E), 0);
    if (old_scr) {
        lv_obj_del_async(old_scr);
    }

    /* Top bar */
    lv_obj_t *top = lv_obj_create(g_ir_scr);
    lv_obj_set_size(top, 800, 60);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_style_bg_color(top, lv_color_hex(0x16213E), 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_btn = lv_btn_create(top);
    lv_obj_set_size(back_btn, 70, 40);
    lv_obj_set_pos(back_btn, 10, 10);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xE94560), 0);
    lv_obj_add_event_cb(back_btn, back_to_menu_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_lbl);
    lv_obj_clear_flag(back_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(top);
    lv_label_set_text(title, LV_SYMBOL_CHARGE "  Universal IR Remote");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xECF0F1), 0);
    lv_obj_center(title);

    /* Device cards: 2 cols x 2 rows */
    int card_w = 340;
    int card_h = 160;
    int gap_x = 40;
    int gap_y = 30;
    int start_x = 40;
    int start_y = 90;
    lv_color_t colors[4] = {
        lv_color_hex(0x0F3460),
        lv_color_hex(0x145A32),
        lv_color_hex(0x6C3483),
        lv_color_hex(0x7B241C),
    };

    for (int i = 0; i < IR_MAX_DEVICES; i++) {
        int col = i % 2;
        int row = i / 2;
        int x = start_x + col * (card_w + gap_x);
        int y = start_y + row * (card_h + gap_y);

        lv_obj_t *card = lv_obj_create(g_ir_scr);
        lv_obj_set_size(card, card_w, card_h);
        lv_obj_set_pos(card, x, y);
        lv_obj_set_style_bg_color(card, colors[i], 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, s_db.devices[i].name);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        /* Transparent overlay for touch */
        lv_obj_t *touch = lv_obj_create(card);
        lv_obj_set_size(touch, card_w, card_h);
        lv_obj_set_pos(touch, 0, 0);
        lv_obj_set_style_bg_opa(touch, LV_OPA_0, 0);
        lv_obj_set_style_border_width(touch, 0, 0);
        lv_obj_clear_flag(touch, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(touch, dev_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }

    /* Footer hint */
    lv_obj_t *foot = lv_label_create(g_ir_scr);
    lv_label_set_text(foot, "Tap device to control  |  TX/Learn dual mode");
    lv_obj_set_style_text_font(foot, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(foot, lv_color_hex(0x888888), 0);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, -20);
}

void ir_remote_start(void)
{
    /* Ensure IR driver is ready */
    if (!remote_ir_init()) {
        /* Show error on screen if RMT init failed */
        lv_obj_t *err = lv_label_create(g_ir_scr);
        lv_label_set_text(err, "IR init failed!\nGPIO8 may conflict with LCD.");
        lv_obj_set_style_text_color(err, lv_color_hex(0xFF0000), 0);
        lv_obj_center(err);
        return;
    }
    ir_db_load();
    g_learn_mode = false;
    show_device_list();
}
