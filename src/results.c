/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Results panel
 * =============
 *
 * Everything shown under the editor after running SQL: the result tabs, the grid (header, row
 * numbers, cells, scrollbars), the toolbar with its filter / sort / format chips, the record
 * view, and every overlay that acts on a result: value viewer, filter popover, columns panel,
 * export dialog, review-and-apply dialog, chart, context menus, tab close confirmation and the
 * inline cell editor.
 *
 * Data model
 *   Results     one per app. Owns up to RES_MAX_TABS ResultView tabs, the session history
 *               (HistEntry), the state of every overlay, and the per-frame geometry of the grid
 *               currently on screen (grid_rect, cells_rc, colx, row_h ...).
 *   ResultView  one tab. Owns its ResultSet (db.h), the Grid built over it (grid.h: local sort,
 *               filter, selection, column order, search), its pending edits (EditSet, edit.h),
 *               the column widths and cached row tops, plus provenance (SQL, connection, time).
 *               Derived tabs (transpose, compare) hold a locally computed ResultSet, no Db.
 *
 * How it fits
 *   main.c runs SQL (app_run_sql_mode) and lands each result here through results_set; it also
 *   routes menu shortcuts (copy, find, export, history, close tab ...). panels.c, the docking
 *   shell, calls results_pre_update / results_update / results_draw_overlay once per frame and
 *   draws the Inspector with results_draw_inspector. output.c links tabs to Output executions;
 *   script.c (test harness) asks for cell, header and menu positions.
 *   This module calls grid.c for the data view, edit.c for the editing model, conn / db drivers
 *   for metadata, foreign keys and server-side re-runs, extract.c for custom extractors,
 *   json.c / geo.c / image.c for the value viewer, output.c for the Output tab, and ui.c /
 *   draw.c / font.c for widgets and drawing.
 *
 * Frame protocol
 *   1. results_pre_update: overlays (menus, popovers, dialogs) take the frame's input first,
 *      before any panel, because they float over other panels.
 *   2. results_update: draws the panel and handles grid input.
 *   3. results_draw_overlay: draws the overlays on top, with mouse presses masked.
 *   Several dialogs use one function for both passes through an `input` flag: true handles
 *   input (pre-update pass), false only draws (overlay pass), so layout never diverges.
 *
 * Units: column widths (ResultView.colw) are UI points; colx, rowy, scroll offsets and every
 * Rect are render pixels (px() converts points to pixels). Harness positions returned by
 * results_cell_pos and friends are window units (pixels / density).
 *
 * Ownership and threading: everything runs on the UI thread. A ResultView owns its ResultSet,
 * its SQL copy, widths and row tops, all released by view_free. app_run_sql_mode is
 * synchronous and lands its result through results_set before returning, which may replace,
 * evict or reorder tabs: ResultView pointers must be re-read with cur() after any run.
 * g_app caches the App pointer and is refreshed every frame by results_pre_update.
 *
 * Layout persistence (US-005): hidden / ordered / frozen columns, wrap and widths are saved per
 * connection and column signature in grid-layout.txt in the app-data folder, one
 * "key<TAB>value" line per layout, the most recent 200 kept.
 */
#include "units.h"
#include "results.h"
#include "conn.h"
#include "draw.h"
#include "editor.h"
#include "font.h"
#include "grid.h"
#include "geo.h"
#include "extract.h"
#include "edit.h"
#include "image.h"
#include "output.h"
#include "json.h"
#include "theme.h"
#include "ui.h"
#include <ctype.h>
#include <time.h>

/* Result tabs kept at once; the oldest unpinned tab closes to make room. */
#define RES_MAX_TABS 12
#define HIST_MAX 50   /* session history entries (History menu) */
#define TAB_H 28.0f   /* tab strip height, points */

/* Context menu, tab menu, toolbar and History item ids, as returned by popup_update and
 * dispatched by menu_run. The *_BASE ids are ranges: CM_FK_BASE + fk index, CM_COMPARE_BASE +
 * tab index, CM_TAB_GOTO_BASE + tab index, CM_HIST_BASE + history index, CM_HIST_SAVED_BASE +
 * earlier-session entry. */
enum { CM_COPY = 1, CM_COPY_HDR, CM_COPY_CSV, CM_COPY_JSON, CM_COPY_MD, CM_COPY_SQL, CM_COPY_COLNAME, CM_VIEW, CM_RECORD,
       CM_SORT_ASC, CM_SORT_DESC, CM_SORT_CLEAR, CM_FILTER_EQ, CM_FILTER_COL, CM_FILTER_CLEAR,
       CM_HIDE_COL, CM_SHOW_ALL, CM_FREEZE, CM_UNFREEZE, CM_WRAP, CM_COLUMNS, CM_RESET_LAYOUT,
       CM_SRV_ASC, CM_SRV_DESC, CM_SRV_CLEAR, CM_SRV_FILTER,
       CM_EXPORT_VISIBLE, CM_EXPORT_SEL, CM_EXPORT_FULL,
       CM_TAB_CLOSE, CM_TAB_CLOSE_OTHERS, CM_TAB_CLOSE_ALL, CM_TAB_PIN, CM_TAB_RENAME, CM_TAB_OPEN_SQL, CM_TAB_RERUN, CM_TAB_OUTPUT,
       CM_EDIT, CM_SET_NULL, CM_SET_DEFAULT, CM_ADD_ROW, CM_DUP_ROW, CM_DEL_ROW, CM_REVERT_CELL, CM_REVIEW, CM_DISCARD,
       CM_TRANSPOSE, CM_CHART, CM_OPEN_LINK, CM_COPY_AS, CM_MORE_EDITS, CM_COMPARE_PICK, CM_EXPORT_DLG, CM_ANALYSE,
       CM_SORT_FILTER, CM_VIEW_MENU, CM_COPY_DISPLAYED, CM_TB_FIND, CM_TB_FILTER, CM_TB_SORT, CM_TB_COLUMNS, CM_TB_EXPORT,
       CM_FK_BASE = 80, CM_COMPARE_BASE = 100, CM_TAB_GOTO_BASE = 160,
       /* own ranges: 50 session entries once ran from 120 into the tab-goto ids at 160 */
       CM_HIST_BASE = 400, CM_HIST_SAVED_BASE = 500 };

/* One result tab: the data, its grid view, its edits, its layout and where it came from. */
typedef struct ResultView {
    ResultSet rs;   /* owned rows and columns (db.h) */
    Grid g;   /* view over rs: order, filters, selection (grid.h) */
    bool has, discarded, stale, pinned;   /* has a result / freed by the budget / connection closed / never reused */
    int discarded_rows; double discarded_mb;   /* what a discarded tab held, for its placeholder */
    char title[96];   /* tab label (table name, SQL snippet or user rename) */
    char note[200];              /* why stale / discarded */
    char caveat[200];            /* derived tabs: how far the result can be trusted (shown in the toolbar) */
    /* provenance (US-031) */
    int exec_id; time_t when;   /* session-unique id (#n) and run time */
    const void *owner;           /* the query tab's editor that ran it (compared, never dereferenced); NULL: not from a tab (shown everywhere) */
    char *sql; int sql_len;   /* owned copy of the SQL that produced the tab, NULL for derived tabs */
    char conn[64];   /* connection name at run time */
    Db *db;   /* connection to re-run / edit through; NULL once closed or for derived tabs */
    int mode;                    /* RUN_* it was created with */
    char qtable[256];            /* quoted identifier of a previewed table, else "" */
    char table[160];             /* its unquoted name (pk / fk lookups) */
    /* editing (US-032 .. US-035) */
    bool edit_ok; char edit_why[120];   /* editable, else the reason shown in the menu */
    EditSet ed; bool ed_init;    /* pending changes, statements, apply (edit.h) */
    bool fks_loaded; DbFk *fks; int nfks;   /* foreign keys of `table`, loaded lazily (load_fks) */
    /* server-side state (US-012): sort column name + direction, WHERE clause + its label */
    char srv_sort[128]; bool srv_desc;
    char srv_where[640], srv_where_text[240];
    /* layout (US-005) */
    float *colw;                 /* per source column, points */
    bool wrap;   /* multi-line rows (up to 8 lines) */
    float *rowy; int rowy_n; unsigned rowy_gen; bool rowy_dirty;   /* row tops, pixels */
    GridFormat fmt_seen;         /* formats the widths were measured with */
    char layout_key[96];   /* key of this layout in grid-layout.txt (make_layout_key) */
    int resize_col; float resize_x0, resize_w0;   /* header edge being dragged (-1 none), press x, start width (points) */
    int drag_col; float drag_x0; bool drag_moving;   /* header being dragged to reorder (-1 none) */
    float scroll_x, scroll_y, scroll_ty;   /* pixels; scroll_y eases toward the target scroll_ty */
    ScrollBar sb, hsb;                     /* overlay scrollbars (vertical, horizontal) */
    bool dragging, fetching;   /* cell-range drag in progress / fetch in progress (no re-entry) */
    /* search bar */
    bool search_open, search_focus;   /* find bar shown / has the keyboard */
    TextField search;
    char last_search[128];   /* query last sent to grid_search, to detect edits */
    /* record view (US-022) */
    bool rv_open; float rv_scroll; ScrollBar rv_sb;   /* embedded record view shown; its scroll, pixels */
    int out_exec;                /* Output execution that produced this tab (OUT-009), 0 none */
    bool render_noted;           /* first-draw time already reported to the Output (OUT-026) */
} ResultView;

/* One execution of this session, for the History menu (Cmd+Y); sql is an owned copy or NULL. */
typedef struct HistEntry { int id; time_t when; char conn[64]; char *sql; bool ok; int rows; } HistEntry;

/* All result tabs plus the state of every overlay; one per app (results_create). */
struct Results {
    const void *view_owner;      /* the active query tab's editor: whose results are shown */
    const void *next_owner;      /* owner of the next results_set (results_set_owner) */
    ResultView *tabs[RES_MAX_TABS]; int ntabs, cur;   /* owned tabs, in strip order; cur = selected index */
    int next_id;   /* next exec_id to hand out */
    struct FetchJob *fj;   /* rows being fetched on a worker (fetch_start), or NULL */
    int replace_target;   /* results_target_replace: the tab (exec_id) the next RUN_REPLACE lands in, 0 = the current one */
    HistEntry hist[HIST_MAX]; int nhist;   /* oldest first */
    OutHistItem saved[12]; int nsaved; char saved_lbl[2][160];   /* earlier sessions in the History menu (OUT-025) */
    char menu_text[POPUP_MAX][160];          /* labels of the top-level context menu */
    char sub_text[POPUP_MAX][160];           /* labels of its submenus (both visible at once) */
    char tb_text[3][160];                    /* toolbar labels, rewritten every frame */
    /* per-frame geometry of the current grid */
    Rect grid_rect, cells_rc;   /* whole grid area / cell area below the header, right of the row numbers */
    float row_h, head_h, num_w, frozen_w, line_h;   /* pixels: row, header, row-number column, frozen part, wrapped line */
    float *colx; int colx_cap;   /* cumulative widths per display column, pixels */
    bool rerun_pending; int rerun_tab, last_cur;   /* server re-run queued for the next pre-update; last_cur detects tab switches */
    /* filter popover */
    bool fp_open; int fp_col, fp_op, fp_focus, fp_edit; bool fp_server;   /* display column, OP_*, focused field, chip being edited (-1 new), server scope */
    TextField fp_v1, fp_v2;
    /* value viewer */
    bool vv_open; int vv_row, vv_col, vv_mode; float vv_scroll;   /* cell shown (display coords); mode = segmented choice of the kind */
    Json vv_json; bool vv_json_ok, vv_json_tried; bool *vv_collapsed;   /* parsed JSON and one fold flag per node */
    int vv_kind;                 /* 0 text, 1 json, 2 binary, 3 geometry */
    unsigned char *vv_bytes; long vv_nbytes; SDL_Texture *vv_tex; int vv_tex_w, vv_tex_h; char vv_img_note[120];
    Geo vv_geo;   /* parsed geometry when vv_kind is 3 */
    /* columns panel */
    bool cp_open; float cp_scroll; ScrollBar cp_sb, review_sb, vv_sb; Rect vv_inner; float vv_total;   /* columns panel shown; its list scroll, pixels */
    /* export dialog */
    bool ex_open; int ex_fmt, ex_scope, ex_sep, ex_quote, ex_nl, ex_null, ex_values, ex_focus; bool ex_headers, ex_guard;   /* FMT_*, scope 0 selection / 1 loaded / 2 full, then segmented indices */
    TextField ex_rows, ex_mb, ex_secs, ex_table;   /* full-export limits and the SQL INSERT table name */
    /* custom extractors (US-027): name + command from extractors.txt */
    Extractor extractors[EXTRACT_MAX]; int nextractors; int ex_extractor;
    /* tab rename */
    bool rn_open; int rn_tab; TextField rn;   /* inline rename of a tab */
    /* inline cell editor */
    bool ce_open, ce_fresh; int ce_row, ce_col; TextField ce;   /* fresh = the first typed character replaces the value */
    /* review pending changes (US-034) */
    bool rv_review; float review_scroll; ApplyResult apply_res; bool apply_shown;   /* last failed apply, shown in the dialog */
    /* chart (US-039) */
    bool chart_open; int chart_type;   /* 0 bars, 1 lines, 2 pie */
    /* context menu */
    PopupMenu menu, submenu, submenu2; int menu_row, menu_col, menu_tab;   /* menu + cascading levels */
    /* docking (LUME-PANEL-001): Output and the record view live in their own panels; Results
     * only asks the shell to show them (results_take_requests) */
    bool panels_external; bool want_output, want_results, inspector_toggle;
    bool inspector_shown;        /* set by the shell every frame, for the Show / Hide Record View label */
    bool exporting;   /* an export or extractor is running (no re-entry) */
    /* close confirmation for a tab with unsaved changes (US-056) */
    bool cf_open; int cf_tab;
    /* toolbar tooltips (US-055) */
    int tt_hover; Uint64 tt_since; bool tt_show; Rect tt_rect; const char *tt_text;
    bool output_active;          /* the persistent Output tab is selected (OUT-001) */
};

/* The selected tab, or NULL when there are none. */
/* results belong to the query tab that ran them (Isa, 01/11/2026): only that tab's (and tab-less ones) are shown */
static bool visible_v(const Results *r, const ResultView *v) { return !v->owner || v->owner == r->view_owner; }
static ResultView *cur(Results *r) { return r->ntabs && r->cur >= 0 && r->cur < r->ntabs && visible_v(r, r->tabs[r->cur]) ? r->tabs[r->cur] : NULL; }
/* True when the tab still holds its rows (not empty, not discarded by the memory budget). */
static bool live(ResultView *v) { return v && v->has && !v->discarded; }
/* Forward declaration: pending edited cells of a tab (defined with the close request). */
static int tab_unsaved(ResultView *v);
static App *g_app;   /* the viewer needs the renderer for image previews; the editor the status line */

/* "800,000": counts in the chrome always carry thousands separators */
static void fmt_int(char *buf, size_t n, long v) {
    char raw[32];
    snprintf(raw, sizeof raw, "%ld", v < 0 ? -v : v);
    size_t len = strlen(raw), p = 0;
    if (v < 0 && p < n - 1) buf[p++] = '-';
    for (size_t i = 0; i < len && p < n - 1; i++) {
        if (i && (len - i) % 3 == 0) buf[p++] = ',';
        buf[p++] = raw[i];
    }
    buf[p] = 0;
}

/* True when any Format menu option (grouping, decimals, date or time zone) is on. */
static bool formats_active(App *a) { return a->fmt_group || a->fmt_decimals >= 0 || a->fmt_date || a->fmt_tz; }

/* Writes a short human description of the active Format options into buf ("1,000.00, DD/MM/YYYY"),
 * or "" when none is active. Used by the display chip and the export dialog. */
static void describe_formats(App *a, char *buf, size_t n) {
    size_t p = 0;
    buf[0] = 0;
    if (a->fmt_group || a->fmt_decimals >= 0) {
        char ex[32];
        snprintf(ex, sizeof ex, "%s", a->fmt_group ? "1,000" : "1000");
        if (a->fmt_decimals >= 0) { size_t l = strlen(ex); snprintf(ex + l, sizeof ex - l, ".%.*s", a->fmt_decimals, "000000"); }
        p += snprintf(buf + p, n - p, "%s", ex);
    }
    if (a->fmt_date) p += snprintf(buf + p, n - p, "%s%s", p ? "  \xC2\xB7  " : "", a->fmt_date == DATE_DMY ? "DD/MM/YYYY" : "MM/DD/YYYY");
    if (a->fmt_tz) p += snprintf(buf + p, n - p, "%s%s", p ? "  \xC2\xB7  " : "", a->fmt_tz == TZ_UTC ? "UTC" : "local time");
}

/* ---- layout persistence (US-005) --------------------------------------- */

/* One FNV-1a step over a C string, continuing from hash h. */
static uint32_t fnv(const char *s, uint32_t h) { for (; *s; s++) { h ^= (unsigned char)*s; h *= 16777619u; } return h; }

/* Builds v->layout_key from the connection name, column count and a hash of every column name
 * and type, so a saved layout only applies to results of the same shape. */
static void make_layout_key(ResultView *v) {
    /* connection + column signature: layouts never leak between differently shaped results */
    uint32_t h = 2166136261u;
    for (int c = 0; c < v->rs.ncols; c++) { h = fnv(v->rs.cols[c], h); h = fnv("|", h); h = fnv(v->rs.types[c], h); h = fnv(";", h); }
    snprintf(v->layout_key, sizeof v->layout_key, "%s|%d|%08x", v->conn[0] ? v->conn : "-", v->rs.ncols, h);
}

/* Path of grid-layout.txt in the app-data folder (connections directory). */
static void layout_path(App *a, char *buf, size_t n) {
    const char *dir = a->conns ? a->conns->dir : "";
    snprintf(buf, n, "%sgrid-layout.txt", dir);
}

/* Reads a whole file into a NUL-terminated heap buffer; caller frees. NULL when it cannot be
 * opened; an empty file gives "". */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *s = NULL; size_t len = 0, cap = 0;
    char chunk[4096];
    size_t k;
    while ((k = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (len + k + 1 > cap) { cap = (len + k + 1) * 2; s = realloc(s, cap); }
        memcpy(s + len, chunk, k); len += k;
    }
    fclose(f);
    if (!s) s = calloc(1, 1); else s[len] = 0;
    return s;
}

/* Finds the "key<TAB>value" line for `key` in the layout file content and copies its value
 * (truncated to n - 1 bytes). False when the key is absent. */
static bool layout_find(const char *content, const char *key, char *out, size_t n) {
    size_t kl = strlen(key);
    for (const char *p = content; p && *p;) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (ll > kl && strncmp(p, key, kl) == 0 && p[kl] == '\t') {
            size_t vl = ll - kl - 1;
            if (vl >= n) vl = n - 1;
            memcpy(out, p + kl + 1, vl); out[vl] = 0;
            return true;
        }
        p = nl ? nl + 1 : NULL;
    }
    return false;
}

/* rewrites the file with `key` set to `value` (NULL removes it); keeps the last 200 entries */
static void layout_write(App *a, const char *key, const char *value) {
    char path[1200];
    layout_path(a, path, sizeof path);
    char *old = read_file(path);
    FILE *f = fopen(path, "wb");
    if (!f) { free(old); return; }
    size_t kl = strlen(key);
    /* count lines to drop the oldest when over the cap */
    int total = 0;
    for (const char *p = old; p && *p; ) { total++; const char *nl = strchr(p, '\n'); p = nl ? nl + 1 : NULL; }
    int skip = total > 200 ? total - 200 : 0;
    for (const char *p = old; p && *p; ) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        bool same = ll > kl && strncmp(p, key, kl) == 0 && p[kl] == '\t';
        if (!same && skip > 0) skip--;
        else if (!same && ll > 0) { fwrite(p, 1, ll, f); fputc('\n', f); }
        p = nl ? nl + 1 : NULL;
    }
    if (value) fprintf(f, "%s\t%s\n", key, value);
    fclose(f);
    free(old);
}

/* Persists the tab layout: grid state (order, hidden, frozen, sort) from grid_layout_save, then
 * ";wrap=0|1;widths=w1,w2,..." in points. No-op for empty or discarded tabs. */
static void layout_save(App *a, ResultView *v) {
    if (!live(v) || !v->layout_key[0]) return;
    char buf[2048];
    grid_layout_save(&v->g, buf, sizeof buf);
    size_t p = strlen(buf);
    p += snprintf(buf + p, sizeof buf - p, ";wrap=%d;widths=", v->wrap ? 1 : 0);
    for (int c = 0; c < v->rs.ncols && p < sizeof buf - 12; c++) p += snprintf(buf + p, sizeof buf - p, "%s%d", c ? "," : "", (int)v->colw[c]);
    layout_write(a, v->layout_key, buf);
}

/* Removes the saved layout of this tab shape ("Reset layout"). */
static void layout_forget(App *a, ResultView *v) { if (v->layout_key[0]) layout_write(a, v->layout_key, NULL); }

/* Applies a saved layout to a freshly landed tab, if one exists for its key. Widths outside
 * 40..1200 points are ignored. Returns false when nothing was applied. */
static bool layout_restore(App *a, ResultView *v) {
    char path[1200], val[2048];
    layout_path(a, path, sizeof path);
    char *content = read_file(path);
    if (!content) return false;
    bool found = layout_find(content, v->layout_key, val, sizeof val);
    free(content);
    if (!found) return false;
    if (!grid_layout_load(&v->g, val)) return false;
    const char *w = strstr(val, ";wrap=");
    v->wrap = w && w[6] == '1';
    const char *ws = strstr(val, ";widths=");
    if (ws) {
        const char *p = ws + 8;
        for (int c = 0; c < v->rs.ncols && *p; c++) {
            char *end;
            long x = strtol(p, &end, 10);
            if (end == p) break;
            if (x >= 40 && x <= 1200) v->colw[c] = (float)x;
            p = end;
            if (*p == ',') p++;
        }
    }
    return true;
}

/* ---- views ------------------------------------------------------------- */

/* Drops every pending edit of the tab; when inserted rows go away the grid is rebuilt. */
static void edits_clear(ResultView *v) {
    if (!v->ed_init) return;
    bool had_rows = v->rs.nrows > v->ed.new_from;
    edit_clear(&v->ed);
    if (v->has && !v->discarded && had_rows) { grid_refresh(&v->g); v->rowy_dirty = true; }
}

/* Frees a tab and everything it owns (edits, foreign keys, grid, rows, widths, SQL). */
static void view_free(ResultView *v) {
    if (v->ed_init) { edit_free(&v->ed); v->ed_init = false; }
    if (v->fks) db_fks_free(v->fks, v->nfks);
    grid_free(&v->g);
    rs_free(&v->rs);
    free(v->colw); free(v->rowy); free(v->sql);
    free(v);
}

/* Frees the rows of a tab but keeps the tab as a placeholder (memory budget): remembers the
 * row count and size for the message, and `why` as its note. Pending edits are dropped too. */
static void discard_view(ResultView *v, const char *why) {
    v->discarded_rows = v->rs.nrows; v->discarded_mb = rs_bytes(&v->rs) / 1048576.0;
    if (v->ed_init) { edit_free(&v->ed); v->ed_init = false; }
    v->edit_ok = false;
    grid_free(&v->g);
    rs_free(&v->rs);
    v->discarded = true;
    snprintf(v->note, sizeof v->note, "%s", why);
}

/* Natural width of source column c in points: header text or the widest of the first 200
 * formatted values, plus padding, clamped to 70..360. */
static float measure_column(ResultView *v, App *a, int c) {
    float w = font_width(a->font_ui, v->rs.cols[c], -1);
    int sample = v->rs.nrows < 200 ? v->rs.nrows : 200;
    char tmp[64];
    for (int i = 0; i < sample; i++) {
        const char *cell = grid_format_value(&v->g, c, rs_cell(&v->rs, i, c), tmp, sizeof tmp);
        if (!cell) continue;
        float cw = font_width(a->font_mono, cell, -1);
        if (cw > w) w = cw;
    }
    return clampf(w / a->scale + 24, 70, 360);
}

/* Measures every column of a new tab and remembers the formats they were measured with. */
static void measure_columns(ResultView *v, App *a) {
    for (int c = 0; c < v->rs.ncols; c++) v->colw[c] = measure_column(v, a, c);
    v->fmt_seen = v->g.fmt;
}

/* a Format change can make numbers and dates wider: re-measure those columns so a formatted
 * value is never shown with fewer digits than the raw one */
static void remeasure_for_formats(ResultView *v, App *a) {
    if (memcmp(&v->fmt_seen, &v->g.fmt, sizeof v->g.fmt) == 0) return;
    for (int c = 0; c < v->rs.ncols; c++) {
        int kind = v->rs.kinds[c];
        if (!(v->g.numeric[c] || kind == CK_DATE || kind == CK_TEXT || kind == CK_OTHER)) continue;
        float w = measure_column(v, a, c);
        if (w > v->colw[c]) v->colw[c] = w;
    }
    v->fmt_seen = v->g.fmt;
    v->rowy_dirty = true;
}

/* Double-click on a header edge: fits display column col to its header and up to 5,000 visible
 * values (40..1200 points). */
static void autofit_column(ResultView *v, App *a, int col) {
    int src = grid_col(&v->g, col);
    float w = font_width(a->font_ui, v->rs.cols[src], -1);
    int n = v->g.nvis < 5000 ? v->g.nvis : 5000;
    for (int i = 0; i < n; i++) {
        const char *cell = grid_cell(&v->g, i, col);
        if (!cell) continue;
        float cw = font_width(a->font_mono, cell, -1);
        if (cw > w) w = cw;
    }
    v->colw[src] = clampf(w / a->scale + 24, 40, 1200);
    v->rowy_dirty = true;
}

/* One-line summary of SQL for labels: whitespace collapsed, "--" comments skipped, cut at max
 * bytes with an ellipsis (never inside a UTF-8 sequence). "Results" when nothing is left. */
static void sql_snippet(const char *sql, int len, char *out, size_t n, size_t max) {
    /* first statement words, whitespace collapsed, comments skipped */
    size_t p = 0;
    int i = 0;
    bool space = false;
    while (i < len && p < n - 4) {
        if (sql[i] == '-' && i + 1 < len && sql[i + 1] == '-') { while (i < len && sql[i] != '\n') i++; continue; }
        if (isspace((unsigned char)sql[i])) { space = p > 0; i++; continue; }
        if (space) { out[p++] = ' '; space = false; }
        out[p++] = sql[i++];
        if (p >= max) break;
    }
    out[p] = 0;
    if (i < len && p >= max) { while (p > 0 && (out[p - 1] & 0xC0) == 0x80) p--; snprintf(out + p, n - p, "\xE2\x80\xA6"); }
    if (!out[0]) snprintf(out, n, "Results");
}

/* Short tab title from SQL (28 bytes). */
static void sql_title(const char *sql, int len, char *out, size_t n) { sql_snippet(sql, len, out, n, 28); }

/* ---- editing model (US-032 .. US-035, US-048 .. US-050): see edit.h ------- */

/* The Connection whose Db the tab uses, or NULL when closed / derived. */
static Connection *view_conn(App *a, ResultView *v) {
    if (!a->conns || !v->db) return NULL;
    for (int i = 0; i < a->conns->n; i++) if (a->conns->items[i]->db == v->db) return a->conns->items[i];
    return NULL;
}

/* Explorer metadata of the previewed table (case-insensitive name match), loading its columns
 * on first use. NULL when the tab is not a preview or the table is unknown. */
static DbTable *view_table(App *a, ResultView *v) {
    Connection *c = view_conn(a, v);
    if (!c || !v->table[0]) return NULL;
    for (int i = 0; i < c->ntables; i++) if (strcasecmp(c->tables[i].name, v->table) == 0) {
        DbTable *t = &c->tables[i];
        if (!t->cols_loaded) conn_load_columns(c, t);
        return t;
    }
    return NULL;
}

/* Query results stay read-only; a table preview is editable only with a primary key that is
 * present in the result, so every UPDATE / DELETE targets rows by key, never by position.
 * Generated and identity-always columns are marked read-only (US-050). */
static void edit_capability(ResultView *v, App *a) {
    v->edit_ok = false;
    if (!v->ed_init) { edit_init(&v->ed, &v->rs, v->db ? v->db->drv : NULL, v->qtable, v->g.numeric); v->ed_init = true; }
    if (!live(v) || !v->rs.ok || !v->rs.has_rows) { snprintf(v->edit_why, sizeof v->edit_why, "nothing to edit"); return; }
    if (!v->qtable[0]) { snprintf(v->edit_why, sizeof v->edit_why, "query results are read-only; open a table preview to edit"); return; }
    if (!v->db) { snprintf(v->edit_why, sizeof v->edit_why, "connection closed"); return; }
    DbTable *t = view_table(a, v);
    if (!t) { snprintf(v->edit_why, sizeof v->edit_why, "table metadata not available"); return; }
    if (t->is_view) { snprintf(v->edit_why, sizeof v->edit_why, "views are read-only"); return; }
    int npk = 0;
    for (int c = 0; c < t->ncols; c++) if (t->cols[c].pk) npk++;
    if (!npk) { snprintf(v->edit_why, sizeof v->edit_why, "table has no primary key, rows cannot be identified safely"); return; }
    int *pk = malloc(sizeof *pk * npk), n = 0;
    for (int c = 0; c < t->ncols; c++) {
        int src = -1;
        for (int k = 0; k < v->rs.ncols; k++) if (strcasecmp(v->rs.cols[k], t->cols[c].name) == 0) { src = k; break; }
        if (t->cols[c].pk) {
            if (src < 0) { snprintf(v->edit_why, sizeof v->edit_why, "primary key column %s is not in the result", t->cols[c].name); free(pk); return; }
            pk[n++] = src;
        }
        if (src >= 0 && t->cols[c].generated) edit_set_readonly(&v->ed, src, "generated column (computed by the database)");
        else if (src >= 0 && t->cols[c].identity_always) edit_set_readonly(&v->ed, src, "identity column (generated always)");
    }
    edit_set_key(&v->ed, pk, n);
    free(pk);
    v->edit_ok = true;
    v->edit_why[0] = 0;
}

/* Adds a pending INSERT row (copy_from = source row to duplicate, -1 for an empty row) and
 * refreshes the grid so it shows up at the end. */
static void row_add(ResultView *v, int copy_from) {
    if (edit_row_add(&v->ed, copy_from) < 0) return;
    grid_refresh(&v->g);
    v->rowy_dirty = true;
}

/* Marks / unmarks source row `row` for DELETE; a pending new row is removed outright instead. */
static void row_toggle_delete(ResultView *v, int row) {
    bool was_new = edit_row_new(&v->ed, row);
    edit_row_toggle_delete(&v->ed, row);
    if (was_new) { grid_refresh(&v->g); v->rowy_dirty = true; }
}

/* Runs the statements inside one transaction (US-049). Anything but success keeps the
 * pending changes and shows the outcome in the review dialog; "result unknown" (connection
 * lost while committing) is reported as exactly that. */
static void apply_changes(Results *r, ResultView *v, App *a) {
    ApplyResult ar;
    Uint64 t0 = SDL_GetPerformanceCounter();
    int st = edit_apply(&v->ed, v->db, NULL, &ar);
    double ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
    /* OUT-017: the same Output layer as editor runs records this transaction (before the
     * pending statements are cleared) */
    output_record_apply(a->output, a, v->db, v->title, &v->ed, st, &ar, ms);
    r->apply_res = ar;
    r->apply_shown = st != APPLY_OK;
    if (st != APPLY_OK) { app_status(a, "Not applied (%s): %s", edit_apply_status_name(st), ar.message); return; }
    int n = v->ed.nstmts;
    edits_clear(v);
    r->rv_review = false;
    /* reload so the grid shows what the database has now */
    const DbDriver *drv = v->db->drv;
    char order[400] = "", sql[1800];
    if (v->srv_sort[0]) { char qs[300]; drv->quote_ident(v->srv_sort, qs, sizeof qs); snprintf(order, sizeof order, "%s %s", qs, v->srv_desc ? "DESC" : "ASC"); }
    drv->select_sql(v->qtable, v->srv_where[0] ? v->srv_where : NULL, order[0] ? order : NULL, a->fetch_size > 200 ? a->fetch_size : 200, sql, sizeof sql);
    Db *db = v->db;
    bool reloading = app_run_sql_mode(a, db, sql, -1, v->title, RUN_REPLACE, v->qtable);
    if (reloading) app_status(a, "Applied %d statement%s and committed. Reloading the table\xE2\x80\xA6", n, n == 1 ? "" : "s");
    else app_status(a, "Applied %d statement%s and committed. The session is busy: use Re-run to reload", n, n == 1 ? "" : "s");
}

/* Records a landed tab in the session history (oldest dropped past HIST_MAX); copies its SQL. */
static void history_add(Results *r, ResultView *v) {
    if (r->nhist == HIST_MAX) { free(r->hist[0].sql); memmove(&r->hist[0], &r->hist[1], sizeof r->hist[0] * (HIST_MAX - 1)); r->nhist--; }
    HistEntry *h = &r->hist[r->nhist++];
    h->id = v->exec_id; h->when = v->when; h->ok = v->rs.ok; h->rows = v->rs.has_rows ? v->rs.nrows : v->rs.affected;
    snprintf(h->conn, sizeof h->conn, "%s", v->conn);
    h->sql = v->sql ? strdup(v->sql) : NULL;
}

/* Bytes held by the rows of every live tab (rs_bytes), for the budget and the strip label. */
size_t results_bytes(Results *r) {
    size_t total = 0;
    for (int i = 0; i < r->ntabs; i++) if (live(r->tabs[i])) total += rs_bytes(&r->tabs[i]->rs);
    return total;
}

/* Keeps the results under the memory budget (results_budget_mb, default 256 MB) by discarding
 * the oldest tab that is not current, not pinned and has no unsaved edits; repeats until under
 * budget or nothing can go, and says so in the status bar. */
static void enforce_budget(Results *r, App *a) {
    size_t budget = (size_t)(a->results_budget_mb > 0 ? a->results_budget_mb : 256) << 20;
    for (;;) {
        size_t total = results_bytes(r);
        if (total <= budget) return;
        int pick = -1;
        for (int i = 0; i < r->ntabs; i++) {
            ResultView *v = r->tabs[i];
            if (i == r->cur || v->pinned || !live(v) || tab_unsaved(v) > 0) continue;   /* unsaved edits are never discarded silently */
            if (pick < 0 || v->exec_id < r->tabs[pick]->exec_id) pick = i;
        }
        if (pick < 0) {
            app_status(a, "Results hold %zu MB, above the %d MB budget; pinned and current tabs are kept", total >> 20, a->results_budget_mb);
            return;
        }
        char why[200];
        snprintf(why, sizeof why, "Result discarded to stay within the %d MB results budget", a->results_budget_mb);
        discard_view(r->tabs[pick], why);
        app_status(a, "Discarded \"%s\" (#%d) to stay within the %d MB results budget", r->tabs[pick]->title, r->tabs[pick]->exec_id, a->results_budget_mb);
    }
}

/* Closes tab i without asking: saves its layout, frees it, fixes the current index and closes
 * the overlays that pointed into it. Use tab_close_request for user actions. */
static void tab_remove(Results *r, App *a, int i) {
    if (i < 0 || i >= r->ntabs) return;
    layout_save(a, r->tabs[i]);
    view_free(r->tabs[i]);
    memmove(&r->tabs[i], &r->tabs[i + 1], sizeof *r->tabs * (r->ntabs - i - 1));
    r->ntabs--;
    if (r->cur >= r->ntabs) r->cur = r->ntabs - 1;
    else if (r->cur > i) r->cur--;
    if (r->cur < 0) r->cur = 0;
    r->vv_open = r->fp_open = r->cp_open = r->rn_open = false;
}

/* Allocates the Results with export defaults (CSV, loaded rows, headers, formula guard, 1,000,000
 * rows / 1,024 MB / no time limit for full exports). Freed by results_destroy. */
static Results *g_results;   /* for the connection close hook */
static void fetch_on_close(Connection *c);
Results *results_create(void) {
    Results *r = calloc(1, sizeof *r);
    g_results = r; conn_add_close_hook(fetch_on_close);
    r->next_id = 1;
    r->ex_headers = true; r->ex_guard = true; r->ex_scope = 1; r->ex_fmt = FMT_CSV;
    textfield_set(&r->ex_rows, "1000000");
    textfield_set(&r->ex_mb, "1024");
    textfield_set(&r->ex_secs, "0");
    r->ex_extractor = -1;
    r->tt_hover = -1;
    return r;
}

/* Frees every tab, the history and the value-viewer state, then the Results itself. */
static void fetch_stop(Results *r);
void results_destroy(Results *r) {
    fetch_stop(r);
    conn_remove_close_hook(fetch_on_close); if (g_results == r) g_results = NULL;
    for (int i = 0; i < r->ntabs; i++) view_free(r->tabs[i]);
    for (int i = 0; i < r->nhist; i++) free(r->hist[i].sql);
    if (r->vv_json_ok) json_free(&r->vv_json);
    free(r->vv_collapsed);
    free(r->colx);
    free(r);
}

/* Lands a finished execution in a tab. Takes ownership of *set (moved out with rs_move).
 * The receiving tab depends on mode: RUN_REPLACE reuses the current tab, RUN_EDITOR the most
 * recent unpinned editor tab, RUN_PREVIEW the unpinned tab of the same table, RUN_NEW_TAB
 * always opens one. A reused tab keeps its layout when the column shape matches, and on
 * RUN_REPLACE also its local sort / filter and server sort / WHERE. At RES_MAX_TABS the oldest
 * unpinned tab is closed. Finally restores the saved layout, decides editability, records
 * history and enforces the memory budget (which may discard other tabs). */
void results_set(Results *r, App *a, ResultSet *set, const char *title, const char *sql, int len, int mode, const char *qtable) {
    ResultView *old = cur(r);
    int slot = -1;
    if (mode == RUN_REPLACE && r->replace_target > 0) {   /* an async reload: the tab it started from, even if another one is shown now */
        old = NULL;
        for (int i = 0; i < r->ntabs; i++) if (r->tabs[i]->exec_id == r->replace_target) { old = r->tabs[i]; r->cur = i; }
        if (!old) mode = RUN_NEW_TAB;   /* that tab was closed meanwhile */
    }
    r->replace_target = 0;
    /* which tab receives the result (US-006): editor runs reuse the current unpinned editor
     * tab, previews reuse the same table's tab, RUN_REPLACE always lands in the current tab */
    if (mode == RUN_REPLACE && old) slot = r->cur;
    else if (mode == RUN_EDITOR) {
        /* the most recent unpinned editor tab, wherever it is */
        for (int i = 0; i < r->ntabs; i++)
            if (r->tabs[i]->mode == RUN_EDITOR && !r->tabs[i]->pinned && r->tabs[i]->owner == r->next_owner && (slot < 0 || r->tabs[i]->exec_id > r->tabs[slot]->exec_id)) slot = i;
    }
    else if (mode == RUN_PREVIEW) {
        for (int i = 0; i < r->ntabs; i++)
            if (r->tabs[i]->mode == RUN_PREVIEW && !r->tabs[i]->pinned && strcmp(r->tabs[i]->title, title) == 0) { slot = i; break; }
    }
    /* build the new view: rows, provenance, title, grid, widths, layout key */
    ResultView *v = calloc(1, sizeof *v);
    v->owner = r->next_owner; r->next_owner = NULL;
    if (v->owner) r->view_owner = v->owner;   /* the tab that ran it is the one on screen */
    rs_move(&v->rs, set);
    v->has = true;
    v->exec_id = r->next_id++;
    v->when = time(NULL);
    v->mode = mode == RUN_REPLACE && old ? old->mode : mode;
    v->resize_col = -1; v->drag_col = -1;
    if (sql && len != 0) { if (len < 0) len = (int)strlen(sql); v->sql = malloc(len + 1); memcpy(v->sql, sql, len); v->sql[len] = 0; v->sql_len = len; }
    if (a->active) snprintf(v->conn, sizeof v->conn, "%s", a->active->name);
    v->db = v->rs.db ? v->rs.db : a->db;
    if (qtable) snprintf(v->qtable, sizeof v->qtable, "%s", qtable);
    if (qtable && title) snprintf(v->table, sizeof v->table, "%s", title);
    if (mode == RUN_REPLACE && old && old->table[0]) snprintf(v->table, sizeof v->table, "%s", old->table);
    if (title && strcmp(title, "Results") != 0) snprintf(v->title, sizeof v->title, "%s", title);
    else if (v->sql) sql_title(v->sql, v->sql_len, v->title, sizeof v->title);
    else snprintf(v->title, sizeof v->title, "Results");
    rs_trim(&v->rs);
    v->colw = calloc(v->rs.ncols ? v->rs.ncols : 1, sizeof *v->colw);
    grid_init(&v->g, &v->rs);
    measure_columns(v, a);
    make_layout_key(v);
    /* install it: replace the slot (carrying over what still applies) or append a tab */
    ResultView *prev = slot >= 0 ? r->tabs[slot] : NULL;
    if (prev) {
        /* same tab: keep what the user set up when the shape is the same */
        v->pinned = prev->pinned;
        if (mode == RUN_REPLACE) { snprintf(v->title, sizeof v->title, "%s", prev->title); }
        if (live(prev) && strcmp(prev->layout_key, v->layout_key) == 0) {
            char buf[2048];
            grid_layout_save(&prev->g, buf, sizeof buf);
            grid_layout_load(&v->g, buf);
            memcpy(v->colw, prev->colw, sizeof *v->colw * v->rs.ncols);
            v->wrap = prev->wrap;
            v->rv_open = prev->rv_open;
            v->scroll_x = prev->scroll_x;
            if (mode == RUN_REPLACE) {
                for (int k = 0; k < prev->g.nsort && k < GRID_MAX_SORT; k++) { v->g.sort_col[k] = prev->g.sort_col[k]; v->g.sort_desc[k] = prev->g.sort_desc[k]; }
                v->g.nsort = prev->g.nsort;
                memcpy(v->g.filters, prev->g.filters, sizeof v->g.filters); v->g.nfilters = prev->g.nfilters;
                grid_refresh(&v->g);
            }
        }
        if (mode == RUN_REPLACE) {
            snprintf(v->srv_sort, sizeof v->srv_sort, "%s", prev->srv_sort); v->srv_desc = prev->srv_desc;
            snprintf(v->srv_where, sizeof v->srv_where, "%s", prev->srv_where);
            snprintf(v->srv_where_text, sizeof v->srv_where_text, "%s", prev->srv_where_text);
        }
        view_free(prev);
        r->tabs[slot] = v;
        r->cur = slot;
    } else {
        if (r->ntabs == RES_MAX_TABS) {
            int pick = -1;
            for (int i = 0; i < r->ntabs; i++) if (!r->tabs[i]->pinned && (pick < 0 || r->tabs[i]->exec_id < r->tabs[pick]->exec_id)) pick = i;
            if (pick < 0) pick = r->cur;              /* everything pinned: the current tab gives way */
            app_status(a, "Closed \"%s\" (#%d): %d result tabs is the limit", r->tabs[pick]->title, r->tabs[pick]->exec_id, RES_MAX_TABS);
            tab_remove(r, a, pick);
        }
        r->tabs[r->ntabs] = v;
        r->cur = r->ntabs++;
    }
    /* finish: saved layout, editability, history, memory budget */
    if (live(v) && v->rs.ok && v->rs.has_rows) layout_restore(a, v);
    edit_capability(v, a);
    v->rowy_dirty = true;
    r->vv_open = r->fp_open = r->cp_open = r->rn_open = false;
    popup_close(&r->menu);
    history_add(r, v);
    enforce_budget(r, a);
}

/* Called before a connection closes: releases open cursors on it and marks its tabs stale
 * (no more fetch, re-run or edit), keeping the rows already loaded. */
void results_release_db(Results *r, Db *db) {
    for (int i = 0; i < r->ntabs; i++) {
        ResultView *v = r->tabs[i];
        if (v->rs.db == db && v->rs.cursor) { rs_release(&v->rs); v->rs.cursor_lost = true; }
        if (v->rs.db == db) v->rs.db = NULL;
        if (v->db == db) { v->db = NULL; v->stale = true; snprintf(v->note, sizeof v->note, "connection closed: this result is no longer in sync with the database"); }
    }
}

/* ---- fetching more rows, on a worker (a slow fetch never freezes the window) ----
 * The tab's cursor moves into a scratch ResultSet with the same columns; the worker fetches into
 * it while the grid keeps drawing the rows it has; when it is done the rows are appended to the
 * tab and the cursor moves back. The session is marked busy meanwhile, like during a run. */
typedef struct FetchJob {
    ResultSet tmp; int tab; int n; bool all; volatile int done, cancel; SDL_Thread *th; Db *db;
    /* Fetch all streams in blocks: the worker hands FETCH_BLOCK rows over in `ready` and the UI
     * appends them to the tab, so the memory held twice is one block, never the whole result */
    SDL_Mutex *mu; ResultSet ready; bool has_ready; int delivered;
    Uint64 last_refresh;   /* the grid re-indexes at most once a second while blocks land (its order map is the size of the result) */
} FetchJob;
#define FETCH_BLOCK 5000   /* small: what is held twice is one block, and the UI lands several per frame */
/* columns of src copied into an empty dst (its rows come from take_rows / the driver) */
static void rs_like(ResultSet *dst, const ResultSet *src) {
    rs_init(dst);
    for (int c = 0; c < src->ncols; c++) rs_add_column(dst, src->cols[c], src->types[c] ? src->types[c] : "", src->kinds ? src->kinds[c] : CK_OTHER);
    dst->ok = true; dst->has_rows = true;
}
/* moves the rows (cell table and arena) of from into to, leaving from empty */
static void take_rows(ResultSet *to, ResultSet *from) {
    to->cells = from->cells; to->nrows = from->nrows; to->cap_rows = from->cap_rows;
    to->arena = from->arena; to->arena_len = from->arena_len; to->arena_cap = from->arena_cap;
    from->cells = NULL; from->nrows = from->cap_rows = 0; from->arena = NULL; from->arena_len = from->arena_cap = 0;
}
/* worker: a block is ready; waits (briefly, polling) until the UI took the previous one */
static void fetch_handover(FetchJob *f) {
    SDL_LockMutex(f->mu);
    while (f->has_ready && !__atomic_load_n(&f->cancel, __ATOMIC_SEQ_CST)) { SDL_UnlockMutex(f->mu); SDL_Delay(5); SDL_LockMutex(f->mu); }
    if (!f->has_ready) {
        rs_like(&f->ready, &f->tmp); take_rows(&f->ready, &f->tmp); f->has_ready = true; f->delivered += f->ready.nrows;
        rs_reserve(&f->tmp, f->ready.nrows, f->ready.arena_len);   /* the next block, sized like this one: no doubling churn */
    }
    SDL_UnlockMutex(f->mu);
}
/* UI: appends a block handed over by the worker to its tab; true when one landed */
static bool fetch_take_block(Results *r, App *a, FetchJob *f) {
    ResultSet blk; bool got = false;
    SDL_LockMutex(f->mu);
    if (f->has_ready) { blk = f->ready; memset(&f->ready, 0, sizeof f->ready); f->has_ready = false; got = true; }
    SDL_UnlockMutex(f->mu);
    if (!got) return false;
    ResultView *v = NULL;
    for (int i = 0; i < r->ntabs; i++) if (r->tabs[i]->exec_id == f->tab) v = r->tabs[i];
    if (v) {
        int before = v->rs.nrows, ap = rs_append(&v->rs, &blk);
        if (ap < blk.nrows) app_status(a, "The result holds the most rows a tab can show: the rest is not loaded (export the full result instead)");
        else { char nr[32]; fmt_int(nr, sizeof nr, v->rs.nrows); app_status(a, "Fetching every remaining row\xE2\x80\xA6 %s rows so far", nr); }
        if (v->rs.nrows > before && SDL_GetTicks() - f->last_refresh > 1000) { grid_refresh(&v->g); v->rowy_dirty = true; f->last_refresh = SDL_GetTicks(); }
    }
    rs_free(&blk);
    return true;
}
/* quitting: the worker stops after its current batch; its cursor is closed */
static void fetch_stop(Results *r) {
    FetchJob *f = r->fj; if (!f) return;
    __atomic_store_n(&f->cancel, 1, __ATOMIC_SEQ_CST);
    db_cancel(f->db);   /* interrupts the batch in progress (on its own thread) */
    /* a worker blocked in the driver on a network that died returns only when TCP gives up,
     * minutes later: after a few seconds it is left behind with the session and its rows */
    Uint64 t0 = SDL_GetTicks();
    while (!__atomic_load_n(&f->done, __ATOMIC_SEQ_CST) && SDL_GetTicks() - t0 < 8000) SDL_Delay(50);
    if (!__atomic_load_n(&f->done, __ATOMIC_SEQ_CST)) { __atomic_store_n(&f->db->abandoned, 1, __ATOMIC_SEQ_CST); SDL_DetachThread(f->th); r->fj = NULL; return; }
    SDL_WaitThread(f->th, NULL);
    __atomic_store_n(&f->db->busy, 0, __ATOMIC_SEQ_CST);
    if (f->has_ready) rs_free(&f->ready);
    SDL_DestroyMutex(f->mu);
    rs_free(&f->tmp); free(f); r->fj = NULL;
}
/* a connection about to close: its fetch stops first (the worker must not use a closed session) */
static void fetch_on_close(Connection *c) { if (g_results && g_results->fj && c && g_results->fj->db == c->db) fetch_stop(g_results); }
static int fetch_worker(void *ud) {
    FetchJob *f = ud;
    do {
        if (!rs_fetch_more(&f->tmp, f->n)) break;
        if (f->all && f->tmp.nrows >= FETCH_BLOCK) fetch_handover(f);
    } while (f->all && f->tmp.cursor && !__atomic_load_n(&f->cancel, __ATOMIC_SEQ_CST) && f->delivered + f->tmp.nrows < RS_ROW_CAP - f->n);
    __atomic_store_n(&f->done, 1, __ATOMIC_SEQ_CST);
    return 0;
}
static bool fetch_start(Results *r, App *a, int n, bool all) {
    ResultView *v = cur(r);
    if (r->fj) { app_status(a, "Rows are still being fetched"); return true; }
    if (!live(v) || !v->rs.cursor || v->fetching) return false;   /* no double fetch (US-030) */
    if (v->ed_init && v->rs.nrows > v->ed.new_from) { app_status(a, "Apply or discard the pending inserts before fetching more rows"); return false; }
    Db *db = v->rs.db;
    if (!db || __atomic_load_n(&db->busy, __ATOMIC_SEQ_CST)) { app_status(a, "The connection is busy with a running statement: fetch more when it ends"); return true; }
    FetchJob *f = calloc(1, sizeof *f); if (!f) return false;
    f->mu = SDL_CreateMutex(); if (!f->mu) { free(f); return false; }
    rs_like(&f->tmp, &v->rs);
    f->tmp.db = db; f->tmp.cursor = v->rs.cursor; f->tmp.truncated = true;
    v->rs.cursor = NULL;
    if (db->drv->rehome) db->drv->rehome(db, &f->tmp);   /* the driver's cursor now fills the scratch set */
    f->tab = v->exec_id; f->n = n > 0 ? n : 500; f->all = all; f->db = db;
    __atomic_store_n(&db->busy, 1, __ATOMIC_SEQ_CST);
    f->th = SDL_CreateThread(fetch_worker, "lume-fetch", f);
    if (!f->th) {   /* give the cursor back */
        __atomic_store_n(&db->busy, 0, __ATOMIC_SEQ_CST);
        v->rs.cursor = f->tmp.cursor; f->tmp.cursor = NULL; if (db->drv->rehome) db->drv->rehome(db, &v->rs);
        SDL_DestroyMutex(f->mu); rs_free(&f->tmp); free(f); return false;
    }
    v->fetching = true; r->fj = f;
    app_status(a, all ? "Fetching every remaining row\xE2\x80\xA6" : "Fetching %d more rows\xE2\x80\xA6", f->n);
    app_wake_in(a, 50);
    return true;
}
/* per frame: lands a finished fetch in its tab (or closes its cursor when the tab is gone) */
static void fetch_pump(Results *r, App *a) {
    FetchJob *f = r->fj;
    if (!f) return;
    bool landed = false;   /* blocks of a Fetch all land as they come, several per frame within a small time budget */
    for (Uint64 t0 = SDL_GetTicks(); fetch_take_block(r, a, f); landed = true) if (SDL_GetTicks() - t0 > 8) break;
    if (!__atomic_load_n(&f->done, __ATOMIC_SEQ_CST)) { app_wake_in(a, landed ? 10 : 100); return; }
    SDL_WaitThread(f->th, NULL);
    fetch_take_block(r, a, f);   /* a last block handed over just before the end */
    SDL_DestroyMutex(f->mu); f->mu = NULL;
    r->fj = NULL;
    __atomic_store_n(&f->db->busy, 0, __ATOMIC_SEQ_CST);
    ResultView *v = NULL;
    for (int i = 0; i < r->ntabs; i++) if (r->tabs[i]->exec_id == f->tab) v = r->tabs[i];
    if (!v) { rs_free(&f->tmp); free(f); return; }   /* the tab was closed: its cursor closes now */
    int before = v->rs.nrows;
    int appended = rs_append(&v->rs, &f->tmp);   /* the arena moves over once; no per-cell copy on the UI thread */
    if (appended < f->tmp.nrows && f->tmp.nrows) app_status(a, "The result holds the most rows a tab can show: the rest was not loaded (export the full result instead)");
    v->rs.cursor = f->tmp.cursor; f->tmp.cursor = NULL;
    if (v->rs.cursor && f->db->drv->rehome) f->db->drv->rehome(f->db, &v->rs);
    v->rs.complete = f->tmp.complete; v->rs.truncated = v->rs.cursor != NULL; v->rs.fetch_ms += f->tmp.fetch_ms;
    if (!f->tmp.ok) { v->rs.cursor_lost = !f->tmp.complete; app_status(a, "Fetching more rows failed: %s", f->tmp.error[0] ? f->tmp.error : "the cursor was closed"); }
    else { char nr[32]; fmt_int(nr, sizeof nr, v->rs.nrows); app_status(a, v->rs.cursor ? "%s rows loaded, more available" : "%s rows: everything is loaded", nr); }
    if (v->ed_init) v->ed.new_from = v->rs.nrows;   /* fetched rows are stored rows, not inserts */
    if (v->rs.nrows > before) { grid_refresh(&v->g); v->rowy_dirty = true; }
    if (!v->rs.cursor) rs_trim(&v->rs);
    v->fetching = false;
    rs_free(&f->tmp); free(f);
    enforce_budget(r, a);
}

/* Fetches up to n more rows from the current tab cursor, on a worker. Refused while a fetch runs
 * or while pending inserts exist (they sit after the stored rows). True when a fetch started. */
bool results_fetch_more(Results *r, App *a, int n) { return fetch_start(r, a, n, false); }
bool results_fetch_all(Results *r, App *a) { return fetch_start(r, a, FETCH_BLOCK, true); }
bool results_fetching(Results *r) { return r->fj != NULL; }

/* True while an overlay of Results (or of the Output tab) owns the keyboard. */
bool results_modal_open(Results *r) { return r->vv_open || r->fp_open || r->cp_open || r->ex_open || r->rn_open || r->ce_open || r->rv_review || r->chart_open || r->cf_open || r->menu.open || (r->output_active && g_app && output_modal_open(g_app->output)); }
/* True while a text field of Results (find bar, filter, export, rename, cell editor) or of the
 * Output tab has the keyboard, so global shortcuts leave typing alone. */
bool results_text_focus(Results *r) {
    if (r->output_active && g_app && output_text_focus(g_app->output)) return true;
    ResultView *v = cur(r);
    return (v && v->search_open && v->search_focus) || r->fp_open || r->ex_open || r->rn_open || r->ce_open;
}

/* ---- derived tabs (US-038 transposed, US-040 compare) ------------------ */

/* Forward declarations for helpers defined further down. */
static bool col_visible(Results *r, ResultView *v, int c, float *x, float *w);
static void load_extractors(Results *r, App *a);
static void draw_record_list(Results *r, ResultView *v, App *a, Rect list);   /* record view / Inspector */
static void chain_copy_as(Results *r, App *a);
static void chain_more_edits(Results *r, ResultView *v);
static void chain_compare(Results *r, ResultView *v);
static void chain_analyse(Results *r, ResultView *v);
static float draw_wrapped(Font *f, SDL_Renderer *rr, const char *s, Rect rc, Color c, float scroll, bool draw);
static void chain_sort_filter(Results *r, ResultView *v);
static void chain_view(Results *r, ResultView *v);
static void context_menu_open_at(Results *r, ResultView *v, App *a, int row, int col, bool on_header, float mx, float my);
static void toolbar_action(Results *r, ResultView *v, App *a, int id, Rect from);
static void tab_close_request(Results *r, App *a, int i);

/* Opens a locally computed result (transpose, compare) in a new tab: takes ownership of *rs,
 * detaches it from any connection and makes it read-only. */
static void add_derived(Results *r, App *a, ResultSet *rs, const char *title, const char *conn) {
    results_set(r, a, rs, title, NULL, 0, RUN_NEW_TAB, NULL);
    ResultView *v = cur(r);
    if (!v) return;
    v->db = NULL;                       /* no cursor, no server, never editable */
    snprintf(v->conn, sizeof v->conn, "%s", conn ? conn : "derived");
    snprintf(v->edit_why, sizeof v->edit_why, "derived result (computed locally) is read-only");
    v->edit_ok = false;
}

/* ---- inline cell editor (US-033) ---------------------------------------- */

/* Opens the inline editor on display cell (row, col) with its pending or stored value. Refused
 * for read-only tabs, deleted rows and read-only columns (the reason goes to the status bar). */
static void cell_edit_open(Results *r, ResultView *v, int row, int col) {
    if (!v->edit_ok || row < 0 || col < 0) return;
    int src_row = v->g.order[row], src_col = grid_col(&v->g, col);
    if (edit_row_deleted(&v->ed, src_row)) return;
    const char *ro = edit_col_readonly(&v->ed, src_col);
    if (ro) { if (g_app) app_status(g_app, "%s is read-only: %s", v->rs.cols[src_col], ro); return; }
    int st; const char *val = edit_pending_value(&v->ed, src_row, src_col, &st);
    textfield_set(&r->ce, st == 2 || st == 3 || !val ? "" : val);
    r->ce_open = true; r->ce_fresh = true; r->ce_row = row; r->ce_col = col;
    r->ce.blink = SDL_GetTicks();
}

/* Validates the editor text for its column and stores it as a pending change, closing the
 * editor. Returns false (editor stays open) when validation fails. */
static bool cell_edit_commit(Results *r, ResultView *v, App *a) {
    int src_row = v->g.order[r->ce_row], src_col = grid_col(&v->g, r->ce_col);
    char why[200];
    if (!edit_validate(&v->ed, src_col, r->ce.buf, why, sizeof why)) { app_status(a, "%s", why); return false; }
    edit_put_text(&v->ed, src_row, src_col, r->ce.buf);   /* back to the stored value drops the change */
    r->ce_open = false;
    return true;
}

/* Screen rectangle of display cell (row, col), pixels, from this frame geometry. Needs fresh
 * row tops (rowy) and column offsets (colx). */
static Rect cell_rect(Results *r, ResultView *v, int row, int col) {
    float x, w;
    col_visible(r, v, col, &x, &w);
    float y = r->cells_rc.y + v->rowy[row] - v->scroll_y, h = v->rowy[row + 1] - v->rowy[row];
    return rect_make(x, y, w, h);
}

/* Input for the inline cell editor (pre-update pass): Enter commits, Tab / Shift+Tab commit and
 * move to the next / previous cell, Esc cancels, a click outside commits. Swallows all keys
 * and text while open; closes itself when the cell no longer exists. */
static void cell_edit_update(Results *r, App *a) {
    Input *in = &a->in;
    ResultView *v = cur(r);
    if (!r->ce_open) return;
    if (!live(v) || !v->edit_ok || r->ce_row >= v->g.nvis || r->ce_col >= v->g.ncols || v->rowy_n != v->g.nvis) { r->ce_open = false; return; }
    for (int k = 0; k < in->nkeys; k++) {
        KeyPress *kp = &in->keys[k];
        if (kp->used) continue;
        if (kp->key == SDLK_ESCAPE) { r->ce_open = false; kp->used = true; }
        else if (kp->key == SDLK_RETURN || kp->key == SDLK_KP_ENTER) { if (cell_edit_commit(r, v, a)) { grid_select_cell(&v->g, r->ce_row, r->ce_col, false); } kp->used = true; }
        else if (kp->key == SDLK_TAB) {
            int row = r->ce_row, col = r->ce_col;
            if (cell_edit_commit(r, v, a)) {
                if (kp->mods & KM_SHIFT) { if (--col < 0) { col = v->g.ncols - 1; row--; } }
                else if (++col >= v->g.ncols) { col = 0; row++; }
                if (row >= 0 && row < v->g.nvis) { grid_select_cell(&v->g, row, col, false); cell_edit_open(r, v, row, col); }
            }
            kp->used = true;
        }
    }
    if (!r->ce_open) { in->nkeys = 0; in->ntext = 0; return; }
    /* spreadsheet rule: the first typed character replaces the stored value; any other key
     * (arrows, backspace) keeps it for editing in place */
    if (r->ce_fresh && in->ntext > 0) textfield_set(&r->ce, "");
    if (in->ntext > 0 || in->nkeys > 0) r->ce_fresh = false;
    Rect cr = cell_rect(r, v, r->ce_row, r->ce_col);
    Rect fr = rect_make(cr.x, cr.y, fmaxf(cr.w, px(a, 120)), fmaxf(cr.h, px(a, 26)));
    ui_textfield(a, &r->ce, fr, "", true);
    if (in->mouse_pressed && !in->mouse_consumed && !rect_contains(fr, in->mouse_x, in->mouse_y)) { cell_edit_commit(r, v, a); r->ce_open = false; }
    in->nkeys = 0; in->ntext = 0;
}

/* ---- review dialog (US-034) ---------------------------------------------- */

/* Centred rectangle of the review dialog: up to 760 x 520 points, 20 points from the window edges. */
static Rect review_panel(App *a) {
    float w = fminf(px(a, 760), a->width - px(a, 40)), h = fminf(px(a, 520), a->height - px(a, 40));
    return rect_make((a->width - w) / 2, (a->height - h) / 2, w, h);
}

/* Review-and-apply dialog for pending edits (US-034): a summary line, the strategy used to
 * target rows, the outcome of the last failed apply, then every generated statement soft-
 * wrapped in a scrollable list, with Apply / Discard all / Close. input = true handles input
 * (pre-update pass), false draws (overlay pass). Apply is disabled after an "unknown" outcome
 * until the user re-runs the preview. */
static void review_dialog(Results *r, ResultView *v, App *a, bool input) {
    SDL_Renderer *rr = a->renderer;
    Input *in = &a->in;
    Rect p = review_panel(a);
    EditSet *e = &v->ed;
    float top = px(a, 92) + (r->apply_shown ? px(a, 48) : 0);
    /* 1. backdrop, title, explanation and the last failed outcome */
    if (!input) {
        draw_rect(rr, rect_make(0, 0, (float)a->width, (float)a->height), color_alpha(color_hex(0x000000), 110));
        draw_rrect(rr, p, px(a, 10), color_hex(T_BORDER));
        draw_rrect(rr, rect_inset(p, px(a, 1), px(a, 1)), px(a, 9), color_hex(T_PANEL_ALT));
        int nu = 0, nd = 0, ni = 0;
        for (int i = 0; i < e->nstmts; i++) { if (e->stmts[i].kind == 0) nu++; else if (e->stmts[i].kind == 1) nd++; else ni++; }
        char title[200], strat[400];
        snprintf(title, sizeof title, "Unsaved rows on %s  \xC2\xB7  %d UPDATE  \xC2\xB7  %d DELETE  \xC2\xB7  %d INSERT", v->title, nu, nd, ni);
        draw_text_fit(a->font_ui, rr, title, -1, rect_make(p.x + px(a, 24), p.y + px(a, 14), p.w - px(a, 48), px(a, 26)), color_hex(T_TEXT), ALIGN_LEFT);
        draw_text_fit(a->font_small, rr, "One transaction: a conflict or an error rolls everything back and keeps these changes here. DELETE cannot be undone after commit.", -1, rect_make(p.x + px(a, 24), p.y + px(a, 40), p.w - px(a, 48), px(a, 20)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        edit_describe_strategy(e, strat, sizeof strat);
        draw_text_fit(a->font_small, rr, strat, -1, rect_make(p.x + px(a, 24), p.y + px(a, 60), p.w - px(a, 48), px(a, 20)), color_hex(e->snap_note[0] ? T_YELLOW : T_TEXT_MUTED), ALIGN_LEFT);
        if (r->apply_shown) {
            /* outcome of the last attempt (US-048 / US-049): never a silent failure */
            ApplyResult *ar = &r->apply_res;
            uint32_t col = ar->status == APPLY_UNKNOWN ? T_YELLOW : T_RED;
            Rect box = rect_make(p.x + px(a, 24), p.y + px(a, 84), p.w - px(a, 48), px(a, 44));
            draw_rrect(rr, box, px(a, 6), color_alpha(color_hex(col), 28));
            draw_rect(rr, rect_make(box.x, box.y + px(a, 6), px(a, 3), box.h - px(a, 12)), color_hex(col));
            char line[520];
            snprintf(line, sizeof line, "%s: %s", edit_apply_status_name(ar->status), ar->message);
            line[0] = (char)toupper((unsigned char)line[0]);
            draw_text_fit(a->font_small, rr, line, -1, rect_make(box.x + px(a, 12), box.y + px(a, 3), box.w - px(a, 24), px(a, 20)), color_hex(T_TEXT), ALIGN_LEFT);
            draw_text_fit(a->font_small, rr, ar->conflict[0] ? ar->conflict : "Fix the values or discard the changes, then apply again.", -1, rect_make(box.x + px(a, 12), box.y + px(a, 22), box.w - px(a, 24), px(a, 20)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        }
    }
    /* 2. statement list: measure every statement, clamp the scroll, draw the visible ones */
    Rect list = rect_make(p.x + px(a, 24), p.y + top, p.w - px(a, 48), p.h - top - px(a, 72));
    if (input && rect_contains(list, in->mouse_x, in->mouse_y) && in->wheel_y != 0) { r->review_scroll -= in->wheel_y; in->wheel_y = 0; }
    /* statements soft-wrap so the WHERE snapshot is readable (max 6 lines each) */
    float lineh = font_height(a->font_mono) * 1.3f, sqlw = list.w - px(a, 32);
    float *tops = malloc(sizeof *tops * (e->nstmts + 1));
    float total = px(a, 6);
    for (int i = 0; i < e->nstmts; i++) {
        tops[i] = total;
        float th = draw_wrapped(a->font_mono, rr, e->stmts[i].shown, rect_make(0, 0, sqlw, 10000), color_hex(T_TEXT), 0, false);
        if (th > lineh * 6) th = lineh * 6;
        total += th + px(a, 30);
    }
    tops[e->nstmts] = total;
    float maxs = fmaxf(0, total - list.h);
    r->review_scroll = clampf(r->review_scroll, 0, maxs);
    if (input) ui_scrollbar_input(a, &r->review_sb, list, total, &r->review_scroll);
    if (!input) {
        draw_rrect(rr, list, px(a, M_RADIUS), color_hex(T_BG));
        clip_push(rr, list);
        for (int i = 0; i < e->nstmts; i++) {
            float y = list.y + tops[i] - r->review_scroll, lh = tops[i + 1] - tops[i];
            if (y + lh < list.y || y > list.y + list.h) continue;
            EditStmt *st = &e->stmts[i];
            uint32_t col = st->kind == 1 ? T_RED : st->kind == 2 ? T_GREEN : T_YELLOW;
            bool failed = r->apply_shown && r->apply_res.stmt == i;
            if (failed) draw_rrect(rr, rect_make(list.x + px(a, 4), y + px(a, 2), list.w - px(a, 8), lh - px(a, 6)), px(a, 4), color_alpha(color_hex(T_RED), 30));
            draw_rect(rr, rect_make(list.x + px(a, 8), y + px(a, 6), px(a, 3), lh - px(a, 14)), color_hex(col));
            Rect sr = rect_make(list.x + px(a, 20), y + px(a, 4), sqlw, lh - px(a, 30));
            clip_push(rr, sr);
            draw_wrapped(a->font_mono, rr, st->shown, sr, color_hex(T_TEXT), 0, true);
            clip_pop(rr);
            draw_text_fit(a->font_small, rr, st->note, -1, rect_make(list.x + px(a, 20), y + lh - px(a, 26), list.w - px(a, 32), px(a, 20)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        }
        clip_pop(rr);
        ui_scrollbar_draw(a, &r->review_sb, list, total, r->review_scroll);
    }
    free(tops);
    /* 3. buttons; in the input pass the dialog is modal and eats every key */
    Rect bapply = rect_make(p.x + p.w - px(a, 24) - px(a, 170), p.y + p.h - px(a, 24) - px(a, 30), px(a, 170), px(a, 30));
    Rect bdisc = rect_make(bapply.x - px(a, 8) - px(a, 110), bapply.y, px(a, 110), px(a, 30));
    Rect bclose = rect_make(bdisc.x - px(a, 8) - px(a, 84), bapply.y, px(a, 84), px(a, 30));
    char lbl[64];
    snprintf(lbl, sizeof lbl, "Apply %d statement%s", e->nstmts, e->nstmts == 1 ? "" : "s");
    bool can_apply = e->nstmts > 0 && v->db != NULL && !(r->apply_shown && r->apply_res.status == APPLY_UNKNOWN);
    if (ui_button(a, bapply, lbl, BTN_PRIMARY, can_apply) && input) apply_changes(r, v, a);
    if (r->apply_shown && r->apply_res.status == APPLY_UNKNOWN && !input)
        draw_text_fit(a->font_small, rr, "Re-run the preview to see what the database has before applying again", -1, rect_make(p.x + px(a, 24), bapply.y + px(a, 5), bclose.x - p.x - px(a, 32), px(a, 20)), color_hex(T_YELLOW), ALIGN_LEFT);
    if (ui_button(a, bdisc, "Discard all", BTN_DANGER, true) && input) { edits_clear(v); r->rv_review = false; r->apply_shown = false; app_status(a, "Pending changes discarded"); }
    if (ui_button(a, bclose, "Close", BTN_NORMAL, true) && input) r->rv_review = false;
    if (input) {
        if (input_key(in, SDLK_ESCAPE, 0)) r->rv_review = false;
        in->mouse_consumed = true;
        in->nkeys = 0; in->ntext = 0;
    }
}

/* ---- chart (US-039) -------------------------------------------------------- */

/* Series colours, in order (blue, green, yellow, red). */
static const uint32_t chart_colors[] = { 0x5B8DEF, 0x5FD39B, 0xE8C266, 0xF27070 };

/* Chart of the selection (US-039), as bars, lines or pie. The first selected column is the
 * label (x) axis; up to four numeric columns of the selection are the series (or the first
 * numeric column of the grid when the selection has none). At most 500 rows are charted.
 * input = true handles the type switch and closing (pre-update pass); false draws. */
static void chart_panel(Results *r, ResultView *v, App *a, bool input) {
    SDL_Renderer *rr = a->renderer;
    Input *in = &a->in;
    Grid *g = &v->g;
    float w = a->width * 0.72f, h = a->height * 0.72f;
    Rect p = rect_make((a->width - w) / 2, (a->height - h) / 2, w, h);
    /* 1. what to chart: label column, measures and rows */
    int r0, c0, r1, c1;
    if (!grid_sel_rect(g, &r0, &c0, &r1, &c1)) { r0 = 0; r1 = g->nvis - 1; c0 = 0; c1 = g->ncols - 1; }
    int dim = c0, meas[4], nm = 0;
    for (int c = c0; c <= c1 && nm < 4; c++) if (c != dim && grid_numeric(g, c)) meas[nm++] = c;
    if (!nm) for (int c = 0; c < g->ncols && nm < 1; c++) if (c != dim && grid_numeric(g, c)) meas[nm++] = c;
    int total = r1 - r0 + 1, n = total > 500 ? 500 : total;
    /* 2. backdrop, title and the note on how complete the data is */
    if (!input) {
        draw_rect(rr, rect_make(0, 0, (float)a->width, (float)a->height), color_alpha(color_hex(0x000000), 110));
        draw_rrect(rr, p, px(a, 10), color_hex(T_BORDER));
        draw_rrect(rr, rect_inset(p, px(a, 1), px(a, 1)), px(a, 9), color_hex(T_PANEL_ALT));
        char title[300], nr[32], nl[32];
        fmt_int(nr, sizeof nr, n); fmt_int(nl, sizeof nl, v->rs.nrows);
        snprintf(title, sizeof title, "Chart  \xC2\xB7  x: %s  \xC2\xB7  series: %s%s", grid_col_name(g, dim), nm ? grid_col_name(g, meas[0]) : "none (no numeric column)", nm > 1 ? ", \xE2\x80\xA6" : "");
        for (int k = 1; k < nm && k < 4; k++) { size_t tl = strlen(title); if (k == 1) title[tl - 4] = 0; tl = strlen(title); snprintf(title + tl, sizeof title - tl, ", %s", grid_col_name(g, meas[k])); }
        if (nm == 1) snprintf(title, sizeof title, "Chart  \xC2\xB7  %s by %s", grid_col_name(g, meas[0]), grid_col_name(g, dim));
        draw_text_fit(a->font_ui, rr, title, -1, rect_make(p.x + px(a, 24), p.y + px(a, 14), p.w - px(a, 260), px(a, 26)), color_hex(T_TEXT), ALIGN_LEFT);
        char note[220];
        bool partial = v->rs.cursor || v->rs.cursor_lost;
        char nf[32]; fmt_int(nf, sizeof nf, g->nvis);
        if (g->nfilters) snprintf(note, sizeof note, "Selection: %s row%s%s  \xC2\xB7  of %s filtered locally, %s loaded%s", nr, strcmp(nr, "1") ? "s" : "", total > 500 ? " (first 500 of the selection)" : "", nf, nl, partial ? "  \xC2\xB7  result is partial (more rows on the server)" : "");
        else snprintf(note, sizeof note, "Selection: %s of %s rows%s%s", nr, nl, total > 500 ? " (first 500 of the selection)" : "", partial ? "  \xC2\xB7  result is partial (more rows on the server)" : "");
        draw_text_fit(a->font_small, rr, note, -1, rect_make(p.x + px(a, 24), p.y + px(a, 40), p.w - px(a, 48), px(a, 20)), color_hex(partial ? T_YELLOW : T_TEXT_MUTED), ALIGN_LEFT);
    }
    /* 3. chart type switch and Close; the input pass stops here */
    static const char *const types[] = { "Bars", "Lines", "Pie" };
    Rect seg = rect_make(p.x + p.w - px(a, 24) - px(a, 220), p.y + px(a, 12), px(a, 220), px(a, 28));
    int sel = ui_segmented(a, seg, types, 3, r->chart_type);
    if (sel >= 0 && input) r->chart_type = sel;
    Rect bclose = rect_make(p.x + p.w - px(a, 24) - px(a, 84), p.y + p.h - px(a, 24) - px(a, 30), px(a, 84), px(a, 30));
    if (ui_button(a, bclose, "Close", BTN_PRIMARY, true) && input) r->chart_open = false;
    if (input) {
        if (input_key(in, SDLK_ESCAPE, 0)) r->chart_open = false;
        if (in->mouse_pressed && !in->mouse_consumed && !rect_contains(p, in->mouse_x, in->mouse_y)) r->chart_open = false;
        in->mouse_consumed = true; in->nkeys = 0; in->ntext = 0;
        return;
    }
    /* 4. drawing: plot area, then value range, legend and the chosen chart */
    Rect area = rect_make(p.x + px(a, 60), p.y + px(a, 70), p.w - px(a, 84), p.h - px(a, 70) - px(a, 110));
    draw_rrect(rr, rect_make(p.x + px(a, 24), p.y + px(a, 66), p.w - px(a, 48), p.h - px(a, 66) - px(a, 100)), px(a, M_RADIUS), color_hex(T_BG));
    if (!nm || n <= 0) { draw_text_fit(a->font_ui, rr, "Select a numeric column (and a label column to its left) to chart", -1, area, color_hex(T_TEXT_DIM), ALIGN_CENTER); return; }
    /* values */
    double vmax = 0, vmin = 0;
    for (int i = 0; i < n; i++) for (int m = 0; m < nm; m++) {
        const char *s = grid_cell(g, r0 + i, meas[m]); double d = s ? strtod(s, NULL) : 0;
        if (d > vmax) vmax = d; if (d < vmin) vmin = d;
    }
    if (vmax == vmin) vmax = vmin + 1;
    /* legend */
    float lx = p.x + px(a, 24), ly = p.y + p.h - px(a, 92);
    for (int m = 0; m < nm; m++) {
        draw_rrect(rr, rect_make(lx, ly + px(a, 6), px(a, 10), px(a, 10)), px(a, 2), color_hex(chart_colors[m]));
        float tw = font_width(a->font_small, grid_col_name(g, meas[m]), -1);
        draw_text_fit(a->font_small, rr, grid_col_name(g, meas[m]), -1, rect_make(lx + px(a, 14), ly, tw + px(a, 4), px(a, 22)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        lx += tw + px(a, 30);
    }
    /* pie: share of positive values of the first series, at most 24 slices */
    if (r->chart_type == 2) {
        double sum = 0;
        for (int i = 0; i < n; i++) { const char *s = grid_cell(g, r0 + i, meas[0]); double d = s ? strtod(s, NULL) : 0; if (d > 0) sum += d; }
        if (sum <= 0) { draw_text_fit(a->font_ui, rr, "Pie needs positive values", -1, area, color_hex(T_TEXT_DIM), ALIGN_CENTER); return; }
        float cx = area.x + area.w * 0.35f, cy = area.y + area.h / 2, rad = fminf(area.w * 0.3f, area.h * 0.45f);
        double ang = -M_PI / 2;
        int slices = n > 24 ? 24 : n;
        float legy = area.y;
        for (int i = 0; i < slices; i++) {
            const char *s = grid_cell(g, r0 + i, meas[0]); double d = s ? strtod(s, NULL) : 0;
            if (d <= 0) continue;
            double span = 2 * M_PI * d / sum;
            Color c = color_hex(chart_colors[i % 4]);
            if (i >= 4) c = color_mix(c, color_hex(T_PANEL), 0.15f * ((i / 4) % 4));
            for (double t = 0; t < span; t += 0.004) draw_line(rr, cx, cy, cx + (float)cos(ang + t) * rad, cy + (float)sin(ang + t) * rad, px(a, 2), c);
            ang += span;
            if (legy < area.y + area.h - px(a, 18)) {
                char lbl[120]; const char *dv = grid_cell(g, r0 + i, dim);
                snprintf(lbl, sizeof lbl, "%s  %.1f%%", dv ? dv : "NULL", 100.0 * d / sum);
                draw_rrect(rr, rect_make(area.x + area.w * 0.7f, legy + px(a, 4), px(a, 10), px(a, 10)), px(a, 2), c);
                draw_text_fit(a->font_small, rr, lbl, -1, rect_make(area.x + area.w * 0.7f + px(a, 14), legy, area.w * 0.3f - px(a, 14), px(a, 18)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
                legy += px(a, 18);
            }
        }
        if (n > 24) draw_text_fit(a->font_small, rr, "first 24 slices shown", -1, rect_make(area.x, area.y + area.h - px(a, 18), area.w * 0.6f, px(a, 18)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        return;
    }
    /* axes and gridlines */
    float zero_y = area.y + area.h * (float)(vmax / (vmax - vmin));
    char lbl[64];
    for (int k = 0; k <= 4; k++) {
        double val = vmin + (vmax - vmin) * k / 4.0;
        float gy = area.y + area.h * (float)((vmax - val) / (vmax - vmin));
        draw_line(rr, area.x, gy, area.x + area.w, gy, px(a, 1), color_alpha(color_hex(T_BORDER), 140));
        snprintf(lbl, sizeof lbl, "%.6g", val);
        draw_text_fit(a->font_small, rr, lbl, -1, rect_make(p.x + px(a, 8), gy - px(a, 8), px(a, 50), px(a, 16)), color_hex(T_TEXT_DIM), ALIGN_RIGHT);
    }
    draw_line(rr, area.x, zero_y, area.x + area.w, zero_y, px(a, 1), color_hex(T_BORDER));
    draw_line(rr, area.x, area.y, area.x, area.y + area.h, px(a, 1), color_hex(T_BORDER));
    /* a measure far below the axis range is invisible: say so instead of drawing slivers */
    for (int m = 0; m < nm; m++) {
        double mmax = 0;
        for (int i = 0; i < n; i++) { const char *s = grid_cell(g, r0 + i, meas[m]); double d = s ? fabs(strtod(s, NULL)) : 0; if (d > mmax) mmax = d; }
        if (mmax < 0.03 * fmax(fabs(vmax), fabs(vmin))) {
            snprintf(lbl, sizeof lbl, "%s is below 3%% of the axis range and barely shows", grid_col_name(g, meas[m]));
            draw_text_fit(a->font_small, rr, lbl, -1, rect_make(p.x + px(a, 24), p.y + p.h - px(a, 70), p.w - px(a, 200), px(a, 18)), color_hex(T_YELLOW), ALIGN_LEFT);
            break;
        }
    }
    draw_text_fit(a->font_small, rr, grid_col_name(g, dim), -1, rect_make(area.x, area.y + area.h + px(a, 20), area.w, px(a, 16)), color_hex(T_TEXT_DIM), ALIGN_CENTER);
    /* bars or lines, one slot per row; labels under the axis when there are 24 rows or fewer */
    float slot = area.w / n;
    for (int i = 0; i < n; i++) {
        float x0 = area.x + i * slot;
        for (int m = 0; m < nm; m++) {
            const char *s = grid_cell(g, r0 + i, meas[m]); double d = s ? strtod(s, NULL) : 0;
            float y = area.y + area.h * (float)((vmax - d) / (vmax - vmin));
            Color c = color_hex(chart_colors[m]);
            if (r->chart_type == 0) {
                float bw = fmaxf(px(a, 1), (slot - px(a, 2)) / nm);
                Rect bar = rect_make(x0 + px(a, 1) + m * bw, fminf(y, zero_y), bw - (bw > px(a, 3) ? px(a, 1) : 0), fabsf(zero_y - y));
                draw_rect(rr, bar, c);
            } else if (i > 0) {
                const char *ps = grid_cell(g, r0 + i - 1, meas[m]); double pd = ps ? strtod(ps, NULL) : 0;
                float py = area.y + area.h * (float)((vmax - pd) / (vmax - vmin));
                draw_line(rr, x0 - slot / 2, py, x0 + slot / 2, y, px(a, 2), c);
            }
        }
        if (n <= 24) {
            const char *dv = grid_cell(g, r0 + i, dim);
            draw_text_fit(a->font_small, rr, dv ? dv : "NULL", -1, rect_make(x0, area.y + area.h + px(a, 4), slot, px(a, 16)), color_hex(T_TEXT_DIM), ALIGN_CENTER);
        }
    }
    if (n > 24) {
        const char *first = grid_cell(g, r0, dim), *last = grid_cell(g, r0 + n - 1, dim);
        draw_text_fit(a->font_small, rr, first ? first : "NULL", -1, rect_make(area.x, area.y + area.h + px(a, 4), area.w / 2, px(a, 16)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        draw_text_fit(a->font_small, rr, last ? last : "NULL", -1, rect_make(area.x + area.w / 2, area.y + area.h + px(a, 4), area.w / 2, px(a, 16)), color_hex(T_TEXT_DIM), ALIGN_RIGHT);
    }
}

/* True for an http(s) URL without spaces, quotes or angle brackets (under 2 KB): offered as
 * "Open Link", never opened automatically. */
static bool cell_is_link(const char *v) {
    return v && (strncasecmp(v, "http://", 7) == 0 || strncasecmp(v, "https://", 8) == 0) && strlen(v) < 2048 && !strpbrk(v, " \n\r\t\"'<>");
}

/* Following a foreign key (Go to referenced row): the referenced table's tab, set up when the
 * query lands (it runs on the execution worker) */
typedef struct FkNav { Results *r; char to_table[256], where[1400], text[400]; } FkNav;
static void edit_capability(ResultView *v, App *a);
static void fk_landed(App *a, int tab, const char *err, void *ud) {
    FkNav *fn = ud; Results *r = fn->r;
    ResultView *nv = NULL;
    for (int i = 0; tab && i < r->ntabs; i++) if (r->tabs[i]->exec_id == tab) nv = r->tabs[i];
    if (nv && nv->rs.ok) {
        snprintf(nv->table, sizeof nv->table, "%s", fn->to_table); snprintf(nv->title, sizeof nv->title, "%s \xC2\xB7 %s", fn->to_table, fn->text);
        snprintf(nv->srv_where, sizeof nv->srv_where, "%s", fn->where); snprintf(nv->srv_where_text, sizeof nv->srv_where_text, "%s", fn->text);
        nv->mode = RUN_PREVIEW; edit_capability(nv, a);
    } else app_status(a, "Cannot open %s: %s", fn->to_table, nv && nv->rs.error[0] ? nv->rs.error : err ? err : "the referenced table is not accessible");
    free(fn);
}

/* Loads the foreign keys of the previewed table once, through the driver (when it supports
 * it). A failure just leaves no keys. */
static void load_fks(ResultView *v) {
    if (v->fks_loaded || !v->db || !v->table[0] || !v->db->drv->list_fks) { v->fks_loaded = true; return; }
    char err[256];
    if (!v->db->drv->list_fks(v->db, v->table, &v->fks, &v->nfks, err, sizeof err)) { v->fks = NULL; v->nfks = 0; }
    v->fks_loaded = true;
}

/* Harness: centre of visible display cell (row, col) in window units. False when off-grid or
 * the geometry is not computed yet. */
bool results_cell_pos(Results *r, App *a, int row, int col, float *x, float *y) {
    ResultView *v = cur(r);
    if (!live(v) || row < 0 || col < 0 || row >= v->g.nvis || col >= v->g.ncols || v->rowy_n != v->g.nvis) return false;
    Rect cr = cell_rect(r, v, row, col);
    *x = (cr.x + cr.w / 2) / a->density; *y = (cr.y + cr.h / 2) / a->density;
    return true;
}
/* Harness: centre of the header of display column col in window units; false when not visible. */
bool results_head_pos(Results *r, App *a, int col, float *x, float *y) {
    ResultView *v = cur(r);
    float cx, cw;
    if (!live(v) || col < 0 || col >= v->g.ncols || !col_visible(r, v, col, &cx, &cw)) return false;
    *x = (cx + cw / 2) / a->density; *y = (r->cells_rc.y - r->head_h / 2) / a->density;
    return true;
}
/* Harness: centre of the open context-menu item labelled `label`, window units. */
bool results_menu_pos(Results *r, App *a, const char *label, float *x, float *y) {
    Rect rc;
    if (!popup_item_rect(a, &r->menu, label, &rc)) return false;
    *x = (rc.x + rc.w / 2) / a->density; *y = (rc.y + rc.h / 2) / a->density;
    return true;
}

/* Selects every cell of the current tab. */
void results_select_all(Results *r) { ResultView *v = cur(r); if (live(v)) grid_select_all(&v->g); }

/* Opens and focuses the find bar of the current tab (or the Output tab find when it is shown). */
void results_find(Results *r) {
    if (r->output_active && g_app) { output_find(g_app->output); return; }
    ResultView *v = cur(r);
    if (!live(v)) return;
    v->search_open = true;
    v->search_focus = true;
    v->search.blink = SDL_GetTicks();
}

/* Copies the selection of the current tab to the clipboard in FMT_* format, optionally with a
 * header line; the SQL format uses the table name of previews. */
static void copy_as(Results *r, int fmt, bool headers) {
    ResultView *v = cur(r);
    if (!live(v)) return;
    char *s = grid_copy(&v->g, fmt, headers, v->qtable[0] ? v->title : NULL);
    if (s) { SDL_SetClipboardText(s); free(s); }
}

/* Cmd+C on the grid: selection as TSV without headers. */
void results_copy_selection(Results *r) { ResultView *v = cur(r); if (live(v) && v->rs.has_rows) copy_as(r, FMT_TSV, false); }

/* Cycles to the next (dir >= 0) or previous tab, wrapping; closes the overlays of the old tab. */
void results_next_tab(Results *r, int dir) {
    if (r->ntabs < 2) return;
    for (int n = 0; n < r->ntabs; n++) { r->cur = (r->cur + (dir >= 0 ? 1 : r->ntabs - 1)) % r->ntabs; if (visible_v(r, r->tabs[r->cur])) break; }
    r->vv_open = r->fp_open = r->cp_open = r->rn_open = false;
}

/* Closes the current tab, asking first when it has unsaved edits. False when there is no tab. */
bool results_close_tab(Results *r, App *a) {
    if (!r->ntabs) return false;
    tab_close_request(r, a, r->cur);
    return true;
}

/* a tab with unsaved changes asks first (US-056); the dialog offers to cancel the close */
static int tab_unsaved(ResultView *v) { return live(v) && v->edit_ok && v->ed_init ? edit_pending_count(&v->ed) : 0; }
/* User-initiated close of tab i: opens the confirmation when it has unsaved edits, else closes. */
static void tab_close_request(Results *r, App *a, int i) {
    if (i < 0 || i >= r->ntabs) return;
    if (tab_unsaved(r->tabs[i]) > 0) { r->cf_open = true; r->cf_tab = i; popup_close(&r->menu); return; }
    tab_remove(r, a, i);
}

/* Confirmation for closing a tab with unsaved edits (US-056): Discard and close, Keep open,
 * or Review changes (opens the review dialog on that tab). input = true handles input, false draws. */
static void close_confirm(Results *r, App *a, bool input) {
    SDL_Renderer *rr = a->renderer;
    Input *in = &a->in;
    if (r->cf_tab < 0 || r->cf_tab >= r->ntabs) { r->cf_open = false; return; }
    ResultView *v = r->tabs[r->cf_tab];
    int n = tab_unsaved(v);
    float w = px(a, 460), h = px(a, 150);
    Rect p = rect_make((a->width - w) / 2, (a->height - h) / 2, w, h);
    if (!input) {
        draw_rect(rr, rect_make(0, 0, (float)a->width, (float)a->height), color_alpha(color_hex(0x000000), 110));
        draw_rrect(rr, p, px(a, 10), color_hex(T_BORDER));
        draw_rrect(rr, rect_inset(p, px(a, 1), px(a, 1)), px(a, 9), color_hex(T_PANEL_ALT));
        char t[200];
        snprintf(t, sizeof t, "Close \"%s\"?", v->title);
        draw_text_fit(a->font_ui, rr, t, -1, rect_make(p.x + px(a, 24), p.y + px(a, 16), w - px(a, 48), px(a, 26)), color_hex(T_TEXT), ALIGN_LEFT);
        int rows = edit_pending_rows(&v->ed);
        snprintf(t, sizeof t, "%d unsaved row%s (%d cell%s) would be lost. They are not in the database yet.", rows, rows == 1 ? "" : "s", n, n == 1 ? "" : "s");
        draw_text_fit(a->font_small, rr, t, -1, rect_make(p.x + px(a, 24), p.y + px(a, 46), w - px(a, 48), px(a, 22)), color_hex(T_YELLOW), ALIGN_LEFT);
    }
    Rect bd = rect_make(p.x + w - px(a, 24) - px(a, 170), p.y + h - px(a, 24) - px(a, 30), px(a, 170), px(a, 30));
    Rect bc = rect_make(bd.x - px(a, 8) - px(a, 100), bd.y, px(a, 100), px(a, 30));
    Rect br = rect_make(p.x + px(a, 24), bd.y, px(a, 130), px(a, 30));
    if (ui_button(a, bd, "Discard and close", BTN_DANGER, true) && input) { r->cf_open = false; tab_remove(r, a, r->cf_tab); }
    if (ui_button(a, bc, "Keep open", BTN_PRIMARY, true) && input) r->cf_open = false;
    if (ui_button(a, br, "Review changes", BTN_NORMAL, true) && input) { r->cf_open = false; r->cur = r->cf_tab; edit_build(&v->ed); r->rv_review = true; r->review_scroll = 0; }
    if (input) {
        if (input_key(in, SDLK_ESCAPE, 0)) r->cf_open = false;
        in->mouse_consumed = true;
        in->nkeys = 0; in->ntext = 0;
    }
}

/* Toggles the columns panel (hide / reorder / freeze) for the current tab. */
void results_columns_panel(Results *r) { if (live(cur(r))) { r->cp_open = !r->cp_open; r->cp_scroll = 0; } }

/* ---- export (US-023 / US-024 / US-025) --------------------------------- */

/* Opens the export dialog with scope 0 selection / 1 loaded rows / 2 full result. Refused while
 * the tab has unsaved edits. On the Output tab, goes straight to the save dialog instead. */
void results_export_dialog(Results *r, App *a, int scope) {
    if (r->output_active) { app_export_results(a, 1); return; }   /* the Output tab exports its events through the save dialog */
    ResultView *v = cur(r);
    if (live(v) && v->ed_init && edit_pending_count(&v->ed) > 0) { app_status(a, "Export is off while %d row%s are unsaved: apply or discard them first", edit_pending_rows(&v->ed), edit_pending_rows(&v->ed) == 1 ? "" : "s"); return; }
    if (!live(v) || !v->rs.has_rows) { app_status(a, "Nothing to export"); return; }
    load_extractors(r, a);
    r->ex_extractor = -1;
    r->ex_open = true;
    r->ex_scope = scope;
    r->ex_focus = -1;
    if (v->qtable[0]) textfield_set(&r->ex_table, v->title);
    else if (!r->ex_table.len) textfield_set(&r->ex_table, "table_name");
    popup_close(&r->menu);
}

/* progress and Esc only: the size / time limits travel in ExportOpts and are enforced by grid_export_full */
typedef struct { App *a; const char *path; Uint64 last, start; char title[160]; } ExportProg;

/* Progress callback of a full export: pumps events, returns EXPORT_CANCELLED on Esc, and shows
 * rows / MB / seconds in the window title at most every 150 ms. ud is an ExportProg. */
static int export_progress(void *ud, const ExportProgress *pr) {
    ExportProg *p = ud;
    SDL_PumpEvents();
    SDL_Event ev;
    while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_DOWN) == 1)
        if (ev.key.key == SDLK_ESCAPE) return EXPORT_CANCELLED;
    Uint64 now = SDL_GetTicks();
    if (now - p->last > 150) {
        char t[240], rows[32], total[48];
        fmt_int(rows, sizeof rows, pr->rows);
        if (pr->total >= 0) { char tt[32]; fmt_int(tt, sizeof tt, pr->total); snprintf(total, sizeof total, "of %s loaded", tt); }
        else snprintf(total, sizeof total, "total unknown");
        snprintf(t, sizeof t, "Lume \xE2\x80\x94 exporting\xE2\x80\xA6 %s rows %s \xC2\xB7 %.1f MB \xC2\xB7 %.0f s (Esc cancels)", rows, total, pr->bytes / 1048576.0, pr->secs);
        SDL_SetWindowTitle(p->a->window, t);
        p->last = now;
    }
    return EXPORT_GO;
}

/* keeps the window alive while an extractor runs; Esc cancels (US-045) */
static bool extract_pump(void *ud) {
    ExportProg *p = ud;
    SDL_PumpEvents();
    SDL_Event ev;
    while (SDL_PeepEvents(&ev, 1, SDL_GETEVENT, SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_DOWN) == 1)
        if (ev.key.key == SDLK_ESCAPE) return false;
    (void)p;
    return true;
}

/* Translates the export dialog choices into ExportOpts (separator, quoting, line ending, NULL
 * form, raw or displayed values, formula guard, SQL table name). */
static ExportOpts export_opts(Results *r, ResultView *v) {
    ExportOpts o = { 0 };
    static const char seps[] = { ',', ';', '\t', '|' };
    o.fmt = r->ex_fmt;
    o.headers = r->ex_headers;
    o.sep = seps[r->ex_sep & 3];
    o.quote_all = r->ex_quote == 1;
    o.crlf = r->ex_nl == 1;
    o.null_as = r->ex_null;
    o.displayed = r->ex_values == 1;
    o.guard = r->ex_guard;
    o.table = r->ex_table.len ? r->ex_table.buf : (v->qtable[0] ? v->title : NULL);
    return o;
}

/* Writes the current tab to `path` with the dialog options (called by main.c once the save
 * dialog returns). Paths, in order: a custom extractor (TSV to a sandboxed program), XLSX
 * (loaded rows only), full result streamed from the cursor (scope 2, with row / MB / time
 * limits and Esc), or the loaded rows / selection. The file extension overrides the format.
 * Refused with unsaved edits. Blocks the UI thread while writing; reports in the status bar.
 * Returns true when a file was written. */
bool results_export(Results *r, App *a, const char *path) {
    if (r->output_active) return output_export(a->output, a, path);
    ResultView *v = cur(r);
    if (!live(v) || !v->rs.has_rows) { app_status(a, "Nothing to export"); return false; }
    if (v->ed_init && edit_pending_count(&v->ed) > 0) {
        /* unsaved rows are not in the database: a file would present them as if they were */
        app_status(a, "Not exported: %d unsaved row%s. Apply or discard them first", edit_pending_rows(&v->ed), edit_pending_rows(&v->ed) == 1 ? "" : "s");
        return false;
    }
    if (r->exporting) return false;
    ExportOpts o = export_opts(r, v);
    /* the extension chosen in the save dialog wins over the format button */
    const char *ext = strrchr(path, '.');
    if (ext && r->ex_extractor < 0) {
        if (strcasecmp(ext, ".xlsx") == 0) o.fmt = FMT_XLSX;
        if (strcasecmp(ext, ".csv") == 0) o.fmt = FMT_CSV;
        else if (strcasecmp(ext, ".tsv") == 0 || strcasecmp(ext, ".txt") == 0) o.fmt = FMT_TSV;
        else if (strcasecmp(ext, ".json") == 0) o.fmt = FMT_JSON;
        else if (strcasecmp(ext, ".md") == 0) o.fmt = FMT_MARKDOWN;
        else if (strcasecmp(ext, ".sql") == 0) o.fmt = FMT_SQL;
    }
    /* 1. scope: a full result that is already all loaded is just the loaded rows */
    char err[256] = "";
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    int scope = r->ex_scope;
    if (scope == 2 && !v->rs.cursor && v->rs.complete) scope = 1;   /* everything is already loaded */
    if (r->ex_extractor >= 0 && r->ex_extractor < r->nextractors) {
        /* custom extractor (US-045): Lume writes a TSV into a private scratch folder, the approved
         * program runs on it inside the sandbox and its output is moved to the chosen file */
        Extractor *e = &r->extractors[r->ex_extractor];
        const char *dir = a->conns ? a->conns->dir : "";
        if (!e->trusted) { app_status(a, "Extractor \"%s\" is not approved: approve it in the Export dialog first", e->name); return false; }
        ExtractRun run;
        if (!extract_begin(dir, &run, err, sizeof err)) { app_status(a, "Export failed: %s", err); return false; }
        ExportOpts to = o; to.fmt = FMT_TSV; to.headers = true; to.null_as = NULL_WORD;
        long n = grid_export(&v->g, run.infile, &to, scope == 0 ? 0 : 1, err, sizeof err);
        if (n < 0) { extract_end(&run); app_status(a, "Export failed: %s", err); return false; }
        r->exporting = true;
        ExportProg p = { a, path, 0, SDL_GetTicks(), "" };
        const char *wt = SDL_GetWindowTitle(a->window);
        snprintf(p.title, sizeof p.title, "%s", wt ? wt : "Lume");
        char t[200]; snprintf(t, sizeof t, "Lume \xE2\x80\x94 running extractor \"%s\" on %ld rows (Esc cancels)", e->name, n);
        SDL_SetWindowTitle(a->window, t);
        ExtractResult res;
        extract_run(&run, e, path, NULL, extract_pump, &p, &res);
        extract_end(&run);
        extract_log(dir, e, &res, n, path);
        SDL_SetWindowTitle(a->window, p.title);
        r->exporting = false;
        if (res.status != EXTRACT_OK) { app_status(a, "Extractor \"%s\" %s: %s", e->name, extract_status_name(res.status), res.message); return false; }
        app_status(a, "Extractor \"%s\" wrote %s from %ld row%s (%s, sandboxed)", e->name, base, n, n == 1 ? "" : "s", res.message);
        return true;
    }
    /* 2. XLSX: always from loaded rows */
    if (o.fmt == FMT_XLSX) {
        if (scope == 2) { app_status(a, "XLSX is written from the loaded rows (the format has a 1,048,576-row limit); fetch what you need first"); scope = 1; }
        long n = grid_export_xlsx(&v->g, path, scope, o.headers, o.displayed, err, sizeof err);
        if (n < 0) { app_status(a, "Export failed: %s", err); return false; }
        app_status(a, "Exported %ld row%s to %s%s%s", n, n == 1 ? "" : "s", base, err[0] ? "; " : "", err);
        return true;
    }
    /* 3. full result: stream the cursor to the file with limits and progress */
    if (scope == 2) {
        if (!v->rs.cursor) { app_status(a, "The cursor is closed: re-run the query, then export the full result"); return false; }
        r->exporting = true;
        ExportProg p = { a, path, 0, SDL_GetTicks(), "" };
        o.max_mb = atol(r->ex_mb.buf);
        o.max_secs = atol(r->ex_secs.buf);
        const char *wt = SDL_GetWindowTitle(a->window);
        snprintf(p.title, sizeof p.title, "%s", wt ? wt : "Lume");
        long max_rows = atol(r->ex_rows.buf);
        ExportReport rep;
        long n = grid_export_full(&v->g, path, &o, max_rows, export_progress, &p, &rep);
        SDL_SetWindowTitle(a->window, p.title);
        r->exporting = false;
        grid_refresh(&v->g); v->rowy_dirty = true;
        const char *cursor_note = rep.cursor_released ? "; the cursor was released: re-run the query to fetch more rows" : "";
        if (n == -2) { app_status(a, "Export cancelled after %ld rows: %s.part removed, nothing kept%s", rep.rows, base, cursor_note); return false; }
        if (n < 0) { app_status(a, "Export failed (%s): %s. %s.part removed%s", grid_export_status_name(rep.status), rep.err, base, cursor_note); return false; }
        const char *how = rep.status == EXPORT_OK ? "full result" : rep.status == EXPORT_STOP_ROWS ? "stopped at the row limit: partial file"
                        : rep.status == EXPORT_STOP_BYTES ? (rep.hard_cap ? "stopped at the 16 GB hard cap: partial file" : "stopped at the size limit: partial file")
                        : rep.hard_cap ? "stopped at the 1 h hard cap: partial file" : "stopped at the time limit: partial file";
        app_status(a, "Exported %ld row%s, %.1f MB in %.1f s to %s (%s)%s", n, n == 1 ? "" : "s", rep.bytes / 1048576.0, rep.secs, base, how, cursor_note);
        return true;
    }
    /* 4. selection or loaded rows, in the local sort and filter */
    long n = grid_export(&v->g, path, &o, scope, err, sizeof err);
    if (n < 0) { app_status(a, "Export failed: %s", err); return false; }
    app_status(a, "Exported %ld row%s (%s%s) to %s", n, n == 1 ? "" : "s", scope == 0 ? "selection" : "loaded rows",
               v->rs.cursor || v->rs.cursor_lost ? ", not the full result" : "", base);
    return true;
}

/* ---- history (US-031) -------------------------------------------------- */

/* History (Cmd+Y): this session's executions, then earlier sessions from history.tsv (OUT-025).
 * Choosing one puts its SQL in the editor and never runs it. */
void results_history_menu(Results *r, App *a) {
    output_history_free(r->saved, r->nsaved);
    long expired = 0; int retention = 0;
    r->nsaved = output_history_load(a->output, a, r->saved, 12, &expired, &retention);
    if (!r->nhist && !r->nsaved) { app_status(a, expired ? "No executions kept: older entries expired (retention %d)" : "No executions yet", retention); return; }
    popup_open(&r->menu, px(a, 200), px(a, 40));
    r->menu_tab = -1;
    int shown = 0;
    /* this session, newest first (at most 14) */
    if (r->nhist) popup_add_caption(&r->menu, "THIS SESSION", 0, false);
    for (int i = r->nhist - 1; i >= 0 && shown < 14; i--, shown++) {
        HistEntry *h = &r->hist[i];
        struct tm tm; localtime_r(&h->when, &tm);
        char snippet[100];
        if (h->sql) sql_snippet(h->sql, (int)strlen(h->sql), snippet, sizeof snippet, 56); else snprintf(snippet, sizeof snippet, "(no SQL)");
        char *lbl = r->menu_text[shown];
        char rows[32];
        fmt_int(rows, sizeof rows, h->rows);
        snprintf(lbl, sizeof r->menu_text[0], "%s  %02d:%02d:%02d  %s  \xC2\xB7  %s  \xC2\xB7  %s", h->ok ? "\xE2\x80\xA2" : "\xC3\x97", tm.tm_hour, tm.tm_min, tm.tm_sec,
                 h->conn[0] ? h->conn : "-", snippet, h->ok ? rows : "failed");
        if (h->ok) { size_t l = strlen(lbl); snprintf(lbl + l, sizeof r->menu_text[0] - l, " row%s", h->rows == 1 ? "" : "s"); }
        popup_add(&r->menu, CM_HIST_BASE + i, lbl, false, h->sql != NULL);
    }
    /* earlier sessions from history.tsv, then how many entries expired */
    if (r->nsaved || expired) {
        snprintf(r->saved_lbl[0], sizeof r->saved_lbl[0], "EARLIER SESSIONS  \xC2\xB7  SAVED, SECRETS MASKED  \xC2\xB7  LAST %d KEPT", retention);
        popup_add_caption(&r->menu, r->saved_lbl[0], 0, r->nhist > 0);
    }
    for (int k = 0; k < r->nsaved; k++) {
        OutHistItem *h = &r->saved[k];
        time_t w = (time_t)h->when; struct tm tm; localtime_r(&w, &tm);
        char snippet[100]; sql_snippet(h->sql, (int)strlen(h->sql), snippet, sizeof snippet, 48);
        char *lbl = r->menu_text[16 + k];
        bool ok = !strcmp(h->status, "completed");
        time_t now = time(NULL); struct tm tn; localtime_r(&now, &tn);
        char date[24];   /* the year only when it is not this one */
        if (tm.tm_year == tn.tm_year) snprintf(date, sizeof date, "%02d/%02d %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_hour, tm.tm_min);
        else snprintf(date, sizeof date, "%02d/%02d/%04d %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
        char dur[24];
        snprintf(lbl, sizeof r->menu_text[0], "%s  %s  %s  \xC2\xB7  %s  \xC2\xB7  %s, %s", ok ? "\xE2\x80\xA2" : "\xC3\x97",
                 date, h->session, snippet, h->status, fmt_duration(dur, sizeof dur, h->ms));
        popup_add(&r->menu, CM_HIST_SAVED_BASE + k, lbl, false, h->sql[0] != 0);
    }
    if (expired) {
        snprintf(r->saved_lbl[1], sizeof r->saved_lbl[1], "%ld older entr%s expired (retention %d): not recoverable", expired, expired == 1 ? "y" : "ies", retention);
        popup_add_caption(&r->menu, r->saved_lbl[1], T_TEXT_DIM, false);
    }
}

/* true when the script holds anything but queries: restoring it must say so before a run */
static bool sql_modifies(const char *sql) {
    const char *p = sql;
    while (*p) {
        while (*p && (isspace((unsigned char)*p) || *p == ';' || *p == '(')) p++;
        if (!strncmp(p, "--", 2)) { while (*p && *p != '\n') p++; continue; }
        if (!*p) break;
        if (strncasecmp(p, "select", 6) && strncasecmp(p, "with", 4) && strncasecmp(p, "explain", 7) && strncasecmp(p, "show", 4) && strncasecmp(p, "values", 6)) return true;
        while (*p && *p != ';') p++;
    }
    return false;
}

/* ---- server-side sort / filter (US-012) -------------------------------- */

/* True when the tab can re-run on the server with this DB_CAP_*: a live, connected, non-stale
 * table preview whose driver has the capability. */
static bool server_capable(ResultView *v, unsigned cap) {
    return live(v) && v->db && !v->stale && v->qtable[0] && (v->db->drv->caps & cap);
}

/* Writes val as an SQL literal: as-is when numeric and it parses as a number, else single-quoted
 * with quotes doubled. Truncates to fit n. */
static void sql_literal(const char *val, bool numeric, char *out, size_t n) {
    char *end;
    if (numeric && *val) { strtod(val, &end); while (*end == ' ') end++; if (!*end) { snprintf(out, n, "%s", val); return; } }
    size_t p = 0;
    if (p < n - 1) out[p++] = '\'';
    for (const char *q = val; *q && p < n - 3; q++) { if (*q == '\'') out[p++] = '\''; out[p++] = *q; }
    out[p++] = '\''; out[p] = 0;
}

/* Writes a LIKE pattern literal for val: '%val%' (contains) or 'val%' (starts), escaping % _ and
 * backslash with a backslash (used with ESCAPE) and doubling quotes. */
static void like_literal(const char *val, bool starts, char *out, size_t n) {
    size_t p = 0;
    out[p++] = '\'';
    if (!starts) out[p++] = '%';
    for (const char *q = val; *q && p < n - 6; q++) {
        if (*q == '%' || *q == '_' || *q == '\\') out[p++] = '\\';
        if (*q == '\'') out[p++] = '\'';
        out[p++] = *q;
    }
    out[p++] = '%'; out[p++] = '\''; out[p] = 0;
}

/* Builds the server WHERE condition for source column src and filter op OP_* into out, plus a
 * human label into text (for the chip). Values always go through the literal escapers.
 * Returns false for an unknown op. */
static bool build_condition(ResultView *v, int src, int op, const char *val, const char *val2, char *out, size_t n, char *text, size_t tn) {
    char q[300], l1[600], l2[600];
    v->db->drv->quote_ident(v->rs.cols[src], q, sizeof q);
    bool numeric = v->g.numeric[src];
    const char *name = v->rs.cols[src];
    switch (op) {
    case OP_IS_NULL: snprintf(out, n, "%s IS NULL", q); snprintf(text, tn, "%s is NULL", name); return true;
    case OP_NOT_NULL: snprintf(out, n, "%s IS NOT NULL", q); snprintf(text, tn, "%s is not NULL", name); return true;
    case OP_EQUALS: sql_literal(val, numeric, l1, sizeof l1); snprintf(out, n, "%s = %s", q, l1); snprintf(text, tn, "%s = %s", name, val); return true;
    case OP_GT: sql_literal(val, numeric, l1, sizeof l1); snprintf(out, n, "%s > %s", q, l1); snprintf(text, tn, "%s > %s", name, val); return true;
    case OP_LT: sql_literal(val, numeric, l1, sizeof l1); snprintf(out, n, "%s < %s", q, l1); snprintf(text, tn, "%s < %s", name, val); return true;
    case OP_BETWEEN: sql_literal(val, numeric, l1, sizeof l1); sql_literal(val2, numeric, l2, sizeof l2); snprintf(out, n, "%s BETWEEN %s AND %s", q, l1, l2); snprintf(text, tn, "%s between %s and %s", name, val, val2); return true;
    case OP_CONTAINS: like_literal(val, false, l1, sizeof l1); snprintf(out, n, "%s LIKE %s ESCAPE '\\'", q, l1); snprintf(text, tn, "%s contains %s", name, val); return true;
    case OP_STARTS: like_literal(val, true, l1, sizeof l1); snprintf(out, n, "%s LIKE %s ESCAPE '\\'", q, l1); snprintf(text, tn, "%s starts with %s", name, val); return true;
    }
    return false;
}

/* Runs the queued server re-run of a preview tab: SELECT with its server WHERE and ORDER BY,
 * replacing the tab (RUN_REPLACE), then reports time and rows. Called from results_pre_update. */
static void server_rerun(Results *r, App *a) {
    if (r->rerun_tab < 0 || r->rerun_tab >= r->ntabs) return;
    ResultView *v = r->tabs[r->rerun_tab];
    if (!v->db || !v->qtable[0]) return;
    char order[400] = "", sql[1800];
    if (v->srv_sort[0]) {
        char q[300];
        v->db->drv->quote_ident(v->srv_sort, q, sizeof q);
        snprintf(order, sizeof order, "%s %s", q, v->srv_desc ? "DESC" : "ASC");
    }
    v->db->drv->select_sql(v->qtable, v->srv_where[0] ? v->srv_where : NULL, order[0] ? order : NULL, a->fetch_size, sql, sizeof sql);
    r->cur = r->rerun_tab;
    app_run_sql_mode(a, v->db, sql, -1, v->title, RUN_REPLACE, v->qtable);   /* on the execution worker: the tab is replaced when it lands */
}

/* Queues a server re-run of the current tab for the next frame (results_pre_update). */
static void request_rerun(Results *r) { r->rerun_pending = true; r->rerun_tab = r->cur; }

/* ---- geometry helpers -------------------------------------------------- */

/* Recomputes the top of every visible row (v->rowy, pixels, nvis + 1 entries). Rows are one
 * line high, or with wrap on, as many lines (max 8) as the tallest cell needs, estimated from
 * characters per line of a monospace digit. */
static void rows_recompute(Results *r, ResultView *v, App *a) {
    Grid *g = &v->g;
    v->rowy = realloc(v->rowy, sizeof *v->rowy * (g->nvis + 1));
    float y = 0, cw = font_width(a->font_mono, "0", 1) / a->scale;
    if (cw < 1) cw = 7;
    for (int i = 0; i < g->nvis; i++) {
        v->rowy[i] = y;
        int lines = 1;
        if (v->wrap) for (int c = 0; c < g->ncols; c++) {
            const char *s = grid_cell(g, i, c);
            if (!s || !strpbrk(s, "\n") ) { if (!s) continue; size_t L = strlen(s); int cpl = (int)((v->colw[g->cols[c]] - 20) / cw); if (cpl < 4) cpl = 4; int l = (int)((L + cpl - 1) / cpl); if (l > lines) lines = l; continue; }
            int cpl = (int)((v->colw[g->cols[c]] - 20) / cw);
            if (cpl < 4) cpl = 4;
            int l = 0;
            for (const char *p = s; ; ) {
                size_t seg = strcspn(p, "\n");
                l += seg ? (int)((seg + cpl - 1) / cpl) : 1;
                p += seg;
                if (!*p) break;
                p++;
            }
            if (l > lines) lines = l;
        }
        if (lines > 8) lines = 8;
        y += r->row_h + (lines - 1) * r->line_h;
    }
    v->rowy[g->nvis] = y;
    v->rowy_n = g->nvis; v->rowy_gen = g->gen; v->rowy_dirty = false;
}

/* Visible row under content offset y (pixels from the top of the first row), by binary search
 * over rowy; -1 when outside. */
static int row_at(ResultView *v, float y) {
    int lo = 0, hi = v->rowy_n - 1;
    if (hi < 0 || y < 0 || y >= v->rowy[v->rowy_n]) return -1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (v->rowy[mid] <= y) lo = mid; else hi = mid - 1;
    }
    return lo;
}

/* Recomputes cumulative column offsets (r->colx, pixels, display order) and the frozen width. */
static void cols_recompute(Results *r, ResultView *v, App *a) {
    Grid *g = &v->g;
    if (r->colx_cap < g->ncols + 1) { r->colx_cap = g->ncols + 1; r->colx = realloc(r->colx, sizeof *r->colx * r->colx_cap); }
    r->colx[0] = 0;
    for (int c = 0; c < g->ncols; c++) r->colx[c + 1] = r->colx[c] + px(a, v->colw[g->cols[c]]);
    r->frozen_w = r->colx[g->frozen];
}

/* Screen x of display column c: frozen columns do not scroll horizontally. */
static float col_screen_x(Results *r, ResultView *v, int c) {
    if (c < v->g.frozen) return r->cells_rc.x + r->colx[c];
    return r->cells_rc.x + r->colx[c] - v->scroll_x;
}

/* Display column under screen x mx (frozen part first, then the scrolled part); -1 when none. */
static int col_at_x(Results *r, ResultView *v, float mx) {
    Grid *g = &v->g;
    float rel = mx - r->cells_rc.x;
    if (rel < 0 || rel >= r->cells_rc.w) return -1;
    if (rel < r->frozen_w) { for (int c = 0; c < g->frozen; c++) if (rel < r->colx[c + 1]) return c; return -1; }
    rel += v->scroll_x;
    for (int c = g->frozen; c < g->ncols; c++) if (rel < r->colx[c + 1]) return c;
    return -1;
}

/* Screen x and width of display column c; true when any of it is visible (scrolled columns
 * are clipped by the frozen part). */
static bool col_visible(Results *r, ResultView *v, int c, float *x, float *w) {
    *x = col_screen_x(r, v, c);
    *w = r->colx[c + 1] - r->colx[c];
    float lo = c < v->g.frozen ? r->cells_rc.x : r->cells_rc.x + r->frozen_w;
    return *x + *w > lo && *x < r->cells_rc.x + r->cells_rc.w;
}

/* Scrolls so display cell (row, col) is in view (sets the scroll target). Skipped when the row
 * tops are stale; the next frame recomputes them. */
static void ensure_visible(Results *r, ResultView *v, int row, int col) {
    if (v->rowy_dirty || v->rowy_gen != v->g.gen || v->rowy_n != v->g.nvis) return;   /* next frame */
    float view_w = r->cells_rc.w - r->frozen_w, view_h = r->cells_rc.h;
    float ry = v->rowy[row], rh = v->rowy[row + 1] - v->rowy[row];
    if (ry < v->scroll_ty) v->scroll_ty = ry;
    else if (ry + rh > v->scroll_ty + view_h) v->scroll_ty = ry + rh - view_h;
    if (col >= v->g.frozen) {
        float cx = r->colx[col] - r->frozen_w, cw = r->colx[col + 1] - r->colx[col];
        if (cx < v->scroll_x) v->scroll_x = cx;
        else if (cx + cw > v->scroll_x + view_w) v->scroll_x = cx + cw - view_w;
    }
    if (v->scroll_x < 0) v->scroll_x = 0;
    if (v->scroll_ty < 0) v->scroll_ty = 0;
}

/* ---- overlays: open / close -------------------------------------------- */

/* Closes the value viewer and frees what it decoded (bytes, texture, geometry, JSON). */
static void viewer_close(Results *r) {
    r->vv_open = false;
    free(r->vv_bytes); r->vv_bytes = NULL; r->vv_nbytes = 0;
    if (r->vv_tex) { SDL_DestroyTexture(r->vv_tex); r->vv_tex = NULL; }
    if (r->vv_kind == 3) geo_free(&r->vv_geo);
    r->vv_kind = 0; r->vv_img_note[0] = 0;
    if (r->vv_json_ok) json_free(&r->vv_json);
    r->vv_json_ok = r->vv_json_tried = false;
    free(r->vv_collapsed); r->vv_collapsed = NULL;
}

/* binary cells (US-021): hex text -> bytes, BMP preview through SDL (the only decoder
 * bundled), never anything executed; big blobs are capped at 4 MB of hex */
static void viewer_detect_binary(Results *r, ResultView *v, App *a) {
    const char *val = grid_cell(&v->g, r->vv_row, r->vv_col);
    if (!val || !looks_like_hex_blob(val)) return;
    size_t len = strlen(val);
    if (len > 8u << 20) { r->vv_kind = 2; snprintf(r->vv_img_note, sizeof r->vv_img_note, "blob over 4 MB: shown as hex text only"); return; }
    long n = hex_decode(val, len, NULL, 0);
    if (n <= 0) return;
    r->vv_bytes = malloc(n);
    hex_decode(val, len, r->vv_bytes, n);
    r->vv_nbytes = n; r->vv_kind = 2; r->vv_mode = 0;
    if (n > 2 && r->vv_bytes[0] == 'B' && r->vv_bytes[1] == 'M') {
        SDL_IOStream *io = SDL_IOFromConstMem(r->vv_bytes, n);
        SDL_Surface *s = io ? SDL_LoadBMP_IO(io, true) : NULL;
        if (s && s->w <= 8192 && s->h <= 8192) {
            r->vv_tex = SDL_CreateTextureFromSurface(a->renderer, s);
            r->vv_tex_w = s->w; r->vv_tex_h = s->h;
            snprintf(r->vv_img_note, sizeof r->vv_img_note, "BMP image %dx%d", s->w, s->h);
            r->vv_mode = 2;
        } else snprintf(r->vv_img_note, sizeof r->vv_img_note, "BMP header but not decodable%s", s ? " (too large)" : "");
        if (s) SDL_DestroySurface(s);
    } else if (image_sniff(r->vv_bytes, n, &(const char *){ NULL })) {
        /* PNG / JPEG through Lume's own bounded decoder (US-057): a failure keeps Hex and
         * Base64 and says why; nothing external is ever loaded */
        const char *kind = "image"; image_sniff(r->vv_bytes, n, &kind);
        Image im; char ierr[200];
        if (image_decode(r->vv_bytes, n, &im, ierr, sizeof ierr)) {
            SDL_Surface *s = SDL_CreateSurfaceFrom(im.w, im.h, SDL_PIXELFORMAT_RGBA32, im.rgba, im.w * 4);
            if (s) { r->vv_tex = SDL_CreateTextureFromSurface(a->renderer, s); SDL_DestroySurface(s); }
            if (r->vv_tex) { r->vv_tex_w = im.w; r->vv_tex_h = im.h; snprintf(r->vv_img_note, sizeof r->vv_img_note, "%s image %dx%d (decoded by Lume, %ld bytes)", kind, im.w, im.h, n); r->vv_mode = 2; }
            else snprintf(r->vv_img_note, sizeof r->vv_img_note, "%s image %dx%d: texture could not be created", kind, im.w, im.h);
            image_free(&im);
        } else snprintf(r->vv_img_note, sizeof r->vv_img_note, "%s image not previewed: %s (Hex / Base64 still available)", kind, ierr);
    }
    else if (n > 4 && memcmp(r->vv_bytes, "%PDF", 4) == 0) snprintf(r->vv_img_note, sizeof r->vv_img_note, "PDF document: not rendered");
}

/* Value viewer: when the cell parses as a geometry (geo.h), switches to the Geometry view. */
static void viewer_detect_geo(Results *r, ResultView *v) {
    const char *val = grid_cell(&v->g, r->vv_row, r->vv_col);
    if (!val || r->vv_kind) return;
    if (geo_parse(val, strlen(val), &r->vv_geo)) { r->vv_kind = 3; r->vv_mode = 1; }
}

/* Value viewer: parses the cell as JSON when it looks like an object / array or the column is
 * JSON. On success opens the Tree view, folding large documents; on failure the parser error is
 * kept for the note under the text. */
static void viewer_parse_json(Results *r, ResultView *v) {
    const char *val = grid_cell(&v->g, r->vv_row, r->vv_col);
    r->vv_json_tried = true;
    r->vv_json_ok = false;
    if (!val || r->vv_kind) return;
    const char *p = val;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    bool looks = *p == '{' || *p == '[';
    if (!looks && grid_col_kind(&v->g, r->vv_col) != CK_JSON) return;
    if (json_parse(&r->vv_json, val, strlen(val))) {
        r->vv_json_ok = true;
        r->vv_kind = 1;
        int n = r->vv_json.n;
        r->vv_collapsed = calloc(n ? n : 1, sizeof *r->vv_collapsed);
        /* lazy expansion: big documents start folded below the first level */
        for (int i = 0; i < n; i++) {
            JsonNode *nd = &r->vv_json.nodes[i];
            bool container = nd->type == JN_OBJECT || nd->type == JN_ARRAY;
            r->vv_collapsed[i] = container && ((n > 300 && nd->depth >= 2) || nd->nchildren > 500);
        }
        r->vv_mode = 1;
    } else if (r->vv_json.err[0]) {
        /* keep the message; the text view shows it */
    }
}

/* Opens the value viewer on display cell (row, col) and detects its kind (binary, geometry,
 * JSON, else text). */
static void open_viewer_a(Results *r, ResultView *v, App *a, int row, int col) {
    if (row < 0 || col < 0 || !live(v)) return;
    viewer_close(r);
    r->vv_open = true; r->vv_row = row; r->vv_col = col; r->vv_scroll = 0; r->vv_mode = 0;
    viewer_detect_binary(r, v, a);
    viewer_detect_geo(r, v);
    viewer_parse_json(r, v);
}
/* open_viewer_a with the cached App. */
static void open_viewer(Results *r, ResultView *v, int row, int col) { open_viewer_a(r, v, g_app, row, col); }

/* Opens the filter popover for display column col, prefilled with value (or empty); server
 * selects the "On the server" scope. Defaults to equals for numbers or a value, else contains. */
static void open_filter(Results *r, ResultView *v, int col, const char *value, bool server) {
    r->fp_open = true;
    r->fp_col = col;
    r->fp_edit = -1;
    r->fp_server = server;
    r->fp_op = value || grid_numeric(&v->g, col) ? OP_EQUALS : OP_CONTAINS;
    textfield_set(&r->fp_v1, value ? value : "");
    textfield_set(&r->fp_v2, "");
    r->fp_focus = 0;
    r->fp_v1.blink = SDL_GetTicks();
}

/* reopens an existing filter chip for editing */
static void edit_filter(Results *r, ResultView *v, int idx) {
    GridFilter *f = &v->g.filters[idx];
    int col = -1;
    for (int c = 0; c < v->g.ncols; c++) if (v->g.cols[c] == f->col) { col = c; break; }
    if (col < 0) { grid_col_show(&v->g, f->col); for (int c = 0; c < v->g.ncols; c++) if (v->g.cols[c] == f->col) col = c; }
    if (col < 0) return;
    open_filter(r, v, col, NULL, false);
    r->fp_edit = idx;
    r->fp_op = f->op;
    textfield_set(&r->fp_v1, f->value);
    textfield_set(&r->fp_v2, f->value2);
}

/* ---- keyboard ---------------------------------------------------------- */

/* Grid keyboard (when Results has focus and nothing modal is open): arrows / Page / Home / End
 * move the cursor (Shift extends, Cmd+Home / End go to the first / last row), Tab moves along
 * the row, Enter / F2 edit (or view when read-only), Shift+F10 / Menu opens the context menu,
 * Cmd+Delete toggles row deletion, Esc closes the find bar or clears the selection. */
static void grid_keys(Results *r, ResultView *v, App *a) {
    Input *in = &a->in;
    Grid *g = &v->g;
    int page = (int)(r->cells_rc.h / r->row_h);
    if (page < 1) page = 1;
    for (int k = 0; k < in->nkeys; k++) {
        KeyPress *kp = &in->keys[k];
        if (kp->used) continue;
        bool shift = kp->mods & KM_SHIFT, prim = kp->mods & KM_PRIMARY;
        int row = g->sel_r < 0 ? 0 : g->sel_r, col = g->sel_c < 0 ? 0 : g->sel_c;
        bool move = true;
        switch (kp->key) {
        case SDLK_UP: row -= 1; break;
        case SDLK_DOWN: row += 1; break;
        case SDLK_LEFT: col -= 1; break;
        case SDLK_RIGHT: col += 1; break;
        case SDLK_PAGEUP: row -= page; break;
        case SDLK_PAGEDOWN: row += page; break;
        case SDLK_HOME: if (prim) row = 0; else col = 0; break;
        case SDLK_END: if (prim) row = g->nvis - 1; else col = g->ncols - 1; break;
        case SDLK_TAB:
            if (shift) { if (--col < 0) { col = g->ncols - 1; row--; } }
            else if (++col >= g->ncols) { col = 0; row++; }
            shift = false;
            break;
        case SDLK_RETURN: case SDLK_KP_ENTER:
            if (prim) continue;                 /* run statement, handled globally */
            if (g->sel_r >= 0) { if (v->edit_ok && !shift) cell_edit_open(r, v, g->sel_r, g->sel_c); else open_viewer(r, v, g->sel_r, g->sel_c); }
            move = false; break;
        case SDLK_F2:
            if (g->sel_r >= 0 && v->edit_ok) cell_edit_open(r, v, g->sel_r, g->sel_c);
            move = false; break;
        case SDLK_F10: case SDLK_MENU: case SDLK_APPLICATION:
            /* context menu from the keyboard (US-054): Shift+F10 or the Menu key at the cell */
            if (kp->key == SDLK_F10 && !shift) continue;
            if (g->sel_r >= 0 && v->rowy_n == g->nvis) { Rect cr = cell_rect(r, v, g->sel_r, g->sel_c); context_menu_open_at(r, v, a, g->sel_r, g->sel_c, false, cr.x + cr.w + px(a, 2), cr.y);   /* beside the cell, never over it */
                /* opened from the keyboard: start with the first item highlighted so Enter does something */
                r->menu.kb = popup_step(&r->menu, -1, 1); }
            move = false; break;
        case SDLK_DELETE: case SDLK_BACKSPACE:
            if (g->sel_r >= 0 && v->edit_ok && (kp->mods & KM_PRIMARY)) { row_toggle_delete(v, g->order[g->sel_r]); }
            move = false; break;
        case SDLK_ESCAPE:
            if (v->search_open) { v->search_open = false; grid_search(g, ""); }
            else grid_select_none(g);
            move = false; break;
        default: continue;
        }
        kp->used = true;
        if (!move || !g->nvis) continue;
        row = (int)clampf((float)row, 0, (float)(g->nvis - 1));
        col = (int)clampf((float)col, 0, (float)(g->ncols - 1));
        grid_select_cell(g, row, col, shift);
        ensure_visible(r, v, row, col);
    }
}

/* ---- value viewer (US-019 / US-020) ------------------------------------ */

/* Centred value-viewer rectangle sized to the value (bigger for JSON), at most 70% of the window. */
static Rect viewer_panel(Results *r, ResultView *v, App *a) {
    const char *val = grid_cell(&v->g, r->vv_row, r->vv_col);
    size_t len = val ? strlen(val) : 4;
    int lines = 1;
    for (const char *p = val; p && *p; p++) if (*p == '\n') lines++;
    float want_w = px(a, 80) + (val ? font_width(a->font_mono, val, (int)(len < 120 ? len : 120)) : px(a, 60));
    float want_h = px(a, 170) + (lines + (float)len / 90.0f) * font_height(a->font_mono) * 1.35f;
    if (r->vv_json_ok) { want_w = fmaxf(want_w, px(a, 640)); want_h = fmaxf(want_h, px(a, 420)); }
    float w = clampf(want_w, px(a, 520), a->width * 0.7f), h = clampf(want_h, px(a, 260), a->height * 0.7f);
    return rect_make((a->width - w) / 2, (a->height - h) / 2, w, h);
}

/* Wraps text inside rc; returns the total height used (drawn only when draw is true). */
static float draw_wrapped(Font *f, SDL_Renderer *rr, const char *s, Rect rc, Color c, float scroll, bool draw) {
    float lh = font_height(f) * 1.35f;
    float y = rc.y - scroll, total = 0;
    const char *p = s;
    while (*p) {
        int n = (int)strcspn(p, "\n"), fit = n;
        while (fit > 0 && font_width(f, p, fit) > rc.w) {
            int k = fit - 1;
            while (k > 0 && p[k] != ' ') k--;
            fit = k > 0 ? k : fit - 1;
        }
        if (fit == 0 && n > 0) fit = 1;
        if (draw && y + lh >= rc.y && y <= rc.y + rc.h) font_draw(f, rr, p, fit, rc.x, y, c);
        p += fit;
        if (fit == n && *p == '\n') p++;
        else while (*p == ' ') p++;
        y += lh; total += lh;
        if (n == 0 && *p == 0) break;
    }
    return total;
}

/* Value-viewer button and body rectangles; has_fmt when the formatted value differs from the raw. */
typedef struct { Rect copy_raw, copy_fmt, close, modes, body; bool has_fmt; } ViewerLayout;

/* Lays out the value viewer inside panel; writes the formatted value into fmt (fn bytes). */
static ViewerLayout viewer_layout(Results *r, ResultView *v, App *a, Rect panel, char *fmt, size_t fn) {
    ViewerLayout L;
    const char *val = grid_cell(&v->g, r->vv_row, r->vv_col);
    const char *shown = grid_format_value(&v->g, grid_col(&v->g, r->vv_col), val, fmt, fn);
    L.has_fmt = val && shown == fmt && strcmp(shown, val) != 0;
    L.close = rect_make(panel.x + panel.w - px(a, 24) - px(a, 80), panel.y + panel.h - px(a, 24) - px(a, 30), px(a, 80), px(a, 30));
    L.copy_raw = rect_make(L.close.x - px(a, 8) - px(a, 100), L.close.y, px(a, 100), px(a, 30));
    L.copy_fmt = rect_make(L.copy_raw.x - px(a, 8) - px(a, 130), L.close.y, px(a, 130), px(a, 30));
    L.modes = rect_make(panel.x + panel.w - px(a, 24) - px(a, 150), panel.y + px(a, 12), px(a, 150), px(a, 26));
    L.body = rect_make(panel.x + px(a, 24), panel.y + px(a, 46), panel.w - px(a, 48), panel.h - px(a, 46) - px(a, 72));
    return L;
}

/* visible tree lines after folding; returns the count and fills `out` (node ids) up to max */
static int json_lines(Results *r, int *out, int max) {
    Json *j = &r->vv_json;
    int n = 0;
    int stack[600], sp = 0;
    if (!j->n) return 0;
    stack[sp++] = 0;
    while (sp > 0) {
        int id = stack[--sp];
        if (n < max) out[n] = id;
        n++;
        JsonNode *nd = &j->nodes[id];
        /* siblings continue after this subtree: push next sibling first, then children */
        if (nd->next_sibling >= 0 && sp < 600) stack[sp++] = nd->next_sibling;
        if ((nd->type == JN_OBJECT || nd->type == JN_ARRAY) && !r->vv_collapsed[id] && nd->first_child >= 0 && sp < 600) stack[sp++] = nd->first_child;
    }
    return n;
}

/* Value-viewer input (pre-update pass): Esc / Enter close, Cmd+C and the buttons copy, the
 * segmented control switches view, a click in the Tree folds a node, a click outside closes.
 * Modal: consumes the mouse and every key. */
static void viewer_update(Results *r, App *a) {
    Input *in = &a->in;
    ResultView *v = cur(r);
    if (!r->vv_open) return;
    if (!live(v)) { viewer_close(r); return; }
    const char *val = grid_cell(&v->g, r->vv_row, r->vv_col);
    if (input_key(in, SDLK_ESCAPE, 0) || input_key(in, SDLK_RETURN, 0)) { viewer_close(r); in->nkeys = 0; in->ntext = 0; in->mouse_consumed = true; return; }
    if (input_key(in, SDLK_C, KM_PRIMARY)) { SDL_SetClipboardText(val ? val : ""); app_status(a, "Value copied"); }
    Rect panel = viewer_panel(r, v, a);
    char fmt[64];
    ViewerLayout L = viewer_layout(r, v, a, panel, fmt, sizeof fmt);
    if (rect_contains(panel, in->mouse_x, in->mouse_y) && in->wheel_y != 0) { r->vv_scroll -= in->wheel_y; if (r->vv_scroll < 0) r->vv_scroll = 0; in->wheel_y = 0; }
    if (r->vv_total > 0) ui_scrollbar_input(a, &r->vv_sb, r->vv_inner, r->vv_total, &r->vv_scroll);
    if (ui_button(a, L.close, "Close", BTN_PRIMARY, true)) { viewer_close(r); in->mouse_consumed = true; return; }
    if (ui_button(a, L.copy_raw, "Copy raw", BTN_NORMAL, true)) { SDL_SetClipboardText(val ? val : ""); app_status(a, "Raw value copied"); }
    if (L.has_fmt && ui_button(a, L.copy_fmt, "Copy formatted", BTN_NORMAL, true)) { SDL_SetClipboardText(fmt); app_status(a, "Formatted value copied"); }
    if (r->vv_kind == 2) {
        static const char *const bmodes[] = { "Hex", "Base64", "Image" };
        int m = ui_segmented(a, L.modes, bmodes, r->vv_tex ? 3 : 2, r->vv_mode);
        if (m >= 0) { r->vv_mode = m; r->vv_scroll = 0; }
    } else if (r->vv_kind == 3) {
        static const char *const gmodes[] = { "Text", "Geometry" };
        int m = ui_segmented(a, L.modes, gmodes, 2, r->vv_mode);
        if (m >= 0) { r->vv_mode = m; r->vv_scroll = 0; }
    }
    if (r->vv_json_ok) {
        static const char *const modes[] = { "Text", "Tree" };
        int m = ui_segmented(a, L.modes, modes, 2, r->vv_mode);
        if (m >= 0) { r->vv_mode = m; r->vv_scroll = 0; }
        if (r->vv_mode == 1 && in->mouse_pressed && !in->mouse_consumed && rect_contains(L.body, in->mouse_x, in->mouse_y)) {
            float lh = font_height(a->font_mono) * 1.4f;
            int idx = (int)floorf((in->mouse_y - (L.body.y + px(a, 8)) + r->vv_scroll) / lh);
            int ids[4096];
            int n = json_lines(r, ids, 4096);
            if (idx >= 0 && idx < n && idx < 4096) {
                JsonNode *nd = &r->vv_json.nodes[ids[idx]];
                if (nd->type == JN_OBJECT || nd->type == JN_ARRAY) { r->vv_collapsed[ids[idx]] = !r->vv_collapsed[ids[idx]]; in->mouse_consumed = true; }
            }
        }
    }
    if (in->mouse_pressed && !in->mouse_consumed && !rect_contains(panel, in->mouse_x, in->mouse_y)) viewer_close(r);
    in->mouse_consumed = true;
    in->nkeys = 0; in->ntext = 0;
}

/* ---- filter popover ---------------------------------------------------- */

/* Lays out the filter popover: under the column header when visible (else centred), kept on
 * screen; the scope row appears only when the server can filter. Shared by input and drawing. */
static void filter_layout(Results *r, ResultView *v, App *a, Rect *panel, Rect *scope, Rect *ops1, Rect *ops2, Rect *v1, Rect *v2, Rect *apply, Rect *cancel) {
    bool can_server = server_capable(v, DB_CAP_SERVER_FILTER);
    float w = px(a, 440), h = px(a, can_server ? 236 : 196);
    /* anchored under the column header when it is on screen, else centred */
    float cw;
    float ax, ay = r->cells_rc.y;
    bool vis = r->fp_col >= 0 && r->fp_col < v->g.ncols && col_visible(r, v, r->fp_col, &ax, &cw);
    if (!vis || ay <= 0) { ax = (a->width - w) / 2; ay = (a->height - h) / 2; }
    if (ax + w > a->width - px(a, 8)) ax = a->width - px(a, 8) - w;
    if (ay + h > a->height - px(a, 8)) ay = a->height - px(a, 8) - h;
    if (ax < px(a, 8)) ax = px(a, 8);
    *panel = rect_make(ax, ay, w, h);
    float x = panel->x + px(a, 20), y = panel->y + px(a, 44);
    if (can_server) { *scope = rect_make(x, y, w - px(a, 40), px(a, 28)); y += px(a, 40); }
    else *scope = rect_make(0, 0, 0, 0);
    *ops1 = rect_make(x, y, w - px(a, 40), px(a, 28));
    *ops2 = rect_make(x, y + px(a, 28), w - px(a, 40), px(a, 28));
    y += px(a, 66);
    bool two = r->fp_op == OP_BETWEEN;
    float fw = two ? (w - px(a, 40) - px(a, 8)) / 2 : w - px(a, 40);
    *v1 = rect_make(x, y, fw, px(a, 30));
    *v2 = rect_make(x + fw + px(a, 8), y, fw, px(a, 30));
    *apply = rect_make(panel->x + w - px(a, 20) - px(a, 84), panel->y + h - px(a, 20) - px(a, 30), px(a, 84), px(a, 30));
    *cancel = rect_make(apply->x - px(a, 8) - px(a, 84), apply->y, px(a, 84), px(a, 30));
}

/* Applies the popover: server scope ANDs the condition into the tab WHERE and queues a re-run;
 * local scope adds (or replaces, when editing a chip) a grid filter. Closes the popover. */
static void filter_apply(Results *r, ResultView *v) {
    if (r->fp_server) {
        char cond[700], text[260];
        if (build_condition(v, grid_col(&v->g, r->fp_col), r->fp_op, r->fp_v1.buf, r->fp_v2.buf, cond, sizeof cond, text, sizeof text)) {
            /* never send a WHERE cut short: a truncated condition is broken or, worse, different SQL */
            size_t need = v->srv_where[0] ? strlen(v->srv_where) + strlen(cond) + 10 : strlen(cond);
            if (need >= sizeof v->srv_where) {
                if (g_app) app_status(g_app, "Server filter not applied: the combined condition is too long (%zu of %zu bytes); remove a filter or use a local one", need, sizeof v->srv_where - 1);
                return;   /* the popover stays open with the values */
            }
            if (v->srv_where[0]) {
                char tmp[sizeof v->srv_where];
                snprintf(tmp, sizeof tmp, "(%s) AND (%s)", v->srv_where, cond);
                snprintf(v->srv_where, sizeof v->srv_where, "%s", tmp);
                char tt[240];
                snprintf(tt, sizeof tt, "%s and %s", v->srv_where_text, text);
                snprintf(v->srv_where_text, sizeof v->srv_where_text, "%s", tt);
            } else { snprintf(v->srv_where, sizeof v->srv_where, "%s", cond); snprintf(v->srv_where_text, sizeof v->srv_where_text, "%s", text); }
            request_rerun(r);
        }
    } else {
        if (r->fp_edit >= 0) grid_filter_remove(&v->g, r->fp_edit);
        grid_filter_add(&v->g, r->fp_col, r->fp_op, r->fp_v1.buf, r->fp_v2.buf);
        v->rowy_dirty = true;
    }
    r->fp_open = false;
}

/* Operator rows of the filter popover; index = OP_* (first row 0..3, second row 4..7). */
static const char *const ops_a[] = { "contains", "equals", "starts with", ">" };
static const char *const ops_b[] = { "<", "between", "is NULL", "is not NULL" };
static const char *const scope_labels[] = { "Loaded rows (local)", "On the server (re-run)" };

/* Filter popover input (pre-update pass): Enter applies, Esc / Cancel / click outside close, Tab
 * switches between the two BETWEEN fields. Modal while open. */
static void filter_update(Results *r, App *a) {
    Input *in = &a->in;
    ResultView *v = cur(r);
    if (!r->fp_open) return;
    if (!live(v) || r->fp_col >= v->g.ncols) { r->fp_open = false; return; }
    Rect panel, scope, ops1, ops2, v1, v2, apply, cancel;
    filter_layout(r, v, a, &panel, &scope, &ops1, &ops2, &v1, &v2, &apply, &cancel);
    for (int k = 0; k < in->nkeys; k++) {
        KeyPress *kp = &in->keys[k];
        if (kp->used) continue;
        if (kp->key == SDLK_ESCAPE) { r->fp_open = false; kp->used = true; }
        else if (kp->key == SDLK_RETURN || kp->key == SDLK_KP_ENTER) { filter_apply(r, v); kp->used = true; }
        else if (kp->key == SDLK_TAB) { r->fp_focus = r->fp_op == OP_BETWEEN ? !r->fp_focus : 0; kp->used = true; }
    }
    if (!r->fp_open) { in->nkeys = 0; in->ntext = 0; in->mouse_consumed = true; return; }
    if (scope.w > 0) { int s = ui_segmented(a, scope, scope_labels, 2, r->fp_server ? 1 : 0); if (s >= 0) r->fp_server = s == 1; }
    int p1 = ui_segmented(a, ops1, ops_a, 4, r->fp_op < 4 ? r->fp_op : -1);
    int p2 = ui_segmented(a, ops2, ops_b, 4, r->fp_op >= 4 ? r->fp_op - 4 : -1);
    if (p1 >= 0) r->fp_op = p1;
    if (p2 >= 0) r->fp_op = p2 + 4;
    bool needs_value = r->fp_op != OP_IS_NULL && r->fp_op != OP_NOT_NULL;
    if (needs_value) {
        if (ui_textfield(a, &r->fp_v1, v1, "value", r->fp_focus == 0)) r->fp_focus = 0;
        if (r->fp_op == OP_BETWEEN && ui_textfield(a, &r->fp_v2, v2, "and", r->fp_focus == 1)) r->fp_focus = 1;
    }
    if (ui_button(a, apply, "Apply", BTN_PRIMARY, true)) filter_apply(r, v);
    if (ui_button(a, cancel, "Cancel", BTN_NORMAL, true)) r->fp_open = false;
    if (in->mouse_pressed && !in->mouse_consumed && !rect_contains(panel, in->mouse_x, in->mouse_y)) r->fp_open = false;
    in->mouse_consumed = true;
    in->nkeys = 0; in->ntext = 0;
}

/* ---- columns panel (US-005) -------------------------------------------- */

/* Columns panel rectangle: top-right of the grid, tall enough for every column up to 70% of
 * the window. */
static Rect columns_panel_rect(Results *r, ResultView *v, App *a) {
    float w = px(a, 320);
    float rows = (float)v->rs.ncols;
    float h = fminf(px(a, 150) + rows * px(a, 28), a->height * 0.7f);
    float x = r->grid_rect.x + r->grid_rect.w - w - px(a, 12), y = r->grid_rect.y + px(a, 4);
    if (y + h > a->height - px(a, 8)) y = fmaxf(px(a, 8), a->height - px(a, 8) - h);
    return rect_make(x, y, w, h);
}

/* draws (and, when `input`, handles) the panel; shared by update and overlay passes */
static void columns_panel(Results *r, ResultView *v, App *a, bool input) {
    SDL_Renderer *rr = a->renderer;
    Input *in = &a->in;
    Grid *g = &v->g;
    Rect p = columns_panel_rect(r, v, a);
    if (!input) {
        for (int s = 3; s >= 1; s--)
            draw_rrect(rr, rect_make(p.x - s * px(a, 2), p.y + s * px(a, 2), p.w + s * px(a, 4), p.h + s * px(a, 2)), px(a, 10), color_alpha(color_hex(0x000000), 40));
        draw_rrect(rr, p, px(a, 10), color_hex(T_BORDER));
        draw_rrect(rr, rect_inset(p, px(a, 1), px(a, 1)), px(a, 9), color_hex(T_PANEL_ALT));
        char title[80];
        snprintf(title, sizeof title, "Columns  \xC2\xB7  %d shown, %d hidden", g->ncols, grid_hidden_count(g));
        draw_text_fit(a->font_ui, rr, title, -1, rect_make(p.x + px(a, 16), p.y + px(a, 8), p.w - px(a, 60), px(a, 26)), color_hex(T_TEXT), ALIGN_LEFT);
    }
    /* close button, then the frozen-count stepper */
    Rect bx = rect_make(p.x + p.w - px(a, 32), p.y + px(a, 10), px(a, 22), px(a, 22));
    if (ui_button(a, bx, "", BTN_QUIET, true) && input) r->cp_open = false;
    draw_cross(rr, bx.x + bx.w / 2, bx.y + bx.h / 2, px(a, 8), px(a, 1.5f), color_hex(T_TEXT_MUTED));
    float y = p.y + px(a, 40);
    /* row 1: frozen count (the frozen columns are tinted in the list below) */
    char fz[40];
    snprintf(fz, sizeof fz, "Frozen columns: %d", g->frozen);
    draw_text_fit(a->font_ui, rr, fz, -1, rect_make(p.x + px(a, 16), y, px(a, 200), px(a, 26)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    Rect fm = rect_make(p.x + px(a, 232), y, px(a, 30), px(a, 26)), fpl = rect_make(p.x + px(a, 268), y, px(a, 30), px(a, 26));
    if (ui_button(a, fm, "-", BTN_NORMAL, g->frozen > 0) && input) { grid_freeze(g, g->frozen - 1); layout_save(a, v); }
    if (ui_button(a, fpl, "+", BTN_NORMAL, g->frozen < g->ncols - 1) && input) { grid_freeze(g, g->frozen + 1); layout_save(a, v); }
    y += px(a, 34);
    /* list: visible columns in display order, then hidden ones */
    Rect list = rect_make(p.x + px(a, 8), y, p.w - px(a, 16), p.y + p.h - y - px(a, 72));
    if (input && rect_contains(list, in->mouse_x, in->mouse_y) && in->wheel_y != 0) { r->cp_scroll -= in->wheel_y; in->wheel_y = 0; }
    float lh = px(a, 28);
    float maxs = fmaxf(0, v->rs.ncols * lh - list.h);
    r->cp_scroll = clampf(r->cp_scroll, 0, maxs);
    if (input) ui_scrollbar_input(a, &r->cp_sb, list, v->rs.ncols * lh, &r->cp_scroll);
    clip_push(rr, list);
    int n = 0;
    for (int pass = 0; pass < 2; pass++) {
        int count = pass == 0 ? g->ncols : v->rs.ncols;
        for (int i = 0; i < count; i++) {
            int src = pass == 0 ? g->cols[i] : i;
            if (pass == 1 && !g->hidden[src]) continue;
            float ry = list.y + n * lh - r->cp_scroll;
            n++;
            if (ry + lh < list.y || ry > list.y + list.h) continue;
            Rect row = rect_make(list.x, ry, list.w, lh);
            bool frozen = pass == 0 && i < g->frozen;
            if (frozen) draw_rect(rr, row, color_alpha(color_hex(T_SELECTED), 70));
            Rect cb = rect_make(row.x + px(a, 8), ry, row.w - px(a, 150), lh);
            draw_text_fit(a->font_small, rr, v->rs.types[src], -1, rect_make(row.x + row.w - px(a, 150), ry, px(a, 74), lh), color_hex(T_TEXT_DIM), ALIGN_RIGHT);
            if (ui_checkbox(a, cb, v->rs.cols[src], pass == 0, pass == 1 || g->ncols > 1) && input) {
                if (pass == 0) grid_col_hide(g, i); else grid_col_show(g, src);
                v->rowy_dirty = true;
                layout_save(a, v);
                clip_pop(rr);
                return;
            }
            if (pass == 0) {
                Rect up = rect_make(row.x + row.w - px(a, 70), ry + px(a, 2), px(a, 30), lh - px(a, 4));
                Rect dn = rect_make(row.x + row.w - px(a, 36), ry + px(a, 2), px(a, 30), lh - px(a, 4));
                bool can_up = i > 0, can_dn = i < g->ncols - 1;
                if (ui_button(a, up, "", BTN_QUIET, can_up) && input) { grid_col_move(g, i, i - 1); layout_save(a, v); }
                if (ui_button(a, dn, "", BTN_QUIET, can_dn) && input) { grid_col_move(g, i, i + 1); layout_save(a, v); }
                draw_triangle(rr, up.x + up.w / 2, up.y + up.h / 2, px(a, 9), px(a, 6), false, color_hex(can_up ? T_TEXT_MUTED : T_BORDER));
                draw_triangle(rr, dn.x + dn.w / 2, dn.y + dn.h / 2, px(a, 9), px(a, 6), true, color_hex(can_dn ? T_TEXT_MUTED : T_BORDER));
            }
        }
    }
    clip_pop(rr);
    ui_scrollbar_draw(a, &r->cp_sb, list, v->rs.ncols * lh, r->cp_scroll);
    /* footer: wrap toggle, Show all, Reset layout; input pass closes on Esc or a click outside */
    Rect wr = rect_make(p.x + px(a, 16), p.y + p.h - px(a, 64), px(a, 110), px(a, 26));
    if (ui_checkbox(a, wr, "Wrap text", v->wrap, true) && input) { v->wrap = !v->wrap; v->rowy_dirty = true; layout_save(a, v); }
    draw_text_fit(a->font_small, rr, "Saved per connection and column set", -1, rect_make(p.x + px(a, 140), p.y + p.h - px(a, 64), p.w - px(a, 156), px(a, 26)), color_hex(T_TEXT_DIM), ALIGN_RIGHT);
    Rect reset = rect_make(p.x + p.w - px(a, 16) - px(a, 110), p.y + p.h - px(a, 36), px(a, 110), px(a, 28));
    Rect showall = rect_make(reset.x - px(a, 8) - px(a, 90), reset.y, px(a, 90), px(a, 28));
    if (ui_button(a, reset, "Reset layout", BTN_NORMAL, !grid_layout_default(g) || v->wrap) && input) {
        grid_layout_reset(g); v->wrap = false; measure_columns(v, a); v->rowy_dirty = true; layout_forget(a, v);
    }
    if (ui_button(a, showall, "Show all", BTN_NORMAL, grid_hidden_count(g) > 0) && input) { grid_col_show_all(g); v->rowy_dirty = true; layout_save(a, v); }
    if (input) {
        if (input_key(in, SDLK_ESCAPE, 0)) r->cp_open = false;
        if (in->mouse_pressed && !in->mouse_consumed && !rect_contains(p, in->mouse_x, in->mouse_y)) r->cp_open = false;
        in->mouse_consumed = true;
        in->nkeys = 0; in->ntext = 0;
    }
}

/* ---- export dialog (US-024) -------------------------------------------- */

/* Export formats as shown, and the FMT_* of each label. */
static const char *const fmt_labels[] = { "CSV", "TSV", "JSON", "Markdown", "SQL", "XLSX" };
static const int fmt_of_label[] = { FMT_CSV, FMT_TSV, FMT_JSON, FMT_MARKDOWN, FMT_SQL, FMT_XLSX };
/* Index of FMT_* fmt in fmt_labels (0 when unknown). */
static int label_of_fmt(int fmt) { for (int i = 0; i < 6; i++) if (fmt_of_label[i] == fmt) return i; return 0; }

/* extractors.txt in the app-data folder (US-027 / US-045): see extract.h. Entries run only
 * after the user approves them in the export dialog, in a sandbox, never through a shell. */
static void load_extractors(Results *r, App *a) {
    r->nextractors = a->conns ? extract_load(a->conns->dir, r->extractors, EXTRACT_MAX) : 0;
}

/* Segmented labels of the export dialog; the selected index is stored in r->ex_*. */
static const char *const scope_labels3[] = { "Selection", "Loaded rows", "Full result" };
static const char *const sep_labels[] = { "Comma", "Semicolon", "Tab", "Pipe" };
static const char *const quote_labels[] = { "As needed", "Always" };
static const char *const nl_labels[] = { "LF", "CRLF" };
static const char *const null_labels[] = { "Empty", "NULL", "\\N" };
static const char *const values_labels[] = { "Raw values", "Displayed values" };

/* Export dialog (US-024): format, rows (selection / loaded / full result), raw or displayed
 * values, format options (CSV / TSV), SQL table name, full-export limits, and custom extractors
 * with their approval. Captions say exactly what the file will contain. Export hands over to
 * app_export_results (save dialog), which later calls results_export. input = true handles
 * input, false draws; the height grows with the options shown. */
static void export_dialog(Results *r, ResultView *v, App *a, bool input) {
    SDL_Renderer *rr = a->renderer;
    Input *in = &a->in;
    Grid *g = &v->g;
    bool csv = r->ex_fmt == FMT_CSV, tsv = r->ex_fmt == FMT_TSV, sql = r->ex_fmt == FMT_SQL;
    float w = px(a, 560);
    float h = px(a, 320) + (csv || tsv ? px(a, 36 * (csv ? 5 : 3) + 30) : px(a, 36)) + (sql ? px(a, 36) : 0) + (r->ex_scope == 2 ? px(a, 36) : 0) + (r->nextractors ? px(a, 76) : 0);
    if (r->ex_extractor >= 0 && r->ex_extractor < r->nextractors && !r->extractors[r->ex_extractor].trusted) h += px(a, 92);
    Rect p = rect_make((a->width - w) / 2, (a->height - h) / 2, w, h);
    if (!input) {
        draw_rect(rr, rect_make(0, 0, (float)a->width, (float)a->height), color_alpha(color_hex(0x000000), 110));
        for (int sh = 3; sh >= 1; sh--)
            draw_rrect(rr, rect_make(p.x - sh * px(a, 2), p.y + sh * px(a, 2), p.w + sh * px(a, 4), p.h + sh * px(a, 2)), px(a, 10), color_alpha(color_hex(0x000000), 40));
        draw_rrect(rr, p, px(a, 10), color_hex(T_BORDER));
        draw_rrect(rr, rect_inset(p, px(a, 1), px(a, 1)), px(a, 9), color_hex(T_PANEL_ALT));
        draw_text_fit(a->font_ui, rr, "Export", -1, rect_make(p.x + px(a, 24), p.y + px(a, 14), px(a, 200), px(a, 26)), color_hex(T_TEXT), ALIGN_LEFT);
    }
    /* 1. format and rows, with the caption on how complete the file will be */
    float lx = p.x + px(a, 24), cx = p.x + px(a, 134), cw = w - px(a, 158), y = p.y + px(a, 52), rh = px(a, 36);
    int sel;
    /* how many rows the choice writes */
    int r0, c0, r1, c1;
    long nsel = grid_sel_rect(g, &r0, &c0, &r1, &c1) ? (long)(r1 - r0 + 1) : 0;
    long nrows = r->ex_scope == 0 ? nsel : r->ex_scope == 1 ? g->nvis : v->rs.nrows;
    char cnt[32];
    fmt_int(cnt, sizeof cnt, nrows);

    draw_text_fit(a->font_ui, rr, "Format", -1, rect_make(lx, y, px(a, 100), px(a, 28)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    if ((sel = ui_segmented(a, rect_make(cx, y, cw, px(a, 28)), fmt_labels, 6, label_of_fmt(r->ex_fmt))) >= 0 && input) r->ex_fmt = fmt_of_label[sel];
    y += rh;
    draw_text_fit(a->font_ui, rr, "Rows", -1, rect_make(lx, y, px(a, 100), px(a, 28)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    if ((sel = ui_segmented(a, rect_make(cx, y, cw, px(a, 28)), scope_labels3, 3, r->ex_scope)) >= 0 && input) r->ex_scope = sel;
    y += px(a, 30);
    /* caption right under the control it explains (US-025) */
    char note[220];
    int hidden = grid_hidden_count(g);
    if (r->ex_scope == 2) {
        if (v->rs.complete && !v->rs.cursor) snprintf(note, sizeof note, "Every row is already loaded: %s rows, source order, hidden columns excluded", cnt);
        else if (v->rs.cursor) snprintf(note, sizeof note, "%s loaded rows, then the open cursor streamed block by block (memory stays flat); source order, no local sort or filter; Esc cancels", cnt);
        else snprintf(note, sizeof note, "Cursor closed: re-run the query to export beyond the %s loaded rows; the result may differ between runs", cnt);
    } else if (r->ex_scope == 1) {
        char nl[32]; fmt_int(nl, sizeof nl, v->rs.nrows);
        if (v->rs.complete) snprintf(note, sizeof note, "The file will be complete: %s row%s in the current local sort and filter%s", cnt, nrows == 1 ? "" : "s", hidden ? ", hidden columns excluded" : "");
        else snprintf(note, sizeof note, "The file will be partial: %s row%s%s of %s loaded; the result has more rows on the server. Choose Full result to stream every row.", cnt, nrows == 1 ? "" : "s", g->nfilters ? " (local filter)" : "", nl);
    }
    else snprintf(note, sizeof note, nsel ? "%s selected rows, local sort applied" : "Nothing is selected: all %s visible rows are written", cnt);
    draw_wrapped(a->font_small, rr, note, rect_make(cx, y, cw, px(a, 40)), color_hex(r->ex_scope == 1 && !v->rs.complete ? T_YELLOW : T_TEXT_MUTED), 0, !input);
    y += px(a, 36);
    /* 2. raw or displayed values */
    draw_text_fit(a->font_ui, rr, "Values", -1, rect_make(lx, y, px(a, 100), px(a, 28)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    if ((sel = ui_segmented(a, rect_make(cx, y, px(a, 300), px(a, 28)), values_labels, 2, r->ex_values)) >= 0 && input) r->ex_values = sel;
    y += px(a, 30);
    char fdesc[160], vnote[260];
    describe_formats(a, fdesc, sizeof fdesc);
    if (r->ex_values == 1) snprintf(vnote, sizeof vnote, "As shown in the grid: %s", fdesc[0] ? fdesc : "no Format option is active, so the same as raw");
    else snprintf(vnote, sizeof vnote, "Exactly what the database returned%s", fdesc[0] ? " (the Format menu is ignored)" : "");
    draw_text_fit(a->font_small, rr, vnote, -1, rect_make(cx, y, cw, px(a, 20)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    y += px(a, 26);
    /* 3. options of the chosen format */
    if (csv || tsv) {
        draw_rect(rr, rect_make(lx, y + px(a, 4), w - px(a, 48), px(a, 1)), color_hex(T_BORDER));
        draw_text_fit(a->font_small, rr, csv ? "CSV OPTIONS" : "TSV OPTIONS", -1, rect_make(lx, y + px(a, 8), px(a, 200), px(a, 20)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        y += px(a, 30);
        if (csv) {
            draw_text_fit(a->font_ui, rr, "Delimiter", -1, rect_make(lx, y, px(a, 100), px(a, 28)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
            if ((sel = ui_segmented(a, rect_make(cx, y, px(a, 360), px(a, 28)), sep_labels, 4, r->ex_sep)) >= 0 && input) r->ex_sep = sel;
            y += rh;
            draw_text_fit(a->font_ui, rr, "Quotes", -1, rect_make(lx, y, px(a, 100), px(a, 28)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
            if ((sel = ui_segmented(a, rect_make(cx, y, px(a, 220), px(a, 28)), quote_labels, 2, r->ex_quote)) >= 0 && input) r->ex_quote = sel;
            y += rh;
        }
        draw_text_fit(a->font_ui, rr, "NULL as", -1, rect_make(lx, y, px(a, 100), px(a, 28)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        if ((sel = ui_segmented(a, rect_make(cx, y, px(a, 270), px(a, 28)), null_labels, 3, r->ex_null)) >= 0 && input) r->ex_null = sel;
        y += rh;
        draw_text_fit(a->font_ui, rr, "Line ending", -1, rect_make(lx, y, px(a, 100), px(a, 28)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        if ((sel = ui_segmented(a, rect_make(cx, y, px(a, 180), px(a, 28)), nl_labels, 2, r->ex_nl)) >= 0 && input) r->ex_nl = sel;
        y += rh;
        if (ui_checkbox(a, rect_make(cx, y, px(a, 130), px(a, 28)), "Header line", r->ex_headers, true) && input) r->ex_headers = !r->ex_headers;
        if (ui_checkbox(a, rect_make(cx + px(a, 150), y, px(a, 400), px(a, 28)), "Guard formulas (' before text starting = + - @)", r->ex_guard, true) && input) r->ex_guard = !r->ex_guard;
        y += rh;
    } else {
        draw_text_fit(a->font_ui, rr, "Line ending", -1, rect_make(lx, y, px(a, 100), px(a, 28)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        if ((sel = ui_segmented(a, rect_make(cx, y, px(a, 180), px(a, 28)), nl_labels, 2, r->ex_nl)) >= 0 && input) r->ex_nl = sel;
        y += rh;
    }
    if (sql) {
        draw_text_fit(a->font_ui, rr, "Table name", -1, rect_make(lx, y, px(a, 100), px(a, 28)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        if (ui_textfield(a, &r->ex_table, rect_make(cx, y, px(a, 300), px(a, 28)), "table_name", r->ex_focus == 0) && input) r->ex_focus = 0;
        y += rh;
    }
    /* 4. full-result limits */
    if (r->ex_scope == 2) {
        draw_text_fit(a->font_ui, rr, "Stop after", -1, rect_make(lx, y, px(a, 100), px(a, 28)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        if (ui_textfield(a, &r->ex_rows, rect_make(cx, y, px(a, 110), px(a, 28)), "rows", r->ex_focus == 1) && input) r->ex_focus = 1;
        draw_text_fit(a->font_small, rr, "rows", -1, rect_make(cx + px(a, 116), y, px(a, 36), px(a, 28)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        if (ui_textfield(a, &r->ex_mb, rect_make(cx + px(a, 152), y, px(a, 80), px(a, 28)), "MB", r->ex_focus == 2) && input) r->ex_focus = 2;
        draw_text_fit(a->font_small, rr, "MB", -1, rect_make(cx + px(a, 238), y, px(a, 30), px(a, 28)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        if (ui_textfield(a, &r->ex_secs, rect_make(cx + px(a, 268), y, px(a, 70), px(a, 28)), "s", r->ex_focus == 3) && input) r->ex_focus = 3;
        draw_text_fit(a->font_small, rr, "s (0 = none; hard cap 1 h / 16 GB; a limit keeps the partial file)", -1, rect_make(cx + px(a, 344), y, px(a, 240), px(a, 28)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        y += rh;
    }
    /* 5. custom extractors: pick one, review its sandbox permissions, approve or revoke */
    if (r->nextractors) {
        draw_text_fit(a->font_ui, rr, "Extractor", -1, rect_make(lx, y, px(a, 100), px(a, 28)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        const char *labels[EXTRACT_MAX + 1] = { "None" };
        for (int i = 0; i < r->nextractors; i++) labels[i + 1] = r->extractors[i].name;
        bool sandbox = extract_sandbox_available();
        if ((sel = ui_segmented(a, rect_make(cx, y, cw, px(a, 28)), labels, r->nextractors + 1, r->ex_extractor + 1)) >= 0 && input && sandbox) r->ex_extractor = sel - 1;
        y += px(a, 30);
        Extractor *e = r->ex_extractor >= 0 && r->ex_extractor < r->nextractors ? &r->extractors[r->ex_extractor] : NULL;
        if (!sandbox) draw_text_fit(a->font_small, rr, "No sandbox on this system (sandbox-exec / bubblewrap): extractors are disabled", -1, rect_make(cx, y, cw, px(a, 20)), color_hex(T_RED), ALIGN_LEFT);
        else if (!e) draw_wrapped(a->font_small, rr, "From extractors.txt: name, tab, program with {file} and {out} (absolute path, no shell). Pick one to review its permissions and approve it.", rect_make(cx, y, cw, px(a, 40)), color_hex(T_TEXT_DIM), 0, !input);
        else if (e->trusted) {
            char t[300];
            snprintf(t, sizeof t, "Approved  \xC2\xB7  sandboxed: reads the TSV, writes the output, no network, %d s limit  \xC2\xB7  %s", extract_default_limits(e).wall_secs, e->cmd);
            draw_text_fit(a->font_small, rr, t, -1, rect_make(cx, y, cw - px(a, 90), px(a, 20)), color_hex(T_GREEN), ALIGN_LEFT);
            if (ui_button(a, rect_make(cx + cw - px(a, 84), y - px(a, 4), px(a, 84), px(a, 26)), "Revoke", BTN_QUIET, true) && input) extract_trust(a->conns->dir, e, false);
        } else {
            char t[700];
            snprintf(t, sizeof t, "%s  \xC2\xB7  %s", e->origin, e->cmd);
            draw_text_fit(a->font_small, rr, t, -1, rect_make(cx, y, cw, px(a, 20)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
            extract_describe(e, t, sizeof t);
            /* the permissions text wraps to as many lines as it needs; the button goes below what
             * was actually drawn, so it never covers part of what the user is approving */
            float th = draw_wrapped(a->font_small, rr, t, rect_make(cx, y + px(a, 22), cw, px(a, 200)), color_hex(T_YELLOW), 0, !input);
            char lbl[80]; snprintf(lbl, sizeof lbl, "Approve \"%s\"", e->name);
            float by = y + px(a, 22) + th + px(a, 8);
            if (ui_button(a, rect_make(cx, by, px(a, 200), px(a, 26)), lbl, BTN_PRIMARY, true) && input) { if (!extract_trust(a->conns->dir, e, true)) app_status(a, "Could not record the approval"); }
            y = by + px(a, 6);
        }
        y += px(a, 46);
    }
    /* 6. Export / Cancel; Enter exports, Tab cycles the text fields */
    Rect bexp = rect_make(p.x + w - px(a, 24) - px(a, 170), p.y + h - px(a, 24) - px(a, 30), px(a, 170), px(a, 30));
    Rect bcan = rect_make(bexp.x - px(a, 8) - px(a, 84), bexp.y, px(a, 84), px(a, 30));
    bool can = r->ex_scope != 2 || v->rs.cursor || v->rs.complete;
    if (r->ex_extractor >= 0 && r->ex_extractor < r->nextractors && !r->extractors[r->ex_extractor].trusted) can = false;
    char blabel[64];
    if (r->ex_scope == 2 && v->rs.cursor) snprintf(blabel, sizeof blabel, "Export full result\xE2\x80\xA6");
    else snprintf(blabel, sizeof blabel, "Export %s row%s\xE2\x80\xA6", cnt, nrows == 1 ? "" : "s");
    if (ui_button(a, bexp, blabel, BTN_PRIMARY, can) && input) { r->ex_open = false; app_export_results(a, r->ex_scope); }
    if (ui_button(a, bcan, "Cancel", BTN_NORMAL, true) && input) r->ex_open = false;
    if (input) {
        if (input_key(in, SDLK_ESCAPE, 0)) r->ex_open = false;
        if (input_key(in, SDLK_RETURN, 0) && can) { r->ex_open = false; app_export_results(a, r->ex_scope); }
        if (input_key(in, SDLK_TAB, 0)) r->ex_focus = (r->ex_focus + 1) % 4;
        in->mouse_consumed = true;
        in->nkeys = 0; in->ntext = 0;
    }
}

/* ---- context menu ------------------------------------------------------ */

/* Runs a chosen menu item (CM_* id) from the context menu, its submenus, the tab menu, the
 * History menu, the toolbar "More" menu or the tab overflow list. Acts on the current tab and on
 * r->menu_row / menu_col / menu_tab recorded when the menu opened. Some items run SQL
 * synchronously (re-run, follow a foreign key), which may replace or evict tabs. */
static void menu_run(Results *r, App *a, int id) {
    ResultView *v = cur(r);
    /* 1. id ranges: tab overflow list, history entries, then the tab menu */
    if (id >= CM_TAB_GOTO_BASE && id < CM_TAB_GOTO_BASE + RES_MAX_TABS) {   /* a tab picked from the overflow list */
        r->cur = id - CM_TAB_GOTO_BASE; r->output_active = false; r->vv_open = r->fp_open = r->cp_open = false;
        return;
    }
    if (id >= CM_HIST_SAVED_BASE && id < CM_HIST_SAVED_BASE + 12) {
        int k = id - CM_HIST_SAVED_BASE;
        if (k < r->nsaved && r->saved[k].sql) {
            OutHistItem *h = &r->saved[k];
            editor_set_text(a->editor, h->sql); a->focus = FOCUS_EDITOR;
            bool mod = sql_modifies(h->sql);
            if (h->masked && mod) app_status(a, "Restored in the editor, not run: it changes data or schema, and secret literals were masked when it was saved; replace them and review before running");
            else if (h->masked) app_status(a, "Restored in the editor, not run: secret literals were masked when it was saved; replace them before running");
            else if (mod) app_status(a, "Restored in the editor, not run: it changes data or schema; review it, then run it yourself");
            else app_status(a, "Restored in the editor from %s (%s); not run", h->session, h->status);
        }
        return;
    }
    if (id >= CM_HIST_BASE && id < CM_HIST_BASE + HIST_MAX) {
        int i = id - CM_HIST_BASE;
        if (i >= 0 && i < r->nhist && r->hist[i].sql) { editor_set_text(a->editor, r->hist[i].sql); a->focus = FOCUS_EDITOR; app_status(a, "SQL of execution #%d restored in the editor", r->hist[i].id); }
        return;
    }
    if (id >= CM_TAB_CLOSE && id <= CM_TAB_OUTPUT) {
        int t = r->menu_tab;
        if (t < 0 || t >= r->ntabs) return;
        ResultView *tv = r->tabs[t];
        switch (id) {
        case CM_TAB_CLOSE: tab_close_request(r, a, t); break;
        case CM_TAB_OUTPUT: results_show_output(r, true); output_show_exec(a->output, a, tv->out_exec); break;
        case CM_TAB_CLOSE_OTHERS: case CM_TAB_CLOSE_ALL: {
            int kept = 0;
            for (int i = r->ntabs - 1; i >= 0; i--) {
                if ((id == CM_TAB_CLOSE_OTHERS && i == t) || r->tabs[i]->pinned) continue;
                if (tab_unsaved(r->tabs[i]) > 0) { kept++; continue; }
                tab_remove(r, a, i);
            }
            for (int i = 0; i < r->ntabs; i++) if (r->tabs[i] == tv) r->cur = i;
            if (kept) app_status(a, "%d tab%s with unsaved changes kept open; apply or discard them first", kept, kept == 1 ? "" : "s");
            break;
        }
        case CM_TAB_PIN: tv->pinned = !tv->pinned; app_status(a, tv->pinned ? "Tab pinned: it keeps its result and is never reused" : "Tab unpinned"); break;
        case CM_TAB_RENAME: r->rn_open = true; r->rn_tab = t; textfield_set(&r->rn, tv->title); r->rn.blink = SDL_GetTicks(); break;
        case CM_TAB_OPEN_SQL: if (tv->sql) { editor_set_text(a->editor, tv->sql); a->focus = FOCUS_EDITOR; app_status(a, "SQL of #%d opened in the editor", tv->exec_id); } break;
        case CM_TAB_RERUN:
            if (tv->sql) {
                if (!tv->db) { app_status(a, "The connection of this tab is closed; connect again to re-run"); break; }
                r->cur = t;
                app_run_sql_mode(a, tv->db, tv->sql, tv->sql_len, tv->title, RUN_REPLACE, tv->qtable[0] ? tv->qtable : NULL);
            }
            break;
        }
        return;
    }
    /* 2. items that act on the cells of the current tab */
    if (!live(v)) return;
    Grid *g = &v->g;
    switch (id) {
    case CM_COPY: copy_as(r, FMT_TSV, false); app_status(a, "Copied as TSV"); break;
    case CM_COPY_HDR: copy_as(r, FMT_TSV, true); app_status(a, "Copied as TSV with headers"); break;
    case CM_COPY_CSV: copy_as(r, FMT_CSV, true); app_status(a, "Copied as CSV"); break;
    case CM_COPY_JSON: copy_as(r, FMT_JSON, true); app_status(a, "Copied as JSON"); break;
    case CM_COPY_MD: copy_as(r, FMT_MARKDOWN, true); app_status(a, "Copied as Markdown"); break;
    case CM_COPY_SQL: copy_as(r, FMT_SQL, true); app_status(a, "Copied as SQL INSERT"); break;
    case CM_COPY_DISPLAYED: {
        ExportOpts o = { .fmt = FMT_TSV, .headers = true, .sep = '\t', .null_as = NULL_WORD, .displayed = true };
        char *t = grid_copy_opts(g, &o);
        if (t) { SDL_SetClipboardText(t); free(t); }
        app_status(a, "Copied displayed values (formatted); raw values are the default copy");
        break;
    }
    case CM_SORT_FILTER: chain_sort_filter(r, v); break;
    case CM_VIEW_MENU: chain_view(r, v); break;
    case CM_TB_FIND: case CM_TB_FILTER: case CM_TB_SORT: case CM_TB_COLUMNS: case CM_TB_EXPORT:
        toolbar_action(r, v, a, id, rect_make(r->menu.x, r->menu.y, 0, 0)); break;
    case CM_COPY_COLNAME: if (r->menu_col >= 0) { SDL_SetClipboardText(grid_col_name(g, r->menu_col)); app_status(a, "Column name copied"); } break;
    case CM_SORT_ASC: case CM_SORT_DESC:
        if (r->menu_col >= 0) {
            grid_sort_clear(g);
            grid_sort_click(g, r->menu_col, false);
            if (id == CM_SORT_DESC) grid_sort_click(g, r->menu_col, false);
            v->rowy_dirty = true;
        }
        break;
    case CM_VIEW: open_viewer(r, v, r->menu_row, r->menu_col); break;
    case CM_RECORD:
        if (g->sel_r < 0 && r->menu_row >= 0) grid_select_cell(g, r->menu_row, r->menu_col < 0 ? 0 : r->menu_col, false);
        if (r->panels_external) { r->inspector_toggle = true; break; }   /* the Inspector panel shows the record */
        v->rv_open = !v->rv_open;
        break;
    case CM_FILTER_EQ: {
        const char *val = grid_cell(g, r->menu_row, r->menu_col);
        if (val) grid_filter_add(g, r->menu_col, OP_EQUALS, val, NULL); else grid_filter_add(g, r->menu_col, OP_IS_NULL, NULL, NULL);
        v->rowy_dirty = true;
        break;
    }
    case CM_FILTER_COL: open_filter(r, v, r->menu_col, NULL, false); break;
    case CM_FILTER_CLEAR: grid_filter_clear(g); v->rowy_dirty = true; break;
    case CM_SORT_CLEAR: grid_sort_clear(g); v->rowy_dirty = true; break;
    case CM_HIDE_COL: if (r->menu_col >= 0) { grid_col_hide(g, r->menu_col); v->rowy_dirty = true; layout_save(a, v); } break;
    case CM_SHOW_ALL: grid_col_show_all(g); v->rowy_dirty = true; layout_save(a, v); break;
    case CM_FREEZE: if (r->menu_col >= 0) { grid_freeze(g, r->menu_col + 1); v->scroll_x = 0; layout_save(a, v); } break;
    case CM_UNFREEZE: grid_freeze(g, 0); layout_save(a, v); break;
    case CM_WRAP: v->wrap = !v->wrap; v->rowy_dirty = true; layout_save(a, v); break;
    case CM_COLUMNS: r->cp_open = true; r->cp_scroll = 0; break;
    case CM_RESET_LAYOUT: grid_layout_reset(g); v->wrap = false; measure_columns(v, a); v->rowy_dirty = true; layout_forget(a, v); break;
    case CM_SRV_ASC: case CM_SRV_DESC:
        if (r->menu_col >= 0) { snprintf(v->srv_sort, sizeof v->srv_sort, "%s", grid_col_name(g, r->menu_col)); v->srv_desc = id == CM_SRV_DESC; request_rerun(r); }
        break;
    case CM_SRV_CLEAR: v->srv_sort[0] = 0; v->srv_where[0] = 0; v->srv_where_text[0] = 0; request_rerun(r); break;
    case CM_SRV_FILTER: open_filter(r, v, r->menu_col, NULL, true); break;
    case CM_EXPORT_VISIBLE: results_export_dialog(r, a, 1); break;
    case CM_EXPORT_SEL: results_export_dialog(r, a, 0); break;
    case CM_EXPORT_FULL: results_export_dialog(r, a, 2); break;
    case CM_EXPORT_DLG: results_export_dialog(r, a, g->sel_r >= 0 && (g->anc_r != g->sel_r || g->anc_c != g->sel_c || g->sel_mode != SEL_CELLS) ? 0 : 1); break;
    case CM_COPY_AS: chain_copy_as(r, a); break;
    case CM_ANALYSE: chain_analyse(r, v); break;
    case CM_MORE_EDITS: chain_more_edits(r, v); break;
    case CM_COMPARE_PICK: chain_compare(r, v); break;
    case CM_EDIT: cell_edit_open(r, v, r->menu_row, r->menu_col); break;
    case CM_SET_NULL: if (r->menu_row >= 0) { edit_put(&v->ed, g->order[r->menu_row], grid_col(g, r->menu_col), NULL, true); } break;
    case CM_SET_DEFAULT: if (r->menu_row >= 0) edit_remove(&v->ed, g->order[r->menu_row], grid_col(g, r->menu_col)); break;
    case CM_REVERT_CELL: if (r->menu_row >= 0) edit_remove(&v->ed, g->order[r->menu_row], grid_col(g, r->menu_col)); break;
    case CM_ADD_ROW: row_add(v, -1); grid_select_cell(g, g->nvis - 1, 0, false); ensure_visible(r, v, g->nvis - 1, 0); break;
    case CM_DUP_ROW: if (r->menu_row >= 0) { row_add(v, g->order[r->menu_row]); grid_select_cell(g, g->nvis - 1, 0, false); } break;
    case CM_DEL_ROW: if (r->menu_row >= 0) row_toggle_delete(v, g->order[r->menu_row]); break;
    case CM_REVIEW: edit_build(&v->ed); r->rv_review = true; r->review_scroll = 0; break;
    case CM_DISCARD: edits_clear(v); edit_stmts_free(&v->ed); app_status(a, "Pending changes discarded"); break;
    case CM_TRANSPOSE: {
        int r0, c0, r1, c1;
        if (!grid_sel_rect(g, &r0, &c0, &r1, &c1)) { r0 = 0; r1 = g->nvis - 1; }
        if (r1 - r0 + 1 > 200) { r1 = r0 + 199; app_status(a, "Transposed the first 200 selected rows"); }
        ResultSet t; grid_transpose(g, r0, r1, &t);
        char title[140]; snprintf(title, sizeof title, "%s (transposed)", v->title);
        char conn[64]; snprintf(conn, sizeof conn, "%s", v->conn);
        add_derived(r, a, &t, title, conn);
        break;
    }
    case CM_CHART: r->chart_open = true; break;
    case CM_OPEN_LINK: {
        const char *val = grid_cell(g, r->menu_row, r->menu_col);
        if (cell_is_link(val)) { if (SDL_OpenURL(val)) app_status(a, "Opened in the browser"); else app_status(a, "Could not open the link: %s", SDL_GetError()); }
        break;
    }
    /* 3. range ids: follow a foreign key, compare with another tab */
    default:
        if (id >= CM_FK_BASE && id < CM_FK_BASE + 20 && v->fks && id - CM_FK_BASE < v->nfks && r->menu_row >= 0) {
            /* every column of the constraint goes into the WHERE; values are quoted
             * literals through the same escaper as server filters (the drivers have no bind
             * API; nothing from the cell is ever spliced in unquoted) */
            int first = id - CM_FK_BASE;
            DbFk *fk = &v->fks[first];
            if (!v->db) break;
            char q[300], qc[300], lit[600], where[1400] = "", text[400] = "", sql[2200];
            bool ok = true;
            for (int k = first; k < v->nfks && strcmp(v->fks[k].name, fk->name) == 0; k++) {
                int src = -1;
                for (int c = 0; c < v->rs.ncols; c++) if (strcasecmp(v->rs.cols[c], v->fks[k].from_col) == 0) { src = c; break; }
                const char *cv = src >= 0 ? rs_cell(&v->rs, g->order[r->menu_row], src) : NULL;
                if (!cv) { ok = false; break; }
                v->db->drv->quote_ident(v->fks[k].to_col, qc, sizeof qc);
                sql_literal(cv, v->g.numeric[src], lit, sizeof lit);
                size_t l = strlen(where), t = strlen(text);
                snprintf(where + l, sizeof where - l, "%s%s = %s", l ? " AND " : "", qc, lit);
                snprintf(text + t, sizeof text - t, "%s%s = %s", t ? ", " : "", v->fks[k].to_col, cv);
            }
            if (!ok) { app_status(a, "Cannot follow %s: a key column is NULL or missing from the result", fk->name); break; }
            v->db->drv->quote_ident(fk->to_table, q, sizeof q);
            v->db->drv->select_sql(q, where, NULL, a->fetch_size, sql, sizeof sql);
            Db *db = v->db;
            /* runs on the execution worker: the new tab is set up when it lands (fk_landed) */
            FkNav *fn = calloc(1, sizeof *fn);
            if (!fn) break;
            snprintf(fn->to_table, sizeof fn->to_table, "%s", fk->to_table); snprintf(fn->where, sizeof fn->where, "%s", where); snprintf(fn->text, sizeof fn->text, "%s", text);
            fn->r = r;
            OutRunOpts op = { .mode = RUN_NEW_TAB, .title = fn->to_table, .qtable = q, .keep_tabs = true, .no_history = true, .landed = fk_landed, .ud = fn };
            if (!output_run_opts(a->output, a, db, sql, -1, 0, -1, &op)) free(fn);
            v = NULL; fk = NULL;
        } else if (id >= CM_COMPARE_BASE && id < CM_COMPARE_BASE + RES_MAX_TABS) {
            int t = id - CM_COMPARE_BASE;
            if (t < 0 || t >= r->ntabs || !live(r->tabs[t])) break;
            ResultView *o = r->tabs[t];
            int key_a = r->menu_col >= 0 ? r->menu_col : 0, key_b = -1;
            for (int c = 0; c < o->g.ncols; c++) if (strcmp(grid_col_name(&o->g, c), grid_col_name(g, key_a)) == 0) key_b = c;
            if (key_b < 0) { app_status(a, "Column %s does not exist in #%d; right-click the key column to compare on", grid_col_name(g, key_a), o->exec_id); break; }
            ResultSet d; CompareStats st;
            grid_compare(g, &o->g, key_a, key_b, &d, &st);
            char title[140]; snprintf(title, sizeof title, "A #%d vs B #%d on %s", v->exec_id, o->exec_id, grid_col_name(g, key_a));
            char conn[64]; snprintf(conn, sizeof conn, "%s", v->conn);
            bool partial = v->rs.cursor || v->rs.cursor_lost || o->rs.cursor || o->rs.cursor_lost;
            bool filtered = v->g.nfilters > 0 || o->g.nfilters > 0;
            { size_t tl = strlen(title); snprintf(title + tl, sizeof title - tl, "%s%s", partial ? " \xC2\xB7 partial" : "", filtered ? " \xC2\xB7 filtered" : ""); }
            /* the new tab may evict the oldest one, which can be A or B: keep what is needed afterwards */
            bool pa = v->rs.cursor || v->rs.cursor_lost, pb = o->rs.cursor || o->rs.cursor_lost;
            int exec_a = v->exec_id, exec_b = o->exec_id;
            char key_name[128]; snprintf(key_name, sizeof key_name, "%s", grid_col_name(g, key_a));
            add_derived(r, a, &d, title, conn);
            v = o = NULL; g = NULL;
            /* US-051: a partial side makes "only in" unreliable; say which side and what it means,
             * in the tab itself (the status bar message disappears after a few seconds) */
            ResultView *dv = cur(r);
            if (dv && (partial || filtered)) {
                char who[48];
                if (pa && pb) snprintf(who, sizeof who, "A #%d and B #%d are", exec_a, exec_b);
                else if (pa) snprintf(who, sizeof who, "A #%d is", exec_a);
                else snprintf(who, sizeof who, "B #%d is", exec_b);
                if (partial) snprintf(dv->caveat, sizeof dv->caveat, "%s partial (loaded rows only): \"only in %s\" rows may exist in rows not fetched%s", who,
                                      pa && pb ? "A / B" : pa ? "B" : "A", filtered ? "; local filter active" : "");
                else snprintf(dv->caveat, sizeof dv->caveat, "a local filter is active: only visible rows were compared");
            }
            app_status(a, "Compared on %s: %d same, %d changed, %d only in #%d, %d only in #%d%s%s%s", key_name, st.same, st.changed, st.only_a, exec_a, st.only_b, exec_b,
                       st.keys_unique ? "" : "; keys are not unique, rows were paired in order", partial ? "; at least one result is partial" : "", filtered ? "; a local filter is active on one side: only its visible rows were compared" : "");
        }
        break;
    }
}

/* Opens the cell or header context menu at the mouse pointer. */
static void context_menu_open(Results *r, ResultView *v, App *a, int row, int col, bool on_header) {
    context_menu_open_at(r, v, a, row, col, on_header, a->in.mouse_x, a->in.mouse_y);
}

/* Cell menu (US-054): what acts on the cell first (copy, view, record, link, keys, edit),
 * then one entry per group (sort & filter, view, analysis, export) that opens a chained
 * level; the header menu keeps its column-centric groups. */
static void context_menu_open_at(Results *r, ResultView *v, App *a, int row, int col, bool on_header, float mx, float my) {
    Grid *g = &v->g;
    r->menu_row = row; r->menu_col = col; r->menu_tab = -1;
    popup_open(&r->menu, mx, my);
    bool cell = row >= 0 && col >= 0 && !on_header;
    bool sel = g->sel_r >= 0;
    if (cell && !grid_in_sel(g, row, col)) grid_select_cell(g, row, col, false), sel = true;
    bool srv_sort = server_capable(v, DB_CAP_SERVER_SORT), srv_filter = server_capable(v, DB_CAP_SERVER_FILTER);
    /* header menu: column caption, local sort / filter, server section, layout */
    if (on_header) {
        /* captions make local vs server unmistakable: the server section re-runs the query */
        if (col >= 0) {
            snprintf(r->menu_text[0], sizeof r->menu_text[0], "%s  \xC2\xB7  %s", grid_col_name(g, col), grid_col_type(g, col)[0] ? grid_col_type(g, col) : "no declared type");
            popup_add_caption(&r->menu, r->menu_text[0], T_TEXT_MUTED, false);
        }
        popup_add(&r->menu, CM_COPY_COLNAME, "Copy Column Name", false, col >= 0);
        popup_add_caption(&r->menu, "LOADED ROWS (LOCAL)", 0, true);
        popup_add(&r->menu, CM_SORT_ASC, "Sort Ascending", false, col >= 0);
        popup_add(&r->menu, CM_SORT_DESC, "Sort Descending", false, col >= 0);
        popup_add(&r->menu, CM_FILTER_COL, "Filter Column\xE2\x80\xA6", false, col >= 0);
        if (g->nsort > 0) popup_add(&r->menu, CM_SORT_CLEAR, "Clear Local Sort", false, true);
        if (g->nfilters > 0) popup_add(&r->menu, CM_FILTER_CLEAR, "Clear Local Filters", false, true);
        if (srv_sort || srv_filter) {
            popup_add_caption(&r->menu, "SERVER  \xC2\xB7  RE-RUNS THE QUERY", T_GREEN, true);
            if (srv_sort) {
                popup_add(&r->menu, CM_SRV_ASC, "Sort on Server, Ascending", false, col >= 0);
                popup_add(&r->menu, CM_SRV_DESC, "Sort on Server, Descending", false, col >= 0);
            }
            if (srv_filter) popup_add(&r->menu, CM_SRV_FILTER, "Filter on Server\xE2\x80\xA6", false, col >= 0);
            if (v->srv_sort[0] || v->srv_where[0]) popup_add(&r->menu, CM_SRV_CLEAR, "Clear Server Sort and Filter", false, true);
        }
        popup_add_caption(&r->menu, "LAYOUT", 0, true);
        popup_add(&r->menu, CM_HIDE_COL, "Hide Column", false, col >= 0 && g->ncols > 1);
        if (grid_hidden_count(g) > 0) popup_add(&r->menu, CM_SHOW_ALL, "Show All Columns", false, true);
        if (col >= 0 && col + 1 == g->frozen) popup_add(&r->menu, CM_UNFREEZE, "Unfreeze Columns", false, true);
        else popup_add(&r->menu, CM_FREEZE, "Freeze Columns Up to Here", false, col >= 0 && col < g->ncols - 1);
        popup_add(&r->menu, CM_WRAP, v->wrap ? "Single-line Rows" : "Wrap Text in Rows", false, true);
        popup_add(&r->menu, CM_COLUMNS, "Columns\xE2\x80\xA6", false, true);
        if (!grid_layout_default(g) || v->wrap) popup_add(&r->menu, CM_RESET_LAYOUT, "Reset Layout", false, true);
        return;
    }
    /* cell menu: copy, view, record, link and foreign keys */
    const char *what = sel && (g->sel_mode != SEL_CELLS || g->anc_r != g->sel_r || g->anc_c != g->sel_c) ? "Copy Selection" : "Copy Cell";
    popup_add(&r->menu, CM_COPY, what, false, sel || cell);
    popup_add(&r->menu, CM_COPY_HDR, "Copy with Headers", false, sel || cell);
    popup_add(&r->menu, CM_COPY_AS, "Copy as  \xE2\x80\xBA", false, true);
    popup_add(&r->menu, CM_VIEW, "View Value", true, cell);
    popup_add(&r->menu, CM_RECORD, (r->panels_external ? r->inspector_shown : v->rv_open) ? "Hide Record View" : "Show Record View", false, true);
    if (cell) {
        const char *val = grid_cell(g, row, col);
        if (cell_is_link(val)) popup_add(&r->menu, CM_OPEN_LINK, "Open Link in Browser", true, true);
        load_fks(v);
        /* one item per constraint that includes this column; a composite key needs every
         * one of its columns in the result, with a value (NULL never references) */
        int nfk = 0;
        for (int i = 0; i < v->nfks && nfk < 8; i++) {
            if (i > 0 && strcmp(v->fks[i].name, v->fks[i - 1].name) == 0) continue;   /* not the first column of its key */
            bool mine = false, complete = true;
            char lbl[160] = "";
            for (int k = i; k < v->nfks && strcmp(v->fks[k].name, v->fks[i].name) == 0; k++) {
                int src = -1;
                for (int c = 0; c < v->rs.ncols; c++) if (strcasecmp(v->rs.cols[c], v->fks[k].from_col) == 0) { src = c; break; }
                const char *cv = src >= 0 ? rs_cell(&v->rs, g->order[row], src) : NULL;
                if (strcasecmp(v->fks[k].from_col, grid_col_name(g, col)) == 0) mine = true;
                if (!cv) { complete = false; break; }
                size_t l = strlen(lbl);
                snprintf(lbl + l, sizeof lbl - l, "%s%s = %s", l ? " and " : "", v->fks[k].to_col, cv);
            }
            if (!mine) continue;
            if (!v->fks[i].to_table[0]) {   /* the driver could not see the referenced table (no privilege) */
                snprintf(r->menu_text[8 + nfk], sizeof r->menu_text[0], "%s: referenced table not visible to this user", v->fks[i].name);
                popup_add(&r->menu, CM_FK_BASE + i, r->menu_text[8 + nfk], nfk == 0, false);
                nfk++;
                continue;
            }
            if (complete) snprintf(r->menu_text[8 + nfk], sizeof r->menu_text[0], "Go to %s where %s", v->fks[i].to_table, lbl);
            else snprintf(r->menu_text[8 + nfk], sizeof r->menu_text[0], "Go to %s (needs every key column, non-NULL)", v->fks[i].to_table);
            popup_add(&r->menu, CM_FK_BASE + i, r->menu_text[8 + nfk], nfk == 0, complete);
            nfk++;
        }
    }
    /* edit section (or the read-only reason), then the chained groups */
    if (v->edit_ok) {
        int pend = edit_pending_count(&v->ed);
        int prow = edit_pending_rows(&v->ed);
        snprintf(r->menu_text[1], sizeof r->menu_text[0], "EDIT  \xC2\xB7  %d UNSAVED ROW%s", prow, prow == 1 ? "" : "S");
        popup_add_caption(&r->menu, pend ? r->menu_text[1] : "EDIT  \xC2\xB7  SAVED ONLY WHEN APPLIED", T_YELLOW, true);
        int src_row = row >= 0 ? g->order[row] : -1;
        const char *ro = cell ? edit_col_readonly(&v->ed, grid_col(g, col)) : NULL;
        if (ro) { snprintf(r->menu_text[3], sizeof r->menu_text[0], "%s is read-only: %s", grid_col_name(g, col), ro); popup_add_caption(&r->menu, r->menu_text[3], T_TEXT_DIM, false); }
        popup_add(&r->menu, CM_EDIT, "Edit Cell", false, cell && !ro && !edit_row_deleted(&v->ed, src_row));
        popup_add(&r->menu, CM_SET_NULL, "Set NULL", false, cell && !ro && !edit_row_deleted(&v->ed, src_row));
        popup_add(&r->menu, CM_ADD_ROW, "Add Row", false, true);
        popup_add(&r->menu, CM_DEL_ROW, row >= 0 && edit_row_deleted(&v->ed, src_row) ? "Undelete Row" : "Delete Row", false, row >= 0);
        popup_add(&r->menu, CM_MORE_EDITS, "More edits  \xE2\x80\xBA", false, row >= 0);
        snprintf(r->menu_text[2], sizeof r->menu_text[0], "Review and apply\xE2\x80\xA6 (%d row%s)", prow, prow == 1 ? "" : "s");
        popup_add(&r->menu, CM_REVIEW, r->menu_text[2], false, pend > 0);
    } else {
        popup_add_caption(&r->menu, "READ-ONLY", T_TEXT_DIM, true);
        snprintf(r->menu_text[1], sizeof r->menu_text[0], "%s", v->edit_why);
        popup_add_caption(&r->menu, r->menu_text[1], 0, false);
    }
    char sf[64];
    snprintf(sf, sizeof sf, "Sort & Filter%s%s  \xE2\x80\xBA", g->nsort || g->nfilters ? "  \xC2\xB7  " : "", g->nfilters ? "filtered" : g->nsort ? "sorted" : "");
    snprintf(r->menu_text[4], sizeof r->menu_text[0], "%s", sf);
    popup_add(&r->menu, CM_SORT_FILTER, r->menu_text[4], true, true);
    popup_add(&r->menu, CM_VIEW_MENU, "View  \xE2\x80\xBA", false, true);
    popup_add(&r->menu, CM_ANALYSE, "Chart, Transpose, Compare  \xE2\x80\xBA", false, true);
    {
        int pend_rows = v->ed_init ? edit_pending_rows(&v->ed) : 0;
        if (pend_rows) snprintf(r->menu_text[5], sizeof r->menu_text[0], "Export\xE2\x80\xA6  (off: %d unsaved row%s)", pend_rows, pend_rows == 1 ? "" : "s");
        popup_add(&r->menu, CM_EXPORT_DLG, pend_rows ? r->menu_text[5] : "Export\xE2\x80\xA6", false, !pend_rows);
    }
}

/* Cell-menu submenu "Sort & Filter": local sort / filter on the clicked column, then the server
 * section when the tab can re-run on the server. */
static void chain_sort_filter(Results *r, ResultView *v) {
    Grid *g = &v->g;
    int col = r->menu_col, row = r->menu_row;
    bool cell = row >= 0 && col >= 0;
    PopupMenu *m = popup_open_sub(&r->menu, &r->submenu);   /* cascades beside the clicked item */
    if (col >= 0) { snprintf(r->sub_text[0], sizeof r->sub_text[0], "%s  \xC2\xB7  %s", grid_col_name(g, col), grid_col_type(g, col)[0] ? grid_col_type(g, col) : "no declared type"); popup_add_caption(m, r->sub_text[0], T_TEXT_MUTED, false); }
    char nr[32]; fmt_int(nr, sizeof nr, v->rs.nrows);
    snprintf(r->sub_text[5], sizeof r->sub_text[0], "LOCAL  \xC2\xB7  %s LOADED ROW%s%s", nr, v->rs.nrows == 1 ? "" : "S", v->rs.complete ? "" : ", PARTIAL");
    popup_add_caption(m, r->sub_text[5], 0, col >= 0);
    popup_add(m, CM_SORT_ASC, "Sort Ascending", false, col >= 0);
    popup_add(m, CM_SORT_DESC, "Sort Descending", false, col >= 0);
    popup_add(m, CM_FILTER_EQ, "Filter by This Value", false, cell);
    popup_add(m, CM_FILTER_COL, "Filter Column\xE2\x80\xA6", false, col >= 0);
    if (g->nsort > 0) popup_add(m, CM_SORT_CLEAR, "Clear Local Sort", false, true);
    if (g->nfilters > 0) popup_add(m, CM_FILTER_CLEAR, "Clear Local Filters", false, true);
    bool srv_sort = server_capable(v, DB_CAP_SERVER_SORT), srv_filter = server_capable(v, DB_CAP_SERVER_FILTER);
    if (srv_sort || srv_filter) {
        popup_add_caption(m, "SERVER  \xC2\xB7  RE-RUNS THE QUERY", T_GREEN, true);
        if (srv_sort) { popup_add(m, CM_SRV_ASC, "Sort on Server, Ascending", false, col >= 0); popup_add(m, CM_SRV_DESC, "Sort on Server, Descending", false, col >= 0); }
        if (srv_filter) popup_add(m, CM_SRV_FILTER, "Filter on Server\xE2\x80\xA6", false, col >= 0);
        if (v->srv_sort[0] || v->srv_where[0]) popup_add(m, CM_SRV_CLEAR, "Clear Server Sort and Filter", false, true);
    } else if (!v->qtable[0]) popup_add_caption(m, "SERVER  \xC2\xB7  ONLY FOR TABLE PREVIEWS", T_TEXT_DIM, true);
}

/* Cell-menu submenu "View": hide / show / freeze columns, wrap, record view, columns panel. */
static void chain_view(Results *r, ResultView *v) {
    Grid *g = &v->g;
    int col = r->menu_col;
    PopupMenu *m = popup_open_sub(&r->menu, &r->submenu);   /* cascades beside the clicked item */
    popup_add_caption(m, "VIEW  \xC2\xB7  LAYOUT ONLY, DATA UNCHANGED", 0, false);
    popup_add(m, CM_HIDE_COL, "Hide Column", false, col >= 0 && g->ncols > 1);
    if (grid_hidden_count(g) > 0) popup_add(m, CM_SHOW_ALL, "Show All Columns", false, true);
    if (col >= 0 && col + 1 == g->frozen) popup_add(m, CM_UNFREEZE, "Unfreeze Columns", false, true);
    else popup_add(m, CM_FREEZE, "Freeze Columns Up to Here", false, col >= 0 && col < g->ncols - 1);
    popup_add(m, CM_WRAP, v->wrap ? "Single-line Rows" : "Wrap Text in Rows", false, true);
    popup_add(m, CM_RECORD, (r->panels_external ? r->inspector_shown : v->rv_open) ? "Hide Record View" : "Show Record View", false, true);
    popup_add(m, CM_COLUMNS, "Columns\xE2\x80\xA6", true, true);
    if (!grid_layout_default(g) || v->wrap) popup_add(m, CM_RESET_LAYOUT, "Reset Layout", false, true);
}

/* Cell-menu submenu "Chart, Transpose, Compare"; Compare needs another live tab with rows. */
static void chain_analyse(Results *r, ResultView *v) {
    PopupMenu *m = popup_open_sub(&r->menu, &r->submenu);   /* cascades beside the clicked item */
    popup_add_caption(m, "LOADED ROWS, COMPUTED LOCALLY", 0, false);
    popup_add(m, CM_CHART, "Chart Selection\xE2\x80\xA6", false, true);
    popup_add(m, CM_TRANSPOSE, "Transpose Selected Rows", false, true);
    int ncmp = 0;
    for (int i = 0; i < r->ntabs; i++) { ResultView *o = r->tabs[i]; if (o != v && live(o) && o->rs.has_rows) ncmp++; }
    popup_add(m, CM_COMPARE_PICK, "Compare with Another Tab  \xE2\x80\xBA", false, ncmp > 0);
}

/* Submenus of the cell menu: each fills the cascading level opened beside its item (ui.h). */
/* "Copy as": CSV, JSON, Markdown, SQL INSERT, displayed values, column name. */
static void chain_copy_as(Results *r, App *a) {
    PopupMenu *m = popup_open_sub(&r->menu, &r->submenu);   /* cascades beside the clicked item */
    popup_add_caption(m, "COPY AS", 0, false);
    popup_add(m, CM_COPY_CSV, "CSV", false, true);
    popup_add(m, CM_COPY_JSON, "JSON", false, true);
    popup_add(m, CM_COPY_MD, "Markdown", false, true);
    popup_add(m, CM_COPY_SQL, "SQL INSERT", false, true);
    popup_add(m, CM_COPY_DISPLAYED, formats_active(a) ? "TSV, displayed values (Format menu)" : "TSV, displayed values (no format active: same as raw)", true, true);
    popup_add(m, CM_COPY_COLNAME, "Column Name", true, r->menu_col >= 0);
}

/* Cell-menu submenu "More edits": set DEFAULT (new rows), revert cell, duplicate row, discard all. */
static void chain_more_edits(Results *r, ResultView *v) {
    Grid *g = &v->g;
    int src_row = r->menu_row >= 0 ? g->order[r->menu_row] : -1;
    bool cell = r->menu_row >= 0 && r->menu_col >= 0;
    PopupMenu *m = popup_open_sub(&r->menu, &r->submenu);   /* cascades beside the clicked item */
    popup_add_caption(m, "MORE EDITS  \xC2\xB7  PENDING UNTIL APPLIED", T_YELLOW, false);
    popup_add(m, CM_SET_DEFAULT, "Set DEFAULT (new rows only)", false, cell && edit_row_new(&v->ed, src_row));
    popup_add(m, CM_REVERT_CELL, "Revert Cell", false, cell && edit_find(&v->ed, src_row, grid_col(g, r->menu_col)) != NULL);
    popup_add(m, CM_DUP_ROW, "Duplicate Row (keys take DEFAULT)", false, r->menu_row >= 0);
    popup_add(m, CM_DISCARD, "Discard All Changes", true, edit_pending_count(&v->ed) > 0);
}

/* Third-level submenu: the tabs to compare with on the clicked column (up to 10); tabs without
 * a column of that name are listed disabled. */
static void chain_compare(Results *r, ResultView *v) {
    PopupMenu *m = popup_open_sub(&r->submenu, &r->submenu2);   /* third level: beside "Compare with Another Tab" */
    snprintf(r->sub_text[3], sizeof r->sub_text[0], "COMPARE ON %s WITH", r->menu_col >= 0 ? grid_col_name(&v->g, r->menu_col) : grid_col_name(&v->g, 0));
    popup_add_caption(m, r->sub_text[3], 0, false);
    const char *key = r->menu_col >= 0 ? grid_col_name(&v->g, r->menu_col) : grid_col_name(&v->g, 0);
    int ncmp = 0;
    for (int i = 0; i < r->ntabs && ncmp < 10; i++) {
        ResultView *o = r->tabs[i];
        if (o == v || !live(o) || !o->rs.has_rows) continue;
        bool has = false;
        for (int c = 0; c < o->g.ncols; c++) if (strcmp(grid_col_name(&o->g, c), key) == 0) has = true;
        char nr[32]; fmt_int(nr, sizeof nr, o->rs.nrows);
        snprintf(r->sub_text[16 + ncmp], sizeof r->sub_text[0], "#%d %s  \xC2\xB7  %s rows%s", o->exec_id, o->title, nr, has ? "" : ", no such column");
        popup_add(m, CM_COMPARE_BASE + i, r->sub_text[16 + ncmp], false, has);
        ncmp++;
    }
}

/* Right-click menu of tab t: close / close others / close all, pin, rename, open SQL, re-run,
 * show execution output. */
static void tab_menu_open(Results *r, App *a, int t) {
    ResultView *tv = r->tabs[t];
    r->menu_tab = t; r->menu_row = r->menu_col = -1;
    popup_open(&r->menu, a->in.mouse_x, a->in.mouse_y);
    popup_add(&r->menu, CM_TAB_CLOSE, "Close Tab", false, true);
    popup_add(&r->menu, CM_TAB_CLOSE_OTHERS, "Close Other Tabs", false, r->ntabs > 1);
    popup_add(&r->menu, CM_TAB_CLOSE_ALL, "Close All Tabs", false, true);
    popup_add(&r->menu, CM_TAB_PIN, tv->pinned ? "Unpin Tab" : "Pin Tab", true, true);
    popup_add(&r->menu, CM_TAB_RENAME, "Rename Tab\xE2\x80\xA6", false, true);
    popup_add(&r->menu, CM_TAB_OPEN_SQL, "Open SQL in Editor", true, tv->sql != NULL);
    popup_add(&r->menu, CM_TAB_RERUN, "Re-run", false, tv->sql != NULL && tv->db != NULL);
    popup_add(&r->menu, CM_TAB_OUTPUT, "Show Execution Output", true, tv->out_exec > 0);
}

/* ---- header / tabs / toolbar ------------------------------------------- */


/* Meta line above the grid: RESULTS / ERROR label, provenance (#id, time, connection, pinned),
 * a stale or discarded note, and on the right the execution info: rows loaded (a total only
 * when the cursor is exhausted), local scope, columns, pending rows, exec / fetch time and
 * memory. v may be NULL (no tab). */
static void draw_header(Results *r, ResultView *v, App *a, Rect hdr) {
    SDL_Renderer *rr = a->renderer;
    bool is_err = v && v->has && !v->discarded && !v->rs.ok;
    draw_rect(rr, hdr, color_hex(T_PANEL_ALT));
    if (is_err) draw_rect(rr, hdr, color_alpha(color_hex(T_RED), 28));
    draw_rect(rr, rect_make(hdr.x, hdr.y + hdr.h - px(a, 1), hdr.w, px(a, 1)), color_hex(T_BORDER));
    float hx = hdr.x + px(a, 14);
    const char *label = is_err ? "ERROR" : "RESULTS";
    float lw = font_width(a->font_small, label, -1);
    draw_text_fit(a->font_small, rr, label, -1, rect_make(hx, hdr.y, lw + px(a, 2), hdr.h), color_hex(is_err ? T_RED : T_TEXT_MUTED), ALIGN_LEFT);
    hx += lw + px(a, 12);
    if (v && v->has) {
        /* provenance (US-031): execution id, time and connection, then any staleness note */
        struct tm tm; localtime_r(&v->when, &tm);
        char prov[200];
        snprintf(prov, sizeof prov, "#%d  \xC2\xB7  %02d:%02d:%02d  \xC2\xB7  %s%s", v->exec_id, tm.tm_hour, tm.tm_min, tm.tm_sec, v->conn[0] ? v->conn : "no connection", v->pinned ? "  \xC2\xB7  pinned" : "");
        float pw = font_width(a->font_small, prov, -1);
        draw_text_fit(a->font_small, rr, prov, -1, rect_make(hx, hdr.y, pw + px(a, 2), hdr.h), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        hx += pw + px(a, 12);
        if (v->stale || v->discarded) {
            const char *n = v->discarded ? "discarded" : "stale: connection closed";
            float nw = font_width(a->font_small, n, -1);
            draw_text_fit(a->font_small, rr, n, -1, rect_make(hx, hdr.y, nw + px(a, 2), hdr.h), color_hex(v->discarded ? T_TEXT_DIM : T_YELLOW), ALIGN_LEFT);
            hx += nw + px(a, 12);
        }
    }
    /* Execution info (US-028): rows loaded, whether more exist, exec vs fetch time. A total is
     * only stated when the source is exhausted (RN-004). Local vs server scope is spelled out. */
    char stats[300] = "";
    float right = hdr.x + hdr.w - px(a, 14);
    if (live(v) && v->rs.ok) {
        Grid *g = &v->g;
        char ms[32], fms[32], local[96] = "";
        fmt_duration(ms, sizeof ms, v->rs.exec_ms);
        fmt_duration(fms, sizeof fms, v->rs.fetch_ms);
        if (g->nfilters) snprintf(local, sizeof local, "%d of ", g->nvis);
        if (v->rs.has_rows) {
            char rows[120], nr[32];
            int loaded = v->ed_init && v->ed.new_from < v->rs.nrows ? v->ed.new_from : v->rs.nrows;
            fmt_int(nr, sizeof nr, loaded);
            if (g->nfilters) { char nv[32]; fmt_int(nv, sizeof nv, g->nvis); snprintf(local, sizeof local, "%s of ", nv); }
            if (v->rs.complete) snprintf(rows, sizeof rows, "%s%s%s row%s", local, nr, g->nfilters ? " loaded" : "", loaded == 1 ? "" : "s");
            else if (v->rs.cursor_lost) snprintf(rows, sizeof rows, "%s%s rows loaded  \xC2\xB7  total unknown, cursor closed", local, nr);
            else snprintf(rows, sizeof rows, "%s%s rows loaded  \xC2\xB7  total unknown", local, nr);
            /* server-side state lives in the chips; only the local scope is a metric here */
            char scope[120] = "";
            if (g->nfilters) strncat(scope, "  \xC2\xB7  filtered locally", sizeof scope - strlen(scope) - 1);
            else if (g->nsort) strncat(scope, "  \xC2\xB7  sorted locally", sizeof scope - strlen(scope) - 1);
            if (g->frozen) { char fz[32]; snprintf(fz, sizeof fz, "  \xC2\xB7  frozen %d", g->frozen); strncat(scope, fz, sizeof scope - strlen(scope) - 1); }
            if (v->wrap) strncat(scope, "  \xC2\xB7  wrap", sizeof scope - strlen(scope) - 1);
            int hidden = grid_hidden_count(g);
            char cols[48];
            if (hidden) snprintf(cols, sizeof cols, "%d of %d cols", g->ncols, v->rs.ncols);
            else snprintf(cols, sizeof cols, "%d col%s", v->rs.ncols, v->rs.ncols == 1 ? "" : "s");
            if (v->ed_init && edit_pending_rows(&v->ed) > 0) { size_t l2 = strlen(rows); int newr = v->rs.nrows - loaded; if (newr > 0) snprintf(rows + l2, sizeof rows - l2, " + %d new  \xC2\xB7  %d unsaved", newr, edit_pending_rows(&v->ed)); else snprintf(rows + l2, sizeof rows - l2, "  \xC2\xB7  %d unsaved", edit_pending_rows(&v->ed)); }
            char mem[24]; size_t by = rs_bytes(&v->rs);
            if (by >= (1u << 20)) snprintf(mem, sizeof mem, "  \xC2\xB7  mem ~%.1f MB", by / 1048576.0); else snprintf(mem, sizeof mem, "  \xC2\xB7  mem ~%zu KB", by >> 10 ? by >> 10 : 1);
            /* derived tabs (transpose / compare) were computed locally: no execution time to report */
            if (!v->db && !v->stale && !v->qtable[0] && !v->sql) snprintf(stats, sizeof stats, "%s%s  \xC2\xB7  %s  \xC2\xB7  computed locally%s", rows, scope, cols, mem);
            else if (v->rs.fetch_ms >= 0.05)
                snprintf(stats, sizeof stats, "%s%s  \xC2\xB7  %s  \xC2\xB7  exec %s  \xC2\xB7  fetch %s%s", rows, scope, cols, ms, fms, mem);
            else
                snprintf(stats, sizeof stats, "%s%s  \xC2\xB7  %s  \xC2\xB7  exec %s%s", rows, scope, cols, ms, mem);
        } else
            snprintf(stats, sizeof stats, "%d row%s affected  \xC2\xB7  %s", v->rs.affected, v->rs.affected == 1 ? "" : "s", ms);
    }
    draw_text_fit(a->font_small, rr, stats, -1, rect_make(hx, hdr.y, right - hx, hdr.h), color_hex(T_TEXT_MUTED), ALIGN_RIGHT);
}

/* Tab strip: draws every result tab (status dot, #id, title, unsaved / partial badge, close
 * mark) and handles clicks (select, double-click rename, close, right-click menu). Tabs shrink
 * to fit, down to 120 points; the rest go behind an "N more" list. Then the persistent Output
 * tab (unless docked externally) and the memory label. Sets *clicked_tabs when a tab took the
 * click. */
static void tabs_update(Results *r, App *a, Rect tb, bool *clicked_tabs) {
    SDL_Renderer *rr = a->renderer;
    Input *in = &a->in;
    draw_rect(rr, tb, color_hex(T_BG));
    draw_rect(rr, rect_make(tb.x, tb.y + tb.h - px(a, 1), tb.w, px(a, 1)), color_hex(T_BORDER));
    /* what the strip must always keep: the Output tab and the memory label (US-056) */
    char out_lbl[64], mem[64];
    {
        int errs = 0, unseen = output_unseen(a->output, a, &errs);
        if (output_job_running(a->output)) snprintf(out_lbl, sizeof out_lbl, "Output  \xC2\xB7  running");
        else if (unseen) snprintf(out_lbl, sizeof out_lbl, "Output  \xC2\xB7  %d %s", unseen, errs ? (unseen == 1 ? "error" : "errors") : (unseen == 1 ? "warning" : "warnings"));
        else snprintf(out_lbl, sizeof out_lbl, "Output");
        size_t bytes = results_bytes(r);
        if (bytes < (1u << 20)) snprintf(mem, sizeof mem, "Results %zu KB / %d MB", bytes < 1024 ? (size_t)1 : bytes >> 10, a->results_budget_mb);
        else snprintf(mem, sizeof mem, "Results %.1f MB / %d MB", bytes / 1048576.0, a->results_budget_mb);
    }
    float keep = (r->panels_external ? 0 : font_width(a->font_ui, out_lbl, -1) + px(a, 52)) + font_width(a->font_small, mem, -1) + px(a, 28);
    /* widths: text + room for the dot and the close mark, shrunk to fit the strip but never
     * below 120 pt; tabs that still do not fit go behind an "N more" list, the current tab stays visible */
    float want[RES_MAX_TABS], minw[RES_MAX_TABS], total = 0;
    for (int i = 0; i < r->ntabs; i++) {
        ResultView *tv = r->tabs[i];
        if (!visible_v(r, tv)) { want[i] = minw[i] = 0; continue; }   /* another query tab's results */
        float extra = tab_unsaved(tv) > 0 ? px(a, 80) : (live(tv) && tv->rs.has_rows && !tv->rs.complete) ? px(a, 56) : 0;   /* room for the badge */
        minw[i] = px(a, 120) + extra;
        want[i] = clampf(font_width(a->font_ui, tv->title, -1) + px(a, 82) + extra, minw[i], px(a, 240) + extra);
        total += want[i];
    }
    float avail = tb.w - px(a, 16) - keep;
    float k = total > avail ? avail / total : 1;
    int first = 0, nvis = r->ntabs;
    bool overflow = false;
    for (int i = 0; i < r->ntabs; i++) if (want[i] * k < minw[i]) overflow = true;
    if (overflow) {
        float room = avail - px(a, 90);   /* the "N more" button */
        nvis = 0; float used = 0;
        for (int i = 0; i < r->ntabs; i++) { if (used + minw[i] > room) break; used += minw[i]; nvis++; }
        if (nvis < 1) nvis = 1;
        if (r->cur >= nvis) { first = r->cur - nvis + 1; }
        k = 1;
    }
    /* result tabs */
    float x = tb.x + px(a, 8);
    for (int i = 0; i < r->ntabs; i++) {
        ResultView *v = r->tabs[i];
        if (i < first || i >= first + nvis || !visible_v(r, v)) continue;
        float w = overflow ? minw[i] : fmaxf(want[i] * k, px(a, 64));
        Rect t = rect_make(x, tb.y + px(a, 3), w - px(a, 2), tb.h - px(a, 4));
        bool current = i == r->cur;
        bool hover = rect_contains(t, in->mouse_x, in->mouse_y);
        Rect close = rect_make(t.x + t.w - px(a, 24), t.y, px(a, 22), t.h);
        { char tg[24]; snprintf(tg, sizeof tg, "rtab:#%d", v->exec_id); ui_record(tg, t); }   /* harness target */
        bool hover_close = rect_contains(close, in->mouse_x, in->mouse_y);
        if (current) {
            draw_rrect(rr, t, px(a, 5), color_hex(T_PANEL));
            draw_rect(rr, rect_make(t.x, t.y + t.h - px(a, 2), t.w, px(a, 2)), color_hex(T_ACCENT));
        } else if (hover) draw_rrect(rr, t, px(a, 5), color_hex(T_HOVER));
        /* status: ok = green dot, error = red dot with "!", stale = hollow yellow ring,
         * discarded = dim dot (shape differs too, for colour-blind readers) */
        bool err = v->has && !v->discarded && !v->rs.ok;
        int unsaved = v->edit_ok ? edit_pending_count(&v->ed) : 0;
        bool cancelled = v->has && !v->discarded && v->rs.cancelled;
        if (cancelled) err = false;                  /* cancelled is amber, not the red of a failure */
        uint32_t dot = !v->has || v->discarded ? T_TEXT_DIM : err ? T_RED : v->stale || unsaved || cancelled ? T_YELLOW : T_GREEN;
        float d = px(a, err ? 9 : 7);
        Rect dr = rect_make(t.x + px(a, 9), t.y + (t.h - d) / 2, d, d);
        if (v->stale && !err) { draw_rrect(rr, dr, d / 2, color_hex(dot)); draw_rrect(rr, rect_inset(dr, px(a, 1.5f), px(a, 1.5f)), d / 2 - px(a, 1.5f), color_hex(current ? T_PANEL : T_BG)); }
        else draw_rrect(rr, dr, d / 2, color_hex(dot));
        if (err) draw_text_fit(a->font_small, rr, "!", -1, dr, color_hex(0xFFFFFF), ALIGN_CENTER);
        if (v->pinned) draw_rect(rr, rect_make(t.x + t.w - px(a, 7), t.y + px(a, 3), px(a, 4), px(a, 4)), color_hex(T_ACCENT));
        if (r->rn_open && r->rn_tab == i) {
            Rect fr = rect_make(t.x + px(a, 20), t.y + px(a, 1), t.w - px(a, 24), t.h - px(a, 2));
            ui_textfield(a, &r->rn, fr, "name", true);
            bool done = false;
            for (int q = 0; q < in->nkeys; q++) {
                KeyPress *kp = &in->keys[q];
                if (kp->used) continue;
                if (kp->key == SDLK_RETURN || kp->key == SDLK_KP_ENTER) { if (r->rn.len) snprintf(v->title, sizeof v->title, "%s", r->rn.buf); done = true; kp->used = true; }
                else if (kp->key == SDLK_ESCAPE) { done = true; kp->used = true; }
            }
            if (in->mouse_pressed && !rect_contains(fr, in->mouse_x, in->mouse_y)) done = true;
            if (done) r->rn_open = false;
        } else {
            char num[12];
            snprintf(num, sizeof num, "#%d", v->exec_id);
            float numw = font_width(a->font_small, num, -1);
            draw_text_fit(a->font_small, rr, num, -1, rect_make(t.x + px(a, 22), t.y, numw + px(a, 2), t.h), color_hex(T_TEXT_DIM), ALIGN_LEFT);
            Rect text = rect_make(t.x + px(a, 26) + numw, t.y, t.w - px(a, 26) - numw - px(a, 24), t.h);   /* room for the close button, always shown */
            /* the badge (unsaved rows / partial) is never the part that gets elided */
            char badge[40] = "";
            bool part = live(v) && v->rs.has_rows && !v->rs.complete;
            int urows = unsaved ? edit_pending_rows(&v->ed) : 0;
            if (urows) snprintf(badge, sizeof badge, "\xC2\xB7 %d unsaved", urows);
            else if (part) snprintf(badge, sizeof badge, "\xC2\xB7 partial");
            float bw = badge[0] ? font_width(a->font_small, badge, -1) + px(a, 6) : 0;
            if (bw > text.w - px(a, 28)) bw = text.w - px(a, 28);   /* the title gives way first */
            float tw = fminf(font_width(a->font_ui, v->title, -1), text.w - bw);
            draw_text_fit(a->font_ui, rr, v->title, -1, rect_make(text.x, text.y, text.w - bw, text.h), color_hex(v->discarded ? T_TEXT_DIM : current ? T_TEXT : T_TEXT_MUTED), ALIGN_LEFT);
            if (badge[0]) draw_text_fit(a->font_small, rr, badge, -1, rect_make(text.x + tw + px(a, 6), text.y, bw - px(a, 4), text.h), color_hex(urows ? T_YELLOW : T_TEXT_DIM), ALIGN_LEFT);
            /* close button: the click target exists on every tab, so the mark is always drawn:
             * dim on inactive tabs, normal on the current / hovered one, highlighted under the pointer */
            if (hover_close) draw_rrect(rr, rect_make(close.x + px(a, 2), close.y + (close.h - px(a, 18)) / 2, px(a, 18), px(a, 18)), px(a, 9), color_hex(T_HOVER));
            draw_cross(rr, close.x + close.w / 2, close.y + close.h / 2, px(a, 8), px(a, 1.5f),
                       color_hex(hover_close ? T_TEXT : (hover || current) ? T_TEXT_MUTED : T_BORDER));
        }
        if (hover) a->want_cursor = CURSOR_HAND;
        if (hover && in->mouse_pressed && !in->mouse_consumed) {
            in->mouse_consumed = true;
            *clicked_tabs = true;
            a->focus = FOCUS_RESULTS;
            if (hover_close && !(r->rn_open && r->rn_tab == i)) { tab_close_request(r, a, i); return; }
            if (in->clicks >= 2) { r->rn_open = true; r->rn_tab = i; textfield_set(&r->rn, v->title); r->rn.blink = SDL_GetTicks(); }
            else { r->cur = i; r->output_active = false; r->vv_open = r->fp_open = r->cp_open = false; }
        }
        if (hover && in->mouse_rpressed && !in->mouse_consumed) { in->mouse_consumed = true; *clicked_tabs = true; a->focus = FOCUS_RESULTS; r->cur = i; r->output_active = false; tab_menu_open(r, a, i); }
        x += w;
    }
    /* overflow list of the tabs that did not fit */
    if (overflow && nvis < r->ntabs) {
        char more[24]; snprintf(more, sizeof more, "%d more    ", r->ntabs - nvis);
        Rect mb = rect_make(x + px(a, 4), tb.y + px(a, 6), px(a, 80), tb.h - px(a, 10));
        if (ui_button(a, mb, more, BTN_QUIET, true)) {
            popup_open(&r->menu, mb.x, mb.y + mb.h); r->menu_tab = -1; r->menu_row = -1;
            int slot = 10;
            for (int i = 0; i < r->ntabs && slot < POPUP_MAX; i++) {
                if (i >= first && i < first + nvis) continue;
                ResultView *tv = r->tabs[i];
                int ur = tab_unsaved(tv) > 0 ? edit_pending_rows(&tv->ed) : 0;
                bool part = live(tv) && tv->rs.has_rows && !tv->rs.complete;
                snprintf(r->menu_text[slot], sizeof r->menu_text[0], "%s  #%d  %s%s%s", !tv->has || tv->discarded ? "\xC2\xB7" : tv->has && !tv->rs.ok ? "\xC3\x97" : "\xE2\x80\xA2", tv->exec_id, tv->title, ur ? "  \xC2\xB7 unsaved" : part ? "  \xC2\xB7 partial" : "", "");
                popup_add(&r->menu, CM_TAB_GOTO_BASE + i, r->menu_text[slot++], false, true);
            }
        }
        draw_chevron(rr, mb.x + mb.w - px(a, 12), mb.y + mb.h / 2, px(a, 8), px(a, 1.5f), true, color_hex(T_TEXT_MUTED));
        x += px(a, 88);
    }
    /* the persistent Output tab (OUT-001): always present, badge for unseen warnings / errors;
     * with docking the Output is its own panel and this tab is not drawn */
    if (!r->panels_external) {
        int errs = 0, unseen = output_unseen(a->output, a, &errs);
        bool running = output_job_running(a->output);
        const char *lbl = out_lbl;
        float w = font_width(a->font_ui, lbl, -1) + px(a, 40);
        Rect t = rect_make(x + px(a, 6), tb.y + px(a, 3), w, tb.h - px(a, 4));
        bool current = r->output_active, hover = rect_contains(t, in->mouse_x, in->mouse_y);
        if (current) { draw_rrect(rr, t, px(a, 5), color_hex(T_PANEL)); draw_rect(rr, rect_make(t.x, t.y + t.h - px(a, 2), t.w, px(a, 2)), color_hex(T_ACCENT)); }
        else if (hover) draw_rrect(rr, t, px(a, 5), color_hex(T_HOVER));
        float d = px(a, 7);
        uint32_t dot = running ? T_ACCENT : errs ? T_RED : unseen ? T_YELLOW : T_TEXT_DIM;
        draw_rrect(rr, rect_make(t.x + px(a, 9), t.y + (t.h - d) / 2, d, d), d / 2, color_hex(dot));
        draw_text_fit(a->font_ui, rr, lbl, -1, rect_make(t.x + px(a, 22), t.y, t.w - px(a, 26), t.h), color_hex(current ? T_TEXT : unseen || running ? T_TEXT : T_TEXT_MUTED), ALIGN_LEFT);
        if (hover) a->want_cursor = CURSOR_HAND;
        if (hover && (in->mouse_pressed || in->mouse_rpressed) && !in->mouse_consumed) { in->mouse_consumed = true; *clicked_tabs = true; a->focus = FOCUS_RESULTS; r->output_active = true; r->vv_open = r->fp_open = r->cp_open = false; output_mark_seen(a->output, a); }
        x += w + px(a, 6);
    }
    /* memory in use across tabs (US-030) */
    size_t bytes = results_bytes(r);
    double use = a->results_budget_mb > 0 ? bytes / 1048576.0 / a->results_budget_mb : 0;
    float mw = font_width(a->font_small, mem, -1);
    if (x + mw + px(a, 24) < tb.x + tb.w)
        draw_text_fit(a->font_small, rr, mem, -1, rect_make(tb.x + tb.w - mw - px(a, 14), tb.y, mw + px(a, 2), tb.h), color_hex(use >= 0.95 ? T_RED : use >= 0.8 ? T_YELLOW : T_TEXT_DIM), ALIGN_RIGHT);
}

/* ---- cells ------------------------------------------------------------- */

/* Per-frame state shared by every cell draw: selection rectangle, hovered row, grid focus. */
typedef struct { bool has_sel; int r0, c0, r1, c1, hover_row; bool focused; } CellCtx;

/* Draws display cell (i, c) in cr: pending-edit tints, selection, search hits, the cursor, then
 * the value (NULL, empty, DEFAULT markers; wrapped text in tall rows) and the column divider. */
static void draw_cell(Results *r, ResultView *v, App *a, const CellCtx *cx, int i, int c, Rect cr) {
    SDL_Renderer *rr = a->renderer;
    Grid *g = &v->g;
    const char *cell = grid_cell(g, i, c);
    bool row_sel = cx->has_sel && i >= cx->r0 && i <= cx->r1;
    bool in_sel = row_sel && c >= cx->c0 && c <= cx->c1;
    bool cursor = i == g->sel_r && c == g->sel_c;
    /* pending edits (US-033): yellow cell, green new row, red deleted row */
    int pstate = 0;
    int src_row = g->order[i], src_col = g->cols[c];
    bool del = false, newrow = false;
    if (v->edit_ok || (v->ed_init && edit_pending_count(&v->ed) > 0)) {
        cell = edit_pending_value(&v->ed, src_row, src_col, &pstate);
        del = edit_row_deleted(&v->ed, src_row); newrow = edit_row_new(&v->ed, src_row);
        if (newrow) draw_rect(rr, cr, color_alpha(color_hex(T_GREEN), 28));
        if (del) draw_rect(rr, cr, color_alpha(color_hex(T_RED), 36));
        if (pstate == 1 || pstate == 2) draw_rect(rr, cr, color_alpha(color_hex(T_YELLOW), 50));
    }
    if (in_sel) draw_rect(rr, cr, color_hex(T_SELECTED));
    /* a corner mark survives the selection fill: the cell is still unsaved */
    if (pstate == 1 || pstate == 2) { draw_rect(rr, rect_make(cr.x, cr.y, px(a, 7), px(a, 2)), color_hex(T_YELLOW)); draw_rect(rr, rect_make(cr.x, cr.y, px(a, 2), px(a, 7)), color_hex(T_YELLOW)); }
    if (g->search[0] && grid_is_hit(g, i, c)) {
        bool current = g->hit >= 0 && g->hits[g->hit * 2] == i && g->hits[g->hit * 2 + 1] == c;
        draw_rect(rr, cr, color_alpha(color_hex(T_YELLOW), current ? 120 : 50));
        if (current) draw_rect_outline(rr, cr, px(a, 1), color_hex(T_YELLOW));
    }
    if (in_sel) {
        /* 1pt accent border on the outer edges of the range */
        Color bc = color_hex(T_ACCENT);
        if (i == cx->r0) draw_rect(rr, rect_make(cr.x, cr.y, cr.w, px(a, 1)), bc);
        if (i == cx->r1) draw_rect(rr, rect_make(cr.x, cr.y + cr.h - px(a, 1), cr.w, px(a, 1)), bc);
        if (c == cx->c0) draw_rect(rr, rect_make(cr.x, cr.y, px(a, 1), cr.h), bc);
        if (c == cx->c1) draw_rect(rr, rect_make(cr.x + cr.w - px(a, 1), cr.y, px(a, 1), cr.h), bc);
    }
    if (cursor && cx->focused && (cx->r0 != cx->r1 || cx->c0 != cx->c1)) draw_rect_outline(rr, cr, px(a, 1.5f), color_hex(0xFFFFFF));
    else if (cursor && cx->focused) draw_rect_outline(rr, cr, px(a, 1.5f), color_hex(T_ACCENT));
    int align = grid_numeric(g, c) ? ALIGN_RIGHT : ALIGN_LEFT;
    Rect tr = rect_inset(cr, px(a, 10), 0);
    /* display form (US-017 / US-018): NULL and '' are distinct markers; booleans read as
     * words; numbers and dates follow the Format menu; the raw value is never modified */
    char tmp[64];
    const char *shown = pstate ? grid_format_value(g, src_col, cell, tmp, sizeof tmp) : grid_display(g, i, c, tmp, sizeof tmp);
    Color tc = color_hex(in_sel ? 0xFFFFFF : shown == tmp ? T_SYN_KEYWORD : T_TEXT);
    if (shown == tmp && grid_col_kind(g, c) != CK_BOOL) tc = color_hex(in_sel ? 0xFFFFFF : T_TEXT);
    if (del) draw_rect(rr, rect_make(tr.x, cr.y + r->row_h / 2, tr.w, px(a, 1)), color_hex(T_RED));
    if (pstate == 3) draw_text_fit(a->font_mono, rr, "DEFAULT", -1, tr, color_hex(T_TEXT_DIM), align);
    else if (!cell) draw_text_fit(a->font_mono, rr, "NULL", -1, tr, color_hex(T_TEXT_DIM), align);
    else if (!*cell) draw_text_fit(a->font_mono, rr, "''", -1, tr, color_hex(T_TEXT_DIM), align);
    else if (v->wrap && cr.h > r->row_h + 1 && (strchr(shown, '\n') || font_width(a->font_mono, shown, -1) > tr.w)) {
        Rect wr = rect_make(tr.x, tr.y + px(a, 3), tr.w, cr.h - px(a, 4));
        clip_push(rr, wr);
        draw_wrapped(a->font_mono, rr, shown, wr, tc, 0, true);
        clip_pop(rr);
    } else if (v->wrap && cr.h > r->row_h + 1) draw_text_fit(a->font_mono, rr, shown, -1, rect_make(tr.x, tr.y, tr.w, r->row_h), tc, align);   /* top-aligned in a tall row */
    else draw_text_fit(a->font_mono, rr, shown, -1, tr, tc, align);
    draw_rect(rr, rect_make(cr.x + cr.w - px(a, 1), cr.y, px(a, 1), cr.h), color_alpha(color_hex(T_BORDER), 120));
}

/* Draws the header of display column c: name, local sort arrow and priority, or the green
 * server-sort arrow with an "S" badge; highlighted when hovered or selected. */
static void draw_head_cell(Results *r, ResultView *v, App *a, const CellCtx *cx, int c, Rect cr, int head_hover) {
    SDL_Renderer *rr = a->renderer;
    Grid *g = &v->g;
    bool desc = false;
    int prio = grid_sort_priority(g, c, &desc);
    bool col_sel = cx->has_sel && c >= cx->c0 && c <= cx->c1;
    bool hot = c == head_hover || col_sel;
    bool srv = v->srv_sort[0] && strcmp(v->srv_sort, grid_col_name(g, c)) == 0;
    if (hot) draw_rect(rr, cr, color_hex(T_HOVER));
    else if (prio || srv) draw_rect(rr, cr, color_hex(T_PANEL_ALT));
    Rect tr = rect_inset(cr, px(a, 10), 0);
    bool numeric = grid_numeric(g, c);
    if (prio || srv) {
        /* indicator always follows the name; the name is truncated before the glyph */
        float badge = g->nsort > 1 || srv ? px(a, 26) : px(a, 14);
        tr.w -= badge;
        float lw2 = font_width(a->font_ui, grid_col_name(g, c), -1);
        if (lw2 > tr.w) lw2 = tr.w;
        float ix = (numeric ? tr.x + tr.w : tr.x + lw2) + px(a, 8);
        draw_triangle(rr, ix, cr.y + cr.h / 2, px(a, 8), px(a, 5), srv ? v->srv_desc : desc, color_hex(srv ? T_GREEN : T_ACCENT));
        if (g->nsort > 1 && prio) {
            char pn[4];
            snprintf(pn, sizeof pn, "%d", prio);
            draw_text_fit(a->font_small, rr, pn, -1, rect_make(ix + px(a, 6), cr.y, px(a, 12), cr.h), color_hex(T_ACCENT), ALIGN_LEFT);
        } else if (srv) {
            /* filled badge so "server" survives any header highlight */
            Rect bd = rect_make(ix + px(a, 6), cr.y + (cr.h - px(a, 14)) / 2, px(a, 14), px(a, 14));
            draw_rrect(rr, bd, px(a, 3), color_hex(T_GREEN));
            draw_text_fit(a->font_small, rr, "S", -1, bd, color_hex(0x10261B), ALIGN_CENTER);
        }
    }
    draw_text_fit(a->font_ui, rr, grid_col_name(g, c), -1, tr, color_hex(hot || prio || srv ? T_TEXT : T_TEXT_MUTED), numeric ? ALIGN_RIGHT : ALIGN_LEFT);
    draw_rect(rr, rect_make(cr.x + cr.w - px(a, 1), cr.y, px(a, 1), cr.h), color_hex(T_BORDER));
}

/* ---- main panel -------------------------------------------------------- */

/* Overlays (menus, popovers, dialogs) may extend over other panels, so they take the frame's
 * input before any panel does; main.c calls this right after the menu bar. */
void results_pre_update(Results *r, App *a) {
    g_app = a;
    fetch_pump(r, a);
    if (r->rerun_pending) { r->rerun_pending = false; server_rerun(r, a); }
    ResultView *v = cur(r);
    if (live(v)) {
        GridFormat f = { a->fmt_group, a->fmt_decimals, a->fmt_date, a->fmt_tz };
        v->g.fmt = f;
        remeasure_for_formats(v, a);
    }
    if (r->cur != r->last_cur) { r->last_cur = r->cur; a->status[0] = 0; }   /* a stale message must not describe another tab */
    int chosen = popup_update(a, &r->menu);
    if (chosen >= 0) menu_run(r, a, chosen);
    v = cur(r);
    if (r->output_active) output_pre_update(a->output, a);
    viewer_update(r, a);
    filter_update(r, a);
    if (r->cp_open) { if (live(v)) columns_panel(r, v, a, true); else r->cp_open = false; }
    if (r->ex_open) { if (live(v)) export_dialog(r, v, a, true); else r->ex_open = false; }
    if (r->rv_review) { if (live(v) && v->edit_ok) review_dialog(r, v, a, true); else r->rv_review = false; }
    if (r->chart_open) { if (live(v) && v->rs.has_rows) chart_panel(r, v, a, true); else r->chart_open = false; }
    if (r->cf_open) close_confirm(r, a, true);
    cell_edit_update(r, a);
}

/* shows label + shortcut after 500 ms over a toolbar button (drawn in the overlay) */
static void tooltip_track(Results *r, App *a, int idx, Rect rc, const char *text) {
    bool over = rect_contains(rc, a->in.mouse_x, a->in.mouse_y);
    if (!over) { if (r->tt_hover == idx) r->tt_hover = -1; return; }
    Uint64 now = SDL_GetTicks();
    if (r->tt_hover != idx) { r->tt_hover = idx; r->tt_since = now; app_wake_in(a, 520); return; }
    if (now - r->tt_since >= 500) { r->tt_show = true; r->tt_rect = rc; r->tt_text = text; }
}

/* Runs a toolbar button (CM_TB_*) on the selected column (or the first one); `from` is the
 * button rectangle, where the Sort menu opens. */
static void toolbar_action(Results *r, ResultView *v, App *a, int id, Rect from) {
    Grid *g = &v->g;
    int col = g->sel_c >= 0 ? g->sel_c : 0;
    switch (id) {
    case CM_TB_FIND: results_find(r); break;
    case CM_TB_FILTER: if (g->ncols) open_filter(r, v, col, NULL, false); break;
    case CM_TB_SORT:
        if (!g->ncols) break;
        popup_open(&r->menu, from.x, from.y + from.h); r->menu_tab = -1; r->menu_row = -1; r->menu_col = col;
        snprintf(r->menu_text[0], sizeof r->menu_text[0], "LOADED ROWS (LOCAL)  \xC2\xB7  %s", grid_col_name(g, col));
        popup_add_caption(&r->menu, r->menu_text[0], 0, false);
        popup_add(&r->menu, CM_SORT_ASC, "Sort Ascending", false, true);
        popup_add(&r->menu, CM_SORT_DESC, "Sort Descending", false, true);
        if (g->nsort) popup_add(&r->menu, CM_SORT_CLEAR, "Clear Local Sort (back to the received order)", true, true);
        if (server_capable(v, DB_CAP_SERVER_SORT)) { popup_add_caption(&r->menu, "SERVER  \xC2\xB7  RE-RUNS THE QUERY", T_GREEN, true); popup_add(&r->menu, CM_SRV_ASC, "Sort on Server, Ascending", false, true); popup_add(&r->menu, CM_SRV_DESC, "Sort on Server, Descending", false, true); }
        break;
    case CM_TB_COLUMNS: results_columns_panel(r); break;
    case CM_TB_EXPORT: results_export_dialog(r, a, 1); break;
    }
}

/* Forward declaration: the drawing and input of the panel, measured by results_update. */
static void results_update_body(Results *r, App *a, Rect b);

/* Draws the results panel. The first time a tab that came from an Output execution is shown,
 * the CPU time of this draw is stored on that execution's result_set event (OUT-026), so the
 * Output can show execution, fetch and rendering as separate, labelled measurements. */
void results_update(Results *r, App *a, Rect b) {
    ResultView *v = cur(r);
    bool measure = !r->output_active && live(v) && v->out_exec > 0 && !v->render_noted;
    Uint64 t0 = measure ? SDL_GetPerformanceCounter() : 0;
    results_update_body(r, a, b);
    if (measure && cur(r) == v) {
        double ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
        v->render_noted = true;
        output_note_render(a->output, v->out_exec, v->exec_id, ms);
    }
}

/* Draws the Results panel in b and handles its input, in order: tab strip (or the Output
 * tab), meta line, toolbar (actions, fetch buttons, find bar, chips, pending-edit actions),
 * selection footer, embedded record view, then the grid: geometry, scrolling, keyboard,
 * header interaction (resize, autofit, reorder, sort, select), cell interaction, and the
 * cells, row numbers, header and scrollbars. Empty, error, discarded and no-rows states draw
 * a message instead of the grid. */
static void results_update_body(Results *r, App *a, Rect b) {
    SDL_Renderer *rr = a->renderer;
    Input *in = &a->in;
    ResultView *v = cur(r);
    bool focused = a->focus == FOCUS_RESULTS;

    draw_rect(rr, b, color_hex(T_PANEL));
    Rect body = b;
    /* Tab strip first (US-006): it selects what the meta line below describes; the Output
     * tab lives there too, so the strip is always drawn */
    {
        Rect tb = rect_make(body.x, body.y, body.w, px(a, TAB_H));
        bool clicked_tabs = false;
        tabs_update(r, a, tb, &clicked_tabs);
        v = cur(r);
        body.y += tb.h; body.h -= tb.h;
    }
    /* the Output tab replaces everything below the strip */
    if (r->output_active) { r->grid_rect = rect_make(0, 0, 0, 0); output_update(a->output, a, body); return; }
    Rect hdr = rect_make(body.x, body.y, body.w, px(a, M_HEADER));
    draw_header(r, v, a, hdr);
    body.y += hdr.h; body.h -= hdr.h;
    Grid *g = live(v) ? &v->g : NULL;

    /* Toolbar: search field, filter chips, server chips, fetch buttons (only when in use) */
    bool toolbar = g && v->rs.has_rows;
    r->tt_show = false;
    if (toolbar) {
        Rect tb = rect_make(body.x, body.y, body.w, px(a, 34));
        draw_rect(rr, tb, color_hex(T_PANEL_ALT));
        draw_rect(rr, rect_make(tb.x, tb.y + tb.h - px(a, 1), tb.w, px(a, 1)), color_hex(T_BORDER));
        float x = tb.x + px(a, 10);
        /* action buttons (US-055): the essentials, with tooltips and shortcuts; what does not
         * fit in the left 45 % of the strip moves behind "More" instead of vanishing */
        {
            int nflt = g->nfilters + (v->srv_where[0] ? 1 : 0), nsrt = g->nsort + (v->srv_sort[0] ? 1 : 0);
            snprintf(r->tb_text[0], sizeof r->tb_text[0], nflt ? "Filter (%d)" : "Filter", nflt);
            snprintf(r->tb_text[1], sizeof r->tb_text[0], nsrt ? "Sort (%d)" : "Sort", nsrt);
            bool layout_on = !grid_layout_default(g) || v->wrap;
            int pend_rows = v->ed_init ? edit_pending_rows(&v->ed) : 0;
            if (pend_rows) snprintf(r->tb_text[2], sizeof r->tb_text[0], "Export unavailable: %d unsaved row%s. Apply or discard them first.", pend_rows, pend_rows == 1 ? "" : "s");
            const char *labels[5] = { "Find", r->tb_text[0], r->tb_text[1], layout_on ? "Columns \xE2\x80\xA2" : "Columns", "Export" };
            const char *tips[5] = { "Find in loaded rows (local)  \xE2\x8C\x98" "F", "Filter the selected column: loaded rows only (local), the query is not re-run", "Sort the selected column: loaded rows only (local); server sort is in the header menu", layout_on ? "Columns: hidden / reordered / frozen / wrap active  \xE2\x87\xA7\xE2\x8C\x98" "C" : "Hide, reorder, freeze columns  \xE2\x87\xA7\xE2\x8C\x98" "C", pend_rows ? r->tb_text[2] : "Export rows\xE2\x80\xA6 (selection, loaded or full result)  \xE2\x87\xA7\xE2\x8C\x98" "E" };
            const int ids[5] = { CM_TB_FIND, CM_TB_FILTER, CM_TB_SORT, CM_TB_COLUMNS, CM_TB_EXPORT };
            /* fold order: Columns first, then Export, then Sort, then Filter; what the strip must keep
             * is the fetch buttons (when a cursor is open) and room for at least one chip */
            float reserved = (v->rs.cursor ? px(a, 210) : 0) + px(a, 150);
            float budget = tb.x + tb.w - reserved;
            bool show[5] = { true, true, true, true, true };
            float bw[5], need = 0;
            for (int i = 0; i < 5; i++) { bw[i] = font_width(a->font_ui, labels[i], -1) + px(a, 22); need += bw[i] + px(a, 4); }
            const int fold_order[4] = { 3, 4, 2, 1 };
            for (int k = 0; k < 4 && x + need + px(a, 72) > budget; k++) { show[fold_order[k]] = false; need -= bw[fold_order[k]] + px(a, 4); }
            int shown = 0;
            bool hidden_state = false;
            for (int i = 0; i < 5; i++) {
                float w = bw[i];
                if (!show[i]) { if ((i == 1 && nflt) || (i == 2 && nsrt) || (i == 3 && layout_on)) hidden_state = true; continue; }
                Rect br = rect_make(x, tb.y + px(a, 5), w, tb.h - px(a, 10));
                bool active = (i == 1 && nflt) || (i == 2 && nsrt);
                if (ui_button(a, br, labels[i], active ? BTN_SELECTED : BTN_QUIET, !(ids[i] == CM_TB_EXPORT && pend_rows))) toolbar_action(r, v, a, ids[i], br);
                tooltip_track(r, a, i, br, tips[i]);
                x += w + px(a, 4); shown++;
            }
            if (shown < 5) {
                Rect br = rect_make(x, tb.y + px(a, 5), px(a, 68), tb.h - px(a, 10));
                /* the UI font has no U+25BE, so the chevron is drawn (same as the Output scope button) */
                if (ui_button(a, br, hidden_state ? "More \xE2\x80\xA2  " : "More    ", BTN_QUIET, true)) {
                    popup_open(&r->menu, br.x, br.y + br.h); r->menu_tab = -1; r->menu_row = -1; r->menu_col = g->sel_c;
                    const char *more_labels[5] = { "Find  \xE2\x8C\x98" "F", r->tb_text[0], r->tb_text[1], layout_on ? "Columns\xE2\x80\xA6 \xE2\x80\xA2  \xE2\x87\xA7\xE2\x8C\x98" "C" : "Columns\xE2\x80\xA6  \xE2\x87\xA7\xE2\x8C\x98" "C", pend_rows ? r->tb_text[2] : "Export\xE2\x80\xA6  \xE2\x87\xA7\xE2\x8C\x98" "E" };
                    for (int i = 0; i < 5; i++) if (!show[i]) popup_add(&r->menu, ids[i], more_labels[i], false, !(ids[i] == CM_TB_EXPORT && pend_rows));
                }
                draw_chevron(rr, br.x + br.w - px(a, 12), br.y + br.h / 2, px(a, 8), px(a, 1.5f), true, color_hex(T_TEXT_MUTED));
                tooltip_track(r, a, 9, br, "More actions (narrow window)");
                x += px(a, 72);
            }
            draw_rect(rr, rect_make(x + px(a, 2), tb.y + px(a, 8), px(a, 1), tb.h - px(a, 16)), color_hex(T_BORDER));
            x += px(a, 12);
        }
        if (v->rs.cursor) {
            Rect bm = rect_make(tb.x + tb.w - px(a, 14) - px(a, 112), tb.y + px(a, 5), px(a, 112), tb.h - px(a, 10));
            Rect ba = rect_make(bm.x - px(a, 6) - px(a, 76), bm.y, px(a, 76), bm.h);
            char lbl[32];
            snprintf(lbl, sizeof lbl, "Fetch %d more", a->fetch_size);
            if (ui_button(a, ba, "Fetch all", BTN_NORMAL, !r->fj)) results_fetch_all(r, a);
            if (ui_button(a, bm, r->fj ? "Fetching\xE2\x80\xA6" : lbl, BTN_NORMAL, !r->fj)) results_fetch_more(r, a, a->fetch_size);
        }
        if (v->search_open) {
            Rect sf = rect_make(x, tb.y + px(a, 4), px(a, 240), tb.h - px(a, 8));
            if (ui_textfield(a, &v->search, sf, "Find in loaded rows", v->search_focus && focused)) { v->search_focus = true; a->focus = FOCUS_RESULTS; focused = true; }
            if (strcmp(v->search.buf, v->last_search) != 0) {
                snprintf(v->last_search, sizeof v->last_search, "%s", v->search.buf);
                grid_search(g, v->search.buf);
                int row, col;
                if (grid_search_step(g, 1, &row, &col)) { grid_select_cell(g, row, col, false); ensure_visible(r, v, row, col); }
            }
            x += sf.w + px(a, 8);
            char cnt[64];
            if (v->search.len) snprintf(cnt, sizeof cnt, "%d of %d  \xC2\xB7  loaded rows", g->nhits ? g->hit + 1 : 0, g->nhits);
            else snprintf(cnt, sizeof cnt, "loaded rows");
            float cw = font_width(a->font_small, cnt, -1);
            draw_text_fit(a->font_small, rr, cnt, -1, rect_make(x, tb.y, cw + px(a, 4), tb.h), color_hex(T_TEXT_DIM), ALIGN_LEFT);
            x += cw + px(a, 10);
            Rect bp = rect_make(x, tb.y + px(a, 5), px(a, 26), tb.h - px(a, 10)), bn = rect_make(x + px(a, 30), bp.y, px(a, 26), bp.h);
            int row, col;
            if (ui_button(a, bp, "\xE2\x80\xB9", BTN_NORMAL, g->nhits > 0) && grid_search_step(g, -1, &row, &col)) { grid_select_cell(g, row, col, false); ensure_visible(r, v, row, col); }
            if (ui_button(a, bn, "\xE2\x80\xBA", BTN_NORMAL, g->nhits > 0) && grid_search_step(g, 1, &row, &col)) { grid_select_cell(g, row, col, false); ensure_visible(r, v, row, col); }
            if (v->search_focus && focused) {
                for (int k = 0; k < in->nkeys; k++) {
                    KeyPress *kp = &in->keys[k];
                    if (kp->used) continue;
                    if (kp->key == SDLK_RETURN || kp->key == SDLK_KP_ENTER) {
                        if (grid_search_step(g, (kp->mods & KM_SHIFT) ? -1 : 1, &row, &col)) { grid_select_cell(g, row, col, false); ensure_visible(r, v, row, col); }
                        kp->used = true;
                    } else if (kp->key == SDLK_ESCAPE) { v->search_open = false; v->search_focus = false; grid_search(g, ""); v->last_search[0] = 0; kp->used = true; }
                    else if (kp->key == SDLK_DOWN || kp->key == SDLK_UP) { v->search_focus = false; }
                }
            }
            x += px(a, 64);
            Rect bx = rect_make(x, bp.y, px(a, 26), bp.h);
            if (ui_button(a, bx, "", BTN_QUIET, true)) { v->search_open = false; v->search_focus = false; grid_search(g, ""); v->last_search[0] = 0; }
            draw_cross(rr, bx.x + bx.w / 2, bx.y + bx.h / 2, px(a, 8), px(a, 1.5f), color_hex(T_TEXT_MUTED));
            x += px(a, 40);
        }
        /* chips: server state first (green, labelled "Server"), then local filters, then the
         * display formats (neutral). Every chip has an x and the local ones reopen on click. */
        enum { CH_SRV_SORT, CH_SRV_WHERE, CH_LOCAL, CH_DISPLAY };
        int kinds[GRID_MAX_FILTERS + 3], nchips = 0;
        if (v->srv_sort[0]) kinds[nchips++] = CH_SRV_SORT;
        if (v->srv_where[0]) kinds[nchips++] = CH_SRV_WHERE;
        for (int i = 0; i < g->nfilters; i++) kinds[nchips++] = CH_LOCAL;
        if (formats_active(a)) kinds[nchips++] = CH_DISPLAY;
        bool have_srv = v->srv_sort[0] || v->srv_where[0], have_local = g->nfilters || g->nsort, local_hdr_done = false;
        if (have_srv) {
            float lw3 = font_width(a->font_small, "Server", -1);
            draw_text_fit(a->font_small, rr, "Server", -1, rect_make(x, tb.y, lw3 + px(a, 2), tb.h), color_hex(T_GREEN), ALIGN_LEFT);
            x += lw3 + px(a, 8);
        }
        /* scope badge (US-051): drawn only when a local operation is active, after the server group,
         * so the green chips never read as local and the badge never reads as an instruction */
        char lb[96];
        {
            char nr[32]; fmt_int(nr, sizeof nr, v->rs.nrows);
            if (v->rs.complete) snprintf(lb, sizeof lb, "Local%s  \xC2\xB7  all %s row%s", g->nsort && !g->nfilters ? "  \xC2\xB7  sorted" : "", nr, v->rs.nrows == 1 ? "" : "s");
            else snprintf(lb, sizeof lb, "Local%s  \xC2\xB7  %s row%s loaded (partial)", g->nsort && !g->nfilters ? "  \xC2\xB7  sorted" : "", nr, v->rs.nrows == 1 ? "" : "s");
        }
#define LOCAL_HEADER() do { \
            if (have_srv) { draw_rect(rr, rect_make(x + px(a, 2), tb.y + px(a, 8), px(a, 1), tb.h - px(a, 16)), color_hex(T_BORDER)); x += px(a, 14); } \
            float lw4 = font_width(a->font_small, lb, -1); \
            draw_text_fit(a->font_small, rr, lb, -1, rect_make(x, tb.y, lw4 + px(a, 2), tb.h), color_hex(v->rs.complete ? T_TEXT_MUTED : T_YELLOW), ALIGN_LEFT); \
            x += lw4 + px(a, 8); local_hdr_done = true; } while (0)
        int fi = 0;
        for (int i = 0; i < nchips; i++) {
            int kind = kinds[i];
            char d[300];
            if (kind == CH_SRV_SORT) snprintf(d, sizeof d, "ORDER BY %s %s", v->srv_sort, v->srv_desc ? "DESC" : "ASC");
            else if (kind == CH_SRV_WHERE) snprintf(d, sizeof d, "WHERE %s", v->srv_where_text);
            else if (kind == CH_DISPLAY) { char f[120]; describe_formats(a, f, sizeof f); snprintf(d, sizeof d, "Display: %s", f); }
            else grid_filter_describe(g, fi, d, sizeof d);
            bool srv = kind == CH_SRV_SORT || kind == CH_SRV_WHERE;
            if (!srv && !local_hdr_done && have_local) LOCAL_HEADER();
            float w = font_width(a->font_small, d, -1) + px(a, 34);
            if (w > px(a, 360)) w = px(a, 360);
            Rect chip = rect_make(x, tb.y + px(a, 6), w, tb.h - px(a, 12));
            bool hc = rect_contains(chip, in->mouse_x, in->mouse_y);
            draw_rrect(rr, chip, chip.h / 2, color_hex(srv ? T_GREEN : T_BORDER));
            draw_rrect(rr, rect_inset(chip, px(a, 1), px(a, 1)), chip.h / 2 - px(a, 1), color_hex(hc ? T_HOVER : T_PANEL));
            draw_text_fit(a->font_small, rr, d, -1, rect_make(chip.x + px(a, 10), chip.y, w - px(a, 30), chip.h), color_hex(kind == CH_DISPLAY ? T_TEXT_MUTED : T_TEXT), ALIGN_LEFT);
            Rect xr = rect_make(chip.x + w - px(a, 22), chip.y, px(a, 18), chip.h);
            bool hx2 = rect_contains(xr, in->mouse_x, in->mouse_y);
            if (hx2) draw_rrect(rr, rect_make(xr.x, xr.y + (xr.h - px(a, 18)) / 2, px(a, 18), px(a, 18)), px(a, 9), color_hex(T_HOVER));
            draw_cross(rr, xr.x + xr.w / 2, xr.y + xr.h / 2, px(a, 8), px(a, 1.5f), color_hex(hx2 ? T_TEXT : T_TEXT_MUTED));
            if (hc) a->want_cursor = CURSOR_HAND;
            if (in->mouse_pressed && !in->mouse_consumed) {
                if (hx2) {
                    in->mouse_consumed = true;
                    if (kind == CH_SRV_SORT) { v->srv_sort[0] = 0; request_rerun(r); }
                    else if (kind == CH_SRV_WHERE) { v->srv_where[0] = 0; v->srv_where_text[0] = 0; request_rerun(r); }
                    else if (kind == CH_DISPLAY) { a->fmt_group = false; a->fmt_decimals = -1; a->fmt_date = 0; a->fmt_tz = 0; app_status(a, "Display formats reset: raw values"); }
                    else { grid_filter_remove(g, fi); v->rowy_dirty = true; }
                    break;
                }
                if (hc) {
                    in->mouse_consumed = true;
                    if (kind == CH_LOCAL) edit_filter(r, v, fi);
                    else if (kind == CH_SRV_WHERE && g->ncols) open_filter(r, v, 0, NULL, true);
                }
            }
            if (kind == CH_LOCAL) fi++;
            x += w + px(a, 6);
        }
        if (!local_hdr_done && have_local) LOCAL_HEADER();
#undef LOCAL_HEADER
        if (g->nfilters >= 2) {
            /* AND / OR between the local filters (US-011) */
            static const char *const modes[] = { "all", "any" };
            Rect mr = rect_make(x + px(a, 4), tb.y + px(a, 5), px(a, 90), tb.h - px(a, 10));
            int m = ui_segmented(a, mr, modes, 2, g->filter_any ? 1 : 0);
            if (m >= 0) { grid_filter_mode(g, m == 1); v->rowy_dirty = true; }
            x += px(a, 98);
        }
        if (v->caveat[0]) {
            float cw = fminf(font_width(a->font_small, v->caveat, -1), tb.x + tb.w - x - px(a, 16));
            draw_text_fit(a->font_small, rr, v->caveat, -1, rect_make(x + px(a, 6), tb.y, cw + px(a, 4), tb.h), color_hex(T_YELLOW), ALIGN_LEFT);
            x += cw + px(a, 16);
        }
        if (v->edit_ok && edit_pending_count(&v->ed) > 0) {
            /* one unit everywhere: rows (cells in brackets); never "changes" vs "statements" */
            int prow = edit_pending_rows(&v->ed), pcell = edit_pending_count(&v->ed);
            char pb[96];
            snprintf(pb, sizeof pb, "%d unsaved row%s (%d cell%s), not in the database yet", prow, prow == 1 ? "" : "s", pcell, pcell == 1 ? "" : "s");
            float pw = font_width(a->font_small, pb, -1);
            draw_text_fit(a->font_small, rr, pb, -1, rect_make(x + px(a, 6), tb.y, pw + px(a, 4), tb.h), color_hex(T_YELLOW), ALIGN_LEFT);
            x += pw + px(a, 16);
            Rect br = rect_make(x, tb.y + px(a, 5), px(a, 130), tb.h - px(a, 10)), bd = rect_make(x + px(a, 150), tb.y + px(a, 5), px(a, 120), tb.h - px(a, 10));
            char dl[40];
            snprintf(dl, sizeof dl, "Discard %d row%s", prow, prow == 1 ? "" : "s");
            if (ui_button(a, br, "Review and apply\xE2\x80\xA6", BTN_PRIMARY, true)) { edit_build(&v->ed); r->rv_review = true; r->review_scroll = 0; }
            if (ui_button(a, bd, dl, BTN_DANGER, true)) { edits_clear(v); edit_stmts_free(&v->ed); app_status(a, "Unsaved rows discarded"); }
            x += px(a, 280);
        }
        if (g->nfilters) {
            Rect bc = rect_make(x + px(a, 4), tb.y + px(a, 5), px(a, 72), tb.h - px(a, 10));
            bool hb = rect_contains(bc, in->mouse_x, in->mouse_y);
            if (hb) a->want_cursor = CURSOR_HAND;
            draw_text_fit(a->font_small, rr, "Clear all", -1, bc, color_hex(hb ? 0x8FB2FF : T_ACCENT), ALIGN_CENTER);
            if (hb && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; grid_filter_clear(g); v->rowy_dirty = true; }
        }
        body.y += tb.h; body.h -= tb.h;
    }

    /* Footer: selection summary (US-037) */
    char summary[300] = "";
    if (g && v->rs.has_rows) grid_sel_summary(g, summary, sizeof summary);
    if (summary[0]) {
        Rect ft = rect_make(body.x, body.y + body.h - px(a, 24), body.w, px(a, 24));
        draw_rect(rr, ft, color_hex(T_PANEL_ALT));
        draw_rect(rr, rect_make(ft.x, ft.y, ft.w, px(a, 1)), color_hex(T_BORDER));
        draw_text_fit(a->font_small, rr, summary, -1, rect_inset(ft, px(a, 14), 0), color_hex(T_TEXT), ALIGN_LEFT);
        body.h -= ft.h;
    }

    /* Record view panel on the right */
    Rect rv = rect_make(0, 0, 0, 0);
    if (g && v->rv_open && !r->panels_external && v->rs.has_rows && body.w > px(a, 600)) {
        float w = px(a, 340);
        rv = rect_make(body.x + body.w - w, body.y, w, body.h);
        body.w -= w;
    }

    r->grid_rect = body;
    bool hover = rect_contains(body, in->mouse_x, in->mouse_y);
    bool clicked = false;
    if (hover && in->mouse_pressed && !in->mouse_consumed) {
        /* focus now; the press itself is consumed after the scroll bars had their chance (a drag of the bar
         * must reach them, and must not also select a cell) */
        a->focus = FOCUS_RESULTS;
        focused = true;
        clicked = true;
        if (v) v->search_focus = false;
    }
    bool rclicked = hover && in->mouse_rpressed && !in->mouse_consumed;
    if (rclicked) { in->mouse_consumed = true; a->focus = FOCUS_RESULTS; focused = true; }

    if (!v) {
#ifdef __APPLE__
        const char *empty = "Run a statement with \xE2\x8C\x98\xE2\x86\xA9 to see results here";
#else
        const char *empty = "Run a statement with Ctrl+Enter to see results here";
#endif
        draw_text_fit(a->font_ui, rr, empty, -1, body, color_hex(T_TEXT_DIM), ALIGN_CENTER);
        return;
    }
    if (v->discarded) {
        float cy = body.y + body.h / 2 - px(a, 56);
        draw_text_fit(a->font_ui, rr, v->note, -1, rect_make(body.x, cy, body.w, px(a, 24)), color_hex(T_TEXT), ALIGN_CENTER);
        struct tm tm; localtime_r(&v->when, &tm);
        char meta[200], rows[32];
        fmt_int(rows, sizeof rows, v->discarded_rows);
        snprintf(meta, sizeof meta, "#%d  \xC2\xB7  %s rows  \xC2\xB7  %.1f MB  \xC2\xB7  ran %02d:%02d:%02d  \xC2\xB7  %s", v->exec_id, rows, v->discarded_mb, tm.tm_hour, tm.tm_min, tm.tm_sec, v->conn[0] ? v->conn : "no connection");
        draw_text_fit(a->font_small, rr, meta, -1, rect_make(body.x, cy + px(a, 26), body.w, px(a, 20)), color_hex(T_TEXT_MUTED), ALIGN_CENTER);
        if (v->sql) {
            char snip[100];
            sql_title(v->sql, v->sql_len, snip, sizeof snip);
            draw_text_fit(a->font_mono, rr, snip, -1, rect_make(body.x, cy + px(a, 48), body.w, px(a, 20)), color_hex(T_TEXT_DIM), ALIGN_CENTER);
        }
        Rect br = rect_make(body.x + body.w / 2 - px(a, 110), cy + px(a, 82), px(a, 130), px(a, 32));
        Rect bc = rect_make(br.x + br.w + px(a, 10), br.y, px(a, 90), px(a, 32));
        if (ui_button(a, br, "Re-run query", BTN_PRIMARY, v->sql && v->db) && v->sql) app_run_sql_mode(a, v->db, v->sql, v->sql_len, v->title, RUN_REPLACE, v->qtable[0] ? v->qtable : NULL);
        if (ui_button(a, bc, "Close tab", BTN_QUIET, true)) tab_remove(r, a, r->cur);
        return;
    }
    if (!v->rs.ok) {
        draw_wrapped(a->font_mono, rr, v->rs.error, rect_inset(body, px(a, 14), px(a, 12)), color_hex(T_RED), 0, true);
        return;
    }
    if (!v->rs.has_rows) {
        char msg[128];
        snprintf(msg, sizeof msg, "Statement executed. %d row%s affected.", v->rs.affected, v->rs.affected == 1 ? "" : "s");
        draw_text_fit(a->font_ui, rr, msg, -1, body, color_hex(T_TEXT_MUTED), ALIGN_CENTER);
        return;
    }
    if (v->rs.nrows == 0) {
        draw_text_fit(a->font_ui, rr, v->srv_where[0] ? "No rows match the server-side filter" : "No rows returned", -1, body, color_hex(T_TEXT_DIM), ALIGN_CENTER);
        return;
    }
    if (g->nvis == 0) {
        draw_text_fit(a->font_ui, rr, "No loaded rows match the filters", -1, body, color_hex(T_TEXT_DIM), ALIGN_CENTER);
        return;
    }

    /* Grid geometry */
    float row_h = px(a, M_GRID_ROW), head_h = px(a, M_GRID_HEAD), num_w = px(a, 44);
    char nb[16];
    snprintf(nb, sizeof nb, "%d", g->nvis);
    float nw = font_width(a->font_small, nb, -1) + px(a, 20);
    if (nw > num_w) num_w = nw;
    r->row_h = row_h; r->head_h = head_h; r->num_w = num_w; r->line_h = font_height(a->font_mono) * 1.3f;
    cols_recompute(r, v, a);
    float view_w = body.w - num_w, view_h = body.h - head_h;
    r->cells_rc = rect_make(body.x + num_w, body.y + head_h, view_w, view_h);
    if (r->frozen_w > view_w * 0.7f) { r->frozen_w = 0; }        /* frozen part would hide the rest: ignore it */
    int frozen = r->frozen_w > 0 ? g->frozen : 0;
    if (v->rowy_dirty || v->rowy_gen != g->gen || v->rowy_n != g->nvis) rows_recompute(r, v, a);
    float total_w = r->colx[g->ncols], total_h = v->rowy[g->nvis];
    float max_sx = fmaxf(0, total_w - view_w), max_sy = fmaxf(0, total_h - view_h);

    /* scrolling (eased toward the target unless reduce motion) and keyboard */
    if (hover) {
        if (in->wheel_y != 0) v->scroll_ty -= in->wheel_y;
        if (in->wheel_x != 0) v->scroll_x -= in->wheel_x;
    }
    if (focused && !v->search_focus && !results_modal_open(r)) grid_keys(r, v, a);
    v->scroll_x = clampf(v->scroll_x, 0, max_sx);
    v->scroll_ty = clampf(v->scroll_ty, 0, max_sy);
    /* the bars take their presses before the cells */
    if (ui_scrollbar_input(a, &v->sb, r->cells_rc, total_h, &v->scroll_ty) && v->sb.drag) v->scroll_y = v->scroll_ty;
    ui_hscrollbar_input(a, &v->hsb, r->cells_rc, total_w, &v->scroll_x);
    if (clicked) { if (v->sb.drag || v->hsb.drag) clicked = false; in->mouse_consumed = true; }
    v->scroll_y = a->reduce_motion ? v->scroll_ty : lerpf(v->scroll_y, v->scroll_ty, 0.4f);   /* PNL-039 */
    if (fabsf(v->scroll_y - v->scroll_ty) > 0.5f) a->animating = true; else v->scroll_y = v->scroll_ty;

    /* Header: drag an edge to resize, double-click an edge to autofit, click to sort (shift
     * adds a key), cmd-click selects the column, drag a name to reorder, right-click = menu */
    Rect head_hit = rect_make(body.x + num_w, body.y, view_w, head_h);
    int head_hover = -1;
    bool shift = app_modstate(a) & SDL_KMOD_SHIFT;
    bool on_head = rect_contains(head_hit, in->mouse_x, in->mouse_y);
    if (v->resize_col >= 0) {
        v->colw[grid_col(g, v->resize_col)] = clampf(v->resize_w0 + (in->mouse_x - v->resize_x0) / a->scale, 40, 1200);
        if (!in->mouse_down) { v->resize_col = -1; v->rowy_dirty = true; layout_save(a, v); }
        a->want_cursor = CURSOR_EW;
    } else if (on_head || v->drag_col >= 0) {
        int edge = -1, col = col_at_x(r, v, in->mouse_x);
        for (int c = 0; c < g->ncols; c++) {
            float x, w;
            if (!col_visible(r, v, c, &x, &w)) continue;
            if (fabsf(in->mouse_x - (x + w)) <= px(a, 4)) { edge = c; break; }
        }
        if (v->drag_col < 0) {
            if (edge >= 0) a->want_cursor = CURSOR_EW;
            else if (col >= 0) { a->want_cursor = CURSOR_HAND; head_hover = col; }
        }
        if (clicked && on_head) {
            if (edge >= 0) {
                if (in->clicks >= 2) { autofit_column(v, a, edge); layout_save(a, v); }
                else { v->resize_col = edge; v->resize_x0 = in->mouse_x; v->resize_w0 = v->colw[grid_col(g, edge)]; }
            } else if (col >= 0) { v->drag_col = col; v->drag_x0 = in->mouse_x; v->drag_moving = false; }
        }
        if (v->drag_col >= 0) {
            if (!v->drag_moving && fabsf(in->mouse_x - v->drag_x0) > px(a, 6)) v->drag_moving = true;
            if (v->drag_moving) a->want_cursor = CURSOR_HAND;
            if (!in->mouse_down) {
                if (v->drag_moving) {
                    int to = col_at_x(r, v, in->mouse_x);
                    if (to < 0) to = in->mouse_x > r->cells_rc.x + r->cells_rc.w / 2 ? g->ncols - 1 : 0;
                    if (to != v->drag_col) { grid_col_move(g, v->drag_col, to); layout_save(a, v); }
                } else if (v->drag_col < g->ncols) {
                    if (app_modstate(a) & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) grid_select_col(g, v->drag_col, shift);
                    else { grid_sort_click(g, v->drag_col, shift); v->rowy_dirty = true; }
                }
                v->drag_col = -1; v->drag_moving = false;
            }
        }
        if (rclicked && on_head) context_menu_open(r, v, a, -1, col, true);
    }

    /* cells: click selects, double-click edits or views, drag extends, right-click opens the menu */
    int hover_row = -1, hover_col = -1;
    if (hover && in->mouse_y >= body.y + head_h) {
        hover_row = row_at(v, in->mouse_y - body.y - head_h + v->scroll_y);
        if (in->mouse_x >= body.x + num_w) hover_col = col_at_x(r, v, in->mouse_x);
    }
    bool on_numbers = hover && in->mouse_x < body.x + num_w && in->mouse_y >= body.y + head_h;
    if (clicked && hover_row >= 0) {
        if (on_numbers) grid_select_row(g, hover_row, shift);
        else if (hover_col >= 0) {
            if (in->clicks >= 2) { if (v->edit_ok) cell_edit_open(r, v, hover_row, hover_col); else open_viewer(r, v, hover_row, hover_col); }
            else { grid_select_cell(g, hover_row, hover_col, shift); v->dragging = true; }
        }
    }
    if (v->dragging) {
        /* press, move and release can land in one frame: extend before ending the drag */
        if (hover_row >= 0 && hover_col >= 0 && !(clicked && hover_row == g->anc_r && hover_col == g->anc_c)) grid_select_cell(g, hover_row, hover_col, true);
        if (!in->mouse_down) v->dragging = false;
    }
    if (rclicked && hover_row >= 0 && hover_col >= 0) context_menu_open(r, v, a, hover_row, hover_col, false);

    /* rows in view and the selection, computed once for every cell */
    int first = row_at(v, v->scroll_y);
    if (first < 0) first = 0;
    int last = row_at(v, v->scroll_y + view_h);
    last = last < 0 ? g->nvis : last + 1;
    CellCtx cx = { false, -1, -1, -1, -1, hover_row, focused };
    cx.has_sel = grid_sel_rect(g, &cx.r0, &cx.c0, &cx.r1, &cx.c1);

    /* Cells: scrolled columns first, frozen columns on top (US-005) */
    Rect cells_rc = r->cells_rc;
    Rect scrolled = rect_make(cells_rc.x + r->frozen_w, cells_rc.y, cells_rc.w - r->frozen_w, cells_rc.h);
    for (int pass = 0; pass < 2; pass++) {
        Rect clip = pass == 0 ? scrolled : rect_make(cells_rc.x, cells_rc.y, r->frozen_w, cells_rc.h);
        if (pass == 1 && frozen == 0) break;
        clip_push(rr, clip);
        for (int i = first; i < last; i++) {
            float y = cells_rc.y + v->rowy[i] - v->scroll_y, rh = v->rowy[i + 1] - v->rowy[i];
            Rect row = rect_make(clip.x, y, clip.w, rh);
            if (i == g->sel_r) draw_rect(rr, row, color_hex(T_HOVER));
            else if (i == hover_row) draw_rect(rr, row, color_alpha(color_hex(T_HOVER), 140));
            else if (i % 2) draw_rect(rr, row, color_alpha(color_hex(T_PANEL_ALT), 110));
            int c0 = pass == 0 ? frozen : 0, c1 = pass == 0 ? g->ncols : frozen;
            for (int c = c0; c < c1; c++) {
                float x, w;
                if (!col_visible(r, v, c, &x, &w)) continue;
                draw_cell(r, v, a, &cx, i, c, rect_make(x, y, w, rh));
            }
            if (v->wrap) draw_rect(rr, rect_make(clip.x, y + rh - px(a, 1), clip.w, px(a, 1)), color_alpha(color_hex(T_BORDER), 120));
        }
        clip_pop(rr);
    }
    if (frozen && v->scroll_x > 0) {
        for (int s = 0; s < 6; s++)
            draw_rect(rr, rect_make(cells_rc.x + r->frozen_w + s * px(a, 1), cells_rc.y, px(a, 1), cells_rc.h), color_alpha(color_hex(0x000000), (uint8_t)(70 - s * 11)));
    }

    /* Row numbers (sticky left): a visual index, not a key (US-001) */
    Rect num_rc = rect_make(body.x, body.y + head_h, num_w, view_h);
    draw_rect(rr, num_rc, color_hex(T_PANEL));
    clip_push(rr, num_rc);
    for (int i = first; i < last; i++) {
        float y = num_rc.y + v->rowy[i] - v->scroll_y, rh = v->rowy[i + 1] - v->rowy[i];
        bool rs = cx.has_sel && i >= cx.r0 && i <= cx.r1;
        if (rs) draw_rect(rr, rect_make(num_rc.x, y, num_w, rh), color_hex(T_HOVER));
        if (v->edit_ok || (v->ed_init && edit_pending_count(&v->ed) > 0)) {
            int st = edit_row_state(&v->ed, g->order[i]);
            if (st) draw_rect(rr, rect_make(num_rc.x, y, px(a, 3), rh), color_hex(st == 1 ? T_YELLOW : st == 2 ? T_RED : T_GREEN));
        }
        snprintf(nb, sizeof nb, "%d", i + 1);
        draw_text_fit(a->font_small, rr, nb, -1, rect_make(num_rc.x, y, num_w - px(a, 10), row_h),
                      color_hex(rs ? T_ACCENT : T_TEXT_DIM), ALIGN_RIGHT);
    }
    clip_pop(rr);
    draw_rect(rr, rect_make(num_rc.x + num_w - px(a, 1), num_rc.y, px(a, 1), view_h), color_hex(T_BORDER));
    if (on_numbers && hover_row >= 0) a->want_cursor = CURSOR_HAND;

    /* Header (sticky top) */
    Rect head_rc = rect_make(body.x, body.y, body.w, head_h);
    draw_rect(rr, head_rc, color_hex(T_BG));
    for (int pass = 0; pass < 2; pass++) {
        Rect clip = pass == 0 ? rect_make(scrolled.x, body.y, scrolled.w, head_h) : rect_make(cells_rc.x, body.y, r->frozen_w, head_h);
        if (pass == 1 && frozen == 0) break;
        clip_push(rr, clip);
        int c0 = pass == 0 ? frozen : 0, c1 = pass == 0 ? g->ncols : frozen;
        for (int c = c0; c < c1; c++) {
            float x, w;
            if (!col_visible(r, v, c, &x, &w)) continue;
            if (v->drag_moving && c == v->drag_col) continue;
            draw_head_cell(r, v, a, &cx, c, rect_make(x, body.y, w, head_h), head_hover);
        }
        clip_pop(rr);
    }
    if (v->drag_moving && v->drag_col >= 0 && v->drag_col < g->ncols) {
        /* ghost of the dragged header and the insertion marker */
        int to = col_at_x(r, v, in->mouse_x);
        if (to >= 0) {
            float x, w;
            col_visible(r, v, to, &x, &w);
            float mx = to > v->drag_col ? x + w : x;
            draw_rect(rr, rect_make(mx - px(a, 1), body.y, px(a, 2), body.h), color_hex(T_ACCENT));
        }
        float gw = r->colx[v->drag_col + 1] - r->colx[v->drag_col];
        Rect ghost = rect_make(in->mouse_x - gw / 2, body.y, gw, head_h);
        draw_rrect(rr, ghost, px(a, 4), color_alpha(color_hex(T_SELECTED), 220));
        draw_text_fit(a->font_ui, rr, grid_col_name(g, v->drag_col), -1, rect_inset(ghost, px(a, 10), 0), color_hex(T_TEXT), ALIGN_LEFT);
    }
    draw_rect(rr, rect_make(body.x, body.y + head_h - px(a, 1), body.w, px(a, 1)), color_hex(T_BORDER));
    if (frozen) draw_rect(rr, rect_make(cells_rc.x + r->frozen_w - px(a, 2), body.y, px(a, 2), body.h), color_hex(T_SCROLLBAR));   /* freeze divider, header included */

    /* Scrollbars */
    ui_scrollbar_draw(a, &v->sb, cells_rc, total_h, v->scroll_y);
    ui_hscrollbar_draw(a, &v->hsb, cells_rc, total_w, v->scroll_x);

    /* embedded record view (when not docked as the Inspector) */
    if (rv.w > 0) {
        draw_rect(rr, rv, color_hex(T_PANEL_ALT));
        draw_rect(rr, rect_make(rv.x, rv.y, px(a, 1), rv.h), color_hex(T_BORDER));
        Rect hd = rect_make(rv.x + px(a, 12), rv.y, rv.w - px(a, 24), px(a, 30));
        char title[96];
        if (g->sel_r >= 0) snprintf(title, sizeof title, "RECORD  \xC2\xB7  row %d of %d", g->sel_r + 1, g->nvis);
        else snprintf(title, sizeof title, "RECORD  \xC2\xB7  select a row");
        draw_text_fit(a->font_small, rr, title, -1, hd, color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        Rect bx = rect_make(rv.x + rv.w - px(a, 30), rv.y + px(a, 5), px(a, 20), px(a, 20));
        if (ui_button(a, bx, "", BTN_QUIET, true)) v->rv_open = false;
        draw_cross(rr, bx.x + bx.w / 2, bx.y + bx.h / 2, px(a, 8), px(a, 1.5f), color_hex(T_TEXT_MUTED));
        Rect list = rect_make(rv.x, rv.y + px(a, 30), rv.w, rv.h - px(a, 30));
        draw_record_list(r, v, a, list);
    }
}

/* The record view list (US-022): every column of the selected row as name, type and value.
 * Drawn inside the grid (embedded mode) or as the Inspector panel (docking). */
static void draw_record_list(Results *r, ResultView *v, App *a, Rect list) {
    SDL_Renderer *rr = a->renderer;
    Input *in = &a->in;
    Grid *g = &v->g;
    if (rect_contains(list, in->mouse_x, in->mouse_y) && in->wheel_y != 0) { v->rv_scroll -= in->wheel_y; in->wheel_y = 0; }
    float lh = px(a, 40);
    float maxs = fmaxf(0, g->ncols * lh + px(a, 8) - list.h);
    v->rv_scroll = clampf(v->rv_scroll, 0, maxs);
    ui_scrollbar_input(a, &v->rv_sb, list, g->ncols * lh + px(a, 8), &v->rv_scroll);
    clip_push(rr, list);
    if (g->sel_r >= 0) {
        float name_w = px(a, 110);
        for (int c = 0; c < g->ncols; c++) {
            float y = list.y + px(a, 4) + c * lh - v->rv_scroll;
            if (y + lh < list.y || y > list.y + list.h) continue;
            Rect rowr = rect_make(list.x, y, list.w, lh);
            bool curc = c == g->sel_c;
            if (curc) draw_rect(rr, rowr, color_alpha(color_hex(T_SELECTED), 120));
            if (rect_contains(rowr, in->mouse_x, in->mouse_y) && in->mouse_pressed && !in->mouse_consumed) {
                in->mouse_consumed = true; a->focus = FOCUS_RESULTS;
                if (in->clicks >= 2) open_viewer(r, v, g->sel_r, c); else grid_select_cell(g, g->sel_r, c, false);
            }
            draw_text_fit(a->font_small, rr, grid_col_name(g, c), -1, rect_make(rowr.x + px(a, 12), rowr.y + px(a, 2), name_w - px(a, 16), px(a, 18)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
            draw_text_fit(a->font_small, rr, grid_col_type(g, c), -1, rect_make(rowr.x + px(a, 12), rowr.y + px(a, 20), name_w - px(a, 16), px(a, 16)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
            const char *val = grid_cell(g, g->sel_r, c);
            char tmp2[64];
            const char *shown = grid_display(g, g->sel_r, c, tmp2, sizeof tmp2);
            /* a pending edit wins over the stored value, marked as unsaved (never shown as if applied) */
            int pst = 0; const char *pv = v->ed_init ? edit_pending_value(&v->ed, g->order[g->sel_r], grid_col(g, c), &pst) : NULL;
            bool pending = pst == 1 || pst == 2;
            if (pending) { val = pst == 2 ? NULL : pv; shown = val ? val : ""; }
            Rect vr = rect_make(rowr.x + name_w, rowr.y, rowr.w - name_w - px(a, 12), lh);
            uint32_t dim = pending ? T_YELLOW : T_TEXT_DIM;
            if (!val) draw_text_fit(a->font_mono, rr, "NULL", -1, vr, color_hex(dim), ALIGN_LEFT);
            else if (!*val) draw_text_fit(a->font_mono, rr, "''", -1, vr, color_hex(dim), ALIGN_LEFT);
            else draw_text_fit(a->font_mono, rr, shown, -1, vr, color_hex(pending ? T_YELLOW : shown == tmp2 && grid_col_kind(g, c) == CK_BOOL ? T_SYN_KEYWORD : T_TEXT), ALIGN_LEFT);
            if (pending) draw_text_fit(a->font_small, rr, "unsaved", -1, rect_make(vr.x + vr.w - px(a, 60), vr.y, px(a, 58), vr.h), color_hex(T_YELLOW), ALIGN_RIGHT);
            draw_rect(rr, rect_make(rowr.x + px(a, 12), rowr.y + lh - px(a, 1), rowr.w - px(a, 24), px(a, 1)), color_alpha(color_hex(T_BORDER), 120));
        }
    }
    clip_pop(rr);
    ui_scrollbar_draw(a, &v->rv_sb, list, g->ncols * lh + px(a, 8), v->rv_scroll);
}

/* Inspector panel (LUME-PANEL-001, "Inspector / Record View"): the selected row of the current
 * result tab; says what to do when there is nothing to show. */
void results_draw_inspector(Results *r, App *a, Rect rc) {
    SDL_Renderer *rr = a->renderer;
    draw_rect(rr, rc, color_hex(T_PANEL_ALT));
    ResultView *v = cur(r);
    const char *why = !live(v) || !v->rs.has_rows ? "No result rows in the current tab" : v->g.sel_r < 0 ? "Select a row in Results to inspect it" : NULL;
    char sub[96];
    if (!why) snprintf(sub, sizeof sub, "#%d  \xC2\xB7  row %d of %d", v->exec_id, v->g.sel_r + 1, v->g.nvis);
    draw_text_fit(a->font_small, rr, why ? why : sub, -1, rect_make(rc.x + px(a, 12), rc.y, rc.w - px(a, 24), px(a, 28)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    if (why) return;
    draw_record_list(r, v, a, rect_make(rc.x, rc.y + px(a, 28), rc.w, rc.h - px(a, 28)));
}

/* ---- overlays: drawing ------------------------------------------------- */

/* Dims the window and draws a dialog frame with a soft shadow at p. */
static void panel_frame(App *a, Rect p) {
    SDL_Renderer *r = a->renderer;
    draw_rect(r, rect_make(0, 0, (float)a->width, (float)a->height), color_alpha(color_hex(0x000000), 110));
    for (int s = 3; s >= 1; s--)
        draw_rrect(r, rect_make(p.x - s * px(a, 2), p.y + s * px(a, 2), p.w + s * px(a, 4), p.h + s * px(a, 2)), px(a, 10), color_alpha(color_hex(0x000000), 40));
    draw_rrect(r, p, px(a, 10), color_hex(T_BORDER));
    draw_rrect(r, rect_inset(p, px(a, 1), px(a, 1)), px(a, 9), color_hex(T_PANEL_ALT));
}

/* Draws the JSON Tree view of the value viewer: one line per visible node (up to 4,096),
 * indented by depth, with fold chevrons, keys and syntax-coloured values, plus a scrollbar. */
static void draw_json_tree(Results *r, App *a, Rect inner) {
    SDL_Renderer *rr = a->renderer;
    Json *j = &r->vv_json;
    float lh = font_height(a->font_mono) * 1.4f;
    int ids[4096];
    int total = json_lines(r, ids, 4096);
    int shown = total < 4096 ? total : 4096;
    float y0 = inner.y + px(a, 8) - r->vv_scroll;
    int first = (int)floorf(r->vv_scroll / lh);
    if (first < 0) first = 0;
    char buf[200];
    for (int li = first; li < shown; li++) {
        float y = y0 + li * lh;
        if (y > inner.y + inner.h) break;
        JsonNode *nd = &j->nodes[ids[li]];
        float x = inner.x + px(a, 8) + nd->depth * px(a, 18);
        bool container = nd->type == JN_OBJECT || nd->type == JN_ARRAY;
        bool folded = container && r->vv_collapsed[ids[li]];
        if (container && nd->nchildren > 0) draw_chevron(rr, x - px(a, 2), y + lh / 2, px(a, 7), px(a, 1.5f), !folded, color_hex(T_TEXT_MUTED));
        x += px(a, 10);
        if (nd->key_len) {
            json_unescape(j->src + nd->key_off, nd->key_len, buf, 80);
            float kw = font_width(a->font_mono, buf, -1);
            font_draw(a->font_mono, rr, buf, -1, x, y + (lh - font_height(a->font_mono)) / 2, color_hex(T_SYN_FUNC));
            x += kw;
            font_draw(a->font_mono, rr, ": ", 2, x, y + (lh - font_height(a->font_mono)) / 2, color_hex(T_TEXT_MUTED));
            x += font_width(a->font_mono, ": ", 2);
        }
        Color c = color_hex(T_TEXT);
        if (container) {
            if (folded || nd->nchildren == 0) snprintf(buf, sizeof buf, nd->type == JN_OBJECT ? "{%d}" : "[%d]", nd->nchildren);
            else snprintf(buf, sizeof buf, nd->type == JN_OBJECT ? "{" : "[");
            c = color_hex(T_TEXT_MUTED);
        } else if (nd->type == JN_NULL) {
            snprintf(buf, sizeof buf, "null");
            c = color_hex(T_TEXT_DIM);          /* same tone as SQL NULL in the grid */
        } else if (nd->type == JN_STRING) {
            char raw[160];
            json_unescape(j->src + nd->val_off, nd->val_len, raw, sizeof raw);
            snprintf(buf, sizeof buf, "\"%s\"", raw);
            c = color_hex(T_SYN_STRING);
        } else if (nd->type == JN_NUMBER) {
            snprintf(buf, sizeof buf, "%.*s", (int)(nd->val_len < 150 ? nd->val_len : 150), j->src + nd->val_off);
            c = color_hex(T_SYN_NUMBER);
        } else { snprintf(buf, sizeof buf, "%.*s", (int)nd->val_len, j->src + nd->val_off); c = color_hex(T_SYN_KEYWORD); }
        draw_text_fit(a->font_mono, rr, buf, -1, rect_make(x, y, inner.x + inner.w - x, lh), c, ALIGN_LEFT);
    }
    float total_h = total * lh + px(a, 16);
    float maxs = fmaxf(0, total_h - inner.h);
    if (r->vv_scroll > maxs) r->vv_scroll = maxs;
    r->vv_inner = inner; r->vv_total = total_h;
    ui_scrollbar_draw(a, &r->vv_sb, inner, total_h, r->vv_scroll);
    if (total > 4096) {
        char note[80];
        snprintf(note, sizeof note, "%d lines; fold nodes to navigate further", total);
        draw_text_fit(a->font_small, rr, note, -1, rect_make(inner.x, inner.y + inner.h - px(a, 20), inner.w, px(a, 20)), color_hex(T_YELLOW), ALIGN_RIGHT);
    }
}

/* Draws every open overlay of Results above all panels (overlay pass): filter popover, columns
 * panel, export / review / chart / close dialogs, toolbar tooltip, inline cell editor, value
 * viewer, then the context menu. Mouse presses are masked so widgets only draw; the frame
 * input is restored at the end. */
void results_draw_overlay(Results *r, App *a) {
    SDL_Renderer *rr = a->renderer;
    ResultView *v = cur(r);
    Input saved = a->in;
    a->in.mouse_pressed = false;          /* draw-only pass for widgets */
    /* filter popover (its input ran in filter_update) */
    if (r->fp_open && live(v)) {
        Rect panel, scope, ops1, ops2, v1, v2, apply, cancel;
        filter_layout(r, v, a, &panel, &scope, &ops1, &ops2, &v1, &v2, &apply, &cancel);
        for (int sh = 3; sh >= 1; sh--)
            draw_rrect(rr, rect_make(panel.x - sh * px(a, 2), panel.y + sh * px(a, 2), panel.w + sh * px(a, 4), panel.h + sh * px(a, 2)), px(a, 10), color_alpha(color_hex(0x000000), 40));
        draw_rrect(rr, panel, px(a, 10), color_hex(T_BORDER));
        draw_rrect(rr, rect_inset(panel, px(a, 1), px(a, 1)), px(a, 9), color_hex(T_PANEL_ALT));
        char title[200];
        snprintf(title, sizeof title, "Filter %s", grid_col_name(&v->g, r->fp_col));
        draw_text_fit(a->font_ui, rr, title, -1, rect_make(panel.x + px(a, 20), panel.y + px(a, 10), panel.w - px(a, 40), px(a, 30)), color_hex(T_TEXT), ALIGN_LEFT);
        if (scope.w > 0) ui_segmented(a, scope, scope_labels, 2, r->fp_server ? 1 : 0);
        ui_segmented(a, ops1, ops_a, 4, r->fp_op < 4 ? r->fp_op : -1);
        ui_segmented(a, ops2, ops_b, 4, r->fp_op >= 4 ? r->fp_op - 4 : -1);
        bool needs_value = r->fp_op != OP_IS_NULL && r->fp_op != OP_NOT_NULL;
        if (needs_value) {
            ui_textfield(a, &r->fp_v1, v1, "value", r->fp_focus == 0);
            if (r->fp_focus == 0) draw_rect_outline(rr, v1, px(a, 1), color_hex(T_ACCENT));
            if (r->fp_op == OP_BETWEEN) { ui_textfield(a, &r->fp_v2, v2, "and", r->fp_focus == 1); if (r->fp_focus == 1) draw_rect_outline(rr, v2, px(a, 1), color_hex(T_ACCENT)); }
        }
        ui_button(a, apply, r->fp_server ? "Re-run" : "Apply", BTN_PRIMARY, true);
        ui_button(a, cancel, "Cancel", BTN_NORMAL, true);
    }
    /* dialogs, the toolbar tooltip and the inline cell editor */
    if (r->cp_open && live(v)) columns_panel(r, v, a, false);
    if (r->ex_open && live(v)) export_dialog(r, v, a, false);
    if (r->rv_review && live(v)) review_dialog(r, v, a, false);
    if (r->chart_open && live(v)) chart_panel(r, v, a, false);
    if (r->cf_open) close_confirm(r, a, false);
    if (r->menu.open || r->cf_open || r->ex_open || a->in.mouse_pressed) r->tt_show = false;
    if (r->tt_show && r->tt_text) {
        float w = font_width(a->font_small, r->tt_text, -1) + px(a, 16), h = px(a, 24);
        float x = fminf(r->tt_rect.x, a->width - w - px(a, 8)), y = r->tt_rect.y + r->tt_rect.h + px(a, 6);
        draw_rrect(rr, rect_make(x, y, w, h), px(a, 5), color_hex(T_BORDER));
        draw_rrect(rr, rect_make(x + px(a, 1), y + px(a, 1), w - px(a, 2), h - px(a, 2)), px(a, 4), color_hex(T_BG));
        draw_text_fit(a->font_small, rr, r->tt_text, -1, rect_make(x + px(a, 8), y, w - px(a, 16), h), color_hex(T_TEXT), ALIGN_LEFT);
    }
    if (r->ce_open && live(v) && r->ce_row < v->g.nvis && v->rowy_n == v->g.nvis) {
        Rect cr = cell_rect(r, v, r->ce_row, r->ce_col);
        Rect fr = rect_make(cr.x, cr.y, fmaxf(cr.w, px(a, 120)), fmaxf(cr.h, px(a, 26)));
        draw_rrect(rr, rect_inset(fr, -px(a, 2), -px(a, 2)), px(a, 4), color_hex(T_YELLOW));
        ui_textfield(a, &r->ce, fr, "", true);
        draw_text_fit(a->font_small, rr, "\xE2\x86\xA9 keeps  \xC2\xB7  Tab next  \xC2\xB7  Esc cancels  \xC2\xB7  pending until applied", -1, rect_make(fr.x, fr.y + fr.h + px(a, 2), px(a, 320), px(a, 18)), color_hex(T_YELLOW), ALIGN_LEFT);
    }
    /* value viewer: title, view switch, body by kind, the note under it, buttons */
    if (r->vv_open && live(v)) {
        Rect panel = viewer_panel(r, v, a);
        panel_frame(a, panel);
        Grid *g = &v->g;
        const char *val = grid_cell(g, r->vv_row, r->vv_col);
        char fmt[64];
        ViewerLayout L = viewer_layout(r, v, a, panel, fmt, sizeof fmt);
        char title[360], dt[96];
        int col = r->vv_col, src = g->order[r->vv_row] + 1;
        const char *type = grid_col_type(g, col)[0] ? grid_col_type(g, col) : "no declared type";
        if (val && r->vv_kind == 2) snprintf(title, sizeof title, "%s  \xC2\xB7  %s  \xC2\xB7  %ld bytes  \xC2\xB7  row %d (source row %d)", grid_col_name(g, col), type, r->vv_nbytes, r->vv_row + 1, src);
        else if (val) snprintf(title, sizeof title, "%s  \xC2\xB7  %s  \xC2\xB7  %zu bytes  \xC2\xB7  row %d (source row %d)", grid_col_name(g, col), type, strlen(val), r->vv_row + 1, src);
        else snprintf(title, sizeof title, "%s  \xC2\xB7  %s  \xC2\xB7  row %d (source row %d)", grid_col_name(g, col), type, r->vv_row + 1, src);
        if (val && grid_describe_datetime(val, dt, sizeof dt)) { strncat(title, "  \xC2\xB7  ", sizeof title - strlen(title) - 1); strncat(title, dt, sizeof title - strlen(title) - 1); }

        draw_text_fit(a->font_small, rr, title, -1, rect_make(panel.x + px(a, 24), panel.y + px(a, 12), panel.w - px(a, 48) - (r->vv_kind ? px(a, 200) : 0), px(a, 24)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        if (r->vv_json_ok) {
            static const char *const modes[] = { "Text", "Tree" };
            ui_segmented(a, L.modes, modes, 2, r->vv_mode);
        } else if (r->vv_kind == 2) {
            static const char *const bmodes[] = { "Hex", "Base64", "Image" };
            ui_segmented(a, L.modes, bmodes, r->vv_tex ? 3 : 2, r->vv_mode);
        } else if (r->vv_kind == 3) {
            static const char *const gmodes[] = { "Text", "Geometry" };
            ui_segmented(a, L.modes, gmodes, 2, r->vv_mode);
        }
        draw_text_fit(a->font_small, rr, r->vv_json_ok && r->vv_mode == 1 ? "Click a node to fold  \xC2\xB7  \xE2\x8C\x98" "C copies raw  \xC2\xB7  Esc closes" : "\xE2\x8C\x98" "C copies raw  \xC2\xB7  Esc closes", -1, rect_make(panel.x + px(a, 24), panel.y + panel.h - px(a, 24) - px(a, 30), px(a, 320), px(a, 30)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        Rect bodyr = L.body;
        draw_rrect(rr, bodyr, px(a, M_RADIUS), color_hex(T_BG));
        Rect inner = rect_inset(bodyr, px(a, 12), px(a, 10));
        clip_push(rr, inner);
        if (!val) draw_text(a->font_mono, rr, "NULL", inner.x, inner.y, color_hex(T_TEXT_DIM));
        else if (!*val) draw_text(a->font_mono, rr, "'' (empty string)", inner.x, inner.y, color_hex(T_TEXT_DIM));
        else if (r->vv_json_ok && r->vv_mode == 1) draw_json_tree(r, a, inner);
        else if (r->vv_kind == 2 && r->vv_bytes && r->vv_mode == 2 && r->vv_tex) {
            /* fit to the box; tiny images are magnified with nearest sampling so pixels stay pixels */
            float sc = fminf(inner.w / r->vv_tex_w, inner.h / r->vv_tex_h);
            if (sc > 1) { sc = fminf(sc, fminf(inner.w, inner.h) * 0.5f / fmaxf(r->vv_tex_w, r->vv_tex_h)); if (sc < 1) sc = 1; SDL_SetTextureScaleMode(r->vv_tex, SDL_SCALEMODE_NEAREST); }
            SDL_FRect dst = { inner.x + (inner.w - r->vv_tex_w * sc) / 2, inner.y + (inner.h - r->vv_tex_h * sc) / 2, r->vv_tex_w * sc, r->vv_tex_h * sc };
            /* a 1 px outline so dark pixels do not vanish into the canvas */
            draw_rect_outline(rr, rect_make(dst.x - px(a, 1), dst.y - px(a, 1), dst.w + px(a, 2), dst.h + px(a, 2)), px(a, 1), color_hex(T_TEXT_DIM));
            SDL_RenderTexture(rr, r->vv_tex, NULL, &dst);
            char zl[48]; snprintf(zl, sizeof zl, "%dx%d shown at %.0f%%", r->vv_tex_w, r->vv_tex_h, sc * 100);
            draw_text_fit(a->font_small, rr, zl, -1, rect_make(inner.x, inner.y + inner.h - px(a, 18), inner.w, px(a, 18)), color_hex(T_TEXT_DIM), ALIGN_RIGHT);
        } else if (r->vv_kind == 2 && r->vv_bytes) {
            /* hex dump or base64 of the first 64 KB; the rest is described, not drawn */
            long show = r->vv_nbytes < 65536 ? r->vv_nbytes : 65536;
            float lh = font_height(a->font_mono) * 1.35f;
            if (r->vv_mode == 0) {
                long lines = (show + 15) / 16;
                int first = (int)(r->vv_scroll / lh);
                char line[96];
                for (long li = first; li < lines; li++) {
                    float y = inner.y + li * lh - r->vv_scroll;
                    if (y > inner.y + inner.h) break;
                    long off = li * 16;
                    int p = snprintf(line, sizeof line, "%08lx  ", off);
                    for (int k = 0; k < 16; k++) { if (off + k < show) p += snprintf(line + p, sizeof line - p, "%02x ", r->vv_bytes[off + k]); else p += snprintf(line + p, sizeof line - p, "   "); if (k == 7) p += snprintf(line + p, sizeof line - p, " "); }
                    p += snprintf(line + p, sizeof line - p, " |");
                    for (int k = 0; k < 16 && off + k < show; k++) { unsigned char ch = r->vv_bytes[off + k]; line[p++] = ch >= 32 && ch < 127 ? (char)ch : '.'; }
                    line[p++] = '|'; line[p] = 0;
                    font_draw(a->font_mono, rr, line, -1, inner.x, y, color_hex(T_TEXT));
                }
                float total = lines * lh, maxs = fmaxf(0, total - inner.h);
                if (r->vv_scroll > maxs) r->vv_scroll = maxs;
            } else {
                size_t cap = (size_t)show * 4 / 3 + 8;
                char *b64 = malloc(cap);
                base64_encode(r->vv_bytes, show, b64, cap);
                float total = draw_wrapped(a->font_mono, rr, b64, inner, color_hex(T_TEXT), r->vv_scroll, true);
                free(b64);
                float maxs = fmaxf(0, total - inner.h);
                if (r->vv_scroll > maxs) r->vv_scroll = maxs;
            }
            if (show < r->vv_nbytes) draw_text_fit(a->font_small, rr, "first 64 KB shown", -1, rect_make(inner.x, inner.y + inner.h - px(a, 18), inner.w, px(a, 18)), color_hex(T_YELLOW), ALIGN_RIGHT);
        } else if (r->vv_kind == 3 && r->vv_mode == 1) {
            /* geometry drawn as stored: x right, y up, uniform scale, no reprojection */
            Geo *ge = &r->vv_geo;
            double spanx = ge->maxx - ge->minx, spany = ge->maxy - ge->miny;
            if (spanx <= 0) spanx = 1; if (spany <= 0) spany = 1;
            float pad = px(a, 20);
            double sc = fmin((inner.w - 2 * pad) / spanx, (inner.h - 2 * pad) / spany);
            float ox = inner.x + pad + (float)(((inner.w - 2 * pad) - spanx * sc) / 2), oy = inner.y + pad + (float)(((inner.h - 2 * pad) - spany * sc) / 2);
            #define GX(px_) (ox + (float)(((px_) - ge->minx) * sc))
            #define GY(py_) (oy + (float)((ge->maxy - (py_)) * sc))
            Color c = color_hex(T_GREEN);
            for (int p = 0; p < ge->nparts; p++) {
                int s = ge->part_start[p], e = p + 1 < ge->nparts ? ge->part_start[p + 1] : ge->npts;
                if (e - s == 1 || ge->type == GEO_POINT || ge->type == GEO_MULTIPOINT) {
                    for (int i = s; i < e; i++) draw_rrect(rr, rect_make(GX(ge->pts[i].x) - px(a, 4), GY(ge->pts[i].y) - px(a, 4), px(a, 8), px(a, 8)), px(a, 4), c);
                } else for (int i = s + 1; i < e; i++) draw_line(rr, GX(ge->pts[i - 1].x), GY(ge->pts[i - 1].y), GX(ge->pts[i].x), GY(ge->pts[i].y), px(a, 2), c);
            }
            #undef GX
            #undef GY
            char lbl[160];
            snprintf(lbl, sizeof lbl, "x %.6g .. %.6g   y %.6g .. %.6g", ge->minx, ge->maxx, ge->miny, ge->maxy);
            draw_text_fit(a->font_small, rr, lbl, -1, rect_make(inner.x, inner.y + inner.h - px(a, 18), inner.w, px(a, 18)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
            /* what the drawing is (US-059): type, SRID, size, source form and the axis meaning */
            char gd[240];
            geo_describe(ge, gd, sizeof gd);
            draw_wrapped(a->font_small, rr, gd, rect_make(inner.x, inner.y, inner.w, px(a, 40)), color_hex(ge->truncated ? T_YELLOW : T_TEXT_DIM), 0, true);
        }
        else {
            float total = draw_wrapped(a->font_mono, rr, val, inner, color_hex(T_TEXT), r->vv_scroll, true);
            float maxs = fmaxf(0, total - inner.h);
            if (r->vv_scroll > maxs) r->vv_scroll = maxs;
            if (maxs > 0) {
                float thumb = fmaxf(px(a, 24), inner.h * inner.h / total);
                float ty = inner.y + (inner.h - thumb) * (r->vv_scroll / maxs);
                draw_rrect(rr, rect_make(bodyr.x + bodyr.w - px(a, 8), ty, px(a, 4), thumb), px(a, 2), color_hex(T_SCROLLBAR));
            }
        }
        clip_pop(rr);
        Rect noter = rect_make(panel.x + px(a, 24), bodyr.y + bodyr.h + px(a, 6), panel.w - px(a, 48) - px(a, 300), px(a, 24));   /* clear of the buttons */
        if (val && strcmp(val, "<lob>") == 0)
            draw_text_fit(a->font_small, rr, "Large object: the driver did not fetch its content", -1, noter, color_hex(T_YELLOW), ALIGN_LEFT);
        else if (r->vv_kind == 2 && r->vv_img_note[0])
            draw_text_fit(a->font_small, rr, r->vv_img_note, -1, noter, color_hex(r->vv_tex ? T_TEXT_DIM : T_YELLOW), ALIGN_LEFT);
        else if (val && cell_is_link(val))
            draw_text_fit(a->font_small, rr, "Link: open it from the cell's context menu (never automatically)", -1, noter, color_hex(T_TEXT_DIM), ALIGN_LEFT);
        else if (val && r->vv_json_tried && !r->vv_json_ok && grid_col_kind(g, col) == CK_JSON) {
            char e[160];
            snprintf(e, sizeof e, "Not valid JSON: %s", r->vv_json.err);
            draw_text_fit(a->font_small, rr, e, -1, noter, color_hex(T_YELLOW), ALIGN_LEFT);
        } else if (val && r->vv_json_tried && !r->vv_json_ok && (val[0] == '{' || val[0] == '[')) {
            char e[160];
            snprintf(e, sizeof e, "Looks like JSON but is not valid: %s", r->vv_json.err);
            draw_text_fit(a->font_small, rr, e, -1, noter, color_hex(T_TEXT_DIM), ALIGN_LEFT);
        } else if (L.has_fmt) {
            char e[200];
            snprintf(e, sizeof e, "Displayed as %s (Format menu); the raw value above is what the database returned", fmt);
            draw_text_fit(a->font_small, rr, e, -1, noter, color_hex(T_TEXT_DIM), ALIGN_LEFT);
        }
        ui_button(a, L.close, "Close", BTN_PRIMARY, true);
        ui_button(a, L.copy_raw, "Copy raw", BTN_NORMAL, true);
        if (L.has_fmt) ui_button(a, L.copy_fmt, "Copy formatted", BTN_NORMAL, true);
    }
    a->in = saved;
    popup_draw(a, &r->menu);
}

/* ---- Execution Output integration (OUT-001 / OUT-009) ------------------------- */

/* Shows (on) or leaves the Output tab. With docking, only records a request for the shell. */
void results_show_output(Results *r, bool on) {
    if (r->panels_external) { if (on) r->want_output = true; else r->want_results = true; return; }   /* the shell shows the panel */
    r->output_active = on; if (on) { r->vv_open = r->fp_open = r->cp_open = false; }
}
/* Docking on: Output and the record view are panels of their own, not drawn by Results. */
void results_set_panels_external(Results *r, bool on) { r->panels_external = on; if (on) r->output_active = false; }
/* Shell tells whether the Inspector panel is visible (for the Show / Hide Record View label). */
void results_set_inspector_shown(Results *r, bool shown) { r->inspector_shown = shown; }
/* Hands the shell the show-Output / show-Results / toggle-Inspector requests of this frame and
 * clears them. */
void results_take_requests(Results *r, bool *output, bool *results, bool *inspector_toggle) {
    *output = r->want_output; *results = r->want_results; *inspector_toggle = r->inspector_toggle;
    r->want_output = r->want_results = r->inspector_toggle = false;
}

/* What the panel bar and the collapsed strip show for Results (PNL-025 / 029 / 037). */
void results_badge(Results *r, int *tabs, int *unsaved_rows, bool *error) {
    *tabs = 0; *unsaved_rows = 0; *error = false;
    for (int i = 0; i < r->ntabs; i++) {
        ResultView *v = r->tabs[i];
        (*tabs)++;
        if (tab_unsaved(v) > 0) *unsaved_rows += edit_pending_rows(&v->ed);
        if (i == r->cur && v->has && !v->discarded && !v->rs.ok && !v->rs.cancelled) *error = true;
    }
}
/* True while the Output tab is the one shown. */
bool results_output_active(Results *r) { return r->output_active; }
/* exec_id of the current tab, 0 when none. */
int results_current_exec_id(Results *r) { ResultView *v = cur(r); return v ? v->exec_id : 0; }
void results_target_replace(Results *r, int exec_id) { r->replace_target = exec_id; }
/* See results.h: the SQL and session behind the current tab (Report Studio, LI-031). */
const char *results_current_sql(Results *r, struct Db **db) { ResultView *v = cur(r); if (db) *db = v ? v->db : NULL; return v && v->sql && v->sql_len > 0 ? v->sql : NULL; }
/* Selects the tab with this exec_id (and asks the shell to show Results when docked); true
 * when found and still holding its rows. */
/* See results.h: result tabs for the toolbar's Switch Tab (TBR-035). */
int results_tab_count(Results *r) { return r->ntabs; }
bool results_tab_info(Results *r, int i, int *exec_id, char *title, size_t n, bool *pinned, bool *current) {
    if (i < 0 || i >= r->ntabs) return false;
    ResultView *v = r->tabs[i];
    *exec_id = v->exec_id; *pinned = v->pinned; *current = i == r->cur;
    snprintf(title, n, "%s", v->title);
    return true;
}

bool results_select_exec(Results *r, int exec_id) {
    for (int i = 0; i < r->ntabs; i++) if (r->tabs[i]->exec_id == exec_id) { r->cur = i; r->output_active = false; if (r->panels_external) r->want_results = true; return live(r->tabs[i]); }
    return false;
}
/* Links the tab with exec_id tab_exec to the Output execution out_exec. */
void results_tag_output(Results *r, int tab_exec, int out_exec) {
    for (int i = 0; i < r->ntabs; i++) if (r->tabs[i]->exec_id == tab_exec) r->tabs[i]->out_exec = out_exec;
}
/* Output execution id of the current tab, 0 when none. */
int results_output_of_current(Results *r) { ResultView *v = cur(r); return v ? v->out_exec : 0; }
/* Public wrapper of draw_wrapped for output.c and panels.c. */
float results_draw_wrapped(Font *f, SDL_Renderer *rr, const char *str, Rect rc, Color c, float scroll, bool draw) { return draw_wrapped(f, rr, str, rc, c, scroll, draw); }

bool results_current_info(Results *r, char *err, size_t en, char *sql, size_t sn, char *conn, size_t cn, char *cols, size_t coln) {
    ResultView *v = r ? cur(r) : NULL;
    if (en) err[0] = 0; if (sn) sql[0] = 0; if (cn) conn[0] = 0; if (coln) cols[0] = 0;
    if (!v || !v->has) return false;
    snprintf(err, en, "%s", v->rs.error);
    if (v->sql) snprintf(sql, sn, "%.*s", v->sql_len, v->sql);
    snprintf(conn, cn, "%s", v->conn);
    size_t w = 0;
    for (int i = 0; i < v->rs.ncols && w + 8 < coln; i++)
        w += (size_t)snprintf(cols + w, coln - w, "%s%s %s", i ? ", " : "", v->rs.cols[i] ? v->rs.cols[i] : "?", v->rs.types && v->rs.types[i] ? v->rs.types[i] : "");
    return true;
}

static int cmp_cell(const char *a, const char *b) {
    char *ea, *eb; double x = strtod(a, &ea), y = strtod(b, &eb);
    if (*a && *b && !*ea && !*eb) return x < y ? -1 : x > y;
    return strcmp(a, b);
}
bool results_current_summary(Results *r, char *out, size_t n) {
    ResultView *v = r ? cur(r) : NULL;
    if (n) out[0] = 0;
    if (!v || !v->has || !v->rs.ncols) return false;
    const ResultSet *rs = &v->rs;
    size_t w = (size_t)snprintf(out, n, "rows loaded: %d%s\n", rs->nrows, rs->truncated && !rs->complete ? " (more exist)" : "");
    for (int c = 0; c < rs->ncols && c < 40 && w + 120 < n; c++) {
        int nulls = 0, distinct = 0; const char *mn = NULL, *mx = NULL;
        const char *seen[1000]; int ns = 0;
        for (int i = 0; i < rs->nrows; i++) {
            const char *x = rs_cell(rs, i, c);
            if (!x) { nulls++; continue; }
            if (!mn || cmp_cell(x, mn) < 0) mn = x;
            if (!mx || cmp_cell(x, mx) > 0) mx = x;
            if (ns < 1000) { bool f = false; for (int k = 0; k < ns && !f; k++) f = !strcmp(seen[k], x); if (!f) { seen[ns++] = x; distinct++; } }
        }
        char a[60] = "", b[60] = "";
        if (mn) snprintf(a, sizeof a, "%.50s", mn); if (mx) snprintf(b, sizeof b, "%.50s", mx);
        for (char *p = a; *p; p++) if (*p == '\n' || *p == '<') *p = ' ';
        for (char *p = b; *p; p++) if (*p == '\n' || *p == '<') *p = ' ';
        w += (size_t)snprintf(out + w, n - w, "%s (%s): nulls %d, distinct %d%s, min %s, max %s\n", rs->cols[c] ? rs->cols[c] : "?", rs->types && rs->types[c] ? rs->types[c] : "", nulls, distinct, ns >= 1000 ? "+" : "", mn ? a : "-", mx ? b : "-");
    }
    return true;
}

void results_set_owner(Results *r, const void *owner) { r->next_owner = owner; }
/* A query tab starts a run: its earlier results go (pinned ones and ones with unsaved edits stay) */
void results_begin_run(Results *r, App *a, const void *owner) {
    if (!owner) return;
    for (int i = r->ntabs - 1; i >= 0; i--) {
        ResultView *v = r->tabs[i];
        if (v->owner != owner || v->pinned || tab_unsaved(v) > 0) continue;
        /* a cursor still open on a session that is busy (another run on it) is never closed from here */
        if (v->rs.cursor && v->rs.db && __atomic_load_n(&v->rs.db->busy, __ATOMIC_SEQ_CST)) continue;
        tab_remove(r, a, i);
    }
    r->view_owner = owner;
}
/* the active query tab changed: show its last run's results */
void results_follow_tab(Results *r, const void *owner) {
    if (owner == r->view_owner) return;
    r->view_owner = owner;
    int best = -1;
    for (int i = 0; i < r->ntabs; i++) if (visible_v(r, r->tabs[i]) && (best < 0 || r->tabs[i]->exec_id > r->tabs[best]->exec_id)) best = i;
    if (best >= 0) r->cur = best;
    r->vv_open = r->fp_open = r->cp_open = r->rn_open = false;
}

void results_dump(Results *r) {
    int n = 0; for (int i = 0; i < r->ntabs; i++) if (visible_v(r, r->tabs[i])) n++;
    ResultView *c = cur(r);
    printf("resstate shown=%d total=%d cur=#%d scroll=%d", n, r->ntabs, c ? c->exec_id : 0, c ? (int)c->scroll_ty : 0);
    for (int i = 0; i < r->ntabs; i++) if (visible_v(r, r->tabs[i])) printf(" #%d:%d", r->tabs[i]->exec_id, r->tabs[i]->rs.nrows);
    printf("\n"); fflush(stdout);
}
