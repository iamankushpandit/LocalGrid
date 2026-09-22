/*
 * ui_theme.h - the screens' colours and fonts, in one place for every screen (D10).
 *
 * Two themes, chosen at runtime and remembered:
 *
 *   Night     the original Terminal look: pale text on near-black. Easy on the eyes indoors and
 *             after dark, and it is what the product has always looked like.
 *   Daylight  black text on white. Asked for from use outdoors (owner, 2026-09-21: "it was
 *             difficult to read otherwise"): in sunlight a screen is competing with the sky, and
 *             what wins is the largest possible difference between ink and paper, with the paper
 *             bright. Every foreground colour in this theme is dark, because a mid-tone accent
 *             that looks fine on black is nearly invisible on white outdoors.
 *
 * The colour names below stay exactly as they were, so no screen had to change: they now read a
 * live table rather than a compile-time constant. That also means a colour is no longer usable in
 * a static initialiser - put it in a local or assign it at runtime.
 */
#pragma once

#include "lg_draw.h"
#include "lg_emoji.h"

typedef struct {
    const char *name;
    lg_color_t  bg;
    lg_color_t  bar;
    lg_color_t  surface;
    lg_color_t  outline;
    lg_color_t  text;
    lg_color_t  muted;
    lg_color_t  accent;
    lg_color_t  accent_ink;
    lg_color_t  warning;
    lg_color_t  error;
    lg_color_t  mark_wait;
    lg_color_t  mark_node;
    lg_color_t  mark_deliv;
    lg_color_t  mark_read;
} ui_theme_t;

typedef enum {
    UI_THEME_NIGHT = 0,
    UI_THEME_DAYLIGHT = 1,
} ui_theme_kind_t;

/* The theme every screen draws with. Never NULL: it points at Night until a choice is loaded. */
extern const ui_theme_t *ui_theme;

#define C_BG         (ui_theme->bg)
#define C_BAR        (ui_theme->bar)
#define C_SURFACE    (ui_theme->surface)
#define C_OUTLINE    (ui_theme->outline)
#define C_TEXT       (ui_theme->text)
#define C_MUTED      (ui_theme->muted)
#define C_ACCENT     (ui_theme->accent)
#define C_ACCENT_INK (ui_theme->accent_ink)
#define C_WARNING    (ui_theme->warning)
#define C_ERROR      (ui_theme->error)
#define C_MARK_WAIT  (ui_theme->mark_wait)
#define C_MARK_NODE  (ui_theme->mark_node)
#define C_MARK_DELIV (ui_theme->mark_deliv)
#define C_MARK_READ  (ui_theme->mark_read)

/* Loads the remembered choice. Call once, before the first screen is drawn. */
void ui_theme_start(void);

ui_theme_kind_t ui_theme_kind(void);
const char *ui_theme_name(ui_theme_kind_t kind);

/*
 * Switches theme, remembers it, and repaints the screen that is showing: a retained screen (D55)
 * keeps what it drew, so without a repaint half the display would stay in the old colours.
 */
void ui_theme_set(ui_theme_kind_t kind);

#define F_TINY   (&lg_font_montserrat_10)
#define F_SMALL  (&lg_font_montserrat_12)
#define F_BODY   (&lg_font_montserrat_14)
#define F_ICON   (&lg_font_montserrat_16)
#define F_TITLE  (&lg_font_montserrat_20)
#define F_HUGE   (&lg_font_montserrat_28)
#define F_EMOJI  (&lg_font_emoji_14)
#define F_EMOJI_KEY (&lg_font_emoji_20)

#define UI_PAD     6
#define UI_GAP     4
#define UI_HEAD_H  30
