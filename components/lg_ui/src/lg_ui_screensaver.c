/*
 * The screen saver: green characters falling down the panel, after the films.
 *
 * It sits on the top layer, so the screen underneath keeps its widgets and its scroll
 * position and nothing is rebuilt when the saver goes away. One timer does both jobs: while
 * the saver is hidden it asks LVGL how long the panel has gone untouched, and while it shows
 * it moves the columns. The first touch only dismisses it, the way a phone does, because the
 * cover swallows that press instead of letting it reach a button underneath.
 *
 * Nothing here is allocated per frame: the columns and their text are fixed arrays, sized at
 * start from the panel that is actually there (D9), and the colours come from the theme (D10).
 */
#include "lg_ui_screensaver.h"

#include <string.h>

#include "lg_bsp_settings.h"
#include "lg_display.h"
#include "lg_theme.h"

#define COLUMNS_MAX   30
#define TRAIL         14        /* characters following the head down the panel */
#define STEP_MS       40        /* one move of the rain, which is a whole row */
#define IDLE_CHECK_MS 250       /* how often we ask whether the panel is still untouched */

/*
 * The setting is stored by lg_bsp, not here. lg_ui reaches platform services only through the
 * board support layer (D27), and the layer check rejected this file's first attempt to open
 * NVS itself before the build could even start.
 */
#define SETTING_SAVER "saver"

static struct {
    bool        enabled;
    lv_obj_t   *cover;
    lv_obj_t   *head[COLUMNS_MAX];
    lv_obj_t   *tail[COLUMNS_MAX];
    int32_t     y[COLUMNS_MAX];
    int32_t     speed[COLUMNS_MAX];   /* steps between moves; 1 is the fastest column */
    int32_t     wait[COLUMNS_MAX];
    char        text[COLUMNS_MAX][TRAIL * 2 + 1];   /* a character and a newline per row */
    char        head_text[COLUMNS_MAX][2];
    uint8_t     columns;
    int32_t     line_h;
    int32_t     width;      /* taken from the panel at start: a percentage size is not
                             * resolved until LVGL has laid the cover out */
    int32_t     height;
    uint32_t    idle_ms;
    bool        showing;
    lv_timer_t *timer;
} s;

/* xorshift, so the rain needs no libc random and no allocation. */
static uint32_t rnd(void)
{
    static uint32_t state = 0x1F35A2C7u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

/* Katakana would need another font; these are characters the theme's font already carries. */
static char glyph(void)
{
    static const char SET[] = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ<>*+-/|=";
    return SET[rnd() % (sizeof(SET) - 1u)];
}

static void fill_column(uint8_t i)
{
    size_t k = 0;
    for (uint8_t row = 0; row < TRAIL; row++) {
        s.text[i][k++] = glyph();
        if (row + 1 < TRAIL) {
            s.text[i][k++] = '\n';
        }
    }
    s.text[i][k] = '\0';
    s.head_text[i][0] = glyph();
    s.head_text[i][1] = '\0';
    s.speed[i] = (int32_t)(1u + (rnd() % 2u));            /* a move every 1 or 2 steps */
    s.wait[i] = 0;
    s.y[i] = -(int32_t)(rnd() % (uint32_t)(s.height + 1));  /* start scattered above */
}

static void place_column(uint8_t i)
{
    int32_t x = (int32_t)i * (s.width / (int32_t)s.columns);
    lv_obj_set_pos(s.tail[i], x, s.y[i] - TRAIL * s.line_h);
    lv_obj_set_pos(s.head[i], x, s.y[i]);
}

static void animate(void)
{
    for (uint8_t i = 0; i < s.columns; i++) {
        if (++s.wait[i] < s.speed[i]) {
            continue;   /* one of the slower columns, so it holds where it is this step */
        }
        s.wait[i] = 0;
        /* A whole row at a time. Stepping by pixels moved every column every tick, which
         * redraws most of the panel: 240x320 at two bytes is 150 KB, about 30 ms of SPI on
         * the faster board, so smooth scrolling ran out of bandwidth before it looked fast. */
        s.y[i] += s.line_h;
        if (s.y[i] - TRAIL * s.line_h > s.height) {
            fill_column(i);
            lv_label_set_text(s.tail[i], s.text[i]);
            lv_label_set_text(s.head[i], s.head_text[i]);
        } else if ((rnd() & 0x0Fu) == 0) {
            /* One character flickers, which is what makes the rain look alive. */
            s.head_text[i][0] = glyph();
            lv_label_set_text(s.head[i], s.head_text[i]);
        }
        place_column(i);
    }
}

static void on_touch(lv_event_t *e);

/*
 * The rain's sixty labels exist only while it falls. Kept hidden for the life of the handheld
 * they cost about 8 KB on the Hosyond, which has none to spare, for something shown after a
 * minute of nobody touching the panel. Built on show, freed on hide.
 */
static void build_cover(void)
{
    const lg_theme_t *t = lg_theme();
    s.cover = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s.cover);
    lv_obj_set_size(s.cover, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s.cover, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s.cover, LV_OPA_COVER, 0);
    lv_obj_add_flag(s.cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s.cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s.cover, on_touch, LV_EVENT_PRESSED, NULL);

    for (uint8_t i = 0; i < s.columns; i++) {
        s.tail[i] = lv_label_create(s.cover);
        lv_obj_set_style_text_font(s.tail[i], t->font_small, 0);
        lv_obj_set_style_text_color(s.tail[i], t->success, 0);
        lv_obj_set_style_text_line_space(s.tail[i], 0, 0);
        s.head[i] = lv_label_create(s.cover);
        lv_obj_set_style_text_font(s.head[i], t->font_small, 0);
        lv_obj_set_style_text_color(s.head[i], t->text, 0);   /* the head is the bright one */
    }
}

static void show(void)
{
    if (s.cover == NULL) {
        build_cover();
    }
    s.showing = true;
    for (uint8_t i = 0; i < s.columns; i++) {
        fill_column(i);
        lv_label_set_text(s.tail[i], s.text[i]);
        lv_label_set_text(s.head[i], s.head_text[i]);
        place_column(i);
    }
    lv_obj_remove_flag(s.cover, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s.cover);
    lv_timer_set_period(s.timer, STEP_MS);
}

static void hide(void)
{
    s.showing = false;
    if (s.cover != NULL) {
        /* Async: hide() also runs from the cover's own touch event. */
        lv_obj_delete_async(s.cover);
        s.cover = NULL;
        memset(s.head, 0, sizeof(s.head));
        memset(s.tail, 0, sizeof(s.tail));
    }
    lv_timer_set_period(s.timer, IDLE_CHECK_MS);
}

static void on_touch(lv_event_t *e)
{
    (void)e;
    hide();   /* the first touch only dismisses, so it cannot also press what is underneath */
}

static void tick(lv_timer_t *timer)
{
    (void)timer;
    if (!s.enabled) {
        if (s.showing) {
            hide();   /* switched off while it was running */
        }
        return;
    }
    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (!s.showing) {
        if (idle >= s.idle_ms) {
            show();
        }
    } else if (idle < s.idle_ms) {
        hide();
    } else {
        animate();
    }
}

bool lg_ui_screensaver_enabled(void)
{
    return s.enabled;
}

void lg_ui_screensaver_set_enabled(bool enabled)
{
    s.enabled = enabled;
    /* Also called from the console task (D28), and hide() deletes widgets the drawing task may be walking.
     * The lock is recursive, so Settings calling this from the drawing task is unaffected. */
    lg_display_lock(1000);
    if (!enabled && s.showing) {
        hide();
    }
    lg_display_unlock();
    lg_bsp_setting_set_bool(SETTING_SAVER, enabled);
}

bool lg_ui_screensaver_showing(void)
{
    return s.showing;
}

void lg_ui_screensaver_dismiss(void)
{
    lg_display_lock(1000);   /* recursive: safe from the drawing task and from any other */
    if (s.showing) {
        hide();
    }
    lg_display_unlock();
}

void lg_ui_screensaver_start(uint32_t idle_ms)
{
    if (s.timer != NULL) {
        s.idle_ms = idle_ms;
        return;
    }
    s.enabled = lg_bsp_setting_get_bool(SETTING_SAVER, true);   /* on unless it was turned off */
    const lg_theme_t *t = lg_theme();
    lv_display_t *disp = lv_display_get_default();
    int32_t w = lv_display_get_horizontal_resolution(disp);
    s.width = w;
    s.height = lv_display_get_vertical_resolution(disp);
    s.idle_ms = idle_ms;
    s.line_h = lv_font_get_line_height(t->font_small);

    /* One column per character width the panel can hold, up to what memory allows. */
    int32_t char_w = lv_font_get_glyph_width(t->font_small, 'W', 'W');
    /* Every character cell, not every other one: half-spaced columns read as scattered
     * streaks rather than as rain. Thirty columns on a 240 px panel is sixty labels, which
     * is about 6 KB more than the spaced version on the board with the least to spare. */
    int32_t fit = char_w > 0 ? w / char_w : 8;
    s.columns = (uint8_t)(fit > COLUMNS_MAX ? COLUMNS_MAX : (fit < 4 ? 4 : fit));

    s.timer = lv_timer_create(tick, IDLE_CHECK_MS, NULL);
}
