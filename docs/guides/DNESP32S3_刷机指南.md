# DNESP32S3 开发板刷机指南

## 一、硬件环境

### 开发板
- **型号**: 正点原子 DNESP32S3
- **芯片**: ESP32-S3 (QFN56, rev v0.2)
- **Flash**: 16MB (QIO)
- **PSRAM**: 8MB Octal (AP_3v3, 80MHz)
- **晶体**: 40MHz
- **MAC**: 14:c1:9f:42:2f:94

### 串口连接
- **最近验证设备路径**: `/dev/cu.usbserial-21230`
- **旧记录设备路径**: `/dev/cu.usbserial-21220`
- **波特率**: 115200 (刷机用 460800)
- **USB芯片**: CH340 (VID:PID=1A86:7523)
- **复位方式**: DTR 拉低再拉高可触发硬件复位
- **UART0引脚**: TX=GPIO43, RX=GPIO44

### 开发环境
- **ESP-IDF 版本**: v5.5.2
- **安装路径**: `~/esp/esp-idf-v5.5`
- **激活命令**: `source ~/esp/esp-idf-v5.5/export.sh`
- **Python**: 3.14.4

---

## 二、DNESP32S3 完整 IO 引脚分配表

### RGB LCD (ATK-4384 4.3寸 800x480) — DE模式，无HSYNC/VSYNC

| 信号 | GPIO | 说明 |
|------|------|------|
| DE | IO4 | 数据使能信号 |
| PCLK | IO5 | 像素时钟 |
| B7 | IO6 | 蓝色最高位 |
| B6 | IO7 | |
| B5 | IO15 | |
| B4 | IO16 | |
| B3 | IO17 | |
| G7 (M1) | IO18 | 绿色最高位 (也用做LCD ID读取) |
| G6 | IO8 | |
| G5 | IO3 | |
| G4 | IO46 | |
| G3 | IO9 | |
| G2 | IO10 | |
| R7 (M0) | IO14 | 红色最高位 (也用做LCD ID读取) |
| R6 | IO21 | |
| R5 | IO47 | |
| R4 | IO48 | |
| R3 | IO45 | |
| B7 (M2) | IO6 | LCD ID读取位2 |
| 背光 | XL9555 Bit8 | 通过XL9555 IO扩展器控制 |
| HSYNC | NC | DE模式不需要 |
| VSYNC | NC | DE模式不需要 |

### ATK-4384 时序参数
- PCLK: 20MHz
- H_RES: 800, V_RES: 480
- HSYNC: 48, HBP: 88, HFP: 40
- VSYNC: 3, VBP: 32, VFP: 13
- pclk_active_neg: true (下降沿采样)

### I2C 总线 (I2C_NUM_0)
- SDA: IO41
- SCL: IO42
- 设备: ES8388 (0x10), XL9555 (0x20)

### XL9555 IO扩展器 (I2C地址 0x20)
| 位 | 功能 | 说明 |
|----|------|------|
| Bit0 | AP_INT | 三轴传感器中断 |
| Bit1 | QMA_INT | QMA6100P中断 |
| Bit2 | SPK_EN | 喇叭使能 |
| Bit3 | BEEP | 蜂鸣器 |
| Bit4 | OV_PWDN | 摄像头掉电 |
| Bit5 | OV_RESET | 摄像头复位 |
| Bit6 | GBC_LED | LED |
| Bit7 | GBC_KEY | 按键 |
| **Bit8** | **LCD_BL** | **LCD背光** |
| Bit9 | CT_RST | 触摸复位 |
| Bit10 | SLCD_RST | SPI LCD复位 |
| Bit11 | SLCD_PWR | SPI LCD电源 |
| Bit12-15 | KEY0-3 | 扩展按键 |

### I2S 音频 (ES8388)
- MCLK: IO3
- LRCK/WS: IO9
- BCLK/SCK: IO46
- ES8388 SDIN / DSDIN: IO10，ESP32-S3 作为 I2S DOUT 输出到 ES8388，用于喇叭播放
- ES8388 SDOUT / ASDOUT: IO14，ESP32-S3 作为 I2S DIN 从 ES8388 输入

注意：原理图信号名是站在 ES8388 角度命名。做喇叭播放时，ESP32 必须从
GPIO10 输出音频到 ES8388 SDIN，不要把 GPIO10 和 GPIO14 反接或在代码里互换。

### 其他功能引脚
| 功能 | GPIO | 说明 |
|------|------|------|
| LED | IO1 | 板载LED |
| BOOT按钮 | IO0 | 启动按键 |
| SPI LCD CS | IO21 | SPI屏片选 |
| SPI LCD DC | IO40 | SPI屏数据/命令 |
| SPI SCK | IO12 | |
| SPI MOSI | IO11 | |
| SPI MISO | IO13 | |
| 触摸 INT | IO40 | CT_INT |
| 触摸 SDA | IO39 | CT_SDA |
| 触摸 SCL | IO38 | CT_SCL |
| 触摸 CS | IO2 | CT_CS |
| 红外接收 | IO2 | REMOTE_IN |
| 红外发送 | IO8 | REMOTE_OUT |
| SD卡 CS | IO2 | TF_CS |
| 摄像头 D0-D7 | IO4~IO18 | 与RGB LCD共享 |
| UART0 RX | RX0 | |
| UART0 TX | TX0 | |
| 不可用 | IO35,36,37 | PSRAM占用 |

---

## 三、GPIO 冲突说明 ⚠️

### RGB LCD vs I2S 音频冲突
DNESP32S3 的 **RGB LCD 和音频不能同时使用**，以下引脚被共享：

| GPIO | RGB LCD | I2S音频 | 
|------|---------|---------|
| IO3 | LCD_G5 | MCLK |
| IO9 | LCD_G3 | LRCK |
| IO10 | LCD_G2 | SDIN(麦克风) |
| IO14 | LCD_R7 | SDOUT(喇叭) |
| IO46 | LCD_G4 | BCLK/SCK |

后果：接RGB LCD时喇叭有爆音、麦克风无法工作。

解决方案：使用SPI LCD（GPIO11-13,40,21）则不冲突。

---

## 四、刷机命令速查

### 环境激活（每次新终端必需）
```bash
source ~/esp/esp-idf-v5.5/export.sh
```

### 当前仓库固件（DNESP32S3-4.3-RGBLCD-2048）
```bash
cd /Volumes/liutupi/DNESP32S3-4.3-RGBLCD-2048
source ~/esp/esp-idf-v5.5/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-21230 -b 460800 flash
```

如果串口号不同：
```bash
ls /dev/cu.* | grep -E 'usb|wch|serial|SLAB|ACM|modem'
```

### 小智AI工程 (ATK-4384 RGB LCD版)
```bash
cd ~/xiaozhi-esp32
idf.py set-target esp32s3
idf.py -p /dev/cu.usbserial-21220 -b 460800 flash
```

### 小智AI工程 (SPI LCD版，音频可用)
在 `~/xiaozhi-esp32/sdkconfig` 中将：
```
CONFIG_BOARD_TYPE_ATK_DNESP32S3=y
```
改为：
```
CONFIG_BOARD_TYPE_ATK_DNESP32S3_BOX=y
```

### 读取串口日志（DTR复位方式）
```bash
python3 -c "
import serial, time
ser = serial.Serial('/dev/cu.usbserial-21220', 115200, timeout=0.1)
ser.dtr = False; time.sleep(0.3)
ser.dtr = True; time.sleep(0.3)
ser.dtr = False
while time.time() - start < 20:
    c = ser.read(1)
    if c: print(c.decode('utf-8', errors='replace'), end='', flush=True)
"
```

### 使用 idf.py monitor（推荐）
```bash
cd ~/xiaozhi-esp32
source ~/esp/esp-idf-v5.5/export.sh
idf.py -p /dev/cu.usbserial-21220 monitor
```

### 手动 esptool 刷写
```bash
source ~/esp/esp-idf-v5.5/export.sh
esptool.py --chip esp32s3 -p /dev/cu.usbserial-21220 -b 460800 \
  --before default_reset --after hard_reset write_flash \
  0x0 bootloader/bootloader.bin \
  0x20000 xiaozhi.bin \
  0x8000 partition_table/partition-table.bin \
  0xd000 ota_data_initial.bin \
  0x800000 generated_assets.bin
```

---

## 五、RGB LCD 基础驱动工程

路径: `~/esp32_rgb_lcd/`
- 基于ESP-IDF rgb_panel示例
- 使用LVGL v9.2.0
- 引脚可通过 menuconfig 调整
- 刷新率约42Hz (18MHz PCLK)

---

## 六、重要文件和路径

| 路径 | 说明 |
|------|------|
| `~/esp32_rgb_lcd/` | RGB LCD基础驱动项目 |
| `~/esp32_rgb_lcd/main/main.c` | 主程序 |
| `~/esp32_rgb_lcd/main/Kconfig.projbuild` | 引脚配置菜单 |
| `~/xiaozhi-esp32/` | 小智AI工程 |
| `~/xiaozhi-esp32/main/boards/atk-dnesp32s3/` | DNESP32S3板级配置 |
| `~/xiaozhi-esp32/main/boards/atk-dnesp32s3/config.h` | 引脚定义 |
| `~/xiaozhi-esp32/main/boards/atk-dnesp32s3/atk_dnesp32s3.cc` | 板级初始化代码 |
| `/Volumes/liutupi/...A盘/` | 正点原子官方资料（原理图、例程） |

---

## 七、已知问题和注意事项

1. **DTR vs RTS 复位**: 该开发板用 DTR 复位，RTS 不可靠
2. **刷写波特率**: CH340在921600不稳定，建议用460800
3. **C++ 结构体初始化顺序**: 必须严格按头文件声明的字段顺序
4. **DE模式**: ATK-4384用DE模式，HSYNC/VSYNC必须设为GPIO_NUM_NC
5. **背光控制**: 通过XL9555 (I2C 0x20) 的Bit8控制，非直接GPIO
6. **LVGL 9.x**: ESP-IDF v5.5 自带 LVGL 9.2.0，API与v8不同
7. **PSRAM**: 必须启用，800x480双缓冲需要1.5MB，内部RAM不够
