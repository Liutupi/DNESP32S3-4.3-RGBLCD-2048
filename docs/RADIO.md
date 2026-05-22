# Radio / Network Radio

WarmOS now includes a Radio app in the Game Center menu.

## Current RGB LCD limitation

The DNESP32S3 4.3 inch RGB LCD build cannot safely use the board ES8388 audio path. The RGB LCD uses GPIO3, GPIO9, GPIO10, GPIO14, and GPIO46, which overlap the ES8388 I2S MCLK, LRCK, SDIN, SDOUT, and BCLK/SCK pins. Initializing board audio in this build can break the LCD display or produce unstable audio.

For this reason the Radio app does not initialize ES8388 or I2S. A successful stream probe is shown as:

`Stream OK, audio unavailable on RGB LCD build`

## Implemented

- WarmOS Radio UI for 800x480.
- Built-in station list with MP3 direct URLs and fallback URLs.
- Remote `stations.json` download from GitHub raw when WiFi is connected.
- Built-in English test station is kept, and remote stations are appended with name/URL dedupe.
- Short HTTP MP3 probe with clear serial logs.
- Unsupported URL guard for `.m3u8`, `.aac`, `.flv`, and tokenized URLs.
- Reserved audio abstraction:
  - `radio_audio_available()`
  - `radio_audio_start()`
  - `radio_audio_write_pcm()`
  - `radio_audio_stop()`

## Touch controls

- Tap left area or swipe right: previous station.
- Tap right area or swipe left: next station.
- Tap center area: probe current station and fallbacks.
- Back: return to the WarmOS menu.

## Remote stations format

```json
{
  "stations": [
    {
      "name": "CNR中国之声",
      "category": "新闻",
      "url": "https://lhttp.qtfm.cn/live/15318317/64k.mp3",
      "fallback_urls": [
        "https://lhttp-hw.qtfm.cn/live/15318317/64k.mp3"
      ],
      "type": "mp3",
      "enabled": true
    }
  ]
}
```

Only MP3 direct stations are enabled by default. HLS examples may remain in JSON as disabled examples, but HLS playback is not supported in this build.

## To make it actually play audio later

Use an output path that does not reuse the RGB LCD pins, for example:

- an external I2S DAC wired to non-conflicting GPIOs,
- Bluetooth A2DP,
- USB audio if the target build supports it,
- or a SPI LCD build where the ES8388 I2S pins are free.
