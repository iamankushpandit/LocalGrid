/*
 * lg_font.h - bitmap fonts for lg_draw (D55).
 *
 * The tables are the ones lv_font_conv generates (4 bits per pixel, uncompressed), rewritten by
 * tools/convert_font.py into these types so no UI library is needed to read them. Bitmaps and
 * glyph tables are const and stay in flash.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t bitmap_index : 20;   /* start of the glyph in the font's bitmap */
    uint32_t adv_w : 12;          /* advance, in 1/16 px */
    uint8_t  box_w;
    uint8_t  box_h;
    int8_t   ofs_x;
    int8_t   ofs_y;               /* bottom of the box above the base line */
} lg_glyph_t;

typedef enum {
    LG_FONT_CMAP_FORMAT0_FULL,
    LG_FONT_CMAP_SPARSE_FULL,
    LG_FONT_CMAP_FORMAT0_TINY,
    LG_FONT_CMAP_SPARSE_TINY,
} lg_font_cmap_type_t;

/* Code points range_start.. mapped to glyph ids (the lv_font_conv formats, by type). */
typedef struct {
    uint32_t        range_start;
    uint16_t        range_length;
    uint16_t        glyph_id_start;
    const uint16_t *unicode_list;
    const void     *glyph_id_ofs_list;   /* uint8_t for FORMAT0_FULL, uint16_t for SPARSE_FULL */
    uint16_t        list_length;
    uint8_t         type;                /* lg_font_cmap_type_t */
} lg_font_cmap_t;

typedef struct {
    const uint8_t        *bitmap;
    const lg_glyph_t     *glyphs;
    const lg_font_cmap_t *cmaps;
    uint16_t              cmap_num;
    uint8_t               bpp;           /* always 4 */
    int16_t               line_height;
    int16_t               base_line;     /* from the bottom of the line */
} lg_font_t;

extern const lg_font_t lg_font_montserrat_10;
extern const lg_font_t lg_font_montserrat_12;
extern const lg_font_t lg_font_montserrat_14;
extern const lg_font_t lg_font_montserrat_16;
extern const lg_font_t lg_font_montserrat_20;
extern const lg_font_t lg_font_montserrat_28;

/* Font Awesome icons carried in the Montserrat fonts, as UTF-8 (the code points LVGL used). */
#define LG_SYMBOL_BACKSPACE "\xEF\x95\x9A"
#define LG_SYMBOL_CLOSE     "\xEF\x80\x8D"
#define LG_SYMBOL_DOWN      "\xEF\x81\xB8"
#define LG_SYMBOL_DOWNLOAD  "\xEF\x80\x99"
#define LG_SYMBOL_ENVELOPE  "\xEF\x83\xA0"
#define LG_SYMBOL_EYE_OPEN  "\xEF\x81\xAE"
#define LG_SYMBOL_HOME      "\xEF\x80\x95"
#define LG_SYMBOL_LEFT      "\xEF\x81\x93"
#define LG_SYMBOL_LIST      "\xEF\x80\x8B"
#define LG_SYMBOL_OK        "\xEF\x80\x8C"
#define LG_SYMBOL_PLUS      "\xEF\x81\xA7"
#define LG_SYMBOL_RIGHT     "\xEF\x81\x94"
#define LG_SYMBOL_SETTINGS  "\xEF\x80\x93"
#define LG_SYMBOL_UP        "\xEF\x81\xB7"
#define LG_SYMBOL_UPLOAD    "\xEF\x82\x93"
#define LG_SYMBOL_WARNING   "\xEF\x81\xB1"
#define LG_SYMBOL_WIFI      "\xEF\x87\xAB"

#ifdef __cplusplus
}
#endif
