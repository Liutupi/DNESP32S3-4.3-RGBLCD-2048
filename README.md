# DNESP32S3 2048 Game

基于 **正点原子 DNESP32S3** 开发板的触屏版 2048 游戏。

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
│       ├── game2048.c / .h     # 2048 游戏逻辑与 UI
│       └── (依赖 components/BSP 中的 LCD/TOUCH 驱动)
└── components/
    └── BSP/                    # 正点原子 BSP（RGB LCD、GT9xxx、XL9555 等）
```

**关键修改点**：
- `lvgl_demo.c` 中把原来的 `lv_demo_music()` 替换为 `game2048_start()`。
- `game2048.c` 使用 LVGL v8 的 `lv_obj_create(NULL)` + `lv_scr_load()` 创建新屏幕，**不能直接 `lv_obj_clean(lv_scr_act())`**，否则会删掉 `lv_port_indev_init()` 中创建的 `debug_label` 和 `cursor_obj`，导致 `touchpad_read` 访问悬空指针而 `LoadProhibited` 崩溃。

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

### 坑 5：音频和 RGB LCD 不能同时工作
**现象**：需要音频时发现无输出。
**根因**：硬件上 RGB LCD 和 I2S 音频共用 GPIO（IO3/IO9/IO10/IO14/IO46），详见《DNESP32S3 V1.0 硬件参考手册》和《刷机指南》。
**结论**：使用 4.3" RGB 屏时，板载 ES8388/MD8002A 音频不可用。如需音频，换用 SPI LCD（1.3"/2.4"）或外接 I2S DAC 到空闲 GPIO。

---

## 🔧 已知限制

| 限制 | 说明 |
|------|------|
| 音频 | 与 4.3" RGB LCD 引脚冲突，不可用 |
| 触摸 | 仅支持单点触摸（GT9xxx 已配置为单点模式） |
| 存储 | 无 SD 卡/文件系统依赖，纯内存运行 |
| 最佳分数 | 未做 NVS 持久化，复位后清零 |

---

## 📝 给 AI / 大模型的上下文

如果你是 AI Agent，接到用户说"帮我编译/修改这个仓库"，请务必注意：

1. **先问路径**：确认项目是否在纯英文路径下。如果还在中文路径（如 `【正点原子】...`），必须建议用户复制到 `D:\esp_project\xxx`。
2. **激活环境**：Windows 下先执行 `C:\Users\Administrator\esp-idf\export.ps1`。
3. **编译前检查**：如果用户之前编译失败过，先执行 `idf.py fullclean`。
4. **烧录端口**：默认 COM10，波特率 460800。
5. **崩溃调试**：如果遇到 `Guru Meditation Error`，用 `xtensa-esp32s3-elf-addr2line.exe -e build/lvgl.elf <PC地址>` 解码回溯。
6. **LVGL 屏幕管理**：不要对当前活动屏幕做 `lv_obj_clean()` 来替换 UI，除非你能确保所有回调/指针都已更新。推荐用 `lv_obj_create(NULL)` + `lv_scr_load()`。
