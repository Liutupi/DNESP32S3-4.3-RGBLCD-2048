# DNESP32S3 Game Center

Current firmware / build / flash handoff for humans and other AI agents:
`docs/CURRENT_FIRMWARE_FLASH.md`.

Current onboard ES8388 headless Network Radio handoff:
`docs/RADIO_HEADLESS_HANDOFF.md`.

基于 **正点原子 DNESP32S3** 开发板的触屏应用集合，带 WarmOS 主菜单和
板载 ES8388 无头网络电台。

| 项目 | 说明 |
|------|------|
| 开发板 | DNESP32S3 (ESP32-S3, 16MB Flash, 8MB PSRAM) |
| 显示屏 | 4.3" RGB LCD (800×480) |
| 触摸 | GT9xxx 电容触摸 (I2C1) |
| 框架 | ESP-IDF v5.5 + LVGL v8 |
| 串口 | COM10 (CH340, 烧录波特率 460800, 监控波特率 115200) |

---

## 📁 项目结构

```
├── CMakeLists.txt              # 顶层 CMake，指定 EXTRA_COMPONENT_DIRS
├── sdkconfig                   # 项目配置（16MB Flash、自定义分区表等）
├── partitions-16MiB.csv        # 分区表（可选，由 sdkconfig 引用）
├── main/
│   ├── CMakeLists.txt          # SRC_DIRS: "." "APP"
│   ├── main.c                  # app_main() -> lvgl_demo()
│   └── APP/
│       ├── lvgl_demo.c / .h    # LVGL 移植层：显示、触摸、时基
│       ├── menu.c / .h         # 主菜单（分级选择游戏）
│       ├── game2048.c / .h     # 2048 游戏逻辑与 UI
│       ├── reaction_test.c / .h # 反应速度测试
│       ├── radio_headless.c / .h # ES8388 无头网络电台
│       ├── radio_player.c / .h # HTTP/MP3 解码与 I2S 写入
│       └── (依赖 components/BSP 中的 LCD/TOUCH 驱动)
├── components/
│   └── BSP/                    # 正点原子 BSP（RGB LCD、GT9xxx、XL9555 等）
└── docs/                       # 📚 硬件文档（原理图 / 管脚表 / 芯片手册）
    ├── PINOUT.md               # IO 管脚分配表（大模型可直接读取）
    ├── hardware/
    │   ├── DNESP32S3_V1.0_硬件参考手册.pdf
    │   ├── DNESP32S3_V1.2_原理图.pdf
    │   └── DNESP32S3_IO管脚分配表.xlsx
    ├── datasheets/             # 关键芯片数据手册
    │   ├── ES8388-DS.pdf       # 音频编解码器
    │   ├── GT9147_数据手册.pdf  # 触摸芯片
    │   ├── CH340.pdf           # USB 转串口
    │   └── XL9555_...pdf       # IO 扩展芯片
    └── guides/
        ├── DNESP32S3_刷机指南.md
        └── LVGL移植教程.pdf
```

**关键修改点**：
- `lvgl_demo.c` 中把原来的 `lv_demo_music()` 替换为 `menu_start()`，启动后进入 Game Center 主菜单。
- `game2048.c` 使用 LVGL v8 的 `lv_obj_create(NULL)` + `lv_scr_load()` 创建新屏幕，**不能直接 `lv_obj_clean(lv_scr_act())`**，否则会删掉 `lv_port_indev_init()` 中创建的 `debug_label` 和 `cursor_obj`，导致 `touchpad_read` 访问悬空指针而 `LoadProhibited` 崩溃。
- `menu.c` 在主屏幕（默认屏幕）上构建菜单 UI，避免启动时屏幕切换的复杂性问题。
- 所有游戏通过 `menu_go_back()` 返回主菜单，该函数加载菜单屏幕并删除游戏屏幕。

---

## ⚡ 快速开始（编译 & 烧录）

> ⚠️ **路径里不要有中文！** 本项目已从 `【正点原子】DNESP32S3开发板资料（A盘）/4，程序源码/...` 复制到 `D:\esp_project\2048` 才能正常编译。

### 1. 激活 ESP-IDF 环境
```powershell
C:\Users\Administrator\esp-idf\export.ps1
```

### 2. 编译
```powershell
cd D:\esp_project\2048
idf.py build
```

### 3. 烧录
```powershell
idf.py -p COM10 flash
```

macOS 上最近验证过的串口是：

```bash
. ~/esp/esp-idf-v5.5/export.sh
idf.py -p /dev/cu.usbserial-21230 -b 460800 flash
```

更完整、给其他模型交接用的刷机说明见
`docs/CURRENT_FIRMWARE_FLASH.md`。

### 4. 查看串口日志
```powershell
idf.py -p COM10 monitor
# 或直接用 python 读 115200
python -c "import serial; s=serial.Serial('COM10',115200);
while True: print(s.readline().decode('utf-8','replace'), end='')"
```

---

## 🐛 踩坑记录（按时间线）

### 坑 1：中文路径导致 `UnicodeDecodeError`
**现象**：
```
UnicodeDecodeError: 'gbk' codec can't decode byte 0xXX in position Y
```
**根因**：ESP-IDF 的 Python 工具链（`kconfgen`、`prepare_kconfig_files.py`）在 Windows 上默认用 GBK 解析路径。
**解决**：把项目复制到纯英文短路径，例如 `D:\esp_project\2048`。

### 坑 2：编译中断后 `.a` 库损坏 / `objdump: file format not recognized`
**现象**：
```
xtensa-esp-elf-ranlib.exe: 'libcmock.a': No such file
xtensa-esp-elf-objdump: file format not recognized
```
**根因**：上次编译被中断，或 ccache 里的对象文件/静态库损坏。
**解决**：
```powershell
idf.py fullclean
# 如果还不行，手动删 managed_components 再重新 build
Remove-Item -Recurse -Force managed_components
idf.py build
```

### 坑 3：`stray '\' in program`
**现象**：编译报错 `stray '\' in program`。
**根因**：代码里写了字面量字符串 `"\n"`（两个字符反斜杠+n），但本意是换行符 `\n`（转义）。
**解决**：检查字符串，确认是 `"\n"`（2 字节）还是 `\n`（1 字节换行）。

### 坑 4：刷机后循环重启 `LoadProhibited`
**现象**：
```
I (1935) GT9XXX: CTP:1158
Guru Meditation Error: Core 0 panic'ed (LoadProhibited)
Backtrace: ... lvgl_demo.c:264 ...
```
**根因**：`game2048_start()` 最初用了 `lv_obj_clean(scr)` 清屏，把 `lv_port_indev_init()` 在同一屏幕上创建的 `debug_label` / `cursor_obj` 删掉了。但 `touchpad_read()` 回调仍持有该指针，每次定时器调用都会访问已释放内存。
**解决**：改为 `lv_obj_t *scr = lv_obj_create(NULL); lv_scr_load(scr);` 创建新屏幕并切换，旧屏幕保留在内存中供回调安全访问。

### 坑 6：触摸按钮无反应（LVGL v8 事件冒泡缺陷）

**现象**：屏幕上的按钮（如 "New Game"、"Menu"）点击无任何响应，但触摸硬件正常。

**根因**：LVGL v8 的 `lv_btn` 内部添加 `lv_label` 作为子对象后，Label 在 z-order 上位于按钮之上。`lv_obj_hit_test()` 判定 Label 为 `LV_OBJ_FLAG_CLICKABLE`（默认所有对象均此 flag），因此 indev 将触摸事件分发给 Label 而非 Button。Label 无事件回调 → 事件被丢弃。

**关键源码**（`lv_obj_pos.c:949`）：
```c
bool lv_obj_hit_test(lv_obj_t * obj, const lv_point_t * point)
{
    if(!lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) return false;
    ...
}
```

所有 `lv_obj_create()` 创建的对象默认带 `LV_OBJ_FLAG_CLICKABLE`（`lv_obj.c:436`）。

**解决**：
1. **方案 A（推荐）**：在按钮 Label 上也注册相同的事件回调。
   ```c
   lv_obj_add_event_cb(btn_label, btn_callback, LV_EVENT_RELEASED, NULL);
   ```
2. **方案 B**：使用全屏透明触摸层 + 坐标 `hit_test`（menu.c 采用此方案）。
3. **方案 C**：`lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE)`（但这会使 Label 完全不可交互，indev 会继续搜索到 Button）。

### 坑 7：新屏幕触摸失效

**现象**：`lv_obj_create(NULL)` + `lv_scr_load()` 创建的新屏幕上所有触摸事件不响应。

**根因**：`lv_obj_create(NULL)` 创建的屏幕对象没有与任何 display 关联时，`lv_scr_load()` 可能无法正确设置活动屏幕。

**解决**：主菜单直接在 `lv_port_indev_init()` 创建的默认屏幕上构建（`lv_scr_act()`），游戏屏幕用 `lv_obj_create(NULL)` + `lv_scr_load()` 创建。返回菜单时通过 `menu_go_back()` 切换回默认屏并删除游戏屏。



## 🎮 游戏列表

| 游戏/应用 | 文件 | 说明 |
|------|------|------|
| **2048** | `game2048.c/h` | 在 4×4 棋盘上滑动合并数字方块，达到 2048 即胜利 |
| **Reaction Test** | `reaction_test.c/h` | 等待屏幕变绿后尽快点击，测试反应速度（毫秒级） |
| **Photo Viewer** ⭐ | `photoviewer.c/h` | 从 SD 卡读取 JPEG 幻灯片播放，支持触摸切换、自动轮播 |
| **Flip Clock** | `flip_clock.c/h` | 桌面翻页时钟 |
| **Tomato** | `tomato_timer.c/h` | 番茄钟与联网信息 |
| **Radio** | `radio_headless.c/h`, `radio_player.c/h` | 板载 ES8388 无头网络电台 |

Bird Launcher 和 Racing 已从当前 main 分支删除，不再编译也不再出现在菜单中。

主菜单 `menu.c/h` 启动后显示 Game & Photo Center，触摸卡片选择应用。每个应用内有 "Back" 按钮可返回。

---

| 限制 | 说明 |
|------|------|
| 音频 | 与 4.3" RGB LCD 引脚冲突；Radio 会先关闭/释放 LCD，再进入 headless 播放 |
| 触摸 | 仅支持单点触摸（GT9xxx 已配置为单点模式） |
| 存储 | 2048/Reaction 纯内存运行；Photo Viewer 依赖 SD 卡 |
| 最佳分数 | 未做 NVS 持久化，复位后清零 |

---

## 🖼️ Photo Viewer（照片幻灯片）

从 **SD 卡**读取 JPEG 照片，全屏幻灯片播放。

### 硬件要求
- **SD 卡**：FAT32 格式，插入开发板 SD 卡槽（SPI2 接口）
- **照片目录**：`/PHOTOS/*.jpg` 或根目录 `/*.jpg`
- **照片格式**：Baseline JPEG（手机/相机默认输出）

### 操作方式
| 手势 | 功能 |
|------|------|
| 左滑 | 下一张 |
| 右滑 | 上一张 |
| 单击 | 暂停 / 继续自动播放 |
| 左上角 Back | 返回主菜单 |

### 照片显示策略
- **大图片**（>800×480）：智能降采样 + 居中裁剪填满全屏（Cover 模式）
- **小图片**（<800×480）：Nearest-neighbor 拉伸填满全屏
- **推荐比例**：屏幕比例为 **5:3**（800×480），照片裁剪成 **5:3**（如 1600×960、4000×2400）可完整显示、不被裁剪

### 关键技术点
- **JPEG 解码**：TJpgDec（Tiny JPEG Decompressor），`JD_FORMAT=1` 输出 RGB565
- **双缓冲**：双 PSRAM Canvas（800×480×2 × 2 ≈ 1.5MB），300ms 淡入淡出切换
- **中文文件名**：`sdkconfig` 启用 FatFS LFN + Codepage 936（GBK）
- **ESP-IDF v5.5 兼容**：`sdmmc_host_t` 需补充 `.check_buffer_alignment` 字段

### 新增/修改的文件
```
main/APP/photoviewer.c/h      # 照片查看器核心
main/APP/tjpgd.c/h            # TJpgDec 解码器
components/BSP/SPI/spi.c/h    # SPI2 总线初始化（防重复初始化）
components/BSP/SDIO/spi_sdcard.c/h  # SD 卡驱动（ESP-IDF v5.5 适配）
components/BSP/CMakeLists.txt  # 添加 SDIO/SPI 到构建
main/APP/menu.c               # 添加 Photo Viewer 菜单卡片
```

---

## 🔄 基于本项目开发新游戏/软件的通用流程

> 本项目是一个**开箱即用的 LVGL 模板**。开发新软件时，不要从零新建项目，直接复制本仓库，改代码即可。

### 1. 复制项目到纯英文路径
```powershell
# 例如做一个俄罗斯方块
Copy-Item -Recurse D:\esp_project\2048 D:\esp_project\tetris
cd D:\esp_project\tetris
```

### 2. 清理旧编译产物
```powershell
idf.py fullclean
```

### 3. 替换入口函数
打开 `main/APP/lvgl_demo.c`，把 `menu_start()` 换成你的新入口，或在 `menu.c` 中添加新的游戏卡片：

```c
// 方式1: 直接替换入口（跳过菜单）
// menu_start();          // 注释掉菜单
mygame_start();           // 直接启动你的游戏

// 方式2: 在菜单中添加新卡片
// 在 menu.c 中仿照现有卡片添加 create_card() 调用
```

在 `main/APP/` 下新建 `tetris.c` / `tetris.h`，实现 `void tetris_start(void)`。

### 4. 编译 & 烧录
```powershell
C:\Users\Administrator\esp-idf\export.ps1
idf.py build
idf.py -p COM10 flash
```

---

## 🛠️ 常见需求速查（给 AI）

| 需求 | 操作 |
|------|------|
| **加新图片/字体** | 用 [LVGL Font Converter](https://lvgl.io/tools/fontconverter) 生成 `.c` 文件放到 `main/APP/`，`main/CMakeLists.txt` 已包含 `APP` 目录，无需额外修改 |
| **加 WiFi / 网络** | 代码里直接 `#include "esp_wifi.h"` 调用；如需改配置可用 `idf.py menuconfig` |
| **换屏幕（SPI 屏）** | 需替换 `components/BSP/RGBLCD/` 相关驱动，建议 copy 正点原子对应屏幕例程的 BSP |
| **音频（I2S）** | ⚠️ 4.3" RGB LCD 和板载 ES8388 **引脚冲突**，不能同时用。换 SPI 屏后才可用音频 |

### Network Radio audio note

The Radio card now opens the visible Radio page instead of the old headless ES8388 test. In the default 4.3" RGB LCD build it will not initialize onboard ES8388, because GPIO3/GPIO9/GPIO10/GPIO14/GPIO46 are already used by RGB LCD signals. This cannot be fixed in software while keeping the 4.3" RGB LCD active.

To play sound, enable `Network Radio -> Enable external I2S DAC for Radio` in menuconfig and wire an external DAC to the configurable non-conflicting defaults:

| Signal | Default GPIO |
|--------|--------------|
| BCLK | GPIO35 |
| LRCK / WS | GPIO36 |
| DOUT / DIN on DAC | GPIO37 |
| MCLK | disabled |

Use the Radio page's bottom-center beep test first. If the beep has no sound, fix the external DAC wiring before debugging station URLs.

Radio debug order:

1. Enable `Network Radio -> Enable external I2S DAC for Radio`.
2. Wire BCLK GPIO35, LRCK/WS GPIO36, DOUT/DIN GPIO37, GND, and DAC VCC.
3. Enter Radio and run the bottom-center 440Hz beep test.
4. Test the built-in English MP3 direct station.
5. Test a built-in Chinese MP3 direct station such as `CNR China Voice` or `Chinese Classics 500`.
6. Test remote `stations.json` only after built-in playback works.

Common Radio failures: external DAC disabled, DAC wiring error, station URL returns HTML/JSON instead of audio, HLS/m3u8 unsupported, WiFi offline, TLS certificate trouble, unsupported MP3 stream, or I2S GPIO collision with GPIO3/GPIO9/GPIO10/GPIO14/GPIO46.
| **改 sdkconfig** | `idf.py menuconfig` 图形化配置，或手动改 `sdkconfig` |
| **崩溃定位** | 记下串口 `Backtrace:` 地址，执行 `xtensa-esp32s3-elf-addr2line.exe -e build/lvgl.elf <地址>` |

---

## 🐛 编译/烧录问题速查（给 AI）

| 现象 | 解决 |
|------|------|
| `UnicodeDecodeError` | 路径有中文，复制到 `D:\esp_project\xxx` |
| `file format not recognized` / `.a` 损坏 | `idf.py fullclean`，再不行删掉 `managed_components` |
| `Guru Meditation Error` / 循环重启 | 看 `Backtrace` 用 `addr2line` 定位；重点检查是否访问了已删除的 LVGL 对象 |
| 烧录失败 / 连接超时 | 按住 BOOT 再按 RST 进入下载模式；检查 COM 口号；换 USB 线 |
| 触摸没反应 | 确认 GT9xxx 初始化日志 `CTP:1158` 已输出；检查 `lv_port_indev_init()` |

---

## 📝 给 AI / 大模型的上下文

如果你是 AI Agent，接到用户说"帮我编译/修改这个仓库"，请务必注意：

1. **先问路径**：确认项目是否在纯英文路径下。如果还在中文路径（如 `【正点原子】...`），必须建议用户复制到 `D:\esp_project\xxx`。
2. **激活环境**：Windows 下先执行 `C:\Users\Administrator\esp-idf\export.ps1`。
3. **编译前检查**：如果用户之前编译失败过，先执行 `idf.py fullclean`。
4. **烧录端口**：默认 COM10，波特率 460800。
5. **崩溃调试**：如果遇到 `Guru Meditation Error`，用 `xtensa-esp32s3-elf-addr2line.exe -e build/lvgl.elf <PC地址>` 解码回溯。
6. **LVGL 屏幕管理**：不要对当前活动屏幕做 `lv_obj_clean()` 来替换 UI，除非你能确保所有回调/指针都已更新。推荐用 `lv_obj_create(NULL)` + `lv_scr_load()`。
7. **LVGL v8 触摸事件**：`lv_btn` 内的 `lv_label` 会拦截触摸事件（因默认 `LV_OBJ_FLAG_CLICKABLE`）。必须在 Label 上也注册事件回调，或使用全屏透明触摸层 + 坐标判断方案。
8. **菜单架构**：主菜单在默认屏幕（`lv_scr_act()`）上构建，各应用创建独立屏幕。返回菜单用 `menu_go_back()` 切换回默认屏。
9. **Photo Viewer 屏幕管理**：双 Canvas 固定 z-order，通过 opacity 动画切换。**不要**用 `lv_obj_move_foreground()`，否则会遮挡 topbar/触摸层。
10. **开发新应用**：以本仓库为模板，复制 → 在 `menu.c` 中添加卡片 → 新建 `main/APP/xxx.c` 实现新应用 → `idf.py fullclean && build && flash`。
