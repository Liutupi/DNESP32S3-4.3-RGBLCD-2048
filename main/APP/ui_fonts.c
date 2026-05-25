#include "ui_fonts.h"

#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "spi.h"
#include "spi_sdcard.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#define UI_FONT_FS_LETTER 'S'
#define UI_FONT_SD_PREFIX "/sdcard"

static const char *TAG = "UI_FONTS";

const lv_font_t *UI_FONT_CN_16 = &lv_font_montserrat_16;
const lv_font_t *UI_FONT_CN_20 = &lv_font_montserrat_20;
const lv_font_t *UI_FONT_CN_24 = &lv_font_montserrat_24;
const lv_font_t *UI_FONT_CN_32 = &lv_font_montserrat_32;
const lv_font_t *UI_FONT_DIGIT_48 = &lv_font_montserrat_48;

static lv_font_t *s_font_cn_16;
static lv_font_t *s_font_cn_20;
static lv_font_t *s_font_cn_24;
static lv_font_t *s_font_cn_32;
static bool s_initialized;
static bool s_fs_registered;
static lv_fs_drv_t s_sd_fs_drv;

static const lv_font_t *font_fallback_for_size(uint16_t size)
{
    switch (size) {
        case 16: return &lv_font_montserrat_16;
        case 20: return &lv_font_montserrat_20;
        case 24: return &lv_font_montserrat_24;
        case 32: return &lv_font_montserrat_32;
        default: return &lv_font_montserrat_14;
    }
}

static void build_sd_path(const char *path, char *out, size_t out_size)
{
    while (path && *path == '/') {
        path++;
    }
    snprintf(out, out_size, "%s/%s", UI_FONT_SD_PREFIX, path ? path : "");
}

static void *sd_fs_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    (void)drv;
    if ((mode & LV_FS_MODE_RD) == 0 || (mode & LV_FS_MODE_WR) != 0) {
        return NULL;
    }

    char full_path[LV_FS_MAX_PATH_LENGTH];
    build_sd_path(path, full_path, sizeof(full_path));
    return fopen(full_path, "rb");
}

static lv_fs_res_t sd_fs_close_cb(lv_fs_drv_t *drv, void *file_p)
{
    (void)drv;
    if (!file_p) {
        return LV_FS_RES_INV_PARAM;
    }
    return fclose((FILE *)file_p) == 0 ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t sd_fs_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf,
                                 uint32_t btr, uint32_t *br)
{
    (void)drv;
    if (!file_p || !buf) {
        return LV_FS_RES_INV_PARAM;
    }

    size_t read_len = fread(buf, 1, btr, (FILE *)file_p);
    if (br) {
        *br = (uint32_t)read_len;
    }
    if (read_len < btr && ferror((FILE *)file_p)) {
        return LV_FS_RES_FS_ERR;
    }
    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_fs_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos,
                                 lv_fs_whence_t whence)
{
    (void)drv;
    if (!file_p) {
        return LV_FS_RES_INV_PARAM;
    }

    int origin = SEEK_SET;
    if (whence == LV_FS_SEEK_CUR) {
        origin = SEEK_CUR;
    } else if (whence == LV_FS_SEEK_END) {
        origin = SEEK_END;
    }

    return fseek((FILE *)file_p, (long)pos, origin) == 0 ? LV_FS_RES_OK : LV_FS_RES_FS_ERR;
}

static lv_fs_res_t sd_fs_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos_p)
{
    (void)drv;
    if (!file_p || !pos_p) {
        return LV_FS_RES_INV_PARAM;
    }

    long pos = ftell((FILE *)file_p);
    if (pos < 0) {
        return LV_FS_RES_FS_ERR;
    }

    *pos_p = (uint32_t)pos;
    return LV_FS_RES_OK;
}

static void register_sd_font_fs(void)
{
    if (s_fs_registered) {
        return;
    }

    lv_fs_drv_init(&s_sd_fs_drv);
    s_sd_fs_drv.letter = UI_FONT_FS_LETTER;
    s_sd_fs_drv.open_cb = sd_fs_open_cb;
    s_sd_fs_drv.close_cb = sd_fs_close_cb;
    s_sd_fs_drv.read_cb = sd_fs_read_cb;
    s_sd_fs_drv.seek_cb = sd_fs_seek_cb;
    s_sd_fs_drv.tell_cb = sd_fs_tell_cb;
    lv_fs_drv_register(&s_sd_fs_drv);
    s_fs_registered = true;
}

static const lv_font_t *load_font_or_fallback(const char *path, uint16_t size,
                                              lv_font_t **loaded_slot)
{
    const lv_font_t *fallback = font_fallback_for_size(size);
    lv_font_t *font = lv_font_load(path);
    if (!font) {
        ESP_LOGW(TAG, "Font load failed: %s, fallback to Montserrat %u", path, size);
        return fallback;
    }

    ESP_LOGI(TAG, "Loaded font: %s", path);
    *loaded_slot = font;
    return font;
}

void ui_fonts_init(void)
{
    if (s_initialized) {
        return;
    }
    s_initialized = true;

    UI_FONT_CN_16 = &lv_font_montserrat_16;
    UI_FONT_CN_20 = &lv_font_montserrat_20;
    UI_FONT_CN_24 = &lv_font_montserrat_24;
    UI_FONT_CN_32 = &lv_font_montserrat_32;
    UI_FONT_DIGIT_48 = &lv_font_montserrat_48;

    register_sd_font_fs();

    spi2_init();
    esp_err_t err = sd_spi_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD card mount failed: %s, using built-in fallback fonts", esp_err_to_name(err));
        return;
    }

    UI_FONT_CN_16 = load_font_or_fallback("S:/fonts/lxgw_wenkai_16.bin", 16, &s_font_cn_16);
    vTaskDelay(pdMS_TO_TICKS(1));
    UI_FONT_CN_20 = load_font_or_fallback("S:/fonts/lxgw_wenkai_20.bin", 20, &s_font_cn_20);
    vTaskDelay(pdMS_TO_TICKS(1));
    UI_FONT_CN_24 = load_font_or_fallback("S:/fonts/lxgw_wenkai_24.bin", 24, &s_font_cn_24);
    vTaskDelay(pdMS_TO_TICKS(1));
    UI_FONT_CN_32 = &lv_font_montserrat_32;
}

void ui_fonts_deinit(void)
{
    if (s_font_cn_16) {
        lv_font_free(s_font_cn_16);
        s_font_cn_16 = NULL;
    }
    if (s_font_cn_20) {
        lv_font_free(s_font_cn_20);
        s_font_cn_20 = NULL;
    }
    if (s_font_cn_24) {
        lv_font_free(s_font_cn_24);
        s_font_cn_24 = NULL;
    }
    if (s_font_cn_32) {
        lv_font_free(s_font_cn_32);
        s_font_cn_32 = NULL;
    }

    UI_FONT_CN_16 = &lv_font_montserrat_16;
    UI_FONT_CN_20 = &lv_font_montserrat_20;
    UI_FONT_CN_24 = &lv_font_montserrat_24;
    UI_FONT_CN_32 = &lv_font_montserrat_32;
    UI_FONT_DIGIT_48 = &lv_font_montserrat_48;
    s_initialized = false;
}
