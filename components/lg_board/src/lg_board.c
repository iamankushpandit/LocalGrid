/*
 * Board profiles.
 *
 * Provenance: pins, colour order, inversion, backlight polarity, and touch wiring for
 * the E32R32P (Hosyond 3.2in) and Freenove FNK0104B were measured on these exact
 * boards by the owner for Braino (github.com/iamankushpandit/Gume, commit 1e2a11f:
 * include/boards/e32r32p.h, include/boards/fnk0104b.h, and the TFT_eSPI flags in
 * platformio.ini). They are restated here as facts, not copied code.
 *
 * The E32R28T-1, the dual-USB ESP32-2432S028 with the inverting panel, and the E32R40T come
 * from the same place (include/boards/e32r28t1.h, esp32-2432s028-inv.h, e32r40t.h and their
 * [board_*] sections of platformio.ini). Each says below which facts Braino measured on the
 * board and which it inherited from a sibling; an inherited fact is a guess until checked.
 *
 * Mapping a Braino board to a profile here: its TFT_eSPI driver macro names the panel kind and
 * init sequence; colour order is BGR unless TFT_RGB_ORDER says otherwise (TFT_eSPI's default
 * for the ILI9341 and ST7796); inversion is on only if the driver's init turns it on
 * (TFT_INVERSION_ON, or the ST7789 sequence) or the profile's invertColours asks for it at
 * run time; and Braino's portrait rotation 0 is MADCTL MX on the ILI9341 and ST7796, which is
 * mirror_x here, and nothing on the ST7789.
 *
 * The Waveshare 1.47in ESP32-C6 boards (C6L, C6T) come from Waveshare's own pin tables and
 * factory example; see the comment above their entries.
 */
#include "lg_board.h"

#include <string.h>

static const lg_board_t BOARDS[] = {
    {
        .code = "ELG",
        .name = "Elegoo ESP32 dev board",
        .panel = { .kind = LG_PANEL_NONE },
        .touch = { .kind = LG_TOUCH_NONE },
        .boot_button = 0,
        .supply_sense = LG_PIN_NONE,   /* no divider on this dev board; wire one to an ADC1 pin to measure */
        .gps_rx = LG_PIN_NONE,
        .gps_tx = LG_PIN_NONE,
        .lora_rx = LG_PIN_NONE,   /* D71: no LoRa module connector on this board */
        .lora_tx = LG_PIN_NONE,
        .lora_reset = LG_PIN_NONE,
    },
    {
        /* LCDWIKI E32R32P, sold as Hosyond 3.2in. ST7789P3, BGR (measured: red and blue
         * swap with RGB), backlight GPIO27 active high. TFT_eSPI's ST7789 init turns
         * inversion on, and Braino adds none on top, so the panel runs inverted. */
        .code = "HY3",
        .name = "Hosyond 3.2in (E32R32P)",
        /* GPIO26 is DAC channel 1 on a classic ESP32, and the amplifier behind it is enabled
         * by GPIO4 held LOW. Both come from the vendor's own E32R32P pin table rather than
         * from a sibling board, and nothing else in this profile claims either pin. The 75
         * volume ceiling is inherited from the rest of that board family and has not been set
         * by listening on this one, so treat it as a starting point. */
        .audio = { .kind = LG_AUDIO_DAC, .speaker = 26, .codec_addr = 0,
                   .i2s_mclk = LG_PIN_NONE, .i2s_bclk = LG_PIN_NONE, .i2s_ws = LG_PIN_NONE,
                   .i2s_dout = LG_PIN_NONE, .i2s_din = LG_PIN_NONE,
                   .amp_enable = 4, .amp_active_low = true, .max_volume = 75 },
        .panel = {
            .kind = LG_PANEL_ST7789,
            .native_width = 240, .native_height = 320,
            .mosi = 13, .miso = 12, .sclk = 14, .cs = 15, .dc = 2, .rst = LG_PIN_NONE,
            .spi_hz = 40000000,
            .bgr = true, .invert = true, .mirror_x = false, .mirror_y = false,
            .backlight = 27, .backlight_active_high = true,
            .px_per_10mm = 49,   /* 3.2in diagonal, 240x320: about 125 px per inch */
        },
        .touch = {
            .kind = LG_TOUCH_XPT2046_SPI,
            .cs = 33, .irq = 36, .irq_usable = false,   /* GPIO36 has no pull-up on this board */
            .sda = LG_PIN_NONE, .scl = LG_PIN_NONE, .rst = LG_PIN_NONE,
            .pressure_threshold = 350,
        },
        .boot_button = 0,
        /* Braino (E32R32P): IO34, ADC1_CH6, behind the 2:1 divider the vendor manual states. With
         * no battery fitted it reads the charger output, which follows USB. */
        .supply_sense = 34,
        .supply_divider_milli = 2000,
        .gps_rx = LG_PIN_NONE,   /* UART0 is this board's console, and no free connector carries a UART */
        .gps_tx = LG_PIN_NONE,
        .lora_rx = LG_PIN_NONE,   /* D71: no LoRa module connector on this board */
        .lora_tx = LG_PIN_NONE,
        .lora_reset = LG_PIN_NONE,
    },
    {
        /* Freenove FNK0104B. ILI9341 with TFT_eSPI's ILI9341_2 sequence, BGR, inversion
         * on (TFT_INVERSION_ON), backlight GPIO45 active high. TFT_eSPI's portrait
         * rotation for ILI9341 sets the horizontal mirror. */
        .code = "F4B",
        .name = "Freenove FNK0104B 2.8in",
        /* This board has an ES8311 codec, a microphone and a speaker, and the ESP32-S3 has no
         * DAC, so every sound goes through the codec over I2S. There is no bare speaker pin
         * and there must not be one: the transducer hangs off the codec's amplifier.
         *
         * The wiring below was established on this board rather than inferred, and three
         * parts of it are easy to get wrong: the amplifier enable is active LOW, MCLK is 384
         * times the sample rate (6.144 MHz at 16 kHz), and the codec's DAC volume register
         * resets to silence. The S3 has no audio PLL, so MCLK comes from the PLL through the
         * I2S fractional divider. None of these pins touch GPIO33 to 37,
         * which are the octal PSRAM lines here and must never be assigned.
         * lg_bsp_audio.c drives it. */
        .audio = { .kind = LG_AUDIO_ES8311_I2S, .speaker = LG_PIN_NONE, .codec_addr = 0x18,
                   .i2s_mclk = 4, .i2s_bclk = 5, .i2s_ws = 7, .i2s_dout = 8, .i2s_din = 6,
                   .amp_enable = 1, .amp_active_low = true, .max_volume = 85 },
        .panel = {
            .kind = LG_PANEL_ILI9341,
            .native_width = 240, .native_height = 320,
            .mosi = 11, .miso = 13, .sclk = 12, .cs = 10, .dc = 46, .rst = LG_PIN_NONE,
            .spi_hz = 40000000,
            .bgr = true, .invert = true, .mirror_x = true, .mirror_y = false,
            .backlight = 45, .backlight_active_high = true,
            .px_per_10mm = 56,   /* 2.8in diagonal, 240x320: about 143 px per inch */
        },
        .touch = {
            .kind = LG_TOUCH_FT6336_I2C,
            .cs = LG_PIN_NONE, .irq = 17, .irq_usable = true,
            .sda = 16, .scl = 15, .rst = 18, .i2c_addr = 0x38,
            /* Braino's hardware-verified FT6336 transform (native to landscape, then
             * TFT_eSPI rotation 0) reduces to no swap and no mirror in portrait. */
            .swap_xy = false, .mirror_x = false, .mirror_y = false,
        },
        .boot_button = 0,
        /* Braino (FNK0104B): GPIO9, ADC1_CH8, x2.0 as Freenove's sketch states. Braino read
         * 4.09-4.16 V here on USB with no pack fitted. */
        .supply_sense = 9,
        .supply_divider_milli = 2000,
        /* D65: the 4-pin "UART" connector (RXD, TXD, GND, 5V). The console is USB-Serial-JTAG, so
         * these are free: board RXD is GPIO44 (the GPS module's TXD goes here), board TXD GPIO43.
         * The owner wired a GT-U7 here on device 1. */
        .gps_rx = 44,
        .gps_tx = 43,
        /* D71/D76: the owner wired an RYLR998 here on the I2C header's 3.3 V: module TXD to
         * GPIO21, module RXD to GPIO14, RST to GPIO2. The GPS keeps UART0 on GPIO44/43, so
         * the two modules share only power and ground. */
        .lora_rx = 21,
        .lora_tx = 14,
        .lora_reset = 2,
    },
    {
        /* LCDWIKI / Hosyond E32R28T-1 (ESP32-32E, classic ESP32), 2.8in, one USB-C. ILI9341 on
         * TFT_eSPI's ILI9341_2 sequence, BGR by that driver's default, no inversion; all
         * measured by Braino, which ships on this board. Backlight GPIO21 active high. */
        .code = "E28",
        .name = "LCDWIKI E32R28T-1 2.8in",
        /* GPIO26 (DAC channel 2) into an FM8002E amplifier whose enable is GPIO4, active LOW:
         * from the vendor's pin table, confirmed by the owner. GPIO4 is not the RGB LED's green
         * (that is GPIO16; red GPIO22, blue GPIO17, common anode, none driven here). The 75
         * ceiling is the CYD family's by ear, not yet set by listening on this board.
         * The touch clock is GPIO25, the other DAC pad: lg_bsp_audio.c must only ever claim
         * the speaker's channel, or touch dies the first time a sound plays (Braino 5.5.0). */
        .audio = { .kind = LG_AUDIO_DAC, .speaker = 26, .codec_addr = 0,
                   .i2s_mclk = LG_PIN_NONE, .i2s_bclk = LG_PIN_NONE, .i2s_ws = LG_PIN_NONE,
                   .i2s_dout = LG_PIN_NONE, .i2s_din = LG_PIN_NONE,
                   .amp_enable = 4, .amp_active_low = true, .max_volume = 75 },
        .panel = {
            .kind = LG_PANEL_ILI9341,
            .native_width = 240, .native_height = 320,
            .mosi = 13, .miso = 12, .sclk = 14, .cs = 15, .dc = 2, .rst = LG_PIN_NONE,
            .spi_hz = 40000000,
            .bgr = true, .invert = false, .mirror_x = true, .mirror_y = false,
            .backlight = 21, .backlight_active_high = true,
            .px_per_10mm = 56,   /* 2.8in diagonal, 240x320: about 143 px per inch */
        },
        .touch = {
            /* XPT2046 on pins of its own, not the display's HSPI bus (Braino bit-bangs it; here
             * it gets SPI3). Measured working in Braino. */
            .kind = LG_TOUCH_XPT2046_SPI,
            .cs = 33, .own_bus = true, .mosi = 32, .miso = 39, .sclk = 25,
            .irq = 36, .irq_usable = true,
            .sda = LG_PIN_NONE, .scl = LG_PIN_NONE, .rst = LG_PIN_NONE,
            .pressure_threshold = 350,
        },
        .boot_button = 0,
        /* GPIO34 (ADC1_CH6) behind the 2:1 divider the vendor manual states. TP4054 charger;
         * the board cannot tell a missing pack from a present one. */
        .supply_sense = 34,
        .supply_divider_milli = 2000,
        .gps_rx = LG_PIN_NONE,   /* UART0 is the console; Braino documents no free UART connector */
        .gps_tx = LG_PIN_NONE,
        .lora_rx = LG_PIN_NONE,   /* D71: no LoRa module connector on this board */
        .lora_tx = LG_PIN_NONE,
        .lora_reset = LG_PIN_NONE,
    },
    {
        /* ESP32-2432S028 "cheap yellow display", the dual-USB (USB-C and micro-USB) variant with
         * the inverting panel. Classic ESP32, 2.8in, no battery. Braino measured it one
         * combination at a time: the ILI9341 sequence with inversion switched on is the only
         * correct one (no inversion: every colour flipped; the ST7789 sequence: wrong colours;
         * the Rv3's backlight GPIO27: dark). Backlight GPIO21 active high, measured. Other CYDs
         * under the same silkscreen (micro-USB ILI9341 without inversion, the ST7789 Rv3) are
         * different boards and must not borrow this profile. */
        .code = "CYD",
        .name = "ESP32-2432S028 2.8in (dual USB)",
        /* GPIO26 (DAC channel 2) into an SC8002B amplifier and the 1.25 mm speaker connector.
         * The amplifier has no enable line (owner). GPIO4 is the RGB LED's red here, not an
         * amplifier enable. 75 is the CYD ceiling set by ear in Braino. */
        .audio = { .kind = LG_AUDIO_DAC, .speaker = 26, .codec_addr = 0,
                   .i2s_mclk = LG_PIN_NONE, .i2s_bclk = LG_PIN_NONE, .i2s_ws = LG_PIN_NONE,
                   .i2s_dout = LG_PIN_NONE, .i2s_din = LG_PIN_NONE,
                   .amp_enable = LG_PIN_NONE, .amp_active_low = false, .max_volume = 75 },
        .panel = {
            .kind = LG_PANEL_ILI9341,
            .native_width = 240, .native_height = 320,
            .mosi = 13, .miso = 12, .sclk = 14, .cs = 15, .dc = 2, .rst = LG_PIN_NONE,
            .spi_hz = 40000000,
            .bgr = true, .invert = true, .mirror_x = true, .mirror_y = false,
            .backlight = 21, .backlight_active_high = true,
            .px_per_10mm = 56,   /* 2.8in diagonal, 240x320: about 143 px per inch */
        },
        .touch = {
            /* Same wiring as the E32R28T-1: its own pins, measured working in Braino. */
            .kind = LG_TOUCH_XPT2046_SPI,
            .cs = 33, .own_bus = true, .mosi = 32, .miso = 39, .sclk = 25,
            .irq = 36, .irq_usable = true,
            .sda = LG_PIN_NONE, .scl = LG_PIN_NONE, .rst = LG_PIN_NONE,
            .pressure_threshold = 350,
        },
        .boot_button = 0,
        /* No supply sense. GPIO34 on a CYD is the light sensor (LDR), not a battery divider;
         * reading it as one shows a light level as a charge. Published pin map, not yet
         * checked on this board. */
        .supply_sense = LG_PIN_NONE,
        .gps_rx = LG_PIN_NONE,   /* UART0 is the console; Braino documents no free UART connector */
        .gps_tx = LG_PIN_NONE,
        .lora_rx = LG_PIN_NONE,   /* D71: no LoRa module connector on this board */
        .lora_tx = LG_PIN_NONE,
        .lora_reset = LG_PIN_NONE,
    },
    {
        /* LCDWIKI E32R40T (ESP32-32E, classic ESP32), 4.0in, one USB-C. ST7796 at 320x480
         * native, BGR by TFT_eSPI's default, no inversion, measured by Braino (2026-09-03):
         * colours and all four edges correct. The display bus is pin for pin the E32R28T-1's;
         * the backlight is NOT: GPIO27 active high, measured (GPIO21 was dark twice).
         * The ST7796 wakes only with Sleep Out AND Display On; Sleep Out alone leaves the glass
         * black (Braino, measured). Nothing here sleeps the panel today; anything that starts
         * to must send both. */
        .code = "E40",
        .name = "LCDWIKI E32R40T 4.0in",
        /* Vendor schematic: GPIO26 (DAC channel 2) through an RC filter into an 8002-series
         * amplifier (SC8002B or FM8002E by revision) whose shutdown input, AUDIO_EN, is GPIO4,
         * active LOW. The 75 ceiling is inherited from the 2.8in board, not set by ear here. */
        .audio = { .kind = LG_AUDIO_DAC, .speaker = 26, .codec_addr = 0,
                   .i2s_mclk = LG_PIN_NONE, .i2s_bclk = LG_PIN_NONE, .i2s_ws = LG_PIN_NONE,
                   .i2s_dout = LG_PIN_NONE, .i2s_din = LG_PIN_NONE,
                   .amp_enable = 4, .amp_active_low = true, .max_volume = 75 },
        .panel = {
            .kind = LG_PANEL_ST7796,
            .native_width = 320, .native_height = 480,
            .mosi = 13, .miso = 12, .sclk = 14, .cs = 15, .dc = 2, .rst = LG_PIN_NONE,
            .spi_hz = 40000000,
            .bgr = true, .invert = false, .mirror_x = true, .mirror_y = false,
            .backlight = 27, .backlight_active_high = true,
            .px_per_10mm = 57,   /* 4.0in diagonal, 320x480: about 144 px per inch */
        },
        .touch = {
            /* XPT2046 sharing the display bus with its own CS, as on the Hosyond. GPIO36 has no
             * pull-up on this board and floats low, so pressure alone decides a press: idle
             * reads 10-20, a press 2000 and up (Braino, measured). */
            .kind = LG_TOUCH_XPT2046_SPI,
            .cs = 33, .own_bus = false, .irq = 36, .irq_usable = false,
            .sda = LG_PIN_NONE, .scl = LG_PIN_NONE, .rst = LG_PIN_NONE,
            .pressure_threshold = 350,
        },
        .boot_button = 0,
        /* INHERITED from the E32R28T-1, not measured: GPIO34 behind a 2:1 divider. A wrong pin
         * here does not fail loudly, it reads a plausible voltage that is fiction. Check one
         * reading against a meter before trusting it. */
        .supply_sense = 34,
        .supply_divider_milli = 2000,
        .gps_rx = LG_PIN_NONE,   /* UART0 is the console; Braino documents no free UART connector */
        .gps_tx = LG_PIN_NONE,
        .lora_rx = LG_PIN_NONE,   /* D71: no LoRa module connector on this board */
        .lora_tx = LG_PIN_NONE,
        .lora_reset = LG_PIN_NONE,
    },
    /*
     * The alert and distress unit (D66): Waveshare's two 1.47in ESP32-C6 boards. They share a name
     * and a case but not their wiring, so each has its own profile, and each names the other as its
     * sibling: at boot lg_bsp_board_resolve() looks for the Touch board's I2C parts (the QMI8658 IMU
     * at 0x6B, the AXS5106L at 0x63, on GPIO18/19) and uses whichever profile the board really is.
     *
     * Sources, restated as facts:
     *   - Waveshare's pin tables: docs.waveshare.com/ESP32-C6-LCD-1.47 and
     *     docs.waveshare.com/ESP32-C6-Touch-LCD-1.47 (the www.waveshare.com/wiki pages of the same
     *     names carry the same tables and schematics).
     *   - Waveshare's factory example for the Touch board (01_factory; published at
     *     github.com/cpg/ESP32-C6-Touch-LCD-1.47-factory-firmware-example): bsp_display.h,
     *     bsp_spi.h, bsp_i2c.h, bsp_touch.h/.c, bsp_battery.h/.c, main.c.
     *   - For the plain board's panel setup, a community sketch that runs on it
     *     (github.com/druzvv-hash/esp32c6-lcd-1.47, NOTES.txt and the Arduino_GFX constructor):
     *     ST7789, IPS (inversion on), x offset 34, BOOT GPIO9.
     * Nothing here has been checked on the bench's board yet: colours, mirroring, and the touch
     * mapping are the vendor's, to be confirmed by eye at bring-up.
     *
     * Both: the console is the chip's USB-Serial/JTAG (GPIO12/13), the BOOT button is GPIO9 (a
     * strapping pin, pulled up on the board, pressed = LOW; Waveshare's factory example reads it
     * as active-low GPIO9), and there is no speaker, microphone, or GPS connector.
     *
     * Buttons: BOOT is the only button either board has besides RESET. A short press reads an
     * alert or cancels the SOS countdown; holding it raises the SOS or, from the SOS screen, says
     * "I'm safe". A builder can wire more buttons (to GND, pull-up on) and give each a row here:
     * a dedicated SOS button would be { .on_hold = LG_ACT_SOS }, a Read button
     * { .on_press = LG_ACT_READ | LG_ACT_CANCEL }. Free pins by Waveshare's tables: on the plain
     * board GPIO0 to 3, 18, 19, 20 and 23; on the Touch board GPIO7 (and GPIO16/17, UART0 on the
     * header, since the console is on USB). Avoid GPIO8, 9 and 15 (strapping) for a new button.
     */
    {
        /* ESP32-C6-LCD-1.47, no touch. ST7789 on SPI2: MOSI 6, SCLK 7, CS 14, DC 15, RST 21,
         * backlight 22 (Waveshare's table). The TF slot shares the bus (MISO 5, CS 4) and is not
         * used. The RGB LED is a WS2812 on GPIO8, a strapping pin, and is not driven. No battery
         * sense is documented. The glass is 172 columns inside the ST7789's 240, 34 in. */
        .code = "C6L",
        .name = "Waveshare ESP32-C6-LCD-1.47",
        .role = LG_ROLE_ALERT_UNIT,
        .audio = { .kind = LG_AUDIO_NONE, .speaker = LG_PIN_NONE, .codec_addr = 0,
                   .i2s_mclk = LG_PIN_NONE, .i2s_bclk = LG_PIN_NONE, .i2s_ws = LG_PIN_NONE,
                   .i2s_dout = LG_PIN_NONE, .i2s_din = LG_PIN_NONE,
                   .amp_enable = LG_PIN_NONE, .amp_active_low = false, .max_volume = 0 },
        .panel = {
            .kind = LG_PANEL_ST7789,
            .native_width = 172, .native_height = 320,
            .mosi = 6, .miso = LG_PIN_NONE, .sclk = 7, .cs = 14, .dc = 15, .rst = 21,
            .spi_hz = 40000000,
            /* Arduino_GFX's ST7789 with ips = true: RGB order, inversion on, no mirror. */
            .bgr = false, .invert = true, .mirror_x = false, .mirror_y = false,
            .backlight = 22, .backlight_active_high = true,
            .px_per_10mm = 97,   /* 1.47in diagonal, 172x320: about 247 px per inch */
            .x_gap = 34, .y_gap = 0,
        },
        .touch = { .kind = LG_TOUCH_NONE, .cs = LG_PIN_NONE, .irq = LG_PIN_NONE,
                   .sda = LG_PIN_NONE, .scl = LG_PIN_NONE, .rst = LG_PIN_NONE },
        .boot_button = 9,
        .supply_sense = LG_PIN_NONE,
        .gps_rx = LG_PIN_NONE,
        .gps_tx = LG_PIN_NONE,
        .lora_rx = LG_PIN_NONE,   /* D71: no LoRa module connector on this board */
        .lora_tx = LG_PIN_NONE,
        .lora_reset = LG_PIN_NONE,
        .n_buttons = 1,
        .buttons = {
            { .gpio = 9, .active_low = true, .pull_up = true,
              .on_press = LG_ACT_READ | LG_ACT_CANCEL, .on_hold = LG_ACT_SOS | LG_ACT_SAFE },
        },
        .sibling = "C6T",
        .probe = { .sda = LG_PIN_NONE, .scl = LG_PIN_NONE, .addr = { 0, 0 } },   /* nothing only it has */
    },
    {
        /* ESP32-C6-Touch-LCD-1.47 (ESP32-C6FH8, 8 MB flash). JD9853 on SPI2: SCLK 1, MOSI 2, CS 14,
         * DC 15, RST 22, backlight 23 (Waveshare's table and factory example; the example drives the
         * backlight by PWM at up to 100 %, so high is on). The TF slot shares SCLK and MOSI (MISO 3,
         * CS 4) and is not used. The factory example sets RGB order, inversion on, no mirror, and a
         * 34-column gap; an ESPHome configuration for the same board uses BGR and no inversion with
         * a different driver, so colours are to be checked by eye. */
        .code = "C6T",
        .name = "Waveshare ESP32-C6-Touch-LCD-1.47",
        .role = LG_ROLE_ALERT_UNIT,
        .audio = { .kind = LG_AUDIO_NONE, .speaker = LG_PIN_NONE, .codec_addr = 0,
                   .i2s_mclk = LG_PIN_NONE, .i2s_bclk = LG_PIN_NONE, .i2s_ws = LG_PIN_NONE,
                   .i2s_dout = LG_PIN_NONE, .i2s_din = LG_PIN_NONE,
                   .amp_enable = LG_PIN_NONE, .amp_active_low = false, .max_volume = 0 },
        .panel = {
            .kind = LG_PANEL_JD9853,
            .native_width = 172, .native_height = 320,
            .mosi = 2, .miso = LG_PIN_NONE, .sclk = 1, .cs = 14, .dc = 15, .rst = 22,
            .spi_hz = 40000000,   /* the example runs 80 MHz; 40 is the bench's value on every board */
            .bgr = false, .invert = true, .mirror_x = false, .mirror_y = false,
            .backlight = 23, .backlight_active_high = true,
            .px_per_10mm = 97,
            .x_gap = 34, .y_gap = 0,
        },
        .touch = {
            /* AXS5106L at 0x63 on I2C SDA 18, SCL 19, reset GPIO20, interrupt GPIO21. The example
             * maps portrait with a horizontal mirror and no swap. The QMI8658 IMU (0x6B) shares the
             * bus and is not used. If the controller does not answer, the unit runs on its button. */
            .kind = LG_TOUCH_AXS5106_I2C,
            .cs = LG_PIN_NONE, .irq = 21, .irq_usable = false,   /* polled, as every touch here is */
            .sda = 18, .scl = 19, .rst = 20, .i2c_addr = 0x63,
            .swap_xy = false, .mirror_x = true, .mirror_y = false,
        },
        .boot_button = 9,
        /* GPIO0 (ADC1 channel 0) behind 200k over 100k: VBAT = 3 x the pin (Waveshare's table and
         * bsp_battery.c). Not yet checked against a meter. */
        .supply_sense = 0,
        .supply_divider_milli = 3000,
        .gps_rx = LG_PIN_NONE,
        .gps_tx = LG_PIN_NONE,
        .lora_rx = LG_PIN_NONE,   /* D71: no LoRa module connector on this board */
        .lora_tx = LG_PIN_NONE,
        .lora_reset = LG_PIN_NONE,
        .n_buttons = 1,
        .buttons = {
            { .gpio = 9, .active_low = true, .pull_up = true,
              .on_press = LG_ACT_READ | LG_ACT_CANCEL, .on_hold = LG_ACT_SOS | LG_ACT_SAFE },
        },
        .sibling = "C6L",
        .probe = { .sda = 18, .scl = 19, .addr = { 0x6B, 0x63 } },   /* the IMU, then the touch controller */
    },
};

const lg_board_t *lg_board_find(const char *code)
{
    if (code == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < sizeof(BOARDS) / sizeof(BOARDS[0]); i++) {
        if (strcmp(BOARDS[i].code, code) == 0) {
            return &BOARDS[i];
        }
    }
    return NULL;
}
