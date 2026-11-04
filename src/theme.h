/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/* Colour themes (Settings > Appearance > Theme): T_* are the current theme's colours
 * (0xRRGGBB, turn into a Color with color_hex()), T_SYN_* the editor's syntax highlighting.
 * They are read at draw time, so switching themes repaints everything on the next frame.
 * M_* are layout metrics (UI points; convert to render pixels with px()). */
#include <stdbool.h>
#include <stdint.h>

typedef struct LumeTheme {
    const char *id, *name;   /* stable id saved in settings.txt ("dracula"), display name */
    bool light;
    uint32_t bg, panel, panel_alt, editor_bg, line_hl, border, hover, selected;
    uint32_t accent, accent_dim, accent_hover, on_accent;   /* on_accent: text on an accent fill */
    uint32_t text, text_muted, text_dim;   /* contrast on panel: primary >= 7:1, secondary / tertiary >= 4.5:1 */
    uint32_t green, red, yellow, scrollbar, warn_bg;   /* status colours; warn_bg: warning banners */
    uint32_t syn_keyword, syn_string, syn_number, syn_comment, syn_func, syn_op, syn_punct;
} LumeTheme;
extern const LumeTheme *g_theme;   /* never NULL */

int theme_count(void);
const LumeTheme *theme_at(int i);          /* NULL out of range */
/* Chooses a theme by id, or "system" (Lume Dark / Lume Light following the OS). False, and
 * nothing changes, when the id is unknown. */
bool theme_select(const char *id);
const char *theme_choice(void);           /* the id chosen: a theme id or "system" */
/* With "system": picks again after the OS appearance changed. True when the theme changed. */
bool theme_follow_system(void);

#define T_BG          (g_theme->bg)          /* deepest chrome: menu bar, tab strip, grid header */
#define T_PANEL       (g_theme->panel)       /* side/bottom chrome */
#define T_PANEL_ALT   (g_theme->panel_alt)   /* panel headers, status bar */
#define T_EDITOR_BG   (g_theme->editor_bg)   /* editor text area */
#define T_LINE_HL     (g_theme->line_hl)     /* editor current-line highlight */
#define T_BORDER      (g_theme->border)      /* 1-px separators and outlines */
#define T_HOVER       (g_theme->hover)       /* hovered rows / buttons */
#define T_SELECTED    (g_theme->selected)    /* selected rows, text selection */
#define T_ACCENT      (g_theme->accent)      /* active / focused elements */
#define T_ACCENT_DIM  (g_theme->accent_dim)  /* muted accent (e.g. the table icon header) */
#define T_ACCENT_HOVER (g_theme->accent_hover)
#define T_ON_ACCENT   (g_theme->on_accent)
#define T_TEXT        (g_theme->text)
#define T_TEXT_MUTED  (g_theme->text_muted)
#define T_TEXT_DIM    (g_theme->text_dim)
#define T_GREEN       (g_theme->green)
#define T_RED         (g_theme->red)
#define T_YELLOW      (g_theme->yellow)
#define T_SCROLLBAR   (g_theme->scrollbar)
#define T_WARN_BG     (g_theme->warn_bg)

/* SQL syntax highlighting (editor.c) */
#define T_SYN_KEYWORD (g_theme->syn_keyword)
#define T_SYN_STRING  (g_theme->syn_string)
#define T_SYN_NUMBER  (g_theme->syn_number)
#define T_SYN_COMMENT (g_theme->syn_comment)
#define T_SYN_FUNC    (g_theme->syn_func)
#define T_SYN_OP      (g_theme->syn_op)
#define T_SYN_PUNCT   (g_theme->syn_punct)

/* layout metrics, in UI points */
#define M_MENUBAR     30.0f
#define M_STATUSBAR   24.0f
#define M_HEADER      30.0f   /* panel header height */
#define M_ROW         24.0f   /* list row height (Explorer, menus) */
#define M_GRID_ROW    22.0f   /* results grid row / header heights */
#define M_GRID_HEAD   26.0f
#define M_FONT_UI     13.0f   /* font sizes: UI, small labels, editor monospace */
#define M_FONT_SMALL  11.0f
#define M_FONT_MONO   13.0f
#define M_RADIUS      5.0f    /* default corner radius */
#define M_SIDEBAR_MIN 180.0f  /* sidebar width limits and minimum results height */
#define M_SIDEBAR_MAX 520.0f
#define M_RESULTS_MIN 120.0f
