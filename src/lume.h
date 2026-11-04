/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/* lume.h: the shared core header. Geometry and color helpers, the per-frame Input record,
 * the App struct that main.c owns (one per process) and the app_* services modules call back
 * into. Every module includes it.
 *
 * Units used across the code base (see also units.h):
 *   render pixels  - what the renderer draws in and Input reports; Rects are in these
 *   UI points      - layout constants (theme.h); px(a, pt) = pt * App.scale converts
 *   window units   - SDL window / event coordinates; render px = window units * App.density
 * Everything here is main-thread only unless a comment says otherwise. */
#include <SDL3/SDL.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- basic geometry & color ------------------------------------------- */

typedef struct { float x, y, w, h; } Rect;          /* top-left corner and size, usually render pixels */
typedef struct { uint8_t r, g, b, a; } Color;     /* 8-bit RGBA, straight (non-premultiplied) alpha */

/* Build a Rect from position and size. */
static inline Rect rect_make(float x, float y, float w, float h) { Rect r = { x, y, w, h }; return r; }
/* True when (px, py) is inside r (left / top edges inclusive, right / bottom exclusive). */
static inline bool rect_contains(Rect r, float px, float py) {
    return px >= r.x && py >= r.y && px < r.x + r.w && py < r.y + r.h;
}
/* Shrink r by dx on the left and right and dy on top and bottom (negative values grow it). */
static inline Rect rect_inset(Rect r, float dx, float dy) {
    return rect_make(r.x + dx, r.y + dy, r.w - 2 * dx, r.h - 2 * dy);
}
/* Overlap of a and b; zero width / height when they do not overlap. */
static inline Rect rect_intersect(Rect a, Rect b) {
    float x0 = fmaxf(a.x, b.x), y0 = fmaxf(a.y, b.y);
    float x1 = fminf(a.x + a.w, b.x + b.w), y1 = fminf(a.y + a.h, b.y + b.h);
    return rect_make(x0, y0, fmaxf(0, x1 - x0), fmaxf(0, y1 - y0));
}
/* Opaque Color from 0xRRGGBB (the theme.h format). */
static inline Color color_hex(uint32_t hex) {
    Color c = { (uint8_t)(hex >> 16), (uint8_t)(hex >> 8), (uint8_t)hex, 255 };
    return c;
}
/* c with its alpha replaced by a (0..255). */
static inline Color color_alpha(Color c, uint8_t a) { c.a = a; return c; }
/* Linear blend from a (t = 0) to b (t = 1), alpha included. */
static inline Color color_mix(Color a, Color b, float t) {
    Color c = { (uint8_t)(a.r + (b.r - a.r) * t), (uint8_t)(a.g + (b.g - a.g) * t),
                (uint8_t)(a.b + (b.b - a.b) * t), (uint8_t)(a.a + (b.a - a.a) * t) };
    return c;
}
/* Clamp v to [lo, hi]; linear interpolation from a to b. */
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }

/* ---- input ------------------------------------------------------------- */

/* Normalised key modifiers stored in KeyPress.mods (see normalize_mods in main.c). PRIMARY is
 * the platform's command modifier: Cmd on macOS, Ctrl elsewhere. */
enum { KM_PRIMARY = 1, KM_SHIFT = 2, KM_ALT = 4, KM_CTRL = 8 /* Control on macOS; elsewhere Control is PRIMARY */ };

/* One key press of this frame; `used` is set by whoever handles it so nobody else does. */
typedef struct { SDL_Keycode key; unsigned mods; bool used; } KeyPress;
#define LUME_MAX_KEYS 32   /* key presses kept per frame; extra ones are dropped */

/* Input gathered for one frame (main.c fills it from SDL events; widgets read and consume it).
 * Floating panel windows have their own Input swapped in while they are processed. */
typedef struct Input {
    float mouse_x, mouse_y;                  /* in render pixels */
    bool mouse_down, mouse_pressed, mouse_released, mouse_consumed;   /* left button: held; went down / up this frame; a widget already took this press */
    bool mouse_rpressed;                     /* right button, this frame */
    int clicks;                              /* click count of this frame's press */
    float wheel_x, wheel_y;                  /* in render pixels */
    float pinch;                             /* trackpad pinch this frame: product of the scale changes (1 = none) */
    KeyPress keys[LUME_MAX_KEYS]; int nkeys; /* key presses this frame, in order */
    char text[2048]; int ntext;              /* UTF-8 typed this frame */
} Input;

/* Consume a key press matching key + exact modifier set. */
bool input_key(Input *in, SDL_Keycode key, unsigned mods);
/* Consume a key press matching key with any modifiers; reports them. */
bool input_key_any(Input *in, SDL_Keycode key, unsigned *mods_out);

/* ---- app --------------------------------------------------------------- */

/* Which area receives keyboard input (App.focus). */
enum { FOCUS_NONE, FOCUS_EXPLORER, FOCUS_EDITOR, FOCUS_RESULTS };
/* Mouse cursor shapes (App.want_cursor indexes App.cursors). */
enum { CURSOR_ARROW, CURSOR_IBEAM, CURSOR_EW, CURSOR_NS, CURSOR_HAND, CURSOR_COUNT };

/* Opaque module types; each is defined in its own .c / .h. */
typedef struct Font Font;
typedef struct Explorer Explorer;
typedef struct Editor Editor;
typedef struct Results Results;
typedef struct Output Output;
typedef struct Menu Menu;
typedef struct Completer Completer;
typedef struct ConnStore ConnStore;
typedef struct Connection Connection;
typedef struct Dialog Dialog;
typedef struct Db Db;
typedef struct Panels Panels;
typedef struct Toolbar Toolbar;

/* The application: one instance, owned by main(), passed to nearly every function. */
typedef struct App {
    SDL_Window *window;         /* main window */
    SDL_Renderer *renderer;
    int width, height;          /* render output size, pixels */
    float density;              /* render pixels per window unit */
    float scale;                /* render pixels per UI point */
    float zoom;                 /* editor font zoom */
    int fetch_size;             /* rows per batch for row-returning statements */
    int results_budget_mb;      /* memory allowed for all result tabs together (US-030) */
    /* display formats (US-018): see GridFormat in grid.h */
    bool fmt_group; int fmt_decimals, fmt_date, fmt_tz;

    Font *font_ui, *font_small, *font_mono;   /* UI text, small labels, editor / grid monospace; sized for App.scale */
    Input in;                   /* this frame's input for the main window */

    bool running, animating;    /* false ends the loop; true redraws again immediately (something is moving) */
    Uint64 wake_at;             /* SDL ticks (ms) at which to redraw; 0 = none */
    int focus;                  /* FOCUS_* */

    float sidebar_w, results_h; /* points */
    bool sidebar_visible, results_visible;   /* View menu toggles (Cmd+B / Cmd+J) */
    int drag;                   /* 0 none, 1 sidebar divider, 2 results divider */

    Explorer *explorer;
    Editor *editor;
    Results *results;
    Panels *panels;             /* workspace shell: docking, panel bar, editor tabs (panels.h) */
    Output *output;             /* execution output (docs/execution-output.md) */
    bool continue_on_error;     /* script policy: run the remaining statements after a failure */
    /* timeouts (Settings; settings.txt): 0 query = no limit */
    int query_timeout_s;        /* each editor statement; cancelled when it runs longer */
    int connect_timeout_s;      /* reaching the database server (PostgreSQL, MySQL, SQL Server) */
    int ssh_timeout_s;          /* opening an SSH tunnel */
    int explorer_timeout_s;     /* Explorer catalog reads (a connection's own value wins) */
    int explorer_list_min;      /* listing one schema's tables */
    int report_timeout_s;       /* each report query */
    int git_timeout_min;        /* fetch / pull / push and other network git operations */
    int git_clone_min;          /* clone */
    bool catalog_session;       /* completion lists every visible object, read in a second session (objcat.h) */
    Menu *menu;                 /* menu bar */
    Completer *completer;       /* SQL autocompletion (complete.h) */
    ConnStore *conns;           /* saved connections (conn.h) */
    Connection *active;         /* connection the editor runs against (may be NULL) */
    Connection *pending_activate;   /* a background connect that should become active when ready */
    Toolbar *toolbar;           /* main toolbar under the menu bar (toolbar.h) */
    Dialog *dialog;             /* connection dialog (modal) */
    Db *db;                     /* == active->db, kept for convenience */

    char status[256];           /* status bar message (app_status) */
    Uint64 status_until;        /* SDL ticks (ms) when it disappears */

    SDL_Cursor *cursors[CURSOR_COUNT];
    int want_cursor, cur_cursor;   /* cursor requested this frame / currently set (CURSOR_*) */
    Uint32 ev_open_path;        /* user event: data1 = malloc'd path to open */
    Uint32 ev_save_path;        /* user event: data1 = malloc'd path to export to */
    int export_scope;           /* scope of the pending export dialog: 0 selection, 1 loaded rows */
    int mod_override;           /* -1 = use the real keyboard; else SDL_Keymod forced by the harness */
    char assets_dir[1024];      /* folder holding fonts/ (find_assets in main.c) */
    const char *shot_path;      /* when set, frame() saves a BMP before presenting */
    double last_frame_ms;       /* duration of the last frame, present included (harness "stat") */
    double last_present_ms;     /* of which SDL_RenderPresent: mostly waiting for vsync, not work */
    bool reduce_motion;         /* PNL-039: no smooth scrolling / animation (system setting or Window menu) */
    int crash_reports;            /* Settings > Privacy: crash reports 0 ask each time, 1 always send, 2 never (crashrep.h) */
    bool telemetry;               /* Settings > Privacy: anonymous usage statistics (off unless the user turns it on) */
    bool update_check;            /* Settings > Privacy: check GitHub for a newer release at launch and daily (on by default; update.h) */
    char report_locale[8];        /* new reports: "pt-BR" / "en-US" ("" = Lume\'s default) */
} App;

/* UI points -> render pixels. */
static inline float px(const App *a, float pt) { return pt * a->scale; }

/* printf-style status bar message, shown for 6 s, written to the log and kept as a note in the
 * Output of the active session, so it stays as a record after the bar changes. The level is
 * read from the text (failed / error: error; not run / refused / cancelled: warning; else info). */
void app_status(App *a, const char *fmt, ...);
/* Same with a stated level (OL_* in outlog.h), noted on c's session (NULL = the active one):
 * messages about a given connection (connect, failure, disconnect) land in its own Output. */
void app_note(App *a, Connection *c, int level, const char *fmt, ...);
/* Status bar and log only: for text the caller already records in the Output itself. */
void app_status_bar(App *a, const char *fmt, ...);
/* Request a redraw within ms milliseconds even without input (blinking caret, timers). */
void app_wake_in(App *a, Uint32 ms);
/* Runs SQL synchronously on the active connection into the editor's result tab. */
void app_run_sql(App *a, const char *sql, int len, const char *title);
/* How a result lands in the tabs: editor runs reuse the current unpinned editor tab; previews
 * reuse the tab of the same table; RUN_NEW_TAB always opens one; RUN_REPLACE re-executes the
 * current tab in place (server-side sort / filter). `qtable` is the quoted table identifier
 * for previews (server-side operations need it), else NULL. */
enum { RUN_EDITOR, RUN_PREVIEW, RUN_NEW_TAB, RUN_REPLACE };
bool app_run_sql_mode(App *a, Db *db, const char *sql, int len, const char *title, int mode, const char *qtable);   /* db NULL = active; true when the run started (asynchronous); false = refused, explained in the status bar */
/* editor runs go through the executor thread: statements split, Output events, cancel (⌘.) */
void app_run_async(App *a, const char *sql, int len, int mode, int sql_base);
void app_editor_insert(App *a, const char *text);   /* inserts at the editor caret and focuses the editor */
/* connections */
bool app_connect(App *a, Connection *c);          /* opens, makes active; status on failure */
void app_disconnect(App *a, Connection *c);       /* closes c; clears the active connection if it was c */
void app_set_active(App *a, Connection *c);
/* <prefs>/settings.txt: the app-level settings (fetch size, stop on error, results memory, editor zoom,
 * number / date formats, new reports' locale). Saved whenever one changes (menus or Settings). */
void app_prefs_save(App *a);
void app_set_zoom(App *a, float z);       /* makes a connected c the active one (NULL = none) */
void app_edit_connection(App *a, Connection *c);  /* opens the dialog; NULL = new connection */
void app_new_connection_in(App *a, const char *folder);   /* the dialog for a new connection in that Explorer folder */
void app_connections_changed(App *a);             /* saves the store and rebuilds the explorer */
void app_open_path(App *a, const char *path);      /* File > Open / drop: query tab, report or database by the kind of file */
void app_browse_file(App *a);                     /* native open-file dialog; result goes to the dialog or opens a file */
void app_refresh_schema(App *a);                  /* re-points completion at the active schema cache */
void app_export_results(App *a, int scope);       /* native save dialog; scope 0 = selection, 1 = loaded rows */
SDL_Keymod app_modstate(App *a);
/* What a run takes from the active editor (toolbar spec TBR-017 / 018 / 019). */
enum { RUNSRC_AUTO, RUNSRC_SELECTION, RUNSRC_STATEMENT, RUNSRC_FILE };
char *app_editor_sql(App *a, int src, bool allow_partial, int *len, int *base, char *why, size_t n);
struct OutRunOpts;
int app_run_sql_opts(App *a, const char *sql, int len, int base, const struct OutRunOpts *op);
/* Background connect (TBR-011 / 012 / 045): start, finish every frame, reconnect. */
bool app_connect_async(App *a, Connection *c, bool reconnect);
void app_poll_connections(App *a);
bool app_reconnect(App *a, Connection *c);
/* Called by the Output when an execution ends (any session): status is an ES_* value. */
void app_execution_finished(App *a, Db *db, int exec_id, int status);                  /* modifier keys (the test harness can override) */
void app_input_event(App *a, const SDL_Event *e);   /* mouse / wheel / keys / text into a->in */
void app_input_begin(Input *in);                    /* per-frame reset of an Input */
