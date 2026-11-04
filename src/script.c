/* SPDX-License-Identifier: AGPL-3.0-only */
/* script.c: the UI test harness. When LUME_SCRIPT=<file> is set, main.c calls script_load()
 * once at start-up and script_tick() every frame; each tick runs at most one command once its
 * scheduled time has come, then asks the app to wake up for the next one.
 *
 * Commands drive the app as a user would: most of them synthesise SDL events (mouse motion /
 * buttons / wheel, key down, text input) and push them onto the SDL event queue, so they go
 * through exactly the same code path as real input. A few call into modules directly
 * (layout import / export, Output fill, results export) or print machine-readable lines on
 * stdout for the test runner to check (stat, fieldstate, checkfloat, "target not found").
 *
 * Coordinates given as numbers are in window units (SDL event coordinates), not render pixels.
 * Semantic targets avoid hard-coded coordinates: "button Label" finds a widget recorded with
 * ui_record() during the last frame, "cell R C" / "head C" / "menu Label" ask the results grid,
 * Output panel or panels shell, "node Label" asks the Explorer. A leading r / d on those
 * (rbutton, dcell, ...) means right-click / double-click.
 *
 * Script syntax: one command per line, leading spaces ignored, '#' starts a comment line, and a
 * trailing " # comment" is stripped from every command except `type`.
 *
 * State is file-static (one script per process). Lines are heap strings that are never freed;
 * multi-step commands (drag, dragto) insert synthetic follow-up lines after the current one. */
#include "script.h"
#include "results.h"
#include "output.h"
#include "panels.h"
#include "objed.h"
#include "tdes.h"
#include "dialog.h"
#include "telemetry.h"
#include "update.h"
#include "dgmview.h"
#include "log.h"
#include "planview.h"
#include "rptview.h"
#include "settings.h"
#include "objcat.h"
#include "gitview.h"
#include "about.h"
#include "macos_app.h"
#include "editor.h"
#include "conn.h"
#include "sshtun.h"
#include "db.h"
#include "explorer.h"
#include "ui.h"
#include "toolbar.h"
#include "commands.h"
#include "sysinfo.h"
#include <ctype.h>

/* lines[0..nlines): the script's commands; cur: index of the next one to run.
 * next_at: SDL tick (ms) before which the next command must not run.
 * held_text: buffer of the last `type` command, kept alive because the pushed
 * SDL_EVENT_TEXT_INPUT points into it until the event is processed. */
static char **lines; static int nlines, cur;
static Uint64 next_at;
static char *held_text;

/* Read the script file, keep its non-empty, non-comment lines, and schedule the first command
 * 400 ms after start-up (gives the window time to appear). Logs and does nothing if unreadable. */
void script_load(App *a, const char *path) {
    size_t len;
    char *data = SDL_LoadFile(path, &len);
    if (!data) { SDL_Log("script: cannot read %s", path); return; }
    char *p = data;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        while (*p == ' ') p++;
        if (*p && *p != '#') { lines = realloc(lines, sizeof *lines * (nlines + 1)); lines[nlines++] = SDL_strdup(p); }
        p = nl ? nl + 1 : NULL;
    }
    SDL_free(data);
    /* test runs stay in the background and never take the focus from the user (main.c sets the
     * hints); LUME_FOREGROUND=1 raises the window to watch a script (tests/watch.sh also activates
     * it: macOS does not let a process started from a terminal take the focus by itself) */
    if (SDL_getenv("LUME_FOREGROUND")) SDL_RaiseWindow(a->window);
    next_at = SDL_GetTicks() + 400;
}

/* Push a synthetic mouse event at (x, y) window units: motion, or button down / up when `type`
 * is a button event. clicks < 0 means a right-button click (counted as 1); otherwise it is the
 * left button with that click count (2 = double click). */
static void push_mouse(App *a, Uint32 type, float x, float y, int clicks, bool down) {
    SDL_Event e; SDL_zero(e);
    e.type = type;
    e.common.timestamp = SDL_GetTicksNS();
    if (type == SDL_EVENT_MOUSE_MOTION) { e.motion.windowID = SDL_GetWindowID(a->window); e.motion.x = x; e.motion.y = y; }
    else { e.button.windowID = SDL_GetWindowID(a->window); e.button.button = clicks < 0 ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT; e.button.down = down; e.button.clicks = (Uint8)(clicks < 0 ? 1 : clicks); e.button.x = x; e.button.y = y; }
    SDL_PushEvent(&e);
}

/* "cmd" in a key spec is the platform's primary modifier (KM_PRIMARY): Cmd on macOS, Ctrl elsewhere,
 * so the same script drives the same shortcut on every OS. "gui" is always the GUI key. */
#ifdef __APPLE__
#define SCRIPT_KMOD_PRIMARY SDL_KMOD_GUI
#else
#define SCRIPT_KMOD_PRIMARY SDL_KMOD_CTRL
#endif

/* Push a synthetic key-down for a spec such as "cmd+shift+Return": modifiers (cmd = primary, gui,
 * ctrl, shift, alt) joined with '+', then an SDL key name. Logs unknown key names but still pushes. */
static void push_key(App *a, const char *spec) {
    SDL_Keymod mod = 0;
    char name[64] = "";
    const char *p = spec;
    for (;;) {
        const char *plus = strchr(p, '+');
        if (!plus) { snprintf(name, sizeof name, "%s", p); break; }
        size_t n = plus - p;
        if (!strncmp(p, "cmd", n)) mod |= SCRIPT_KMOD_PRIMARY;
        else if (!strncmp(p, "gui", n)) mod |= SDL_KMOD_GUI;
        else if (!strncmp(p, "ctrl", n)) mod |= SDL_KMOD_CTRL;
        else if (!strncmp(p, "shift", n)) mod |= SDL_KMOD_SHIFT;
        else if (!strncmp(p, "alt", n)) mod |= SDL_KMOD_ALT;
        p = plus + 1;
    }
    SDL_Event e; SDL_zero(e);
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.windowID = SDL_GetWindowID(a->window);
    e.key.key = SDL_GetKeyFromName(name);
    e.key.mod = mod;
    e.key.down = true;
    if (e.key.key == SDLK_UNKNOWN) SDL_Log("script: unknown key '%s'", name);
    SDL_PushEvent(&e);
}

/* Run the next script command if its time has come (called once per frame from main.c), then
 * schedule the wake-up for the following one. Commands normally settle 120 ms apart; wait,
 * shot, resize and a few others change that delay. */
void script_tick(App *a) {
    /* not yet time: re-arm the wake-up and return; otherwise take the line and split off the command */
    if (cur >= nlines) return;
    Uint64 now = SDL_GetTicks();
    if (now < next_at) { app_wake_in(a, (Uint32)(next_at - now)); return; }
    char *line = lines[cur++];
    char cmd[16] = ""; float x = 0, y = 0, x1 = 0, y1 = 0;
    sscanf(line, "%15s", cmd);
    const char *arg = line + strlen(cmd);
    while (*arg == ' ') arg++;
    /* trailing "  # comment" on any command except type (which keeps its text verbatim) */
    if (strcmp(cmd, "type") != 0) {
        char *hash = strstr((char *)arg, " #");
        if (hash) { *hash = 0; while (hash > arg && hash[-1] == ' ') *--hash = 0; }
    }
    next_at = now + 120;    /* small settle time between commands */
    { /* LUME_WATCH=<ms>: a person is watching the run; pause that long between steps */
        static int watch = -1;
        if (watch < 0) { const char *w = SDL_getenv("LUME_WATCH"); watch = w ? atoi(w) : 0; if (watch < 0) watch = 0; }
        next_at += (Uint64)watch;
    }

    /* semantic targets (stable across layout changes): cell / dcell / rcell ROW COL,
     * head / rhead COL, menu "Label prefix" */
    if (!strcmp(cmd, "cell") || !strcmp(cmd, "dcell") || !strcmp(cmd, "rcell") || !strcmp(cmd, "head") || !strcmp(cmd, "rhead") || !strcmp(cmd, "menu")) {
        bool ok;
        if (cmd[0] == 'm') {
            const char *lbl = arg; size_t hl = strlen(lbl); char tmp[160];
            if (hl >= 2 && lbl[0] == '"') { snprintf(tmp, sizeof tmp, "%.*s", (int)(hl - 2), lbl + 1); lbl = tmp; }
            ok = results_menu_pos(a->results, a, lbl, &x, &y) || output_menu_pos(a->output, a, lbl, &x, &y) || (a->panels && panels_menu_pos(a->panels, a, lbl, &x, &y)) || ui_textfield_menu_pos(a, lbl, &x, &y) || toolbar_menu_pos(a->toolbar, a, lbl, &x, &y) || objed_menu_pos(a, lbl, &x, &y) || tdes_menu_pos(a, lbl, &x, &y);   /* grid, Output, the shell's menus, a text field's, the toolbar's, the object editor's */
        } else if (strstr(cmd, "head")) { int c = atoi(arg); ok = results_head_pos(a->results, a, c, &x, &y); }
        else { int rr = 0, c = 0; sscanf(arg, "%d %d", &rr, &c); ok = results_cell_pos(a->results, a, rr, c, &x, &y); }
        if (!ok) { SDL_Log("script: target not found: %s %s", cmd, arg); printf("script: target not found: %s %s\n", cmd, arg); }
        else {
            push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, false);
            if (cmd[0] == 'r') { push_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, -1, true); push_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, -1, false); }
            else { int n = cmd[0] == 'd' ? 2 : 1; for (int c = 1; c <= n; c++) { push_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, c, true); push_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, c, false); } }
        }
    }
    /* button / rbutton "Label prefix": a recorded widget (ui_record); node / dnode / rnode "Label": an
     * Explorer node; dragcells R0 C0 R1 C1: drag-select grid cells. Stable across layout changes. */
    else if (!strcmp(cmd, "button") || !strcmp(cmd, "rbutton") || !strcmp(cmd, "dbutton") || !strcmp(cmd, "node") || !strcmp(cmd, "dnode") || !strcmp(cmd, "rnode") || !strcmp(cmd, "xnode")) {
        /* node / dnode / rnode "label": the first visible row with that label, "parent/label" one under
         * a row labelled parent; xnode "label": its chevron (opens a table, a package) */
        const char *lbl = arg; size_t hl = strlen(lbl); char tmp[160];
        if (hl >= 2 && lbl[0] == '"') { snprintf(tmp, sizeof tmp, "%.*s", (int)(hl - 2), lbl + 1); lbl = tmp; }
        bool ok;
        if (strstr(cmd, "node")) ok = explorer_node_pos_ex(a->explorer, a, lbl, cmd[0] == 'x', &x, &y);
        else { Rect rc; ok = ui_target(lbl, &rc); x = (rc.x + rc.w / 2) / a->density; y = (rc.y + rc.h / 2) / a->density; }
        if (!ok) { SDL_Log("script: target not found: %s %s", cmd, arg); printf("script: target not found: %s %s\n", cmd, arg); }
        else {
            push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, false);
            if (cmd[0] == 'r') { push_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, -1, true); push_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, -1, false); }
            else { int n = cmd[0] == 'd' ? 2 : 1; for (int c = 1; c <= n; c++) { push_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, c, true); push_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, c, false); } }
        }
    } else if (!strcmp(cmd, "dragcells") || !strcmp(cmd, "dragto")) {
        /* dragcells R0 C0 R1 C1 | dragto "from target" "to target" */
        bool ok = false;
        if (cmd[4] == 'c') { int r0, c0, r1, c1; ok = sscanf(arg, "%d %d %d %d", &r0, &c0, &r1, &c1) == 4 && results_cell_pos(a->results, a, r0, c0, &x, &y) && results_cell_pos(a->results, a, r1, c1, &x1, &y1); }
        else {
            char f[96] = "", t[96] = ""; Rect ra, rb;
            if (sscanf(arg, "\"%95[^\"]\" \"%95[^\"]\"", f, t) == 2 && ui_target(f, &ra)) {
                x = (ra.x + ra.w / 2) / a->density; y = (ra.y + ra.h / 2) / a->density;
                if (ui_target(t, &rb)) { x1 = (rb.x + rb.w / 2) / a->density; y1 = (rb.y + rb.h / 2) / a->density; ok = true; }
                else if ((t[0] == '+' || t[0] == '-') && sscanf(t, "%f %f", &x1, &y1) == 2) { x1 += x; y1 += y; ok = true; }   /* "+dx dy": relative to the start */
                else if (sscanf(t, "%f %f", &x1, &y1) == 2) ok = true;
            }
        }
        if (!ok) { printf("script: target not found: %s %s\n", cmd, arg); }
        else {
            push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, false);
            push_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, 1, true);
            char *mid = malloc(64), *tail = malloc(64);
            snprintf(mid, 64, "move %g %g", (x + x1) / 2, (y + y1) / 2);
            snprintf(tail, 64, "_dragend %g %g", x1, y1);
            lines = realloc(lines, sizeof *lines * (nlines + 2));
            memmove(&lines[cur + 2], &lines[cur], sizeof *lines * (nlines - cur));
            lines[cur] = mid; lines[cur + 1] = tail; nlines += 2;
        }
    }
    /* hold "target" | hold X Y: press and keep the left button down (a drag in steps, with shots
     * in between); release X Y: move there and let go */
    else if (!strcmp(cmd, "hold")) {
        Rect rc; char t[96] = "";
        if (sscanf(arg, "\"%95[^\"]\"", t) == 1 && ui_target(t, &rc)) { x = (rc.x + rc.w / 2) / a->density; y = (rc.y + rc.h / 2) / a->density; }
        else if (sscanf(arg, "%f %f", &x, &y) != 2) { printf("script: target not found: hold %s\n", arg); x = y = -1; }
        if (x >= 0) { push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, false); push_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, 1, true); }
    } else if (!strcmp(cmd, "release")) {
        sscanf(arg, "%f %f", &x, &y);
        push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, false);
        push_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, 1, false);
    }
    /* exportlayout / importlayout PATH: the code behind the native file dialogs */
    else if (!strcmp(cmd, "exportlayout")) panels_export_layout(a->panels, a, arg);
    else if (!strcmp(cmd, "importlayout")) panels_import_layout(a->panels, a, arg);
    /* panel windows: shotfloat PATH; movefloat X Y (e.g. off every display, as if a monitor was
     * unplugged); checkfloat: prints whether the window is back on a display */
    else if (!strcmp(cmd, "shotfloat")) { panels_shot_float(a->panels, arg); next_at = now + 200; }
    else if (!strcmp(cmd, "movefloat")) { int fx = 0, fy = 0; sscanf(arg, "%d %d", &fx, &fy); panels_move_float(a->panels, fx, fy); }
    else if (!strcmp(cmd, "displaychanged")) panels_revalidate_floats(a->panels);
    else if (!strcmp(cmd, "closefloat")) {   /* the OS close button of the first panel window */
        SDL_Event ev; SDL_zero(ev); ev.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED; ev.window.windowID = panels_float_window_id(a->panels);
        if (ev.window.windowID) SDL_PushEvent(&ev); else printf("script: target not found: closefloat\n");
    }
    else if (!strcmp(cmd, "checkfloat")) { int fx = 0, fy = 0; bool any = panels_float_pos(a->panels, &fx, &fy); printf("floatpos %s %d %d\n", any ? "open" : "none", fx, fy); fflush(stdout); }
    else if (!strcmp(cmd, "dialogstate")) dialog_dump(a->dialog);   /* the connection dialog */
    else if (!strcmp(cmd, "fieldstate")) {   /* content and selection of the focused text field */
        const TextField *t = ui_focused_field();
        if (t) printf("field \"%s\" sel %d %d\n", t->buf, t->anchor < t->cursor ? t->anchor : t->cursor, t->anchor < t->cursor ? t->cursor : t->anchor);
        else printf("field none\n");
        fflush(stdout);
    }
    /* raw-coordinate commands (window units): wait MS, move X Y, click / dblclick / tripleclick X Y */
    else if (!strcmp(cmd, "rawsql")) {
        /* rawsql "connection name" SQL: runs SQL on a separate session of that connection (another
         * user changing the database while Lume edits it, AT-04). The location holds the
         * password: it is never printed. */
        char cn[64] = ""; const char *sql = NULL;
        if (sscanf(arg, "\"%63[^\"]\"", cn) == 1) { sql = strchr(arg + 1, '"'); if (sql) { sql++; while (*sql == ' ') sql++; } }
        Connection *c = NULL; for (int i = 0; a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->name, cn)) c = a->conns->items[i];
        const DriverInfo *di = c ? conn_driver(c) : NULL;
        if (!di || !sql) { printf("script: rawsql failed: unknown connection\n"); fflush(stdout); }
        else {
            char err[256] = ""; struct SshTunnel *tun = NULL;
            Db *db = conn_open(c, &tun, err, sizeof err);
            if (!db) { printf("script: rawsql failed: %s\n", err); fflush(stdout); }
            else { ResultSet rs; rs_init(&rs); db->drv->exec(db, sql, (int)strlen(sql), 0, &rs); printf("rawsql %s%s\n", rs.ok ? "ok" : "error: ", rs.ok ? "" : rs.error); fflush(stdout); rs_free(&rs); db_close(db); sshtun_close(tun); }
        }
    }
    else if (!strcmp(cmd, "aboutstate")) about_dump(a);
    else if (!strcmp(cmd, "explorerdump")) { if (!strcmp(arg, "rows")) explorer_dump_rows(a->explorer); else explorer_dump(a->explorer); }
    else if (!strcmp(cmd, "nativesettings")) {   /* clicks the native app menu's Settings... item (macOS) */
        bool ok = macos_trigger_settings();
        printf("nativesettings %s\n", ok ? "sent" : "missing"); fflush(stdout);
    }
    else if (!strcmp(cmd, "nativeabout")) {   /* clicks the native app menu's About item (macOS) */
        bool ok = macos_trigger_about();
        printf("nativeabout %s title=\"%s\"\n", ok ? "sent" : "missing", macos_about_title()); fflush(stdout);
    }
    else if (!strcmp(cmd, "edselect")) {   /* edselect "text": selects its first occurrence in the active editor */
        char q[256]; snprintf(q, sizeof q, "%s", arg[0] == '"' ? arg + 1 : arg); size_t ql = strlen(q); if (ql && q[ql - 1] == '"') q[--ql] = 0;
        int n = 0; char *t = a->editor ? editor_text(a->editor, &n) : NULL; char *at = t ? strstr(t, q) : NULL;
        if (at) editor_select_range(a->editor, (int)(at - t), (int)(at - t) + (int)ql); else { printf("script: target not found: edselect %s\n", arg); fflush(stdout); }
        free(t);
    }
    else if (!strcmp(cmd, "telemetrystate")) telemetry_dump();
    else if (!strcmp(cmd, "updatestate")) update_dump();
    else if (!strcmp(cmd, "dgm")) dgmview_cmd(a, arg);        /* ER diagrams: dgm <op> [args] (dgmview.c) */
    else if (!strcmp(cmd, "dgmstate")) dgmview_dump(a);
    else if (!strcmp(cmd, "dialogawait")) {   /* repeat until the connection dialog's Test connection ends (or the deadline) */
        static Uint64 ddeadline; int ms = atoi(arg);
        if (!ddeadline) ddeadline = now + (Uint64)(ms > 0 ? ms : 30000);
        if (dialog_testing(a->dialog) && now < ddeadline) { cur--; next_at = now + 50; }
        else { if (dialog_testing(a->dialog)) { printf("script: target not found: dialogawait timed out\n"); fflush(stdout); } ddeadline = 0; }
    }
    else if (!strcmp(cmd, "objcatawait")) {   /* repeat until the active connection's object catalog read ended (or the deadline) */
        static Uint64 odeadline; int ms = atoi(arg);
        if (!odeadline) odeadline = now + (Uint64)(ms > 0 ? ms : 60000);
        if (!objcat_settled(a) && now < odeadline) { cur--; next_at = now + 100; }
        else { if (!objcat_settled(a)) { printf("script: target not found: objcatawait timed out\n"); fflush(stdout); } odeadline = 0; }
    }
    else if (!strcmp(cmd, "outawait")) {   /* repeat until no execution is running in any session (or the deadline) */
        static Uint64 odeadline; int ms = atoi(arg);
        if (!odeadline) odeadline = now + (Uint64)(ms > 0 ? ms : 60000);
        if (output_job_running(a->output) && now < odeadline) { cur--; next_at = now + 50; }
        else { if (output_job_running(a->output)) { printf("script: target not found: outawait timed out\n"); fflush(stdout); } odeadline = 0; }
    }
    else if (!strcmp(cmd, "outexport")) output_export(a->output, a, arg);   /* outexport PATH: the active session's Output (.json or text) */
    else if (!strcmp(cmd, "outnotes")) {   /* outnotes "connection": that session's notes (no name: the active session) */
        char cn[64] = ""; sscanf(arg, "\"%63[^\"]\"", cn);
        Connection *nc = NULL;
        for (int i = 0; cn[0] && a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->name, cn)) nc = a->conns->items[i];
        if (cn[0] && !nc) { printf("script: target not found: outnotes %s\n", arg); fflush(stdout); }
        else output_notes_dump(a->output, a, nc);
    }
    else if (!strcmp(cmd, "fetchawait")) {   /* repeat until no Fetch more / Fetch all is running (or the deadline) */
        static Uint64 fdeadline; int ms = atoi(arg);
        if (!fdeadline) fdeadline = now + (Uint64)(ms > 0 ? ms : 60000);
        if (results_fetching(a->results) && now < fdeadline) { cur--; next_at = now + 50; }
        else { if (results_fetching(a->results)) { printf("script: target not found: fetchawait timed out\n"); fflush(stdout); } fdeadline = 0; }
    }
    else if (!strcmp(cmd, "dgmawait")) {   /* repeat until the active diagram's catalog is read and laid out (or the deadline) */
        static Uint64 gdeadline; int ms = atoi(arg);
        if (!gdeadline) gdeadline = now + (Uint64)(ms > 0 ? ms : 30000);
        if (!dgmview_ready(a) && now < gdeadline) { cur--; next_at = now + 50; }
        else { if (!dgmview_ready(a)) { printf("script: target not found: dgmawait timed out\n"); fflush(stdout); } gdeadline = 0; }
    }
    else if (!strcmp(cmd, "updateawait")) { for (int i = 0; i < 1000 && update_busy(); i++) SDL_Delay(10); }   /* the request ends (10 s at most); the next frames show its answer */   /* the update check: state, answer, skipped version */
    else if (!strcmp(cmd, "crashsim")) log_write_crash_report(11);   /* the report a SIGSEGV would leave, without dying (crashrep tests) */   /* consent and the session's counters */
    else if (!strcmp(cmd, "edscroll")) {   /* the active editor's horizontal scroll, its widest line and its text width (px) */
        float sx = 0, wd = 0, vw = 0; if (a->editor) editor_scroll_state(a->editor, &sx, &wd, &vw);
        printf("edscroll sx=%.0f widest=%.0f view=%.0f\n", sx, wd, vw); fflush(stdout);
    }
    else if (!strcmp(cmd, "edtext")) {   /* the active editor tab's text (one line, newlines as \\n) and revision */
        int n = 0; char *t = a->editor ? editor_text(a->editor, &n) : NULL;
        printf("edtext rev=%u text=\"", a->editor ? editor_revision(a->editor) : 0);
        for (int i = 0; i < n; i++) { if (t[i] == '\n') printf("\\n"); else putchar(t[i]); }
        printf("\"\n"); fflush(stdout); free(t);
    }
    else if (!strcmp(cmd, "clipboard")) {   /* what the clipboard holds now (Copy Info tests) */
        char *t = SDL_GetClipboardText();
        printf("clipboard-begin\n%s\nclipboard-end\n", t ? t : ""); fflush(stdout); SDL_free(t);
    }
    else if (!strcmp(cmd, "nsx") && strstr(arg, "\" await")) {   /* repeat this line until the model is idle (or the deadline) */
        static Uint64 deadline; int ms = 0; const char *w = strstr(arg, "\" await"); sscanf(w + 7, "%d", &ms);
        if (!deadline) deadline = now + (Uint64)(ms > 0 ? ms : 30000);
        if (!explorer_nsx_ready(a, arg) && now < deadline) { cur--; next_at = now + 100; }
        else { if (now >= deadline) { printf("script: target not found: nsx await timed out %s\n", arg); fflush(stdout); } deadline = 0; }
    }
    else if (!strcmp(cmd, "nsx")) explorer_nsx_cmd(a->explorer, a, arg);   /* Schema Explorer v2 */
    else if (!strcmp(cmd, "td")) tdes_cmd(a, arg);           /* Visual Table Designer */
    else if (!strcmp(cmd, "plan")) planview_cmd(a, arg);     /* Execution Plan + AI */
    else if (!strcmp(cmd, "planstate")) planview_dump(a);
    else if (!strcmp(cmd, "awaitconn") || !strcmp(cmd, "objawait")) {   /* repeat until connected / the object loaded (or the deadline) */
        static Uint64 cdeadline; char cn[64] = ""; int ms = 0;
        if (!strcmp(cmd, "awaitconn")) sscanf(arg, "\"%63[^\"]\" %d", cn, &ms); else ms = atoi(arg);
        if (!cdeadline) cdeadline = now + (Uint64)(ms > 0 ? ms : 60000);
        bool ready = true;
        if (cn[0]) { ready = false; for (int i = 0; a->conns && i < a->conns->n; i++) { Connection *c = a->conns->items[i]; if (!strcmp(c->name, cn)) ready = c->db || c->state == CONN_FAILED; } }
        else ready = objed_idle(a);
        if (!ready && now < cdeadline) { cur--; next_at = now + 100; }
        else { if (!ready) { printf("script: target not found: %s %s timed out\n", cmd, arg); fflush(stdout); } cdeadline = 0; }
    }
    else if (!strcmp(cmd, "rpt") && !strncmp(arg, "await", 5)) {   /* repeat until queries and AI are idle (or the deadline) */
        static Uint64 rdeadline; int ms = atoi(arg + 5);
        if (!rdeadline) rdeadline = now + (Uint64)(ms > 0 ? ms : 30000);
        if (!rptview_idle(a) && now < rdeadline) { cur--; next_at = now + 100; }
        else { if (now >= rdeadline) { printf("script: target not found: rpt await timed out\n"); fflush(stdout); } rdeadline = 0; }
    }
    else if (!strcmp(cmd, "git") && !strncmp(arg, "await", 5)) {   /* repeat until Git jobs are done (or the deadline) */
        static Uint64 gdeadline; int ms = atoi(arg + 5);
        if (!gdeadline) gdeadline = now + (Uint64)(ms > 0 ? ms : 30000);
        if (!gitview_idle(a) && now < gdeadline) { cur--; next_at = now + 100; }
        else { if (now >= gdeadline) { printf("script: target not found: git await timed out\n"); fflush(stdout); } gdeadline = 0; }
    }
    else if (!strcmp(cmd, "git")) gitview_cmd(a, arg);        /* Source Control */
    else if (!strcmp(cmd, "gitstate")) gitview_dump(a);
    else if (!strcmp(cmd, "rpt")) rptview_cmd(a, arg);        /* Report Studio */
    else if (!strcmp(cmd, "rptstate")) rptview_dump(a);
    else if (!strcmp(cmd, "settingsstate")) settings_dump();
    else if (!strcmp(cmd, "completestate")) { if (a->editor) editor_dump_completion(a->editor); }
    else if (!strcmp(cmd, "objcatstate")) objcat_dump(a);
    else if (!strcmp(cmd, "resstate")) { if (a->results) results_dump(a->results); }
    else if (!strcmp(cmd, "tdstate")) tdes_dump(a);
    else if (!strcmp(cmd, "objstate")) objed_dump(a);   /* the active object tab: state, problems, plan */
    else if (!strcmp(cmd, "objexport")) objed_export_to(a, arg);
    else if (!strcmp(cmd, "objopen")) {
        /* objopen <connection name> <kind> <schema> <name> [signature | table] : as the Explorer would */
        char cn[64] = "", kind[32] = "", sch[128] = "", nm[128] = "", extra[256] = "";
        sscanf(arg, "\"%63[^\"]\" %31s %127s %127s %255[^\n]", cn, kind, sch, nm, extra);
        Connection *c = NULL; for (int i = 0; a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->name, cn)) c = a->conns->items[i];
        ObjKey k; memset(&k, 0, sizeof k);
        static const char *kinds[] = { "view", "matview", "function", "procedure", "package", "package-body", "trigger", "type", "type-body", "operator", "event-trigger", "vtable" };
        k.kind = -1; for (int i = 0; i < 12; i++) if (!strcmp(kind, kinds[i])) k.kind = i;
        snprintf(k.schema, sizeof k.schema, "%s", strcmp(sch, "-") ? sch : ""); snprintf(k.name, sizeof k.name, "%s", nm);
        if (k.kind == OBJK_TRIGGER) snprintf(k.table, sizeof k.table, "%s", extra); else snprintf(k.signature, sizeof k.signature, "%s", extra);
        if (!c || k.kind < 0 || !objed_open(a, c, &k)) { printf("script: objopen failed: %s\n", arg); fflush(stdout); }
    }
    else if (!strcmp(cmd, "tbstate")) { printf("%s\n", toolbar_state_text(a->toolbar, a)); fflush(stdout); }
    else if (!strcmp(cmd, "runtofile")) toolbar_run_to_file(a->toolbar, a, arg);
    else if (!strcmp(cmd, "tbnames")) toolbar_print_names(a->toolbar, a);
    else if (!strcmp(cmd, "menudump")) toolbar_dump_menu(a->toolbar);
    else if (!strcmp(cmd, "tbmodal")) toolbar_dump_modal(a->toolbar, a->scale);
    else if (!strcmp(cmd, "palettedump")) { char q[96]; snprintf(q, sizeof q, "%s", arg[0] == '"' ? arg + 1 : arg); size_t ql = strlen(q); if (ql && q[ql - 1] == '"') q[ql - 1] = 0; panels_palette_dump(a->panels, a, q); }
    else if (!strcmp(cmd, "collisions")) { const char *x = NULL, *y = NULL; int n = commands_collisions(&x, &y); printf("collisions %d%s%s%s%s\n", n, n ? " " : "", n ? x : "", n ? " / " : "", n ? y : ""); fflush(stdout); }
    else if (!strcmp(cmd, "tbadd")) { bool ok = toolbar_add_action(a->toolbar, arg); printf("tbadd %s %s\n", arg, ok ? "ok" : "refused"); fflush(stdout); }
    else if (!strcmp(cmd, "unloadmodule")) {   /* TBR-050: a module disabled at run time: its panels and its commands */
        int nc = commands_unregister_module(arg);
        bool p1 = !strcmp(arg, "lume.markdown") && panels_unregister(a->panels, a, "lume.md.outline");
        bool p2 = !strcmp(arg, "lume.markdown") && panels_unregister(a->panels, a, "lume.md.stats");
        printf("unloadmodule %s commands=%d panels=%d\n", arg, nc, (int)p1 + (int)p2); fflush(stdout);
    }
    else if (!strcmp(cmd, "where")) {   /* rectangle of a recorded target, window units */
        char tgt[96]; snprintf(tgt, sizeof tgt, "%s", arg[0] == '"' ? arg + 1 : arg); size_t tl = strlen(tgt); if (tl && tgt[tl - 1] == '"') tgt[tl - 1] = 0;
        Rect rc; if (ui_target(tgt, &rc)) printf("where %s %.0f %.0f %.0f %.0f\n", arg, rc.x / a->density, rc.y / a->density, rc.w / a->density, rc.h / a->density); else printf("where %s none\n", arg);
        fflush(stdout);
    }
    else if (!strcmp(cmd, "invoke")) commands_invoke(a, arg);   /* a command by id, as the toolbar does */
    else if (!strcmp(cmd, "open")) app_open_path(a, arg);   /* File > Open / drop: by the kind of file */
    else if (!strcmp(cmd, "wheelon")) {   /* wheelon "<target>" DY: scrolls over a recorded target */
        char tgt[96] = ""; float dy = 0; sscanf(arg, "\"%95[^\"]\" %f", tgt, &dy);
        Rect rc; if (ui_target(tgt, &rc)) {
            float wx = (rc.x + rc.w / 2) / a->density, wy = (rc.y + rc.h / 2) / a->density;
            push_mouse(a, SDL_EVENT_MOUSE_MOTION, wx, wy, 0, false);
            SDL_Event e; SDL_zero(e); e.type = SDL_EVENT_MOUSE_WHEEL; e.wheel.windowID = SDL_GetWindowID(a->window);
            e.wheel.y = dy; e.wheel.mouse_x = wx; e.wheel.mouse_y = wy; SDL_PushEvent(&e);
        } else printf("script: target not found: wheelon %s\n", arg);
    }
    else if (!strcmp(cmd, "drop")) {   /* drop "<target>" <path>: a file dropped from the Finder onto a recorded target */
        char tgt[96] = "", path[1024] = ""; sscanf(arg, "\"%95[^\"]\" %1023[^\n]", tgt, path);
        Rect rc; if (ui_target(tgt, &rc)) {
            SDL_Event e; SDL_zero(e); e.type = SDL_EVENT_DROP_FILE; e.drop.windowID = SDL_GetWindowID(a->window);
            e.drop.x = (rc.x + rc.w / 2) / a->density; e.drop.y = (rc.y + rc.h / 2) / a->density; e.drop.data = SDL_strdup(path);
            SDL_PushEvent(&e);
        } else printf("script: target not found: drop %s\n", arg);
    }
    else if (!strcmp(cmd, "clickon")) {   /* clickon "<target>": a click at the centre of a recorded target */
        /* clickon "<target>" [clicks]: clicks = 2 / 3 for a double / triple click */
        char tgt[96] = ""; int nclk = 1;
        if (arg[0] == '"') sscanf(arg, "\"%95[^\"]\" %d", tgt, &nclk); else snprintf(tgt, sizeof tgt, "%s", arg);
        if (nclk < 1 || nclk > 3) nclk = 1;
        Rect rc; if (ui_target(tgt, &rc)) { float cx = (rc.x + rc.w / 2) / a->density, cy = (rc.y + rc.h / 2) / a->density; push_mouse(a, SDL_EVENT_MOUSE_MOTION, cx, cy, 0, false); for (int c = 1; c <= nclk; c++) { push_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, cx, cy, c, true); push_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, cx, cy, c, false); } }
        else printf("script: target not found: clickon %s\n", arg);
    }
    else if (!strcmp(cmd, "hover")) {   /* moves the pointer onto a recorded target (tooltips) */
        char tgt[96]; snprintf(tgt, sizeof tgt, "%s", arg[0] == '"' ? arg + 1 : arg); size_t tl = strlen(tgt); if (tl && tgt[tl - 1] == '"') tgt[tl - 1] = 0;
        Rect rc; if (ui_target(tgt, &rc)) push_mouse(a, SDL_EVENT_MOUSE_MOTION, (rc.x + rc.w / 2) / a->density, (rc.y + rc.h / 2) / a->density, 0, false);
        else printf("script: target not found: hover %s\n", arg);
    }
    else if (!strcmp(cmd, "unregister")) {   /* a module disabled at run time (PNL-047) */
        bool ok = panels_unregister(a->panels, a, arg);
        printf("unregister %s %s\n", arg, ok ? "ok" : "refused"); fflush(stdout);
    }
    else if (!strcmp(cmd, "wait")) next_at = now + (Uint64)atoi(arg);
    else if (!strcmp(cmd, "move")) { sscanf(arg, "%f %f", &x, &y); push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, false); }
    else if (!strcmp(cmd, "click") || !strcmp(cmd, "dblclick") || !strcmp(cmd, "tripleclick")) {
        /* one motion, then one down/up pair per click with increasing click counts */
        int clicks = cmd[0] == 'd' ? 2 : cmd[0] == 't' ? 3 : 1;
        sscanf(arg, "%f %f", &x, &y);
        push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, false);
        for (int c = 1; c <= clicks; c++) {
            push_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, c, true);
            push_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, c, false);
        }
    } else if (!strcmp(cmd, "mods")) {
        /* mods shift|cmd|alt|none: modifier state seen by mouse handlers */
        if (!strcmp(arg, "none")) a->mod_override = -1;
        else {
            int m = 0;
            if (strstr(arg, "shift")) m |= SDL_KMOD_SHIFT;
            if (strstr(arg, "cmd")) m |= SDL_KMOD_GUI;
            if (strstr(arg, "alt")) m |= SDL_KMOD_ALT;
            if (strstr(arg, "ctrl")) m |= SDL_KMOD_CTRL;
            a->mod_override = m;
        }
    } else if (!strcmp(cmd, "outfill")) {
        /* outfill EXECUTIONS EVENTS: fill the Output panel with synthetic history (load testing) */
        int ex = 0, ev = 0; sscanf(arg, "%d %d", &ex, &ev);
        output_fill(a->output, a, ex, ev);
    } else if (!strcmp(cmd, "stat")) {
        /* stat LABEL: frame time without the vsync wait, process RSS, Output rows, present time */
        printf("stat %s: frame %.2f ms  rss %.1f MB  output rows %d  present %.2f ms\n", arg, a->last_frame_ms - a->last_present_ms, sys_rss_bytes() / 1048576.0, output_rows(a->output), a->last_present_ms);
        fflush(stdout);
    } else if (!strcmp(cmd, "export")) {
        /* export PATH: write the current result tab to a file, bypassing the save dialog */
        results_export(a->results, a, arg);
    } else if (!strcmp(cmd, "rclick")) {
        sscanf(arg, "%f %f", &x, &y);
        push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, false);
        push_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, -1, true);
        push_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, -1, false);
    } else if (!strcmp(cmd, "drag")) {
        sscanf(arg, "%f %f %f %f", &x, &y, &x1, &y1);
        push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, false);
        push_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, 1, true);
        /* drag is completed on the following ticks so the app sees intermediate frames */
        char *tail = malloc(64);
        snprintf(tail, 64, "_dragend %g %g", x1, y1);
        lines = realloc(lines, sizeof *lines * (nlines + 1));
        memmove(&lines[cur + 1], &lines[cur], sizeof *lines * (nlines - cur));
        lines[cur] = tail; nlines++;
    } else if (!strcmp(cmd, "pinch")) {   /* pinch X Y SCALE: a trackpad pinch over (X, Y), as SDL delivers it (SDL_EVENT_PINCH_UPDATE) */
        float sc = 1; sscanf(arg, "%f %f %f", &x, &y, &sc);
        push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, false);
        SDL_Event pe; SDL_zero(pe); pe.type = SDL_EVENT_PINCH_BEGIN; SDL_PushEvent(&pe);
        SDL_zero(pe); pe.type = SDL_EVENT_PINCH_UPDATE; pe.pinch.scale = sc; SDL_PushEvent(&pe);
        SDL_zero(pe); pe.type = SDL_EVENT_PINCH_END; SDL_PushEvent(&pe);
    } else if (!strcmp(cmd, "mdown")) {   /* press and hold (a drag spread over frames: mdown, move..., mup) */
        sscanf(arg, "%f %f", &x, &y);
        push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, false);
        push_mouse(a, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, 1, true);
    } else if (!strcmp(cmd, "mup")) {
        sscanf(arg, "%f %f", &x, &y);
        push_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, 1, false);
    } else if (!strcmp(cmd, "_dragend")) {
        sscanf(arg, "%f %f", &x, &y);
        push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, false);
        push_mouse(a, SDL_EVENT_MOUSE_BUTTON_UP, x, y, 1, false);
    } else if (!strcmp(cmd, "key")) push_key(a, arg);
    else if (!strcmp(cmd, "type")) {
        /* type TEXT or type "TEXT": one SDL_EVENT_TEXT_INPUT with the whole text */
        free(held_text);
        held_text = SDL_strdup(arg);
        size_t hl = strlen(held_text);
        if (hl >= 2 && held_text[0] == '"' && held_text[hl - 1] == '"') { held_text[hl - 1] = 0; memmove(held_text, held_text + 1, hl - 1); }
        /* "\n" in a script types a newline (multi-statement scripts) */
        for (char *q = held_text, *w = held_text;; q++) { if (q[0] == '\\' && q[1] == 'n') { *w++ = '\n'; q++; } else { *w++ = *q; if (!*q) break; } }
        SDL_Event e; SDL_zero(e);
        e.type = SDL_EVENT_TEXT_INPUT;
        e.text.windowID = SDL_GetWindowID(a->window);
        e.text.text = held_text;
        SDL_PushEvent(&e);
    } else if (!strcmp(cmd, "wheel")) {
        /* wheel X Y DY [DX]: scroll at (X, Y) */
        float dy = 0, dx = 0;
        sscanf(arg, "%f %f %f %f", &x, &y, &dy, &dx);
        push_mouse(a, SDL_EVENT_MOUSE_MOTION, x, y, 0, false);
        SDL_Event e; SDL_zero(e);
        e.type = SDL_EVENT_MOUSE_WHEEL;
        e.wheel.windowID = SDL_GetWindowID(a->window);
        e.wheel.y = dy; e.wheel.x = dx; e.wheel.mouse_x = x; e.wheel.mouse_y = y;
        SDL_PushEvent(&e);
    } else if (!strcmp(cmd, "shot")) { a->shot_path = SDL_strdup(arg); next_at = now + 200; }
    else if (!strcmp(cmd, "resize")) { int w = 0, h = 0; sscanf(arg, "%d %d", &w, &h); if (w > 0 && h > 0) SDL_SetWindowSize(a->window, w, h); next_at = now + 300; }
    else if (!strcmp(cmd, "quit")) a->running = false;
    else SDL_Log("script: unknown command '%s'", cmd);
    app_wake_in(a, (Uint32)(next_at - now));
}
