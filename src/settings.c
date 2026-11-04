/* SPDX-License-Identifier: AGPL-3.0-only */
/* Settings screen: see settings.h. */
#include "settings.h"
#include "telemetry.h"
#include "commands.h"
#include "conn.h"
#include "results.h"
#include "log.h"
#include "macos_app.h"
#include "draw.h"
#include "gitview.h"
#include "menu.h"
#include "output.h"
#include "panels.h"
#include "rptview.h"
#include "theme.h"
#include "ui.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SEC_GENERAL, SEC_APPEARANCE, SEC_EDITOR, SEC_CONN, SEC_QUERY, SEC_RESULTS, SEC_EXPLORER, SEC_REPORTS, SEC_GIT, SEC_INTELLIGENCE, SEC_PRIVACY, SEC_N };
static const char *const SEC_NAME[SEC_N] = { "General", "Appearance", "Editor", "Connections", "Query Execution", "Results & Format", "Schema Explorer", "Reports", "Source Control", "Lume Intelligence", "Privacy" };
static struct {
    bool open; int sec; Input stash; bool stashed; float scroll, content_h; ScrollBar sb; int prev_focus;
    TextField f_fetch, f_hist; int field;   /* 1 fetch size, 2 history */
    /* the SQL formatter rules (sqlstyle.txt, "*" lines) */
    bool style_loaded; int kw_case, indent; bool tabs, clauses;
} S;
static App *S_app;
static Uint32 S_event;   /* the native application menu's Settings item posts it */

bool settings_is_open(void) { return S.open; }
static void style_load(App *a);
void settings_open(App *a) {
    if (S.open) return;
    S.open = true; S.scroll = 0; S.prev_focus = a->focus; S.field = 0;
    char b[32]; snprintf(b, sizeof b, "%d", a->fetch_size); textfield_set(&S.f_fetch, b);
    int hm; bool ao; output_get_prefs(a->output, &hm, &ao); snprintf(b, sizeof b, "%d", hm); textfield_set(&S.f_hist, b);
    S.style_loaded = false; style_load(a);
    LOG_I("Settings opened");
}
void settings_close(App *a) {
    if (!S.open) return;
    S.open = false; a->focus = S.prev_focus;
    LOG_I("Settings closed");
}
static void inv_open(void *ud, const LumeCtx *c) { (void)ud; (void)c; if (S_app) settings_open(S_app); }
/* ---- colour themes (theme.h): applied on the next frame, saved with the other settings ---- */
static void set_theme(App *a, const char *id) {
    if (!theme_select(id)) return;
    LOG_I("theme: %s%s", g_theme->name, !strcmp(id, "system") ? " (following the system)" : "");
    if (a) app_status(a, "Theme: %s%s", g_theme->name, !strcmp(id, "system") ? " (following the system)" : "");
}
static void inv_theme(void *ud, const LumeCtx *c) { (void)c; set_theme(S_app, (const char *)ud); }
void settings_init(App *a) {
    S_app = a;
    Command c; memset(&c, 0, sizeof c);
    c.id = "app.settings.open"; c.label = "Settings..."; c.group = "Lume"; c.synonyms = "preferences options configuration intelligence fetch format zoom"; c.icon = "settings";
    c.invoke = inv_open; c.module = "lume.settings";
    commands_register(&c);
    /* one palette command per theme ("Theme: Dracula"), plus following the system */
    static char labels[16][48];
    for (int i = -1; i < theme_count() && i < 15; i++) {
        const LumeTheme *t = theme_at(i);
        Command tc; memset(&tc, 0, sizeof tc);
        static char ids[16][48];
        snprintf(ids[i + 1], sizeof ids[i + 1], "app.theme.%s", t ? t->id : "system");
        snprintf(labels[i + 1], sizeof labels[i + 1], "Theme: %s", t ? t->name : "Match System");
        tc.id = ids[i + 1]; tc.label = labels[i + 1]; tc.group = "Appearance"; tc.synonyms = "theme color colour dark light appearance palette"; tc.icon = "settings";
        tc.invoke = inv_theme; tc.ud = (void *)(t ? t->id : "system"); tc.module = "lume.settings";
        commands_register(&tc);
    }
    S_event = SDL_RegisterEvents(1);
    if (S_event && macos_route_settings(S_event)) LOG_I("native application menu: Settings routed to Lume's Settings");
}
Uint32 settings_event_type(void) { return S_event; }
void settings_pre_update(App *a) {
    if (!S.open) {
        if (input_key(&a->in, SDLK_COMMA, KM_PRIMARY)) settings_open(a);
        if (!S.open) return;
    }
    if (input_key(&a->in, SDLK_ESCAPE, 0)) { settings_close(a); return; }
    S.stash = a->in; S.stashed = true;   /* the frame's input belongs to the screen; given back in settings_draw */
    Input *in = &a->in;
    in->mouse_pressed = in->mouse_released = in->mouse_rpressed = false; in->mouse_consumed = true;
    in->nkeys = 0; in->ntext = 0; in->wheel_x = in->wheel_y = 0;
}

/* ---- the SQL formatter's rules: sqlstyle.txt, the "*" (every engine) lines; engine lines are kept ---- */
static void style_path(App *a, char *o, size_t n) { snprintf(o, n, "%ssqlstyle.txt", a->conns ? a->conns->dir : ""); }
static void style_load(App *a) {
    if (S.style_loaded) return;
    S.style_loaded = true; S.kw_case = 0; S.indent = 4; S.tabs = false; S.clauses = true;
    char p[1200]; style_path(a, p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[300];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strncmp(line, "*.", 2)) continue;
        char *eq = strchr(line, '='); if (!eq) continue; *eq++ = 0;
        const char *k = line + 2;
        if (!strcmp(k, "keywords")) S.kw_case = !strcmp(eq, "lower") ? 1 : !strcmp(eq, "keep") ? 2 : 0;
        else if (!strcmp(k, "indent")) { int v = atoi(eq); if (v >= 1 && v <= 8) S.indent = v; }
        else if (!strcmp(k, "tabs")) S.tabs = !strcmp(eq, "yes");
        else if (!strcmp(k, "clauses")) S.clauses = strcmp(eq, "inline") != 0;
    }
    fclose(f);
}
static void style_save(App *a) {
    char p[1200], tmp[1300]; style_path(a, p, sizeof p); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *in = fopen(p, "r"), *out = fopen(tmp, "w"); if (!out) { if (in) fclose(in); return; }
    char line[300];
    while (in && fgets(line, sizeof line, in)) if (strncmp(line, "*.", 2)) fputs(line, out);   /* engine-specific lines stay */
    if (in) fclose(in);
    fprintf(out, "*.keywords=%s\n*.indent=%d\n", S.kw_case == 1 ? "lower" : S.kw_case == 2 ? "keep" : "upper", S.indent);
    if (S.tabs) fprintf(out, "*.tabs=yes\n");
    if (!S.clauses) fprintf(out, "*.clauses=inline\n");
    fclose(out); rename(tmp, p);
}

/* ---- layout helpers ---- */
#define HEAD(txt) do { y += px(a, 8); ui_label(a, rect_make(x, y, w, px(a, 20)), txt, ALIGN_LEFT, T_TEXT); y += px(a, 24); } while (0)
#define NOTE(txt) do { float hh_ = results_draw_wrapped(a->font_small, a->renderer, txt, rect_make(x, y, w, px(a, 60)), color_hex(T_TEXT_DIM), 0, true); y += hh_ + px(a, 6); } while (0)
static int seg(App *a, float x, float *y, float w, const char *label, const char *const *opts, int n, int cur, const char *tag) {
    ui_label(a, rect_make(x, *y, w, px(a, 18)), label, ALIGN_LEFT, T_TEXT_MUTED); *y += px(a, 20);
    float sw = fminf(w, px(a, 110) * (float)n);
    Rect r = rect_make(x, *y, sw, px(a, 28)); ui_record(tag, r);
    float ew = (r.w - px(a, 4)) / (float)n;   /* each choice as "<tag>#<i>" (the harness picks one; same layout as ui_segmented) */
    for (int i = 0; i < n; i++) { char t[64]; snprintf(t, sizeof t, "%s#%d", tag, i); ui_record(t, rect_make(r.x + px(a, 2) + (float)i * ew, r.y + px(a, 2), ew, r.h - px(a, 4))); }
    int k = ui_segmented(a, r, opts, n, cur);
    *y += px(a, 36);
    return k >= 0 && k != cur ? k : -1;
}
/* seg over values: returns the value picked (another one), or -1. A value set by hand in
 * settings.txt that is not among the choices shows with nothing selected. */
static int segv(App *a, float x, float *y, float w, const char *label, const char *const *opts, const int *vals, int n, int cur, const char *tag) {
    int c = -1; for (int i = 0; i < n; i++) if (vals[i] == cur) c = i;
    int k = seg(a, x, y, w, label, opts, n, c, tag);
    return k >= 0 ? vals[k] : -1;
}
static bool chk(App *a, float x, float *y, float w, const char *label, bool v, const char *tag) {
    Rect r = rect_make(x, *y, w, px(a, 22)); ui_record(tag, r);
    bool t = ui_checkbox(a, r, label, v, true);
    *y += px(a, 28);
    return t;
}

static void sec_privacy(App *a, float x, float *py, float w) {
    float y = *py;
    HEAD("Anonymous usage statistics");
    if (chk(a, x, &y, w, "Share anonymous usage statistics", a->telemetry, "settings:telemetry")) { a->telemetry = !a->telemetry; telemetry_set_enabled(a->telemetry); }
    NOTE("Off unless you turn it on. It helps decide what to fix and improve: how fast Lume starts, how often it stutters, which features are used.");
    NOTE("Sent: the version and build, the operating system (name, major version, architecture) and, per session, counts and timings in coarse ranges (\"10-99 queries\", \"startup 0.5-1 s\", \"memory 100-250 MB\").");
    NOTE("Never sent: SQL, data, names of connections, tables or files, hosts, users, paths, keys or prompts. No installation id either: every launch is a new, unrelated session.");
    NOTE("Where: Aptabase, in the EU (Germany). It uses the IP address only to derive the country and does not store it. A session's summary is sent at the next launch, so quitting never waits on the network.");
    HEAD("Crash reports");
    { static const char *const cr[] = { "Ask each time", "Always send", "Never send" };
      int k = seg(a, x, &y, w, "When Lume Data closes unexpectedly, on the next launch:", cr, 3, a->crash_reports, "settings:crash"); if (k >= 0) a->crash_reports = k; }
    NOTE("A crash report holds the version, the operating system, the signal and the function names where Lume stopped. Never SQL, data, names, paths or user information. It goes to GlitchTip; View report (in the dialog) shows exactly what would be sent.");
    HEAD("Updates");
    if (chk(a, x, &y, w, "Check for updates automatically", a->update_check, "settings:updates")) a->update_check = !a->update_check;
    NOTE("At launch and then once a day, Lume asks GitHub for the latest release of Lume Data and, when there is a newer one, offers it. The request carries nothing about you or your work (GitHub sees the IP address, as with any download). Help > Check for Updates works either way.");
    HEAD("Exactly what is sent when Lume starts");
    { char pv[1400]; telemetry_preview(pv, sizeof pv); float hh = results_draw_wrapped(a->font_mono ? a->font_mono : a->font_small, a->renderer, pv, rect_make(x, y, w, px(a, 200)), color_hex(T_TEXT_MUTED), 0, true); y += hh + px(a, 8); }
    *py = y;
}
static void sec_general(App *a, float x, float *py, float w) {
    float y = *py;
    HEAD("Where Lume keeps things");
    char m[1400]; snprintf(m, sizeof m, "Preferences, connections (no passwords: those stay in the %s) and these settings: %s", "OS secret store", a->conns ? a->conns->dir : "?");
    NOTE(m);
    NOTE("Settings are global (every workspace). Rules are edited here; what Lume learns (query history, completion usage, structure indexes, the glossary) stays in its own files.");
    HEAD("Workspace");
    NOTE("Workspaces (their tabs and toolbar) are chosen from the toolbar's workspace menu.");
    *py = y;
}
/* a theme's colours at a glance: chrome, editor, text, accent, keyword, string */
static void theme_swatch(App *a, float x, float y, const LumeTheme *t) {
    SDL_Renderer *r = a->renderer;
    Rect b = rect_make(x, y + px(a, 1), px(a, 112), px(a, 20));
    draw_rrect(r, b, px(a, 4), color_hex(T_BORDER));
    draw_rrect(r, rect_inset(b, px(a, 1), px(a, 1)), px(a, 3), color_hex(t->panel));
    draw_rect(r, rect_make(b.x + px(a, 28), b.y + px(a, 1), b.w - px(a, 29), b.h - px(a, 2)), color_hex(t->editor_bg));
    const uint32_t dots[] = { t->text, t->accent, t->syn_keyword, t->syn_string, t->green };
    for (int i = 0; i < 5; i++) draw_rrect(r, rect_make(b.x + px(a, 34 + 15 * i), b.y + px(a, 5), px(a, 10), px(a, 10)), px(a, 5), color_hex(dots[i]));
    draw_rect(r, rect_make(b.x + px(a, 6), b.y + px(a, 5), px(a, 16), px(a, 2)), color_hex(t->text_muted));
    draw_rect(r, rect_make(b.x + px(a, 6), b.y + px(a, 10), px(a, 12), px(a, 2)), color_hex(t->text_dim));
}
static void sec_appearance(App *a, float x, float *py, float w) {
    float y = *py;
    HEAD("Theme");
    {
        const char *ch = theme_choice();
        float lw = w > px(a, 300) ? w - px(a, 124) : w;
        if (chk(a, x, &y, lw, "Match the system (Lume Dark / Lume Light)", !strcmp(ch, "system"), "settings:theme:system")) set_theme(a, "system");
        for (int i = 0; i < theme_count(); i++) {
            const LumeTheme *t = theme_at(i);
            char tag[64]; snprintf(tag, sizeof tag, "settings:theme:%s", t->id);
            float y0 = y;
            if (chk(a, x, &y, lw, t->name, !strcmp(ch, t->id), tag)) set_theme(a, t->id);
            if (lw < w) theme_swatch(a, x + lw + px(a, 12), y0, t);
        }
        NOTE("Also in the Command Palette (\"Theme: ...\"). Report colours come from the report theme (Report > Theme), not from this one.");
    }
    HEAD("Editor font size");
    static const char *const z[] = { "90%", "100%", "110%", "125%", "150%" }; static const float zv[] = { 0.9f, 1.0f, 1.1f, 1.25f, 1.5f };
    int cur = -1; for (int i = 0; i < 5; i++) if (a->zoom > zv[i] - 0.02f && a->zoom < zv[i] + 0.02f) cur = i;
    int k = seg(a, x, &y, w, "Zoom (also Cmd+= / Cmd+- / Cmd+0)", z, 5, cur, "settings:zoom"); if (k >= 0) app_set_zoom(a, zv[k]);
    HEAD("Panels");
    if (chk(a, x, &y, w, "Compact panel headers", panels_flag(a->panels, 0), "settings:compact")) panels_action(a->panels, a, ACT_COMPACT);
    if (chk(a, x, &y, w, "Reduce motion (no smooth scrolling or animation)", a->reduce_motion, "settings:motion")) panels_action(a->panels, a, ACT_REDUCE_MOTION);
    *py = y;
}
static void sec_editor(App *a, float x, float *py, float w) {
    float y = *py;
    style_load(a);
    HEAD("SQL formatter");
    NOTE("Used by Format in the object editor and when the assistant's SQL is shown formatted. Saved in sqlstyle.txt (every engine); engine-specific lines there are kept.");
    static const char *const kc[] = { "UPPER", "lower", "As written" };
    int k = seg(a, x, &y, w, "Keywords", kc, 3, S.kw_case, "settings:kwcase"); if (k >= 0) { S.kw_case = k; style_save(a); }
    static const char *const ind[] = { "2 spaces", "4 spaces", "8 spaces" }; static const int iv[] = { 2, 4, 8 };
    int ci = S.indent == 2 ? 0 : S.indent == 8 ? 2 : 1;
    k = seg(a, x, &y, w, "Indent", ind, 3, ci, "settings:indent"); if (k >= 0) { S.indent = iv[k]; style_save(a); }
    if (chk(a, x, &y, w, "Indent with tabs", S.tabs, "settings:tabs")) { S.tabs = !S.tabs; style_save(a); }
    if (chk(a, x, &y, w, "One clause per line (SELECT / FROM / WHERE ...)", S.clauses, "settings:clauses")) { S.clauses = !S.clauses; style_save(a); }
    *py = y;
}
static void sec_query(App *a, float x, float *py, float w) {
    float y = *py;
    HEAD("Fetching");
    ui_label(a, rect_make(x, y, w, px(a, 18)), "Rows per fetch (the first page of a result; scroll or Load more for the rest)", ALIGN_LEFT, T_TEXT_MUTED); y += px(a, 20);
    Rect fr = rect_make(x, y, px(a, 120), px(a, 26)); ui_record("settings:fetch", fr);
    if (ui_textfield(a, &S.f_fetch, fr, "500", S.field == 1)) S.field = 1;
    if (ui_button(a, rect_make(x + px(a, 126), y, px(a, 70), px(a, 26)), "Apply", BTN_NORMAL, atoi(S.f_fetch.buf) != a->fetch_size)) {
        int v = atoi(S.f_fetch.buf); if (v < 10) v = 10; if (v > 1000000) v = 1000000; a->fetch_size = v;
        char b[32]; snprintf(b, sizeof b, "%d", v); textfield_set(&S.f_fetch, b);
    }
    static const char *const fp[] = { "100", "500", "1,000", "5,000" }; static const int fv[] = { 100, 500, 1000, 5000 };
    for (int i = 0; i < 4; i++) if (ui_button(a, rect_make(x + px(a, 210) + px(a, 64) * (float)i, y, px(a, 60), px(a, 26)), fp[i], a->fetch_size == fv[i] ? BTN_SELECTED : BTN_QUIET, true)) { a->fetch_size = fv[i]; char b[32]; snprintf(b, sizeof b, "%d", fv[i]); textfield_set(&S.f_fetch, b); }
    y += px(a, 34);
    static const char *const bud[] = { "64 MB", "256 MB", "1 GB", "4 GB" }; static const int bv[] = { 64, 256, 1024, 4096 };
    int cb = -1; for (int i = 0; i < 4; i++) if (a->results_budget_mb == bv[i]) cb = i;
    int k = seg(a, x, &y, w, "Memory for results (older tabs give their rows back first)", bud, 4, cb, "settings:budget"); if (k >= 0) a->results_budget_mb = bv[k];
    HEAD("Timeout");
    { static const char *const o[] = { "No limit", "30 s", "5 min", "30 min" }; static const int v[] = { 0, 30, 300, 1800 };
      int k = segv(a, x, &y, w, "Each statement in the editor; past it the execution is cancelled, as with Stop", o, v, 4, a->query_timeout_s, "settings:qtimeout"); if (k >= 0) a->query_timeout_s = k; }
    HEAD("Scripts");
    if (chk(a, x, &y, w, "Continue after an error (otherwise the script stops at the first failing statement)", a->continue_on_error, "settings:continue")) a->continue_on_error = !a->continue_on_error;
    HEAD("Protection");
    NOTE("Always on, per connection (Edit connection > environment and read-only): on PROD, SQL that may write or a script of several statements asks first; read-only connections refuse writes; Explain Analyze of a write is refused on PROD / read-only.");
    *py = y;
}
static void sec_connections(App *a, float x, float *py, float w) {
    float y = *py;
    HEAD("Timeouts");
    { static const char *const o[] = { "10 s", "30 s", "1 min", "2 min" }; static const int v[] = { 10, 30, 60, 120 };
      int k = segv(a, x, &y, w, "Reaching the database server", o, v, 4, a->connect_timeout_s, "settings:ctimeout"); if (k >= 0) a->connect_timeout_s = k; }
    NOTE("PostgreSQL, MySQL / MariaDB and SQL Server. Oracle uses its client's own setting (sqlnet.ora). Applies to the next connect.");
    { static const char *const o[] = { "20 s", "1 min", "2 min" }; static const int v[] = { 20, 60, 120 };
      int k = segv(a, x, &y, w, "Opening an SSH tunnel", o, v, 3, a->ssh_timeout_s, "settings:sshtimeout"); if (k >= 0) a->ssh_timeout_s = k; }
    HEAD("Completion");
    if (chk(a, x, &y, w, "List every object I can see, in a second session", a->catalog_session, "settings:objcat")) { a->catalog_session = !a->catalog_session; app_refresh_schema(a); }
    NOTE("Completion then proposes the tables, views, synonyms, procedures, functions, packages, sequences and types of every schema your user may see. They are read in a second session, so the editor is never kept waiting; only the names are kept on disk, so the next connect proposes them at once. Off: your own schema's tables only.");
    NOTE("Connections themselves (host, user, SSH, environment, read-only) are edited from the Explorer: Edit connection.");
    *py = y;
}
static void sec_explorer(App *a, float x, float *py, float w) {
    float y = *py;
    HEAD("Timeouts");
    { static const char *const o[] = { "5 s", "15 s", "30 s", "2 min" }; static const int v[] = { 5, 15, 30, 120 };
      int k = segv(a, x, &y, w, "Reading the catalog (columns, indexes, properties, search)", o, v, 4, a->explorer_timeout_s, "settings:etimeout"); if (k >= 0) a->explorer_timeout_s = k; }
    { static const char *const o[] = { "2 min", "10 min", "30 min", "1 h" }; static const int v[] = { 2, 10, 30, 60 };
      int k = segv(a, x, &y, w, "Listing a schema's tables", o, v, 4, a->explorer_list_min, "settings:elist"); if (k >= 0) a->explorer_list_min = k; }
    NOTE("While a listing runs, the schema shows Loading\xE2\x80\xA6 with the seconds elapsed and a Cancel; it uses the connection's session, so the editor waits for it.");
    NOTE("The tree's own options (what is shown, page size, pinned schemas) are per connection: the Explorer's Options menu.");
    *py = y;
}
static void sec_results(App *a, float x, float *py, float w) {
    float y = *py;
    HEAD("Numbers and dates in the grid (display only: stored values and exports stay as they are)");
    if (chk(a, x, &y, w, "Thousands separator", a->fmt_group, "settings:group")) a->fmt_group = !a->fmt_group;
    static const char *const dec[] = { "As stored", "0", "2", "4" }; static const int dv[] = { -1, 0, 2, 4 };
    int cd = 0; for (int i = 0; i < 4; i++) if (a->fmt_decimals == dv[i]) cd = i;
    int k = seg(a, x, &y, w, "Decimals", dec, 4, cd, "settings:decimals"); if (k >= 0) a->fmt_decimals = dv[k];
    static const char *const dt[] = { "ISO (2026-11-01)", "DMY (01/11/2026)", "MDY (11/01/2026)" };
    k = seg(a, x, &y, w, "Dates", dt, 3, a->fmt_date, "settings:dates"); if (k >= 0) a->fmt_date = k;
    static const char *const tz[] = { "As stored", "UTC", "Local" };
    k = seg(a, x, &y, w, "Time zone of timestamps", tz, 3, a->fmt_tz, "settings:tz"); if (k >= 0) a->fmt_tz = k;
    HEAD("Execution Output");
    int hm; bool ao; output_get_prefs(a->output, &hm, &ao);
    if (chk(a, x, &y, w, "Open the Output when a statement fails", ao, "settings:autoopen")) output_set_prefs(a->output, a, hm, !ao);
    ui_label(a, rect_make(x, y, w, px(a, 18)), "Query History keeps the last (entries)", ALIGN_LEFT, T_TEXT_MUTED); y += px(a, 20);
    Rect hr = rect_make(x, y, px(a, 120), px(a, 26)); ui_record("settings:history", hr);
    if (ui_textfield(a, &S.f_hist, hr, "500", S.field == 2)) S.field = 2;
    if (ui_button(a, rect_make(x + px(a, 126), y, px(a, 70), px(a, 26)), "Apply", BTN_NORMAL, atoi(S.f_hist.buf) != hm)) { output_set_prefs(a->output, a, atoi(S.f_hist.buf), ao); output_get_prefs(a->output, &hm, &ao); char b[32]; snprintf(b, sizeof b, "%d", hm); textfield_set(&S.f_hist, b); }
    y += px(a, 34);
    *py = y;
}
static void sec_reports(App *a, float x, float *py, float w) {
    float y = *py;
    HEAD("New reports");
    static const char *const lc[] = { "Português (pt-BR)", "English (en-US)" };
    int cur = !strcmp(a->report_locale, "en-US") ? 1 : 0;
    int k = seg(a, x, &y, w, "Language and number / date format of new reports (each report keeps its own)", lc, 2, cur, "settings:rlocale"); if (k >= 0) snprintf(a->report_locale, sizeof a->report_locale, "%s", k ? "en-US" : "pt-BR");
    NOTE("The look of new reports comes from the workspace theme (Brand Kit) when there is one: Report > Theme.");
    HEAD("Timeout");
    { static const char *const o[] = { "30 s", "2 min", "10 min" }; static const int v[] = { 30, 120, 600 };
      int k = segv(a, x, &y, w, "Each report query (read-only); past it the query is cancelled and the visual shows the timeout", o, v, 3, a->report_timeout_s, "settings:rtimeout"); if (k >= 0) a->report_timeout_s = k; }
    *py = y;
}
static void sec_git(App *a, float x, float *py, float w) {
    float y = *py;
    HEAD("Pull");
    static const char *const pm[] = { "Fast-forward only", "Merge", "Rebase" };
    int k = seg(a, x, &y, w, "Strategy", pm, 3, gitview_pull_mode(a), "settings:pull"); if (k >= 0) gitview_set_pull_mode(a, k);
    HEAD("Timeouts");
    { static const char *const o[] = { "2 min", "10 min", "30 min" }; static const int v[] = { 2, 10, 30 };
      int k = segv(a, x, &y, w, "Fetch, pull, push and other operations on the remote", o, v, 3, a->git_timeout_min, "settings:gtimeout"); if (k >= 0) a->git_timeout_min = k; }
    { static const char *const o[] = { "30 min", "1 h", "2 h" }; static const int v[] = { 30, 60, 120 };
      int k = segv(a, x, &y, w, "Clone", o, v, 3, a->git_clone_min, "settings:gclone"); if (k >= 0) a->git_clone_min = k; }
    HEAD("Identity");
    char m[400]; snprintf(m, sizeof m, "Commits as %s. Lume reads it from git config and never changes it (git config user.name / user.email).", gitview_identity());
    NOTE(m);
    *py = y;
}

void settings_draw(App *a) {
    if (!S.open) return;
    if (S.stashed) { bool down = a->in.mouse_down; float mx = a->in.mouse_x, my = a->in.mouse_y; a->in = S.stash; a->in.mouse_down = down; a->in.mouse_x = mx; a->in.mouse_y = my; S.stashed = false; }
    SDL_Renderer *r = a->renderer;
    draw_rect(r, rect_make(0, 0, a->width, a->height), color_alpha(color_hex(0x000000), 190));   /* dark enough that the app behind does not compete */
    float W = fminf(px(a, 860), a->width - px(a, 32)), H = fminf(px(a, 620), a->height - px(a, 32));
    Rect card = rect_make((a->width - W) / 2, (a->height - H) / 2, W, H);
    draw_rrect(r, card, px(a, 10), color_hex(T_PANEL));
    draw_rect_outline(r, card, px(a, 1), color_hex(T_BORDER));
    ui_record("settings:card", card);
    float nav_w = px(a, 200);
    draw_text_fit(a->font_ui, r, "Settings", -1, rect_make(card.x + px(a, 16), card.y + px(a, 12), nav_w, px(a, 24)), color_hex(T_TEXT), ALIGN_LEFT);
    float ny = card.y + px(a, 48);
    for (int i = 0; i < SEC_N; i++) {
        Rect sr = rect_make(card.x + px(a, 8), ny, nav_w - px(a, 16), px(a, 28));
        char tg[48]; snprintf(tg, sizeof tg, "settings:sec %s", SEC_NAME[i]); ui_record(tg, sr);
        /* a navigation list, left aligned: selected = tinted row with an accent edge (not a button) */
        bool hov = rect_contains(sr, a->in.mouse_x, a->in.mouse_y);
        if (hov) a->want_cursor = CURSOR_HAND;
        if (i == S.sec) { draw_rrect(r, sr, px(a, M_RADIUS), color_hex(T_SELECTED)); draw_rect(r, rect_make(sr.x, sr.y + px(a, 5), px(a, 3), sr.h - px(a, 10)), color_hex(T_ACCENT)); }
        else if (hov) draw_rrect(r, sr, px(a, M_RADIUS), color_hex(T_HOVER));
        draw_text_fit(a->font_ui, r, SEC_NAME[i], -1, rect_make(sr.x + px(a, 12), sr.y, sr.w - px(a, 16), sr.h), color_hex(i == S.sec ? T_TEXT : T_TEXT_MUTED), ALIGN_LEFT);
        if (hov && a->in.mouse_pressed && !a->in.mouse_consumed) { a->in.mouse_consumed = true; S.sec = i; S.scroll = 0; S.field = 0; }
        ny += px(a, 32);
    }
    if (ui_button(a, rect_make(card.x + px(a, 12), card.y + card.h - px(a, 40), px(a, 90), px(a, 26)), "Close", BTN_NORMAL, true)) { settings_close(a); return; }
    {   /* the usual close box, top right (Esc closes too) */
        Rect xb = rect_make(card.x + card.w - px(a, 36), card.y + px(a, 10), px(a, 26), px(a, 26)); ui_record("settings:x", xb);
        if (ui_button(a, xb, "\xC3\x97", BTN_QUIET, true)) { settings_close(a); return; }
    }
    draw_rect(r, rect_make(card.x + nav_w, card.y + px(a, 8), px(a, 1), card.h - px(a, 16)), color_hex(T_BORDER));
    Rect body = rect_make(card.x + nav_w + px(a, 16), card.y + px(a, 12), card.w - nav_w - px(a, 40), card.h - px(a, 24));
    /* scrolling only when the section is taller than the room, clamped before drawing (no jump) */
    float maxs = S.content_h > body.h ? S.content_h - body.h : 0;
    if (maxs > 0 && rect_contains(body, a->in.mouse_x, a->in.mouse_y) && a->in.wheel_y) { S.scroll -= a->in.wheel_y * px(a, 30); a->in.wheel_y = 0; }
    if (maxs > 0) ui_scrollbar_input(a, &S.sb, body, S.content_h, &S.scroll);
    if (S.scroll > maxs) S.scroll = maxs; if (S.scroll < 0) S.scroll = 0;
    clip_push(r, body);
    float x = body.x, w = body.w, y0 = body.y - S.scroll, y = y0;
    switch (S.sec) {
    case SEC_GENERAL: sec_general(a, x, &y, w); break;
    case SEC_APPEARANCE: sec_appearance(a, x, &y, w); break;
    case SEC_EDITOR: sec_editor(a, x, &y, w); break;
    case SEC_CONN: sec_connections(a, x, &y, w); break;
    case SEC_QUERY: sec_query(a, x, &y, w); break;
    case SEC_EXPLORER: sec_explorer(a, x, &y, w); break;
    case SEC_RESULTS: sec_results(a, x, &y, w); break;
    case SEC_REPORTS: sec_reports(a, x, &y, w); break;
    case SEC_GIT: sec_git(a, x, &y, w); break;
    case SEC_INTELLIGENCE: rptview_settings_intelligence(a, body, &y); break;
    case SEC_PRIVACY: sec_privacy(a, x, &y, w); break;
    }
    clip_pop(r);
    S.content_h = y - y0 + px(a, 12);
    if (S.content_h > body.h) ui_scrollbar_draw(a, &S.sb, body, S.content_h, S.scroll);
    a->in.nkeys = 0; a->in.ntext = 0;   /* nothing below the screen takes keys this frame */
}
void settings_dump(void) {
    printf("settingsstate open=%d section=%s scroll=%.0f content=%.0f theme=%s applied=%s\n", S.open, SEC_NAME[S.sec], S.scroll, S.content_h, theme_choice(), g_theme->id); fflush(stdout);
}
