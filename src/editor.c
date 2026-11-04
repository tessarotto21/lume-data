/* SPDX-License-Identifier: AGPL-3.0-only */
/* editor.c: the SQL text editor (one Editor per editor tab).
 *
 * Text model: an array of Line, each a heap buffer of UTF-8 bytes without the newline. The
 * caret is (cx, cy) = byte column / line index and the selection runs from the anchor (ax, ay)
 * to the caret; byte columns always sit on code-point boundaries. Whole-text offsets used by
 * the public API count each line break as one byte ('\n').
 *
 * Responsibilities:
 *   - editing, caret movement, word / line selection, clipboard (SDL clipboard);
 *   - undo / redo as whole-buffer snapshots, consecutive typing coalesced into one step;
 *   - "statement under the cursor": the ';'-separated statement around the caret (skipping
 *     strings and comments), highlighted with a band and run by Cmd+Return (main.c);
 *   - SQL syntax highlighting with a per-line tokenizer (block comments carry over lines);
 *   - the autocompletion popup, fed by complete.c; statements the caret leaves are learnt
 *     by the completer even if never executed;
 *   - drawing: optional tab strip with the run hint (when not docked in the panels shell),
 *     gutter with line numbers, text, caret, scrollbar.
 *
 * Frame flow: editor_update() handles this frame's input (App.in) and draws the body; after every
 * panel is drawn, main.c calls editor_draw_overlay() so the completion popup sits on top.
 * Geometry is in render pixels. Main thread only. Strings returned by the public getters are
 * malloc'd and freed by the caller. */
#include "editor.h"
#include "ui.h"
#include "sqllex.h"
#include "complete.h"
#include "draw.h"
#include <strings.h>
#include "font.h"
#include "theme.h"
#include <ctype.h>

#define COMPLETE_ROW 22   /* height of a completion popup row (UI points): drawing and hit tests */

/* One line of text: NUL-terminated buffer s of len bytes (cap allocated), no trailing '\n'. */
typedef struct { char *s; int len, cap; } Line;

/* Undo history: whole-buffer snapshots, coalesced while typing. Query text is small, so this
 * stays cheap; total bytes are capped so a long session cannot grow unbounded. */
typedef struct { char *text; int len, cx, cy; } Snap;
/* Undo / redo limits (steps per stack, bytes over both stacks) and edit kinds used for
 * coalescing: other (never merged), typing, typing whitespace, deleting. */
#define UNDO_MAX 200
#define UNDO_BYTES (2 * 1024 * 1024)
enum { EK_OTHER, EK_TYPE, EK_TYPE_WS, EK_DEL };

struct Editor {
    bool no_strip;      /* docking: the shell draws the editor tabs, this draws the text only */
    /* lines[0..n): the text (always at least one line); cap allocated */
    Line *lines; int n, cap;
    Snap undo[UNDO_MAX]; int nundo;
    Snap redo[UNDO_MAX]; int nredo;
    /* undo_bytes: snapshot bytes held by both stacks; last_edit / last_kind: SDL ticks and EK_*
     * of the previous edit, for coalescing */
    size_t undo_bytes;
    Uint64 last_edit; int last_kind;
    /* completion popup */
    /* cands / ncands: proposals; csel: selected row; cscroll: first visible row; cprefix: bytes
     * before the caret that accepting replaces; copen: popup visible; popup_rect: where it was
     * drawn (render px, for hit testing); popup_x / popup_y: anchor under the prefix; chover:
     * row under the pointer or -1 */
    Cand cands[COMPLETE_MAX]; int ncands, csel, cscroll, cprefix;
    bool copen;
    bool cpending, cexplicit; uint32_t crev; int ccur;   /* the popup waits for columns: recomputed when they arrive if text and caret are unchanged */
    Rect popup_rect;
    float popup_x, popup_y;
    int chover;
    int stmt_first, stmt_last;   /* statement under the cursor last frame, for learning */
    char *stmt_text;             /* copy of that statement, learned when the cursor leaves it */
    /* learned_hash: hash of the last statement handed to the completer (no double learning);
     * stmt_seen_edit: last_edit value when stmt_text was copied */
    uint32_t learned_hash;
    Uint64 stmt_seen_edit;
    int cx, cy;          /* cursor byte column / line */
    int ax, ay;          /* selection anchor */
    float want_x;        /* preferred x (px) for vertical movement, <0 none */
    /* scroll_y: current vertical scroll (px), eased toward the target scroll_ty; scroll_x:
     * horizontal scroll (px); blink_start: SDL ticks the caret blink restarted; dragging: mouse
     * selection in progress; scroll_to_cursor: bring the caret into view next frame; text_rect:
     * text area of the last frame; gutter_w / line_h: last frame's gutter width and line
     * height (px); revision: bumped on every text change (editor_revision) */
    float scroll_y, scroll_ty, scroll_x;
    ScrollBar sb;   /* the overlay scrollbar */
    Uint64 blink_start;
    bool dragging, scroll_to_cursor;
    Rect text_rect;
    float gutter_w, line_h;
    uint32_t revision;
    float widest; uint32_t widest_rev; float widest_lh;   /* widest line (px), for the horizontal scroll limit */
    /* diagnostics drawn under the text (object editor, OBJ-022): valid for marks_rev only */
    EdMark marks[EDITOR_MARKS_MAX]; int nmarks; uint32_t marks_rev;
    bool object_mode;    /* a definition, not a console: no "statement under the cursor" band */
    bool read_only;      /* no edits at all (system / encrypted source) */
    int dialect;         /* lexical rules used for highlighting (sqllex.h DIALECT_*) */
};

/* ---- line storage ------------------------------------------------------ */

/* Make sure l can hold `need` bytes plus the NUL (grows to at least 64 bytes, doubling). */
static void line_reserve(Line *l, int need) {
    if (l->cap >= need + 1) return;
    l->cap = (need + 1) * 2 < 64 ? 64 : (need + 1) * 2;
    l->s = realloc(l->s, l->cap);
}

/* Insert n bytes of s at byte `at` of l. */
static void line_insert(Line *l, int at, const char *s, int n) {
    line_reserve(l, l->len + n);
    memmove(l->s + at + n, l->s + at, l->len - at);
    memcpy(l->s + at, s, n);
    l->len += n;
    l->s[l->len] = 0;
}

/* Remove n bytes at byte `at` of l. */
static void line_erase(Line *l, int at, int n) {
    memmove(l->s + at, l->s + at + n, l->len - at - n);
    l->len -= n;
    l->s[l->len] = 0;
}

/* Insert an empty line at index `at`, shifting the following ones down. */
static void ed_insert_line(Editor *e, int at) {
    if (e->n == e->cap) { e->cap = e->cap ? e->cap * 2 : 64; e->lines = realloc(e->lines, sizeof *e->lines * e->cap); }
    memmove(&e->lines[at + 1], &e->lines[at], sizeof *e->lines * (e->n - at));
    memset(&e->lines[at], 0, sizeof e->lines[at]);
    line_reserve(&e->lines[at], 0);
    e->lines[at].s[0] = 0;
    e->n++;
}

/* Free line `at` and close the gap. */
static void ed_remove_line(Editor *e, int at) {
    free(e->lines[at].s);
    memmove(&e->lines[at], &e->lines[at + 1], sizeof *e->lines * (e->n - at - 1));
    e->n--;
}

/* Create an empty editor (one empty line, no history). Free with editor_destroy(). */
Editor *editor_create(void) {
    Editor *e = calloc(1, sizeof *e);
    ed_insert_line(e, 0);
    e->want_x = -1;
    e->stmt_first = e->stmt_last = -1;
    e->chover = -1;
    return e;
}

static void history_clear(Editor *e);

/* Free the editor, its text, history and learning state. */
void editor_destroy(Editor *e) {
    history_clear(e);
    free(e->stmt_text);
    for (int i = 0; i < e->n; i++) free(e->lines[i].s);
    free(e->lines);
    free(e);
}

/* ---- selection helpers ------------------------------------------------- */

/* Selection helpers: has_sel tells whether anchor and caret differ; collapse drops the selection
 * (anchor = caret); touch restarts the caret blink and asks to scroll the caret into view. */
static bool has_sel(Editor *e) { return e->ax != e->cx || e->ay != e->cy; }
static void collapse(Editor *e) { e->ax = e->cx; e->ay = e->cy; }
static void touch(Editor *e) { e->blink_start = SDL_GetTicks(); e->scroll_to_cursor = true; }

/* Selection as ordered (x0, y0) .. (x1, y1), whichever of anchor / caret comes first. */
static void sel_range(Editor *e, int *x0, int *y0, int *x1, int *y1) {
    bool anchor_first = e->ay < e->cy || (e->ay == e->cy && e->ax < e->cx);
    *x0 = anchor_first ? e->ax : e->cx; *y0 = anchor_first ? e->ay : e->cy;
    *x1 = anchor_first ? e->cx : e->ax; *y1 = anchor_first ? e->cy : e->ay;
}

/* Delete text from (x0, y0) to (x1, y1) (ordered, byte columns), joining lines as needed; the
 * caret lands at the start. Does not record undo: callers call edit_begin first. */
static void delete_range(Editor *e, int x0, int y0, int x1, int y1) {
    e->revision++;
    if (y0 == y1) {
        line_erase(&e->lines[y0], x0, x1 - x0);
    } else {
        Line *a = &e->lines[y0], *b = &e->lines[y1];
        line_erase(a, x0, a->len - x0);
        line_insert(a, a->len, b->s + x1, b->len - x1);
        for (int i = y1; i > y0; i--) ed_remove_line(e, i);
    }
    e->cx = x0; e->cy = y0;
    collapse(e);
}

/* Delete the selected text, if any. */
static void delete_selection(Editor *e) {
    if (!has_sel(e)) return;
    int x0, y0, x1, y1;
    sel_range(e, &x0, &y0, &x1, &y1);
    delete_range(e, x0, y0, x1, y1);
}

/* ---- undo / redo ------------------------------------------------------- */

/* Snapshot helpers: snap_take copies the whole text and caret into s (s->text malloc'd);
 * snap_free releases it. */
static void snap_take(Editor *e, Snap *s) { s->text = editor_text(e, &s->len); s->cx = e->cx; s->cy = e->cy; }
static void snap_free(Snap *s) { free(s->text); s->text = NULL; }

/* Push a snapshot (ownership moves to the stack). Drops the oldest entries when the stack is
 * full or the byte budget UNDO_BYTES is exceeded (always keeps the newest). */
static void stack_push(Editor *e, Snap *st, int *n, Snap s) {
    if (*n == UNDO_MAX) { e->undo_bytes -= st[0].len; snap_free(&st[0]); memmove(st, st + 1, sizeof *st * (UNDO_MAX - 1)); (*n)--; }
    st[(*n)++] = s;
    e->undo_bytes += s.len;
    while (e->undo_bytes > UNDO_BYTES && *n > 1) {
        e->undo_bytes -= st[0].len; snap_free(&st[0]);
        memmove(st, st + 1, sizeof *st * (*n - 1)); (*n)--;
    }
}

/* Drop all undo and redo snapshots. */
static void history_clear(Editor *e) {
    for (int i = 0; i < e->nundo; i++) snap_free(&e->undo[i]);
    for (int i = 0; i < e->nredo; i++) snap_free(&e->redo[i]);
    e->nundo = e->nredo = 0;
    e->undo_bytes = 0;
    e->last_kind = EK_OTHER;
}

/* Call before mutating the buffer. Consecutive edits of the same typing kind within a second
 * share one undo step, so undo removes a word, not a letter. */
static void edit_begin(Editor *e, int kind) {
    Uint64 now = SDL_GetTicks();
    bool coalesce = kind != EK_OTHER && kind == e->last_kind && now - e->last_edit < 1000 && e->nundo > 0;
    e->last_edit = now;
    e->last_kind = kind;
    if (coalesce) return;
    Snap s;
    snap_take(e, &s);
    stack_push(e, e->undo, &e->nundo, s);
    for (int i = 0; i < e->nredo; i++) snap_free(&e->redo[i]);
    e->nredo = 0;
}

static void load_text(Editor *e, const char *text, int len);

/* Replace the text with a snapshot and put the caret back (clamped), without touching history. */
static void snap_apply(Editor *e, Snap *s) {
    load_text(e, s->text, s->len);
    e->cy = s->cy < e->n ? s->cy : e->n - 1;
    e->cx = s->cx <= e->lines[e->cy].len ? s->cx : e->lines[e->cy].len;
    collapse(e);
    e->want_x = -1;
    e->last_edit = SDL_GetTicks();   /* the statement under the cursor changed too */
    touch(e);
}

/* Undo one step: the current state goes onto the redo stack. Closes the completion popup. */
void editor_undo(Editor *e) {
    if (e->read_only) return;
    if (!e->nundo) return;
    Snap cur;
    snap_take(e, &cur);
    stack_push(e, e->redo, &e->nredo, cur);
    Snap s = e->undo[--e->nundo];
    e->undo_bytes -= s.len;
    snap_apply(e, &s);
    snap_free(&s);
    e->last_kind = EK_OTHER;
    e->copen = false;
}

/* Redo one step: the current state goes back onto the undo stack. */
void editor_redo(Editor *e) {
    if (e->read_only) return;
    if (!e->nredo) return;
    Snap cur;
    snap_take(e, &cur);
    stack_push(e, e->undo, &e->nundo, cur);
    Snap s = e->redo[--e->nredo];
    e->undo_bytes -= s.len;
    snap_apply(e, &s);
    snap_free(&s);
    e->last_kind = EK_OTHER;
    e->copen = false;
}

/* ---- editing ----------------------------------------------------------- */

/* Insert n bytes of s (n < 0: NUL-terminated) at the caret, replacing the selection. '\n' splits
 * lines, '\r' is dropped (CRLF / CR normalised). No undo record. Bumps the revision: typed text
 * is a change too (the Output's "Go to SQL" and the toolbar's statement count rely on it). */
static void ins_raw(Editor *e, const char *s, int n) {
    if (n < 0) n = (int)strlen(s);
    if (n > 0) e->revision++;
    delete_selection(e);
    int start = 0;
    for (int i = 0; i <= n; i++) {
        if (i == n || s[i] == '\n') {
            int chunk = i - start;
            if (chunk > 0) { line_insert(&e->lines[e->cy], e->cx, s + start, chunk); e->cx += chunk; }
            if (i < n) {
                Line *cur = &e->lines[e->cy];
                ed_insert_line(e, e->cy + 1);
                cur = &e->lines[e->cy];
                Line *nl = &e->lines[e->cy + 1];
                line_insert(nl, 0, cur->s + e->cx, cur->len - e->cx);
                line_erase(cur, e->cx, cur->len - e->cx);
                e->cy++; e->cx = 0;
            }
            start = i + 1;
        } else if (s[i] == '\r') {
            /* normalise CRLF: emit chunk before \r, skip it */
            int chunk = i - start;
            if (chunk > 0) { line_insert(&e->lines[e->cy], e->cx, s + start, chunk); e->cx += chunk; }
            start = i + 1;
        }
    }
    collapse(e);
    e->want_x = -1;
    touch(e);
}

/* Insert text at the caret as one undo step (replaces the selection). */
void editor_insert(Editor *e, const char *s, int n) {
    if (e->read_only) return;
    e->revision++;
    edit_begin(e, EK_OTHER);
    ins_raw(e, s, n);
}

/* Replace the whole buffer with len bytes of text (len < 0: NUL-terminated); caret at the start. */
static void load_text(Editor *e, const char *text, int len) {
    e->revision++;
    for (int i = 0; i < e->n; i++) free(e->lines[i].s);
    e->n = 0;
    ed_insert_line(e, 0);
    e->cx = e->cy = e->ax = e->ay = 0;
    ins_raw(e, text, len);
    e->cx = e->cy = 0;
    collapse(e);
}

/* Load new text, forget the undo history and scroll to the top. */
void editor_set_text(Editor *e, const char *text) {
    load_text(e, text, -1);
    history_clear(e);
    e->copen = false;
    e->scroll_ty = e->scroll_y = 0;
}

/* Whole text joined with '\n'. Returns a malloc'd string (caller frees); *len gets its length. */
char *editor_text(Editor *e, int *len) {
    int total = 0;
    for (int i = 0; i < e->n; i++) total += e->lines[i].len + 1;
    char *out = malloc(total + 1);
    int p = 0;
    for (int i = 0; i < e->n; i++) {
        memcpy(out + p, e->lines[i].s, e->lines[i].len);
        p += e->lines[i].len;
        if (i + 1 < e->n) out[p++] = '\n';
    }
    out[p] = 0;
    if (len) *len = p;
    return out;
}

/* Selected text as a malloc'd string (caller frees), or NULL when nothing is selected. */
char *editor_selection(Editor *e, int *len) {
    if (!has_sel(e)) return NULL;
    int x0, y0, x1, y1;
    sel_range(e, &x0, &y0, &x1, &y1);
    int total = 0;
    for (int i = y0; i <= y1; i++) total += e->lines[i].len + 1;
    char *out = malloc(total + 1);
    int p = 0;
    for (int i = y0; i <= y1; i++) {
        int from = i == y0 ? x0 : 0, to = i == y1 ? x1 : e->lines[i].len;
        memcpy(out + p, e->lines[i].s + from, to - from);
        p += to - from;
        if (i < y1) out[p++] = '\n';
    }
    out[p] = 0;
    if (len) *len = p;
    return out;
}

/* Byte offset of the caret in the joined text. */
static int cursor_offset(Editor *e) {
    int off = 0;
    for (int i = 0; i < e->cy; i++) off += e->lines[i].len + 1;
    return off + e->cx;
}

/* Statement containing the cursor: split on ';' outside strings/comments.
   Returns byte offsets [*s, *e) into *text_out (malloc'd), or false. */
static bool statement_range(Editor *e, char **text_out, int *so, int *eo) {
    int n;
    char *t = editor_text(e, &n);
    int off = cursor_offset(e);
    int start = 0, i = 0;
    int best_s = -1, best_e = -1;
    while (i < n) {
        char c = t[i];
        if (c == '\'' || c == '"') {
            char q = c; i++;
            while (i < n && t[i] != q) i++;
            i++;
        } else if (c == '-' && i + 1 < n && t[i + 1] == '-') {
            while (i < n && t[i] != '\n') i++;
        } else if (c == '/' && i + 1 < n && t[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(t[i] == '*' && t[i + 1] == '/')) i++;
            i += 2;
        } else if (c == ';') {
            i++;
            if (off <= i) { best_s = start; best_e = i; break; }
            start = i;
        } else i++;
    }
    if (best_s < 0) {
        /* Cursor after the last ';' : use trailing text if non-blank, else the last statement. */
        bool blank = true;
        for (int k = start; k < n; k++) if (!isspace((unsigned char)t[k])) { blank = false; break; }
        if (!blank) { best_s = start; best_e = n; }
        else if (start > 0) {
            /* walk back to the previous statement start */
            int k = start - 1;         /* index of ';' */
            int s2 = 0, j = 0;
            /* re-scan to find the start of the statement ending at k */
            while (j < k) {
                if (t[j] == ';') s2 = j + 1;
                j++;
            }
            best_s = s2; best_e = k + 1;
        }
    }
    if (best_s < 0) { free(t); return false; }
    while (best_s < best_e && isspace((unsigned char)t[best_s])) best_s++;
    while (best_e > best_s && isspace((unsigned char)t[best_e - 1])) best_e--;
    /* a statement made only of comments is not runnable */
    bool only_comment = true;
    for (int k = best_s; k < best_e; k++) {
        if (t[k] == '-' && k + 1 < best_e && t[k + 1] == '-') { while (k < best_e && t[k] != '\n') k++; continue; }
        if (!isspace((unsigned char)t[k])) { only_comment = false; break; }
    }
    if (best_e <= best_s || only_comment) { free(t); return false; }
    *text_out = t; *so = best_s; *eo = best_e;
    return true;
}

/* The statement under the caret (trimmed) as a malloc'd string, its length and its byte offset
 * in the joined text; NULL when there is none. */
char *editor_statement_at_cursor_pos(Editor *e, int *len, int *start) {
    char *t; int s, en;
    if (!statement_range(e, &t, &s, &en)) return NULL;
    int l = en - s;
    char *out = malloc(l + 1);
    memcpy(out, t + s, l);
    out[l] = 0;
    free(t);
    if (len) *len = l;
    if (start) *start = s;
    return out;
}
/* Same as editor_statement_at_cursor_pos without the offset. */
char *editor_statement_at_cursor(Editor *e, int *len) { return editor_statement_at_cursor_pos(e, len, NULL); }

/* Counter bumped on every text change (see editor.h). */
uint32_t editor_revision(Editor *e) { return e->revision; }

/* Byte offset of the start of the selection in the joined text, -1 when nothing is selected. */
int editor_selection_start(Editor *e) {
    if (!has_sel(e)) return -1;
    int x0, y0, x1, y1;
    sel_range(e, &x0, &y0, &x1, &y1);
    int off = 0;
    for (int i = 0; i < y0; i++) off += e->lines[i].len + 1;
    return off + x0;
}

/* Select joined-text bytes [start, end) (anchor at start, caret at end). False when either
 * offset is outside the text. */
bool editor_select_range(Editor *e, int start, int end) {
    if (start < 0 || end < start) return false;
    int off = 0, sx = -1, sy = 0, ex = -1, ey = 0;
    for (int i = 0; i < e->n; i++) {
        int next = off + e->lines[i].len + 1;
        if (sx < 0 && start >= off && start < next) { sx = start - off; sy = i; }
        if (ex < 0 && end >= off && end < next) { ex = end - off; ey = i; }
        off = next;
    }
    if (sx < 0 || ex < 0) return false;
    e->ax = sx; e->ay = sy; e->cx = ex; e->cy = ey;
    e->copen = false;
    touch(e);
    return true;
}

/* 0-based first and last line of the statement under the caret; false when there is none. */
bool editor_statement_lines(Editor *e, int *first, int *last) {
    char *t; int s, en;
    if (!statement_range(e, &t, &s, &en)) return false;
    free(t);
    int off = 0;
    *first = 0; *last = 0;
    for (int i = 0; i < e->n; i++) {
        int next = off + e->lines[i].len + 1;
        if (s >= off && s < next) *first = i;
        if (en - 1 >= off && en - 1 < next) *last = i;
        off = next;
    }
    return true;
}

/* Select the whole text (caret at the end). */
void editor_select_all(Editor *e) {
    e->ax = 0; e->ay = 0;
    e->cy = e->n - 1; e->cx = e->lines[e->cy].len;
    touch(e);
}

/* Copy the selection to the system clipboard (no-op without a selection). */
void editor_copy(Editor *e) {
    char *s = editor_selection(e, NULL);
    if (s) { SDL_SetClipboardText(s); free(s); }
}

/* Copy the selection to the clipboard and delete it, as one undo step. */
void editor_cut(Editor *e) {
    if (e->read_only) return;
    if (!has_sel(e)) return;
    editor_copy(e);
    edit_begin(e, EK_OTHER);
    delete_selection(e);
    touch(e);
}

/* Insert the clipboard text at the caret (one undo step). */
void editor_paste(Editor *e) {
    if (e->read_only) return;
    char *s = SDL_GetClipboardText();
    if (s && *s) editor_insert(e, s, -1);
    SDL_free(s);
}

/* Caret position for the status bar: 1-based line and 1-based column counted in code points. */
void editor_cursor_pos(Editor *e, int *line, int *col) {
    *line = e->cy + 1;
    int c = 0;
    for (int i = 0; i < e->cx;) { uint32_t cp; i = utf8_next(e->lines[e->cy].s, e->cx, i, &cp); c++; }
    *col = c + 1;
}

/* ---- movement ---------------------------------------------------------- */

/* Word character for word movement and double-click: ASCII alnum, '_' or any non-ASCII byte. */
static bool is_word(unsigned char c) { return isalnum(c) || c == '_' || c >= 0x80; }

/* Move the caret one code point (or, with `word`, to the previous word start) to the left,
 * wrapping to the end of the previous line. Does not touch the anchor. */
static void move_left(Editor *e, bool word) {
    Line *l = &e->lines[e->cy];
    if (e->cx == 0) { if (e->cy > 0) { e->cy--; e->cx = e->lines[e->cy].len; } return; }
    if (!word) { e->cx = utf8_prev(l->s, e->cx); return; }
    while (e->cx > 0 && !is_word(l->s[e->cx - 1])) e->cx--;
    while (e->cx > 0 && is_word(l->s[e->cx - 1])) e->cx--;
}

/* Move the caret one code point (or past the next word) to the right, wrapping to the next line. */
static void move_right(Editor *e, bool word) {
    Line *l = &e->lines[e->cy];
    if (e->cx >= l->len) { if (e->cy + 1 < e->n) { e->cy++; e->cx = 0; } return; }
    if (!word) { uint32_t cp; e->cx = utf8_next(l->s, l->len, e->cx, &cp); return; }
    while (e->cx < l->len && !is_word(l->s[e->cx])) e->cx++;
    while (e->cx < l->len && is_word(l->s[e->cx])) e->cx++;
}

/* Byte column in line l nearest to x offset `xrel` (pixels from the text start). */
static int col_from_x(Font *f, Line *l, float xrel) {
    int i = 0;
    float w = 0;
    while (i < l->len) {
        uint32_t cp;
        int nx = utf8_next(l->s, l->len, i, &cp);
        float gw = font_width(f, l->s + i, nx - i);
        if (xrel < w + gw / 2) return i;
        w += gw;
        i = nx;
    }
    return l->len;
}

/* Move the caret dy lines up / down, keeping the preferred x (want_x) across short lines;
 * past the first / last line it goes to the start / end of the text. */
static void move_vertical(Editor *e, Font *f, int dy) {
    if (e->want_x < 0) e->want_x = font_width(f, e->lines[e->cy].s, e->cx);
    int ny = e->cy + dy;
    if (ny < 0) { e->cy = 0; e->cx = 0; return; }
    if (ny >= e->n) { e->cy = e->n - 1; e->cx = e->lines[e->cy].len; return; }
    e->cy = ny;
    e->cx = col_from_x(f, &e->lines[e->cy], e->want_x);
}

/* Select the word around the caret (double-click). */
static void select_word_at(Editor *e) {
    Line *l = &e->lines[e->cy];
    int s = e->cx, t = e->cx;
    while (s > 0 && is_word(l->s[s - 1])) s--;
    while (t < l->len && is_word(l->s[t])) t++;
    e->ax = s; e->cx = t;
    e->ay = e->cy;
}

/* ---- syntax highlighting ----------------------------------------------- */

/* Words highlighted as keywords (upper case; matching is case-insensitive). */
static const char *keywords[] = {
    "SELECT","FROM","WHERE","AND","OR","NOT","NULL","IS","IN","AS","ON","JOIN","LEFT","RIGHT","INNER",
    "OUTER","FULL","CROSS","GROUP","BY","ORDER","HAVING","LIMIT","OFFSET","INSERT","INTO","VALUES",
    "UPDATE","SET","DELETE","CREATE","TABLE","VIEW","INDEX","DROP","ALTER","ADD","COLUMN","PRIMARY",
    "KEY","FOREIGN","REFERENCES","UNIQUE","CHECK","DEFAULT","CONSTRAINT","DISTINCT","UNION","ALL",
    "EXCEPT","INTERSECT","CASE","WHEN","THEN","ELSE","END","BEGIN","COMMIT","ROLLBACK","TRANSACTION",
    "EXISTS","BETWEEN","LIKE","ASC","DESC","WITH","RECURSIVE","RETURNING","IF","REPLACE","TRUE","FALSE",
    "INTEGER","TEXT","REAL","BLOB","VARCHAR","INT","BOOLEAN","DATE","TIMESTAMP","PRAGMA","EXPLAIN",
    "USING","NATURAL","TEMP","TEMPORARY","TRIGGER","VACUUM","ANALYZE","CAST","COLLATE","GLOB","ESCAPE",
    "AUTOINCREMENT","CONFLICT","IGNORE","ABORT","FAIL","INDEXED","OVER","PARTITION","WINDOW","FILTER",
};

/* True when the n bytes at s are a keyword (case-insensitive). */
static bool is_keyword(const char *s, int n) {
    if (n > 16) return false;
    char up[17];
    for (int i = 0; i < n; i++) up[i] = (char)toupper((unsigned char)s[i]);
    up[n] = 0;
    for (size_t k = 0; k < sizeof keywords / sizeof keywords[0]; k++)
        if (strcmp(up, keywords[k]) == 0) return true;
    return false;
}

/* Highlighting token kinds and a token: byte range within a line plus kind. */
enum { HL_PLAIN, HL_KEYWORD, HL_STRING, HL_NUMBER, HL_COMMENT, HL_FUNC, HL_OP, HL_PUNCT };
typedef struct { int start, len, type; } Token;

/* Split one line (n bytes) into highlighting tokens, at most `max`. *in_block carries "inside a
 * block comment" from the previous line and is updated for the next one. A word followed by
 * '(' is a function. Every byte belongs to exactly one token, so drawing all tokens in order
 * reproduces the line. Returns the token count. */
/* Highlighting state carried from one line to the next: inside a block comment (depth, for
 * dialects that nest them), a string that did not close, or a dollar-quoted body. */
typedef struct HlState { int comment; char quote; char dollar[32]; } HlState;

/* Tokenises one line for highlighting with the dialect's lexical rules (sqllex.h): MySQL # comments
 * and backslash escapes, Oracle q'[...]', PostgreSQL nested comments and $tag$ bodies (whose
 * content is code and stays highlighted as SQL), SQL Server [ident] / @var / GO. */
static int tokenize(const char *s, int n, HlState *st, const SqlLexRules *R, Token *out, int max) {
    int nt = 0, i = 0;
    while (i < n && nt < max) {
        int start = i, type = HL_PLAIN;
        if (st->comment) {
            while (i < n && st->comment) {
                if (R->nested_comments && s[i] == '/' && i + 1 < n && s[i + 1] == '*') { st->comment++; i += 2; continue; }
                if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') { st->comment--; i += 2; continue; }
                i++;
            }
            type = HL_COMMENT;
        } else if (st->quote) {
            /* a string continued from an earlier line */
            char q = st->quote;
            while (i < n) {
                if (R->backslash_escapes && s[i] == '\\') { i += 2; continue; }
                if (s[i] == q) { if (i + 1 < n && s[i + 1] == q) { i += 2; continue; } i++; st->quote = 0; break; }
                i++;
            }
            if (i > n) i = n;
            type = HL_STRING;
        } else if ((s[i] == '-' && i + 1 < n && s[i + 1] == '-') || (R->hash_comments && s[i] == '#')) {
            i = n; type = HL_COMMENT;
        } else if (s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            st->comment = 1; i += 2;
            while (i < n && st->comment) {
                if (R->nested_comments && s[i] == '/' && i + 1 < n && s[i + 1] == '*') { st->comment++; i += 2; continue; }
                if (s[i] == '*' && i + 1 < n && s[i + 1] == '/') { st->comment--; i += 2; continue; }
                i++;
            }
            type = HL_COMMENT;
        } else if (R->dollar_bodies && s[i] == '$' && (i + 1 < n) && (s[i + 1] == '$' || isalpha((unsigned char)s[i + 1]) || s[i + 1] == '_')) {
            /* $tag$: the delimiter is marked; the body between is code (PL/pgSQL), highlighted as SQL */
            int j = i + 1; while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
            if (j < n && s[j] == '$') {
                i = j + 1;
                int tl = i - start;
                if (st->dollar[0] && (int)strlen(st->dollar) == tl && !memcmp(st->dollar, s + start, (size_t)tl)) st->dollar[0] = 0;
                else if (!st->dollar[0] && tl < (int)sizeof st->dollar) { memcpy(st->dollar, s + start, (size_t)tl); st->dollar[tl] = 0; }
                type = HL_KEYWORD;
            } else i++;
        } else if (R->q_quote && (s[i] == 'q' || s[i] == 'Q') && i + 2 < n && s[i + 1] == '\'') {
            char open = s[i + 2], close = open == '[' ? ']' : open == '{' ? '}' : open == '(' ? ')' : open == '<' ? '>' : open;
            i += 3;
            while (i + 1 < n && !(s[i] == close && s[i + 1] == '\'')) i++;
            i = i + 1 < n ? i + 2 : n;
            type = HL_STRING;
        } else if (s[i] == '\'' || (s[i] == '"' && R->dq_strings)) {
            char q = s[i++];
            st->quote = q;
            while (i < n) {
                if (R->backslash_escapes && s[i] == '\\') { i += 2; continue; }
                if (s[i] == q) { if (i + 1 < n && s[i + 1] == q) { i += 2; continue; } i++; st->quote = 0; break; }
                i++;
            }
            if (i > n) i = n;
            type = HL_STRING;
        } else if (s[i] == '"' || (s[i] == '`' && R->backtick_ident) || (s[i] == '[' && R->bracket_ident)) {
            char q = s[i] == '[' ? ']' : s[i]; i++;
            while (i < n && s[i] != q) i++;
            if (i < n) i++;
            type = HL_PLAIN;
        } else if (isdigit((unsigned char)s[i])) {
            while (i < n && (isalnum((unsigned char)s[i]) || s[i] == '.')) i++;
            type = HL_NUMBER;
        } else if (is_word((unsigned char)s[i]) || (s[i] == '@' && R->at_vars && i + 1 < n && is_word((unsigned char)s[i + 1]))) {
            i++;
            while (i < n && (is_word((unsigned char)s[i]) || s[i] == '$' || s[i] == '#')) i++;
            int j = i;
            while (j < n && s[j] == ' ') j++;
            bool go = R->go_batches && i - start == 2 && !strncasecmp(s + start, "GO", 2) && start == 0;
            if (go || (s[start] != '@' && is_keyword(s + start, i - start))) type = HL_KEYWORD;
            else if (j < n && s[j] == '(') type = HL_FUNC;
        } else if (strchr("=<>!+-*/%|&", s[i])) {
            i++; type = HL_OP;
        } else if (strchr(",;.()", s[i])) {
            i++; type = HL_PUNCT;
        } else {
            i++;
        }
        out[nt].start = start; out[nt].len = i - start; out[nt].type = type;
        nt++;
    }
    return nt;
}

/* Theme color for a highlighting token kind. */
static Color token_color(int type) {
    switch (type) {
    case HL_KEYWORD: return color_hex(T_SYN_KEYWORD);
    case HL_STRING: return color_hex(T_SYN_STRING);
    case HL_NUMBER: return color_hex(T_SYN_NUMBER);
    case HL_COMMENT: return color_hex(T_SYN_COMMENT);
    case HL_FUNC: return color_hex(T_SYN_FUNC);
    case HL_OP: return color_hex(T_SYN_OP);
    case HL_PUNCT: return color_hex(T_SYN_PUNCT);
    default: return color_hex(T_TEXT);
    }
}

/* ---- input ------------------------------------------------------------- */

/* ---- completion -------------------------------------------------------- */

/* Recompute completion proposals for the caret (explicit: the user pressed Ctrl+Space) and open
 * the popup if there are any; closes it otherwise or while text is selected. */
static void complete_refresh(Editor *e, App *a, bool explicit) {
    e->copen = false;
    if (!a->completer || has_sel(e)) return;
    char *text = NULL;
    int so, eo;
    int cur = cursor_offset(e);
    int n;
    if (!statement_range(e, &text, &so, &eo) || cur < so || cur > eo) {
        /* cursor outside any statement (e.g. after the last ';'): use the text between the
         * surrounding semicolons so aliases of a statement being started still resolve */
        if (text) free(text);
        text = editor_text(e, &n);
        so = 0; eo = n;
        for (int i = 0; i < cur; i++) if (text[i] == ';') so = i + 1;
        for (int i = cur; i < n; i++) if (text[i] == ';') { eo = i; break; }
    }
    int nc = complete_query(a->completer, text + so, eo - so, cur - so, explicit, e->cands, COMPLETE_MAX, &e->cprefix);
    free(text);
    e->ncands = nc;
    e->csel = nc > 1 && e->cands[0].info ? 1 : 0;   /* a Loading row is never the default choice */
    e->cscroll = 0; e->chover = -1;
    e->copen = nc > 0;
    e->cpending = complete_pending(a->completer); e->cexplicit = explicit; e->crev = editor_revision(e); e->ccur = cur;
}
/* Columns asked for in the background arrived: the popup that waited for them is recomputed,
 * only for the text and caret it was opened for (never a stale list). */
void editor_metadata_arrived(Editor *e, App *a) {
    if (!e || !e->cpending) return;
    e->cpending = false;
    if (!e->copen) return;   /* closed meanwhile (Esc, a click elsewhere): it does not come back uninvited */
    if (editor_revision(e) != e->crev || cursor_offset(e) != e->ccur || has_sel(e)) return;
    complete_refresh(e, a, e->cexplicit);
}

/* Replace the typed prefix with the selected proposal (one undo step), teach the completer the
 * choice, and close the popup. */
static void complete_accept(Editor *e, App *a) {
    if (!e->copen || e->csel < 0 || e->csel >= e->ncands) return;
    Cand *cd = &e->cands[e->csel];
    if (cd->info) return;   /* a state row inserts nothing and the popup stays, waiting for the columns */
    edit_begin(e, EK_OTHER);
    if (e->cprefix > 0) delete_range(e, e->cx - e->cprefix, e->cy, e->cx, e->cy);
    ins_raw(e, cd->text, -1);
    complete_learn_choice(a->completer, cd);
    e->copen = false;
    e->last_kind = EK_OTHER;
}

/* Open the completion popup on demand (Ctrl+Space). */
void editor_complete(Editor *e, App *a) { complete_refresh(e, a, true); }
void editor_dump_completion(const Editor *e) {
    printf("completestate open=%d n=%d", e->copen, e->copen ? e->ncands : 0);
    for (int i = 0; e->copen && i < e->ncands && i < 12; i++) printf(" [%s|%s]", e->cands[i].label[0] ? e->cands[i].label : e->cands[i].text, e->cands[i].detail);
    printf("\n"); fflush(stdout);
}

/* Popup navigation while it is open: Up / Down / PageUp / PageDown move, Return / Tab accept,
 * Esc closes. Returns true when the key was used (Cmd+Return is left for "run"). */
static bool complete_keys(Editor *e, App *a, KeyPress *kp) {
    int page = 8;
    switch (kp->key) {
    /* a state row ("Loading columns of…") is skipped over: it is not a choice */
    case SDLK_UP: { int s = e->csel > 0 ? e->csel - 1 : e->ncands - 1; if (e->cands[s].info && e->ncands > 1) s = s > 0 ? s - 1 : e->ncands - 1; if (!e->cands[s].info) e->csel = s; return true; }
    case SDLK_DOWN: { int s = (e->csel + 1) % e->ncands; if (e->cands[s].info && e->ncands > 1) s = (s + 1) % e->ncands; if (!e->cands[s].info) e->csel = s; return true; }
    case SDLK_PAGEUP: { int s = e->csel - page < 0 ? 0 : e->csel - page; if (e->cands[s].info && s + 1 < e->ncands) s++; e->csel = s; return true; }
    case SDLK_PAGEDOWN: { int s = e->csel + page >= e->ncands ? e->ncands - 1 : e->csel + page; if (e->cands[s].info && s > 0) s--; e->csel = s; return true; }
    case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_TAB:
        if (kp->mods & KM_PRIMARY) return false;
        complete_accept(e, a); return true;
    case SDLK_ESCAPE: e->copen = false; return true;
    default: return false;
    }
}

/* 32-bit FNV-1a hash of n bytes, to recognise a statement that was already learnt. */
static uint32_t hash_text(const char *s, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
    return h;
}

/* Learns a statement the cursor just left (rule: written queries count, not only executed ones).
 * Called every frame: while the caret stays in the same statement its text copy is refreshed
 * after edits; when the caret moves to another statement, the old copy is learnt (once). */
static void track_statement(Editor *e, App *a) {
    int f, l;
    if (!editor_statement_lines(e, &f, &l)) { f = l = -1; }
    if (f == e->stmt_first && l == e->stmt_last) {
        /* same statement: keep the copy current while it is edited */
        if (f >= 0 && e->last_edit != e->stmt_seen_edit) {
            free(e->stmt_text);
            int len;
            e->stmt_text = editor_statement_at_cursor(e, &len);
            e->stmt_seen_edit = e->last_edit;
        }
        return;
    }
    if (e->stmt_text && a->completer) {
        int len = (int)strlen(e->stmt_text);
        uint32_t h = hash_text(e->stmt_text, len);
        if (h != e->learned_hash) { complete_learn_statement(a->completer, e->stmt_text, len); e->learned_hash = h; }
    }
    free(e->stmt_text);
    e->stmt_text = NULL;
    if (f >= 0) { int len; e->stmt_text = editor_statement_at_cursor(e, &len); }
    e->stmt_first = f; e->stmt_last = l;
    e->stmt_seen_edit = e->last_edit;
}

/* Apply this frame's unused key presses and typed text to the editor (only when focused).
 * Keys it handles are marked used; Cmd+Return and other global shortcuts are left alone. */
static void handle_keys(Editor *e, App *a) {
    Input *in = &a->in;
    unsigned m;
    Font *f = a->font_mono;
    for (int k = 0; k < in->nkeys; k++) {
        KeyPress *kp = &in->keys[k];
        if (kp->used) continue;
        m = kp->mods;
        bool shift = m & KM_SHIFT, alt = m & KM_ALT, prim = m & KM_PRIMARY;
        bool handled = true, moved = false;
        if (e->copen && complete_keys(e, a, kp)) { kp->used = true; touch(e); continue; }
        if (kp->key == SDLK_SPACE && (m & (KM_CTRL | KM_PRIMARY))) { kp->used = true; complete_refresh(e, a, true); continue; }
        switch (kp->key) {
        case SDLK_LEFT:
            if (has_sel(e) && !shift) { int x0, y0, x1, y1; sel_range(e, &x0, &y0, &x1, &y1); e->cx = x0; e->cy = y0; }
            else if (prim) e->cx = 0;
            else move_left(e, alt);
            moved = true; break;
        case SDLK_RIGHT:
            if (has_sel(e) && !shift) { int x0, y0, x1, y1; sel_range(e, &x0, &y0, &x1, &y1); e->cx = x1; e->cy = y1; }
            else if (prim) e->cx = e->lines[e->cy].len;
            else move_right(e, alt);
            moved = true; break;
        case SDLK_UP:
            if (prim) { e->cy = 0; e->cx = 0; } else move_vertical(e, f, -1);
            moved = true; break;
        case SDLK_DOWN:
            if (prim) { e->cy = e->n - 1; e->cx = e->lines[e->cy].len; } else move_vertical(e, f, 1);
            moved = true; break;
        case SDLK_HOME: e->cx = 0; moved = true; break;
        case SDLK_END: e->cx = e->lines[e->cy].len; moved = true; break;
        case SDLK_PAGEUP: move_vertical(e, f, -(int)(e->text_rect.h / e->line_h)); moved = true; break;
        case SDLK_PAGEDOWN: move_vertical(e, f, (int)(e->text_rect.h / e->line_h)); moved = true; break;
        case SDLK_BACKSPACE:
            edit_begin(e, has_sel(e) ? EK_OTHER : EK_DEL);
            if (has_sel(e)) delete_selection(e);
            else if (e->cx > 0 || e->cy > 0) {
                int ox = e->cx, oy = e->cy;
                move_left(e, alt);
                delete_range(e, e->cx, e->cy, ox, oy);
            }
            break;
        case SDLK_DELETE:
            edit_begin(e, has_sel(e) ? EK_OTHER : EK_DEL);
            if (has_sel(e)) delete_selection(e);
            else if (e->cx < e->lines[e->cy].len || e->cy + 1 < e->n) {
                int ox = e->cx, oy = e->cy;
                move_right(e, alt);
                delete_range(e, ox, oy, e->cx, e->cy);
            }
            break;
        case SDLK_RETURN: case SDLK_KP_ENTER:
            if (prim) { handled = false; break; }  /* run: handled globally */
            {
                /* keep indentation */
                Line *l = &e->lines[e->cy];
                int ind = 0;
                while (ind < l->len && ind < e->cx && (l->s[ind] == ' ' || l->s[ind] == '\t')) ind++;
                char buf[128] = "\n";
                int n = ind < 126 ? ind : 126;
                memcpy(buf + 1, l->s, n);
                editor_insert(e, buf, n + 1);
            }
            break;
        case SDLK_TAB:
            editor_insert(e, "  ", 2);
            break;
        case SDLK_ESCAPE: collapse(e); break;
        default: handled = false;
        }
        if (!handled) continue;
        kp->used = true;
        if (kp->key == SDLK_BACKSPACE && e->copen) complete_refresh(e, a, false);
        else if (kp->key != SDLK_BACKSPACE) e->copen = false;
        if (moved) {
            if (!shift) collapse(e);
            if (kp->key != SDLK_UP && kp->key != SDLK_DOWN && kp->key != SDLK_PAGEUP && kp->key != SDLK_PAGEDOWN) e->want_x = -1;
        } else e->want_x = -1;
        touch(e);
    }
    /* typed text: one undo step per run of typing; may auto-open the completion popup */
    if (in->ntext) {
        int nt = in->ntext;
        unsigned char c0 = (unsigned char)in->text[0];
        int kind = has_sel(e) ? EK_OTHER : (c0 == ' ' || c0 == '\n' || c0 == '\t') ? EK_TYPE_WS : EK_TYPE;
        edit_begin(e, kind);
        ins_raw(e, in->text, in->ntext);
        in->ntext = 0;
        /* auto-trigger completion on identifier characters and '.' */
        unsigned char last = (unsigned char)in->text[nt - 1];
        if (is_word(last) || last == '.' || last == ' ') complete_refresh(e, a, false);
        else e->copen = false;
    }
}

/* ---- update / draw ----------------------------------------------------- */

/* Screen x (render px) of byte column col of a line, given the current horizontal scroll. */
static float line_x(Editor *e, Font *f, int line, int col) {
    return e->text_rect.x - e->scroll_x + font_width(f, e->lines[line].s, col);
}

static void editor_update_body(Editor *e, App *a, Rect body, bool focused);

/* Per-frame update + draw of the editor in `b` (render px): with the strip enabled draws the
 * "query.sql" tab and the run hint first, then the body. */
void editor_update(Editor *e, App *a, Rect b) {
    SDL_Renderer *r = a->renderer;
    bool focused = a->focus == FOCUS_EDITOR && a->editor == e;   /* only the active editor tab types */
    if (focused && e->read_only) {
        /* read-only source (system / encrypted object, OBJ-005): navigation and copy only */
        Input *in = &a->in;
        in->ntext = 0;
        for (int i = 0; i < in->nkeys; i++) {
            KeyPress *k = &in->keys[i];
            bool cmd = k->mods & (SDL_KMOD_GUI | SDL_KMOD_CTRL);
            if (k->key == SDLK_BACKSPACE || k->key == SDLK_DELETE || k->key == SDLK_RETURN || k->key == SDLK_KP_ENTER || k->key == SDLK_TAB ||
                (cmd && (k->key == SDLK_V || k->key == SDLK_X || k->key == SDLK_Z || k->key == SDLK_Y || k->key == SDLK_SLASH || k->key == SDLK_D)))
                k->used = true;
        }
    }

    if (e->no_strip) {
        /* docking (LUME-PANEL-027): tabs and the run hint belong to the editor group header */
        editor_update_body(e, a, b, focused);
        return;
    }
    /* Tab strip: dark strip, the active tab shares the editor background and breaks the bottom border */
    Rect strip = rect_make(b.x, b.y, b.w, px(a, M_HEADER));
    draw_rect(r, strip, color_hex(T_BG));
    draw_rect(r, rect_make(strip.x, strip.y + strip.h - px(a, 1), strip.w, px(a, 1)), color_hex(T_BORDER));
    float tab_w = font_width(a->font_ui, "query.sql", -1) + px(a, 32);
    Rect tab = rect_make(strip.x, strip.y, tab_w, strip.h);
    draw_rect(r, tab, color_hex(T_EDITOR_BG));
    draw_rect(r, rect_make(tab.x, tab.y, tab.w, px(a, 2)), color_hex(T_ACCENT));
    draw_rect(r, rect_make(tab.x + tab.w, tab.y, px(a, 1), tab.h), color_hex(T_BORDER));
    draw_text_fit(a->font_ui, r, "query.sql", -1, rect_inset(tab, px(a, 16), 0), color_hex(T_TEXT), ALIGN_LEFT);

    int stmt_first = -1, stmt_last = -1;
    bool has_stmt = editor_statement_lines(e, &stmt_first, &stmt_last);
    {
#ifdef __APPLE__
        const char *keys = "\xE2\x8C\x98\xE2\x86\xA9";
#else
        const char *keys = "Ctrl+Enter";
#endif
        const char *label = has_stmt ? "Run statement" : "No statement under cursor";
        float lw = font_width(a->font_small, label, -1);
        float kw = font_width(a->font_small, keys, -1) + px(a, 12);
        float x = strip.x + strip.w - px(a, 14) - lw;
        draw_text_fit(a->font_small, r, label, -1, rect_make(x, strip.y, lw + px(a, 2), strip.h), color_hex(has_stmt ? T_TEXT_MUTED : T_TEXT_DIM), ALIGN_LEFT);
        Rect chip = rect_make(x - px(a, 8) - kw, strip.y + px(a, 7), kw, strip.h - px(a, 14));
        draw_rrect(r, chip, px(a, 4), color_hex(T_BORDER));
        draw_rrect(r, rect_inset(chip, px(a, 1), px(a, 1)), px(a, 3), color_hex(T_PANEL_ALT));
        draw_text_fit(a->font_small, r, keys, -1, chip, color_hex(has_stmt ? T_TEXT : T_TEXT_DIM), ALIGN_CENTER);
    }

    Rect body = rect_make(b.x, strip.y + strip.h, b.w, b.h - strip.h);
    editor_update_body(e, a, body, focused);
}

/* Choose whether the editor draws its own tab strip (false when docked in the panels shell). */
void editor_set_strip(Editor *e, bool draw) { e->no_strip = !draw; }

/* The editor body: popup mouse input, text mouse / wheel / key input, statement tracking,
 * scrolling, then drawing of the statement band, selection, gutter, highlighted text, caret
 * and scrollbar. Also records where the completion popup should appear. */
static void editor_update_body(Editor *e, App *a, Rect body, bool focused) {
    SDL_Renderer *r = a->renderer;
    Input *in = &a->in;
    Font *f = a->font_mono;
    int stmt_first = -1, stmt_last = -1;
    bool has_stmt = editor_statement_lines(e, &stmt_first, &stmt_last);   /* highlighted band */
    draw_rect(r, body, color_hex(T_EDITOR_BG));

    e->line_h = roundf(font_size(f) * 1.55f);
    int digits = 1;
    for (int n = e->n; n >= 10; n /= 10) digits++;
    if (digits < 2) digits = 2;
    e->gutter_w = font_width(f, "0", 1) * digits + px(a, 28);
    Rect text = rect_make(body.x + e->gutter_w, body.y + px(a, 8), body.w - e->gutter_w - px(a, 8), body.h - px(a, 8));
    e->text_rect = text;

    /* Completion popup input (drawn later in editor_draw_overlay, so it is handled first) */
    if (e->copen && !focused) e->copen = false;
    if (e->copen && rect_contains(e->popup_rect, in->mouse_x, in->mouse_y)) {
        a->want_cursor = CURSOR_ARROW;
        int row = (int)floorf((in->mouse_y - e->popup_rect.y - px(a, 4)) / px(a, COMPLETE_ROW)) + e->cscroll;
        e->chover = row >= 0 && row < e->ncands ? row : -1;
        if (in->wheel_y != 0) { e->cscroll -= (int)(in->wheel_y / px(a, COMPLETE_ROW) * 1.5f); in->wheel_y = 0; }
        if (in->mouse_pressed && !in->mouse_consumed) {
            in->mouse_consumed = true;
            if (e->chover >= 0) { e->csel = e->chover; complete_accept(e, a); }
        }
    } else e->chover = -1;

    /* the scrollbar takes its own presses before the text does */
    Rect sbv = rect_make(body.x, text.y, body.w, text.h);
    if (ui_scrollbar_input(a, &e->sb, sbv, e->n * e->line_h + text.h * 0.5f, &e->scroll_ty) && e->sb.drag) e->scroll_y = e->scroll_ty;
    /* Input */
    bool hover = rect_contains(body, in->mouse_x, in->mouse_y);
    if (hover && !in->mouse_consumed) a->want_cursor = CURSOR_IBEAM;
    if (hover && in->mouse_pressed && !in->mouse_consumed) {
        e->copen = false;
        in->mouse_consumed = true;
        a->focus = FOCUS_EDITOR;
        focused = true;
        int line = (int)floorf((in->mouse_y - text.y + e->scroll_y) / e->line_h);
        line = line < 0 ? 0 : line >= e->n ? e->n - 1 : line;
        e->cy = line;
        e->cx = col_from_x(f, &e->lines[line], in->mouse_x - text.x + e->scroll_x);
        if (in->clicks == 2) select_word_at(e);
        else if (in->clicks >= 3) { e->ax = 0; e->ay = line; e->cx = e->lines[line].len; }
        else if (!(app_modstate(a) & SDL_KMOD_SHIFT)) collapse(e);
        e->dragging = in->clicks == 1;
        e->want_x = -1;
        touch(e);
        e->scroll_to_cursor = false;
    }
    if (e->dragging) {
        if (!in->mouse_down) e->dragging = false;
        else {
            int line = (int)floorf((in->mouse_y - text.y + e->scroll_y) / e->line_h);
            line = line < 0 ? 0 : line >= e->n ? e->n - 1 : line;
            e->cy = line;
            e->cx = col_from_x(f, &e->lines[line], in->mouse_x - text.x + e->scroll_x);
        }
    }
    if (hover && in->wheel_y != 0) e->scroll_ty -= in->wheel_y;
    /* sideways only when the gesture is mostly sideways (a vertical swipe on a trackpad carries a
     * little horizontal motion too) */
    if (hover && in->wheel_x != 0 && fabsf(in->wheel_x) > fabsf(in->wheel_y)) e->scroll_x = fmaxf(0, e->scroll_x - in->wheel_x);
    if (focused) handle_keys(e, a);
    track_statement(e, a);

    /* Scrolling */
    float content_h = e->n * e->line_h + text.h * 0.5f;
    float max_scroll = fmaxf(0, content_h - text.h);
    if (e->scroll_to_cursor) {
        e->scroll_to_cursor = false;
        float cy0 = e->cy * e->line_h, cy1 = cy0 + e->line_h;
        if (cy0 < e->scroll_ty) e->scroll_ty = cy0;
        else if (cy1 > e->scroll_ty + text.h) e->scroll_ty = cy1 - text.h;
        float cx = font_width(f, e->lines[e->cy].s, e->cx);
        if (cx < e->scroll_x) e->scroll_x = fmaxf(0, cx - px(a, 40));
        else if (cx > e->scroll_x + text.w - px(a, 20)) e->scroll_x = cx - text.w + px(a, 60);
    }
    e->scroll_ty = clampf(e->scroll_ty, 0, max_scroll);
    /* never further sideways than the widest line (plus a margin): nothing to show past it */
    if (e->widest_rev != e->revision + 1 || e->widest_lh != e->line_h) {
        float wmax = 0;
        for (int i = 0; i < e->n; i++) { float w = font_width(f, e->lines[i].s, e->lines[i].len); if (w > wmax) wmax = w; }
        e->widest = wmax; e->widest_rev = e->revision + 1; e->widest_lh = e->line_h;
    }
    e->scroll_x = clampf(e->scroll_x, 0, fmaxf(0, e->widest + px(a, 60) - text.w));
    e->scroll_y = a->reduce_motion ? e->scroll_ty : lerpf(e->scroll_y, e->scroll_ty, 0.4f);   /* PNL-039 */
    if (fabsf(e->scroll_y - e->scroll_ty) > 0.5f) a->animating = true; else e->scroll_y = e->scroll_ty;

    /* Draw */
    clip_push(r, body);
    int first = (int)floorf(e->scroll_y / e->line_h);
    int last = (int)ceilf((e->scroll_y + text.h) / e->line_h) + 1;
    if (first < 0) first = 0;
    if (last > e->n) last = e->n;

    int sx0, sy0, sx1, sy1;
    bool sel = has_sel(e);
    if (sel) sel_range(e, &sx0, &sy0, &sx1, &sy1);

    /* block-comment state must be computed from the top */
    HlState hl; memset(&hl, 0, sizeof hl);
    const SqlLexRules *rules = sql_lex_rules(e->dialect);
    Token toks[512];
    for (int i = 0; i < first; i++) tokenize(e->lines[i].s, e->lines[i].len, &hl, rules, toks, 512);

    /* byte offset of each visible line (marks are byte ranges in the whole text) */
    int line_off[512]; int lo = 0;
    if (e->nmarks) { for (int i = 0; i < first; i++) lo += e->lines[i].len + 1; }
    for (int i = first; i < last && i - first < 512; i++) { line_off[i - first] = lo; lo += e->lines[i].len + 1; }
    for (int i = first; i < last; i++) {
        Line *l = &e->lines[i];
        float y = text.y + i * e->line_h - e->scroll_y;
        if (i - first >= 512) break;
        Rect row = rect_make(body.x, y, body.w, e->line_h);
        if (has_stmt && !e->object_mode && i >= stmt_first && i <= stmt_last) {
            draw_rect(r, row, color_alpha(color_hex(T_TEXT), g_theme->light ? 10 : 6));
            draw_rect(r, rect_make(text.x - px(a, 10), y, px(a, 2), e->line_h), color_alpha(color_hex(T_ACCENT), 150));
        }
        if (i == e->cy && focused && !sel) draw_rect(r, row, color_alpha(color_hex(T_LINE_HL), 200));

        /* selection background */
        if (sel && i >= sy0 && i <= sy1) {
            float x0 = i == sy0 ? line_x(e, f, i, sx0) : text.x - e->scroll_x;
            float x1 = i == sy1 ? line_x(e, f, i, sx1) : line_x(e, f, i, l->len) + font_width(f, " ", 1);
            draw_rect(r, rect_make(x0, y, x1 - x0, e->line_h), color_hex(focused ? T_SELECTED : T_HOVER));
        }

        /* gutter */
        char num[16];
        snprintf(num, sizeof num, "%d", i + 1);
        Color nc = color_hex(i == e->cy ? T_TEXT_MUTED : T_TEXT_DIM);
        draw_text_fit(f, r, num, -1, rect_make(body.x, y, e->gutter_w - px(a, 14), e->line_h), nc, ALIGN_RIGHT);

        /* text */
        clip_push(r, rect_make(text.x, body.y, text.w + px(a, 8), body.h));
        float ty = y + (e->line_h - font_height(f)) / 2;
        int nt = tokenize(l->s, l->len, &hl, rules, toks, 512);
        float x = text.x - e->scroll_x;
        for (int t = 0; t < nt; t++)
            x = font_draw(f, r, l->s + toks[t].start, toks[t].len, x, ty, token_color(toks[t].type));

        /* diagnostics: wavy underline under the range, a dot in the gutter (colour + shape, and
         * the Problems list says it in words) */
        if (e->nmarks && e->marks_rev == e->revision) {
            int ls = line_off[i - first], le = ls + l->len;
            for (int m = 0; m < e->nmarks; m++) {
                const EdMark *mk = &e->marks[m];
                if (mk->end < ls || mk->start > le) continue;
                if (mk->start == mk->end && (mk->start < ls || mk->start > le)) continue;
                int a0 = mk->start > ls ? mk->start - ls : 0, a1 = mk->end < le ? mk->end - ls : l->len;
                if (a1 <= a0) a1 = a0 + 1 <= l->len ? a0 + 1 : a0;
                float x0 = line_x(e, f, i, a0), x1 = a1 > a0 ? line_x(e, f, i, a1) : x0 + font_width(f, " ", 1);
                Color mc = color_hex(mk->severity == 0 ? T_RED : mk->severity == 1 ? T_YELLOW : T_TEXT_DIM);
                float base = y + e->line_h - px(a, 3), step = px(a, 2);
                for (float xx = x0; xx < x1; xx += step) draw_rect(r, rect_make(xx, base + (((int)((xx - x0) / step)) % 2 ? px(a, 1) : 0), step, px(a, 1.2f)), mc);
                if (a0 == (mk->start > ls ? mk->start - ls : 0) && mk->start >= ls)
                    draw_rrect(r, rect_make(body.x + px(a, 4), y + e->line_h / 2 - px(a, 3), px(a, 6), px(a, 6)), px(a, 3), mc);
            }
        }

        /* caret */
        if (focused && i == e->cy) {
            bool on = ((SDL_GetTicks() - e->blink_start) / 530) % 2 == 0;
            if (on) draw_rect(r, rect_make(line_x(e, f, i, e->cx), y + px(a, 2), px(a, 2), e->line_h - px(a, 4)), color_hex(T_ACCENT));
            app_wake_in(a, 530 - (Uint32)((SDL_GetTicks() - e->blink_start) % 530));
        }
        clip_pop(r);
    }

    (void)max_scroll;
    ui_scrollbar_draw(a, &e->sb, sbv, content_h, e->scroll_y);
    clip_pop(r);

    /* anchor for the completion popup: under the start of the prefix being completed */
    e->popup_x = line_x(e, f, e->cy, e->cx - (e->copen ? e->cprefix : 0));
    e->popup_y = text.y + e->cy * e->line_h - e->scroll_y + e->line_h;
}

/* One badge system for every kind: 14pt chip, tinted fill, one letter. */
/* Draws the small kind badge (C column, A alias, S schema, T table, V view, O other object,
 * F function, K keyword) centred on (cx, cy). One entry per CAND_* kind, in enum order. */
static void draw_kind_badge(App *a, float cx, float cy, int kind) {
    SDL_Renderer *r = a->renderer;
    static const char *const letters[] = { "C", "A", "S", "T", "V", "O", "F", "K" };
    static const uint32_t tints[] = { 0x9AA3B0, 0x5BC0DE, 0xD9A441, 0x5B8DEF, 0x3DBE8B, 0xC08BD9, 0xE0A85B, 0xA78BFA };
    _Static_assert(sizeof letters / sizeof letters[0] == CAND_KEYWORD + 1 && sizeof tints / sizeof tints[0] == CAND_KEYWORD + 1, "one badge per candidate kind");
    if (kind < 0 || kind > CAND_KEYWORD) return;
    float s = px(a, 7);
    Color c = color_hex(tints[kind]);
    draw_rrect(r, rect_make(cx - s, cy - s, s * 2, s * 2), px(a, 3), color_alpha(c, 56));
    draw_text_fit(a->font_small, r, letters[kind], -1, rect_make(cx - s, cy - s, s * 2, s * 2), c, ALIGN_CENTER);
}

/* Draws `text` with the part matching `prefix` (first case-insensitive occurrence) in accent. */
static float draw_match(App *a, const char *text, const char *prefix, int plen, float x, float y, Color base, Color hl) {
    Font *f = a->font_mono;
    SDL_Renderer *r = a->renderer;
    int tlen = (int)strlen(text), at = -1;
    if (plen > 0) for (int i = 0; i + plen <= tlen; i++) if (strncasecmp(text + i, prefix, plen) == 0) { at = i; break; }
    if (at < 0) return font_draw(f, r, text, tlen, x, y, base);
    x = font_draw(f, r, text, at, x, y, base);
    x = font_draw(f, r, text + at, plen, x, y, hl);
    return font_draw(f, r, text + at + plen, tlen - at - plen, x, y, base);
}

/* Draw the completion popup (up to 8 rows, scrolling) under the completed prefix, or above the
 * line when it would hit the status bar. Also stores its rect for next frame's hit testing. */
void editor_draw_overlay(Editor *e, App *a) {
    if (!e->copen || e->ncands == 0) { e->popup_rect = rect_make(0, 0, 0, 0); return; }
    SDL_Renderer *r = a->renderer;
    float row_h = px(a, COMPLETE_ROW), pad = px(a, 4);
    int visible = e->ncands < 8 ? e->ncands : 8;
    const char *prefix = e->lines[e->cy].s + e->cx - e->cprefix;
    if (e->csel < e->cscroll) e->cscroll = e->csel;
    if (e->csel >= e->cscroll + visible) e->cscroll = e->csel - visible + 1;
    if (e->cscroll > e->ncands - visible) e->cscroll = e->ncands - visible;
    if (e->cscroll < 0) e->cscroll = 0;

    /* size: widest proposal + widest detail, clamped to 220..460 points; position it on screen */
    float tw = 0, dw = 0;
    for (int i = 0; i < e->ncands; i++) {
        float w = font_width(a->font_mono, e->cands[i].label[0] ? e->cands[i].label : e->cands[i].text, -1);
        if (w > tw) tw = w;
        w = font_width(a->font_small, e->cands[i].detail, -1);
        if (w > dw) dw = w;
    }
    float w = clampf(px(a, 34) + tw + px(a, 24) + dw + px(a, 14), px(a, 220), px(a, 460));
    if (dw > w * 0.5f) dw = w * 0.5f;
    float h = visible * row_h + pad * 2;
    float x = e->popup_x - px(a, 34), y = e->popup_y + px(a, 4);
    if (x + w > a->width - px(a, 8)) x = a->width - px(a, 8) - w;
    if (x < px(a, 8)) x = px(a, 8);
    if (y + h > a->height - px(a, M_STATUSBAR + 8)) y = e->popup_y - e->line_h - h - px(a, 2);
    Rect rc = rect_make(x, y, w, h);
    e->popup_rect = rc;

    /* shadow, border, background, then the visible rows: badge, text with the match
     * highlighted, detail right-aligned */
    draw_rrect(r, rect_make(rc.x + px(a, 2), rc.y + px(a, 4), rc.w, rc.h), px(a, M_RADIUS), color_alpha(color_hex(0x000000), 90));
    draw_rrect(r, rc, px(a, M_RADIUS), color_hex(T_BORDER));
    draw_rrect(r, rect_inset(rc, px(a, 1), px(a, 1)), px(a, M_RADIUS - 1), color_hex(T_PANEL_ALT));
    clip_push(r, rect_inset(rc, px(a, 1), pad));
    for (int i = e->cscroll; i < e->cscroll + visible && i < e->ncands; i++) {
        Cand *cd = &e->cands[i];
        Rect row = rect_make(rc.x + px(a, 4), rc.y + pad + (i - e->cscroll) * row_h, rc.w - px(a, 8), row_h);
        bool sel = i == e->csel;
        if (sel) {
            draw_rrect(r, row, px(a, 3), color_hex(T_SELECTED));
            draw_rect(r, rect_make(row.x, row.y + px(a, 3), px(a, 2), row.h - px(a, 6)), color_hex(T_ACCENT));
        } else if (i == e->chover) draw_rrect(r, row, px(a, 3), color_hex(T_HOVER));
        if (!cd->info) draw_kind_badge(a, row.x + px(a, 16), row.y + row.h / 2, cd->kind);   /* a state row ("Loading columns of…") is not an object: no badge */
        Rect tr = rect_make(row.x + px(a, 30), row.y, row.w - px(a, 30), row.h);
        float dwi = font_width(a->font_small, cd->detail, -1);
        if (dwi > dw) dwi = dw;
        clip_push(r, rect_make(tr.x, tr.y, tr.w - dwi - px(a, 14), tr.h));
        if (cd->label[0]) draw_text(a->font_mono, r, cd->label, tr.x, tr.y + (row.h - font_height(a->font_mono)) / 2, color_hex(cd->info ? T_TEXT_DIM : sel ? 0xFFFFFF : T_TEXT));
        else draw_match(a, cd->text, prefix, e->cprefix, tr.x, tr.y + (row.h - font_height(a->font_mono)) / 2,
                        color_hex(sel ? 0xFFFFFF : T_TEXT), color_hex(sel ? 0xA8C4FF : T_ACCENT));
        clip_pop(r);
        draw_text_fit(a->font_small, r, cd->detail, -1, rect_make(tr.x + tr.w - px(a, 8) - dwi, tr.y, dwi, tr.h), color_hex(sel ? 0xB4BCC8 : T_TEXT_DIM), ALIGN_RIGHT);   /* 0xB4BCC8: 4.5:1 on the selected row */
    }
    clip_pop(r);
    if (e->ncands > visible) {
        float track = h - pad * 2, thumb = fmaxf(px(a, 16), track * visible / e->ncands);
        float ty = rc.y + pad + (track - thumb) * ((float)e->cscroll / (e->ncands - visible));
        draw_rrect(r, rect_make(rc.x + rc.w - px(a, 5), ty, px(a, 3), thumb), px(a, 1.5f), color_hex(T_SCROLLBAR));
    }
}

/* See editor.h. */
void editor_set_marks(Editor *e, const EdMark *m, int n) {
    if (n > EDITOR_MARKS_MAX) n = EDITOR_MARKS_MAX;
    if (n > 0) memcpy(e->marks, m, sizeof *m * (size_t)n);
    e->nmarks = n < 0 ? 0 : n; e->marks_rev = e->revision;
}
void editor_set_object_mode(Editor *e, bool on) { e->object_mode = on; }
void editor_set_read_only(Editor *e, bool on) { e->read_only = on; }
void editor_set_dialect(Editor *e, int dialect) { if (e) e->dialect = dialect; }
bool editor_read_only(const Editor *e) { return e && e->read_only; }
bool editor_object_mode(const Editor *e) { return e && e->object_mode; }
/* See editor.h. */
bool editor_goto_offset(Editor *e, int off) {
    int line = 0, rem = off;
    while (line < e->n - 1 && rem > e->lines[line].len) { rem -= e->lines[line].len + 1; line++; }
    if (rem > e->lines[line].len) rem = e->lines[line].len;
    if (rem < 0) return false;
    e->cy = e->ay = line; e->cx = e->ax = rem; e->want_x = -1; e->scroll_to_cursor = true;
    return true;
}
/* See editor.h. */
void editor_replace_all(Editor *e, const char *text, int len) {
    editor_select_all(e);
    editor_insert(e, text, len);
}

/* Harness: the horizontal scroll and the widest line (px), to check the scroll stays within the text. */
void editor_scroll_state(const Editor *e, float *sx, float *widest, float *view_w) { *sx = e->scroll_x; *widest = e->widest; *view_w = e->text_rect.w; }
