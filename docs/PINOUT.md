# DNESP32S3 IO 管脚分配表

> 来源：正点原子 DNESP32-S3 IO管脚分配表.xlsx

| 序号 | GPIO | 默认功能 | 复用功能 | 是否安全启动可用 | 功能说明 |
|------|------|----------|----------|------------------|----------|
| 4 | IO4 | LCD_DE | OV_D0 | N | 1，	RGBLCD的DE信号<br>2，	摄像头的D0信号 |
| 5 | IO5 | LCD_CLK | OV_D1 | N | 1，	RGBLCD的CLK信号<br>2，	摄像头的D1信号 |
| 6 | IO6 | LCD_B7 | OV_D2 | N | 1，	RGBLCD的B7信号<br>2，	摄像头的D2信号 |
| 7 | IO7 | LCD_B6 | OV_D3 | N | 1，	RGBLCD的B6信号<br>2，	摄像头的D3信号 |
| 8 | IO15 | LCD_B5 | OV_D4 | N | 1，	RGBLCD的B5信号<br>2，	摄像头的D4信号 |
| 9 | IO16 | LCD_B4 | OV_D5 | N | 1，	RGBLCD的B4信号<br>2，	摄像头的D5信号 |
| 10 | IO17 | LCD_B3 | OV_D6 | N | 1，	RGBLCD的B3信号<br>2，	摄像头的D6信号 |
| 11 | IO18 | LCD_G7 | OV_D7 | N | 1，	RGBLCD的G7信号<br>2，	摄像头的D7信号 |
| 12 | IO8 | LCD_G5、ADC_IN | REMOTE_OUT | N | 1，	RGBLCD的G6信号<br>2，	ADC输入信号<br>3，	红外发送信号 |
| 13 | IO19 | USB_D- | USB_D- | N | USB |
| 14 | IO20 | USB_D+ | USB_D+ | N |  |
| 15 | IO3 | LCD_G5 | I2S_MCLK | N | 1，	RGBLCD的G5信号<br>2，	音频的MCLK信号 |
| 16 | IO46 | LCD_G4 | I2S_SCK | N | 1，	RGBLCD的G4信号<br>2，	音频的SCK信号 |
| 17 | IO9 | LCD_G3 | I2S_LRCK | N | 1，	RGBLCD的G3信号<br>2，	音频的LRCK信号 |
| 18 | IO10 | LCD_G2 | I2S_SDIN | N | 1，	RGBLCD的G2信号<br>2，	音频的SDIN信号 |
| 19 | IO11 | SPI_MOSI | SPI_MOSI | N | SPI2口的MOSI信号 |
| 20 | IO12 | SPI_SCK | SPI_SCK | N | SPI2口的SCK信号 |
| 21 | IO13 | SPI_MISO | SPI_MISO | N | SPI2口的MISO信号 |
| 22 | IO14 | LCD_R7 | I2S_SDOUT | N | 1，	RGBLCD的R7信号<br>2，	音频的SDOUT信号 |
| 23 | IO21 | LCD_R6 | LCD_CS | N | 1，	RGBLCD的R6信号<br>2，	SPILCD的CS信号 |
| 24 | IO47 | LCD_R5 | OV_VSYNC | N | 1，	RGBLCD的R5信号<br>2，	摄像头的VSYNC信号 |
| 25 | IO48 | LCD_R4 | OV_HREF | N | 1，	RGBLCD的R4信号<br>2，	摄像头的HREF信号 |
| 26 | IO45 | LCD_R3 | OV_PCLK | N | 1，	RGBLCD的R3信号<br>2，	摄像头的PCLK信号 |
| 27 | IO0 | IIC_INT、BOOT | 1WIRE_DQ | N | 1，	XL9555的INT信号<br>2，	BOOT按键信号<br>3，	DHT11/DS18B20信号 |
| 28 | IO35 |  |  | Y | 不可用 |
| 29 | IO36 |  |  | Y |  |
| 30 | IO37 |  |  | Y |  |
| 31 | IO38 | CT_SCL | OV_SCL | N | 1，	触摸IC的SCL信号<br>2，	摄像头的SCL信号 |
| 32 | IO39 | CT_SDA | OV_SDA | N | 1，	触摸IC的SDA信号<br>2，	摄像头的SDA信号 |
| 33 | IO40 | CT_INT | LCD_DC | N | 1，	触摸IC的INT信号<br>2，	SPILCD的DC信号 |
| 34 | IO41 | IIC_SDA | IIC_SDA | N | IIC0的SDA信号 |
| 35 | IO42 | IIC_SCL | IIC_SCL | N | IIC0的SCL信号 |
| 36 | RXD0 | U1RXD | U0RXD | N | UART0 |
| 37 | TXD0 | U1TXD | U0TXD | N |  |
| 38 | IO2 | TF_CS | REMOTE_IN | N | 1，	触摸IC的CS信号<br>2，	红外发送信号 |
| 39 | IO1 | LED1 | LED0 | N | LED信号 |
      GPIO：DNESP32S3的IO口

## 关键冲突说明

- **RGB LCD 与 I2S 音频冲突**：IO3/IO9/IO10/IO14/IO46 同时用于 RGB 数据线和 I2S 音频。使用 4.3" RGB 屏时，板载 ES8388 音频不可用。
- **ES8388 播放方向**：原理图的 `I2S_SDIN` 是 ES8388 视角的数据输入，因此 ESP32 播放应配置 `GPIO10` 为 I2S DOUT；`I2S_SDOUT` 是 ES8388 输出，因此 `GPIO14` 是 ESP32 I2S DIN。
- **摄像头复用**：部分 IO 与 OV2640/OV5640 摄像头数据线复用。
- **SD 卡**：使用 SPI 或 SDMMC 接口，具体见原理图。
