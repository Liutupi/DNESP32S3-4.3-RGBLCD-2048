/**
 ****************************************************************************************************
 * @file        lvgl_demo.c
 * @author      正点原子团队(ALIENTEK)
 * @version     V1.0
 * @date        2025-01-01
 * @brief       LVGL V8移植 实验
 * @license     Copyright (c) 2020-2032, 广州市星翼电子科技有限公司
 ****************************************************************************************************
 * @attention
 *
 * 实验平台:正点原子 ESP32-S3 开发板
 * 在线视频:www.yuanzige.com
 * 技术论坛:www.openedv.com
 * 公司网址:www.alientek.com
 * 购买地址:openedv.taobao.com
 *
 ****************************************************************************************************
 */

#include "lvgl_demo.h"
#include "app_network.h"
#include "menu.h"
#include "tomato_timer.h"
#include "ui_fonts.h"
#include "lcd.h"
#include "touch.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "lvgl.h"
#include "demos/lv_demos.h"

static const char *TAG = "LVGL_DEMO";
static esp_timer_handle_t s_lvgl_tick_timer = NULL;
static lv_disp_draw_buf_t s_disp_buf;
static lv_disp_drv_t s_disp_drv;
static lv_disp_t *s_disp = NULL;
static bool s_lvgl_suspended = false;

static void increase_lvgl_tick(void *arg);
static void lvgl_disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map);

/**
 * @brief       lvgl_demo入口函数
 * @param       无
 * @retval      无
 */
void lvgl_demo(void)
{
    lv_init();              /* 初始化LVGL图形库 */
    lv_port_disp_init();    /* lvgl显示接口初始化,放在lv_init()的后面 */
    lv_port_indev_init();   /* lvgl输入接口初始化,放在lv_init()的后面 */
    ui_fonts_init();        /* 初始化WarmOS外部中文字体，失败时自动回退 */
    vTaskDelay(pdMS_TO_TICKS(1));

    /* 为LVGL提供时基单元 */
    const esp_timer_create_args_t lvgl_tick_timer_args = {
        .callback = &increase_lvgl_tick,    /* 设置定时器回调 */
        .name = "lvgl_tick"                 /* 定时器名称 */
    };
    ESP_ERROR_CHECK(esp_timer_create(&lvgl_tick_timer_args, &s_lvgl_tick_timer));   /* 创建定时器 */
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_lvgl_tick_timer, 1 * 1000));         /* 启动定时器 */

    /* 还原为主菜单原生入口 */
    menu_start();
    lv_obj_invalidate(lv_scr_act());
    lv_refr_now(NULL);
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(20));
    lv_timer_handler();
    lv_refr_now(NULL);
    app_network_start();

    while (1)
    {
        if (!s_lvgl_suspended) {
            lv_timer_handler();         /* LVGL计时器 */
        }
        vTaskDelay(pdMS_TO_TICKS(10));  /* 延时10毫秒 */
    }
}

/**
 * @brief       初始化并注册显示设备
 * @param       无
 * @retval      lvgl显示设备指针
 */
lv_disp_t *lv_port_disp_init(void)
{
    void *lcd_buffer[2];        /* 指向屏幕双缓存 */

    /* 初始化显示设备LCD */
    lcd_init();                /* LCD初始化 */

    /*-----------------------------
     * 创建一个绘图缓冲区
     *----------------------------*/
    /**
     * LVGL 需要一个缓冲区用来绘制小部件
     * 随后，这个缓冲区的内容会通过显示设备的 'flush_cb'(显示设备刷新函数) 复制到显示设备上
     * 这个缓冲区的大小需要大于显示设备一行的大小
     *
     * 这里有3种缓冲配置:
     * 1. 单缓冲区:
     *      LVGL 会将显示设备的内容绘制到这里，并将他写入显示设备。
     *
     * 2. 双缓冲区:
     *      LVGL 会将显示设备的内容绘制到其中一个缓冲区，并将他写入显示设备。
     *      需要使用 DMA 将要显示在显示设备的内容写入缓冲区。
     *      当数据从第一个缓冲区发送时，它将使 LVGL 能够将屏幕的下一部分绘制到另一个缓冲区。
     *      这样使得渲染和刷新可以并行执行。
     *
     * 3. 全尺寸双缓冲区
     *      设置两个屏幕大小的全尺寸缓冲区，并且设置 disp_drv.full_refresh = 1。
     *      这样，LVGL将始终以 'flush_cb' 的形式提供整个渲染屏幕，您只需更改帧缓冲区的地址。
     */
    /* 使用双缓冲 */
    if (lcddev.id <= 0x7084)    /* RGB屏触摸屏 */
    {
        ESP_ERROR_CHECK(esp_lcd_rgb_panel_get_frame_buffer(lcddev.lcd_panel_handle, 2, &lcd_buffer[0], &lcd_buffer[1]));
    }
    
    size_t draw_buffer_sz = lcddev.width * lcddev.height * sizeof(lv_color_t);          /* 计算绘画缓冲区大小 */

    /* 初始化显示缓冲区 */
    lv_disp_draw_buf_init(&s_disp_buf, lcd_buffer[0], lcd_buffer[1], draw_buffer_sz);   /* 初始化显示缓冲区 */
    
    /* 在LVGL中注册显示设备 */
    lv_disp_drv_init(&s_disp_drv);      /* 初始化显示设备 */
    
    /* 设置显示设备的分辨率 
     * 这里为了适配正点原子的多款屏幕，采用了动态获取的方式，
     * 在实际项目中，通常所使用的屏幕大小是固定的，因此可以直接设置为屏幕的大小 
     */
    s_disp_drv.hor_res = lcddev.width;                  /* 设置水平分辨率 */
    s_disp_drv.ver_res = lcddev.height;                 /* 设置垂直分辨率 */

    /* 用来将缓冲区的内容复制到显示设备 */
    s_disp_drv.flush_cb = lvgl_disp_flush_cb;           /* 设置刷新回调函数 */

    /* 设置显示缓冲区 */
    s_disp_drv.draw_buf = &s_disp_buf;                  /* 设置绘画缓冲区 */

    s_disp_drv.user_data = lcddev.lcd_panel_handle;     /* 传递屏幕控制句柄 */
    s_disp_drv.full_refresh = 1;                        /* 设置为完全刷新 */
    /* 注册显示设备 */
    s_disp = lv_disp_drv_register(&s_disp_drv);
    return s_disp;                    
}

void lvgl_demo_suspend(void)
{
    if (s_lvgl_suspended) {
        return;
    }

    s_lvgl_suspended = true;
    if (s_lvgl_tick_timer) {
        esp_err_t ret = esp_timer_stop(s_lvgl_tick_timer);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "esp_timer_stop failed: %s", esp_err_to_name(ret));
        }
    }
}

void lvgl_demo_resume(void)
{
    if (!s_lvgl_suspended) {
        return;
    }

    if (s_lvgl_tick_timer) {
        esp_err_t ret = esp_timer_start_periodic(s_lvgl_tick_timer, 1 * 1000);
        if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "esp_timer_start_periodic failed: %s", esp_err_to_name(ret));
        }
    }
    s_lvgl_suspended = false;
}

void lvgl_demo_rebind_display(void)
{
    if (!s_disp) {
        ESP_LOGW(TAG, "No LVGL display to rebind");
        return;
    }

    void *lcd_buffer[2] = {0};
    esp_err_t ret = esp_lcd_rgb_panel_get_frame_buffer(lcddev.lcd_panel_handle, 2, &lcd_buffer[0], &lcd_buffer[1]);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Get RGB frame buffer failed: %s", esp_err_to_name(ret));
        return;
    }

    lv_disp_draw_buf_init(&s_disp_buf, lcd_buffer[0], lcd_buffer[1],
                          lcddev.width * lcddev.height * sizeof(lv_color_t));
    s_disp->driver->hor_res = lcddev.width;
    s_disp->driver->ver_res = lcddev.height;
    s_disp->driver->draw_buf = &s_disp_buf;
    s_disp->driver->user_data = lcddev.lcd_panel_handle;
    lv_disp_drv_update(s_disp, s_disp->driver);
}

bool lvgl_demo_is_suspended(void)
{
    return s_lvgl_suspended;
}

bool lvgl_demo_get_framebuffers(void **fb0, void **fb1, int *w, int *h)
{
    if (lcddev.lcd_panel_handle == NULL) {
        return false;
    }

    void *buf[2] = {0};
    esp_err_t ret = esp_lcd_rgb_panel_get_frame_buffer(lcddev.lcd_panel_handle, 2, &buf[0], &buf[1]);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Get RGB frame buffer failed: %s", esp_err_to_name(ret));
        return false;
    }

    if (fb0) *fb0 = buf[0];
    if (fb1) *fb1 = buf[1];
    if (w)   *w   = lcddev.width;
    if (h)   *h   = lcddev.height;
    return true;
}

lv_obj_t * debug_label = NULL;

/**
 * @brief       初始化并注册输入设备
 * @param       无
 * @retval      lvgl输入设备指针
 */
lv_indev_t *lv_port_indev_init(void)
{
    /* 初始化触摸屏 */
    tp_dev.init();

    /* 初始化输入设备 */
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);

    /* 配置输入设备类型 */
    indev_drv.type = LV_INDEV_TYPE_POINTER;

    /* 设置输入设备读取回调函数 */
    indev_drv.read_cb = touchpad_read;

    /* 在LVGL中注册驱动程序，并保存创建的输入设备对象 */
    lv_indev_t * indev = lv_indev_drv_register(&indev_drv);

    return indev;
}

/**
* @brief        将内部缓冲区的内容刷新到显示屏上的特定区域
* @note         可以使用 DMA 或者任何硬件在后台加速执行这个操作
*               但是，需要在刷新完成后调用函数 'lv_disp_flush_ready()'
* @param        disp_drv : 显示设备
* @param        area : 要刷新的区域，包含了填充矩形的对角坐标
* @param        color_map : 颜色数组
* @retval       无
*/
static void lvgl_disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    esp_lcd_panel_handle_t panel_handle = (esp_lcd_panel_handle_t)drv->user_data;

    if (s_lvgl_suspended || panel_handle == NULL) {
        lv_disp_flush_ready(drv);
        return;
    }

    /* 特定区域打点 */
    esp_lcd_panel_draw_bitmap(panel_handle, area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);

    /* 重要!!! 通知图形库，已经刷新完毕了 */
    lv_disp_flush_ready(drv);
}

/**
 * @brief       告诉LVGL运行时间
 * @param       arg : 传入参数(未用到)
 * @retval      无
 */
static void increase_lvgl_tick(void *arg)
{
    /* 告诉LVGL已经过了多少毫秒 */
    lv_tick_inc(1);
}

/**
 * @brief       获取触摸屏设备的状态
 * @param       无
 * @retval      返回触摸屏设备是否被按下
 */
static bool touchpad_is_pressed(void)
{
    tp_dev.scan(0);     /* 触摸按键扫描 */

    if (tp_dev.sta & TP_PRES_DOWN)
    {
        return true;
    }

    return false;
}


/**
 * @brief       在触摸屏被按下的时候读取 x、y 坐标
 * @param       x   : x坐标的指针
 * @param       y   : y坐标的指针
 * @retval      无
 */
static void touchpad_get_xy(lv_coord_t *x, lv_coord_t *y)
{
    (*x) = tp_dev.x[0];
    (*y) = tp_dev.y[0];
}

/**
 * @brief       图形库的触摸屏读取回调函数
 * @param       indev_drv   : 触摸屏设备
 * @param       data        : 输入设备数据结构体
 * @retval      无
 */
void touchpad_read(lv_indev_drv_t *indev_drv, lv_indev_data_t *data)
{
    static lv_coord_t last_x = 0;
    static lv_coord_t last_y = 0;

    /* 保存按下的坐标和状态 */
    if(touchpad_is_pressed())
    {
        touchpad_get_xy(&last_x, &last_y);  /* 在触摸屏被按下的时候读取 x、y 坐标 */
        data->state = LV_INDEV_STATE_PR;
    } 
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }

    /* 设置最后按下的坐标 */
    data->point.x = last_x;
    data->point.y = last_y;

    if (debug_label) {
        lv_label_set_text(debug_label, touch_debug_info);
    }
}
