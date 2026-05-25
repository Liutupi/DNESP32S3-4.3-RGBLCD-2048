# DNESP32S3 4.3 RGBLCD Headless ES8388 Radio Handoff

Last updated: 2026-05-25

This file is the current handoff for the onboard ES8388 network radio work.
Read this first before continuing the next debugging session.

## Current Repository State

- GitHub repo: `https://github.com/Liutupi/DNESP32S3-4.3-RGBLCD-2048.git`
- Current source of truth for build/flash: `docs/CURRENT_FIRMWARE_FLASH.md`
- Active local macOS build path:
  `/Volumes/liutupi/DNESP32S3-4.3-RGBLCD-2048`
- Recommended Windows build path: `C:\espbuild\dnesp32s3-2048-radio`
- Original materials path: the DNESP32S3 vendor materials directory on `D:`;
  use the short ASCII build path above for ESP-IDF commands.
- Serial port used for the latest successful macOS flash: `/dev/cu.usbserial-21230`
- Earlier Windows serial port: `COM12`
- Build command: `idf.py build`
- Flash command on macOS: `idf.py -p /dev/cu.usbserial-21230 -b 460800 flash`
- Latest flashed radio tags: `SELFTEST V4`, `STREAMDIAG V4`, `STREAMPLAY V4`

The latest V4 build was compiled successfully and flashed to
`/dev/cu.usbserial-21230`.

Bird Launcher and Racing were removed from the current main branch. Do not
re-add those modules unless the user explicitly asks for them.

## Active Audio Strategy

The active path is onboard ES8388 headless radio:

1. Start from WarmOS menu.
2. Tap `Radio`.
3. Firmware checks WiFi.
4. Firmware shuts down LVGL/RGB LCD and turns off the backlight.
5. Firmware resets the RGB/ES8388 conflict GPIOs.
6. Firmware initializes I2C, XL9555, ES8388, and I2S.
7. Firmware plays two beep passes to prove the speaker path.
8. Firmware restores the display for visible HTTP/MP3 stream diagnostics.
9. Short BOOT starts `STREAMDIAG`.
10. After one stream is verified, short BOOT enters headless `STREAMPLAY`.

Do not mix this up with the old external-I2S-DAC route. The current goal is to
finish the onboard ES8388 path after the RGB LCD has been released.

## Hard Constraints

- Do not modify `main/APP/board_audio.c` unless a future test proves the
  verified hardware audio chain is wrong.
- The current `board_audio.c` path has already passed the beep test.
- A working beep means ES8388, XL9555, I2S, and LCD-release/restore are good
  enough to continue debugging the network stream path.
- Do not use GPIO42 as a normal GPIO. It is I2C SCL.
- Do not push local `sdkconfig` if it contains WiFi or API-key values.

Known fixed pins:

| Signal | GPIO |
| --- | --- |
| I2S MCLK | GPIO3 |
| I2S BCLK | GPIO46 |
| I2S LRCK | GPIO9 |
| I2S DOUT / ESP32 TX -> ES8388 DSDIN | GPIO10 |
| I2S DIN / ESP32 RX <- ES8388 ASDOUT | GPIO14 or unused |
| I2C SDA | GPIO41 |
| I2C SCL | GPIO42 |
| SPK_EN | XL9555 bit2, active low |

## Current Code Map

- `main/APP/menu.c`
  - The Radio menu entry calls `radio_headless_start()`.
- `main/APP/radio_headless.c`
  - Owns the visible self-test / stream-diagnostic / headless-play flow.
  - Current tags are V4.
  - Long BOOT exits headless play.
  - Short BOOT starts stream diagnostics or starts playback after a stream is
    verified.
  - Short BOOT while already playing requests next station, but V4 refuses to
    stack a new player if the old player did not stop cleanly.
- `main/APP/radio_player.c`
  - Owns HTTP open, MP3 sync scanning, MP3 decode, PCM validation, and I2S
    writes through `board_audio_i2s_write()`.
  - V4 tightened MP3 sync detection so random `0xFFE` bytes are not accepted as
    a real frame start.
  - V4 logs `PCM_FRAME` with sample rate, bits, channels, bitrate, average
    absolute sample value, and peak.
  - V4 no longer reports `RADIO_PLAYING` after only one I2S write. It now
    requires decoded pre-roll frames plus several good PCM writes.
  - Headless playback applies `RADIO_PCM_GAIN_Q15` before I2S writes. Keep
    this below full scale if the speaker sounds clipped.
- `main/APP/board_audio.c`
  - Verified hardware layer. The successful headless build uses GPIO10 as
    I2S DOUT to ES8388 DSDIN and GPIO14 as I2S DIN from ES8388 ASDOUT.
    Do not swap these when debugging streams.
- `components/BSP/ES8388/`
  - ES8388 driver used by `board_audio.c`.

## Pitfalls Already Hit

1. Wrong checkout or wrong path

   There are several similar ESP32/DNESP32S3 projects on this machine. Always
   confirm the real checkout first. The current short build path is
   `C:\espbuild\dnesp32s3-2048-radio`.

2. Long or Chinese path issues on Windows

   ESP-IDF tools on Windows are much more reliable from a short ASCII path.
   Use the short path above for build and flash.

3. RGB LCD and ES8388 share pins

   LCD and onboard ES8388 cannot run at the same time on this 4.3-inch RGB LCD
   board. The correct onboard-codec route is headless: release LCD first, then
   start ES8388/I2S.

4. Mistaking hardware failure for stream failure

   The beep test already proved the hardware chain. If beep works and stream
   hisses, the next suspect is decoded PCM, stream content, task concurrency, or
   decoder/I2S format assumptions, not the ES8388 init sequence.

5. False MP3 sync

   Previous code accepted weak `0xFFE` sync-like bytes. That produced
   `decode failed err=-1 raw=16377 sync=0 tried 5 streams` and could hide the
   real stream problem. V4 requires a valid MP3 header and a following valid
   frame.

6. Treating one I2S write as success

   Earlier playback could say "playing" after the first write even if the
   output was only hiss. V4 waits for more decoded frames and logs PCM details
   before announcing `RADIO_PLAYING`.

7. Player task stacking

   If BOOT short-press starts a new station while the old HTTP/decode task is
   still alive, two tasks can fight over the same audio path. V4 waits longer
   for stop and refuses to start a new player if the old one is still running.

8. ES8388 SDIN / SDOUT direction

   The schematic labels are from the ES8388 side. `I2S_SDIN` is data input
   into ES8388, so ESP32 playback must drive it as I2S DOUT on GPIO10.
   `I2S_SDOUT` is ES8388 ADC data output, so it is ESP32 I2S DIN on GPIO14.
   A bad build had these reversed in `board_audio.c`, which made headless
   playback fail even though the rest of the audio path was close.

## Last Known Good

- Date: 2026-05-25
- Port used for hardware flash test: `/dev/cu.usbserial-21230`
- Hardware: DNESP32S3 4.3 inch RGB LCD board, onboard ES8388 path
- Result: build, flash, and headless radio playback succeeded after fixing
  GPIO10/14 I2S data direction in `main/APP/board_audio.c`.

9. COM12 left busy by monitor

   `idf.py monitor` can keep COM12 open after a timeout. If flash says
   `PermissionError(13, refused access)`, find and stop only the stale COM12
   monitor/esptool processes, then flash again.

10. Speaker clipping / overload

   `RADIO_HEADLESS_VOLUME` is an ES8388 output volume value with 33 as the
   driver clamp. Full scale can overload the small onboard speaker path and
   sound like clipping. The current balanced starting point is volume 28 plus
   `RADIO_PCM_GAIN_Q15` at 0.85x in `radio_player.c`. If clipping returns,
   first lower the PCM gain before lowering codec volume.

## Latest User-Visible Symptom

Before V4, the device reached headless stream play and then stopped at steady
hissing / static with no useful response.

That means the firmware got far enough to turn off the LCD, initialize the
speaker path, play the headless pre-play beep, begin stream playback, and write
something to I2S. It did not yet prove that the bytes written to I2S are
intelligible PCM.

## How To Test The Current V4 Build

1. Boot into the main WarmOS menu.
2. Confirm Tomato Clock WiFi is connected.
3. Tap `Radio`.
4. Screen should show `Beep OK / SELFTEST V4`.
5. Short BOOT starts `STREAMDIAG V4`.
6. If stream diagnostic succeeds, short BOOT starts `STREAMPLAY V4`.
7. Screen goes off, beep plays, then the verified stream starts.
8. Long BOOT should exit headless play and restore the display.

## What To Record Next

If it fails on the visible screen:

- Record the last line starting with `Last reason:`.
- Record which tag is shown: `SELFTEST V4`, `STREAMDIAG V4`, or
  `STREAMPLAY V4`.

If it enters black-screen headless play and only hisses:

- Capture serial logs around:
  - `RADIO_URL_TRY`
  - `HTTP status=`
  - `MP3_SYNC_SKIP`
  - `MP3_DECODER_START_OK`
  - `PCM_FRAME`
  - `I2S_WRITE_OK`
  - `RADIO_PLAYING`
  - `RADIO_STOP_TIMEOUT`
- The most important line is `PCM_FRAME`, especially `rate`, `bits`, `ch`,
  `bitrate`, `avg_abs`, and `peak`.

## Next Debugging Goal

The next session should decide whether the hiss is caused by stream/decoder
data or by I2S/codec data format.

Use this decision tree:

1. `no_valid_mp3_sync`

   The URL is not a clean direct MP3 stream, or the stream contains data the
   current sync scanner is rejecting. Fix station selection or stream parsing in
   `radio_player.c`.

2. `decode failed` or repeated `decode_wait`

   The decoder is not getting a stable frame stream. Keep changes inside
   `radio_player.c`; improve buffering, resync, or fallback URL handling.

3. `PCM_FRAME` has unexpected `rate`, `bits`, or `ch`

   The decoder output format is not what the I2S write path assumes. Add
   conversion or reject the stream before writing to I2S.

4. `PCM_FRAME` looks normal but output is still hiss

   Compare decoded PCM playback against a generated PCM tone through the exact
   same `board_audio_i2s_write()` path. Since beep is good, do not edit
   `board_audio.c` first; add a small diagnostic in `radio_player.c` or
   `radio_headless.c` to write a known PCM pattern immediately before/after
   decoded stream frames.

5. `RADIO_STOP_TIMEOUT`

   The player task did not exit cleanly. Keep the fix in `radio_player.c` by
   making HTTP read/decode more interruptible, then retry BOOT short/long press.

## Files That Should Be Treated Carefully

- `main/APP/board_audio.c`
  - Current verified hardware chain. Do not change casually.
- `sdkconfig`
  - Local build config may contain WiFi/API values. Do not push unsanitized.
- `main/APP/radio_player.c`
  - Safe place for stream, decode, PCM validation, and logging fixes.
- `main/APP/radio_headless.c`
  - Safe place for flow control, visible diagnostics, BOOT behavior, and
    headless state handling.

## Known Good Build/Flash Baseline

```powershell
cd C:\espbuild\dnesp32s3-2048-radio
& 'C:\Users\Administrator\esp-idf\export.ps1'
idf.py build
idf.py -p COM12 -b 460800 flash
```

macOS:

```bash
cd /Volumes/liutupi/DNESP32S3-4.3-RGBLCD-2048
. ~/esp/esp-idf-v5.5/export.sh
idf.py build
idf.py -p /dev/cu.usbserial-21230 -b 460800 flash
```

Latest local verification before this handoff:

- `idf.py build`: passed.
- `idf.py -p /dev/cu.usbserial-21230 -b 460800 flash`: passed.
- Flash target: ESP32-S3, MAC `14:c1:9f:42:2f:94`.
- App binary size after removing Bird/Racing: about `0x1cc0e0`, app partition
  has about `0x23f20` bytes free.
