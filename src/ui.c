/* SPDX-License-Identifier: AGPL-3.0-only */
/* ui.c: small immediate-mode widgets used by dialogs, panels and the shell.
 *
 * Immediate mode: there is no widget tree. Each frame the owner calls ui_button(), ui_checkbox(),
 * ui_textfield(), popup_update() / popup_draw() ... with a rectangle in render pixels; the call
 * draws the widget and reacts to this frame's input in App.in, returning what happened (clicked,
 * toggled, chosen id). Mouse clicks are claimed through Input.mouse_consumed so that only the
 * first widget under the pointer reacts; keys are claimed through KeyPress.used. Widget state
 * that must survive between frames (TextField, PopupMenu) lives in caller-owned structs.
 *
 * Sections:
 *   - test-harness targets: every labelled widget records its rectangle each frame so script.c
 *     can click "Apply" by name (ui_record / ui_target);
 *   - text field: single-line, macOS-like editing with undo (documented in ui.h);
 *   - buttons, segmented control, checkbox, label;
 *   - popup menus with cascading submenus (keyboard and pointer navigation).
 *
 * All state here is main-thread only. Colors and metrics come from theme.h. */
#include "ui.h"
#include <strings.h>
#include "draw.h"
#include "font.h"
#include "theme.h"

/* Harness targets recorded during the current frame (label truncated to 63 bytes, rect in render
 * pixels); at most 512 per frame, later ones are dropped. ui_frame counts frames. */
typedef struct { char label[64]; Rect r; } UiTarget;
static UiTarget targets[512]; static int ntargets;
static Uint64 ui_frame;
/* Start a new frame: forget last frame's targets. main.c calls it before drawing. */
void ui_new_frame(void) { ntargets = 0; ui_frame++; }
/* Remember a widget's label (leading spaces skipped; empty labels ignored) and rect for ui_target(). */
void ui_record(const char *label, Rect rc) {
    if (!label || ntargets >= 512) return;
    while (*label == ' ') label++;
    if (!*label) return;
    snprintf(targets[ntargets].label, sizeof targets[0].label, "%s", label);
    targets[ntargets++].r = rc;
}
/* Find the most recently recorded target whose label starts with `prefix` (case-insensitive);
 * later = drawn on top. Writes its rect (render pixels) to *out. */
bool ui_target(const char *prefix, Rect *out) {
    size_t n = strlen(prefix);
    for (int i = ntargets - 1; i >= 0; i--) if (strncasecmp(targets[i].label, prefix, n) == 0) { *out = targets[i].r; return true; }
    return false;
}

/* ---- text field ---------------------------------------------------------- */

/* Frame counters: ui_text_editing() reports whether a focused field was drawn last frame. */
static Uint64 tf_focus_frame;

static const TextField *tf_focused;
const TextField *ui_focused_field(void) { return tf_focused; }
bool ui_text_editing(void) { return tf_focus_frame + 1 >= ui_frame && tf_focus_frame != 0; }

void textfield_set(TextField *t, const char *s) {
    snprintf(t->buf, sizeof t->buf, "%s", s ? s : "");
    t->len = (int)strlen(t->buf);
    t->cursor = t->anchor = t->len;
    t->scroll = 0;
    t->nundo = t->nredo = 0;
    t->last_op = 0;
}

static int tf_lo(const TextField *t) { return t->cursor < t->anchor ? t->cursor : t->anchor; }
static int tf_hi(const TextField *t) { return t->cursor > t->anchor ? t->cursor : t->anchor; }
static bool tf_has_sel(const TextField *t) { return t->cursor != t->anchor; }

/* ---- history ---- */

enum { TF_OP_NONE, TF_OP_TYPE, TF_OP_DELETE, TF_OP_OTHER };

static void tf_snap(const TextField *t, TfSnap *s) {
    memcpy(s->buf, t->buf, sizeof s->buf);
    s->len = (short)t->len; s->cursor = (short)t->cursor; s->anchor = (short)t->anchor;
}
static void tf_restore(TextField *t, const TfSnap *s) {
    memcpy(t->buf, s->buf, sizeof t->buf);
    t->len = s->len; t->cursor = s->cursor; t->anchor = s->anchor;
}
static void tf_push(TfSnap *stack, int *n, const TextField *t) {
    if (*n == TF_UNDO) { memmove(stack, stack + 1, sizeof(TfSnap) * (TF_UNDO - 1)); (*n)--; }   /* drop the oldest */
    tf_snap(t, &stack[(*n)++]);
}
/* Records the state before an edit of kind `op`. Consecutive typing (or deleting) is one step;
 * any other edit, a caret move in between or a selection being replaced starts a new one. */
static void tf_begin_edit(TextField *t, int op) {
    if (op == TF_OP_OTHER || op != t->last_op || tf_has_sel(t)) tf_push(t->undo, &t->nundo, t);
    t->nredo = 0;
    t->last_op = op;
}
static void tf_undo(TextField *t) {
    if (!t->nundo) return;
    tf_push(t->redo, &t->nredo, t);
    tf_restore(t, &t->undo[--t->nundo]);
    t->last_op = TF_OP_NONE;
}
static void tf_redo(TextField *t) {
    if (!t->nredo) return;
    tf_push(t->undo, &t->nundo, t);
    tf_restore(t, &t->redo[--t->nredo]);
    t->last_op = TF_OP_NONE;
}

/* ---- primitive edits ---- */

/* Removes bytes [from, to) and leaves the caret, without a selection, at `from`. */
static void tf_erase(TextField *t, int from, int to) {
    if (to <= from) return;
    memmove(t->buf + from, t->buf + to, t->len - to + 1);
    t->len -= to - from;
    t->cursor = t->anchor = from;
}

/* Replaces the selection (if any) with n bytes of s, cut at a UTF-8 boundary when full. */
static void tf_insert(TextField *t, const char *s, int n) {
    if (tf_has_sel(t)) tf_erase(t, tf_lo(t), tf_hi(t));
    int room = (int)sizeof t->buf - 1 - t->len;
    if (n > room) { n = room; while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--; }
    if (n <= 0) return;
    memmove(t->buf + t->cursor + n, t->buf + t->cursor, t->len - t->cursor + 1);
    memcpy(t->buf + t->cursor, s, n);
    t->len += n; t->cursor += n; t->anchor = t->cursor;
}

/* ---- word boundaries ---- */

/* 0 space, 1 word (letters, digits, '_' and any non-ASCII), 2 punctuation */
static int tf_class(unsigned char c) {
    if (c == ' ' || c == '\t') return 0;
    if (c >= 0x80 || c == '_' || (c >= '0' && c <= '9') || ((c | 0x20) >= 'a' && (c | 0x20) <= 'z')) return 1;
    return 2;
}
/* Option+Left: skips spaces, then the run of the class just before. Password fields: the start. */
static int tf_word_left(const TextField *t, int i) {
    if (t->password) return 0;
    while (i > 0 && tf_class((unsigned char)t->buf[i - 1]) == 0) i--;
    if (i > 0) { int c = tf_class((unsigned char)t->buf[i - 1]); while (i > 0 && tf_class((unsigned char)t->buf[i - 1]) == c) i--; }
    return i;
}
/* Option+Right: skips spaces, then the run of the class at the caret. Password fields: the end. */
static int tf_word_right(const TextField *t, int i) {
    if (t->password) return t->len;
    while (i < t->len && tf_class((unsigned char)t->buf[i]) == 0) i++;
    if (i < t->len) { int c = tf_class((unsigned char)t->buf[i]); while (i < t->len && tf_class((unsigned char)t->buf[i]) == c) i++; }
    return i;
}
/* Double-click: the run of one class around byte i. */
static void tf_word_at(const TextField *t, int i, int *lo, int *hi) {
    if (t->password || t->len == 0) { *lo = 0; *hi = t->len; return; }
    if (i >= t->len) i = t->len - 1;
    int c = tf_class((unsigned char)t->buf[i]);
    int a = i, b = i;
    while (a > 0 && tf_class((unsigned char)t->buf[a - 1]) == c) a--;
    while (b < t->len && tf_class((unsigned char)t->buf[b]) == c) b++;
    *lo = a; *hi = b;
}

/* ---- display ---- */

/* The text as drawn: the content, or one bullet per code point for passwords. */
static void masked(const TextField *t, char *out, size_t n) {
    size_t p = 0;
    for (int i = 0; i < t->len && p + 3 < n;) { uint32_t cp; i = utf8_next(t->buf, t->len, i, &cp); memcpy(out + p, "\xE2\x80\xA2", 3); p += 3; }
    out[p] = 0;
}
/* Byte offset in the drawn text that matches byte i of buf (a bullet is 3 bytes). */
static int tf_shown_off(const TextField *t, int i) {
    if (!t->password) return i;
    int cps = 0;
    for (int k = 0; k < i && k < t->len;) { uint32_t cp; k = utf8_next(t->buf, t->len, k, &cp); cps++; }
    return cps * 3;
}
/* x of byte i, relative to the start of the text, in render pixels */
static float tf_x(App *a, const TextField *t, const char *shown, int i) {
    return font_width(a->font_ui, shown, tf_shown_off(t, i));
}
/* Byte position nearest to x (relative to the start of the text). */
static int tf_index_at(App *a, const TextField *t, const char *shown, float x) {
    float prev = 0;
    for (int i = 0; i < t->len;) {
        uint32_t cp; int nx = utf8_next(t->buf, t->len, i, &cp);
        float w = tf_x(a, t, shown, nx);
        if (x < (prev + w) / 2) return i;
        prev = w; i = nx;
    }
    return t->len;
}

/* ---- keyboard ---- */

/* Moves the caret to `to`; with Shift the anchor stays and the selection grows. */
static void tf_move(TextField *t, int to, bool shift) {
    t->cursor = to;
    if (!shift) t->anchor = to;
    t->last_op = TF_OP_NONE;
}

/* Deletes the selection, or the range from the caret to `to` when nothing is selected. */
static void tf_delete_to(TextField *t, int to) {
    int a = tf_has_sel(t) ? tf_lo(t) : (to < t->cursor ? to : t->cursor);
    int b = tf_has_sel(t) ? tf_hi(t) : (to < t->cursor ? t->cursor : to);
    if (a == b) return;
    tf_begin_edit(t, b - a > 4 || tf_has_sel(t) ? TF_OP_OTHER : TF_OP_DELETE);
    tf_erase(t, a, b);
}

static void tf_copy(const TextField *t) {
    if (t->password || !tf_has_sel(t)) return;
    char tmp[sizeof t->buf];
    int n = tf_hi(t) - tf_lo(t);
    memcpy(tmp, t->buf + tf_lo(t), n); tmp[n] = 0;
    SDL_SetClipboardText(tmp);
}

static void tf_paste(TextField *t) {
    char *s = SDL_GetClipboardText();
    if (!s) return;
    /* one line: control characters (line breaks, tabs) become spaces, a trailing break is dropped */
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) n--;
    for (size_t i = 0; i < n; i++) if ((unsigned char)s[i] < 0x20) s[i] = ' ';
    if (n) { tf_begin_edit(t, TF_OP_OTHER); tf_insert(t, s, (int)n); }
    SDL_free(s);
}

/* Applies one key press; false when the key is not a text-editing key (left for the caller). */
static bool tf_key(TextField *t, const KeyPress *kp) {
    bool prim = kp->mods & KM_PRIMARY, alt = kp->mods & KM_ALT, shift = kp->mods & KM_SHIFT, ctrl = kp->mods & KM_CTRL;
    uint32_t cp;
    switch (kp->key) {
    case SDLK_LEFT:
        if (prim) tf_move(t, 0, shift);
        else if (alt) tf_move(t, tf_word_left(t, t->cursor), shift);
        else if (tf_has_sel(t) && !shift) tf_move(t, tf_lo(t), false);   /* collapse to the start */
        else tf_move(t, t->cursor > 0 ? utf8_prev(t->buf, t->cursor) : 0, shift);
        return true;
    case SDLK_RIGHT:
        if (prim) tf_move(t, t->len, shift);
        else if (alt) tf_move(t, tf_word_right(t, t->cursor), shift);
        else if (tf_has_sel(t) && !shift) tf_move(t, tf_hi(t), false);   /* collapse to the end */
        else tf_move(t, t->cursor < t->len ? utf8_next(t->buf, t->len, t->cursor, &cp) : t->len, shift);
        return true;
    case SDLK_HOME: tf_move(t, 0, shift); return true;
    case SDLK_END: tf_move(t, t->len, shift); return true;
    case SDLK_BACKSPACE:
        if (prim) tf_delete_to(t, 0);
        else if (alt) tf_delete_to(t, tf_word_left(t, t->cursor));
        else tf_delete_to(t, t->cursor > 0 ? utf8_prev(t->buf, t->cursor) : 0);
        return true;
    case SDLK_DELETE:
        if (prim) tf_delete_to(t, t->len);
        else if (alt) tf_delete_to(t, tf_word_right(t, t->cursor));
        else tf_delete_to(t, t->cursor < t->len ? utf8_next(t->buf, t->len, t->cursor, &cp) : t->len);
        return true;
    case SDLK_A:
        if (prim && !shift) { t->anchor = 0; t->cursor = t->len; t->last_op = TF_OP_NONE; return true; }
        if (ctrl) { tf_move(t, 0, shift); return true; }        /* Emacs-style start of line (macOS) */
        return false;
    case SDLK_E:
        if (ctrl) { tf_move(t, t->len, shift); return true; }  /* end of line */
        return false;
    case SDLK_K:
        if (ctrl) { t->anchor = t->cursor; tf_delete_to(t, t->len); return true; }   /* delete to the end */
        return false;
    case SDLK_C: if (prim && !shift) { tf_copy(t); return true; } return false;
    case SDLK_X:
        if (prim && !shift) { if (!t->password && tf_has_sel(t)) { tf_copy(t); tf_delete_to(t, t->cursor); } return true; }
        return false;
    case SDLK_V: if (prim && !shift) { tf_paste(t); return true; } return false;
    case SDLK_Z:
        if (prim && shift) { tf_redo(t); return true; }
        if (prim) { tf_undo(t); return true; }
        return false;
    default: return false;
    }
}

/* ---- right-click menu ---- */

enum { TFM_UNDO = 1, TFM_REDO, TFM_CUT, TFM_COPY, TFM_PASTE, TFM_DELETE, TFM_SELECT_ALL };
static PopupMenu tf_menu;
static const TextField *tf_menu_target;   /* compared, never dereferenced, until it is drawn again */
static Uint64 tf_menu_seen;               /* last frame the target field was drawn */
static int tf_menu_pending;               /* chosen action waiting for the field's next draw; 0 = none */

/* Opens the menu for t at the pointer, with each item enabled only when it can do something. */
static void tf_menu_open(TextField *t, float x, float y) {
    bool sel = tf_has_sel(t), secret = t->password;
    popup_open(&tf_menu, x, y);
    popup_add(&tf_menu, TFM_UNDO, "Undo", false, t->nundo > 0);
    popup_add(&tf_menu, TFM_REDO, "Redo", false, t->nredo > 0);
    popup_add(&tf_menu, TFM_CUT, "Cut", true, sel && !secret);
    popup_add(&tf_menu, TFM_COPY, "Copy", false, sel && !secret);
    popup_add(&tf_menu, TFM_PASTE, "Paste", false, SDL_HasClipboardText());
    popup_add(&tf_menu, TFM_DELETE, "Delete", false, sel);
    popup_add(&tf_menu, TFM_SELECT_ALL, "Select All", true, t->len > 0);
    tf_menu_target = t; tf_menu_seen = ui_frame; tf_menu_pending = 0;
}

/* Runs a menu action on its field (the same operations as the keyboard shortcuts). */
static void tf_menu_apply(TextField *t, int id) {
    switch (id) {
    case TFM_UNDO: tf_undo(t); break;
    case TFM_REDO: tf_redo(t); break;
    case TFM_CUT: if (!t->password && tf_has_sel(t)) { tf_copy(t); tf_delete_to(t, t->cursor); } break;
    case TFM_COPY: tf_copy(t); break;
    case TFM_PASTE: tf_paste(t); break;
    case TFM_DELETE: if (tf_has_sel(t)) tf_delete_to(t, t->cursor); break;
    case TFM_SELECT_ALL: t->anchor = 0; t->cursor = t->len; t->last_op = TF_OP_NONE; break;
    }
    t->blink = SDL_GetTicks();
}

void ui_textfield_menu_update(App *a) {
    if (!tf_menu.open) return;
    if (tf_menu_seen + 2 < ui_frame) { popup_close(&tf_menu); tf_menu_target = NULL; return; }   /* its field is gone */
    int id = popup_update(a, &tf_menu);
    if (id > 0) { tf_menu_pending = id; popup_close(&tf_menu); app_wake_in(a, 0); }
}

void ui_textfield_menu_draw(App *a) { if (tf_menu.open) popup_draw(a, &tf_menu); }

bool ui_textfield_menu_pos(App *a, const char *label, float *x, float *y) {
    Rect rc;
    if (!tf_menu.open || !popup_item_rect(a, &tf_menu, label, &rc)) return false;
    *x = (rc.x + rc.w / 2) / a->density; *y = (rc.y + rc.h / 2) / a->density;
    return true;
}

/* ---- the widget ---- */

bool ui_textfield(App *a, TextField *t, Rect rc, const char *placeholder, bool focused) {
    SDL_Renderer *r = a->renderer;
    Input *in = &a->in;
    t->rect = rc;
    /* callers may edit buf / len directly (e.g. digits-only fields): keep the positions valid */
    if (t->len < 0 || t->len >= (int)sizeof t->buf) t->len = (int)strlen(t->buf);
    if (t->cursor > t->len) t->cursor = t->len;
    if (t->cursor < 0) t->cursor = 0;
    if (t->anchor > t->len || t->anchor < 0) t->anchor = t->cursor;

    Rect inner = rect_inset(rc, px(a, 10), 0);
    { char tg[96]; snprintf(tg, sizeof tg, "field:%s", placeholder ? placeholder : ""); ui_record(tg, rc); }   /* harness: fields by placeholder */
    bool hover = rect_contains(rc, in->mouse_x, in->mouse_y);
    bool clicked = false;
    if (hover) a->want_cursor = CURSOR_IBEAM;

    char shown[1024];
    /* a right-click menu action chosen for this field: applied now that the field is known alive */
    if (t == tf_menu_target) {
        tf_menu_seen = ui_frame;
        if (tf_menu_pending) { tf_menu_apply(t, tf_menu_pending); tf_menu_pending = 0; tf_menu_target = NULL; }
    }
    if (t->password) masked(t, shown, sizeof shown); else snprintf(shown, sizeof shown, "%s", t->buf);

    /* right-click: the menu; a click outside the selection first moves the caret there (macOS) */
    if (hover && in->mouse_rpressed) {
        in->mouse_rpressed = false; clicked = true;
        int at = tf_index_at(a, t, shown, in->mouse_x - inner.x + t->scroll);
        if (!tf_has_sel(t) || at < tf_lo(t) || at > tf_hi(t)) { t->cursor = t->anchor = at; t->last_op = TF_OP_NONE; }
        tf_menu_open(t, in->mouse_x, in->mouse_y);
    }

    /* mouse: click places the caret, Shift+click extends, drag selects, double-click selects a
     * word and triple-click the whole text. Works on the click that gives the field focus too. */
    if (hover && in->mouse_pressed && !in->mouse_consumed) {
        in->mouse_consumed = true; clicked = true;
        int at = tf_index_at(a, t, shown, in->mouse_x - inner.x + t->scroll);
        if (in->clicks >= 3) { t->anchor = 0; t->cursor = t->len; }
        else if (in->clicks == 2) { int lo, hi; tf_word_at(t, at, &lo, &hi); t->anchor = lo; t->cursor = hi; }
        else { t->cursor = at; if (!(app_modstate(a) & SDL_KMOD_SHIFT)) t->anchor = at; t->dragging = true; }
        t->last_op = TF_OP_NONE;
        t->blink = SDL_GetTicks();
    }
    if (t->dragging) {
        /* follow the pointer, including the frame of the release: a fast drag can deliver its
         * last motion and the button-up together, and that motion is where the selection ends */
        t->cursor = tf_index_at(a, t, shown, in->mouse_x - inner.x + t->scroll);
        if (!in->mouse_down) t->dragging = false;
        else if (in->mouse_x < inner.x || in->mouse_x > inner.x + inner.w) app_wake_in(a, 30);   /* past either edge the text scrolls under the pointer */
    }

    if (focused && t->input_frame != ui_frame) {
        t->input_frame = ui_frame;
        for (int k = 0; k < in->nkeys; k++) {
            KeyPress *kp = &in->keys[k];
            if (kp->used || !tf_key(t, kp)) continue;
            kp->used = true;
            t->blink = SDL_GetTicks();
        }
        if (in->ntext) {
            tf_begin_edit(t, TF_OP_TYPE);
            tf_insert(t, in->text, in->ntext);
            in->ntext = 0; t->blink = SDL_GetTicks();
        }
        if (t->password) masked(t, shown, sizeof shown); else snprintf(shown, sizeof shown, "%s", t->buf);
    }
    if (focused) { tf_focus_frame = ui_frame; tf_focused = t; }

    /* horizontal scroll: keep the caret inside the field, and no empty space after the text */
    float caret_x = tf_x(a, t, shown, t->cursor);
    float text_w = font_width(a->font_ui, shown, -1);
    float room = inner.w - px(a, 2);
    if (caret_x - t->scroll > room) t->scroll = caret_x - room;
    if (caret_x - t->scroll < 0) t->scroll = caret_x;
    if (t->scroll > 0 && text_w - t->scroll < room) t->scroll = text_w - room > 0 ? text_w - room : 0;
    if (t->scroll < 0) t->scroll = 0;

    draw_rrect(r, rc, px(a, M_RADIUS), color_hex(focused ? T_ACCENT : hover ? T_TEXT_DIM : T_BORDER));
    draw_rrect(r, rect_inset(rc, px(a, 1), px(a, 1)), px(a, M_RADIUS - 1), color_hex(T_BG));
    clip_push(r, inner);
    float ty = rc.y + (rc.h - font_height(a->font_ui)) / 2;
    float tx = inner.x - t->scroll;
    if (focused && tf_has_sel(t)) {
        float x0 = tf_x(a, t, shown, tf_lo(t)), x1 = tf_x(a, t, shown, tf_hi(t));
        draw_rect(r, rect_make(tx + x0, rc.y + px(a, 6), x1 - x0, rc.h - px(a, 12)), color_hex(T_SELECTED));
    }
    if (t->len == 0 && placeholder) draw_text(a->font_ui, r, placeholder, inner.x, ty, color_hex(T_TEXT_DIM));   /* kept while focused: an empty field still says what it takes (or its default) */
    else draw_text(a->font_ui, r, shown, tx, ty, color_hex(T_TEXT));
    if (focused) {
        bool on = tf_has_sel(t) || ((SDL_GetTicks() - t->blink) / 530) % 2 == 0;
        if (on) draw_rect(r, rect_make(tx + caret_x, rc.y + px(a, 7), px(a, 1.5f), rc.h - px(a, 14)), color_hex(T_ACCENT));
        if (!tf_has_sel(t)) app_wake_in(a, 530 - (Uint32)((SDL_GetTicks() - t->blink) % 530));
    }
    clip_pop(r);
    return clicked;
}

/* Push button with a centred, ellipsised label. `style` is BTN_*; a disabled button is drawn
 * greyed and never reacts. Records itself as a harness target. Returns true on the frame it is
 * pressed (on mouse down, not release) and consumes that click. */
bool ui_button(App *a, Rect rc, const char *label, int style, bool enabled) {
    SDL_Renderer *r = a->renderer;
    ui_record(label, rc);
    Input *in = &a->in;
    bool hover = enabled && rect_contains(rc, in->mouse_x, in->mouse_y);
    bool clicked = hover && in->mouse_pressed && !in->mouse_consumed;
    if (clicked) in->mouse_consumed = true;
    if (hover) a->want_cursor = CURSOR_HAND;
    Color fill, text;
    switch (style) {
    case BTN_PRIMARY: fill = color_hex(hover ? T_ACCENT_HOVER : T_ACCENT); text = color_hex(T_ON_ACCENT); break;
    case BTN_DANGER: fill = color_alpha(color_hex(T_RED), hover ? 70 : 45); text = color_hex(T_RED); break;
    /* QUIET is a ghost button: a faint outline at rest (so it reads as clickable, not as text), filled on hover */
    case BTN_QUIET: fill = color_alpha(color_hex(T_HOVER), hover ? 255 : 0); text = color_hex(hover ? T_TEXT : T_TEXT_MUTED); break;
    case BTN_SELECTED: fill = color_hex(hover ? 0x324670 : T_SELECTED); text = color_hex(T_TEXT); break;
    default: fill = color_hex(hover ? T_HOVER : T_PANEL_ALT); text = color_hex(T_TEXT); break;
    }
    if (!enabled) { fill = style == BTN_QUIET ? color_alpha(color_hex(T_HOVER), 0) : color_hex(T_PANEL_ALT); text = color_hex(T_TEXT_DIM); }
    if (style == BTN_QUIET) {   /* ghost: the outline only, the background shows through */
        if (hover) draw_rrect(r, rc, px(a, M_RADIUS), fill);
        draw_rrect_outline(r, rc, px(a, M_RADIUS), px(a, 1), color_hex(T_BORDER));
    } else {
        if (style != BTN_PRIMARY) draw_rrect(r, rc, px(a, M_RADIUS), color_hex(style == BTN_SELECTED ? T_ACCENT_DIM : T_BORDER));
        draw_rrect(r, style == BTN_PRIMARY ? rc : rect_inset(rc, px(a, 1), px(a, 1)), px(a, M_RADIUS - 1), fill);
    }
    draw_text_fit(a->font_ui, r, label, -1, rc, text, ALIGN_CENTER);
    return clicked;
}

/* Segmented control: `n` equal-width options inside rc, `selected` highlighted. Each segment is a
 * harness target. Returns the index pressed this frame (the caller stores it), or -1. */
int ui_segmented(App *a, Rect rc, const char *const *labels, int n, int selected) {
    SDL_Renderer *r = a->renderer;
    Input *in = &a->in;
    int picked = -1;
    draw_rrect(r, rc, px(a, M_RADIUS), color_hex(T_BORDER));
    draw_rrect(r, rect_inset(rc, px(a, 1), px(a, 1)), px(a, M_RADIUS - 1), color_hex(T_BG));
    float w = (rc.w - px(a, 4)) / n;
    for (int i = 0; i < n; i++) {
        Rect seg = rect_make(rc.x + px(a, 2) + i * w, rc.y + px(a, 2), w, rc.h - px(a, 4));
        ui_record(labels[i], seg);
        bool hover = rect_contains(seg, in->mouse_x, in->mouse_y);
        if (hover) a->want_cursor = CURSOR_HAND;
        if (hover && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; picked = i; }
        bool sel = i == selected;
        if (sel) draw_rrect(r, seg, px(a, M_RADIUS - 2), color_hex(T_SELECTED));
        else if (hover) draw_rrect(r, seg, px(a, M_RADIUS - 2), color_hex(T_HOVER));
        draw_text_fit(a->font_ui, r, labels[i], -1, seg, color_hex(sel ? T_TEXT : T_TEXT_MUTED), ALIGN_CENTER);
    }
    return picked;
}

/* Checkbox: a 16-point box with a check mark when `value`, followed by the label; the whole rc
 * is clickable. Returns true when clicked this frame; the caller flips its own value. */
bool ui_checkbox(App *a, Rect rc, const char *label, bool value, bool enabled) {
    SDL_Renderer *r = a->renderer;
    ui_record(label, rc);
    Input *in = &a->in;
    bool hover = enabled && rect_contains(rc, in->mouse_x, in->mouse_y);
    bool clicked = hover && in->mouse_pressed && !in->mouse_consumed;
    if (clicked) in->mouse_consumed = true;
    if (hover) a->want_cursor = CURSOR_HAND;
    float s = px(a, 16);
    Rect box = rect_make(rc.x, rc.y + (rc.h - s) / 2, s, s);
    draw_rrect(r, box, px(a, 4), color_hex(value && enabled ? T_ACCENT : T_BORDER));
    if (!value || !enabled) draw_rrect(r, rect_inset(box, px(a, 1), px(a, 1)), px(a, 3), color_hex(hover ? T_HOVER : T_BG));
    if (value) {
        /* check mark: two strokes */
        Color c = color_hex(enabled ? 0xFFFFFF : T_TEXT_DIM);
        draw_line(r, box.x + s * 0.25f, box.y + s * 0.52f, box.x + s * 0.44f, box.y + s * 0.72f, px(a, 2), c);
        draw_line(r, box.x + s * 0.44f, box.y + s * 0.72f, box.x + s * 0.78f, box.y + s * 0.3f, px(a, 2), c);
    }
    draw_text_fit(a->font_ui, r, label, -1, rect_make(box.x + s + px(a, 8), rc.y, rc.w - s - px(a, 8), rc.h), color_hex(enabled ? T_TEXT : T_TEXT_DIM), ALIGN_LEFT);
    return clicked;
}

/* Static text in rc, aligned with ALIGN_* and ellipsised; `color` is 0xRRGGBB. */
void ui_label(App *a, Rect rc, const char *text, int align, uint32_t color) {
    draw_text_fit(a->font_ui, a->renderer, text, -1, rc, color_hex(color), align);
}

/* ---- popup menu (with cascading submenus; see ui.h) ---------------------- */

/* Popup layout, in UI points: caption row, item row, separator gap, inner padding. */
#define POPUP_CAPTION_H 20.0f
#define POPUP_ITEM_H 26.0f
#define POPUP_SEP_H 9.0f
#define POPUP_PAD 6.0f
#define POPUP_SWITCH_MS 220        /* hover time before another parent item replaces a submenu */

/* "›" (U+203A) in UTF-8: a label ending with it marks an item that opens a submenu. */
static const char CHEVRON[] = "\xE2\x80\xBA";

/* Closes m and, recursively, every submenu below it; unlinks it from its parent. */
void popup_close(PopupMenu *m) {
    if (!m) return;
    if (m->child) popup_close(m->child);
    m->child = NULL;
    m->open = false;
    if (m->parent && m->parent->child == m) m->parent->child = NULL;
}

/* Resets a menu to an empty, open state (shared by top-level menus and submenus). */
static void popup_reset(PopupMenu *m) {
    m->open = true; m->n = 0; m->kb = -1;
    m->child = NULL; m->pending_sub = -1; m->pending_by_kb = false;
    m->hover_item = -1; m->hover_since = 0; m->hold_item = -1;
}

/* Open m as an empty top-level menu anchored at (x, y) render pixels (top-left; it is shifted to
 * stay inside the window). Fill it with popup_add afterwards. */
void popup_open(PopupMenu *m, float x, float y) {
    if (m->child) popup_close(m->child);   /* a menu reopened in place drops its old submenus */
    popup_reset(m);
    m->parent = NULL; m->parent_item = -1;
    m->x = x; m->y = y;
}

PopupMenu *popup_open_sub(PopupMenu *parent, PopupMenu *child) {
    if (parent->child && parent->child != child) popup_close(parent->child);
    popup_reset(child);
    child->parent = parent;
    child->parent_item = parent->pending_sub >= 0 ? parent->pending_sub : parent->kb;
    parent->child = child;
    child->pending_by_kb = parent->pending_by_kb;   /* first item highlighted once it is filled */
    parent->pending_sub = -1;
    return child;
}

/* Append an item. `label` is not copied: it must stay valid while the menu is open. A label
 * ending in CHEVRON makes the item a submenu opener. When the menu was opened from the keyboard,
 * the first enabled item gets the keyboard highlight. Ignored past POPUP_MAX items. */
void popup_add(PopupMenu *m, int id, const char *label, bool sep_before, bool enabled) {
    if (m->n >= POPUP_MAX) return;
    size_t ll = strlen(label);
    m->labels[m->n] = label; m->ids[m->n] = id; m->sep_before[m->n] = sep_before; m->enabled[m->n] = enabled;
    m->caption[m->n] = false; m->color[m->n] = 0;
    m->is_sub[m->n] = ll > 3 && memcmp(label + ll - 3, CHEVRON, 3) == 0;
    m->n++;
    if (m->pending_by_kb && m->kb < 0 && enabled) m->kb = m->n - 1;
}

/* Append a non-interactive section caption drawn in the small font; color 0 = dim text. */
void popup_add_caption(PopupMenu *m, const char *label, uint32_t color, bool sep_before) {
    if (m->n >= POPUP_MAX) return;
    m->labels[m->n] = label; m->ids[m->n] = -1; m->sep_before[m->n] = sep_before; m->enabled[m->n] = false;
    m->caption[m->n] = true; m->is_sub[m->n] = false; m->color[m->n] = color;
    m->n++;
}

/* Width and height the menu's content needs. */
static void popup_size(App *a, PopupMenu *m, float *w, float *h) {
    *w = px(a, 200); *h = px(a, POPUP_PAD) * 2;
    for (int i = 0; i < m->n; i++) {
        *h += (m->caption[i] ? px(a, POPUP_CAPTION_H) : px(a, POPUP_ITEM_H)) + (m->sep_before[i] ? px(a, POPUP_SEP_H) : 0);
        float need = font_width(m->caption[i] ? a->font_small : a->font_ui, m->labels[i], -1) + px(a, 36);
        if (need > *w) *w = need;
    }
}

static Rect popup_rect(App *a, PopupMenu *m);

/* Rect of item i inside a menu whose outer rect is rc. */
static Rect popup_item_box(App *a, PopupMenu *m, Rect rc, int i) {
    float y = rc.y + px(a, POPUP_PAD);
    for (int k = 0; k < i; k++) y += (m->sep_before[k] ? px(a, POPUP_SEP_H) : 0) + (m->caption[k] ? px(a, POPUP_CAPTION_H) : px(a, POPUP_ITEM_H));
    if (m->sep_before[i]) y += px(a, POPUP_SEP_H);
    return rect_make(rc.x + px(a, POPUP_PAD), y, rc.w - px(a, POPUP_PAD * 2), m->caption[i] ? px(a, POPUP_CAPTION_H) : px(a, POPUP_ITEM_H));
}

/* Outer rect on screen. A top-level menu opens at its anchor; a submenu opens beside its
 * parent item, on the side that has room; both are kept inside the window. */
static Rect popup_rect(App *a, PopupMenu *m) {
    float w, h, margin = px(a, 8);
    popup_size(a, m, &w, &h);
    float x = m->x, y = m->y;
    if (m->parent) {
        Rect pr = popup_rect(a, m->parent);
        Rect it = popup_item_box(a, m->parent, pr, m->parent_item >= 0 ? m->parent_item : 0);
        float overlap = px(a, 2);
        x = pr.x + pr.w - overlap;                                    /* right of the parent */
        if (x + w > a->width - margin) x = pr.x - w + overlap;       /* no room: to the left */
        if (x < margin) x = margin;
        y = it.y - px(a, POPUP_PAD);                                  /* first item level with the parent item */
    }
    if (x + w > a->width - margin) x = a->width - margin - w;
    if (y + h > a->height - margin) y = a->height - margin - h;
    if (y < margin) y = margin;
    return rect_make(x, y, w, h);
}

/* Interactive item under the point in this one menu, or -1. */
static int popup_item_at(App *a, PopupMenu *m, float mx, float my) {
    Rect rc = popup_rect(a, m);
    if (!rect_contains(rc, mx, my)) return -1;
    for (int i = 0; i < m->n; i++)
        if (!m->caption[i] && rect_contains(popup_item_box(a, m, rc, i), mx, my)) return i;
    return -1;
}

/* Deepest open level under the point (submenus are drawn on top), or NULL. */
static PopupMenu *popup_hit(App *a, PopupMenu *root, float mx, float my) {
    PopupMenu *chain[8]; int n = 0;
    for (PopupMenu *p = root; p && p->open && n < 8; p = p->child) chain[n++] = p;
    for (int i = n - 1; i >= 0; i--) if (rect_contains(popup_rect(a, chain[i]), mx, my)) return chain[i];
    return NULL;
}

/* Next selectable (enabled, non-caption) item after `from` in direction dir (+1 / -1), wrapping
 * around; from < 0 starts at the first / last item. Returns -1 when nothing is selectable. */
int popup_step(PopupMenu *m, int from, int dir) {
    for (int k = 0; k < m->n; k++) {
        from = from < 0 ? (dir > 0 ? 0 : m->n - 1) : (from + dir + m->n) % m->n;
        if (!m->caption[from] && m->enabled[from]) return from;
    }
    return -1;
}

/* Harness lookup: rect (render pixels) of the first non-caption item whose label starts with
 * `prefix` (case-insensitive), trying the deepest open submenu first. False if none / closed. */
bool popup_item_rect(App *a, PopupMenu *m, const char *prefix, Rect *out) {
    if (!m->open) return false;
    if (m->child && m->child->open && popup_item_rect(a, m->child, prefix, out)) return true;
    Rect rc = popup_rect(a, m);
    size_t n = strlen(prefix);
    for (int i = 0; i < m->n; i++)
        if (!m->caption[i] && strncasecmp(m->labels[i], prefix, n) == 0) { *out = popup_item_box(a, m, rc, i); return true; }
    return false;
}

/* Asks the owner to open the submenu of item i of m: returns the id to hand back. */
static int request_sub(PopupMenu *m, int i, bool by_kb) {
    if (m->child && m->child->open && m->child->parent_item == i) return -1;   /* already open */
    if (m->child) popup_close(m->child);
    m->pending_sub = i;
    m->pending_by_kb = by_kb;
    return m->ids[i];
}

/* Per-frame input for the whole cascade; call on the top-level menu before drawing. Returns the
 * chosen item's id (and closes everything), a submenu opener's id when the owner should call
 * popup_open_sub, or -1. While open it swallows every key and text event of the frame and every
 * mouse press, so nothing underneath reacts. */
int popup_update(App *a, PopupMenu *m) {
    Input *in = &a->in;
    if (!m->open) return -1;
    int chosen = -1;
    PopupMenu *leaf = m;                                   /* keyboard goes to the deepest level */
    while (leaf->child && leaf->child->open) leaf = leaf->child;

    /* keyboard (US-054): arrows move, Right / Enter open a submenu, Left / Esc close one */
    /* closing a submenu from the keyboard must stick even though the pointer still rests on
     * its parent item: hover re-opening is held until the mouse moves */
    PopupMenu *closed_parent = NULL; int closed_item = -1;
    if (input_key(in, SDLK_ESCAPE, 0)) {
        if (leaf->parent) { closed_parent = leaf->parent; closed_item = leaf->parent_item; popup_close(leaf); }
        else popup_close(m);
    }
    else if (input_key(in, SDLK_LEFT, 0) && leaf->parent) { closed_parent = leaf->parent; closed_item = leaf->parent_item; popup_close(leaf); }
    if (closed_parent) { closed_parent->hold_item = closed_item; closed_parent->hold_x = in->mouse_x; closed_parent->hold_y = in->mouse_y; closed_parent->kb = closed_item; }
    else {
        if (input_key(in, SDLK_DOWN, 0)) leaf->kb = popup_step(leaf, leaf->kb, 1);
        if (input_key(in, SDLK_UP, 0)) leaf->kb = popup_step(leaf, leaf->kb, -1);
        if (input_key(in, SDLK_HOME, 0)) leaf->kb = popup_step(leaf, -1, 1);
        if (input_key(in, SDLK_END, 0)) leaf->kb = popup_step(leaf, -1, -1);
        bool right = input_key(in, SDLK_RIGHT, 0);
        bool enter = input_key(in, SDLK_RETURN, 0) || input_key(in, SDLK_KP_ENTER, 0) || input_key(in, SDLK_SPACE, 0);
        if ((right || enter) && leaf->kb >= 0 && leaf->enabled[leaf->kb]) {
            if (leaf->is_sub[leaf->kb]) chosen = request_sub(leaf, leaf->kb, true);
            else if (enter) { chosen = leaf->ids[leaf->kb]; popup_close(m); }
        }
    }

    /* pointer: the deepest level under it wins */
    PopupMenu *hm = m->open ? popup_hit(a, m, in->mouse_x, in->mouse_y) : NULL;
    int hover = hm ? popup_item_at(a, hm, in->mouse_x, in->mouse_y) : -1;
    if (hm && hover >= 0) {
        hm->kb = -1;
        if (hm->enabled[hover]) a->want_cursor = CURSOR_HAND;
        Uint64 now = SDL_GetTicks();
        if (hm->hover_item != hover) { hm->hover_item = hover; hm->hover_since = now; }
        bool child_open = hm->child && hm->child->open;
        bool settled = now - hm->hover_since >= POPUP_SWITCH_MS;
        if (hm->hold_item >= 0 && (in->mouse_x != hm->hold_x || in->mouse_y != hm->hold_y)) hm->hold_item = -1;
        if (hm->is_sub[hover] && hm->enabled[hover] && hm->hold_item == hover) {
            /* held after a keyboard close: stays shut until the pointer moves */
        } else if (hm->is_sub[hover] && hm->enabled[hover]) {
            /* open at once when nothing is open; switch after the delay */
            if ((!child_open || settled) && chosen < 0) chosen = request_sub(hm, hover, false);
            else if (child_open && hm->child->parent_item != hover) app_wake_in(a, POPUP_SWITCH_MS);
        } else if (child_open) {
            if (settled) popup_close(hm->child);           /* resting on another item closes the submenu */
            else app_wake_in(a, POPUP_SWITCH_MS);
        }
    }
    if (in->mouse_pressed || in->mouse_rpressed) {
        in->mouse_consumed = true;
        if (!hm) popup_close(m);                           /* outside every level: close all */
        else if (hover >= 0 && in->mouse_pressed && hm->enabled[hover]) {
            if (hm->is_sub[hover]) { int id = request_sub(hm, hover, false); if (id >= 0) chosen = id; }
            else { chosen = hm->ids[hover]; popup_close(m); }
        }
    }
    in->nkeys = 0;
    in->ntext = 0;
    return chosen;
}

/* Draws one level, then its open submenu on top. */
void popup_draw(App *a, PopupMenu *m) {
    if (!m->open) return;
    SDL_Renderer *r = a->renderer;
    Rect rc = popup_rect(a, m);
    for (int s = 3; s >= 1; s--)
        draw_rrect(r, rect_make(rc.x - s * px(a, 2), rc.y + px(a, 2), rc.w + s * px(a, 4), rc.h + s * px(a, 3)), px(a, 8) + s * px(a, 2), color_alpha(color_hex(0x000000), 28));
    draw_rrect(r, rc, px(a, 8), color_hex(T_BORDER));
    draw_rrect(r, rect_inset(rc, px(a, 1), px(a, 1)), px(a, 7), color_hex(T_PANEL_ALT));
    PopupMenu *hm = popup_hit(a, m->parent ? m->parent : m, a->in.mouse_x, a->in.mouse_y);
    int hover = hm == m ? popup_item_at(a, m, a->in.mouse_x, a->in.mouse_y) : -1;
    int open_item = m->child && m->child->open ? m->child->parent_item : -1;
    for (int i = 0; i < m->n; i++) {
        Rect ir = popup_item_box(a, m, rc, i);
        if (m->sep_before[i]) draw_rect(r, rect_make(rc.x + px(a, 10), ir.y - px(a, POPUP_SEP_H) / 2, rc.w - px(a, 20), px(a, 1)), color_hex(T_BORDER));
        if (m->caption[i]) {
            Rect cr = rect_make(rc.x + px(a, 16), ir.y + px(a, 4), rc.w - px(a, 32), ir.h - px(a, 4));
            draw_text_fit(a->font_small, r, m->labels[i], -1, cr, color_hex(m->color[i] ? m->color[i] : T_TEXT_DIM), ALIGN_LEFT);
            continue;
        }
        /* highlighted: under the pointer, chosen from the keyboard, or the item whose submenu is open */
        bool hl = m->enabled[i] && (i == hover || (hover < 0 && i == m->kb) || i == open_item);
        if (hl) draw_rrect(r, ir, px(a, M_RADIUS), color_hex(i == open_item && i != hover ? T_SELECTED : T_ACCENT));
        Color tc = color_hex(!m->enabled[i] ? T_TEXT_DIM : hl ? 0xFFFFFF : T_TEXT);
        if (m->is_sub[i]) {   /* "Label  ›": text on the left, chevron at the right edge */
            int tl = (int)strlen(m->labels[i]) - 3; while (tl > 0 && m->labels[i][tl - 1] == ' ') tl--;
            draw_text_fit(a->font_ui, r, m->labels[i], tl, rect_inset(ir, px(a, 10), 0), tc, ALIGN_LEFT);
            draw_text_fit(a->font_ui, r, CHEVRON, -1, rect_make(ir.x + ir.w - px(a, 22), ir.y, px(a, 12), ir.h), tc, ALIGN_CENTER);
        } else draw_text_fit(a->font_ui, r, m->labels[i], -1, rect_inset(ir, px(a, 10), 0), tc, ALIGN_LEFT);
    }
    if (m->child && m->child->open) popup_draw(a, m->child);
}

/* ---- modern overlay scrollbar (vertical or horizontal) ---- */
static double sb_now(void) { return (double)SDL_GetTicks() / 1000.0; }
void ui_scrollbar_touch(ScrollBar *sb) { sb->active_at = sb_now(); }
/* the axis-independent geometry: along = the scrolled axis */
typedef struct { float pos, len, cross, cross_len; } SbAxis;
static SbAxis sb_axis(Rect v, bool h) { SbAxis x; if (h) { x.pos = v.x; x.len = v.w; x.cross = v.y; x.cross_len = v.h; } else { x.pos = v.y; x.len = v.h; x.cross = v.x; x.cross_len = v.w; } return x; }
static Rect sb_hit(App *a, Rect view, bool h) { return h ? rect_make(view.x, view.y + view.h - px(a, 14), view.w, px(a, 14)) : rect_make(view.x + view.w - px(a, 14), view.y, px(a, 14), view.h); }
static void sb_thumb(App *a, SbAxis ax, float content, float scroll, float *t0, float *tl) {
    float maxs = content - ax.len; if (maxs < 1) maxs = 1;
    *tl = fmaxf(px(a, 28), ax.len * ax.len / content); if (*tl > ax.len) *tl = ax.len;
    *t0 = ax.pos + (ax.len - *tl) * (scroll / maxs);
}
static bool sb_input(App *a, ScrollBar *sb, Rect view, float content, float *scroll, bool h) {
    SbAxis ax = sb_axis(view, h);
    float maxs = content - ax.len;
    if (maxs <= 0) { sb->drag = false; return false; }
    Input *in = &a->in;
    bool over = rect_contains(sb_hit(a, view, h), in->mouse_x, in->mouse_y);
    float m = h ? in->mouse_x : in->mouse_y;
    float t0, tl; sb_thumb(a, ax, content, *scroll, &t0, &tl);
    if (over && in->mouse_pressed && !in->mouse_consumed) {
        in->mouse_consumed = true;
        sb->grab = (m >= t0 && m <= t0 + tl) ? m - t0 : tl / 2;   /* grab where pressed, or jump under the pointer */
        sb->drag = true;
    }
    if (sb->drag) {
        if (!in->mouse_down) sb->drag = false;
        else { float t = (m - sb->grab - ax.pos) / fmaxf(1, ax.len - tl); if (t < 0) t = 0; if (t > 1) t = 1; *scroll = t * maxs; sb->active_at = sb_now(); }
    }
    if (over) sb->active_at = sb_now();
    return sb->drag || over;
}
static void sb_draw(App *a, ScrollBar *sb, Rect view, float content, float scroll, bool h) {
    SbAxis ax = sb_axis(view, h);
    if (content - ax.len <= 0) return;
    if (fabsf(scroll - sb->last) > 0.5f) { sb->last = scroll; sb->active_at = sb_now(); }   /* scrolled by any means: show it */
    Input *in = &a->in;
    bool over = rect_contains(sb_hit(a, view, h), in->mouse_x, in->mouse_y) || sb->drag;
    float target = over ? 1.0f : 0.0f;
    sb->grow += (target - sb->grow) * 0.35f; if (fabsf(target - sb->grow) < 0.02f) sb->grow = target;
    double idle = sb_now() - sb->active_at;
    float alpha = over ? 1.0f : idle < 0.9 ? 1.0f : idle < 1.3 ? (float)(1.0 - (idle - 0.9) / 0.4) : 0.0f;
    if (alpha <= 0) return;
    if (alpha < 1 || sb->grow != target) app_wake_in(a, 16);
    else if (!over) { int ms = (int)((0.95 - idle) * 1000); app_wake_in(a, ms > 16 ? ms : 16); }
    float th = px(a, 4) + px(a, 5) * sb->grow;   /* thickness */
    float t0, tl; sb_thumb(a, ax, content, scroll, &t0, &tl);
    float c0 = ax.cross + ax.cross_len - th - px(a, 3);
    Rect track = h ? rect_make(ax.pos + px(a, 2), c0, ax.len - px(a, 4), th) : rect_make(c0, ax.pos + px(a, 2), th, ax.len - px(a, 4));
    Rect thumb = h ? rect_make(t0 + px(a, 2), c0, tl - px(a, 4), th) : rect_make(c0, t0 + px(a, 2), th, tl - px(a, 4));
    if (sb->grow > 0.05f) { Color tc = color_hex(T_TEXT); tc.a = (uint8_t)(18 * sb->grow * alpha); draw_rrect(a->renderer, track, th / 2, tc); }
    Color c = color_hex(sb->drag ? T_TEXT_MUTED : T_SCROLLBAR);
    c.a = (uint8_t)(c.a * alpha * (0.75f + 0.25f * sb->grow));
    draw_rrect(a->renderer, thumb, th / 2, c);
}
bool ui_scrollbar_input(App *a, ScrollBar *sb, Rect view, float content_h, float *scroll) { return sb_input(a, sb, view, content_h, scroll, false); }
void ui_scrollbar_draw(App *a, ScrollBar *sb, Rect view, float content_h, float scroll) { sb_draw(a, sb, view, content_h, scroll, false); }
bool ui_hscrollbar_input(App *a, ScrollBar *sb, Rect view, float content_w, float *scroll) { return sb_input(a, sb, view, content_w, scroll, true); }
void ui_hscrollbar_draw(App *a, ScrollBar *sb, Rect view, float content_w, float scroll) { sb_draw(a, sb, view, content_w, scroll, true); }
/* lists that scroll by rows: the same bar, in rows */
bool ui_scrollbar_input_rows(App *a, ScrollBar *sb, Rect view, float rows, float row_h, float *scroll_rows) {
    if (row_h <= 0) return false;
    float s = *scroll_rows * row_h; bool r = sb_input(a, sb, view, rows * row_h, &s, false);
    if (sb->drag) *scroll_rows = s / row_h;
    return r;
}
void ui_scrollbar_draw_rows(App *a, ScrollBar *sb, Rect view, float rows, float row_h, float scroll_rows) { if (row_h > 0) sb_draw(a, sb, view, rows * row_h, scroll_rows * row_h, false); }
