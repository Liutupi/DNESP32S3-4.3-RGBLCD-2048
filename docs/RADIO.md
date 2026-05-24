# Radio / Network Radio

Current handoff for the onboard ES8388 headless path:
`docs/RADIO_HEADLESS_HANDOFF.md`.

Note: older sections in this file describe the previous external-I2S-DAC plan.
The current active route is onboard ES8388 after the RGB LCD is shut down and
its conflicting GPIOs are released.

WarmOS now includes a Radio app in the Game Center menu.

## Current RGB LCD limitation

The DNESP32S3 4.3 inch RGB LCD build cannot safely use the board ES8388 audio path. The RGB LCD uses GPIO3, GPIO9, GPIO10, GPIO14, and GPIO46, which overlap the ES8388 I2S MCLK, LRCK, SDIN, SDOUT, and BCLK/SCK pins. Initializing board audio in this build can break the LCD display or produce unstable audio.

For this reason the Radio app does not initialize ES8388. The old headless ES8388 test is no longer the menu entry.

Default builds report the real hardware state instead of pretending to play:

`当前 4.3 寸 RGB LCD 模式下，板载 ES8388 I2S 引脚冲突，无法直接播放音频。请使用外接 I2S DAC 重新指定空闲 GPIO，或改用蓝牙音频输出，或更换 SPI 屏。`

## Implemented

- WarmOS Radio UI for 800x480.
- Built-in station list with MP3 direct URLs and fallback URLs.
- Remote `stations.json` download from GitHub raw when WiFi is connected.
- Built-in English test station is kept, and remote stations are appended with name/URL dedupe.
- Visible Connecting / Playing / Failed status.
- HTTP redirect and HTTPS support via `esp_http_client` and certificate bundle.
- HTTP status, Content-Type, length, and selected URL logs.
- `.m3u` / `.pls` playlist parsing for a direct audio URL.
- HLS `.m3u8` detection with a clear unsupported message.
- MP3 decode through `esp_audio_codec`.
- Optional external I2S DAC output through the ESP-IDF v5.5 standard I2S API.
- 440 Hz beep test for validating the external DAC before debugging station URLs.

## Touch controls

- Tap left area or swipe right: previous station.
- Tap right area or swipe left: next station.
- Tap center area: stop current playback, connect, decode, and play the selected station.
- Tap bottom center: 440 Hz external I2S DAC beep test.
- Back: return to the WarmOS menu.

## Remote stations format

```json
{
  "stations": [
    {
      "name": "CNR中国之声",
      "category": "新闻",
      "url": "https://lhttp.qtfm.cn/live/15318317/64k.mp3",
      "codec_hint": "mp3",
      "fallback_urls": [
        "https://lhttp-hw.qtfm.cn/live/15318317/64k.mp3"
      ],
      "enabled": true
    }
  ]
}
```

Only MP3 direct stations are enabled by default. HLS examples may remain in JSON as disabled examples, but HLS playback is not supported in this build.

## To make it actually play audio later

Use an output path that does not reuse the RGB LCD pins. The implemented and default playback path is an external I2S DAC. The onboard ES8388 path is intentionally not initialized in the 4.3 inch RGB LCD build.

Enable it in menuconfig:

```text
Network Radio -> Enable external I2S DAC for Radio
```

Default GPIO assignment:

| External DAC pin | ESP32-S3 GPIO |
|------------------|---------------|
| BCLK / SCK | GPIO35 |
| LRCK / WS | GPIO36 |
| DIN / SDIN | GPIO37 |
| MCLK | disabled by default |
| GND | GND |
| VCC | match your DAC module, usually 3V3 |

The firmware rejects GPIO3, GPIO9, GPIO10, GPIO14, and GPIO46 for Radio I2S because those are already used by the 4.3 inch RGB LCD / onboard ES8388 conflict path.

## Debug order

1. Enter Radio and tap the bottom-center 440 Hz beep test. The serial log must show:

```text
RADIO_I2S: init start
RADIO_I2S: bclk=35 lrck=36 dout=37 mclk=-1
RADIO_I2S: init ok
RADIO_BEEP: start 440Hz
RADIO_BEEP: written bytes=...
RADIO_BEEP: done
```

If this does not make sound on the external DAC, stop here and fix wiring, power, GPIO config, or the DAC module.

2. Test the built-in English MP3 direct station.
3. Test a built-in Chinese MP3 direct station. `CNR China Voice` and `Chinese Classics 500` are the current primary MP3 direct candidates, each with fallback URLs.
4. Test remote `stations.json` only after the built-in stations work.

During station playback the serial log prints station name, original URL, final redirected URL, HTTP status, Content-Type, Content-Length, codec hint, first bytes, received bytes, decoded PCM bytes, and I2S write bytes.

## Common failure causes

- External DAC is not enabled: enable `Network Radio -> Enable external I2S DAC for Radio`.
- DAC wiring or power is wrong: check GPIO35 BCLK, GPIO36 LRCK/WS, GPIO37 DOUT/DIN, GND, and VCC.
- Station URL is a web page or JSON endpoint, not an audio stream.
- HLS/m3u8 is detected but not supported in this build.
- WiFi is not connected.
- HTTPS/TLS certificate or time validation fails.
- The MP3 decoder does not support the stream.
- Radio I2S GPIO overlaps the RGB LCD / ES8388 conflict pins: GPIO3, GPIO9, GPIO10, GPIO14, GPIO46.

Other possible future paths:

- Bluetooth A2DP,
- USB audio if the target build supports it,
- or a SPI LCD build where the ES8388 I2S pins are free.
