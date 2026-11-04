/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Main toolbar: the bar, its layout and overflow, icons, tooltips, keyboard focus, dropdown
 * menus, the datasource popover and the preferences file. See toolbar.h for the overview and
 * toolbar_actions.c for the commands and dialogs it opens. UI thread only.
 *
 * Units: preferences and item sizes are in UI points; everything drawn is in render pixels
 * (px()); harness positions are window units.
 *
 * Layout (TBR-040, spec section 6): every visible item has a full width (icon + label) and a
 * short one (icon only). When the bar is too narrow, labels go first (workspace, then the
 * others), then the datasource name is shortened, then items move into "..." from the lowest
 * priority up. The datasource, Run, Stop (while something runs) and "..." never move there.
 * Nothing overlaps: a slot that cannot fit is in the overflow menu, with its icon, label,
 * shortcut and disabled state.
 */
#include "toolbar_int.h"
/* the built-in table leaves `custom` false on purpose */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include <ctype.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "conn.h"
#include "draw.h"
#include "font.h"
#include "json.h"
#include "log.h"
#include "menu.h"
#include "output.h"
#include "panels.h"
#include "theme.h"
#include "units.h"

/* ---- built-in items ---------------------------------------------------------- */

#define P_MIN (1u << PRESET_MINIMAL)
#define P_DEF (1u << PRESET_DEFAULT)
#define P_ADV (1u << PRESET_ADVANCED)
static const TbItem builtins[] = {
    /* id           kind          group      prio cmd                 icon         label         req    presets */
    { "workspace",  IK_DROPDOWN,  G_FILE,     55, NULL,               "workspace", "Workspace",  false, P_DEF | P_ADV },
    { "new_sql",    IK_BUTTON,    G_FILE,     60, "sql.new",          "plus",      "SQL",        false, P_MIN | P_DEF | P_ADV },
    { "datasource", IK_DATASOURCE, G_CONTEXT, 95, NULL,               "db",        "Datasource", true,  P_MIN | P_DEF | P_ADV },
    { "schema",     IK_SCHEMA,    G_CONTEXT,  50, NULL,               "schema",    "Schema",     false, P_DEF | P_ADV },
    { "tx",         IK_TX,        G_CONTEXT,  40, NULL,               "tx",        "Tx",         false, P_ADV },
    { "commit",     IK_BUTTON,    G_CONTEXT,  35, "tx.commit",        "commit",    "Commit",     false, P_ADV },
    { "rollback",   IK_BUTTON,    G_CONTEXT,  34, "tx.rollback",      "rollback",  "Rollback",   false, P_ADV },
    { "run",        IK_RUN,       G_EXEC,     94, "sql.run",          "run",       "Run",        true,  P_MIN | P_DEF | P_ADV },
    { "stop",       IK_STOP,      G_EXEC,     93, "sql.cancel",       "stop",      "Stop",       true,  P_MIN | P_DEF | P_ADV },
    { "explain",    IK_BUTTON,    G_EXEC,     30, "sql.explain",      "plan",      "Explain",    false, P_ADV },
    { "history",    IK_BUTTON,    G_EXEC,     29, "panel.history",    "history",   "History",    false, P_ADV },
    { "flex",       IK_FLEX,      G_FLEX,    100, NULL,               NULL,        "",           true,  P_MIN | P_DEF | P_ADV },
    { "search",     IK_BUTTON,    G_NAV,      45, "nav.palette",      "search",    "Search",     false, P_DEF | P_ADV },
    { "panels",     IK_DROPDOWN,  G_NAV,      44, NULL,               "panels",    "Panels",     false, P_DEF | P_ADV },
    { "more",       IK_MORE,      G_NAV,      99, NULL,               "more",      "More",       true,  P_MIN | P_DEF | P_ADV },
};
#define NBUILTIN ((int)(sizeof builtins / sizeof builtins[0]))

const char *tb_preset_name(int p) { static const char *n[] = { "Minimal", "Default", "Advanced", "Custom" }; return p >= 0 && p < PRESET_COUNT ? n[p] : "?"; }

static int builtin_index(const char *id) { for (int i = 0; i < NBUILTIN; i++) if (!strcmp(builtins[i].id, id)) return i; return -1; }

/* ---- preferences ---------------------------------------------------------------- */

bool tb_item_hidden(const Toolbar *t, const char *id) { for (int i = 0; i < t->prefs.nhidden; i++) if (!strcmp(t->prefs.hidden[i], id)) return true; return false; }
void tb_set_hidden(Toolbar *t, const char *id, bool hidden) {
    int k = -1; for (int i = 0; i < t->prefs.nhidden; i++) if (!strcmp(t->prefs.hidden[i], id)) k = i;
    if (hidden && k < 0 && t->prefs.nhidden < TB_MAX_ITEMS) snprintf(t->prefs.hidden[t->prefs.nhidden++], 40, "%s", id);
    if (!hidden && k >= 0) { memmove(&t->prefs.hidden[k], &t->prefs.hidden[k + 1], 40 * (size_t)(t->prefs.nhidden - k - 1)); t->prefs.nhidden--; }
}

/* Default order and visibility of a preset; added actions are kept (shown) at the end of Run's group. */
void tb_apply_preset(Toolbar *t, int preset) {
    if (preset < 0 || preset >= PRESET_CUSTOM) preset = PRESET_DEFAULT;
    t->prefs.preset = preset; t->prefs.norder = 0; t->prefs.nhidden = 0;
    for (int i = 0; i < NBUILTIN; i++) {
        snprintf(t->prefs.order[t->prefs.norder++], 40, "%s", builtins[i].id);
        if (!(builtins[i].presets & (1u << preset))) tb_set_hidden(t, builtins[i].id, true);
        if (!strcmp(builtins[i].id, "history")) for (int c = 0; c < t->prefs.ncustom; c++) snprintf(t->prefs.order[t->prefs.norder++], 40, "cmd:%s", t->prefs.custom[c]);
    }
    tb_rebuild_items(t);
}

static void prefs_defaults(Toolbar *t) {
    memset(&t->prefs, 0, sizeof t->prefs);
    t->prefs.visible = true;
    tb_apply_preset(t, PRESET_DEFAULT);
}

/* Builds items[] from the preference order (unknown ids dropped, missing built-ins appended). */
void tb_rebuild_items(Toolbar *t) {
    t->nitems = 0;
    t->items_version = commands_version();
    bool seen[NBUILTIN] = { 0 };
    for (int k = 0; k < t->prefs.norder && t->nitems < TB_MAX_ITEMS; k++) {
        const char *id = t->prefs.order[k];
        int b = builtin_index(id);
        if (b >= 0 && !seen[b]) { seen[b] = true; t->items[t->nitems++] = builtins[b]; continue; }
        if (!strncmp(id, "cmd:", 4)) {
            const Command *c = commands_find(id + 4);
            if (!c) continue;   /* the action's module is gone: no dangling button */
            TbItem it = { .kind = IK_BUTTON, .group = G_EXEC, .prio = 20, .cmd = c->id, .icon = c->icon, .label = c->label, .custom = true };
            snprintf(it.id, sizeof it.id, "%s", id);
            t->items[t->nitems++] = it;
        }
    }
    for (int b = 0; b < NBUILTIN && t->nitems < TB_MAX_ITEMS; b++) if (!seen[b]) t->items[t->nitems++] = builtins[b];
}

void tb_mark_dirty(Toolbar *t) { t->dirty = true; t->dirty_at = SDL_GetTicks(); }

/* "<prefs>/" for the default workspace, "<prefs>/workspaces/<name>/" for the others. */
static void ws_dir_for(App *a, const char *name, char *out, size_t n) {
    const char *base = a->conns ? a->conns->dir : "";
    if (!name[0] || !strcmp(name, "Lume Data")) snprintf(out, n, "%s", base);
    else snprintf(out, n, "%sworkspaces/%s/", base, name);
}

/* JSON string escape for the small values written here (ids, names). */
static void put_str(FILE *f, const char *s) {
    fputc('"', f);
    for (; *s; s++) { if (*s == '"' || *s == '\\') fputc('\\', f); if ((unsigned char)*s >= 0x20) fputc(*s, f); }
    fputc('"', f);
}

void tb_save_prefs(Toolbar *t, App *a) {
    (void)a;
    if (!t->ws_dir[0]) return;
    char path[1200], tmp[1210];
    snprintf(path, sizeof path, "%stoolbar.json", t->ws_dir);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { LOG_W("toolbar: cannot write %s", tmp); return; }
    /* ids, flags and names only: no SQL, no connection strings, no credentials (TBR-042) */
    fprintf(f, "{\n  \"schemaVersion\": 1,\n  \"visible\": %s,\n  \"compact\": %s,\n  \"labels\": %s,\n  \"preset\": ",
            t->prefs.visible ? "true" : "false", t->prefs.compact ? "true" : "false", t->prefs.labels ? "true" : "false");
    put_str(f, tb_preset_name(t->prefs.preset));
    const char *lists[] = { "order", "hidden", "custom", "recentDatasources" };
    for (int L = 0; L < 4; L++) {
        int n = L == 0 ? t->prefs.norder : L == 1 ? t->prefs.nhidden : L == 2 ? t->prefs.ncustom : t->prefs.nrecent;
        fprintf(f, ",\n  \"%s\": [", lists[L]);
        for (int i = 0; i < n; i++) { if (i) fputs(", ", f); put_str(f, L == 0 ? t->prefs.order[i] : L == 1 ? t->prefs.hidden[i] : L == 2 ? t->prefs.custom[i] : t->prefs.recent[i]); }
        fputc(']', f);
    }
    fputs("\n}\n", f);
    fflush(f); fsync(fileno(f)); fclose(f);
    if (rename(tmp, path) != 0) LOG_W("toolbar: cannot replace %s", path);
    t->dirty = false;
}

/* Child of object node obj with key, or -1. */
static int jm(const Json *j, int obj, const char *key) {
    if (obj < 0 || j->nodes[obj].type != JN_OBJECT) return -1;
    size_t kl = strlen(key);
    for (int c = j->nodes[obj].first_child; c >= 0; c = j->nodes[c].next_sibling)
        if (j->nodes[c].key_len == kl && !memcmp(j->src + j->nodes[c].key_off, key, kl)) return c;
    return -1;
}
static bool jstr_(const Json *j, int n, char *out, size_t on) {
    if (n < 0 || j->nodes[n].type != JN_STRING) return false;
    json_unescape(j->src + j->nodes[n].val_off, j->nodes[n].val_len, out, on);
    return true;
}
static int jbool_(const Json *j, int n) {
    if (n < 0) return -1;
    if (j->nodes[n].type == JN_TRUE) return 1;
    if (j->nodes[n].type == JN_FALSE) return 0;
    return -2;
}

/* Reads toolbar.json; false (with why) when it is missing, invalid or from another schema. */
static bool prefs_read(Toolbar *t, const char *path, char *why, size_t wn) {
    size_t len = 0; char *src = SDL_LoadFile(path, &len);
    if (!src) { snprintf(why, wn, "missing"); return false; }
    Json j; bool ok = false;
    TbPrefs p; memset(&p, 0, sizeof p); p.visible = true; p.preset = PRESET_DEFAULT;
    if (!json_parse(&j, src, len)) { snprintf(why, wn, "not valid JSON: %s", j.err); goto out; }
    if (j.nodes[0].type != JN_OBJECT) { snprintf(why, wn, "not a JSON object"); goto out; }
    int v = jm(&j, 0, "schemaVersion");
    if (v < 0 || j.nodes[v].type != JN_NUMBER || atoi(j.src + j.nodes[v].val_off) != 1) { snprintf(why, wn, "unsupported schemaVersion"); goto out; }
    int b;
    if ((b = jbool_(&j, jm(&j, 0, "visible"))) == -2) { snprintf(why, wn, "visible is not a boolean"); goto out; } else if (b >= 0) p.visible = b;
    if ((b = jbool_(&j, jm(&j, 0, "compact"))) == -2) { snprintf(why, wn, "compact is not a boolean"); goto out; } else if (b >= 0) p.compact = b;
    if ((b = jbool_(&j, jm(&j, 0, "labels"))) == -2) { snprintf(why, wn, "labels is not a boolean"); goto out; } else if (b >= 0) p.labels = b;
    char pn[24] = "";
    if (jstr_(&j, jm(&j, 0, "preset"), pn, sizeof pn)) { p.preset = -1; for (int i = 0; i < PRESET_COUNT; i++) if (!strcasecmp(pn, tb_preset_name(i))) p.preset = i; if (p.preset < 0) { snprintf(why, wn, "unknown preset \"%s\"", pn); goto out; } }
    const char *lists[] = { "order", "hidden", "custom", "recentDatasources" };
    for (int L = 0; L < 4; L++) {
        int arr = jm(&j, 0, lists[L]);
        if (arr < 0) continue;
        if (j.nodes[arr].type != JN_ARRAY) { snprintf(why, wn, "%s is not an array", lists[L]); goto out; }
        int max = L == 2 ? TB_MAX_CUSTOM : L == 3 ? TB_RECENT : TB_MAX_ITEMS;
        for (int c = j.nodes[arr].first_child; c >= 0; c = j.nodes[c].next_sibling) {
            char s[40];
            if (!jstr_(&j, c, s, sizeof s)) { snprintf(why, wn, "%s holds a non-string", lists[L]); goto out; }
            int *n = L == 0 ? &p.norder : L == 1 ? &p.nhidden : L == 2 ? &p.ncustom : &p.nrecent;
            if (*n >= max) continue;
            char *dst = L == 0 ? p.order[*n] : L == 1 ? p.hidden[*n] : L == 2 ? p.custom[*n] : p.recent[*n];
            snprintf(dst, L == 3 ? 16 : 40, "%s", s); (*n)++;
        }
    }
    t->prefs = p;
    if (!t->prefs.norder) { int keep = t->prefs.preset; tb_apply_preset(t, keep == PRESET_CUSTOM ? PRESET_DEFAULT : keep); t->prefs.preset = keep; }
    /* safety: required items can never be hidden, whatever the file says */
    for (int i = 0; i < NBUILTIN; i++) if (builtins[i].required) tb_set_hidden(t, builtins[i].id, false);
    tb_rebuild_items(t);
    ok = true;
out:
    if (j.nodes) json_free(&j);
    SDL_free(src);
    return ok;
}

/* Switches to workspace `name` (TBR-004 / 042): its folder and its toolbar.json. A corrupt file is
 * set aside as toolbar.json.bad-<time> and the defaults apply; the app keeps working (TBR-052). */
void tb_load_workspace(Toolbar *t, App *a, const char *name) {
    snprintf(t->ws_name, sizeof t->ws_name, "%s", name && name[0] ? name : "Lume Data");
    ws_dir_for(a, t->ws_name, t->ws_dir, sizeof t->ws_dir);
    if (strcmp(t->ws_name, "Lume Data") != 0) {
        char d[1200]; snprintf(d, sizeof d, "%sworkspaces", a->conns ? a->conns->dir : "");
        SDL_CreateDirectory(d); SDL_CreateDirectory(t->ws_dir);
    }
    char path[1200], why[200] = "";
    snprintf(path, sizeof path, "%stoolbar.json", t->ws_dir);
    if (!prefs_read(t, path, why, sizeof why)) {
        if (strcmp(why, "missing") != 0) {
            char bad[1300]; snprintf(bad, sizeof bad, "%s.bad-%lld", path, (long long)time(NULL));
            rename(path, bad);
            LOG_W("toolbar preferences rejected (%s): kept aside as %s; defaults in use", why, strrchr(bad, '/') ? strrchr(bad, '/') + 1 : bad);
            app_status(a, "Toolbar settings were invalid and were set aside; the default toolbar is in use");
        }
        TbPrefs keep = t->prefs; prefs_defaults(t);
        if (strcmp(why, "missing") == 0 && keep.nrecent) { memcpy(t->prefs.recent, keep.recent, sizeof keep.recent); t->prefs.nrecent = keep.nrecent; }
    }
    LOG_I("workspace \"%s\": toolbar %s, %s preset", t->ws_name, t->prefs.visible ? "shown" : "hidden", tb_preset_name(t->prefs.preset));
}

bool toolbar_export_prefs(Toolbar *t, App *a, const char *path) {
    tb_save_prefs(t, a);
    char src[1200]; snprintf(src, sizeof src, "%stoolbar.json", t->ws_dir);
    size_t len = 0; char *data = SDL_LoadFile(src, &len);
    if (!data) return false;
    FILE *f = fopen(path, "w");
    bool ok = f && fwrite(data, 1, len, f) == len;
    if (f) fclose(f);
    SDL_free(data);
    LOG_I("toolbar settings exported (%s)", ok ? "ok" : "failed");
    return ok;
}

void tb_note_recent(Toolbar *t, const char *conn_id) {
    int k = -1; for (int i = 0; i < t->prefs.nrecent; i++) if (!strcmp(t->prefs.recent[i], conn_id)) k = i;
    if (k < 0) k = t->prefs.nrecent < TB_RECENT ? t->prefs.nrecent++ : TB_RECENT - 1;
    memmove(&t->prefs.recent[1], &t->prefs.recent[0], 16 * (size_t)k);
    snprintf(t->prefs.recent[0], 16, "%s", conn_id);
    tb_mark_dirty(t);
}

Connection *tb_conn_by_id(App *a, const char *id) {
    for (int i = 0; a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->id, id)) return a->conns->items[i];
    return NULL;
}

/* ---- create / destroy ----------------------------------------------------------- */

Toolbar *toolbar_create(App *a) {
    Toolbar *t = calloc(1, sizeof *t);
    t->hover = -1; t->focus = -1; t->menu_item = -1;
    commands_init();
    /* the last workspace used comes back (default otherwise) */
    char last[64] = "", path[1100];
    snprintf(path, sizeof path, "%sworkspace.txt", a->conns ? a->conns->dir : "");
    FILE *f = fopen(path, "r");
    if (f) { if (fgets(last, sizeof last, f)) last[strcspn(last, "\r\n")] = 0; fclose(f); }
    tb_load_workspace(t, a, last);
    /* LUME_TOOLBAR=hidden: this run only (tests written before the toolbar use window coordinates) */
    const char *env = SDL_getenv("LUME_TOOLBAR");
    if (env && !strcmp(env, "hidden")) { t->prefs.visible = false; LOG_I("toolbar hidden for this run (LUME_TOOLBAR=hidden)"); }
    return t;
}

void toolbar_destroy(Toolbar *t, App *a) {
    if (!t) return;
    if (t->dirty) tb_save_prefs(t, a);
    tba_run_free(&t->run);
    for (int i = 0; i < t->nlast; i++) free(t->last[i].sql);
    free(t);
}

bool toolbar_modal_open(Toolbar *t) { return t && (t->modal != TM_NONE || t->ds_open || t->menu.open || t->kb_mode); }

float toolbar_height(Toolbar *t, App *a) { return t && t->prefs.visible ? px(a, t->prefs.compact ? 32 : 40) : 0; }

/* ---- icons ------------------------------------------------------------------------ */

/* Small vector icons drawn with the 2D primitives (no image assets), centred on (cx, cy). */
static void tb_icon(App *a, const char *icon, float cx, float cy, float s, Color c) {
    SDL_Renderer *r = a->renderer;
    float lw = px(a, 1.5f);
    if (!icon) return;
    if (!strcmp(icon, "plus")) { draw_rect(r, rect_make(cx - s * 0.4f, cy - lw / 2, s * 0.8f, lw), c); draw_rect(r, rect_make(cx - lw / 2, cy - s * 0.4f, lw, s * 0.8f), c); }
    else if (!strcmp(icon, "run")) {   /* a "play" triangle pointing right */
        SDL_FColor fc = { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f };
        SDL_Vertex v[3] = { { { cx - s * 0.26f, cy - s * 0.34f }, fc, { 0, 0 } }, { { cx + s * 0.36f, cy }, fc, { 0, 0 } }, { { cx - s * 0.26f, cy + s * 0.34f }, fc, { 0, 0 } } };
        SDL_RenderGeometry(r, NULL, v, 3, NULL, 0);
    }
    else if (!strcmp(icon, "stop")) draw_rrect(r, rect_make(cx - s * 0.3f, cy - s * 0.3f, s * 0.6f, s * 0.6f), px(a, 1.5f), c);
    else if (!strcmp(icon, "search")) {
        float rr = s * 0.28f;
        for (int k = 0; k < 24; k++) { float a0 = k * 6.2832f / 24, a1 = (k + 1) * 6.2832f / 24; draw_line(r, cx - s * 0.08f + cosf(a0) * rr, cy - s * 0.08f + sinf(a0) * rr, cx - s * 0.08f + cosf(a1) * rr, cy - s * 0.08f + sinf(a1) * rr, lw, c); }
        draw_line(r, cx + s * 0.12f, cy + s * 0.12f, cx + s * 0.38f, cy + s * 0.38f, lw * 1.3f, c);
    }
    else if (!strcmp(icon, "panels")) { draw_rect_outline(r, rect_make(cx - s * 0.4f, cy - s * 0.32f, s * 0.8f, s * 0.64f), lw, c); draw_rect(r, rect_make(cx - s * 0.4f, cy - s * 0.32f, s * 0.26f, s * 0.64f), c); }
    else if (!strcmp(icon, "more")) { for (int k = -1; k <= 1; k++) draw_rrect(r, rect_make(cx + k * s * 0.3f - s * 0.07f, cy - s * 0.07f, s * 0.14f, s * 0.14f), s * 0.07f, c); }
    else if (!strcmp(icon, "workspace")) { draw_rect_outline(r, rect_make(cx - s * 0.4f, cy - s * 0.22f, s * 0.8f, s * 0.56f), lw, c); draw_rect(r, rect_make(cx - s * 0.4f, cy - s * 0.34f, s * 0.34f, s * 0.14f), c); }
    else if (!strcmp(icon, "db")) draw_icon_db(r, cx, cy, s * 0.9f, c, color_hex(T_BG));
    else if (!strcmp(icon, "plan")) { for (int k = 0; k < 3; k++) draw_rect(r, rect_make(cx - s * 0.4f + k * s * 0.12f, cy - s * 0.3f + k * s * 0.24f, s * 0.8f - k * s * 0.24f, lw * 1.2f), c); }
    else if (!strcmp(icon, "history")) {
        float rr = s * 0.36f;
        for (int k = 0; k < 24; k++) { float a0 = k * 6.2832f / 24, a1 = (k + 1) * 6.2832f / 24; draw_line(r, cx + cosf(a0) * rr, cy + sinf(a0) * rr, cx + cosf(a1) * rr, cy + sinf(a1) * rr, lw, c); }
        draw_line(r, cx, cy, cx, cy - rr * 0.6f, lw, c); draw_line(r, cx, cy, cx + rr * 0.5f, cy, lw, c);
    }
    else if (!strcmp(icon, "commit")) { draw_line(r, cx - s * 0.32f, cy, cx - s * 0.08f, cy + s * 0.26f, lw * 1.2f, c); draw_line(r, cx - s * 0.08f, cy + s * 0.26f, cx + s * 0.36f, cy - s * 0.28f, lw * 1.2f, c); }
    else if (!strcmp(icon, "rollback")) { draw_chevron_back(r, cx - s * 0.12f, cy, s * 0.5f, lw * 1.2f, false, c); draw_line(r, cx - s * 0.3f, cy, cx + s * 0.34f, cy, lw * 1.2f, c); }
    else if (!strcmp(icon, "tx")) { draw_rect_outline(r, rect_make(cx - s * 0.36f, cy - s * 0.3f, s * 0.72f, s * 0.6f), lw, c); draw_line(r, cx - s * 0.2f, cy, cx + s * 0.2f, cy, lw, c); }
    else if (!strcmp(icon, "tabs")) { draw_rect_outline(r, rect_make(cx - s * 0.4f, cy - s * 0.2f, s * 0.8f, s * 0.5f), lw, c); draw_rect(r, rect_make(cx - s * 0.4f, cy - s * 0.34f, s * 0.36f, s * 0.14f), c); }
    else if (!strcmp(icon, "gear") || !strcmp(icon, "info") || !strcmp(icon, "refresh") || !strcmp(icon, "save") || !strcmp(icon, "plug") || !strcmp(icon, "unplug") || !strcmp(icon, "grid") || !strcmp(icon, "log") || !strcmp(icon, "toolbar") || !strcmp(icon, "schema")) {
        char ch[2] = { (char)toupper((unsigned char)icon[0]), 0 };
        draw_rrect(r, rect_make(cx - s * 0.4f, cy - s * 0.4f, s * 0.8f, s * 0.8f), px(a, 3), color_alpha(c, 40));
        draw_text_fit(a->font_small, r, ch, -1, rect_make(cx - s * 0.4f, cy - s * 0.4f, s * 0.8f, s * 0.8f), c, ALIGN_CENTER);
    }
}

/* ---- per-item content -------------------------------------------------------------- */

/* Whether item it is shown this frame (preferences + context: Stop only while something runs,
 * schema only with a connection, transaction controls when relevant). */
static bool item_present(Toolbar *t, const TbItem *it, const LumeCtx *c) {
    if (it->kind == IK_FLEX || it->kind == IK_MORE) return true;
    bool hidden = tb_item_hidden(t, it->id);
    switch (it->kind) {
    case IK_STOP: return c->running_total > 0;   /* "always quickly reachable while something runs" */
    case IK_SCHEMA: return !hidden && c->has_conn;
    case IK_TX: return c->has_conn && c->caps && c->caps->transactions && (!hidden || c->manual_tx || c->tx_state == 1);   /* contextual (spec 3.2) */
    default: break;
    }
    if (it->cmd && (!strcmp(it->cmd, "tx.commit") || !strcmp(it->cmd, "tx.rollback")))
        return c->has_conn && c->caps && c->caps->transactions && (!hidden || c->tx_state == 1);
    return !hidden;
}

/* Elapsed time as "00:03" / "1:02:03". */
static void fmt_clock(double ms, char *out, size_t n) {
    long s = (long)(ms / 1000);
    if (s >= 3600) snprintf(out, n, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else snprintf(out, n, "%02ld:%02ld", s / 60, s % 60);
}

/* Text of an item (full form). */
static void item_text(Toolbar *t, App *a, const TbItem *it, const LumeCtx *c, char *out, size_t n) {
    (void)a;
    out[0] = 0;
    switch (it->kind) {
    case IK_DATASOURCE:
        if (!c->sql_editor_active) snprintf(out, n, "No query tab");
        else if (!c->has_conn) snprintf(out, n, "Select connection");
        else if (c->conn_state == CONN_CONNECTING) snprintf(out, n, "Connecting %s", c->conn_name);
        else if (c->conn_state == CONN_RECONNECTING) snprintf(out, n, "Reconnecting %s", c->conn_name);
        else if (c->conn_state == CONN_FAILED) snprintf(out, n, "%s \xC2\xB7 failed", c->conn_name);
        else if (!c->connected) snprintf(out, n, "%s \xC2\xB7 disconnected", c->conn_name);
        else snprintf(out, n, "%s", c->conn_name);
        break;
    case IK_SCHEMA:
        if (c->caps && c->caps->attached_dbs) snprintf(out, n, "main");
        else snprintf(out, n, "%s", c->schema[0] ? c->schema : "default schema");
        break;
    case IK_TX:
        snprintf(out, n, "Tx: %s%s", c->manual_tx ? "Manual" : "Auto", c->tx_state == 1 ? " \xC2\xB7 pending" : "");
        break;
    case IK_RUN: {
        if (c->running) { char clk[16]; fmt_clock(c->elapsed_ms, clk, sizeof clk); snprintf(out, n, "%s %s", c->cancelling ? "Cancelling" : "Running", clk); }
        else snprintf(out, n, "Run");
        break;
    }
    case IK_STOP:
        if (c->running_total > 1) snprintf(out, n, "Stop (%d)", c->running_total); else snprintf(out, n, "Stop");
        break;
    case IK_DROPDOWN:
        if (!strcmp(it->id, "workspace")) snprintf(out, n, "%s", t->ws_name);
        else snprintf(out, n, "%s", it->label);
        break;
    default: snprintf(out, n, "%s", it->label ? it->label : ""); break;
    }
}

/* Whether a labelled item shows its label in the full form (TBR-002: labels only where they
 * remove ambiguity; "Show Labels" adds them to the icon-only ones). */
static bool item_has_label(Toolbar *t, const TbItem *it) {
    switch (it->kind) {
    case IK_DATASOURCE: case IK_SCHEMA: case IK_TX: case IK_RUN: case IK_STOP: return true;
    case IK_MORE: case IK_FLEX: return false;
    default: break;
    }
    if (!strcmp(it->id, "workspace") || !strcmp(it->id, "new_sql")) return true;
    return t->prefs.labels || it->custom;
}

static bool item_has_chevron(const TbItem *it) { return it->kind == IK_DROPDOWN || it->kind == IK_DATASOURCE || it->kind == IK_SCHEMA || it->kind == IK_TX || it->kind == IK_RUN || it->kind == IK_STOP; }

/* Command an item invokes, and whether it is enabled (why = the reason). */
static bool item_enabled(const TbItem *it, char *why, size_t n) {
    if (why && n) why[0] = 0;
    if (it->kind == IK_STOP) return commands_enabled("sql.cancel", why, n) || commands_ctx()->running_total > 0;
    if (it->cmd) return commands_enabled(it->cmd, why, n);
    return true;
}

/* ---- layout ------------------------------------------------------------------------- */

typedef struct { int item; float wfull, wshort, wmin; bool labelled; } Cand;

/* Lays the bar out for this frame: which items fit, in which form, and what overflows. */
static void layout(Toolbar *t, App *a, Rect bar, const LumeCtx *c) {
    t->nslots = 0; t->noverflow = 0;
    Cand cand[TB_MAX_ITEMS]; int nc = 0;
    float pad = px(a, t->prefs.compact ? 6 : 8), icon = px(a, 16), gap = px(a, 4), chev = px(a, 14);
    for (int i = 0; i < t->nitems; i++) {
        const TbItem *it = &t->items[i];
        if (!item_present(t, it, c)) continue;
        Cand *k = &cand[nc++];
        k->item = i; k->labelled = item_has_label(t, it);
        if (it->kind == IK_FLEX) { k->wfull = k->wshort = k->wmin = 0; continue; }
        char txt[160]; item_text(t, a, it, c, txt, sizeof txt);
        float tw = k->labelled ? font_width(a->font_ui, txt, -1) : 0;
        float base = pad * 2 + (it->icon ? icon : 0) + (item_has_chevron(it) ? chev : 0);
        if (it->kind == IK_DATASOURCE) base += px(a, 10) + (c->env ? px(a, 44) : 0) + (c->read_only ? px(a, 72) : 0);   /* status dot, env and read-only pills */
        k->wfull = base + (tw > 0 ? tw + gap : 0);
        k->wshort = base;
        k->wmin = it->kind == IK_DATASOURCE ? base + px(a, 40) : k->wshort;   /* the datasource keeps some of its name */
        if (it->kind == IK_RUN) k->wmin = k->wshort;
    }
    float avail = bar.w - px(a, 8) * 2;
    float sep = px(a, 13);
    float width[TB_MAX_ITEMS]; bool shortf[TB_MAX_ITEMS] = { 0 }, out[TB_MAX_ITEMS] = { 0 };
    for (int i = 0; i < nc; i++) width[i] = cand[i].wfull;
    #define TOTAL() ({ float s_ = 0; int g_ = -1; for (int q = 0; q < nc; q++) if (!out[q]) { int gg = t->items[cand[q].item].group; if (g_ >= 0 && gg != g_ && gg != G_FLEX && g_ != G_FLEX) s_ += sep; g_ = gg; s_ += width[q] + gap; } s_; })
    /* 1. labels go, workspace first, then the others (never the datasource / Run text) */
    const char *label_order[] = { "workspace", "new_sql", "*", NULL };
    for (int L = 0; label_order[L] && TOTAL() > avail; L++)
        for (int i = 0; i < nc && TOTAL() > avail; i++) {
            const TbItem *it = &t->items[cand[i].item];
            if (it->kind == IK_DATASOURCE || it->kind == IK_RUN || it->kind == IK_STOP || shortf[i]) continue;
            if (strcmp(label_order[L], "*") && strcmp(label_order[L], it->id)) continue;
            shortf[i] = true; width[i] = cand[i].wshort;
        }
    /* 2. the datasource name is shortened (never below its minimum) */
    for (int i = 0; i < nc && TOTAL() > avail; i++) if (t->items[cand[i].item].kind == IK_DATASOURCE) { float over = TOTAL() - avail; width[i] = fmaxf(cand[i].wmin, width[i] - over); }
    /* 3. lowest priority first into "..." (required items stay) */
    while (TOTAL() > avail) {
        int pick = -1;
        for (int i = 0; i < nc; i++) { const TbItem *it = &t->items[cand[i].item]; if (out[i] || it->required) continue; if (pick < 0 || it->prio < t->items[cand[pick].item].prio) pick = i; }
        if (pick < 0) break;
        out[pick] = true; t->overflow[t->noverflow++] = cand[pick].item;
    }
    /* 4. still too narrow: Run loses its label, the datasource shrinks to what remains */
    for (int i = 0; i < nc && TOTAL() > avail; i++) if (t->items[cand[i].item].kind == IK_RUN) { shortf[i] = true; width[i] = cand[i].wshort; }
    for (int i = 0; i < nc && TOTAL() > avail; i++) if (t->items[cand[i].item].kind == IK_DATASOURCE) width[i] = fmaxf(cand[i].wshort, width[i] - (TOTAL() - avail));
    float used = TOTAL();
    #undef TOTAL
    /* place left to right; the flex item takes the remaining width */
    float x = bar.x + px(a, 8), h = bar.h - px(a, t->prefs.compact ? 6 : 8), y = bar.y + (bar.h - h) / 2;
    int prev_group = -1;
    for (int i = 0; i < nc; i++) {
        if (out[i]) continue;
        const TbItem *it = &t->items[cand[i].item];
        if (prev_group >= 0 && it->group != prev_group && it->group != G_FLEX && prev_group != G_FLEX) {
            draw_rect(a->renderer, rect_make(x + sep / 2 - px(a, 4), y + h * 0.2f, px(a, 1), h * 0.6f), color_hex(T_BORDER));
            x += sep;
        }
        prev_group = it->group;
        float w = it->kind == IK_FLEX ? fmaxf(0, avail - used) : width[i];
        TbSlot *s = &t->slots[t->nslots++];
        s->item = cand[i].item; s->r = rect_make(x, y, w, h); s->short_form = shortf[i] || !cand[i].labelled;
        s->chev = item_has_chevron(it) ? rect_make(x + w - chev - pad / 2, y, chev + pad / 2, h) : rect_make(0, 0, 0, 0);
        x += w + gap;
    }
}

/* ---- tooltips (TBR-044) ---------------------------------------------------------------- */

static void item_tooltip(Toolbar *t, App *a, const TbItem *it, const LumeCtx *c, char *out, size_t n) {
    char why[200] = ""; bool en = item_enabled(it, why, sizeof why);
    const char *sc = it->cmd ? commands_shortcut(it->cmd) : "";
    switch (it->kind) {
    case IK_DATASOURCE: {
        if (!c->has_conn) { snprintf(out, n, "Datasource of this query tab: none. Choose where the SQL runs."); return; }
        const char *drv = c->caps ? c->caps->name : c->driver;
        size_t l = (size_t)snprintf(out, n, "Runs on %s (%s), %s%s%s", c->conn_name, drv, conn_state_name(c->conn_state),
                                    c->env ? "  \xC2\xB7  environment " : "", c->env ? conn_env_name(c->env) : "");
        if (c->read_only && l < n) l += (size_t)snprintf(out + l, n - l, "\n%s", caps_ro_text(c->ro_level ? c->ro_level : (c->caps ? c->caps->read_only : RO_LUME_CHECK)));
        if (c->conn_state == CONN_FAILED && c->error[0] && l < n) snprintf(out + l, n - l, "\nLast failure: %s", c->error);
        return;
    }
    case IK_SCHEMA:
        if (c->caps && c->caps->attached_dbs) snprintf(out, n, "SQLite has no schemas: \"main\" is this database file; attached databases are listed in the menu");
        else snprintf(out, n, "Default schema of this session: %s", c->schema[0] ? c->schema : "the server's own (not changed in Lume; shown as unknown, never assumed)");
        return;
    case IK_TX:
        snprintf(out, n, "Transaction mode of the %s session: %s%s", c->conn_name, c->manual_tx ? "manual (Commit / Rollback end it)" : "auto-commit",
                 c->tx_state == 1 ? "\nA transaction is open, as reported by the driver" : c->tx_state < 0 ? "\nState not reported by this driver" : "");
        return;
    case IK_RUN:
        snprintf(out, n, "Run  %s\nThe selection when there is one, else the statement under the caret, on %s%s%s", sc, c->has_conn ? c->conn_name : "(no connection)",
                 en ? "" : "\nUnavailable: ", en ? "" : why);
        return;
    case IK_STOP: {
        if (c->running) { char clk[16]; fmt_clock(c->elapsed_ms, clk, sizeof clk); snprintf(out, n, "Stop  %s\nCancels execution #%d on %s (%s); other sessions keep running%s", commands_shortcut("sql.cancel"), c->exec_id, c->conn_name, clk, c->running_total > 1 ? "\nThe menu lists every running execution" : ""); }
        else snprintf(out, n, "%d execution(s) running in other sessions: the menu lists them; this tab's connection is idle", c->running_total);
        return;
    }
    case IK_MORE: snprintf(out, n, "More: actions that do not fit, and the toolbar settings"); return;
    default: break;
    }
    if (!strcmp(it->id, "workspace")) { snprintf(out, n, "Workspace: %s\nRecent, open, new and close", t->ws_name); return; }
    if (!strcmp(it->id, "panels")) { snprintf(out, n, "Panels: show or hide Explorer, Results, Output, History and the other registered panels"); return; }
    const Command *cm = it->cmd ? commands_find(it->cmd) : NULL;
    snprintf(out, n, "%s%s%s%s%s", cm ? cm->label : it->label, sc[0] ? "  " : "", sc, en ? "" : "\nUnavailable: ", en ? "" : why);
    (void)a;
}

/* ---- menus --------------------------------------------------------------------------- */

static char *mt(Toolbar *t) { if (t->nmtext >= 64) t->nmtext = 0; return t->mtext[t->nmtext++]; }

/* Adds a command entry (label, shortcut, enabled state from the registry). */
static void add_cmd(Toolbar *t, PopupMenu *m, const char *cmd, const char *label, bool sep) {
    if (t->nentries >= 64) return;
    const Command *c = commands_find(cmd);
    if (!c) return;
    char why[160]; bool en = commands_enabled(cmd, why, sizeof why);
    char *buf = mt(t);
    const char *sc = commands_shortcut(cmd);
    snprintf(buf, 112, "%s%s%s", label ? label : c->label, sc[0] ? "    " : "", sc);
    TbEntry *e = &t->entries[t->nentries]; e->kind = EN_CMD; e->cmd = c->id; e->arg = 0;
    popup_add(m, t->nentries++, buf, sep, en);
}
static void add_entry(Toolbar *t, PopupMenu *m, int kind, int arg, const char *text, bool sep, bool en) {
    if (t->nentries >= 64) return;
    char *buf = mt(t); snprintf(buf, 112, "%s", text);
    TbEntry *e = &t->entries[t->nentries]; e->kind = kind; e->cmd = NULL; e->arg = arg; snprintf(e->text, sizeof e->text, "%s", text);
    popup_add(m, t->nentries++, buf, sep, en);
}
static void open_menu(Toolbar *t, App *a, int item, float x, float y) {
    (void)a;
    t->nentries = 0; t->nmtext = 0; t->menu_item = item;
    popup_open(&t->menu, x, y);
}

/* Schemas known from the catalog the Explorer already loaded (schema.table names): no query. */
static int catalog_schemas(App *a, const char *conn_id, char out[][64], int max) {
    Connection *c = tb_conn_by_id(a, conn_id);
    int n = 0;
    for (int i = 0; c && i < c->ntables && n < max; i++) {
        const char *dot = strchr(c->tables[i].name, '.');
        if (!dot) continue;
        char s[64]; snprintf(s, sizeof s, "%.*s", (int)(dot - c->tables[i].name), c->tables[i].name);
        bool dup = false; for (int k = 0; k < n; k++) if (!strcmp(out[k], s)) dup = true;
        if (!dup) snprintf(out[n++], 64, "%s", s);
    }
    return n;
}

static void fill_dropdown(Toolbar *t, App *a, int item, const LumeCtx *c) {
    const TbItem *it = &t->items[item];
    PopupMenu *m = &t->menu;
    if (!strcmp(it->id, "workspace")) {
        popup_add_caption(m, "WORKSPACE", 0, false);
        add_cmd(t, m, "workspace.new", NULL, false);
        add_cmd(t, m, "workspace.open", NULL, false);
        /* recent workspaces: names only, never full paths or credentials (TBR-004) */
        char path[1100]; snprintf(path, sizeof path, "%srecent_workspaces.txt", a->conns ? a->conns->dir : "");
        FILE *f = fopen(path, "r"); int k = 0;
        if (f) {
            char line[64];
            while (k < 5 && fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; if (!line[0]) continue; char lbl[96]; snprintf(lbl, sizeof lbl, "%s%s", line, !strcmp(line, t->ws_name) ? "  (current)" : ""); add_entry(t, m, EN_RECENT_WS, k, lbl, k == 0, strcmp(line, t->ws_name) != 0); snprintf(t->entries[t->nentries - 1].text, 64, "%s", line); k++; }
            fclose(f);
        }
        add_cmd(t, m, "workspace.settings", NULL, true);
        add_cmd(t, m, "workspace.close", NULL, false);
    } else if (it->kind == IK_SCHEMA) {
        if (c->caps && c->caps->attached_dbs) {
            /* SQLite: no fictional schema; the database file and attached ones, shown as such */
            popup_add_caption(m, "SQLITE DATABASES  \xC2\xB7  NO SCHEMAS", 0, false);
            add_entry(t, m, EN_SCHEMA, -1, "main  \xC2\xB7  this database file", false, false);
            char sch[16][64]; int ns = catalog_schemas(a, c->conn_id, sch, 16);
            for (int i = 0; i < ns; i++) { char l[96]; snprintf(l, sizeof l, "%s  \xC2\xB7  attached database", sch[i]); add_entry(t, m, EN_SCHEMA, -1, l, false, false); }
        } else if (c->caps && c->caps->schemas) {
            popup_add_caption(m, "DEFAULT SCHEMA OF THIS SESSION", 0, false);
            add_entry(t, m, EN_SCHEMA_DEFAULT, 0, c->schema[0] ? "Server default (reset)" : "Server default  (current)", false, c->schema[0] && c->connected && !c->running);
            char sch[48][64]; int ns = catalog_schemas(a, c->conn_id, sch, 48);
            for (int i = 0; i < ns; i++) {
                char l[96]; snprintf(l, sizeof l, "%s%s", sch[i], !strcmp(sch[i], c->schema) ? "  (current)" : "");
                add_entry(t, m, EN_SCHEMA, i, l, i == 0, c->connected && !c->running && strcmp(sch[i], c->schema) != 0);
                snprintf(t->entries[t->nentries - 1].text, 64, "%s", sch[i]);
            }
            if (!ns) popup_add_caption(m, "No schema found in the loaded catalog", 0, false);
        } else popup_add_caption(m, "This driver has no schema to choose", 0, false);
    } else if (it->kind == IK_TX) {
        popup_add_caption(m, c->tx_state == 1 ? "TRANSACTION OPEN (DRIVER REPORT)" : "TRANSACTION MODE", 0, false);
        add_cmd(t, m, "tx.auto", c->manual_tx ? "Auto-commit" : "Auto-commit  (current)", false);
        add_cmd(t, m, "tx.manual", c->manual_tx ? "Manual  (current)" : "Manual", false);
        add_cmd(t, m, "tx.commit", NULL, true);
        add_cmd(t, m, "tx.rollback", NULL, false);
        if (c->caps && c->caps->isolation && c->caps->isolation_levels) {
            popup_add_caption(m, "ISOLATION LEVEL (NEXT TRANSACTIONS)", 0, true);
            const char *p = c->caps->isolation_levels; int k = 0;
            while (*p && k < 8) {
                const char *e = strchr(p, '|'); int l = e ? (int)(e - p) : (int)strlen(p);
                char lv[40]; snprintf(lv, sizeof lv, "%.*s", l, p);
                Connection *cc = tb_conn_by_id(a, c->conn_id);
                char lbl[64]; snprintf(lbl, sizeof lbl, "%s%s", lv, cc && !strcmp(cc->isolation, lv) ? "  (current)" : "");
                add_entry(t, m, EN_ISOLATION, k++, lbl, false, c->connected && !c->running && c->tx_state != 1);
                snprintf(t->entries[t->nentries - 1].text, 64, "%s", lv);
                p = e ? e + 1 : p + l;
            }
        }
        add_cmd(t, m, "ds.session", NULL, true);
    } else if (it->kind == IK_RUN) {
        char all[64]; snprintf(all, sizeof all, c->statements > 0 ? "Run All in File (%d statement%s)" : "Run All in File", c->statements, c->statements == 1 ? "" : "s");
        add_cmd(t, m, "sql.run", "Run Statement Under Caret", false);
        add_cmd(t, m, "sql.run_selection", NULL, false);
        add_cmd(t, m, "sql.run_file", all, false);
        add_cmd(t, m, "sql.run_new_tab", NULL, false);
        add_cmd(t, m, "sql.explain", "Explain Plan\xE2\x80\xA6", true);
        if (c->caps && c->caps->explain_analyze) add_cmd(t, m, "sql.explain_analyze", NULL, false);
        add_cmd(t, m, "sql.run_params", NULL, false);
        add_cmd(t, m, "sql.run_to_file", NULL, false);
        add_cmd(t, m, "sql.rerun_last", NULL, true);
        add_cmd(t, m, "sql.exec_settings", NULL, false);
    } else if (it->kind == IK_STOP) {
        popup_add_caption(m, "RUNNING EXECUTIONS", 0, false);
        OutRunInfo ri;
        for (int k = 0; k < 16 && output_running_info(a->output, k, &ri); k++) {
            char clk[16]; fmt_clock(ri.elapsed_ms, clk, sizeof clk);
            char l[112]; snprintf(l, sizeof l, "Stop #%d on %s  \xC2\xB7  %s%s", ri.exec_id, ri.conn_name, clk, ri.cancelling ? "  \xC2\xB7  cancelling" : "");
            add_entry(t, m, EN_CANCEL_EXEC, ri.exec_id, l, false, !ri.cancelling);
        }
    } else if (!strcmp(it->id, "panels")) {
        /* TBR-036: every registered panel with its real state; the Panel Manager does the work */
        for (int p = 0; p < panels_panel_count(a->panels); p++) {
            PanelInfo pi; if (!panels_panel_info(a->panels, a, p, &pi)) continue;
            char l[112]; snprintf(l, sizeof l, "%s  \xC2\xB7  %s%s%s%s", pi.title, pi.state,
                                   pi.badge.count ? "  \xC2\xB7  new" : "", pi.badge.error ? "  \xC2\xB7  error" : "", pi.visible && strcmp(pi.state, "collapsed") ? "  \xE2\x9C\x93" : "");
            add_entry(t, m, EN_PANEL, p, l, false, true);
        }
        add_cmd(t, m, "layout.reset", NULL, true);
        add_cmd(t, m, "panel.configure", NULL, false);
    } else if (it->kind == IK_MORE) {
        /* overflowed items first, with their state; then the toolbar's own settings */
        if (t->noverflow) popup_add_caption(m, "MORE ACTIONS", 0, false);
        for (int k = 0; k < t->noverflow; k++) {
            const TbItem *oi = &t->items[t->overflow[k]];
            char txt[160]; item_text(t, a, oi, c, txt, sizeof txt);
            if (oi->cmd) add_cmd(t, m, oi->cmd, NULL, false);
            else { char l[112]; snprintf(l, sizeof l, "%s\xE2\x80\xA6", oi->kind == IK_DROPDOWN && !strcmp(oi->id, "workspace") ? "Workspace" : txt); add_entry(t, m, EN_OVERFLOW, t->overflow[k], l, false, true); }   /* opens on click, not on hover */
        }
        popup_add_caption(m, "TOOLBAR", 0, t->noverflow > 0);
        add_cmd(t, m, "toolbar.customize", NULL, false);
        add_cmd(t, m, "toolbar.compact", t->prefs.compact ? "Compact Mode  \xE2\x9C\x93" : "Compact Mode", false);
        add_cmd(t, m, "toolbar.labels", t->prefs.labels ? "Show Labels  \xE2\x9C\x93" : "Show Labels", false);
        add_cmd(t, m, "toolbar.toggle", "Hide Toolbar", false);
        add_cmd(t, m, "toolbar.reset", NULL, false);
    }
}

/* The toolbar's own context menu (TBR-038): only settings, never an SQL command. */
static void open_context_menu(Toolbar *t, App *a, float x, float y) {
    open_menu(t, a, -2, x, y);
    add_cmd(t, &t->menu, "toolbar.customize", NULL, false);
    add_cmd(t, &t->menu, "toolbar.customize", "Add Action\xE2\x80\xA6", false);
    t->entries[t->nentries - 1].arg = 1;   /* opens the dialog on "add" */
    add_cmd(t, &t->menu, "toolbar.toggle", "Hide Toolbar", true);
    add_cmd(t, &t->menu, "toolbar.compact", t->prefs.compact ? "Compact Mode  \xE2\x9C\x93" : "Compact Mode", false);
    add_cmd(t, &t->menu, "toolbar.labels", t->prefs.labels ? "Show Labels  \xE2\x9C\x93" : "Show Labels", false);
    add_cmd(t, &t->menu, "toolbar.reset", NULL, true);
}

/* Runs a chosen popup entry. */
static void menu_choice(Toolbar *t, App *a, int id) {
    if (id < 0 || id >= t->nentries) return;
    TbEntry e = t->entries[id];
    popup_close(&t->menu);
    int item = t->menu_item; t->menu_item = -1;
    switch (e.kind) {
    case EN_CMD:
        if (!strcmp(e.cmd, "toolbar.customize") && e.arg == 1) t->cust_preset_view = 1;
        commands_invoke(a, e.cmd);
        break;
    case EN_SCHEMA: tba_set_schema(t, a, e.text); break;
    case EN_SCHEMA_DEFAULT: tba_set_schema(t, a, NULL); break;
    case EN_ISOLATION: tba_set_isolation(t, a, e.text); break;
    case EN_CANCEL_EXEC: output_cancel_exec(a->output, a, e.arg); break;
    case EN_PANEL: panels_panel_toggle(a->panels, a, e.arg); break;
    case EN_RECENT_WS: tba_ws_request(t, a, e.text); break;
    case EN_OVERFLOW: {
        /* a dropdown that lives in "...": open it where the "..." button is */
        const TbItem *oi = &t->items[e.arg];
        Rect at = t->bar;
        for (int s = 0; s < t->nslots; s++) if (t->items[t->slots[s].item].kind == IK_MORE) at = t->slots[s].r;
        if (oi->kind == IK_DATASOURCE) { t->ds_open = true; t->ds_rect = rect_make(at.x + at.w - px(a, 340), at.y + at.h + px(a, 2), px(a, 340), 0); textfield_set(&t->ds_search, ""); t->ds_sel = 0; }
        else { open_menu(t, a, e.arg, at.x + at.w - px(a, 260), at.y + at.h + px(a, 2)); fill_dropdown(t, a, e.arg, commands_ctx()); }
        break;
    }
    default: break;
    }
    (void)item;
}

bool toolbar_menu_pos(Toolbar *t, App *a, const char *label, float *x, float *y) {
    Rect rc;
    if (!t || !t->menu.open || !popup_item_rect(a, &t->menu, label, &rc)) return false;
    *x = (rc.x + rc.w / 2) / a->density; *y = (rc.y + rc.h / 2) / a->density;
    return true;
}

/* ---- datasource popover (TBR-008 / 009 / 011) -------------------------------------------- */

/* Connections matching the search, recent ones first, then grouped by driver. */
static void ds_build_rows(Toolbar *t, App *a) {
    t->nds_rows = 0;
    const char *q = t->ds_search.buf;
    bool used[256] = { 0 };
    /* recent */
    for (int r = 0; r < t->prefs.nrecent && t->nds_rows < 64; r++)
        for (int i = 0; i < a->conns->n && i < 256; i++)
            if (!strcmp(a->conns->items[i]->id, t->prefs.recent[r]) && (!q[0] || strcasestr(a->conns->items[i]->name, q))) { t->ds_rows[t->nds_rows++] = i; used[i] = true; }
    /* the rest, grouped by driver */
    const char *drivers[] = { "sqlite", "postgres", "oracle", NULL };
    for (int d = 0; drivers[d]; d++)
        for (int i = 0; i < a->conns->n && i < 256 && t->nds_rows < 64; i++) {
            Connection *c = a->conns->items[i];
            if (used[i] || strcmp(c->driver, drivers[d])) continue;
            if (q[0] && !strcasestr(c->name, q) && !strcasestr(c->driver, q)) continue;
            t->ds_rows[t->nds_rows++] = i; used[i] = true;
        }
    if (t->ds_sel >= t->nds_rows) t->ds_sel = t->nds_rows ? t->nds_rows - 1 : 0;
}

/* Status mark of a connection, drawn (the font has no reliable circle glyph): filled = connected,
 * ring = connecting / disconnected, cross = failed. Always next to the state in words. */
static void status_mark(App *a, int state, float x, float cy) {
    SDL_Renderer *r = a->renderer; float d = px(a, 9);
    Rect rc = rect_make(x, cy - d / 2, d, d);
    switch (state) {
    case CONN_CONNECTED: draw_rrect(r, rc, d / 2, color_hex(T_GREEN)); break;
    case CONN_CONNECTING: case CONN_RECONNECTING: draw_rrect(r, rc, d / 2, color_hex(T_YELLOW)); draw_rrect(r, rect_inset(rc, px(a, 2), px(a, 2)), d / 2 - px(a, 2), color_hex(T_BG)); break;
    case CONN_FAILED: draw_cross(r, x + d / 2, cy, d, px(a, 1.8f), color_hex(T_RED)); break;
    default: draw_rrect(r, rc, d / 2, color_hex(T_TEXT_DIM)); draw_rrect(r, rect_inset(rc, px(a, 2), px(a, 2)), d / 2 - px(a, 2), color_hex(T_BG)); break;
    }
}


/* Input of the popover (runs in toolbar_frame); actions rows follow the connection rows. */
enum { DSA_CONNECT = 1000, DSA_DISCONNECT, DSA_CANCEL, DSA_SETTINGS, DSA_NEW };
static int ds_actions(const LumeCtx *c, int *out) {
    int n = 0;
    if (c->has_conn) {
        if (c->conn_state == CONN_CONNECTING || c->conn_state == CONN_RECONNECTING) out[n++] = DSA_CANCEL;
        else { out[n++] = DSA_CONNECT; if (c->connected) out[n++] = DSA_DISCONNECT; }
        out[n++] = DSA_SETTINGS;
    }
    out[n++] = DSA_NEW;
    return n;
}
static const char *ds_action_label(int k, const LumeCtx *c) {
    switch (k) {
    case DSA_CONNECT: return c->connected || c->conn_state == CONN_FAILED ? "Reconnect" : "Connect";
    case DSA_DISCONNECT: return "Disconnect";
    case DSA_CANCEL: return "Cancel Connect";
    case DSA_SETTINGS: return "Open Connection Settings\xE2\x80\xA6";
    default: return "New Connection\xE2\x80\xA6";
    }
}
static void ds_do_action(Toolbar *t, App *a, int k, const LumeCtx *c) {
    t->ds_open = false;
    switch (k) {
    case DSA_CONNECT: commands_invoke(a, c->connected || c->conn_state == CONN_FAILED ? "ds.reconnect" : "ds.connect"); break;
    case DSA_DISCONNECT: commands_invoke(a, "ds.disconnect"); break;
    case DSA_CANCEL: commands_invoke(a, "ds.cancel_connect"); break;
    case DSA_SETTINGS: commands_invoke(a, "ds.settings"); break;
    default: commands_invoke(a, "ds.new"); break;
    }
}

static void ds_input(Toolbar *t, App *a, const LumeCtx *c) {
    Input *in = &a->in;
    ds_build_rows(t, a);
    int acts[8]; int na = ds_actions(c, acts);
    int total = t->nds_rows + na;
    for (int k = 0; k < in->nkeys; k++) {
        KeyPress *kp = &in->keys[k];
        if (kp->used) continue;
        if (kp->key == SDLK_ESCAPE) { t->ds_open = false; kp->used = true; return; }
        if (kp->key == SDLK_DOWN) { t->ds_sel = (t->ds_sel + 1) % (total ? total : 1); kp->used = true; }
        else if (kp->key == SDLK_UP) { t->ds_sel = (t->ds_sel - 1 + total) % (total ? total : 1); kp->used = true; }
        else if (kp->key == SDLK_RETURN || kp->key == SDLK_KP_ENTER) {
            kp->used = true;
            if (t->ds_sel < t->nds_rows) { t->ds_open = false; tba_choose_datasource(t, a, a->conns->items[t->ds_rows[t->ds_sel]]); return; }
            if (t->ds_sel - t->nds_rows < na) { ds_do_action(t, a, acts[t->ds_sel - t->nds_rows], c); return; }
        }
    }
    /* layout: title, search, rows, actions */
    float rh = px(a, 26), w = t->ds_rect.w, x = t->ds_rect.x, y = t->ds_rect.y;
    float h = px(a, 30) + px(a, 36) + rh * (float)t->nds_rows + px(a, 8) + rh * (float)na + px(a, 10);
    if (y + h > a->height - px(a, 8)) h = a->height - px(a, 8) - y;
    t->ds_rect.h = h;
    Rect search = rect_make(x + px(a, 10), y + px(a, 32), w - px(a, 20), px(a, 28));
    ui_textfield(a, &t->ds_search, search, "Search connections", true);
    float ry = y + px(a, 68);
    for (int i = 0; i < t->nds_rows + na; i++) {
        if (i == t->nds_rows) ry += px(a, 8);
        Rect rr = rect_make(x + px(a, 4), ry, w - px(a, 8), rh);
        t->ds_row_rect[i < 64 ? i : 63] = rr;
        char tg[96];
        if (i < t->nds_rows) snprintf(tg, sizeof tg, "ds:%s", a->conns->items[t->ds_rows[i]]->name);
        else snprintf(tg, sizeof tg, "ds-action:%s", ds_action_label(acts[i - t->nds_rows], c));
        ui_record(tg, rr);
        if (rect_contains(rr, in->mouse_x, in->mouse_y)) {
            t->ds_sel = i;
            if (in->mouse_pressed && !in->mouse_consumed) {
                in->mouse_consumed = true;
                if (i < t->nds_rows) { t->ds_open = false; tba_choose_datasource(t, a, a->conns->items[t->ds_rows[i]]); }
                else ds_do_action(t, a, acts[i - t->nds_rows], c);
                return;
            }
        }
        ry += rh;
    }
    if (in->mouse_pressed && !in->mouse_consumed && !rect_contains(t->ds_rect, in->mouse_x, in->mouse_y)) { in->mouse_consumed = true; t->ds_open = false; }
    /* the popover owns the keys and the mouse while open */
    in->nkeys = 0; in->ntext = 0;
    if (rect_contains(t->ds_rect, in->mouse_x, in->mouse_y)) { in->mouse_consumed = true; in->wheel_y = 0; }
}

static void ds_draw(Toolbar *t, App *a, const LumeCtx *c) {
    SDL_Renderer *r = a->renderer;
    Rect b = t->ds_rect;
    draw_rrect(r, rect_inset(b, -px(a, 1), -px(a, 1)), px(a, 7), color_hex(T_BORDER));
    draw_rrect(r, b, px(a, 6), color_hex(T_PANEL_ALT));
    char head[160];
    if (c->has_conn) snprintf(head, sizeof head, "Active for this tab: %s (%s)", c->conn_name, c->caps ? c->caps->name : c->driver);
    else snprintf(head, sizeof head, "This tab has no connection yet");
    draw_text_fit(a->font_small, r, head, -1, rect_make(b.x + px(a, 12), b.y + px(a, 6), b.w - px(a, 24), px(a, 22)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    Rect search = rect_make(b.x + px(a, 10), b.y + px(a, 32), b.w - px(a, 20), px(a, 28));
    ui_textfield(a, &t->ds_search, search, "Search connections", true);
    int acts[8]; int na = ds_actions(c, acts);
    clip_push(r, b);
    for (int i = 0; i < t->nds_rows + na; i++) {
        Rect rr = t->ds_row_rect[i < 64 ? i : 63];
        if (i == t->ds_sel) draw_rrect(r, rr, px(a, 4), color_hex(T_SELECTED));
        if (i < t->nds_rows) {
            Connection *cc = a->conns->items[t->ds_rows[i]];
            status_mark(a, cc->db ? CONN_CONNECTED : cc->state, rr.x + px(a, 10), rr.y + rr.h / 2);
            const DriverCaps *k = caps_for(cc->driver);
            char nm[160]; snprintf(nm, sizeof nm, "%s%s", cc->name, !strcmp(cc->id, c->conn_id) ? "  (this tab)" : "");
            draw_text_fit(a->font_ui, r, nm, -1, rect_make(rr.x + px(a, 28), rr.y, rr.w - px(a, 150), rr.h), color_hex(T_TEXT), ALIGN_LEFT);
            char right[96]; snprintf(right, sizeof right, "%s%s%s  \xC2\xB7  %s", cc->env ? conn_env_name(cc->env) : "", cc->env ? "  " : "", k ? k->name : cc->driver, conn_state_name(cc->db ? CONN_CONNECTED : cc->state));
            draw_text_fit(a->font_small, r, right, -1, rect_make(rr.x + rr.w - px(a, 220), rr.y, px(a, 212), rr.h), color_hex(cc->env ? conn_env_color(cc) : T_TEXT_DIM), ALIGN_RIGHT);
            if (i < t->prefs.nrecent && i == 0 && !t->ds_search.len) draw_text_fit(a->font_small, r, "recent", -1, rect_make(rr.x + rr.w - px(a, 270), rr.y, px(a, 44), rr.h), color_hex(T_TEXT_DIM), ALIGN_RIGHT);
        } else {
            if (i == t->nds_rows) draw_rect(r, rect_make(b.x + px(a, 8), rr.y - px(a, 5), b.w - px(a, 16), px(a, 1)), color_hex(T_BORDER));
            draw_text_fit(a->font_ui, r, ds_action_label(acts[i - t->nds_rows], c), -1, rect_make(rr.x + px(a, 28), rr.y, rr.w - px(a, 36), rr.h), color_hex(T_TEXT), ALIGN_LEFT);
        }
    }
    if (c->conn_state == CONN_FAILED && c->error[0]) {
        char e[220]; snprintf(e, sizeof e, "Last failure: %s", c->error);
        draw_text_fit(a->font_small, r, e, -1, rect_make(b.x + px(a, 12), b.y + b.h - px(a, 20), b.w - px(a, 24), px(a, 18)), color_hex(T_RED), ALIGN_LEFT);
    }
    clip_pop(r);
}

/* ---- the bar ------------------------------------------------------------------------- */

/* Opens the dropdown / popover of slot s (keyboard Down / Enter, or a click on its chevron). */
static void open_slot(Toolbar *t, App *a, int s, const LumeCtx *c) {
    TbSlot *sl = &t->slots[s]; const TbItem *it = &t->items[sl->item];
    if (it->kind == IK_DATASOURCE) {
        t->ds_open = true; t->ds_sel = 0; textfield_set(&t->ds_search, "");
        float w = px(a, 380);
        float x = fminf(sl->r.x, a->width - w - px(a, 8));
        t->ds_rect = rect_make(x, sl->r.y + sl->r.h + px(a, 4), w, 0);
        return;
    }
    if (it->kind == IK_BUTTON) { if (it->cmd) commands_invoke(a, it->cmd); return; }
    float x = it->kind == IK_MORE ? sl->r.x + sl->r.w - px(a, 280) : sl->r.x;
    open_menu(t, a, sl->item, fmaxf(px(a, 4), x), sl->r.y + sl->r.h + px(a, 4));
    fill_dropdown(t, a, sl->item, c);
}

void tb_open_datasource(Toolbar *t, App *a) {
    Rect at = rect_make(t->bar.x + px(a, 8), t->bar.y, px(a, 200), t->bar.h);
    for (int s = 0; s < t->nslots; s++) if (t->items[t->slots[s].item].kind == IK_DATASOURCE) at = t->slots[s].r;
    t->ds_open = true; t->ds_sel = 0; textfield_set(&t->ds_search, "");
    float w = px(a, 380);
    t->ds_rect = rect_make(fminf(at.x, a->width - w - px(a, 8)), at.y + at.h + px(a, 4), w, 0);
}

/* Activates slot s (click on its body, Enter / Space from the keyboard). */
static void activate_slot(Toolbar *t, App *a, int s, const LumeCtx *c, bool on_chevron) {
    const TbItem *it = &t->items[t->slots[s].item];
    switch (it->kind) {
    case IK_RUN: if (on_chevron) open_slot(t, a, s, c); else commands_invoke(a, "sql.run"); break;
    case IK_STOP:
        if (on_chevron || !c->running) open_slot(t, a, s, c);
        else commands_invoke(a, "sql.cancel");
        break;
    case IK_BUTTON: if (it->cmd) commands_invoke(a, it->cmd); break;
    case IK_FLEX: break;
    default: open_slot(t, a, s, c); break;
    }
}

static void draw_slot(Toolbar *t, App *a, int s, const LumeCtx *c, bool hov, bool foc) {
    SDL_Renderer *r = a->renderer;
    TbSlot *sl = &t->slots[s]; const TbItem *it = &t->items[sl->item];
    if (it->kind == IK_FLEX) return;
    char why[200]; bool en = item_enabled(it, why, sizeof why);
    bool open = (t->menu.open && t->menu_item == sl->item) || (t->ds_open && it->kind == IK_DATASOURCE);
    Color text = color_hex(en ? T_TEXT : T_TEXT_DIM);
    /* background: Run is the primary action; hover / open / pressed states */
    if (it->kind == IK_RUN && en && !c->running) draw_rrect(r, sl->r, px(a, M_RADIUS), color_hex(hov ? T_ACCENT_HOVER : T_ACCENT));
    else if (it->kind == IK_STOP) draw_rrect(r, sl->r, px(a, M_RADIUS), color_alpha(color_hex(T_RED), hov ? 70 : 45));
    else if (hov || open) draw_rrect(r, sl->r, px(a, M_RADIUS), color_hex(T_HOVER));
    if (it->kind == IK_RUN && en && !c->running) text = color_hex(T_ON_ACCENT);
    if (it->kind == IK_STOP) text = color_hex(T_RED);
    if (it->kind == IK_RUN && c->running) draw_rrect(r, sl->r, px(a, M_RADIUS), color_alpha(color_hex(T_ACCENT), 50));
    float pad = px(a, t->prefs.compact ? 6 : 8), x = sl->r.x + pad, cy = sl->r.y + sl->r.h / 2, isz = px(a, 15);
    /* datasource: status glyph (with text), environment and read-only pills */
    if (it->kind == IK_DATASOURCE) {
        status_mark(a, c->has_conn ? c->conn_state : CONN_DISCONNECTED, x, cy);
        x += px(a, 14);
        if (c->env) {
            char e[8]; snprintf(e, sizeof e, "%s", conn_env_name(c->env));
            Rect pill = rect_make(x, cy - px(a, 9), px(a, 38), px(a, 18));
            draw_rrect(r, pill, px(a, 4), color_alpha(color_hex(c->env_color), 60));
            draw_text_fit(a->font_small, r, e, -1, pill, color_hex(c->env_color), ALIGN_CENTER);
            x += px(a, 44);
        }
        if (c->read_only) {
            Rect pill = rect_make(x, cy - px(a, 9), px(a, 66), px(a, 18));
            draw_rrect(r, pill, px(a, 4), color_alpha(color_hex(T_YELLOW), 40));
            draw_text_fit(a->font_small, r, "Read-only", -1, pill, color_hex(T_YELLOW), ALIGN_CENTER);
            x += px(a, 72);
        }
    } else if (it->icon) {
        Color ic = text;
        if (it->kind == IK_RUN && c->running && !a->reduce_motion) {
            /* a light spinner: one dot going round, redrawn only while running (TBR-021) */
            float ang = (float)(SDL_GetTicks() % 1000) / 1000.0f * 6.2832f;
            draw_rrect(r, rect_make(x + isz / 2 + cosf(ang) * isz * 0.35f - px(a, 2), cy + sinf(ang) * isz * 0.35f - px(a, 2), px(a, 4), px(a, 4)), px(a, 2), color_hex(T_ACCENT));
            app_wake_in(a, 50);
        } else tb_icon(a, it->kind == IK_RUN && c->running ? "run" : it->icon, x + isz / 2, cy, isz, ic);
        x += isz + px(a, 4);
    }
    if (!sl->short_form || it->kind == IK_DATASOURCE) {
        char txt[160]; item_text(t, a, it, c, txt, sizeof txt);
        float right = (sl->chev.w > 0 ? sl->chev.x : sl->r.x + sl->r.w - pad);
        draw_text_fit(a->font_ui, r, txt, -1, rect_make(x, sl->r.y, right - x, sl->r.h), text, ALIGN_LEFT);
        if (it->kind == IK_DATASOURCE && c->caps && sl->r.w > px(a, 200)) { /* the driver name, dim, when there is room */ }
    }
    if (sl->chev.w > 0) {
        if (it->kind == IK_RUN || it->kind == IK_STOP) draw_rect(r, rect_make(sl->chev.x, sl->r.y + px(a, 5), px(a, 1), sl->r.h - px(a, 10)), color_alpha(text, 90));
        draw_chevron(r, sl->chev.x + sl->chev.w / 2, cy, px(a, 7), px(a, 1.5f), true, text);
    }
    /* badges on Panels (TBR-033): new Output events / an error, with a sign, not only a colour */
    if (!strcmp(it->id, "panels")) {
        for (int p = 0; p < panels_panel_count(a->panels); p++) {
            PanelInfo pi; if (!panels_panel_info(a->panels, a, p, &pi) || strcmp(pi.id, "lume.data.output") || pi.visible) continue;
            if (pi.badge.count || pi.badge.error) {
                Rect bd = rect_make(sl->r.x + sl->r.w - px(a, 14), sl->r.y + px(a, 2), px(a, 12), px(a, 12));
                draw_rrect(r, bd, px(a, 6), color_hex(pi.badge.error ? T_RED : T_ACCENT));
                draw_text_fit(a->font_small, r, pi.badge.error ? "!" : "\xE2\x80\xA2", -1, bd, color_hex(0xFFFFFF), ALIGN_CENTER);
            }
        }
    }
    if (foc) draw_rect_outline(r, rect_inset(sl->r, -px(a, 2), -px(a, 2)), px(a, 2), color_hex(T_ACCENT));   /* visible keyboard focus */
}

void toolbar_frame(Toolbar *t, App *a, Rect bar) {
    if (t->items_version != commands_version()) { tb_rebuild_items(t); if (t->menu.open) popup_close(&t->menu); }   /* a module came or went (TBR-050) */
    commands_frame(a);
    const LumeCtx *c = commands_ctx();
    if (t->dirty && SDL_GetTicks() - t->dirty_at > 500) tb_save_prefs(t, a);   /* coalesced save */
    Input *in = &a->in;
    /* dialogs first: they own the input while open */
    tba_modal_input(t, a);
    if (t->ds_open) ds_input(t, a, c);
    if (t->menu.open) { int id = popup_update(a, &t->menu); if (id >= 0) menu_choice(t, a, id); if (!t->menu.open && t->kb_mode) t->focus = t->focus < 0 ? 0 : t->focus; }
    t->bar = bar;
    if (!t->prefs.visible || bar.h <= 0) { t->nslots = 0; return; }
    SDL_Renderer *r = a->renderer;
    draw_rect(r, bar, color_hex(T_BG));
    draw_rect(r, rect_make(bar.x, bar.y + bar.h - px(a, 1), bar.w, px(a, 1)), color_hex(T_BORDER));
    layout(t, a, bar, c);
    /* keyboard: the bar has the focus (TBR-047): Tab / arrows move, Enter / Space activate,
     * Down opens a dropdown, Esc gives the focus back */
    if (t->kb_mode && !t->menu.open && !t->ds_open && t->modal == TM_NONE) {
        if (t->focus >= t->nslots) t->focus = t->nslots - 1;
        for (int k = 0; k < in->nkeys; k++) {
            KeyPress *kp = &in->keys[k];
            if (kp->used) continue;
            int dir = 0;
            if (kp->key == SDLK_RIGHT || (kp->key == SDLK_TAB && !(kp->mods & KM_SHIFT))) dir = 1;
            else if (kp->key == SDLK_LEFT || (kp->key == SDLK_TAB && (kp->mods & KM_SHIFT))) dir = -1;
            if (dir) {
                for (int step = 0; step < t->nslots; step++) { t->focus = (t->focus + dir + t->nslots) % t->nslots; if (t->items[t->slots[t->focus].item].kind != IK_FLEX) break; }
                kp->used = true; continue;
            }
            if (kp->key == SDLK_ESCAPE) { t->kb_mode = false; t->focus = -1; a->focus = t->focus_return; kp->used = true; continue; }
            if (t->focus >= 0 && (kp->key == SDLK_RETURN || kp->key == SDLK_SPACE || kp->key == SDLK_KP_ENTER)) { kp->used = true; activate_slot(t, a, t->focus, c, false); continue; }
            if (t->focus >= 0 && kp->key == SDLK_DOWN) { kp->used = true; open_slot(t, a, t->focus, c); if (t->menu.open) t->menu.kb = popup_step(&t->menu, -1, 1); continue; }
        }
        in->ntext = 0;   /* no typing into the editor while the bar has the focus */
    }
    /* mouse */
    int hov = -1;
    bool overlay_open = t->menu.open || t->ds_open || t->modal != TM_NONE;
    for (int s = 0; s < t->nslots; s++) {
        const TbItem *it = &t->items[t->slots[s].item];
        char tg[64]; snprintf(tg, sizeof tg, "tb:%s", it->custom ? it->id + 4 : it->id);
        if (it->kind != IK_FLEX) ui_record(tg, t->slots[s].r);
        if (it->kind == IK_RUN || it->kind == IK_STOP) { snprintf(tg, sizeof tg, "tb:menu:%s", it->id); ui_record(tg, t->slots[s].chev); }
        if (!overlay_open && it->kind != IK_FLEX && rect_contains(t->slots[s].r, in->mouse_x, in->mouse_y)) hov = s;
    }
    if (hov >= 0) {
        a->want_cursor = CURSOR_ARROW;
        if (in->mouse_pressed && !in->mouse_consumed) {
            in->mouse_consumed = true;
            TbSlot *sl = &t->slots[hov];
            bool on_chev = sl->chev.w > 0 && rect_contains(sl->chev, in->mouse_x, in->mouse_y);
            t->hover_since = SDL_GetTicks() + 100000;   /* no tooltip after a click */
            activate_slot(t, a, hov, c, on_chev);
        } else if (in->mouse_rpressed) { in->mouse_rpressed = false; open_context_menu(t, a, in->mouse_x, in->mouse_y); }
    } else if (!overlay_open && rect_contains(bar, in->mouse_x, in->mouse_y)) {
        if (in->mouse_rpressed) { in->mouse_rpressed = false; open_context_menu(t, a, in->mouse_x, in->mouse_y); }
        else if (in->mouse_pressed && !in->mouse_consumed) in->mouse_consumed = true;   /* the empty bar is not the editor */
    }
    if (hov != t->hover) { t->hover = hov; t->hover_since = SDL_GetTicks(); }
    t->tip[0] = 0;
    if (hov >= 0 && SDL_GetTicks() - t->hover_since > 450 && SDL_GetTicks() >= t->hover_since) { item_tooltip(t, a, &t->items[t->slots[hov].item], c, t->tip, sizeof t->tip); t->hover_rect = t->slots[hov].r; }
    else if (hov >= 0 && SDL_GetTicks() < t->hover_since + 460 && SDL_GetTicks() >= t->hover_since) app_wake_in(a, 470);
    if (t->kb_mode && t->focus >= 0 && t->focus < t->nslots) { item_tooltip(t, a, &t->items[t->slots[t->focus].item], c, t->tip, sizeof t->tip); t->hover_rect = t->slots[t->focus].r; }
    for (int s = 0; s < t->nslots; s++) draw_slot(t, a, s, c, s == hov, t->kb_mode && s == t->focus);
    if (c->running) app_wake_in(a, a->reduce_motion ? 1000 : 100);   /* the elapsed time; nothing ticks in idle (TBR-046) */
}

void toolbar_draw_overlay(Toolbar *t, App *a) {
    const LumeCtx *c = commands_ctx();
    SDL_Renderer *r = a->renderer;
    /* tooltip: drawn above, never takes the focus or moves the layout */
    if (t->tip[0] && !t->menu.open && !t->ds_open && t->modal == TM_NONE) {
        float w = 0; int lines = 1; const char *p = t->tip;
        char line[400];
        while (*p) { const char *e = strchr(p, '\n'); int l = e ? (int)(e - p) : (int)strlen(p); snprintf(line, sizeof line, "%.*s", l, p); w = fmaxf(w, font_width(a->font_small, line, -1)); p = e ? e + 1 : p + l; if (e) lines++; }
        w = fminf(w + px(a, 20), px(a, 520));
        float lh = px(a, 16), h = lh * (float)lines + px(a, 10);
        float x = fminf(t->hover_rect.x, a->width - w - px(a, 6)), y = t->hover_rect.y + t->hover_rect.h + px(a, 6);
        draw_rrect(r, rect_make(x, y, w, h), px(a, 5), color_hex(T_BORDER));
        draw_rrect(r, rect_make(x + px(a, 1), y + px(a, 1), w - px(a, 2), h - px(a, 2)), px(a, 4), color_hex(T_PANEL_ALT));
        p = t->tip; float ly = y + px(a, 5);
        while (*p) { const char *e = strchr(p, '\n'); int l = e ? (int)(e - p) : (int)strlen(p); draw_text_fit(a->font_small, r, p, l, rect_make(x + px(a, 10), ly, w - px(a, 20), lh), color_hex(ly == y + px(a, 5) ? T_TEXT : T_TEXT_MUTED), ALIGN_LEFT); ly += lh; p = e ? e + 1 : p + l; }
    }
    if (t->ds_open) ds_draw(t, a, c);
    if (t->menu.open) popup_draw(a, &t->menu);
    tba_modal_draw(t, a);
}

/* ---- harness ----------------------------------------------------------------------------- */

const char *toolbar_state_text(Toolbar *t, App *a) {
    const LumeCtx *c = commands_ctx();
    size_t l = (size_t)snprintf(t->state_line, sizeof t->state_line, "toolbar %s h=%.0f slots=", t->prefs.visible ? "visible" : "hidden", toolbar_height(t, a) / a->density);
    for (int s = 0; s < t->nslots && l < sizeof t->state_line; s++) { const TbItem *it = &t->items[t->slots[s].item]; if (it->kind != IK_FLEX) l += (size_t)snprintf(t->state_line + l, sizeof t->state_line - l, "%s%s%s", s ? "," : "", it->id, t->slots[s].short_form ? "(icon)" : ""); }
    if (l < sizeof t->state_line) l += (size_t)snprintf(t->state_line + l, sizeof t->state_line - l, " overflow=");
    for (int k = 0; k < t->noverflow && l < sizeof t->state_line; k++) l += (size_t)snprintf(t->state_line + l, sizeof t->state_line - l, "%s%s", k ? "," : "", t->items[t->overflow[k]].id);
    /* geometry check (TBR-040): no slot overlaps the next one or leaves the bar */
    int overlap = 0, outside = 0;
    for (int s = 0; s < t->nslots; s++) {
        Rect r1 = t->slots[s].r;
        if (r1.x < t->bar.x - 0.5f || r1.x + r1.w > t->bar.x + t->bar.w + 0.5f || r1.y < t->bar.y - 0.5f || r1.y + r1.h > t->bar.y + t->bar.h + 0.5f) outside++;
        if (s + 1 < t->nslots && r1.x + r1.w > t->slots[s + 1].r.x + 0.5f) overlap++;
    }
    const char *fid = t->kb_mode && t->focus >= 0 && t->focus < t->nslots ? t->items[t->slots[t->focus].item].id : "";
    char tip1[64]; snprintf(tip1, sizeof tip1, "%.60s", t->tip); for (char *p = tip1; *p; p++) if (*p == '\n') *p = '/';
    if (l < sizeof t->state_line) l += (size_t)snprintf(t->state_line + l, sizeof t->state_line - l, " tip=\"%.60s\" hover=%d modal=%d popover=%d menu=%d kb=%d focus=%s overlap=%d outside=%d err=\"%.60s\"", tip1, t->hover, t->modal, t->ds_open, t->menu.open, t->kb_mode, fid, overlap, outside, c->error);
    if (l < sizeof t->state_line) snprintf(t->state_line + l, sizeof t->state_line - l, " ds=\"%s\" state=%s running=%d/%d tx=%d manual=%d schema=\"%s\" preset=%s compact=%d labels=%d ws=\"%s\"",
                                             c->has_conn ? c->conn_name : "", conn_state_name(c->conn_state), c->running, c->running_total, c->tx_state, c->manual_tx, c->schema, tb_preset_name(t->prefs.preset), t->prefs.compact, t->prefs.labels, t->ws_name);
    return t->state_line;
}

bool toolbar_action(Toolbar *t, App *a, int act) { return t ? tba_action(t, a, act) : false; }

/* Harness: the open dropdown's entries with their state; the open dialog's text. */
void toolbar_dump_menu(Toolbar *t) {
    if (!t->menu.open) { printf("menuitem (none open)\n"); fflush(stdout); return; }
    for (int i = 0; i < t->menu.n; i++) printf("menuitem %s%s\n", t->menu.labels[i] ? t->menu.labels[i] : "", t->menu.caption[i] ? "  [caption]" : t->menu.enabled[i] ? "" : "  [disabled]");
    fflush(stdout);
}
void toolbar_dump_modal(Toolbar *t, float scale) {
    char body[2600]; snprintf(body, sizeof body, "%s", t->modal == TM_SESSION ? t->session_text : t->cf_body);
    for (char *p = body; *p; p++) if (*p == '\n') *p = '|';
    printf("tbmodal kind=%d title=\"%s\" body=\"%s\" box=%.0fx%.0f\n", t->modal, t->modal == TM_CONFIRM ? t->cf_title : "", body, t->modal_rect.w / (scale > 0 ? scale : 1), t->modal_rect.h / (scale > 0 ? scale : 1));   /* points: the same on every display */
    fflush(stdout);
}

/* Harness (TBR-044 / 047): the accessible name of every visible control, i.e. the first line of
 * its tooltip, one per line; an empty name is a defect. */
void toolbar_print_names(Toolbar *t, App *a) {
    const LumeCtx *c = commands_ctx();
    for (int s = 0; s < t->nslots; s++) {
        const TbItem *it = &t->items[t->slots[s].item];
        if (it->kind == IK_FLEX) continue;
        char tip[400]; item_tooltip(t, a, it, c, tip, sizeof tip);
        tip[strcspn(tip, "\n")] = 0;
        printf("tbname %s: %s\n", it->id, tip);
    }
    fflush(stdout);
}

/* Harness: adds a command as a toolbar action (what Customize > Add does). */
bool toolbar_add_action(Toolbar *t, const char *cmd_id) {
    if (!commands_find(cmd_id) || t->prefs.ncustom >= TB_MAX_CUSTOM) return false;
    snprintf(t->prefs.custom[t->prefs.ncustom++], 40, "%s", cmd_id);
    int at = t->prefs.norder; for (int k = 0; k < t->prefs.norder; k++) if (!strcmp(t->prefs.order[k], "history")) at = k + 1;
    memmove(&t->prefs.order[at + 1], &t->prefs.order[at], 40 * (size_t)(t->prefs.norder - at));
    snprintf(t->prefs.order[at], 40, "cmd:%s", cmd_id); t->prefs.norder++;
    t->prefs.preset = PRESET_CUSTOM; tb_rebuild_items(t); tb_mark_dirty(t);
    return true;
}
void toolbar_execution_finished(Toolbar *t, App *a, Db *db, int exec_id, int status) { if (t) tba_execution_finished(t, a, db, exec_id, status); }
