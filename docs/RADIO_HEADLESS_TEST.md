# Radio Headless Test

## 为什么必须关闭屏幕

DNESP32S3 4.3 inch RGB LCD 的 RGB 数据线与板载 ES8388 音频 I2S 引脚冲突：

| GPIO | RGB LCD | ES8388 / I2S |
|------|---------|--------------|
| IO3 | LCD_G5 | MCLK |
| IO46 | LCD_G4 | BCLK / SCK |
| IO9 | LCD_G3 | LRCK / WS |
| IO10 | LCD_G2 | ESP32 I2S data out to ES8388 |
| IO14 | LCD_R7 | ESP32 I2S data in from ES8388 |

因此本阶段进入 Radio Test 后会先停止 LVGL 刷新、关闭背光、反初始化 RGB LCD，并复位这些冲突 GPIO，然后才初始化 ES8388 和 I2S。不要尝试让 LCD 和 ES8388 同时工作。

说明：官方 `30_music` 例程将 IO10 配置为 ESP32 的 I2S `data_out`，IO14 配置为 `data_in`。部分资料按 ES8388 视角标注 SDIN/SDOUT，方向容易看反；本测试代码以官方例程的 ESP32 引脚方向为准。

## 当前阶段范围

Radio Headless Mode 阶段 1 只验证板载音频硬件链路：

- 不连接 HTTP 网络电台
- 不接入 MP3 decoder
- 不读取 `stations.json`
- 只播放 1 kHz beep / sine wave 测试音

## 如何进入

启动 WarmOS 主菜单后，点击主卡片区域的 `RADIO`。

屏幕会先显示 2 秒提示：

- `Entering Headless Radio Test`
- `LCD will turn off`
- `Hold BOOT to exit`
- `Testing ES8388 beep`

随后屏幕背光关闭，串口日志 tag 为 `RADIO_HEADLESS`。

## 如何退出

长按 BOOT / IO0 约 2 秒。串口应打印：

```text
BOOT long press detected
Stop beep
Stop I2S
Stop ES8388
Restore GPIO
```

程序会尝试重新初始化 RGB LCD、恢复 LVGL flush、打开背光并返回 WarmOS 主菜单。

## 如果屏幕没有恢复

当前阶段会尽量调用 `esp_lcd_panel_del()` 释放 RGB LCD，再调用原 BSP `lcd_init()` 恢复。若恢复失败，系统应保持不崩溃并打印：

```text
LCD restore not implemented, please reboot to return to UI.
```

此时请按 RST 或重新上电恢复 UI。

## 下一阶段

下一阶段才考虑在 headless audio path 稳定后接入 MP3 网络电台。网络、解码、站点列表和 UI 状态机都不属于本阶段。
