# WarmOS external Chinese fonts

WarmOS uses external LVGL binary fonts for Chinese text. The recommended source font is
LXGW WenKai Lite (霞鹜文楷 Lite).

Put the generated files on the SD card:

```text
/sdcard/fonts/lxgw_wenkai_16.bin
/sdcard/fonts/lxgw_wenkai_20.bin
/sdcard/fonts/lxgw_wenkai_24.bin
/sdcard/fonts/lxgw_wenkai_32.bin
```

Generate LVGL v8 binary fonts with `lv_font_conv`, for example:

```bash
lv_font_conv \
  --font LXGWWenKaiLite-Regular.ttf \
  --format bin \
  --bpp 4 \
  --size 16 \
  --range 0x20-0x7E,0x3000-0x303F,0x3400-0x9FFF,0xFF00-0xFFEF \
  -o lxgw_wenkai_16.bin
```

Repeat with `--size 20`, `--size 24`, and `--size 32`. These four sizes are enough for
the current desktop UI and keep memory use predictable.

Do not convert the full TTF into a huge C array and compile it into firmware. Keeping
fonts on the SD card avoids excessive Flash use, RAM pressure, long link times, and boot
instability.

If the SD card is absent, a font file is missing, or a font fails to load, WarmOS logs a
warning and falls back to built-in Montserrat fonts. The system continues to boot.
