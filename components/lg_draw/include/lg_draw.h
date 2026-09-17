/*
 * lg_draw.h - a retained-box renderer with no UI library (spike, owner 2026-09-17).
 *
 * The screen is a set of boxes the caller owns. A box is drawn only when the caller asks,
 * and only its own rectangle is sent to the panel, in bands of the driver's draw buffer, so
 * a clock that ticks repaints a clock and nothing else. Nothing is allocated per draw: the
 * only buffer is lg_bsp's DMA band buffer.
 *
 * Fonts are LVGL's generated C font tables, read directly: bitmaps and glyph tables stay in
 * flash, and LVGL itself is never started. Only the uncompressed text format is supported.
 *
 * Threading: one task draws. Call lg_draw_* from that task only.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "lg_board.h"
#include "lvgl.h"   /* lv_font_t and the fmt_txt tables only; no LVGL function is called */

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t lg_color_t;   /* RGB565 */

static inline lg_color_t lg_rgb(uint32_t hex)
{
    return (lg_color_t)((((hex >> 16) & 0xF8u) << 8) | (((hex >> 8) & 0xFCu) << 3) | ((hex & 0xFFu) >> 3));
}

typedef struct {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
} lg_rect_t;

typedef enum { LG_ALIGN_LEFT, LG_ALIGN_CENTER, LG_ALIGN_RIGHT } lg_align_t;

#define LG_BOX_TEXT_MAX 64u

/* One rectangle on screen: a filled, optionally bordered and rounded panel with one line of
 * text. `outside` is the colour behind the rounded corners. */
typedef struct {
    lg_rect_t         rect;
    lg_color_t        bg;
    lg_color_t        outside;
    lg_color_t        border;
    uint8_t           border_w;
    uint8_t           radius;
    const lv_font_t  *font;
    const lv_font_t  *fallback;   /* looked up when font has no glyph, e.g. emoji; may be NULL */
    lg_color_t        fg;
    uint8_t           align;      /* lg_align_t */
    int16_t           pad;
    int16_t           text_dy;    /* text baseline shift from vertical centre */
    char              text[LG_BOX_TEXT_MAX];
} lg_box_t;

typedef struct {
    uint32_t boxes;        /* boxes drawn */
    uint64_t pixels;       /* pixels sent to the panel */
    uint64_t draw_us;      /* time spent rendering and sending */
} lg_draw_stats_t;

/* Starts the panel and touch through lg_bsp and clears the screen to `bg`. */
esp_err_t lg_draw_start(const lg_board_t *board, lg_color_t bg, uint16_t *width, uint16_t *height);

/* Draws a box now. */
void lg_draw_box(const lg_box_t *box);

/* Sets a box's text and draws it only if the text changed. Returns true if it drew. */
bool lg_draw_set_text(lg_box_t *box, const char *text);

/* Fills a rectangle with one colour. */
void lg_draw_fill(const lg_rect_t *r, lg_color_t color);

/* Width in pixels of UTF-8 text in a font (with fallback). */
int16_t lg_draw_text_width(const lv_font_t *font, const lv_font_t *fallback, const char *text);

const lg_draw_stats_t *lg_draw_stats(void);

/* A touch that has been seen twice down, as the LVGL input path does. */
bool lg_draw_touch(int16_t *x, int16_t *y);

static inline bool lg_rect_hit(const lg_rect_t *r, int16_t x, int16_t y)
{
    return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

#ifdef __cplusplus
}
#endif
