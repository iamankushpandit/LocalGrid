#include "lg_draw.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lg_bsp_display.h"
#include "lg_bsp_touch.h"

static struct {
    uint8_t          *buf;
    size_t            buf_bytes;
    uint16_t          width;
    uint16_t          height;
    uint16_t          band_lines;
    SemaphoreHandle_t done;
    lg_draw_stats_t   stats;
    uint8_t           down_run;
    uint8_t           up_run;
    bool              down;
    int16_t           last_x;
    int16_t           last_y;
    /* Hardware scroll: screen rows [scroll_top, scroll_top + scroll_h) show memory rows shifted
     * by scroll_vs, wrapping inside the area. scroll_h 0: no scroll area. */
    int16_t           scroll_top;
    int16_t           scroll_h;
    int16_t           scroll_vs;
} s;

static void flush_done(void *ctx)
{
    (void)ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s.done, &woken);   /* the panel IO calls this from its interrupt */
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

esp_err_t lg_draw_start(const lg_board_t *board, lg_color_t bg, uint16_t *width, uint16_t *height)
{
    s.done = xSemaphoreCreateBinary();
    if (s.done == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = lg_bsp_display_start(board, flush_done, NULL);
    if (err != ESP_OK) {
        return err;
    }
    s.buf = lg_bsp_display_buffer(&s.buf_bytes);
    s.width = board->panel.native_width;
    s.height = board->panel.native_height;
    s.band_lines = (uint16_t)(s.buf_bytes / (2u * s.width));
    *width = s.width;
    *height = s.height;
    lg_rect_t all = { 0, 0, (int16_t)s.width, (int16_t)s.height };
    lg_draw_fill(&all, bg);
    return ESP_OK;
}

uint16_t lg_draw_width(void)
{
    return s.width;
}

uint16_t lg_draw_height(void)
{
    return s.height;
}

const lg_draw_stats_t *lg_draw_stats(void)
{
    return &s.stats;
}

/* ---- fonts (lg_font.h) ---- */

static uint32_t utf8_next(const char **p)
{
    const uint8_t *u = (const uint8_t *)*p;
    uint32_t cp;
    size_t n;
    if (u[0] < 0x80) {
        cp = u[0];
        n = 1;
    } else if ((u[0] & 0xE0) == 0xC0 && u[1] != 0) {
        cp = ((uint32_t)(u[0] & 0x1F) << 6) | (u[1] & 0x3F);
        n = 2;
    } else if ((u[0] & 0xF0) == 0xE0 && u[1] != 0 && u[2] != 0) {
        cp = ((uint32_t)(u[0] & 0x0F) << 12) | ((uint32_t)(u[1] & 0x3F) << 6) | (u[2] & 0x3F);
        n = 3;
    } else if ((u[0] & 0xF8) == 0xF0 && u[1] != 0 && u[2] != 0 && u[3] != 0) {
        cp = ((uint32_t)(u[0] & 0x07) << 18) | ((uint32_t)(u[1] & 0x3F) << 12) | ((uint32_t)(u[2] & 0x3F) << 6) |
             (u[3] & 0x3F);
        n = 4;
    } else {
        cp = '?';
        n = 1;
    }
    *p += n;
    return cp;
}

static uint32_t glyph_id(const lg_font_t *d, uint32_t cp)
{
    for (uint16_t i = 0; i < d->cmap_num; i++) {
        const lg_font_cmap_t *c = &d->cmaps[i];
        if (cp < c->range_start) {
            continue;
        }
        uint32_t rcp = cp - c->range_start;
        if (rcp >= c->range_length) {
            continue;
        }
        switch (c->type) {
        case LG_FONT_CMAP_FORMAT0_TINY:
            return c->glyph_id_start + rcp;
        case LG_FONT_CMAP_FORMAT0_FULL:
            return c->glyph_id_start + ((const uint8_t *)c->glyph_id_ofs_list)[rcp];
        case LG_FONT_CMAP_SPARSE_TINY:
        case LG_FONT_CMAP_SPARSE_FULL:
            for (uint16_t k = 0; k < c->list_length; k++) {   /* short lists: a scan beats a search */
                if (c->unicode_list[k] == rcp) {
                    if (c->type == LG_FONT_CMAP_SPARSE_TINY) {
                        return c->glyph_id_start + k;
                    }
                    return c->glyph_id_start + ((const uint16_t *)c->glyph_id_ofs_list)[k];
                }
            }
            break;
        default:
            break;
        }
    }
    return 0;
}

/* The glyph for cp in font, then in fallback. */
static const lg_font_t *find_glyph(const lg_font_t *font, const lg_font_t *fallback, uint32_t cp,
                                   const lg_glyph_t **out)
{
    const lg_font_t *try[2] = { font, fallback };
    for (int i = 0; i < 2; i++) {
        if (try[i] == NULL) {
            continue;
        }
        const lg_font_t *d = try[i];
        uint32_t id = glyph_id(d, cp);
        if (id != 0) {
            *out = &d->glyphs[id];
            return try[i];
        }
    }
    return NULL;
}

static int16_t advance(const lg_glyph_t *g)
{
    return (int16_t)((g->adv_w + 8u) >> 4);
}

int16_t lg_draw_text_width(const lg_font_t *font, const lg_font_t *fallback, const char *text)
{
    int16_t w = 0;
    const char *p = text;
    while (*p != '\0') {
        const lg_glyph_t *g = NULL;
        if (find_glyph(font, fallback, utf8_next(&p), &g) != NULL) {
            w = (int16_t)(w + advance(g));
        }
    }
    return w;
}

/* ---- pixels ---- */

static lg_color_t blend(lg_color_t fg, lg_color_t bg, uint8_t a)   /* a: 0..255 */
{
    uint32_t r = ((fg >> 11) * a + (bg >> 11) * (255u - a)) / 255u;
    uint32_t g = (((fg >> 5) & 0x3F) * a + ((bg >> 5) & 0x3F) * (255u - a)) / 255u;
    uint32_t b = ((fg & 0x1F) * a + (bg & 0x1F) * (255u - a)) / 255u;
    return (lg_color_t)((r << 11) | (g << 5) | b);
}

static inline void put(uint8_t *row, int16_t x, lg_color_t c)
{
    row[2 * x] = (uint8_t)(c >> 8);   /* the panel takes big-endian RGB565 */
    row[2 * x + 1] = (uint8_t)c;
}

/* The panel memory row behind screen row y. */
static int16_t memory_row(int16_t y)
{
    if (s.scroll_h <= 0 || y < s.scroll_top || y >= s.scroll_top + s.scroll_h) {
        return y;
    }
    return (int16_t)(s.scroll_top + (y - s.scroll_top + s.scroll_vs) % s.scroll_h);
}

static void send_rows(int16_t x, int16_t mem_y, int16_t w, int16_t h, const uint8_t *px)
{
    xSemaphoreTake(s.done, 0);   /* clear a stale completion */
    (void)lg_bsp_display_draw(x, mem_y, x + w - 1, mem_y + h - 1, px);   /* the driver takes inclusive corners */
    xSemaphoreTake(s.done, pdMS_TO_TICKS(200));
}

/* Sends the band buffer for screen rows y..y+h-1, split wherever the scroll area wraps, so a
 * painter works in screen rows and never sees where the panel keeps them. */
static void send(int16_t x, int16_t y, int16_t w, int16_t h)
{
    int16_t row = 0;
    while (row < h) {
        int16_t start = memory_row((int16_t)(y + row));
        int16_t run = 1;
        while (row + run < h && memory_row((int16_t)(y + row + run)) == start + run) {
            run++;
        }
        send_rows(x, start, w, run, s.buf + (size_t)row * 2u * (size_t)w);
        row = (int16_t)(row + run);
    }
    s.stats.pixels += (uint64_t)w * (uint64_t)h;
}

void lg_draw_scroll_area(int16_t top, int16_t height)
{
    s.scroll_vs = 0;
    if (height <= 0) {
        s.scroll_h = 0;
        (void)lg_bsp_display_scroll_area(0, s.height, 0);
        (void)lg_bsp_display_scroll_to(0);
        return;
    }
    s.scroll_top = top;
    s.scroll_h = height;
    (void)lg_bsp_display_scroll_area((uint16_t)top, (uint16_t)height, (uint16_t)(s.height - top - height));
    (void)lg_bsp_display_scroll_to((uint16_t)top);
}

void lg_draw_scroll(int16_t dy)
{
    if (s.scroll_h <= 0 || dy == 0) {
        return;
    }
    int32_t vs = (s.scroll_vs + dy) % s.scroll_h;
    s.scroll_vs = (int16_t)(vs < 0 ? vs + s.scroll_h : vs);
    (void)lg_bsp_display_scroll_to((uint16_t)(s.scroll_top + s.scroll_vs));
}

void lg_draw_fill(const lg_rect_t *r, lg_color_t color)
{
    int64_t t0 = esp_timer_get_time();
    for (int16_t y = r->y; y < r->y + r->h; y = (int16_t)(y + s.band_lines)) {
        int16_t h = (int16_t)((r->y + r->h - y) < s.band_lines ? (r->y + r->h - y) : s.band_lines);
        for (int16_t row = 0; row < h; row++) {
            uint8_t *line = s.buf + (size_t)row * 2u * (size_t)r->w;
            for (int16_t x = 0; x < r->w; x++) {
                put(line, x, color);
            }
        }
        send(r->x, y, r->w, h);
    }
    s.stats.draw_us += (uint64_t)(esp_timer_get_time() - t0);
}

/* Inside a rounded rectangle of this size? Distance test at pixel centres, corners only. */
static bool in_round(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r)
{
    int16_t cx = x < r ? r : (x >= w - r ? (int16_t)(w - r - 1) : x);
    int16_t cy = y < r ? r : (y >= h - r ? (int16_t)(h - r - 1) : y);
    int32_t dx = x - cx;
    int32_t dy = y - cy;
    return dx * dx + dy * dy <= (int32_t)r * r;
}

/* ---- painting: a region is sent in bands, and painters draw each band, clipped ---- */

static inline uint8_t *pixel(const lg_canvas_t *c, int16_t x, int16_t y)
{
    return s.buf + 2u * ((size_t)(y - c->band.y) * (size_t)c->band.w + (size_t)(x - c->band.x));
}

/* The overlap of a and b in *out; false when they do not overlap. */
static bool intersect(const lg_rect_t *a, const lg_rect_t *b, lg_rect_t *out)
{
    int16_t x1 = a->x > b->x ? a->x : b->x;
    int16_t y1 = a->y > b->y ? a->y : b->y;
    int16_t x2 = (int16_t)((a->x + a->w) < (b->x + b->w) ? (a->x + a->w) : (b->x + b->w));
    int16_t y2 = (int16_t)((a->y + a->h) < (b->y + b->h) ? (a->y + a->h) : (b->y + b->h));
    if (x2 <= x1 || y2 <= y1) {
        return false;
    }
    *out = (lg_rect_t){ x1, y1, (int16_t)(x2 - x1), (int16_t)(y2 - y1) };
    return true;
}

void lg_paint_panel(const lg_canvas_t *c, const lg_rect_t *clip, const lg_rect_t *box, lg_color_t bg,
                    lg_color_t outside, lg_color_t border, uint8_t border_w, uint8_t radius)
{
    lg_rect_t area;
    lg_rect_t visible;
    if (!intersect(&c->band, box, &area) || !intersect(&area, clip, &visible)) {
        return;
    }
    for (int16_t y = visible.y; y < visible.y + visible.h; y++) {
        int16_t yy = (int16_t)(y - box->y);
        for (int16_t x = visible.x; x < visible.x + visible.w; x++) {
            int16_t xx = (int16_t)(x - box->x);
            lg_color_t col = bg;
            if (radius > 0 && !in_round(xx, yy, box->w, box->h, radius)) {
                col = outside;
            } else if (border_w > 0) {
                bool edge = xx < border_w || yy < border_w || xx >= box->w - border_w || yy >= box->h - border_w ||
                            (radius > 0 && !in_round((int16_t)(xx < box->w / 2 ? xx - border_w : xx + border_w),
                                                     (int16_t)(yy < box->h / 2 ? yy - border_w : yy + border_w),
                                                     box->w, box->h, radius));
                if (edge) {
                    col = border;
                }
            }
            uint8_t *p = pixel(c, x, y);
            p[0] = (uint8_t)(col >> 8);   /* the panel takes big-endian RGB565 */
            p[1] = (uint8_t)col;
        }
    }
}

void lg_paint_text(const lg_canvas_t *c, const lg_rect_t *clip, int16_t x, int16_t line_top, const lg_font_t *font,
                   const lg_font_t *fallback, lg_color_t fg, const char *text, size_t len)
{
    lg_rect_t visible;
    if (font == NULL || !intersect(&c->band, clip, &visible) || line_top >= visible.y + visible.h ||
        line_top + font->line_height + 8 <= visible.y) {
        return;
    }
    const char *p = text;
    const char *end = text + len;
    while (p < end && *p != '\0' && x < visible.x + visible.w) {
        const lg_glyph_t *g = NULL;
        const lg_font_t *f = find_glyph(font, fallback, utf8_next(&p), &g);
        if (f == NULL) {
            continue;
        }
        /* Glyph top from the line top, via the font's base line. The
         * fallback is centred on the main font's line, as the theme's fallback copies do. */
        int16_t top = (int16_t)(line_top + (font->line_height - font->base_line) - g->box_h - g->ofs_y);
        if (f != font) {
            top = (int16_t)(top + (font->line_height - f->line_height) / 2 + (f->base_line - font->base_line));
        }
        int16_t left = (int16_t)(x + g->ofs_x);
        const lg_font_t *d = f;
        x = (int16_t)(x + advance(g));
        if (d->bpp != 4 || top >= visible.y + visible.h || top + g->box_h <= visible.y ||
            left >= visible.x + visible.w || left + g->box_w <= visible.x) {
            continue;
        }
        const uint8_t *bits = &d->bitmap[g->bitmap_index];
        for (int16_t gy = 0; gy < g->box_h; gy++) {
            int16_t y = (int16_t)(top + gy);
            if (y < visible.y || y >= visible.y + visible.h) {
                continue;
            }
            for (int16_t gx = 0; gx < g->box_w; gx++) {
                int16_t xx = (int16_t)(left + gx);
                if (xx < visible.x || xx >= visible.x + visible.w) {
                    continue;
                }
                /* 4 bits per pixel, packed without row padding, blended over what is there. */
                uint32_t bit = (uint32_t)gy * g->box_w * 4u + (uint32_t)gx * 4u;
                uint8_t a4 = (uint8_t)((bits[bit >> 3] >> (4u - (bit & 7u))) & 0x0Fu);
                if (a4 == 0) {
                    continue;
                }
                uint8_t *px = pixel(c, xx, y);
                lg_color_t under = (lg_color_t)((px[0] << 8) | px[1]);
                lg_color_t col = blend(fg, under, (uint8_t)(a4 * 17u));
                px[0] = (uint8_t)(col >> 8);
                px[1] = (uint8_t)col;
            }
        }
    }
}

void lg_paint_pixel(const lg_canvas_t *c, const lg_rect_t *clip, int16_t x, int16_t y, lg_color_t color, uint8_t alpha)
{
    if (alpha == 0 || x < clip->x || x >= clip->x + clip->w || y < clip->y || y >= clip->y + clip->h ||
        x < c->band.x || x >= c->band.x + c->band.w || y < c->band.y || y >= c->band.y + c->band.h) {
        return;
    }
    uint8_t *px = pixel(c, x, y);
    lg_color_t col = color;
    if (alpha < 255) {
        col = blend(color, (lg_color_t)((px[0] << 8) | px[1]), alpha);
    }
    px[0] = (uint8_t)(col >> 8);
    px[1] = (uint8_t)col;
}

uint8_t lg_text_wrap(const lg_font_t *font, const lg_font_t *fallback, const char *text, int16_t max_w,
                     uint16_t *starts, uint8_t max_lines)
{
    uint8_t lines = 0;
    const char *p = text;
    while (*p != '\0' && lines < max_lines) {
        while (*p == ' ') {
            p++;   /* a wrapped line does not start with the space it broke at */
        }
        if (*p == '\0') {
            break;
        }
        starts[lines++] = (uint16_t)(p - text);
        int16_t w = 0;
        const char *last_space = NULL;
        const char *q = p;
        while (*q != '\0' && *q != '\n') {
            const char *before = q;
            const lg_glyph_t *g = NULL;
            uint32_t cp = utf8_next(&q);
            int16_t adv = find_glyph(font, fallback, cp, &g) != NULL ? advance(g) : 0;
            if (w + adv > max_w && before != p) {
                q = last_space != NULL ? last_space : before;   /* break at the last space, or mid-word */
                break;
            }
            if (cp == ' ') {
                last_space = before;
            }
            w = (int16_t)(w + adv);
        }
        p = *q == '\n' ? q + 1 : q;
    }
    starts[lines] = (uint16_t)strlen(text);   /* one past the last line: its end */
    return lines;
}

void lg_draw_region(const lg_rect_t *r, lg_painter_t paint, void *ctx)
{
    int64_t t0 = esp_timer_get_time();
    for (int16_t y = r->y; y < r->y + r->h; y = (int16_t)(y + s.band_lines)) {
        int16_t h = (int16_t)((r->y + r->h - y) < s.band_lines ? (r->y + r->h - y) : s.band_lines);
        lg_canvas_t c = { .band = { r->x, y, r->w, h } };
        paint(&c, ctx);
        send(r->x, y, r->w, h);
    }
    s.stats.draw_us += (uint64_t)(esp_timer_get_time() - t0);
}

static void paint_box(const lg_canvas_t *c, void *ctx)
{
    const lg_box_t *b = ctx;
    lg_paint_panel(c, &b->rect, &b->rect, b->bg, b->outside, b->border, b->border_w, b->radius);
    if (b->font == NULL || b->text[0] == '\0') {
        return;
    }
    const lg_rect_t *r = &b->rect;
    int16_t tw = lg_draw_text_width(b->font, b->fallback, b->text);
    int16_t x = b->align == LG_ALIGN_CENTER ? (int16_t)(r->x + (r->w - tw) / 2)
              : b->align == LG_ALIGN_RIGHT  ? (int16_t)(r->x + r->w - b->pad - tw)
                                            : (int16_t)(r->x + b->pad);
    int16_t line_top = (int16_t)(r->y + (r->h - b->font->line_height) / 2 + b->text_dy);
    lg_paint_text(c, r, x, line_top, b->font, b->fallback, b->fg, b->text, strlen(b->text));
}

void lg_draw_box(const lg_box_t *b)
{
    lg_draw_region(&b->rect, paint_box, (void *)b);
    s.stats.boxes++;
}

bool lg_draw_set_text(lg_box_t *box, const char *text)
{
    if (strncmp(box->text, text, LG_BOX_TEXT_MAX - 1u) == 0) {
        return false;
    }
    strncpy(box->text, text, LG_BOX_TEXT_MAX - 1u);
    box->text[LG_BOX_TEXT_MAX - 1u] = '\0';
    lg_draw_box(box);
    return true;
}

/* ---- touch ---- */

#define TOUCH_CONFIRM 2

bool lg_draw_touch(int16_t *x, int16_t *y)
{
    lg_bsp_touch_raw_t raw;
    int16_t tx = 0;
    int16_t ty = 0;
    if (lg_bsp_touch_read_raw(&raw) && lg_bsp_touch_map(&raw, &tx, &ty)) {
        s.last_x = tx;
        s.last_y = ty;
        s.up_run = 0;
        if (s.down_run < TOUCH_CONFIRM) {
            s.down_run++;
        }
    } else {
        s.down_run = 0;
        if (s.up_run < TOUCH_CONFIRM) {
            s.up_run++;
        }
    }
    if (!s.down && s.down_run >= TOUCH_CONFIRM) {
        s.down = true;
    } else if (s.down && s.up_run >= TOUCH_CONFIRM) {
        s.down = false;
    }
    *x = s.last_x;
    *y = s.last_y;
    return s.down;
}
