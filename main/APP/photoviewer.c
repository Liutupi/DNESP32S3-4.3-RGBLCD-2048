/**
 ****************************************************************************************************
 * @file        photoviewer.c
 * @brief       Photo slideshow viewer for DNESP32S3 (4.3" RGB LCD 800x480, LVGL v8)
 *
 *  SD卡：基于正点原子官方 spi_sdcard 驱动（SPI2，CS=IO2）
 *  JPEG解码：TJpgDec (JD_FORMAT=1, 输出 RGB565)
 *  显示：LVGL v8 lv_canvas
 *
 *  照片：SD卡 /sdcard/PHOTOS/ 目录下的 .jpg 文件，或根目录 .jpg
 *
 *  操作：
 *    左滑 → 下一张 | 右滑 → 上一张 | 点击 → 暂停/继续
 *    左上角按钮 → 返回菜单
 ****************************************************************************************************
 */

#include "photoviewer.h"
#include "menu.h"
#include "spi_sdcard.h"    /* 正点原子官方 SD 卡驱动 */
#include "tjpgd.h"         /* TJpgDec JPEG 解码器 */
#include "lvgl.h"
#include "esp_log.h"
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "PhotoViewer";

/* ── 配置 ───────────────────────────────────────── */
#define PHOTO_DIR_PRIMARY   "/sdcard/PHOTOS"
#define PHOTO_DIR_FALLBACK  "/sdcard"
#define SLIDESHOW_INTERVAL_MS  4000
#define MAX_PHOTOS             256
#define LCD_W                  800
#define LCD_H                  480
#define TJPGD_WORK_SIZE        32768  /* TJpgDec 工作缓冲区大小 */

/* ── 内部状态 ─────────────────────────────────── */
static lv_obj_t   *g_pv_scr        = NULL;
static lv_obj_t   *g_canvas[2]     = {NULL, NULL};  /* 0=底层(当前显示), 1=上层(下一个) */
static lv_obj_t   *g_info_label    = NULL;
static lv_timer_t *g_auto_timer    = NULL;

static char  *g_photo_list[MAX_PHOTOS];
static int    g_photo_count = 0;
static int    g_photo_idx   = 0;
static bool   g_paused      = false;
static esp_err_t g_sd_mount_err = ESP_OK;

static lv_coord_t g_press_x = 0;
static lv_coord_t g_press_y = 0;

static lv_color_t *g_cbuf[2] = {NULL, NULL};  /* buffer 对应 canvas[0] 和 canvas[1] */
static int         g_current   = 0;             /* 当前哪个 canvas 不透明 (0 或 1) */
static bool        g_animating = false;         /* 动画进行中，禁止并发切换 */
static int         g_preload_idx  = -1;         /* 已预加载的照片索引 */
static bool        g_preload_done = false;      /* 预加载是否成功 */

/* ── TJpgDec 解码上下文 ───────────────────────── */
typedef struct {
    FILE       *fp;
    lv_color_t *fb;       /* 帧缓冲 (RGB565) */
    int         fb_w;     /* 帧缓冲宽度 */
    int         fb_h;     /* 帧缓冲高度 */
    int         img_w;    /* 图片实际宽度（解码后得到） */
    int         img_h;    /* 图片实际高度 */
    int         off_x;    /* 居中偏移 X */
    int         off_y;    /* 居中偏移 Y */
    bool        stretch;  /* true = 图片小于屏幕，拉伸到全屏 */
} tjpgd_ctx_t;

/* TJpgDec 输入回调：从文件读取数据 */
static uint16_t tjpgd_input_cb(JDEC *jd, uint8_t *buf, uint16_t len)
{
    tjpgd_ctx_t *ctx = (tjpgd_ctx_t *)jd->device;
    if (!buf) {
        fseek(ctx->fp, len, SEEK_CUR);
        return len;
    }
    return (uint16_t)fread(buf, 1, len, ctx->fp);
}

/* TJpgDec 输出回调：将 RGB565 MCU 块写入帧缓冲 */
static uint16_t tjpgd_output_cb(JDEC *jd, void *bitmap, JRECT *rect)
{
    tjpgd_ctx_t *ctx = (tjpgd_ctx_t *)jd->device;
    uint16_t *src    = (uint16_t *)bitmap;
    int block_w      = (int)rect->right - (int)rect->left + 1;

    if (!ctx->stretch) {
        /* ── 大图片：居中裁剪模式，memcpy 整行复制 ── */
        int dst_x0 = (int)rect->left   + ctx->off_x;
        int dst_y0 = (int)rect->top    + ctx->off_y;
        int dst_x1 = (int)rect->right  + ctx->off_x;
        int dst_y1 = (int)rect->bottom + ctx->off_y;

        int clip_x0 = dst_x0 < 0 ? 0 : dst_x0;
        int clip_y0 = dst_y0 < 0 ? 0 : dst_y0;
        int clip_x1 = dst_x1 >= ctx->fb_w ? ctx->fb_w - 1 : dst_x1;
        int clip_y1 = dst_y1 >= ctx->fb_h ? ctx->fb_h - 1 : dst_y1;

        int copy_w = clip_x1 - clip_x0 + 1;
        if (copy_w <= 0) return 1;

        for (int y = clip_y0; y <= clip_y1; y++) {
            int src_row = y - dst_y0;
            int src_col = clip_x0 - dst_x0;
            memcpy(&ctx->fb[y * ctx->fb_w + clip_x0],
                   &src[src_row * block_w + src_col],
                   copy_w * sizeof(uint16_t));
        }
    } else {
        /* ── 小图片：拉伸到全屏（nearest-neighbor）── */
        for (int src_y = (int)rect->top; src_y <= (int)rect->bottom; src_y++) {
            int dst_y_start = src_y * LCD_H / ctx->img_h;
            int dst_y_end   = (src_y + 1) * LCD_H / ctx->img_h;
            if (dst_y_start >= LCD_H) continue;
            if (dst_y_end > LCD_H) dst_y_end = LCD_H;

            for (int src_x = (int)rect->left; src_x <= (int)rect->right; src_x++) {
                int dst_x_start = src_x * LCD_W / ctx->img_w;
                int dst_x_end   = (src_x + 1) * LCD_W / ctx->img_w;
                if (dst_x_start >= LCD_W) continue;
                if (dst_x_end > LCD_W) dst_x_end = LCD_W;

                uint16_t px = src[(src_y - rect->top) * block_w + (src_x - rect->left)];
                for (int dy = dst_y_start; dy < dst_y_end; dy++) {
                    for (int dx = dst_x_start; dx < dst_x_end; dx++) {
                        ctx->fb[dy * LCD_W + dx].full = px;
                    }
                }
            }
        }
    }
    return 1;  /* 继续解码 */
}

/* ── 前向声明 ─────────────────────────────────── */
static bool pv_load_photo(int idx, lv_obj_t *canvas, lv_color_t *buf);
static void pv_switch_to(int new_idx);
static void pv_auto_timer_cb(lv_timer_t *t);
static void pv_touch_cb(lv_event_t *e);
static void pv_back_cb(lv_event_t *e);
static void pv_fade_anim_cb(void *obj, int32_t v);
static void pv_update_info(void);

/* ═══════════════════════════════════════════════
   扫描照片
   ═══════════════════════════════════════════════ */
static void scan_photos(void)
{
    for (int i = 0; i < g_photo_count; i++) {
        free(g_photo_list[i]);
        g_photo_list[i] = NULL;
    }
    g_photo_count = 0;

    const char *dirs[] = { PHOTO_DIR_PRIMARY, PHOTO_DIR_FALLBACK };
    DIR *d = NULL;
    const char *scan_dir = NULL;
    for (int di = 0; di < 2; di++) {
        d = opendir(dirs[di]);
        if (d) { scan_dir = dirs[di]; break; }
        ESP_LOGI(TAG, "Cannot open dir: %s", dirs[di]);
    }
    if (!d) {
        ESP_LOGE(TAG, "Cannot open any photo directory. Check SD card mount.");
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL && g_photo_count < MAX_PHOTOS) {
        const char *name = entry->d_name;
        size_t len = strlen(name);
        if (len < 5) continue;
        if (name[0] == '.') continue;  /* 跳过隐藏文件（如 macOS 的 ._xxx.jpg） */
        bool is_jpg = (strcasecmp(name + len - 4, ".jpg") == 0) ||
                      (len > 5 && strcasecmp(name + len - 5, ".jpeg") == 0);
        if (!is_jpg) continue;
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", scan_dir, name);
        g_photo_list[g_photo_count] = strdup(path);
        if (g_photo_list[g_photo_count]) {
            ESP_LOGI(TAG, "[%d] %s", g_photo_count, path);
            g_photo_count++;
        }
    }
    closedir(d);
    ESP_LOGI(TAG, "Total: %d photos", g_photo_count);
}

/* ═══════════════════════════════════════════════
   JPEG 解码 → LVGL canvas buffer（用 TJpgDec）
   ═══════════════════════════════════════════════ */
static bool pv_load_photo(int idx, lv_obj_t *canvas, lv_color_t *buf)
{
    size_t bufsz = (size_t)LCD_W * LCD_H * sizeof(lv_color_t);
    memset(buf, 0x00, bufsz);  /* 黑色背景 */

    if (g_photo_count == 0 || idx < 0 || idx >= g_photo_count) {
        lv_draw_label_dsc_t dsc;
        lv_draw_label_dsc_init(&dsc);
        dsc.color = lv_color_hex(0xFFFFFF);
        dsc.font  = &lv_font_montserrat_24;
        if (g_sd_mount_err != ESP_OK) {
            char errbuf[256];
            snprintf(errbuf, sizeof(errbuf),
                "SD card mount failed!\n"
                "Error: %s (%d)\n\n"
                "Please check:\n"
                "1. SD card is inserted\n"
                "2. Format SD card as FAT32\n"
                "3. Not exFAT/NTFS",
                esp_err_to_name(g_sd_mount_err), g_sd_mount_err);
            lv_canvas_draw_text(canvas, 60, 140, 680, &dsc, errbuf);
        } else {
            lv_canvas_draw_text(canvas, 60, 160, 680, &dsc,
                "No JPEG photos found.\n"
                "1. Check SD card is FAT32.\n"
                "2. Create /PHOTOS folder and put .jpg files.\n"
                "3. Use short English names (8.3) if LFN off.");
        }
        return false;
    }

    const char *path = g_photo_list[idx];
    ESP_LOGI(TAG, "Loading: %s", path);

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        ESP_LOGE(TAG, "fopen failed: %s", path);
        lv_draw_label_dsc_t dsc;
        lv_draw_label_dsc_init(&dsc);
        dsc.color = lv_color_hex(0xFF0000);
        dsc.font  = &lv_font_montserrat_20;
        char errtxt[128];
        snprintf(errtxt, sizeof(errtxt), "Cannot open file:\n%s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
        lv_canvas_draw_text(canvas, 60, 200, 680, &dsc, errtxt);
        return false;
    }

    /* 分配 TJpgDec 工作缓冲区（内部 SRAM，TJpgDec 在解码中频繁访问） */
    void *work = malloc(TJPGD_WORK_SIZE);
    if (!work) {
        ESP_LOGE(TAG, "TJpgDec work malloc failed");
        fclose(fp);
        return false;
    }

    tjpgd_ctx_t ctx = {
        .fp       = fp,
        .fb       = buf,
        .fb_w     = LCD_W,
        .fb_h     = LCD_H,
        .off_x    = 0,
        .off_y    = 0,
        .stretch  = false,
    };

    JDEC jd;
    JRESULT res = jd_prepare(&jd, tjpgd_input_cb, work, TJPGD_WORK_SIZE, &ctx);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "jd_prepare failed: %d (path=%s)", res, path);
        lv_draw_label_dsc_t dsc;
        lv_draw_label_dsc_init(&dsc);
        dsc.color = lv_color_hex(0xFF0000);
        dsc.font  = &lv_font_montserrat_20;
        char errtxt[128];
        snprintf(errtxt, sizeof(errtxt), "JPEG decode error: %d\n%s", res, strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
        lv_canvas_draw_text(canvas, 60, 200, 680, &dsc, errtxt);
        goto done;
    }

    /* 选择缩放比例：
     * 目标：让输出尺寸能覆盖屏幕（两边都 >= 屏幕），这样居中裁剪能填满全屏。
     * 如果即使 scale=0 也无法覆盖，保持 scale=0（小图片用拉伸模式）。
     */
    uint8_t scale = 0;
    while (scale < 3) {
        int next_sw = jd.width  >> (scale + 1);
        int next_sh = jd.height >> (scale + 1);
        /* 如果下一级降采样后，两边仍然都 >= 屏幕，就继续降级 */
        if (next_sw >= LCD_W && next_sh >= LCD_H) {
            scale++;
        } else {
            break;
        }
    }

    ctx.img_w = jd.width >> scale;
    ctx.img_h = jd.height >> scale;
    ctx.stretch = (ctx.img_w < LCD_W || ctx.img_h < LCD_H);

    ESP_LOGI(TAG, "JPEG %dx%d, scale=1/%d, output %dx%d, stretch=%d",
             jd.width, jd.height, 1 << scale, ctx.img_w, ctx.img_h, ctx.stretch);

    if (!ctx.stretch) {
        /* 居中偏移：负值表示居中裁剪（图片大于屏幕时只显示中间部分） */
        ctx.off_x = (LCD_W - ctx.img_w) / 2;
        ctx.off_y = (LCD_H - ctx.img_h) / 2;
    }

    res = jd_decomp(&jd, tjpgd_output_cb, scale);
    if (res != JDR_OK) {
        ESP_LOGE(TAG, "jd_decomp failed: %d (path=%s)", res, path);
        goto done;
    }

    free(work);
    fclose(fp);
    lv_obj_invalidate(canvas);
    return true;

done:
    free(work);
    fclose(fp);
    lv_obj_invalidate(canvas);
    return false;
}

/* ═══════════════════════════════════════════════
   过渡动画
   ═══════════════════════════════════════════════ */
static void pv_fade_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void pv_preload_next(void)
{
    if (g_photo_count <= 1) return;

    int next_idx = (g_photo_idx + 1) % g_photo_count;
    if (next_idx == g_preload_idx && g_preload_done) return;  /* 已预加载 */

    int buf_idx = 1 - g_current;
    bool ok = pv_load_photo(next_idx, g_canvas[buf_idx], g_cbuf[buf_idx]);
    g_preload_idx = next_idx;
    g_preload_done = ok;
    ESP_LOGI(TAG, "Preload [%d] %s", next_idx, ok ? "OK" : "FAIL");
}

static void pv_delayed_preload_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    pv_preload_next();
}

static void pv_anim_ready_cb(lv_anim_t *a)
{
    (void)a;
    g_current = 1 - g_current;
    g_animating = false;

    /* 延迟 200ms 后预加载，避免动画刚结束就阻塞 LVGL */
    lv_timer_create(pv_delayed_preload_cb, 200, NULL);
}

static void pv_switch_to(int new_idx)
{
    if (g_photo_count == 0 || g_animating) return;

    /* 计算目标索引 */
    int target = ((new_idx % g_photo_count) + g_photo_count) % g_photo_count;
    int next = 1 - g_current;

    /* 如果目标已预加载，直接使用 */
    bool need_load = true;
    if (target == g_preload_idx && g_preload_done) {
        need_load = false;
        g_photo_idx = target;
        ESP_LOGI(TAG, "Switch use preloaded %d", target);
    }

    if (need_load) {
        int try_idx = target;
        int tried = 0;
        bool ok = false;
        while (tried < g_photo_count) {
            ok = pv_load_photo(try_idx, g_canvas[next], g_cbuf[next]);
            if (ok) {
                g_photo_idx = try_idx;
                break;
            }
            try_idx = (try_idx + 1) % g_photo_count;
            tried++;
        }
        if (!ok) {
            ESP_LOGE(TAG, "All %d photos failed to load", g_photo_count);
            return;
        }
        g_preload_done = false;  /* 同步加载会覆盖预加载内容 */
    }

    g_animating = true;

    /* 淡入淡出 — 延长到 600ms 让低帧率下也有更多可见渐变帧 */
    lv_obj_set_style_opa(g_canvas[next], LV_OPA_TRANSP, 0);

    lv_anim_t a_in;
    lv_anim_init(&a_in);
    lv_anim_set_var(&a_in, g_canvas[next]);
    lv_anim_set_exec_cb(&a_in, pv_fade_anim_cb);
    lv_anim_set_values(&a_in, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a_in, 600);
    lv_anim_set_path_cb(&a_in, lv_anim_path_ease_out);
    lv_anim_start(&a_in);

    lv_anim_t a_out;
    lv_anim_init(&a_out);
    lv_anim_set_var(&a_out, g_canvas[g_current]);
    lv_anim_set_exec_cb(&a_out, pv_fade_anim_cb);
    lv_anim_set_values(&a_out, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a_out, 600);
    lv_anim_set_path_cb(&a_out, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a_out, pv_anim_ready_cb);
    lv_anim_start(&a_out);

    pv_update_info();
}

static void pv_auto_timer_cb(lv_timer_t *t) { (void)t; if (!g_paused) pv_switch_to(g_photo_idx + 1); }

static void pv_update_info(void)
{
    if (!g_info_label) return;
    if (g_photo_count == 0) { lv_label_set_text(g_info_label, "No photos - SD:/PHOTOS/*.jpg"); return; }
    const char *path  = g_photo_list[g_photo_idx];
    const char *fname = strrchr(path, '/'); fname = fname ? fname + 1 : path;
    char buf[160];
    snprintf(buf, sizeof(buf), "%d/%d  %s%s", g_photo_idx + 1, g_photo_count, g_paused ? "[PAUSED] " : "", fname);
    lv_label_set_text(g_info_label, buf);
}

static void pv_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev    = lv_indev_get_act();
    lv_point_t  pt;
    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &pt);
        g_press_x = pt.x; g_press_y = pt.y;
    } else if (code == LV_EVENT_RELEASED) {
        lv_indev_get_point(indev, &pt);
        lv_coord_t dx = pt.x - g_press_x, dy = pt.y - g_press_y;
        if (LV_ABS(dx) > 60 && LV_ABS(dx) > LV_ABS(dy) * 2) {
            if (dx < 0) pv_switch_to(g_photo_idx + 1);
            else        pv_switch_to(g_photo_idx - 1);
        } else if (LV_ABS(dx) < 20 && LV_ABS(dy) < 20) {
            g_paused = !g_paused; pv_update_info();
        }
    }
}

static void pv_back_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) photoviewer_stop();
}

/* ═══════════════════════════════════════════════
   公共接口：启动
   ═══════════════════════════════════════════════ */
void photoviewer_start(void)
{
    ESP_LOGI(TAG, "=== Photo Viewer Start ===");

    /* 1. SPI2 初始化 + SD 卡挂载 */
    spi2_init();
    g_sd_mount_err = sd_spi_init();
    if (g_sd_mount_err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(g_sd_mount_err));
    } else {
        ESP_LOGI(TAG, "SD card mounted at /sdcard");
    }

    /* 2. 扫描照片 */
    scan_photos();

    /* 3. 分配双 PSRAM 画布缓冲 */
    size_t bufsz = (size_t)LCD_W * LCD_H * sizeof(lv_color_t);
    g_cbuf[0] = heap_caps_malloc(bufsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    g_cbuf[1] = heap_caps_malloc(bufsz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!g_cbuf[0] || !g_cbuf[1]) {
        ESP_LOGE(TAG, "PSRAM canvas alloc failed");
        if (g_cbuf[0]) { free(g_cbuf[0]); g_cbuf[0] = NULL; }
        if (g_cbuf[1]) { free(g_cbuf[1]); g_cbuf[1] = NULL; }
        return;
    }
    memset(g_cbuf[0], 0x00, bufsz);
    memset(g_cbuf[1], 0x00, bufsz);

    /* 4. 新屏幕 */
    g_pv_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_pv_scr, lv_color_hex(0x000000), 0);
    lv_scr_load(g_pv_scr);

    /* 5. 底层画布（当前显示） */
    g_canvas[0] = lv_canvas_create(g_pv_scr);
    lv_canvas_set_buffer(g_canvas[0], g_cbuf[0], LCD_W, LCD_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(g_canvas[0], 0, 0);
    lv_obj_clear_flag(g_canvas[0], LV_OBJ_FLAG_CLICKABLE);

    /* 6. 上层画布（下一个照片，初始透明） */
    g_canvas[1] = lv_canvas_create(g_pv_scr);
    lv_canvas_set_buffer(g_canvas[1], g_cbuf[1], LCD_W, LCD_H, LV_IMG_CF_TRUE_COLOR);
    lv_obj_set_pos(g_canvas[1], 0, 0);
    lv_obj_set_style_opa(g_canvas[1], LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(g_canvas[1], LV_OBJ_FLAG_CLICKABLE);

    /* 7. 全屏透明触摸层（覆盖整个屏幕） */
    lv_obj_t *touch_layer = lv_obj_create(g_pv_scr);
    lv_obj_set_size(touch_layer, LCD_W, LCD_H);
    lv_obj_set_pos(touch_layer, 0, 0);
    lv_obj_set_style_bg_opa(touch_layer, LV_OPA_0, 0);
    lv_obj_set_style_border_width(touch_layer, 0, 0);
    lv_obj_clear_flag(touch_layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(touch_layer, pv_touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(touch_layer, pv_touch_cb, LV_EVENT_RELEASED, NULL);

    /* 8. 返回按钮（在触摸层之上，左上角，半透明浮动） */
    lv_obj_t *back_btn = lv_obj_create(g_pv_scr);
    lv_obj_set_size(back_btn, 76, 30);
    lv_obj_set_pos(back_btn, 8, 6);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0xE74C3C), 0);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_70, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_radius(back_btn, 6, 0);
    lv_obj_clear_flag(back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_hex(0xFFFFFF), 0);
    lv_obj_center(back_lbl);
    lv_obj_add_event_cb(back_btn, pv_back_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(back_lbl, pv_back_cb, LV_EVENT_RELEASED, NULL);

    /* 信息标签（在触摸层之上，顶部居中，半透明黑底） */
    g_info_label = lv_label_create(g_pv_scr);
    lv_obj_set_style_text_font(g_info_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(g_info_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_opa(g_info_label, LV_OPA_90, 0);
    lv_obj_set_pos(g_info_label, 100, 8);
    lv_obj_set_width(g_info_label, 600);
    lv_label_set_long_mode(g_info_label, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_bg_color(g_info_label, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(g_info_label, LV_OPA_30, 0);
    lv_obj_set_style_radius(g_info_label, 4, 0);
    lv_obj_set_style_pad_all(g_info_label, 4, 0);

    /* 9. 加载第一张 */
    g_photo_idx = 0;
    g_paused = false;
    g_preload_idx = -1;
    g_preload_done = false;
    pv_load_photo(g_photo_idx, g_canvas[0], g_cbuf[0]);
    pv_update_info();

    /* 延迟预加载下一张，避免第一次切换阻塞 */
    if (g_photo_count > 1) {
        lv_timer_create(pv_delayed_preload_cb, 500, NULL);
    }

    /* 10. 自动播放定时器 */
    if (g_auto_timer) { lv_timer_del(g_auto_timer); g_auto_timer = NULL; }
    g_auto_timer = lv_timer_create(pv_auto_timer_cb, SLIDESHOW_INTERVAL_MS, NULL);
    ESP_LOGI(TAG, "Photo viewer ready, %d photos, timer=%p", g_photo_count, (void*)g_auto_timer);
}

/* ═══════════════════════════════════════════════
   公共接口：停止
   ═══════════════════════════════════════════════ */
void photoviewer_stop(void)
{
    ESP_LOGI(TAG, "Stopping photo viewer");
    if (g_auto_timer) { lv_timer_del(g_auto_timer); g_auto_timer = NULL; }
    menu_go_back();
    if (g_cbuf[0]) { free(g_cbuf[0]); g_cbuf[0] = NULL; }
    if (g_cbuf[1]) { free(g_cbuf[1]); g_cbuf[1] = NULL; }
    g_canvas[0] = g_canvas[1] = NULL;
    g_info_label = g_pv_scr = NULL;
    for (int i = 0; i < g_photo_count; i++) { free(g_photo_list[i]); g_photo_list[i] = NULL; }
    g_photo_count = 0;
    g_paused = false;
    g_current = 0;
    g_animating = false;
    g_preload_idx = -1;
    g_preload_done = false;
}
