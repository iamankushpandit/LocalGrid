/*
 * ui_theme.h - the screens' colours and fonts: the Terminal theme's roles as RGB565, in one place
 * for every screen (D10).
 */
#pragma once

#include "lg_draw.h"
#include "lg_emoji.h"

#define C_BG         lg_rgb(0x000000)
#define C_BAR        lg_rgb(0x0B1A12)
#define C_SURFACE    lg_rgb(0x0A140F)
#define C_OUTLINE    lg_rgb(0x1F3A2A)
#define C_TEXT       lg_rgb(0xD2F5DE)
#define C_MUTED      lg_rgb(0x7FA78F)
#define C_ACCENT     lg_rgb(0x5FD38D)
#define C_ACCENT_INK lg_rgb(0x06120C)
#define C_WARNING    lg_rgb(0xF0B64A)
#define C_ERROR      lg_rgb(0xFF6B6B)
#define C_MARK_WAIT  lg_rgb(0x7FA78F)
#define C_MARK_NODE  lg_rgb(0x4A8FD4)
#define C_MARK_DELIV lg_rgb(0x5FD38D)
#define C_MARK_READ  lg_rgb(0xB98CFF)

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
