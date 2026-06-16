# FC/NES模拟器 - 交接文档

最后更新: 2026-06-15

## 一、项目概述

在DNESP32S3 4.3寸RGB LCD开发板上添加FC/NES模拟器功能，使用retro-go的nofrendo引擎。

### 硬件环境
- **MCU**: ESP32-S3 (QFN56, rev v0.2, 8MB PSRAM)
- **屏幕**: 4.3寸 RGB LCD 800×480 (ID: 0x4384)
- **触摸**: GT9xxx电容触摸屏
- **存储**: SD卡 (FAT32, SPI模式)
- **音频**: ES8388 (I2S, 当前未使用)

### 软件环境
- **ESP-IDF**: v5.5.2
- **LVGL**: v8.4.0
- **编译命令**: `source ~/esp/esp-idf-v5.5/export.sh && idf.py build`
- **烧录命令**: `idf.py -p /dev/cu.usbserial-21240 flash`
- **串口监视**: `idf.py -p /dev/cu.usbserial-21240 monitor`

## 二、已完成的工作

### 1. nofrendo模拟器引擎
- **位置**: `components/nofrendo/`
- **来源**: retro-go项目的nofrendo移植版
- **支持**: 50+ mapper, iNES格式ROM
- **编译**: 已适配ESP-IDF v5.5, 禁用了format warning

### 2. FC模拟器集成层
- **位置**: `main/APP/fc_emulator.c` 和 `fc_emulator.h`
- **ROM浏览器**: 使用LVGL列表控件，扫描`/sdcard/FC/`和`/sdcard/`目录
- **游戏画面**: 独占屏幕模式，1x显示(256×240居中在800×480)
- **输入**: 直接读取触摸设备(`tp_dev`)，不走LVGL事件

### 3. LVGL集成
- **菜单入口**: `main/APP/menu.c` 中添加了 `APP_FC` 和 `fc_emulator_show_rom_list()`
- **LVGL暂停/恢复**: `lvgl_demo_suspend()` 和 `lvgl_demo_resume()` 已实现
- **获取framebuffer**: `lvgl_demo_get_framebuffers()` 已添加

### 4. 分区表
- **文件**: `partitions-16MiB.csv`
- **factory分区**: 0x250000 (2.3MB)

## 三、当前问题：画面上下抖动

### 问题描述
进入FC游戏后，画面不停上下抖动。

### 根本原因
RGB LCD配置了双framebuffer (`num_fbs = 2`)，DMA持续循环读取两个buffer。当FC模拟器只写入一个buffer时，另一个buffer包含旧数据，DMA交替读取导致画面在新旧帧之间快速切换。

### 已尝试的方案（均未完全解决）
1. **直接写入fb0** → 抖动
2. **双buffer切换** → 抖动
3. **memcpy到fb** → 抖动
4. **esp_lcd_panel_draw_bitmap** → 抖动
5. **同时写入fb0和fb1** → 抖动（当前方案）
6. **单buffer (num_fbs=1)** → LVGL黑屏
7. **refresh_on_demand** → LVGL黑屏
8. **关闭bounce_buffer** → LVGL画面左右移动
9. **修正hsync/vsync_pulse_width** → 无效
10. **降低pclk到16MHz** → 无效

### 可能的解决方向
1. **VBlank同步**: VSYNC引脚未连接(GPIO_NUM_NC)，无法直接等待VBlank。可能需要通过LCD控制器寄存器检测VBlank。
2. **暂停DMA**: 在写入framebuffer前暂停LCD DMA，写完后恢复。需要操作ESP32-S3 LCD控制器寄存器。
3. **LVGL flush机制**: 不直接写framebuffer，而是让LVGL的flush_cb负责刷新。FC写入独立render buffer，通过LVGL同步到LCD。
4. **降低LCD刷新率**: 让LCD刷新率匹配NES帧率(~30fps)。
5. **使用esp_lcd_rgb_panel_restart**: 在写入后调用此函数重新同步DMA。

### 关键配置位置
- **RGB LCD配置**: `components/BSP/RGBLCD/rgblcd.c` 第74-105行
- **LVGL flush回调**: `main/APP/lvgl_demo.c` 第246-260行
- **FC blit回调**: `main/APP/fc_emulator.c` 第119行

## 四、文件清单

### 新增文件
```
components/nofrendo/          # NES模拟器引擎
├── CMakeLists.txt
├── nes/nes.h, nes.c         # NES主循环
├── nes/cpu.h, cpu.c         # 6502 CPU
├── nes/ppu.h, ppu.c         # 图像处理
├── nes/apu.h, apu.c         # 音频处理
├── nes/mem.h, mem.c         # 内存管理
├── nes/mmc.h, mmc.c         # Mapper管理
├── nes/rom.h, rom.c         # ROM加载
├── nes/input.h, input.c     # 输入处理
├── mappers/                  # 50+ mapper实现
├── nofrendo.h, nofrendo.c   # 引擎入口
├── palettes.h               # NES调色板
└── config.h                 # 配置

main/APP/fc_emulator.c       # FC模拟器主逻辑
main/APP/fc_emulator.h       # FC模拟器头文件
```

### 修改文件
```
main/APP/lvgl_demo.c         # 添加 lvgl_demo_get_framebuffers()
main/APP/lvgl_demo.h         # 添加函数声明
main/APP/menu.c              # 添加 APP_FC 入口
main/CMakeLists.txt          # 添加 fc_emulator.c
components/BSP/RGBLCD/rgblcd.c  # 修改LCD时序(hsync/vsync已修正)
partitions-16MiB.csv         # 扩大factory分区
```

## 五、ROM文件

- **路径**: `/sdcard/FC/*.nes` 或 `/sdcard/*.nes`
- **格式**: iNES (.nes)
- **文件名**: 支持中文文件名(FATFS codepage 936)
- **当前测试ROM**: `/sdcard/FC/` 目录下有约300个中文命名的NES ROM

## 六、构建注意事项

1. **ESP-IDF版本**: 必须使用v5.5，其他版本可能有兼容性问题
2. **nofrendo编译警告**: 已在`components/nofrendo/CMakeLists.txt`中禁用format warning
3. **分区表**: 使用`partitions-16MiB.csv`，factory分区2.3MB
4. **PSRAM**: 模拟器需要PSRAM分配vidbuf和palette

## 七、下一步优化方向

### 优先级高
1. **解决画面抖动** - 这是核心问题，需要深入研究RGB LCD DMA机制
2. **提高帧率** - 当前约15-25fps，目标30fps+
3. **音频支持** - 通过ES8388输出NES音频

### 优先级中
4. **2x缩放显示** - 512×480全屏显示
5. **存档功能** - SRAM和存档状态保存到SD卡
6. **实体按键支持** - GPIO按键作为NES手柄输入

### 优先级低
7. **中文ROM名显示** - 当前LVGL中文字体需要从SD卡加载
8. **ROM封面显示** - 在ROM列表中显示游戏截图
9. **多手柄支持** - 2P游戏支持

## 八、联系方式

- **GitHub**: https://github.com/Liutupi/DNESP32S3-4.3-RGBLCD-2048
- **分支**: main
- **最新提交**: 待提交

## 九、参考资源

- **retro-go**: https://github.com/ducalex/retro-go
- **nofrendo**: retro-go项目中的NES模拟器引擎
- **ESP-IDF RGB LCD**: `esp_lcd_panel_rgb.h` 中的 `refresh_on_demand` 和 `esp_lcd_rgb_panel_restart`
- **ESP32-S3 LCD DMA**: 可能需要直接操作LCD_CAM寄存器来暂停/恢复DMA
