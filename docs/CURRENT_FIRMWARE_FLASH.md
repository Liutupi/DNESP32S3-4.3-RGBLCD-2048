# Current Firmware Flash Notes

This is the handoff file for humans and other AI agents. Read this before
building or flashing this repository.

## Repository

- GitHub: `https://github.com/Liutupi/DNESP32S3-4.3-RGBLCD-2048`
- Branch to use: `main`
- Target: `esp32s3`
- ESP-IDF: `v5.5.x`
- Board: DNESP32S3 4.3-inch RGB LCD, 16 MB flash, 8 MB PSRAM
- Last known macOS port used successfully: `/dev/cu.usbserial-21230`
- Common Windows port from earlier testing: `COM12`
- Latest verified app binary size: `0x1cc0e0`
- Latest verified free app partition space: `0x23f20`

Always clone or copy the project into a short ASCII-only path before building.
Avoid Chinese characters or long paths when using ESP-IDF on Windows.

## Build And Flash

macOS / Linux:

```bash
cd /path/to/DNESP32S3-4.3-RGBLCD-2048
. ~/esp/esp-idf-v5.5/export.sh
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/cu.usbserial-21230 -b 460800 flash
```

If the serial port differs, list it first:

```bash
ls /dev/cu.* | grep -E 'usb|wch|serial|SLAB|ACM|modem'
```

Windows PowerShell:

```powershell
cd C:\espbuild\dnesp32s3-2048-radio
& "$HOME\esp\esp-idf-v5.5\export.ps1"
idf.py set-target esp32s3
idf.py build
idf.py -p COM12 -b 460800 flash
```

If flashing fails with "port busy", close `idf.py monitor`, serial terminals,
or any process holding the CH340 port, then retry only the flash command.

## Current App Set

The current firmware intentionally keeps these apps:

- Photo Viewer
- Flip Clock
- 2048
- Reaction
- Tomato
- Radio

The Bird Launcher and Racing modules were intentionally removed from the build
and menu. Do not re-add them unless the user explicitly asks for them.

## Latest Verification

- Date: 2026-05-25
- Build: passed with `idf.py build`
- Flash: passed with `idf.py -p /dev/cu.usbserial-21230 -b 460800 flash`
- Flash target: ESP32-S3, MAC `14:c1:9f:42:2f:94`
- Result: hashes verified and board hard-reset successfully

## Radio Is Headless By Design

The onboard ES8388 radio works only after releasing the RGB LCD because the
4.3-inch RGB LCD and ES8388 I2S share pins. Do not try to keep the LCD active
while radio audio is playing.

Verified ES8388 playback wiring in `main/APP/board_audio.c`:

| ES8388 / I2S signal | ESP32-S3 GPIO | Direction for playback |
| --- | --- | --- |
| MCLK | GPIO3 | ESP32 -> ES8388 |
| BCLK | GPIO46 | ESP32 -> ES8388 |
| LRCK / WS | GPIO9 | ESP32 -> ES8388 |
| ES8388 SDIN / DSDIN | GPIO10 | ESP32 I2S DOUT -> ES8388 |
| ES8388 SDOUT / ASDOUT | GPIO14 | ESP32 I2S DIN <- ES8388 |

Important: schematic names are from the ES8388 side. For speaker playback,
ESP32 transmits audio on GPIO10 into ES8388 SDIN. Do not swap GPIO10 and GPIO14.

## Known Good Radio Flow

1. Boot to WarmOS menu.
2. Connect WiFi in Tomato if needed.
3. Tap `Radio`.
4. Speaker self-test runs.
5. Visible stream diagnostics run before headless playback.
6. BOOT button:
   - Short press: start diagnostics / next station during playback.
   - Long press: exit headless playback and restore the menu.
7. During playback the screen is off by design.

## Do Not Change Casually

- Do not modify `main/APP/board_audio.c` I2S pins.
- Do not enable a full CJK LVGL font just for Radio UI; it can overflow the app
  partition and may cause LCD flicker.
- Do not commit local WiFi credentials or private API keys in `sdkconfig`.
- Keep generated build output out of Git.
