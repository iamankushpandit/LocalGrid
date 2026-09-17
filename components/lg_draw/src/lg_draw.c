#include "lg_draw.h"

#include <string.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lg_bsp_display.h"
#include "lg_bsp_touch.h"

#define GLYPHS_MAX 64u

typedef struct {
    int16_t                              x;        /* left of the glyph box */
    int16_t                              y;        /* top of the glyph box */
    const lv_font_fmt_txt_glyph_dsc_t   *dsc;
    const lv_font_fmt_txt_dsc_t         *font;
} placed_t;

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

const lg_draw_stats_t *lg_draw_stats(void)
{
    return &s.stats;
}

/* ---- fonts: LVGL's fmt_txt tables, read directly ---- */

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

static uint32_t glyph_id(const lv_font_fmt_txt_dsc_t *d, uint32_t cp)
{
    for (uint16_t i = 0; i < d->cmap_num; i++) {
        const lv_font_fmt_txt_cmap_t *c = &d->cmaps[i];
        if (cp < c->range_start) {
            continue;
        }
        uint32_t rcp = cp - c->range_start;
        if (rcp >= c->range_length) {
            continue;
        }
        switch (c->type) {
        case LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY:
            return c->glyph_id_start + rcp;
        case LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL:
            return c->glyph_id_start + ((const uint8_t *)c->glyph_id_ofs_list)[rcp];
        case LV_FONT_FMT_TXT_CMAP_SPARSE_TINY:
        case LV_FONT_FMT_TXT_CMAP_SPARSE_FULL:
            for (uint16_t k = 0; k < c->list_length; k++) {   /* short lists: a scan beats a search */
                if (c->unicode_list[k] == rcp) {
                    if (c->type == LV_FONT_FMT_TXT_CMAP_SPARSE_TINY) {
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

/* The glyph for cp in font, then in fallback. Only uncompressed tables are read. */
static const lv_font_t *find_glyph(const lv_font_t *font, const lv_font_t *fallback, uint32_t cp,
                                   const lv_font_fmt_txt_glyph_dsc_t **out)
{
    const lv_font_t *try[2] = { font, fallback };
    for (int i = 0; i < 2; i++) {
        if (try[i] == NULL) {
            continue;
        }
        const lv_font_fmt_txt_dsc_t *d = try[i]->dsc;
        if (d->bitmap_format != LV_FONT_FMT_TXT_PLAIN) {
            continue;
        }
        uint32_t id = glyph_id(d, cp);
        if (id != 0) {
            *out = &d->glyph_dsc[id];
            return try[i];
        }
    }
    return NULL;
}

static int16_t advance(const lv_font_fmt_txt_glyph_dsc_t *g)
{
    return (int16_t)((g->adv_w + 8u) >> 4);
}

int16_t lg_draw_text_width(const lv_font_t *font, const lv_font_t *fallback, const char *text)
{
    int16_t w = 0;
    const char *p = text;
    while (*p != '\0') {
        const lv_font_fmt_txt_glyph_dsc_t *g = NULL;
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

static inline lg_color_t get(const uint8_t *row, int16_t x)
{
    return (lg_color_t)((row[2 * x] << 8) | row[2 * x + 1]);
}

static void send(int16_t x, int16_t y, int16_t w, int16_t h)
{
    xSemaphoreTake(s.done, 0);   /* clear a stale completion */
    (void)lg_bsp_display_draw(x, y, x + w - 1, y + h - 1, s.buf);   /* the driver takes inclusive corners */
    xSemaphoreTake(s.done, pdMS_TO_TICKS(200));
    s.stats.pixels += (uint64_t)w * (uint64_t)h;
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

void lg_draw_box(const lg_box_t *b)
{
    int64_t t0 = esp_timer_get_time();
    const lg_rect_t *r = &b->rect;

    /* Lay the text out once: glyph positions relative to the box. */
    static placed_t glyphs[GLYPHS_MAX];
    size_t n = 0;
    if (b->font != NULL && b->text[0] != '\0') {
        int16_t tw = lg_draw_text_width(b->font, b->fallback, b->text);
        int16_t x = b->align == LG_ALIGN_CENTER ? (int16_t)((r->w - tw) / 2)
                  : b->align == LG_ALIGN_RIGHT  ? (int16_t)(r->w - b->pad - tw)
                                                : b->pad;
        int16_t line_top = (int16_t)((r->h - b->font->line_height) / 2 + b->text_dy);
        const char *p = b->text;
        while (*p != '\0' && n < GLYPHS_MAX) {
            const lv_font_fmt_txt_glyph_dsc_t *g = NULL;
            const lv_font_t *f = find_glyph(b->font, b->fallback, utf8_next(&p), &g);
            if (f == NULL) {
                continue;
            }
            /* Same placement as LVGL: glyph top from the line top, via the font's base line. The
             * fallback is centred on the main font's line, as the theme's fallback copies do. */
            int16_t top = (int16_t)(line_top + (b->font->line_height - b->font->base_line) - g->box_h - g->ofs_y);
            if (f != b->font) {
                top = (int16_t)(top + (b->font->line_height - f->line_height) / 2 +
                                (f->base_line - b->font->base_line));
            }
            glyphs[n].x = (int16_t)(x + g->ofs_x);
            glyphs[n].y = top;
            glyphs[n].dsc = g;
            glyphs[n].font = f->dsc;
            n++;
            x = (int16_t)(x + advance(g));
        }
    }

    for (int16_t by = 0; by < r->h; by = (int16_t)(by + s.band_lines)) {
        int16_t bh = (int16_t)((r->h - by) < s.band_lines ? (r->h - by) : s.band_lines);
        /* Panel: background, border, rounded corners. */
        for (int16_t row = 0; row < bh; row++) {
            int16_t yy = (int16_t)(by + row);
            uint8_t *line = s.buf + (size_t)row * 2u * (size_t)r->w;
            for (int16_t xx = 0; xx < r->w; xx++) {
                lg_color_t c = b->bg;
                if (b->radius > 0 && !in_round(xx, yy, r->w, r->h, b->radius)) {
                    c = b->outside;
                } else if (b->border_w > 0) {
                    bool edge = xx < b->border_w || yy < b->border_w || xx >= r->w - b->border_w ||
                                yy >= r->h - b->border_w ||
                                (b->radius > 0 && !in_round((int16_t)(xx < r->w / 2 ? xx - b->border_w : xx + b->border_w),
                                                            (int16_t)(yy < r->h / 2 ? yy - b->border_w : yy + b->border_w),
                                                            r->w, r->h, b->radius));
                    if (edge) {
                        c = b->border;
                    }
                }
                put(line, xx, c);
            }
        }
        /* Glyphs: 4 bits per pixel, packed without row padding, blended over what is there. */
        for (size_t i = 0; i < n; i++) {
            const placed_t *gp = &glyphs[i];
            const lv_font_fmt_txt_glyph_dsc_t *g = gp->dsc;
            if (gp->y >= by + bh || gp->y + g->box_h <= by || gp->font->bpp != 4) {
                continue;
            }
            const uint8_t *bits = &gp->font->glyph_bitmap[g->bitmap_index];
            for (int16_t gy = 0; gy < g->box_h; gy++) {
                int16_t yy = (int16_t)(gp->y + gy);
                if (yy < by || yy >= by + bh) {
                    continue;
                }
                uint8_t *line = s.buf + (size_t)(yy - by) * 2u * (size_t)r->w;
                for (int16_t gx = 0; gx < g->box_w; gx++) {
                    int16_t xx = (int16_t)(gp->x + gx);
                    uint32_t bit = (uint32_t)gy * g->box_w * 4u + (uint32_t)gx * 4u;
                    uint8_t a4 = (uint8_t)((bits[bit >> 3] >> (4u - (bit & 7u))) & 0x0Fu);
                    if (a4 == 0 || xx < 0 || xx >= r->w) {
                        continue;
                    }
                    put(line, xx, blend(b->fg, get(line, xx), (uint8_t)(a4 * 17u)));
                }
            }
        }
        send(r->x, (int16_t)(r->y + by), r->w, bh);
    }
    s.stats.boxes++;
    s.stats.draw_us += (uint64_t)(esp_timer_get_time() - t0);
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
