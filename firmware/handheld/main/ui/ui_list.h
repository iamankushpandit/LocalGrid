/*
 * ui_list.h - a list screen: a header, optional tabs, and rows that scroll with
 * the panel's hardware scroll. Settings, Groups, the group editor, and the conversation list are
 * all built from it. One list exists at a time; a screen rebuilds its rows and shows them.
 * UI task only.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lg_draw.h"

#define SLIST_ROWS       28
#define SLIST_LABEL_MAX  32
#define SLIST_VALUE_MAX  112

typedef enum {
    ROW_SECTION,   /* a small heading */
    ROW_FACT,      /* label and value, not tappable */
    ROW_ACTION,    /* label, value, and a chevron: tappable */
    ROW_CHOICE,    /* label over segments: tapping a segment chooses it */
    ROW_BUTTON,    /* a full-width button */
    ROW_NOTE,      /* wrapped muted text */
    ROW_CHECK,     /* a checkbox and label */
    ROW_FIELD,     /* a one-line text field: value, or label as placeholder */
} slist_kind_t;

typedef enum { BTN_MAIN, BTN_PLAIN, BTN_DANGER } slist_button_t;

typedef struct {
    uint8_t            kind;
    uint8_t            style;       /* slist_button_t for buttons */
    bool               checked;
    bool               warn;        /* value in the warning colour */
    bool               focused;     /* a field being typed into */
    bool               muted;       /* a row that cannot be used */
    int16_t            id;
    int32_t            arg;
    char               label[SLIST_LABEL_MAX];
    char               value[SLIST_VALUE_MAX];
    const char *const *options;     /* choice */
    uint8_t            n_options;
    uint8_t            selected;
    int16_t            y;           /* layout, list coordinates */
    int16_t            h;
} slist_row_t;

typedef enum { SLIST_NONE, SLIST_ROW, SLIST_TAB, SLIST_BACK, SLIST_PLUS } slist_event_type_t;

typedef struct {
    uint8_t type;
    int16_t id;       /* the row's id */
    int32_t arg;      /* the row's arg; for a choice, the segment tapped */
    uint8_t index;    /* the row index, or the tab */
} slist_event_t;

/* Starts a new set of rows. back: the header icon is a back arrow, else a house. plus: a + too. */
void slist_begin(const char *title, bool back, bool plus);
void slist_tabs(const char *const *names, uint8_t n, uint8_t selected);

slist_row_t *slist_add(slist_kind_t kind, const char *label, const char *value, int16_t id, int32_t arg);

/* Lays the rows out and draws the whole screen. bottom: pixels kept free at the bottom (a
 * keyboard). keep_scroll: stay where the reader was. */
void slist_show(uint16_t w, uint16_t h, int16_t bottom, bool keep_scroll);

/* Draws header and rows again where the reader was (after an overlay). */
void slist_redraw(void);

/* Rows changed but header and tabs did not: lays out again and repaints the list only. */
void slist_update(void);

/* Repaints one row, when only its content changed (a field being typed into). */
void slist_repaint_row(uint8_t index);

slist_row_t *slist_row(uint8_t index);
uint8_t slist_count(void);

bool slist_touch(int16_t x, int16_t y, bool down, slist_event_t *event);
void slist_scroll_by(int16_t dy);
