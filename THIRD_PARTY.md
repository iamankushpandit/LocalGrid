# Provenance and third-party material

LocalGrid carries no licence of its own yet (owner, 2026-09-17: decide before anything is
published). This file records where material in the tree came from, so that decision can be made
with the facts in hand.

## Braino (github.com/iamankushpandit/Gume), GPLv3

Braino is the owner's own project, so its copyright is the owner's to license here as well; GPLv3
binds people the owner licenses to, not the owner. What LocalGrid takes from it is hardware fact
and technique rather than code, restated against a different API (ESP-IDF `esp_lcd`, `i2c_master`
and `spi_master`, where Braino is Arduino and TFT_eSPI):

| Here | What |
|---|---|
| `components/lg_board/src/lg_board.c` | Pins, colour order, inversion, backlight polarity and touch wiring for the E32R32P (Hosyond 3.2in) and Freenove FNK0104B, measured on these exact boards; the supply sense pins and divider ratios. |
| `components/lg_bsp/src/lg_bsp_touch.c` | Which FT6336U registers to read and what the bits mean, the XPT2046 pressure formula and its threshold (both also in the controllers' datasheets), and the three-point affine calibration. |
| `components/lg_bsp/src/lg_bsp_display.c` | That these panels want the TFT_eSPI power and gamma sequences (see below). |
| `components/lg_bsp/src/lg_bsp_audio.c` | The approach only, stated in the header as shared reasoning rather than code. For the microphone, two facts from Braino's FNK0104B bring-up (`src/s3_diag.cpp`): the ES8311 ADC volume register 0x17 resets to minimum and records silence until set, and 0xC8 with register 0x14 at 0x1A (analogue mic, PGA +30 dB) recorded speech on this board; and that the codec's mono ADC arrives in the left I2S slot. |
| `firmware/handheld/main/service/hh_battery.c` | The LiPo discharge curve (4.20 V = 100 % to 3.20 V = 0 %), the ~40 s low-pass at 2 s sampling primed with the first reading, the 2-point display deadband, and the 3.0 to 4.5 V plausibility window, from Braino's `src/hal/BoardPower.cpp`. |
| `firmware/handheld/main/ui/ui_lock.c` | The battery badge's form (digits in the shell, a two-pixel gauge, a terminal nub, the error colour at 15 % and below, no badge for no reading), the padlock's parts, and the lock's behaviour and timings (0.9 s hold, 150 ms contact grace, 12 s back to the saver, the progress bar as the only repaint, the header, hint, button and footer layout), from Braino's `src/ui/Ui.cpp` and `src/engine/AppRuntimeLock.cpp`. |

If any of it is ever found to be copied rather than restated, the GPLv3 terms apply to that part
and the owner, as its author, can also license it here directly.

## TFT_eSPI (github.com/Bodmer/TFT_eSPI), FreeBSD licence

`components/lg_bsp/src/lg_bsp_display.c` sends the controller power and gamma sequences TFT_eSPI
uses for these panels (`TFT_Drivers/ST7789_Init.h`, and the `ILI9341_2` branch of
`ILI9341_Init.h`). The FreeBSD licence asks that the copyright notice and conditions travel with
redistributions:

> Copyright (c) 2023 Bodmer (https://github.com/Bodmer). Redistribution and use in source and
> binary forms, with or without modification, are permitted provided that the conditions of the
> FreeBSD licence are met.

## Fonts

See `components/lg_draw/fonts/README.md`. Montserrat and Font Awesome Free (as converted by
LVGL 9.5) and Noto Emoji are all under the SIL Open Font License 1.1; the licence text is in
`components/lg_draw/fonts/NotoEmoji-OFL.txt`.

## ESP-IDF

Espressif's SDK and its components are used as a dependency under the Apache License 2.0; nothing
from it is copied into this tree.
