/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Workspace shell: see panels.h. Sections:
 *   1. state            6. headers and tabs        11. palette and modals
 *   2. panel contents   7. splitters               12. named layouts
 *   3. editor tabs      8. drag and drop           13. persistence
 *   4. panel bar        9. overlay                 14. actions and shortcuts
 *   5. strips          10. context menus           15. frame
 *                                                  16. native panel windows
 * Unnumbered helper sections: keymap (after 3), drawing helpers (after 12), panel
 * operations (after 10), editor groups (after 6), public helpers and create / destroy
 * (after 14). The order in the file follows dependencies, not the numbers.
 *
 * How it fits: main.c owns one Panels (panels_create at startup, panels_destroy at exit).
 * Every frame main.c calls panels_frame() with the area between the menu bar and the status
 * bar, runs panels_take_action(), draws the module overlays, then panels_draw_overlay(), and
 * after presenting the main window panels_render_floats() for the native panel windows.
 * Menu and keyboard actions reach panels_action() first; events go through
 * panels_handle_event(). The shell calls the engine (dock.c) for every layout decision and
 * the panel modules (explorer.c, results.c, output.c, editor.c) only to draw and to query
 * badges; it never touches their data.
 *
 * Units: the engine works in logical points; R() turns a DockRect into render pixels
 * (points * a->scale). Mouse coordinates in a->in are render pixels, so they are divided by
 * P->s before being handed to the engine. Window units (SDL window coordinates) appear only
 * for native windows and the test harness (a->density = pixels per window unit).
 *
 * Ownership: Panels owns every editor tab (Editor *) including the one main.c created
 * first, the native panel windows and their renderers / fonts, and the layout files. The
 * App pointer passed around is borrowed. Everything runs on the UI thread.
 */
#include "panels.h"
#include "objed.h"
#include "tdes.h"
#include "rptview.h"
#include "gitview.h"
#include "dgmview.h"
#include "sqllex.h"
#include "commands.h"
#include "conn.h"
#include "draw.h"
#include "editor.h"
#include "explorer.h"
#include "font.h"
#include "log.h"
#include "menu.h"
#include "output.h"
#include "results.h"
#include "theme.h"
#include "ui.h"
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* ======================================================================== */
/* 1. state                                                                  */
/* ======================================================================== */

#define MAX_TABS 24              /* editor (query) tabs */
#define MAX_NAMED 16             /* named layouts */
#define MAX_CMDS 400             /* palette entries */
#define DRAG_THRESHOLD 6.0f      /* points before a press becomes a drag */
#define SAVE_DELAY_MS 600        /* layout writes are coalesced (PNL-030 / NFR-05) */

/* One query tab of the center: a slot (used), a stable id known to the engine, the Editor it
 * owns, its title and the id of the connection it runs on ("" = none). */
typedef struct EdTab { bool used; int id; Editor *ed; char title[48]; char conn_id[16]; struct ObjDoc *obj; struct TdDoc *td; struct RvDoc *rv; struct GdDoc *gd; struct DgDoc *dg;   /* gd: Git diff tab (gitview.h); dg: ER diagram (dgmview.h) */
    /* a file on disk (GIT-017 / 064): path, the editor revision last saved / loaded, what the disk had then */
    char path[1024]; uint32_t saved_rev; long long disk_mtime, disk_size; Uint64 check_at; bool disk_changed; } EdTab;   /* rv: Report Studio tab (rptview.h) */   /* td: table designer tab (tdes.h) */   /* obj: object definition tab (objed.h), NULL for a query tab */
/* A saved named layout: its arrangement and the connection it is bound to ("" = none). */
typedef struct Named { char name[48]; DockState st; char conn_id[16]; } Named;
/* A command palette entry: label, shortcut text, CMD_* kind and two kind-specific args
 * (CMD_ACTION: a1 = ACT_*; CMD_PANEL: a1 = panel, a2 = OP_*; CMD_PRESET / CMD_NAMED: a1 = index). */
typedef struct Cmd { char label[112]; char keys[32]; int kind, a1, a2; char search[200]; char cmd_id[40]; } Cmd;   /* search: synonyms + spelled-out shortcut (TBR-005); cmd_id: registry id */
/* A panel shown in its own native window (PNL-019 floating / PNL-020 detached). The panel's
 * content is the same object as when docked; only the renderer, fonts and input differ. */
typedef struct FloatWin {
    bool used; int panel; int mode;                  /* slot in use, panel index, mode it was opened for */
    SDL_Window *win; SDL_Renderer *ren; Font *ui, *small, *mono;   /* owned by this slot */
    float density, scale; int w, h;                  /* pixels per window unit, UI scale, render size in pixels */
    Input in;                                        /* this window's own input state */
} FloatWin;
#define MAX_FLOATS 4             /* native panel windows open at once */

/* Mouse gesture in progress. *_PENDING = pressed but not yet moved past DRAG_THRESHOLD. */
enum { DRAG_NONE, DRAG_SPLIT, DRAG_PANEL_PENDING, DRAG_PANEL, DRAG_EDTAB_PENDING, DRAG_EDTAB, DRAG_BAR_PENDING, DRAG_BAR };
/* Which modal owns the keyboard. */
enum { MODAL_NONE, MODAL_PALETTE, MODAL_NAME, MODAL_CONFIRM };
/* What the name prompt is for. */
enum { NAME_SAVE_AS, NAME_RENAME };
/* What the confirmation dialog confirms (confirm_arg carries the tab id / layout index). */
enum { CONFIRM_RESET, CONFIRM_IMPORT, CONFIRM_CLOSE_TAB, CONFIRM_DELETE_LAYOUT };
/* Kinds of palette commands, see Cmd. */
enum { CMD_ACTION, CMD_PANEL, CMD_PRESET, CMD_NAMED, CMD_INVOKE /* a registry command without ACT_* (extensions) */ };
/* panel operations (context menu, palette) */
enum { OP_SHOW = 1, OP_HIDE, OP_COLLAPSE, OP_EXPAND, OP_MAXIMIZE, OP_MOVE_L, OP_MOVE_R, OP_MOVE_B, OP_MOVE_T,
       OP_SPLIT_BEFORE, OP_SPLIT_AFTER, OP_MODE_DOCKED, OP_MODE_AUTOHIDE, OP_MODE_FLOAT, OP_BAR, OP_RESET_SIZE, OP_FOCUS, OP_MODE_DETACH,
       OP_LANE_BEFORE, OP_LANE_AFTER };   /* PNL-011: split across the region's axis, without dragging */
/* popup ids: PM_OP_BASE + OP_* runs a panel operation; *_BASE + i addresses panel / named
 * layout / preset i; the others open submenus or act on P->menu_layout */
enum { PM_OP_BASE = 100, PM_MOVE = 300, PM_SPLIT, PM_MODE, PM_RESET_LAYOUT, PM_PANEL_BASE = 400,
       PM_LAYOUT_BASE = 500, PM_LAYOUT_APPLY = 600, PM_LAYOUT_SAVE, PM_LAYOUT_RENAME, PM_LAYOUT_DELETE, PM_LAYOUT_BIND,
       PM_PRESET_BASE = 700, PM_MORE_BASE = 800, PM_EXT_BASE = 900 };

/* The whole shell state; one instance per app (see the ownership notes at the top). */
struct Panels {
    Dock dock;                   /* the engine; the only copy of the layout */
    DockMetrics m;               /* points */
    DockLayout lay;              /* points; converted with R() */
    float s;                     /* points -> pixels */
    int P_EX, P_RS, P_OUT, P_IN; /* panel indices of Explorer, Results, Output, Inspector */
    /* editor tabs (PNL-027 / 028), each bound to a connection */
    EdTab tabs[MAX_TABS]; int next_id;              /* next_id: last editor tab id handed out */
    Connection *last_active; bool switching_conn;   /* a->active seen last frame; true while a tab switch changes it */
    /* interaction */
    int drag; DockSplitter dsplit; float dx0, dy0, dsize0;   /* DRAG_*, splitter being dragged, press point (pixels), region size at press (points) */
    int dpanel, dtab; DockDrop drop;   /* panel / editor tab being dragged, current drop target */
    int ed_target_group, ed_target_index, ed_target_split;   /* split: 0 none, 1 right, 2 down */
    int last_panel;              /* PNL-022: panel focused last, for F12 and panel commands */
    /* extension panels (PNL-047): per-panel view from the factory and visibility for the lifecycle hook */
    void *view[DOCK_MAX_PANELS]; bool created[DOCK_MAX_PANELS];
    bool shown_prev[DOCK_MAX_PANELS], shown_now[DOCK_MAX_PANELS];   /* content drawn last frame / this frame */
    DockMenuItem ext_items[DOCK_MAX_MENU_ITEMS]; int next_items, ext_panel;   /* a panel's own context-menu items, while its menu is open */
    bool modal_mouse_free;       /* this frame's click is still unclaimed, for the modal's own widgets */
    float tab_cx[DOCK_MAX_GROUPS][DOCK_MAX_TABS];   /* centre x (pixels) of each drawn panel tab: drop index */
    /* menus */
    PopupMenu menu, sub, sub2; int menu_panel, menu_layout; char mtext[48][112];   /* popup + 2 submenu levels; the panel / layout they act on; label storage (popups keep pointers) */
    /* modals */
    int modal; TextField field; int pal_sel, pal_scroll; Cmd cmds[MAX_CMDS]; int ncmds; int shown[MAX_CMDS]; int nshown;   /* MODAL_*, shared text field, palette selection / scroll, all commands, indices of the filtered ones */
    int name_purpose, name_index; int confirm_kind, confirm_arg; char confirm_text[320];   /* NAME_* and layout index; CONFIRM_* and its argument */
    /* named layouts */
    Named named[MAX_NAMED]; int nnamed; char active_name[48];   /* active_name: named layout last applied / saved, "" = none */
    char dir[1024];              /* preferences folder */
    /* persistence */
    unsigned long long seen_seq, saved_seq; Uint64 changed_at;   /* change_seq last seen / last saved; ticks of the last change */
    /* tooltip */
    int tt_id; Uint64 tt_since; Rect tt_rect; char tt_text[220]; bool tt_show;   /* hovered widget id and since when; what to draw this frame */
    /* keymap (PNL-036) */
    struct { int action; SDL_Keycode key; unsigned mods; char name[32]; } keys[48]; int nkeys;   /* mods: KM_* mask */
    Uint32 ev_export, ev_import;   /* SDL user event types posted by the file dialogs */
    DockState import_st; char import_name[64];   /* parsed import waiting for confirmation */
    char status[120];              /* buffer behind panels_status() */
    FloatWin fw[MAX_FLOATS];
    char float_shot[1100];       /* harness: save the first floating window after its next draw */
    /* while a modal is open its keys and text are held back from the panels below */
    KeyPress held_keys[LUME_MAX_KEYS]; int nheld; char held_text[2048]; int nheld_text;
};

static const char *region_word(int r);
struct Panels;
static const char *side_split_word(const struct Panels *P);
/* Engine rectangle (points) -> render pixels. */
static Rect R(const Panels *P, DockRect d) { return rect_make(d.x * P->s, d.y * P->s, d.w * P->s, d.h * P->s); }

/* ======================================================================== */
/* 2. panel contents                                                         */
/* ======================================================================== */

/* Each panel's content lives in its module; these adapters only call it with the rectangle the
 * engine assigned. Nothing here copies data or reruns SQL (R-03, PNL-040). */
/* DockPanelDesc.draw adapters: ctx is the App, rc is already in render pixels (the shell
 * converts before calling). ud and focused are unused by these modules. */
static void draw_explorer(void *ud, void *ctx, DockRect rc, bool focused) {
    (void)ud; (void)focused; App *a = ctx;
    explorer_update(a->explorer, a, rect_make(rc.x, rc.y, rc.w, rc.h));
}
/* Results grid of the active result tab. */
static void draw_results(void *ud, void *ctx, DockRect rc, bool focused) {
    (void)ud; (void)focused; App *a = ctx;
    results_update(a->results, a, rect_make(rc.x, rc.y, rc.w, rc.h));
}
/* Output log; its pre-update (input) runs here because it has no separate hook. */
static void draw_output(void *ud, void *ctx, DockRect rc, bool focused) {
    (void)ud; (void)focused; App *a = ctx;
    output_pre_update(a->output, a);
    output_update(a->output, a, rect_make(rc.x, rc.y, rc.w, rc.h));
}
/* Record view of the selected result row (content owned by results.c). */
static void draw_inspector(void *ud, void *ctx, DockRect rc, bool focused) {
    (void)ud; (void)focused; App *a = ctx;
    results_draw_inspector(a->results, a, rect_make(rc.x, rc.y, rc.w, rc.h));
}
/* Results badge: number of result tabs, unsaved edited rows, error of the last run. */
static void badge_results(void *ud, void *ctx, DockBadge *b) {
    (void)ud; App *a = ctx; bool err;
    results_badge(a->results, &b->count, &b->unsaved, &err);
    b->error = err;
}
/* Output badge: unseen lines, errors among them, a job still running. */
static void badge_output(void *ud, void *ctx, DockBadge *b) {
    (void)ud; App *a = ctx;
    int errs = 0, unseen = output_unseen(a->output, a, &errs);
    b->loading = output_job_running(a->output);
    b->error = errs > 0; b->warning = unseen > errs; b->count = unseen;
}

/* The object a panel's hooks receive: what its view factory made (created on first use), or
 * the registered ud when the panel has no factory (PNL-047). */
static void *panel_view(Panels *P, App *a, int p) {
    const DockPanelDesc *ds = &P->dock.desc[p];
    if (!ds->create) return ds->ud;
    if (!P->created[p]) { P->view[p] = ds->create(ds->ud, a); P->created[p] = true; LOG_I("panel %s: view created", ds->id); }
    return P->view[p];
}
/* Draws panel p's content and notes that it was visible this frame (for the visibility hook). */
static void panel_draw(Panels *P, App *a, int p, Rect content, bool focused) {
    P->shown_now[p] = true;
    P->dock.desc[p].draw(panel_view(P, a, p), a, (DockRect){ content.x, content.y, content.w, content.h }, focused);
}
/* Calls the visibility hook of every panel whose content appeared or disappeared since the
 * previous frame (main window and panel windows alike), then starts a new frame's record. */
static void panels_lifecycle(Panels *P, App *a) {
    for (int p = 0; p < P->dock.ndesc; p++) {
        const DockPanelDesc *ds = &P->dock.desc[p];
        if (P->shown_now[p] != P->shown_prev[p] && ds->visibility && (P->created[p] || !ds->create)) ds->visibility(panel_view(P, a, p), a, P->shown_now[p]);
        P->shown_prev[p] = P->shown_now[p]; P->shown_now[p] = false;
    }
}

/* Registers the four Lume Data panels with the engine (ids, icons, minimum sizes in points,
 * preferred regions, fold priority) and remembers their indices. */
static void register_panels(Panels *P) {
    DockPanelDesc ex = { .id = "lume.data.explorer", .title = "Explorer", .icon = "db", .shortcut = "1",
        .region = DOCK_LEFT, .min_w = 180, .min_h = 120, .pref_size = 250, .priority = 2, .draw = draw_explorer };
    DockPanelDesc rs = { .id = "lume.data.results", .title = "Results", .icon = "grid", .shortcut = "2",
        .region = DOCK_BOTTOM, .min_w = 260, .min_h = 120, .pref_size = 260, .priority = 1, .draw = draw_results, .badge = badge_results };
    DockPanelDesc out = { .id = "lume.data.output", .title = "Output", .icon = "log", .shortcut = "3",
        .region = DOCK_BOTTOM, .min_w = 260, .min_h = 100, .pref_size = 260, .priority = 1, .draw = draw_output, .badge = badge_output };
    DockPanelDesc in = { .id = "lume.data.inspector", .title = "Inspector", .icon = "info", .shortcut = "4",
        .region = DOCK_RIGHT, .min_w = 220, .min_h = 120, .pref_size = 320, .priority = 3, .start_hidden = true, .draw = draw_inspector };
    const DockPanelDesc *all[] = { &ex, &rs, &out, &in };
    for (int i = 0; i < 4; i++) if (!dock_register(&P->dock, all[i])) LOG_E("panel registration failed: %s", P->dock.err);
    P->P_EX = dock_find(&P->dock, ex.id); P->P_RS = dock_find(&P->dock, rs.id);
    P->P_OUT = dock_find(&P->dock, out.id); P->P_IN = dock_find(&P->dock, in.id);
}

/* True when panel p has the keyboard: the Explorer via a->focus, the panels that share
 * FOCUS_RESULTS via last_panel. */
static bool panel_focused(Panels *P, App *a, int p) {
    if (p == P->P_EX) return a->focus == FOCUS_EXPLORER;
    if (p == P->P_RS || p == P->P_OUT || p == P->P_IN) return a->focus == FOCUS_RESULTS && P->last_panel == p;
    return P->last_panel == p;
}

/* ======================================================================== */
/* 3. editor tabs (PNL-027 / 028) and the connection of each tab             */
/* ======================================================================== */

/* Editor tab by engine id, or NULL. */
static EdTab *tab_by_id(Panels *P, int id) { for (int i = 0; i < MAX_TABS; i++) if (P->tabs[i].used && P->tabs[i].id == id) return &P->tabs[i]; return NULL; }
/* Editor tab owning editor e, or NULL. */
static EdTab *tab_of_editor(Panels *P, Editor *e) { for (int i = 0; i < MAX_TABS; i++) if (P->tabs[i].used && P->tabs[i].ed == e) return &P->tabs[i]; return NULL; }
/* Saved connection by id, or NULL (borrowed from a->conns). */
static Connection *conn_by_id(App *a, const char *id) {
    for (int i = 0; a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->id, id)) return a->conns->items[i];
    return NULL;
}

/* Makes tab `id` the one that types and runs; its connection becomes the active one, so a run
 * always uses the tab's own connection (PNL-027). */
static void activate_tab(Panels *P, App *a, int id, bool focus) {
    EdTab *t = tab_by_id(P, id);
    if (!t) return;
    dock_editor_activate(&P->dock, id);
    a->editor = t->ed;
    if (focus) a->focus = FOCUS_EDITOR;
    Connection *c = t->conn_id[0] ? conn_by_id(a, t->conn_id) : NULL;
    if (c && c != a->active) {
        P->switching_conn = true;
        if (c->db) app_set_active(a, c);
        else {
            /* the tab's own connection is not open: no execution target at all, never the
             * previous tab's connection (TBR-008: Run would otherwise hit the wrong database) */
            app_set_active(a, NULL);
            app_status(a, "%s runs on %s, which is not connected: connect it from the toolbar or the Explorer", t->title, c->name);
        }
    }
}

/* See panels.h. */
Connection *panels_tab_connection(Panels *P, App *a, char *title, size_t tn) {
    EdTab *t = tab_of_editor(P, a->editor);
    if (title && tn) snprintf(title, tn, "%s", t ? t->title : "");
    return t && t->conn_id[0] ? conn_by_id(a, t->conn_id) : NULL;
}

/* See panels.h. Binds the active query tab to c (explicit choice, TBR-009): only this tab
 * changes; a connected c becomes the execution target now, a closed one is connected in the
 * background and becomes the target when it is ready, if the tab still points to it. */
void panels_set_tab_connection(Panels *P, App *a, Connection *c) {
    EdTab *t = tab_of_editor(P, a->editor);
    if (!t) return;
    snprintf(t->conn_id, sizeof t->conn_id, "%s", c ? c->id : "");
    if (!t->obj) editor_set_dialect(t->ed, c ? dialect_for_driver(c->driver) : DIALECT_GENERIC);
    P->switching_conn = true;
    if (c && c->db) app_set_active(a, c);
    else {
        app_set_active(a, NULL);
        if (c && c->state != CONN_CONNECTING && c->state != CONN_RECONNECTING) app_connect_async(a, c, false);
    }
    P->last_active = a->active;
    LOG_I("tab \"%s\" now runs on %s", t->title, c ? c->name : "no connection");
}

/* See panels.h: a background connect finished; the active tab adopts it if it still points there. */
void panels_connection_ready(Panels *P, App *a, Connection *c) {
    EdTab *t = tab_of_editor(P, a->editor);
    if (t && c && c->db && !strcmp(t->conn_id, c->id)) { P->switching_conn = true; app_set_active(a, c); P->last_active = a->active; }
}

/* Takes ownership of `ed` as a new tab in editor group egroup, bound to the active
 * connection. Returns its id, or -1 when every slot is used (ed is then NOT freed). */
static int add_tab(Panels *P, App *a, Editor *ed, const char *title, int egroup) {
    for (int i = 0; i < MAX_TABS; i++) {
        if (P->tabs[i].used) continue;
        EdTab *t = &P->tabs[i];
        memset(t, 0, sizeof *t);
        t->used = true; t->id = ++P->next_id; t->ed = ed;
        snprintf(t->title, sizeof t->title, "%s", title);
        if (a->active) snprintf(t->conn_id, sizeof t->conn_id, "%s", a->active->id);
        editor_set_dialect(ed, a->active ? dialect_for_driver(a->active->driver) : DIALECT_GENERIC);   /* highlighting rules of the tab's engine */
        editor_set_strip(ed, false);
        dock_editor_add_tab(&P->dock, egroup, t->id);
        return t->id;
    }
    return -1;
}

/* Creates "query N.sql" (optionally prefilled with text) in the active editor group and
 * activates it with the keyboard focus. */
void panels_new_editor_tab(Panels *P, App *a, const char *text) {
    int n = 1;
    for (int i = 0; i < MAX_TABS; i++) if (P->tabs[i].used) n++;
    char title[48];
    snprintf(title, sizeof title, n == 1 ? "query.sql" : "query %d.sql", n);
    Editor *ed = editor_create();
    if (text) editor_set_text(ed, text);
    int id = add_tab(P, a, ed, title, P->dock.st.active_editor_group);
    if (id < 0) { editor_destroy(ed); app_status(a, "Too many query tabs (%d)", MAX_TABS); return; }
    activate_tab(P, a, id, true);
}

static void close_tab_now(Panels *P, App *a, int id);
/* ---- query tabs for the toolbar (Switch Tab, workspaces) ---- */
int panels_tab_count(Panels *P) { int n = 0; for (int i = 0; i < MAX_TABS; i++) n += P->tabs[i].used; return n; }
bool panels_tab_info(Panels *P, App *a, int k, TabInfo *out) {
    int n = 0;
    for (int i = 0; i < MAX_TABS; i++) {
        if (!P->tabs[i].used || n++ != k) continue;
        EdTab *t = &P->tabs[i];
        memset(out, 0, sizeof *out);
        out->id = t->id; out->active = a->editor == t->ed;
        snprintf(out->title, sizeof out->title, "%s", t->title);
        snprintf(out->conn_id, sizeof out->conn_id, "%s", t->conn_id);
        out->ed = t->ed;
        return true;
    }
    return false;
}
void panels_activate_tab(Panels *P, App *a, int id) { activate_tab(P, a, id, true); }
void panels_close_tab(Panels *P, App *a, int id) { close_tab_now(P, a, id); }
/* Opens a tab with a title, a connection binding and text (workspace restore). */
int panels_open_tab(Panels *P, App *a, const char *title, const char *conn_id, const char *text) {
    Editor *ed = editor_create();
    if (text) editor_set_text(ed, text);
    int id = add_tab(P, a, ed, title && title[0] ? title : "query.sql", P->dock.st.active_editor_group);
    if (id < 0) { editor_destroy(ed); return -1; }
    EdTab *t = tab_by_id(P, id);
    if (t) snprintf(t->conn_id, sizeof t->conn_id, "%s", conn_id ? conn_id : "");
    return id;
}
/* Object definition tab (objed.c): bound to the object's connection; the document is owned by
 * objed and released through objed_tab_closed when the tab closes. */
int panels_open_object_tab(Panels *P, App *a, const char *title, const char *conn_id, const char *text, struct ObjDoc *obj) {
    int id = panels_open_tab(P, a, title, conn_id, text);
    EdTab *t = tab_by_id(P, id);
    if (!t) return -1;
    t->obj = obj;
    editor_set_object_mode(t->ed, true);
    activate_tab(P, a, id, true);
    return id;
}
/* Table designer tab (tdes.c): the editor shows the read-only SQL preview; the document belongs to tdes. */
int panels_open_designer_tab(Panels *P, App *a, const char *title, const char *conn_id, struct TdDoc *td) {
    int id = panels_open_tab(P, a, title, conn_id, "");
    EdTab *t = tab_by_id(P, id);
    if (!t) return -1;
    t->td = td;
    activate_tab(P, a, id, true);
    return id;
}
void panels_set_tab_title(Panels *P, int id, const char *title) { EdTab *t = tab_by_id(P, id); if (t && strcmp(t->title, title)) snprintf(t->title, sizeof t->title, "%s", title); }

/* ---- file-backed tabs (Source Control: GIT-017 / 064) ---- */
#include <sys/stat.h>
#include <unistd.h>
static bool file_stat(const char *p, long long *mt, long long *sz) {
    struct stat sb; if (stat(p, &sb)) return false;
#ifdef __APPLE__
    *mt = (long long)sb.st_mtimespec.tv_sec * 1000000000LL + sb.st_mtimespec.tv_nsec;
#elif defined(_WIN32)
    *mt = (long long)sb.st_mtime * 1000000000LL;   /* seconds only on Windows */
#else
    *mt = (long long)sb.st_mtim.tv_sec * 1000000000LL + sb.st_mtim.tv_nsec;
#endif
    *sz = (long long)sb.st_size; return true;
}
static char *read_text_file(const char *p, size_t *len, char *why, size_t wn) {
    FILE *f = fopen(p, "rb"); if (!f) { snprintf(why, wn, "cannot open %s", p); return NULL; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 8 * 1024 * 1024) { fclose(f); snprintf(why, wn, "%s is larger than 8 MB: not opened in the editor", p); return NULL; }
    char *b = malloc((size_t)n + 1); size_t r = fread(b, 1, (size_t)n, f); fclose(f); b[r] = 0;
    if (memchr(b, 0, r)) { free(b); snprintf(why, wn, "%s is a binary file: not opened as text", p); return NULL; }
    *len = r; return b;
}
bool panels_tab_dirty_file(Panels *P, int id) { EdTab *t = tab_by_id(P, id); return t && t->path[0] && editor_revision(t->ed) != t->saved_rev; }
const char *panels_tab_path(Panels *P, int id) { EdTab *t = tab_by_id(P, id); return t && t->path[0] ? t->path : NULL; }
int panels_open_file_tab(Panels *P, App *a, const char *path) {
    /* realpath writes up to PATH_MAX bytes (4096 on Linux, where glibc's fortified realpath aborts on a smaller buffer) */
    char abs[4096]; if (!realpath(path, abs)) snprintf(abs, sizeof abs, "%s", path);
    for (int i = 0; i < MAX_TABS; i++) if (P->tabs[i].used && !strcmp(P->tabs[i].path, abs)) { activate_tab(P, a, P->tabs[i].id, true); return P->tabs[i].id; }
    char why[1200]; size_t len = 0;
    char *text = read_text_file(abs, &len, why, sizeof why);
    if (!text) { app_status(a, "%s", why); return -1; }
    const char *base = strrchr(abs, '/'); base = base ? base + 1 : abs;
    int id = panels_open_tab(P, a, base, "", text);
    free(text);
    EdTab *t = tab_by_id(P, id); if (!t) return -1;
    snprintf(t->path, sizeof t->path, "%s", abs);
    t->saved_rev = editor_revision(t->ed);
    file_stat(abs, &t->disk_mtime, &t->disk_size);
    t->disk_changed = false;
    activate_tab(P, a, id, true);
    LOG_I("file: opened %s", abs);
    return id;
}
static bool save_tab(Panels *P, App *a, EdTab *t) {
    if (!t || !t->path[0]) return false;
    int len = 0; char *text = editor_text(t->ed, &len);
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.lume-tmp", t->path);
    FILE *f = fopen(tmp, "wb"); bool ok = f != NULL;
    if (ok) { ok = fwrite(text, 1, (size_t)len, f) == (size_t)len; ok = (fflush(f) == 0) && ok; ok = (fsync(fileno(f)) == 0) && ok; ok = (fclose(f) == 0) && ok; }
    /* keep the file's mode */
    struct stat sb; if (ok && stat(t->path, &sb) == 0) chmod(tmp, sb.st_mode & 07777);
    if (ok && rename(tmp, t->path) != 0) ok = false;
    if (!ok) { unlink(tmp); app_status(a, "Could not save %s (disk full or no permission): the file on disk is unchanged", t->path); free(text); return false; }
    free(text);
    t->saved_rev = editor_revision(t->ed); file_stat(t->path, &t->disk_mtime, &t->disk_size); t->disk_changed = false;
    app_status(a, "Saved %s (saving a file never changes a database)", t->path);
    LOG_I("file: saved %s", t->path);
    (void)P;
    return true;
}
bool panels_save_tab(Panels *P, App *a, int id) { return save_tab(P, a, tab_by_id(P, id)); }
int panels_active_file_tab(Panels *P, App *a) { EdTab *t = tab_of_editor(P, a->editor); return t && t->path[0] ? t->id : -1; }
/* dirty file tabs whose path is inside root: names into out (GIT-064) */
int panels_dirty_files_under(Panels *P, const char *root, char out[][256], int max) {
    int n = 0; size_t rl = strlen(root);
    for (int i = 0; i < MAX_TABS; i++) {
        EdTab *t = &P->tabs[i];
        if (!t->used || !t->path[0] || editor_revision(t->ed) == t->saved_rev) continue;
        if (strncmp(t->path, root, rl) || (t->path[rl] != '/' && t->path[rl])) continue;
        if (n < max) snprintf(out[n], 256, "%s", t->path + rl + (t->path[rl] == '/'));
        n++;
    }
    return n;
}
int panels_save_files_under(Panels *P, App *a, const char *root) {
    int n = 0; size_t rl = strlen(root);
    for (int i = 0; i < MAX_TABS; i++) { EdTab *t = &P->tabs[i]; if (t->used && t->path[0] && !strncmp(t->path, root, rl) && editor_revision(t->ed) != t->saved_rev && save_tab(P, a, t)) n++; }
    return n;
}
static void reload_tab(App *a, EdTab *t) {
    char why[1200]; size_t len = 0; char *text = read_text_file(t->path, &len, why, sizeof why);
    if (!text) { app_status(a, "%s", why); return; }
    int line, col; editor_cursor_pos(t->ed, &line, &col);
    editor_set_text(t->ed, text); free(text);
    t->saved_rev = editor_revision(t->ed); file_stat(t->path, &t->disk_mtime, &t->disk_size); t->disk_changed = false;
    (void)line; (void)col;
}
/* external changes (GIT-017): a clean buffer follows the disk; a dirty one is never overwritten */
static void check_disk(Panels *P, App *a, EdTab *t) {
    if (!t->path[0] || SDL_GetTicks() < t->check_at) return;
    t->check_at = SDL_GetTicks() + 700;
    long long mt, sz;
    if (!file_stat(t->path, &mt, &sz)) { if (!t->disk_changed) { t->disk_changed = true; app_status(a, "%s was deleted or moved on disk: the buffer is kept", t->path); } return; }
    if (mt == t->disk_mtime && sz == t->disk_size) return;
    if (editor_revision(t->ed) == t->saved_rev) { reload_tab(a, t); app_status(a, "Reloaded %s: it changed on disk (checkout, pull or another program)", t->title); LOG_I("file: reloaded %s after an external change", t->path); }
    else if (!t->disk_changed) { t->disk_changed = true; LOG_I("file: %s changed on disk while the buffer has unsaved edits", t->path); }
    (void)P;
}

/* Git diff tab (gitview.c): the diff view replaces the editor; the document belongs to gitview. */
int panels_open_diff_tab(Panels *P, App *a, const char *title, struct GdDoc *gd) {
    int id = panels_open_tab(P, a, title, "", "");
    EdTab *t = tab_by_id(P, id);
    if (!t) return -1;
    t->gd = gd;
    activate_tab(P, a, id, true);
    return id;
}
/* ER diagram tab (dgmview.c): the canvas replaces the editor; the document belongs to dgmview. */
int panels_open_diagram_tab(Panels *P, App *a, const char *title, struct DgDoc *dg) {
    Connection *c = NULL;
    for (int i = 0; a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->id, dgmview_doc_conn(dg))) c = a->conns->items[i];
    int id = panels_open_tab(P, a, title, c ? c->id : "", "");
    EdTab *t = tab_by_id(P, id);
    if (!t) return -1;
    t->dg = dg;
    activate_tab(P, a, id, true);
    return id;
}
struct DgDoc *panels_active_diagram(Panels *P, App *a) {
    (void)a;
    int g = P->dock.st.active_editor_group;
    EdTab *t = tab_by_id(P, P->dock.st.editor[g].active);
    return t ? t->dg : NULL;
}
void panels_activate_diagram(Panels *P, App *a, struct DgDoc *dg) {
    for (int i = 0; i < MAX_TABS; i++) if (P->tabs[i].used && P->tabs[i].dg == dg) { activate_tab(P, a, P->tabs[i].id, true); return; }
}
/* Report Studio tab (rptview.c): the canvas replaces the editor; the document belongs to rptview. */
int panels_open_report_tab(Panels *P, App *a, const char *title, struct RvDoc *rv) {
    int id = panels_open_tab(P, a, title, "", "");
    EdTab *t = tab_by_id(P, id);
    if (!t) return -1;
    t->rv = rv;
    activate_tab(P, a, id, true);
    return id;
}
struct RvDoc *panels_active_report(Panels *P, App *a) {
    int g = P->dock.st.active_editor_group;
    EdTab *t = tab_by_id(P, P->dock.st.editor[g].active);
    return t ? t->rv : NULL;
}
struct TdDoc *panels_active_designer(Panels *P, App *a) {
    int g = P->dock.st.active_editor_group;
    EdTab *t = tab_by_id(P, P->dock.st.editor[g].active);
    return t ? t->td : NULL;
}
struct ObjDoc *panels_tab_obj(Panels *P, int id) { EdTab *t = tab_by_id(P, id); return t ? t->obj : NULL; }
struct ObjDoc *panels_active_obj(Panels *P, App *a) { EdTab *t = tab_of_editor(P, a->editor); return t ? t->obj : NULL; }
Editor *panels_tab_editor(Panels *P, int id) { EdTab *t = tab_by_id(P, id); return t ? t->ed : NULL; }
bool panels_tab_exists(Panels *P, int id) { return tab_by_id(P, id) != NULL; }

/* Closes every tab but leaves one empty, unbound tab (switching workspace). */
void panels_close_all_tabs(Panels *P, App *a) {
    int keep = panels_open_tab(P, a, "query.sql", "", "");
    for (int i = 0; i < MAX_TABS; i++) if (P->tabs[i].used && P->tabs[i].id != keep) close_tab_now(P, a, P->tabs[i].id);
    activate_tab(P, a, keep, true);
}

/* True when the tab's buffer holds anything but whitespace (worth a confirmation). */
static bool tab_has_text(EdTab *t) {
    int len = 0; char *s = editor_text(t->ed, &len);
    bool any = false;
    for (int i = 0; s && i < len; i++) if (!isspace((unsigned char)s[i])) { any = true; break; }
    free(s);
    return any;
}

/* Closes tab id without asking: removes it from the engine, moves the active editor to the
 * group's new active tab if needed, and frees the Editor. */
static void close_tab_now(Panels *P, App *a, int id) {
    EdTab *t = tab_by_id(P, id);
    if (!t) return;
    if (!dock_editor_close_tab(&P->dock, id)) { app_status(a, "%s", P->dock.err); return; }
    if (a->editor == t->ed) {
        int g = P->dock.st.active_editor_group;
        int nid = P->dock.st.editor[g].active;
        EdTab *nt = tab_by_id(P, nid);
        if (nt) activate_tab(P, a, nid, a->focus == FOCUS_EDITOR);
    }
    struct ObjDoc *obj = t->obj;
    struct TdDoc *td = t->td;
    struct RvDoc *rv = t->rv; struct GdDoc *gd = t->gd; struct DgDoc *dg = t->dg;
    t->obj = NULL; t->td = NULL; t->rv = NULL; t->gd = NULL; t->dg = NULL; t->path[0] = 0;
    editor_destroy(t->ed);
    t->used = false;
    if (obj) objed_tab_closed(a, obj);   /* keeps the draft on disk (OBJ-009) */
    if (td) tdes_tab_closed(a, td);
    if (rv) rptview_tab_closed(a, rv);   /* unsaved changes stay in the autosave (LI-050.2) */
    if (gd) gitview_diff_closed(a, gd);
    if (dg) dgmview_tab_closed(a, dg);   /* the layout is saved (DGM-053) */
}

static void open_confirm(Panels *P, int kind, int arg, const char *text);

/* Close requested by the user: refused for the last tab, confirmed first when it has text. */
static void request_close_tab(Panels *P, App *a, int id) {
    EdTab *t = tab_by_id(P, id);
    if (!t) return;
    int total = 0; for (int i = 0; i < MAX_TABS; i++) total += P->tabs[i].used;
    if (total <= 1) { app_status(a, "The last query tab stays open (the editor is never removed)"); return; }
    if (t->td) { if (!tdes_request_close(a, t->td, id)) close_tab_now(P, a, id); return; }   /* Save Draft / Discard / Cancel (TBL-003.3) */
    /* TBR-028: the last tab on a connection with an open transaction says so (the session, and
     * its transaction, stay open: Commit / Rollback end it) */
    Connection *tc = t->conn_id[0] ? conn_by_id(a, t->conn_id) : NULL;
    int on_conn = 0; for (int i = 0; i < MAX_TABS; i++) if (P->tabs[i].used && tc && !strcmp(P->tabs[i].conn_id, tc->id)) on_conn++;
    bool tx_open = tc && tc->db && tc->db->drv->tx_state && !__atomic_load_n(&tc->db->busy, __ATOMIC_SEQ_CST) && tc->db->drv->tx_state(tc->db) == 1;
    if (tx_open && on_conn == 1) {
        char msg[300];
        snprintf(msg, sizeof msg, "Close \"%s\"? %s has an open transaction: it stays open in the session (not committed, not rolled back) until you Commit or Rollback from the toolbar.", t->title, tc->name);
        open_confirm(P, CONFIRM_CLOSE_TAB, id, msg);
        return;
    }
    if (t->path[0]) {   /* a file: only unsaved edits need a question */
        if (editor_revision(t->ed) == t->saved_rev) { close_tab_now(P, a, id); return; }
        char msg[1200]; snprintf(msg, sizeof msg, "Close \"%s\" and discard its unsaved edits? The file on disk keeps its last saved content (%s).", t->title, t->path);
        open_confirm(P, CONFIRM_CLOSE_TAB, id, msg);
        return;
    }
    if (tab_has_text(t)) {
        char msg[200];
        snprintf(msg, sizeof msg, "Close \"%s\"? Its SQL is not saved anywhere else.", t->title);
        open_confirm(P, CONFIRM_CLOSE_TAB, id, msg);
        return;
    }
    close_tab_now(P, a, id);
}

/* ======================================================================== */
/* keymap (PNL-036)                                                          */
/* ======================================================================== */

/* Default bindings of the panel commands. `name` is the key used in keymap.txt. */
static const struct { const char *name; int action; const char *chord; } default_keys[] = {
    { "palette", ACT_PALETTE, "Cmd+Shift+P" },
    { "explorer", ACT_PANEL_EXPLORER, "Cmd+1" },
    { "results", ACT_PANEL_RESULTS, "Cmd+2" },
    { "output", ACT_PANEL_OUTPUT, "Cmd+3" },
    { "inspector", ACT_PANEL_INSPECTOR, "Cmd+4" },
    { "focus_mode", ACT_FOCUS_MODE, "Cmd+Shift+F12" },
    { "maximize_panel", ACT_MAXIMIZE_PANEL, "Cmd+Alt+M" },
    { "collapse_panel", ACT_COLLAPSE_PANEL, "Cmd+Alt+J" },
    { "hide_panel", ACT_HIDE_PANEL, "Shift+Escape" },
    { "undo_layout", ACT_UNDO_LAYOUT, "Cmd+Alt+Shift+Z" },
    { "grow_panel", ACT_RESIZE_GROW, "Ctrl+Alt+Right" },
    { "shrink_panel", ACT_RESIZE_SHRINK, "Ctrl+Alt+Left" },
    { "last_panel", ACT_LAST_PANEL, "F12" },
    { "new_tab", ACT_NEW_EDITOR_TAB, "Cmd+T" },
    { "split_right", ACT_SPLIT_RIGHT, "Cmd+Backslash" },
    { "split_down", ACT_SPLIT_DOWN, "Cmd+Shift+Backslash" },
    { "panels", ACT_PANELS, "Cmd+Alt+P" },
};

/* "Cmd+Shift+F12" -> key + KM_* mask; false when it cannot be read */
static bool parse_chord(const char *s, SDL_Keycode *key, unsigned *mods) {
    *key = 0; *mods = 0;
    char buf[64]; snprintf(buf, sizeof buf, "%s", s);
    for (char *tok = strtok(buf, "+"); tok; tok = strtok(NULL, "+")) {
        if (!strcasecmp(tok, "Cmd") || !strcasecmp(tok, "Command") || !strcasecmp(tok, "Primary")) *mods |= KM_PRIMARY;
#ifdef __APPLE__
        else if (!strcasecmp(tok, "Ctrl") || !strcasecmp(tok, "Control")) *mods |= KM_CTRL;
#else
        else if (!strcasecmp(tok, "Ctrl") || !strcasecmp(tok, "Control")) *mods |= KM_PRIMARY;
#endif
        else if (!strcasecmp(tok, "Shift")) *mods |= KM_SHIFT;
        else if (!strcasecmp(tok, "Alt") || !strcasecmp(tok, "Option")) *mods |= KM_ALT;
        else if ((tok[0] == 'F' || tok[0] == 'f') && isdigit((unsigned char)tok[1])) { int n = atoi(tok + 1); if (n < 1 || n > 12) return false; *key = SDLK_F1 + (SDL_Keycode)(n - 1); }
        else if (!strcasecmp(tok, "Up")) *key = SDLK_UP;
        else if (!strcasecmp(tok, "Down")) *key = SDLK_DOWN;
        else if (!strcasecmp(tok, "Left")) *key = SDLK_LEFT;
        else if (!strcasecmp(tok, "Right")) *key = SDLK_RIGHT;
        else if (!strcasecmp(tok, "Escape") || !strcasecmp(tok, "Esc")) *key = SDLK_ESCAPE;
        else if (!strcasecmp(tok, "Return") || !strcasecmp(tok, "Enter")) *key = SDLK_RETURN;
        else if (!strcasecmp(tok, "Space")) *key = SDLK_SPACE;
        else if (!strcasecmp(tok, "Backslash")) *key = SDLK_BACKSLASH;
        else if (strlen(tok) == 1) *key = (SDL_Keycode)tolower((unsigned char)tok[0]);
        else return false;
    }
    return *key != 0;
}

/* Builds P->keys from default_keys, then applies keymap.txt overrides ("name=Chord", '#'
 * comments) from the preferences folder; bad lines are logged and skipped. */
static void load_keymap(Panels *P) {
    P->nkeys = 0;
    for (size_t i = 0; i < sizeof default_keys / sizeof default_keys[0]; i++) {
        SDL_Keycode k; unsigned m;
        if (!parse_chord(default_keys[i].chord, &k, &m)) continue;
        P->keys[P->nkeys].action = default_keys[i].action; P->keys[P->nkeys].key = k; P->keys[P->nkeys].mods = m;
        snprintf(P->keys[P->nkeys].name, sizeof P->keys[0].name, "%s", default_keys[i].name);
        P->nkeys++;
    }
    char path[1100]; snprintf(path, sizeof path, "%skeymap.txt", P->dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq++ = 0;
        int idx = -1;
        for (int i = 0; i < P->nkeys; i++) if (!strcmp(P->keys[i].name, line)) idx = i;
        SDL_Keycode k; unsigned m;
        if (idx < 0) { LOG_W("keymap.txt: unknown command \"%s\"", line); continue; }
        if (!parse_chord(eq, &k, &m)) { LOG_W("keymap.txt: cannot read the shortcut \"%s\" for %s", eq, line); continue; }
        P->keys[idx].key = k; P->keys[idx].mods = m;
    }
    fclose(f);
    /* conflicts are reported, the first binding wins */
    for (int i = 0; i < P->nkeys; i++) for (int j = i + 1; j < P->nkeys; j++)
        if (P->keys[i].key == P->keys[j].key && P->keys[i].mods == P->keys[j].mods)
            LOG_W("keymap: %s and %s share a shortcut; %s wins", P->keys[i].name, P->keys[j].name, P->keys[i].name);
}

/* "⇧⌘F12" style text of a binding, for tooltips and the palette */
static void chord_text(Panels *P, int action, char *out, size_t n) {
    out[0] = 0;
    for (int i = 0; i < P->nkeys; i++) {
        if (P->keys[i].action != action) continue;
        unsigned m = P->keys[i].mods; SDL_Keycode k = P->keys[i].key;
        char key[16];
        if (k >= SDLK_F1 && k <= SDLK_F12) snprintf(key, sizeof key, "F%d", (int)(k - SDLK_F1) + 1);
        else if (k == SDLK_LEFT) snprintf(key, sizeof key, "\xE2\x86\x90");
        else if (k == SDLK_RIGHT) snprintf(key, sizeof key, "\xE2\x86\x92");
        else if (k == SDLK_UP) snprintf(key, sizeof key, "\xE2\x86\x91");
        else if (k == SDLK_DOWN) snprintf(key, sizeof key, "\xE2\x86\x93");
        else if (k == SDLK_ESCAPE) snprintf(key, sizeof key, "Esc");
        else if (k == SDLK_BACKSLASH) snprintf(key, sizeof key, "\\");
        else snprintf(key, sizeof key, "%c", toupper((int)k));
#ifdef __APPLE__
        snprintf(out, n, "%s%s%s%s%s", m & KM_CTRL ? "\xE2\x8C\x83" : "", m & KM_ALT ? "\xE2\x8C\xA5" : "", m & KM_SHIFT ? "\xE2\x87\xA7" : "", m & KM_PRIMARY ? "\xE2\x8C\x98" : "", key);
#else
        snprintf(out, n, "%s%s%s%s", m & KM_PRIMARY ? "Ctrl+" : "", m & KM_ALT ? "Alt+" : "", m & KM_SHIFT ? "Shift+" : "", key);
#endif
        return;
    }
}

/* ======================================================================== */
/* 13. persistence                                                           */
/* ======================================================================== */

/* <prefs>/layout.json */
static void layout_path(Panels *P, char *out, size_t n) { snprintf(out, n, "%slayout.json", P->dir); }

/* Writes layout.json right away and records what was saved; failures go to the log and
 * the status bar. */
static void save_now(Panels *P, App *a) {
    char path[1100], err[200];
    layout_path(P, path, sizeof path);
    if (!dock_save_file(&P->dock, path, err, sizeof err)) { LOG_W("layout not saved: %s", err); app_status(a, "Layout not saved: %s", err); return; }
    P->saved_seq = P->dock.change_seq; P->dock.dirty = false;
    LOG_D("layout saved (%s)", path);
}

/* Coalesced save: after the layout stopped changing for SAVE_DELAY_MS and no button is held,
 * so a splitter drag writes once, not per pixel (PNL-030 / NFR-05). */
static void maybe_save(Panels *P, App *a) {
    if (P->dock.change_seq != P->seen_seq) { P->seen_seq = P->dock.change_seq; P->changed_at = SDL_GetTicks(); }
    if (!P->dock.dirty || a->in.mouse_down) return;
    Uint64 now = SDL_GetTicks();
    if (now - P->changed_at >= SAVE_DELAY_MS) save_now(P, a);
    else app_wake_in(a, (Uint32)(SAVE_DELAY_MS - (now - P->changed_at)) + 5);
}

/* ======================================================================== */
/* 12. named layouts (PNL-033 / 034)                                         */
/* ======================================================================== */
/* <prefs>/layouts/<name>.json, plus <prefs>/layouts/bindings.txt ("connection-id<TAB>name"). The
 * presets (Default, Query Focus, Data Exploration) are built in and cannot be edited. */

/* <prefs>/layouts and <prefs>/layouts/<name>.json */
static void named_dir(Panels *P, char *out, size_t n) { snprintf(out, n, "%slayouts", P->dir); }
/* Path of the file of named layout `name`. */
static void named_file(Panels *P, const char *name, char *out, size_t n) { snprintf(out, n, "%slayouts/%s.json", P->dir, name); }

/* a name usable as a file name: letters, digits, space, - and _ */
static bool name_ok(const char *s) {
    if (!s[0] || strlen(s) > 40) return false;
    for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && *p != ' ' && *p != '-' && *p != '_') return false;
    for (int i = 0; i < DOCK_PRESETS; i++) if (!strcasecmp(s, dock_preset_name(i))) return false;   /* presets are protected */
    return true;
}

/* Rewrites bindings.txt from the in-memory connection bindings (errors are ignored). */
static void save_bindings(Panels *P) {
    char path[1100]; snprintf(path, sizeof path, "%slayouts/bindings.txt", P->dir);
    FILE *f = fopen(path, "w"); if (!f) return;
    for (int i = 0; i < P->nnamed; i++) if (P->named[i].conn_id[0]) fprintf(f, "%s\t%s\n", P->named[i].conn_id, P->named[i].name);
    fclose(f);
}

/* Loads every valid layouts/<name>.json (invalid ones are logged and skipped) and then the
 * connection bindings. Called once at startup. */
static void load_named(Panels *P) {
    P->nnamed = 0;
    char dir[1100]; named_dir(P, dir, sizeof dir);
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *de;
    while ((de = readdir(dp)) && P->nnamed < MAX_NAMED) {
        size_t l = strlen(de->d_name);
        if (l < 6 || strcmp(de->d_name + l - 5, ".json")) continue;
        char name[48]; snprintf(name, sizeof name, "%.*s", (int)(l - 5), de->d_name);
        char path[1200]; snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        FILE *f = fopen(path, "rb"); if (!f) continue;
        char *buf = malloc(1 << 20); size_t got = fread(buf, 1, (1 << 20) - 1, f); buf[got] = 0; fclose(f);
        char err[200];
        Named *nm = &P->named[P->nnamed];
        if (dock_parse(&P->dock, buf, got, &nm->st, NULL, 0, err, sizeof err)) { snprintf(nm->name, sizeof nm->name, "%s", name); nm->conn_id[0] = 0; P->nnamed++; }
        else LOG_W("named layout %s ignored: %s", name, err);
        free(buf);
    }
    closedir(dp);
    char path[1100]; snprintf(path, sizeof path, "%s/bindings.txt", dir);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[160];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            char *tab = strchr(line, '\t'); if (!tab) continue; *tab++ = 0;
            for (int i = 0; i < P->nnamed; i++) if (!strcmp(P->named[i].name, tab)) snprintf(P->named[i].conn_id, sizeof P->named[i].conn_id, "%s", line);
        }
        fclose(f);
    }
}

/* Writes state st as layouts/<name>.json via a temp file + rename; false (with a status
 * message) on failure. */
static bool write_named(Panels *P, App *a, const char *name, const DockState *st) {
    char dir[1100]; named_dir(P, dir, sizeof dir);
    mkdir(dir, 0700);
    char path[1200], tmp[1210]; named_file(P, name, path, sizeof path); snprintf(tmp, sizeof tmp, "%s.tmp", path);
    char *txt = dock_serialize(&P->dock, st, name);
    FILE *f = fopen(tmp, "w");
    bool ok = f && fputs(txt, f) >= 0;
    if (f) ok = fclose(f) == 0 && ok;
    free(txt);
    if (!ok || rename(tmp, path) != 0) { remove(tmp); app_status(a, "Could not save the layout \"%s\"", name); return false; }
    return true;
}

/* Index of the named layout called `name`, or -1. */
static int named_find(Panels *P, const char *name) { for (int i = 0; i < P->nnamed; i++) if (!strcmp(P->named[i].name, name)) return i; return -1; }

/* Saves the current arrangement under `name`, creating or overwriting it, and makes it the
 * active named layout. */
static void named_save_as(Panels *P, App *a, const char *name) {
    if (!name_ok(name)) { app_status(a, "\"%s\" is not a usable name (letters, digits, spaces, - and _; not a preset name)", name); return; }
    int i = named_find(P, name);
    if (i < 0) { if (P->nnamed >= MAX_NAMED) { app_status(a, "Up to %d named layouts", MAX_NAMED); return; } i = P->nnamed++; memset(&P->named[i], 0, sizeof P->named[i]); snprintf(P->named[i].name, sizeof P->named[i].name, "%s", name); }
    P->named[i].st = P->dock.st;
    if (write_named(P, a, name, &P->named[i].st)) { snprintf(P->active_name, sizeof P->active_name, "%s", name); app_status(a, "Layout \"%s\" saved", name); }
}

/* Applies named layout i (keeps the editor groups; see dock_apply_state). */
static void named_apply(Panels *P, App *a, int i) {
    if (i < 0 || i >= P->nnamed) return;
    if (!dock_apply_state(&P->dock, &P->named[i].st)) { app_status(a, "Layout \"%s\" not applied: %s", P->named[i].name, P->dock.err); return; }
    snprintf(P->active_name, sizeof P->active_name, "%s", P->named[i].name);
    app_status(a, "Layout \"%s\" applied: the same panels, rearranged (nothing was reopened or rerun)", P->named[i].name);
}

/* Renames layout i on disk and in memory; refused for invalid, preset or taken names. */
static void named_rename(Panels *P, App *a, int i, const char *name) {
    if (i < 0 || i >= P->nnamed) return;
    if (!name_ok(name) || named_find(P, name) >= 0) { app_status(a, "\"%s\" cannot be used (invalid, a preset, or already taken)", name); return; }
    char from[1200], to[1200]; named_file(P, P->named[i].name, from, sizeof from); named_file(P, name, to, sizeof to);
    if (rename(from, to) != 0) { app_status(a, "Could not rename the layout"); return; }
    if (!strcmp(P->active_name, P->named[i].name)) snprintf(P->active_name, sizeof P->active_name, "%s", name);
    snprintf(P->named[i].name, sizeof P->named[i].name, "%s", name);
    save_bindings(P);
    app_status(a, "Layout renamed to \"%s\"", name);
}

/* Deletes layout i (file and entry); the current arrangement is not touched. */
static void named_delete(Panels *P, App *a, int i) {
    if (i < 0 || i >= P->nnamed) return;
    char path[1200]; named_file(P, P->named[i].name, path, sizeof path);
    remove(path);
    app_status(a, "Layout \"%s\" deleted (the current arrangement is unchanged)", P->named[i].name);
    if (!strcmp(P->active_name, P->named[i].name)) P->active_name[0] = 0;
    memmove(&P->named[i], &P->named[i + 1], sizeof P->named[0] * (size_t)(P->nnamed - i - 1));
    P->nnamed--;
    save_bindings(P);
}

/* PNL-034: a connection can carry its own layout; activating it applies that layout */
static void apply_binding_for(Panels *P, App *a, Connection *c) {
    if (!c) return;
    for (int i = 0; i < P->nnamed; i++) if (!strcmp(P->named[i].conn_id, c->id) && strcmp(P->active_name, P->named[i].name)) { named_apply(P, a, i); return; }
}

/* ======================================================================== */
/* drawing helpers                                                           */
/* ======================================================================== */

/* Draws a panel's icon ("db", "grid", "log", "info", "history", "outline" / "md", "stats", "plan", "ai",
 * "git"; any other name shows its first letter)
 * centred on (cx, cy), sz pixels wide. bg is the colour behind it (for cut-outs). */
static void draw_panel_icon(App *a, const char *icon, float cx, float cy, float sz, Color c, Color bg) {
    SDL_Renderer *r = a->renderer;
    if (!strcmp(icon, "db")) draw_icon_db(r, cx, cy, sz, c, bg);
    else if (!strcmp(icon, "grid")) draw_icon_table(r, cx, cy, sz, c, color_hex(T_ACCENT_DIM), bg);
    else if (!strcmp(icon, "log")) {
        for (int i = 0; i < 3; i++) draw_rect(r, rect_make(cx - sz / 2, cy - sz / 3 + i * sz / 3 - px(a, 0.75f), i == 2 ? sz * 0.6f : sz, px(a, 1.5f)), c);
    } else if (!strcmp(icon, "info")) {
        draw_rect_outline(r, rect_make(cx - sz / 2, cy - sz / 2, sz, sz), px(a, 1.5f), c);
        draw_rect(r, rect_make(cx - px(a, 0.75f), cy - sz / 6, px(a, 1.5f), sz / 2.2f), c);
        draw_rect(r, rect_make(cx - px(a, 0.75f), cy - sz / 3, px(a, 1.5f), px(a, 1.5f)), c);
    } else if (!strcmp(icon, "history")) {   /* a clock */
        float t = px(a, 1.5f), rr = sz / 2;
        draw_rrect(r, rect_make(cx - rr, cy - rr, sz, sz), rr, c);
        draw_rrect(r, rect_make(cx - rr + t, cy - rr + t, sz - 2 * t, sz - 2 * t), rr - t, bg);
        draw_line(r, cx, cy, cx, cy - rr * 0.6f, t, c);
        draw_line(r, cx, cy, cx + rr * 0.45f, cy + rr * 0.15f, t, c);
    } else if (!strcmp(icon, "md") || !strcmp(icon, "outline")) {   /* a document outline: headings and indented items */
        float t = px(a, 1.5f);
        static const float ind[4] = { 0, 0.3f, 0.3f, 0 }, len[4] = { 1, 0.7f, 0.55f, 0.85f };
        for (int i = 0; i < 4; i++) {
            float y = cy - sz / 2 + i * sz / 3.2f + t / 2, x = cx - sz / 2 + sz * ind[i];
            draw_rect(r, rect_make(x, y, t * 1.4f, t), c);
            draw_rect(r, rect_make(x + t * 2.6f, y, (sz - sz * ind[i] - t * 2.6f) * len[i], t), c);
        }
    } else if (!strcmp(icon, "stats")) {   /* a bar chart */
        float t = px(a, 1.5f), bw = sz / 4.6f;
        static const float h[3] = { 0.45f, 0.95f, 0.7f };
        for (int i = 0; i < 3; i++) draw_rect(r, rect_make(cx - sz / 2 + i * (bw + sz / 9) + sz / 12, cy + sz / 2 - sz * h[i], bw, sz * h[i]), c);
        draw_rect(r, rect_make(cx - sz / 2, cy + sz / 2, sz, t * 0.8f), c);
    } else if (!strcmp(icon, "plan")) {   /* an operator tree, root on the left (as the plan graph) */
        float t = px(a, 1.5f), b = sz * 0.34f;
        Rect root = rect_make(cx - sz / 2, cy - b / 2, b, b), up = rect_make(cx + sz / 2 - b, cy - sz / 2, b, b), dn = rect_make(cx + sz / 2 - b, cy + sz / 2 - b, b, b);
        draw_rect_outline(r, root, t, c); draw_rect(r, up, c); draw_rect(r, dn, c);
        float mx = cx - sz * 0.02f;
        draw_line(r, root.x + b, cy, mx, cy, t, c);
        draw_line(r, mx, up.y + b / 2, mx, dn.y + b / 2, t, c);
        draw_line(r, mx, up.y + b / 2, up.x, up.y + b / 2, t, c);
        draw_line(r, mx, dn.y + b / 2, dn.x, dn.y + b / 2, t, c);
    } else if (!strcmp(icon, "ai")) {   /* a sparkle: a four-pointed star and a small one */
        /* each point is a pair of triangles, so the star reads as filled at small sizes */
        float big = sz * 0.9f, bx = cx - sz * 0.1f, by = cy + sz * 0.08f, w = big * 0.42f;
        draw_triangle(r, bx, by - big / 4, w, big / 2, false, c);
        draw_triangle(r, bx, by + big / 4, w, big / 2, true, c);
        for (int k = -1; k <= 1; k += 2) for (int q = 0; q < 3; q++) { float o = (q - 1) * px(a, 0.8f); draw_line(r, bx, by + o, bx + k * big / 2, by, px(a, 1.1f), c); }
        draw_rrect(r, rect_make(bx - w * 0.6f, by - w * 0.6f, w * 1.2f, w * 1.2f), w * 0.6f, c);
        float sx = cx + sz * 0.36f, sy = cy - sz * 0.32f, s2 = sz * 0.34f;
        draw_triangle(r, sx, sy - s2 / 4, s2 * 0.45f, s2 / 2, false, c);
        draw_triangle(r, sx, sy + s2 / 4, s2 * 0.45f, s2 / 2, true, c);
        draw_line(r, sx - s2 / 2, sy, sx + s2 / 2, sy, px(a, 1.3f), c);
    } else if (!strcmp(icon, "git")) {   /* a branch: main line with a commit, a branch leaving it */
        float t = px(a, 1.6f), d = sz * 0.3f;
        float lx = cx - sz * 0.22f, rx = cx + sz * 0.26f, ty = cy - sz * 0.38f, byy = cy + sz * 0.38f, my = cy - sz * 0.12f;
        draw_line(r, lx, ty, lx, byy, t, c);
        draw_line(r, rx, my + d / 2, rx, cy + sz * 0.05f, t, c);
        draw_line(r, rx, cy + sz * 0.05f, lx, cy + sz * 0.28f, t, c);
        Rect dots[3] = { rect_make(lx - d / 2, ty - d / 2, d, d), rect_make(lx - d / 2, byy - d / 2, d, d), rect_make(rx - d / 2, my - d / 2, d, d) };
        for (int i = 0; i < 3; i++) { draw_rrect(r, dots[i], d / 2, c); draw_rrect(r, rect_inset(dots[i], t * 0.9f, t * 0.9f), d / 2 - t * 0.9f, bg); }
    } else {
        char l[2] = { (char)toupper((unsigned char)(icon && icon[0] ? icon[0] : '?')), 0 };
        draw_text_fit(a->font_ui, r, l, -1, rect_make(cx - sz / 2, cy - sz / 2, sz, sz), c, ALIGN_CENTER);
    }
}

/* Small badge next to an icon or title: separate shapes for loading / error / warning /
 * unsaved, so the meaning never depends on colour alone (PNL-037). */
/* Returns the width drawn, in pixels. */
static float draw_badge(App *a, const DockBadge *b, float x, float cy) {
    SDL_Renderer *r = a->renderer;
    float x0 = x;
    char t[24];
    if (b->loading) { draw_rect_outline(r, rect_make(x, cy - px(a, 4), px(a, 8), px(a, 8)), px(a, 1.5f), color_hex(T_ACCENT)); x += px(a, 12); }
    if (b->error) { draw_cross(r, x + px(a, 4), cy, px(a, 7), px(a, 1.6f), color_hex(T_RED)); x += px(a, 12); }
    else if (b->warning) { draw_triangle(r, x + px(a, 4), cy, px(a, 8), px(a, 7), false, color_hex(T_YELLOW)); x += px(a, 12); }
    if (b->unsaved) { snprintf(t, sizeof t, "\xE2\x80\xA2%d", b->unsaved); float w = font_width(a->font_small, t, -1); draw_text_fit(a->font_small, r, t, -1, rect_make(x, cy - px(a, 8), w + px(a, 2), px(a, 16)), color_hex(T_YELLOW), ALIGN_LEFT); x += w + px(a, 4); }
    if (b->count > 0 && !b->error && !b->warning) { snprintf(t, sizeof t, "%d", b->count); float w = font_width(a->font_small, t, -1); draw_text_fit(a->font_small, r, t, -1, rect_make(x, cy - px(a, 8), w + px(a, 2), px(a, 16)), color_hex(T_TEXT_DIM), ALIGN_LEFT); x += w + px(a, 4); }
    return x - x0;
}

/* Current badge of panel p (all zero when the panel has no badge callback). */
static DockBadge badge_of(Panels *P, App *a, int p) {
    DockBadge b; memset(&b, 0, sizeof b);
    if (P->dock.desc[p].badge) P->dock.desc[p].badge(panel_view(P, a, p), a, &b);
    return b;
}

/* "Results · 3 tabs · 2 unsaved rows · error" for tooltips and strips */
static void describe_panel(Panels *P, App *a, int p, char *out, size_t n) {
    static const char *const rn[] = { "left", "right", "bottom", "top" };
    const DockPanelDesc *d = &P->dock.desc[p];
    DockBadge b = badge_of(P, a, p);
    const char *state = P->dock.st.panel[p].mode == DOCK_HIDDEN ? "hidden" : P->dock.st.panel[p].mode == DOCK_AUTOHIDE ? "auto-hide" :
                        P->dock.st.panel[p].mode == DOCK_FLOATING ? "floating" : dock_is_collapsed(&P->dock, p) ? "collapsed" : "docked";
    char keys[32]; chord_text(P, ACT_PANEL_EXPLORER + (p == P->P_RS ? 1 : p == P->P_OUT ? 2 : p == P->P_IN ? 3 : 0), keys, sizeof keys);
    if (p != P->P_EX && p != P->P_RS && p != P->P_OUT && p != P->P_IN) keys[0] = 0;
    size_t l = (size_t)snprintf(out, n, "%s  \xC2\xB7  %s %s", d->title, state, rn[dock_region_of(&P->dock, p)]);
    if (b.count && l < n) l += (size_t)snprintf(out + l, n - l, "  \xC2\xB7  %d %s", b.count, p == P->P_RS ? "tabs" : "new");
    if (b.unsaved && l < n) l += (size_t)snprintf(out + l, n - l, "  \xC2\xB7  %d unsaved row%s", b.unsaved, b.unsaved == 1 ? "" : "s");
    if (b.loading && l < n) l += (size_t)snprintf(out + l, n - l, "  \xC2\xB7  running");
    if (b.error && l < n) l += (size_t)snprintf(out + l, n - l, "  \xC2\xB7  error");
    else if (b.warning && l < n) l += (size_t)snprintf(out + l, n - l, "  \xC2\xB7  warning");
    if (keys[0] && l < n) snprintf(out + l, n - l, "  \xC2\xB7  %s", keys);
}

/* Call every frame for each widget with a tooltip: when the mouse has rested on rc for
 * 500 ms, schedules `text` to be drawn by panels_draw_overlay(). id must be unique per
 * widget (ranges: 999 / 1000+p bar, 1100+p strips, 1200+p tabs, 1300-1500+g header buttons,
 * 1600+eg split, 1700+tab id editor tabs). */
static void tooltip(Panels *P, App *a, int id, Rect rc, const char *text) {
    if (!rect_contains(rc, a->in.mouse_x, a->in.mouse_y)) { if (P->tt_id == id) P->tt_id = -1; return; }
    Uint64 now = SDL_GetTicks();
    if (P->tt_id != id) { P->tt_id = id; P->tt_since = now; app_wake_in(a, 520); return; }
    if (now - P->tt_since >= 500) { P->tt_show = true; P->tt_rect = rc; snprintf(P->tt_text, sizeof P->tt_text, "%s", text); }
}

/* ======================================================================== */
/* 10. context menus                                                         */
/* ======================================================================== */

/* Label buffer i for popup entries; popups keep the pointer, so labels live in P. */
static char *mt(Panels *P, int i) { return P->mtext[i % 48]; }

/* Actions on panel p, appended to popup m (the context menu or a submenu of "Panels…"). */
static void fill_panel_menu(Panels *P, App *a, PopupMenu *m, int p) {
    Dock *d = &P->dock;
    const DockPanelDesc *ds = &d->desc[p];
    bool vis = dock_is_visible(d, p), col = dock_is_collapsed(d, p);
    int g = dock_group_of(d, p);
    snprintf(mt(P, 0), 112, "%s", ds->title);
    popup_add_caption(m, mt(P, 0), T_TEXT_MUTED, false);
    if (!vis) popup_add(m, PM_OP_BASE + OP_SHOW, col ? "Expand" : "Show", false, true);
    popup_add(m, PM_OP_BASE + OP_HIDE, "Hide (content kept)", false, (ds->caps & DOCK_CAP_HIDE) && d->st.panel[p].mode != DOCK_HIDDEN);
    if (g >= 0) popup_add(m, PM_OP_BASE + (col ? OP_EXPAND : OP_COLLAPSE), col ? "Expand group" : "Collapse group to the edge", false, (ds->caps & DOCK_CAP_COLLAPSE) != 0);
    popup_add(m, PM_OP_BASE + OP_MAXIMIZE, d->maximized_group >= 0 ? "Restore layout" : "Maximize", false, d->maximized_group >= 0 || (g >= 0 && !col));
    popup_add(m, PM_MOVE, "Move to  \xE2\x80\xBA", true, (ds->caps & DOCK_CAP_MOVE) != 0);
    popup_add(m, PM_SPLIT, "Split  \xE2\x80\xBA", false, g >= 0);   /* each entry says whether it applies */
    popup_add(m, PM_MODE, "Mode  \xE2\x80\xBA", false, true);
    popup_add(m, PM_OP_BASE + OP_BAR, d->st.panel[p].in_bar ? "Remove from panel bar" : "Add to panel bar", true, true);
    popup_add(m, PM_OP_BASE + OP_RESET_SIZE, "Reset panel size", false, g >= 0);
    popup_add(m, PM_RESET_LAYOUT, "Reset layout to Default", false, true);
    /* the panel's own actions (PNL-047), after the shell's */
    P->next_items = 0; P->ext_panel = p;
    if (ds->menu) {
        int n = ds->menu(panel_view(P, a, p), a, P->ext_items, DOCK_MAX_MENU_ITEMS);
        if (n > DOCK_MAX_MENU_ITEMS) n = DOCK_MAX_MENU_ITEMS;
        for (int i = 0; i < n; i++) {
            P->ext_items[i].label[sizeof P->ext_items[i].label - 1] = 0;
            popup_add(m, PM_EXT_BASE + i, P->ext_items[i].label, i == 0 || P->ext_items[i].sep_before, P->ext_items[i].enabled);
        }
        P->next_items = n > 0 ? n : 0;
    }
}

/* "Move to" submenu: the four edges, the current one marked, Top disabled when turned off. */
static void fill_move_menu(Panels *P, PopupMenu *m, int p) {
    Dock *d = &P->dock;
    int cur = dock_group_of(d, p) >= 0 ? dock_region_of(d, p) : -1;
    static const char *const names[] = { "Left", "Right", "Bottom", "Top" };
    for (int r = 0; r < DOCK_REGIONS; r++) {
        bool ok = r != DOCK_TOP || d->st.top_enabled;
        if (r == DOCK_TOP && !ok) { snprintf(mt(P, 1), 112, "Top (turned off: Window > Allow Top Edge)"); popup_add(m, PM_OP_BASE + OP_MOVE_L + r, mt(P, 1), false, false); continue; }
        snprintf(mt(P, 2 + r), 112, "%s%s", names[r], r == cur ? "  (current)" : "");
        popup_add(m, PM_OP_BASE + OP_MOVE_L + r, mt(P, 2 + r), false, ok);
    }
}

/* "Mode" submenu: docked, auto-hide, floating window, separate window. */
static void fill_mode_menu(Panels *P, PopupMenu *m, int p) {
    Dock *d = &P->dock;
    int mode = d->st.panel[p].mode;
    popup_add(m, PM_OP_BASE + OP_MODE_DOCKED, mode == DOCK_DOCKED ? "Docked, pinned  (current)" : "Docked, pinned", false, true);
    popup_add(m, PM_OP_BASE + OP_MODE_AUTOHIDE, mode == DOCK_AUTOHIDE ? "Auto-hide overlay  (current)" : "Auto-hide overlay", false, (d->desc[p].caps & DOCK_CAP_AUTOHIDE) != 0);
    popup_add(m, PM_OP_BASE + OP_MODE_FLOAT, mode == DOCK_FLOATING ? "Floating window, above Lume  (current)" : "Floating window, above Lume", false, (d->desc[p].caps & DOCK_CAP_FLOAT) != 0);
    popup_add(m, PM_OP_BASE + OP_MODE_DETACH, mode == DOCK_DETACHED ? "Separate window, any monitor  (current)" : "Separate window, any monitor", false, (d->desc[p].caps & DOCK_CAP_FLOAT) != 0);
}

/* Opens the context menu of panel p at (x, y), render pixels. */
static void open_panel_menu(Panels *P, App *a, int p, float x, float y) {
    P->menu_panel = p;
    popup_open(&P->menu, x, y);
    fill_panel_menu(P, a, &P->menu, p);
}

/* "Panels…" (Window menu, PNL-006 / 037): every registered panel, pinned or not */
static void open_panels_list(Panels *P, App *a, float x, float y, bool only_unpinned) {
    popup_open(&P->menu, x, y);
    P->menu_panel = -1;
    popup_add_caption(&P->menu, only_unpinned ? "MORE PANELS  \xC2\xB7  NOT ON THE BAR" : "PANELS  \xC2\xB7  EVERY REGISTERED PANEL", 0, false);
    int n = 0;
    for (int p = 0; p < P->dock.ndesc; p++) {
        if (only_unpinned && P->dock.st.panel[p].in_bar) continue;
        char d[160]; describe_panel(P, a, p, d, sizeof d);
        snprintf(mt(P, 10 + p), 112, "%s  \xE2\x80\xBA", d);
        popup_add(&P->menu, PM_PANEL_BASE + p, mt(P, 10 + p), false, true);
        n++;
    }
    if (!n) popup_add_caption(&P->menu, "Every panel is on the bar", T_TEXT_DIM, false);
}

/* "Manage Layouts…": presets (apply only) and named layouts (apply / save / rename / delete / bind) */
static void open_layouts_menu(Panels *P, App *a, float x, float y) {
    (void)a;
    popup_open(&P->menu, x, y);
    P->menu_panel = -1;
    popup_add_caption(&P->menu, "PRESETS  \xC2\xB7  PROTECTED", 0, false);
    for (int i = 0; i < DOCK_PRESETS; i++) { snprintf(mt(P, 30 + i), 112, "%s", dock_preset_name(i)); popup_add(&P->menu, PM_PRESET_BASE + i, mt(P, 30 + i), false, true); }
    popup_add_caption(&P->menu, P->nnamed ? "SAVED LAYOUTS" : "SAVED LAYOUTS  \xC2\xB7  none yet (Window > Save Layout As…)", 0, true);
    for (int i = 0; i < P->nnamed && i < 12; i++) {
        snprintf(mt(P, 34 + i), 112, "%s%s%s  \xE2\x80\xBA", P->named[i].name, !strcmp(P->active_name, P->named[i].name) ? "  (current)" : "", P->named[i].conn_id[0] ? "  \xC2\xB7  bound to a connection" : "");
        popup_add(&P->menu, PM_LAYOUT_BASE + i, mt(P, 34 + i), false, true);
    }
}

static bool panel_op(Panels *P, App *a, int p, int op);

/* Runs the popup entry `id` chosen this frame: opens a submenu (panel, Move / Split / Mode,
 * named layout) or performs the action (preset, layout operations, panel operation). */
static void handle_menu_choice(Panels *P, App *a, int id) {
    Dock *d = &P->dock;
    if (id >= PM_EXT_BASE && id < PM_EXT_BASE + P->next_items && P->ext_panel >= 0 && P->ext_panel < d->ndesc) {   /* a panel's own item */
        const DockPanelDesc *ds = &d->desc[P->ext_panel];
        int item = P->ext_items[id - PM_EXT_BASE].id;
        popup_close(&P->menu);
        if (ds->menu_run) ds->menu_run(panel_view(P, a, P->ext_panel), a, item);
        return;
    }
    if (id >= PM_PANEL_BASE && id < PM_PANEL_BASE + DOCK_MAX_PANELS) {         /* "Panels…" > panel */
        P->menu_panel = id - PM_PANEL_BASE;
        PopupMenu *m = popup_open_sub(&P->menu, &P->sub);
        fill_panel_menu(P, a, m, P->menu_panel);
        return;
    }
    if (id == PM_MOVE || id == PM_SPLIT || id == PM_MODE) {                    /* third level from Panels…, second from a tab */
        PopupMenu *parent = P->sub.open && P->sub.parent == &P->menu ? &P->sub : &P->menu;
        PopupMenu *m = popup_open_sub(parent, parent == &P->menu ? &P->sub : &P->sub2);
        if (id == PM_MOVE) fill_move_menu(P, m, P->menu_panel);
        else if (id == PM_MODE) fill_mode_menu(P, m, P->menu_panel);
        else {
            int g = dock_group_of(&P->dock, P->menu_panel), r = g >= 0 ? P->dock.st.group[g].region : DOCK_LEFT;
            bool side = r == DOCK_LEFT || r == DOCK_RIGHT, alone = g >= 0 && P->dock.st.group[g].ntabs == 1;
            popup_add(m, PM_OP_BASE + OP_SPLIT_BEFORE, side ? "New group above" : "New group on the left", false, !alone);
            popup_add(m, PM_OP_BASE + OP_SPLIT_AFTER, side ? "New group below" : "New group on the right", false, !alone);
            bool lanes_ok = g >= 0 && dock_lanes(&P->dock.st, r) < DOCK_MAX_LANES;
            popup_add(m, PM_OP_BASE + OP_LANE_BEFORE, side ? "Side by side, on the left" : "Stacked, above", true, lanes_ok);
            popup_add(m, PM_OP_BASE + OP_LANE_AFTER, side ? "Side by side, on the right" : "Stacked, below", false, lanes_ok);
        }
        return;
    }
    if (id >= PM_LAYOUT_BASE && id < PM_LAYOUT_BASE + MAX_NAMED) {
        P->menu_layout = id - PM_LAYOUT_BASE;
        PopupMenu *m = popup_open_sub(&P->menu, &P->sub);
        Named *nm = &P->named[P->menu_layout];
        popup_add(m, PM_LAYOUT_APPLY, "Apply", false, true);
        popup_add(m, PM_LAYOUT_SAVE, "Save current arrangement into it", false, true);
        popup_add(m, PM_LAYOUT_RENAME, "Rename\xE2\x80\xA6", false, true);
        popup_add(m, PM_LAYOUT_DELETE, "Delete\xE2\x80\xA6", false, true);
        snprintf(mt(P, 47), 112, nm->conn_id[0] ? "Stop using it for its connection" : "Use for the current connection%s", a->active ? "" : " (none active)");
        popup_add(m, PM_LAYOUT_BIND, mt(P, 47), true, nm->conn_id[0] || a->active);
        return;
    }
    if (id >= PM_PRESET_BASE && id < PM_PRESET_BASE + DOCK_PRESETS) {
        if (dock_apply_preset(d, id - PM_PRESET_BASE)) { P->active_name[0] = 0; app_status(a, "Layout: %s (panels rearranged; queries, tabs and edits untouched)", dock_preset_name(id - PM_PRESET_BASE)); }
        return;
    }
    switch (id) {
    case PM_LAYOUT_APPLY: named_apply(P, a, P->menu_layout); return;
    case PM_LAYOUT_SAVE: if (P->menu_layout < P->nnamed) { P->named[P->menu_layout].st = d->st; if (write_named(P, a, P->named[P->menu_layout].name, &d->st)) app_status(a, "Layout \"%s\" updated", P->named[P->menu_layout].name); } return;
    case PM_LAYOUT_RENAME:
        P->modal = MODAL_NAME; P->name_purpose = NAME_RENAME; P->name_index = P->menu_layout;
        textfield_set(&P->field, P->named[P->menu_layout].name); P->field.blink = SDL_GetTicks();
        return;
    case PM_LAYOUT_DELETE: {
        char msg[200]; snprintf(msg, sizeof msg, "Delete the layout \"%s\"? The current arrangement is not changed.", P->named[P->menu_layout].name);
        open_confirm(P, CONFIRM_DELETE_LAYOUT, P->menu_layout, msg);
        return;
    }
    case PM_LAYOUT_BIND: {
        Named *nm = &P->named[P->menu_layout];
        if (nm->conn_id[0]) { nm->conn_id[0] = 0; app_status(a, "\"%s\" is no longer tied to a connection", nm->name); }
        else if (a->active) {
            for (int i = 0; i < P->nnamed; i++) if (!strcmp(P->named[i].conn_id, a->active->id)) P->named[i].conn_id[0] = 0;
            snprintf(nm->conn_id, sizeof nm->conn_id, "%s", a->active->id);
            snprintf(P->active_name, sizeof P->active_name, "%s", nm->name);
            app_status(a, "\"%s\" now opens with %s", nm->name, a->active->name);
        }
        save_bindings(P);
        return;
    }
    case PM_RESET_LAYOUT: panels_action(P, a, ACT_LAYOUT_DEFAULT); return;
    default: break;
    }
    if (id > PM_OP_BASE && id < PM_OP_BASE + 50 && P->menu_panel >= 0) panel_op(P, a, P->menu_panel, id - PM_OP_BASE);
}

/* ======================================================================== */
/* panel operations (menus, palette, shortcuts share them: R-10)             */
/* ======================================================================== */

/* Performs OP_* on panel p through the engine and reports the outcome (or the engine's
 * refusal) on the status bar. Returns true when applied. */
static bool panel_op(Panels *P, App *a, int p, int op) {
    Dock *d = &P->dock;
    if (p < 0 || p >= d->ndesc) return false;
    const char *t = d->desc[p].title;
    bool ok = true;
    switch (op) {
    case OP_SHOW: ok = dock_show(d, p); break;
    case OP_HIDE: ok = dock_hide(d, p); if (ok) app_status(a, "%s hidden; its content is kept (Window > Panels or %s to bring it back)", t, d->desc[p].shortcut ? "its shortcut" : "the palette"); break;
    case OP_COLLAPSE: ok = dock_collapse_group(d, dock_group_of(d, p)); break;
    case OP_EXPAND: ok = dock_expand_group(d, dock_group_of(d, p)); break;
    case OP_MAXIMIZE: ok = dock_toggle_maximize(d, dock_group_of(d, p)); break;
    case OP_MOVE_L: case OP_MOVE_R: case OP_MOVE_B: case OP_MOVE_T: ok = dock_move_to_region(d, p, op - OP_MOVE_L); break;
    case OP_SPLIT_BEFORE: case OP_SPLIT_AFTER: ok = dock_split_group(d, p, dock_group_of(d, p), op == OP_SPLIT_AFTER); break;
    case OP_LANE_BEFORE: case OP_LANE_AFTER: ok = dock_split_lane(d, p, dock_group_of(d, p), op == OP_LANE_AFTER); break;
    case OP_MODE_DOCKED: ok = dock_set_mode(d, p, DOCK_DOCKED); break;
    case OP_MODE_AUTOHIDE: ok = dock_set_mode(d, p, DOCK_AUTOHIDE); if (ok) app_status(a, "%s is in auto-hide: its bar button opens it over the editor; Esc or a click elsewhere puts it away", t); break;
    case OP_MODE_FLOAT: ok = dock_set_mode(d, p, DOCK_FLOATING); if (ok) app_status(a, "%s floats in its own window; closing that window docks it back (nothing is closed)", t); break;
    case OP_MODE_DETACH: ok = dock_set_mode(d, p, DOCK_DETACHED); if (ok) app_status(a, "%s is in a separate window: move it to any monitor; closing it docks the panel back", t); break;
    case OP_BAR: ok = dock_set_in_bar(d, p, !d->st.panel[p].in_bar); break;
    case OP_RESET_SIZE: ok = dock_reset_region_size(d, dock_region_of(d, p)); break;
    case OP_FOCUS:
        ok = dock_show(d, p);
        if (ok) { P->last_panel = p; a->focus = p == P->P_EX ? FOCUS_EXPLORER : FOCUS_RESULTS; }
        break;
    default: ok = false; break;
    }
    if (!ok && d->err[0]) app_status(a, "%s: %s", t, d->err);
    return ok;
}

/* ======================================================================== */
/* 11. palette (PNL-036) and modals                                          */
/* ======================================================================== */

static int pending_action;    /* palette / menu choice for main.c (panels_take_action) */
/* Returns and clears the ACT_* chosen in the palette (file-level, not per Panels). */
int panels_take_action(Panels *P) { (void)P; int a = pending_action; pending_action = ACT_NONE; return a; }

/* Appends a palette command (silently dropped past MAX_CMDS). */
static void add_cmd(Panels *P, const char *label, const char *keys, int kind, int a1, int a2) {
    if (P->ncmds >= MAX_CMDS) return;
    Cmd *c = &P->cmds[P->ncmds++];
    snprintf(c->label, sizeof c->label, "%s", label); snprintf(c->keys, sizeof c->keys, "%s", keys ? keys : "");
    c->kind = kind; c->a1 = a1; c->a2 = a2;
}

/* Rebuilds the palette list: every menu entry, every panel x operation, saved layouts. */
static void build_commands(Panels *P, App *a) {
    (void)a;
    P->ncmds = 0;
    /* every menu action, named as in the menus */
    int n = menu_item_count();
    for (int i = 0; i < n; i++) {
        const char *label, *menu; char keys[32]; int act;
        if (!menu_item_at(i, &menu, &label, keys, sizeof keys, &act)) continue;
        if (act == ACT_PALETTE) continue;
        char l[112]; snprintf(l, sizeof l, "%s: %s", menu, label);
        add_cmd(P, l, keys, CMD_ACTION, act, 0);
        /* search also by the command's synonyms and its shortcut spelled out (TBR-005) */
        if (P->ncmds) {
            Cmd *c = &P->cmds[P->ncmds - 1];
            const Command *rc = commands_by_act(act);
            char spelled[64] = ""; size_t sl = 0;
            for (const char *k = keys; *k && sl < sizeof spelled - 8; ) {
                const char *w = NULL; int adv = 1;
                if (!strncmp(k, "\xE2\x8C\x98", 3)) { w = "cmd "; adv = 3; } else if (!strncmp(k, "\xE2\x87\xA7", 3)) { w = "shift "; adv = 3; }
                else if (!strncmp(k, "\xE2\x8C\xA5", 3)) { w = "alt option "; adv = 3; } else if (!strncmp(k, "\xE2\x86\xA9", 3)) { w = "enter return "; adv = 3; }
                else if (!strncmp(k, "\xE2\x8C\x83", 3)) { w = "ctrl "; adv = 3; }
                if (w) { sl += (size_t)snprintf(spelled + sl, sizeof spelled - sl, "%s", w); k += adv; }
                else { if ((unsigned char)*k < 0x80) spelled[sl++] = *k; spelled[sl] = 0; k++; }
            }
            snprintf(c->search, sizeof c->search, "%s %s %s", rc && rc->synonyms ? rc->synonyms : "", keys, spelled);
            if (rc) snprintf(c->cmd_id, sizeof c->cmd_id, "%s", rc->id);
        }
    }
    /* registry commands with no menu entry (extension actions, TBR-050) */
    for (int i = 0; i < commands_count(); i++) {
        const Command *rc = commands_at(i);
        if (!rc || rc->act) continue;
        char l[112]; snprintf(l, sizeof l, "%s: %s", rc->group ? rc->group : "Action", rc->label);
        add_cmd(P, l, "", CMD_INVOKE, 0, 0);
        if (P->ncmds) { snprintf(P->cmds[P->ncmds - 1].cmd_id, 40, "%s", rc->id); snprintf(P->cmds[P->ncmds - 1].search, 200, "%s", rc->synonyms ? rc->synonyms : ""); }
    }
    /* panel commands, one per panel and operation */
    static const struct { int op; const char *fmt; } ops[] = {
        { OP_FOCUS, "Panel: Show and focus %s" }, { OP_HIDE, "Panel: Hide %s" }, { OP_COLLAPSE, "Panel: Collapse %s" },
        { OP_MAXIMIZE, "Panel: Maximize / restore %s" }, { OP_MOVE_L, "Panel: Move %s to the left" }, { OP_MOVE_R, "Panel: Move %s to the right" },
        { OP_MOVE_B, "Panel: Move %s to the bottom" }, { OP_MODE_AUTOHIDE, "Panel: Auto-hide %s" }, { OP_MODE_DOCKED, "Panel: Dock and pin %s" },
        { OP_MODE_FLOAT, "Panel: Float %s in a window" }, { OP_MODE_DETACH, "Panel: Move %s to a separate window" }, { OP_BAR, "Panel: Add / remove %s on the bar" },
    };
    for (int p = 0; p < P->dock.ndesc; p++)
        for (size_t k = 0; k < sizeof ops / sizeof ops[0]; k++) {
            char l[112]; snprintf(l, sizeof l, ops[k].fmt, P->dock.desc[p].title);
            add_cmd(P, l, NULL, CMD_PANEL, p, ops[k].op);
        }
    /* presets are already there as Window menu actions; only saved layouts are added */
    for (int i = 0; i < P->nnamed; i++) { char l[112]; snprintf(l, sizeof l, "Layout: %s (saved)", P->named[i].name); add_cmd(P, l, NULL, CMD_NAMED, i, 0); }
}

/* every word of the query must appear in the label, case-insensitively */
static bool cmd_matches(const char *label, const char *q) {
    char buf[256]; snprintf(buf, sizeof buf, "%s", q);
    for (char *w = strtok(buf, " "); w; w = strtok(NULL, " ")) if (!strcasestr(label, w)) return false;
    return true;
}
/* A command matches on its label, or on its label plus synonyms and spelled shortcut. */
static bool cmd_matches_full(const Cmd *c, const char *q) {
    if (cmd_matches(c->label, q)) return true;
    if (!c->search[0]) return false;
    char all[320]; snprintf(all, sizeof all, "%s %s", c->label, c->search);
    return cmd_matches(all, q);
}

/* Recomputes the commands matching the text field and keeps the selection in range. */
static void palette_filter(Panels *P) {
    /* ranked: a match in the label before a match only in the synonyms, and what can run now before what
     * cannot (a disabled command is never the first, preselected one) */
    P->nshown = 0;
    for (int band = 0; band < 3; band++)
        for (int i = 0; i < P->ncmds; i++) {
            const Cmd *c = &P->cmds[i];
            if (!cmd_matches_full(c, P->field.buf)) continue;
            char why[160]; bool en = !c->cmd_id[0] || commands_enabled(c->cmd_id, why, sizeof why);
            int b = !en ? 2 : cmd_matches(c->label, P->field.buf) ? 0 : 1;
            if (b == band) P->shown[P->nshown++] = i;
        }
    if (P->pal_sel >= P->nshown) P->pal_sel = P->nshown ? P->nshown - 1 : 0;
}

/* Executes a palette command; menu actions are deferred to main.c via pending_action. */
static void run_cmd(Panels *P, App *a, const Cmd *c) {
    switch (c->kind) {
    case CMD_ACTION: pending_action = c->a1; break;
    case CMD_PANEL: panel_op(P, a, c->a1, c->a2); break;
    case CMD_PRESET: if (dock_apply_preset(&P->dock, c->a1)) { P->active_name[0] = 0; app_status(a, "Layout: %s", dock_preset_name(c->a1)); } break;
    case CMD_NAMED: named_apply(P, a, c->a1); break;
    case CMD_INVOKE: commands_invoke(a, c->cmd_id); break;
    }
}

/* Harness (TBR-005): what the palette lists for a query: label, shortcut, enabled or why not. */
void panels_palette_dump(Panels *P, App *a, const char *query) {
    build_commands(P, a);
    snprintf(P->field.buf, sizeof P->field.buf, "%s", query); P->field.len = (int)strlen(P->field.buf);
    palette_filter(P);
    for (int i = 0; i < P->nshown && i < 12; i++) {
        const Cmd *c = &P->cmds[P->shown[i]];
        char why[160] = ""; bool en = !c->cmd_id[0] || commands_enabled(c->cmd_id, why, sizeof why);
        printf("palette %s  [%s]%s%s\n", c->label, c->keys, en ? "" : "  disabled: ", en ? "" : why);
    }
    printf("palette (%d match%s)\n", P->nshown, P->nshown == 1 ? "" : "es");
    textfield_set(&P->field, "");
    fflush(stdout);
}

/* Opens the command palette with an empty query. */
static void open_palette(Panels *P, App *a) {
    build_commands(P, a);
    P->modal = MODAL_PALETTE; P->pal_sel = 0; P->pal_scroll = 0;
    textfield_set(&P->field, ""); P->field.blink = SDL_GetTicks();
    palette_filter(P);
}

/* Opens the confirmation dialog for CONFIRM_* kind with its argument and message. */
static void open_confirm(Panels *P, int kind, int arg, const char *text) {
    P->modal = MODAL_CONFIRM; P->confirm_kind = kind; P->confirm_arg = arg;
    snprintf(P->confirm_text, sizeof P->confirm_text, "%s", text);
}

/* Carries out the confirmed action stored in confirm_kind / confirm_arg. */
static void confirm_yes(Panels *P, App *a) {
    switch (P->confirm_kind) {
    case CONFIRM_RESET:
        if (dock_apply_preset(&P->dock, DOCK_PRESET_DEFAULT)) { P->active_name[0] = 0; app_status(a, "Layout reset to Default: panels rearranged; queries, result tabs and edits are untouched"); }
        break;
    case CONFIRM_IMPORT:
        if (dock_apply_state(&P->dock, &P->import_st)) { P->active_name[0] = 0; app_status(a, "Imported layout \"%s\" applied", P->import_name); }
        else app_status(a, "Import refused: %s", P->dock.err);
        break;
    case CONFIRM_CLOSE_TAB: close_tab_now(P, a, P->confirm_arg); break;
    case CONFIRM_DELETE_LAYOUT: named_delete(P, a, P->confirm_arg); break;
    }
}

/* Modal input; returns true while a modal is open (it owns the keyboard). */
static bool modal_input(Panels *P, App *a) {
    Input *in = &a->in;
    if (P->modal == MODAL_NONE) return false;
    if (input_key(in, SDLK_ESCAPE, 0)) { P->modal = MODAL_NONE; return true; }
    if (P->modal == MODAL_PALETTE) {
        if (input_key(in, SDLK_DOWN, 0) && P->nshown) P->pal_sel = (P->pal_sel + 1) % P->nshown;
        if (input_key(in, SDLK_UP, 0) && P->nshown) P->pal_sel = (P->pal_sel + P->nshown - 1) % P->nshown;
        if ((input_key(in, SDLK_RETURN, 0) || input_key(in, SDLK_KP_ENTER, 0)) && P->nshown) {
            Cmd c = P->cmds[P->shown[P->pal_sel]];
            P->modal = MODAL_NONE;
            run_cmd(P, a, &c);
        }
    } else if (P->modal == MODAL_NAME) {
        if (input_key(in, SDLK_RETURN, 0) || input_key(in, SDLK_KP_ENTER, 0)) {
            P->modal = MODAL_NONE;
            if (P->name_purpose == NAME_SAVE_AS) named_save_as(P, a, P->field.buf);
            else named_rename(P, a, P->name_index, P->field.buf);
        }
    } else if (P->modal == MODAL_CONFIRM) {
        if (input_key(in, SDLK_RETURN, 0) || input_key(in, SDLK_KP_ENTER, 0)) { P->modal = MODAL_NONE; confirm_yes(P, a); }
    }
    return true;
}

/* Draws the open modal (palette, name prompt or confirmation) over a dimmed window and
 * handles its mouse input: palette rows, click outside, OK / Cancel buttons. */
static void draw_modal(Panels *P, App *a) {
    if (P->modal == MODAL_NONE) return;
    SDL_Renderer *r = a->renderer;
    Input *in = &a->in;
    if (P->modal_mouse_free) { in->mouse_consumed = false; P->modal_mouse_free = false; }
    draw_rect(r, rect_make(0, 0, (float)a->width, (float)a->height), color_alpha(color_hex(0x000000), 90));
    float w = px(a, P->modal == MODAL_PALETTE ? 620 : 460);
    float x = (a->width - w) / 2, y = px(a, P->modal == MODAL_PALETTE ? 70 : 160);
    if (P->modal == MODAL_PALETTE) {
        /* palette: field on top, up to 12 rows, scrolled to keep the selection visible */
        int rows = P->nshown < 12 ? P->nshown : 12;
        float rh = px(a, 28), h = px(a, 56) + rows * rh + px(a, 10);
        Rect box = rect_make(x, y, w, h);
        draw_rrect(r, box, px(a, 8), color_hex(T_BORDER));
        draw_rrect(r, rect_inset(box, px(a, 1), px(a, 1)), px(a, 7), color_hex(T_PANEL_ALT));
        Rect fr = rect_make(x + px(a, 12), y + px(a, 12), w - px(a, 24), px(a, 32));
        char before[256]; snprintf(before, sizeof before, "%s", P->field.buf);
        ui_textfield(a, &P->field, fr, "Type a command: \"move output\", \"focus\", \"layout\"\xE2\x80\xA6", true);
        if (strcmp(before, P->field.buf)) { P->pal_sel = 0; P->pal_scroll = 0; palette_filter(P); }
        if (P->pal_sel < P->pal_scroll) P->pal_scroll = P->pal_sel;
        if (P->pal_sel >= P->pal_scroll + 12) P->pal_scroll = P->pal_sel - 11;
        for (int k = 0; k < rows; k++) {
            int i = P->pal_scroll + k; if (i >= P->nshown) break;
            const Cmd *c = &P->cmds[P->shown[i]];
            Rect rr = rect_make(x + px(a, 8), y + px(a, 52) + k * rh, w - px(a, 16), rh);
            bool hov = rect_contains(rr, in->mouse_x, in->mouse_y);
            if (i == P->pal_sel || hov) draw_rrect(r, rr, px(a, 4), color_hex(i == P->pal_sel ? T_SELECTED : T_HOVER));
            /* unavailable commands stay listed, dimmed, with the reason (TBR-005 / 044) */
            char why[160] = ""; bool en = true;
            if (c->cmd_id[0]) en = commands_enabled(c->cmd_id, why, sizeof why);
            draw_text_fit(a->font_ui, r, c->label, -1, rect_make(rr.x + px(a, 10), rr.y, rr.w - px(a, en ? 120 : 300), rr.h), color_hex(en ? T_TEXT : T_TEXT_DIM), ALIGN_LEFT);
            if (!en) draw_text_fit(a->font_small, r, why, -1, rect_make(rr.x + rr.w - px(a, 290), rr.y, px(a, 280), rr.h), color_hex(T_TEXT_DIM), ALIGN_RIGHT);
            else if (c->keys[0]) draw_text_fit(a->font_small, r, c->keys, -1, rect_make(rr.x + rr.w - px(a, 110), rr.y, px(a, 100), rr.h), color_hex(T_TEXT_DIM), ALIGN_RIGHT);
            if (hov && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; Cmd cc = *c; P->modal = MODAL_NONE; run_cmd(P, a, &cc); return; }
        }
        if (!P->nshown) draw_text_fit(a->font_small, r, "No command matches", -1, rect_make(x + px(a, 20), y + px(a, 52), w, px(a, 28)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        if (in->mouse_pressed && !in->mouse_consumed && !rect_contains(box, in->mouse_x, in->mouse_y)) { in->mouse_consumed = true; P->modal = MODAL_NONE; }
        return;
    }
    /* name prompt / confirmation: title, field or message, Cancel + OK */
    float h = px(a, P->modal == MODAL_NAME ? 150 : 140);
    Rect box = rect_make(x, y, w, h);
    draw_rrect(r, box, px(a, 8), color_hex(T_BORDER));
    draw_rrect(r, rect_inset(box, px(a, 1), px(a, 1)), px(a, 7), color_hex(T_PANEL_ALT));
    const char *title = P->modal == MODAL_NAME ? (P->name_purpose == NAME_SAVE_AS ? "Save layout as" : "Rename layout") : "Please confirm";
    draw_text_fit(a->font_ui, r, title, -1, rect_make(x + px(a, 20), y + px(a, 12), w - px(a, 40), px(a, 26)), color_hex(T_TEXT), ALIGN_LEFT);
    Rect ok = rect_make(x + w - px(a, 20) - px(a, 110), y + h - px(a, 46), px(a, 110), px(a, 30));
    Rect cancel = rect_make(ok.x - px(a, 10) - px(a, 90), ok.y, px(a, 90), ok.h);
    if (P->modal == MODAL_NAME) {
        ui_textfield(a, &P->field, rect_make(x + px(a, 20), y + px(a, 48), w - px(a, 40), px(a, 32)), "Name (letters, digits, spaces, - _)", true);
        if (ui_button(a, ok, P->name_purpose == NAME_SAVE_AS ? "Save" : "Rename", BTN_PRIMARY, P->field.len > 0)) {
            P->modal = MODAL_NONE;
            if (P->name_purpose == NAME_SAVE_AS) named_save_as(P, a, P->field.buf); else named_rename(P, a, P->name_index, P->field.buf);
        }
    } else {
        results_draw_wrapped(a->font_ui, r, P->confirm_text, rect_make(x + px(a, 20), y + px(a, 44), w - px(a, 40), px(a, 50)), color_hex(T_TEXT_MUTED), 0, true);
        const char *yes = P->confirm_kind == CONFIRM_CLOSE_TAB ? "Close" : P->confirm_kind == CONFIRM_IMPORT ? "Replace" : P->confirm_kind == CONFIRM_DELETE_LAYOUT ? "Delete" : "Reset";
        if (ui_button(a, ok, yes, P->confirm_kind == CONFIRM_IMPORT || P->confirm_kind == CONFIRM_RESET ? BTN_PRIMARY : BTN_DANGER, true)) { P->modal = MODAL_NONE; confirm_yes(P, a); }
    }
    if (ui_button(a, cancel, "Cancel", BTN_NORMAL, true)) P->modal = MODAL_NONE;
}

/* ======================================================================== */
/* 4. panel bar (PNL-003 / 006 / 037)                                        */
/* ======================================================================== */

/* Panels pinned on the bar, in bar_order (out must hold DOCK_MAX_PANELS); *n = count. */
static void bar_order(Panels *P, int *out, int *n) {
    *n = 0;
    for (int o = 0; o <= DOCK_MAX_PANELS * 4 && *n < P->dock.ndesc; o++)
        for (int p = 0; p < P->dock.ndesc; p++) if (P->dock.st.panel[p].in_bar && P->dock.st.panel[p].bar_order == o) out[(*n)++] = p;
    /* panels with duplicate / out-of-range orders still appear */
    for (int p = 0; p < P->dock.ndesc; p++) {
        if (!P->dock.st.panel[p].in_bar) continue;
        bool seen = false; for (int k = 0; k < *n; k++) if (out[k] == p) seen = true;
        if (!seen) out[(*n)++] = p;
    }
}

/* Draws the panel bar on the left edge: one button per pinned panel (selected when
 * visible, with its most important badge and a tooltip) and "More panels" at the bottom.
 * A press starts DRAG_BAR_PENDING (click = toggle, drag = reorder / dock, see drag_update);
 * right-click opens the panel's context menu. */
static void draw_bar(Panels *P, App *a) {
    SDL_Renderer *r = a->renderer;
    Input *in = &a->in;
    Rect rail = R(P, P->lay.rail);
    if (rail.w <= 0) return;
    draw_rect(r, rail, color_hex(T_PANEL));
    draw_rect(r, rect_make(rail.x + rail.w - px(a, 1), rail.y, px(a, 1), rail.h), color_hex(T_BORDER));
    int order[DOCK_MAX_PANELS], n; bar_order(P, order, &n);
    float bs = px(a, 32), y = rail.y + px(a, 6);
    for (int k = 0; k < n; k++) {
        int p = order[k];
        Rect b = rect_make(rail.x + (rail.w - bs) / 2, y, bs, bs);
        bool vis = dock_is_visible(&P->dock, p), col = dock_is_collapsed(&P->dock, p), hov = rect_contains(b, in->mouse_x, in->mouse_y);
        if (vis) draw_rrect(r, b, px(a, 6), color_hex(T_SELECTED));
        else if (hov) draw_rrect(r, b, px(a, 6), color_hex(T_HOVER));
        if (vis) draw_rect(r, rect_make(rail.x, b.y + px(a, 6), px(a, 2), b.h - px(a, 12)), color_hex(T_ACCENT));
        Color ic = color_hex(vis ? T_TEXT : col ? T_TEXT_MUTED : T_TEXT_DIM);
        draw_panel_icon(a, P->dock.desc[p].icon ? P->dock.desc[p].icon : "?", b.x + bs / 2, b.y + bs / 2, px(a, 15), ic, color_hex(vis ? T_SELECTED : T_PANEL));
        /* one badge, the most important channel (the tooltip lists all of them) */
        DockBadge bd = badge_of(P, a, p);
        float bx = b.x + bs - px(a, 7), by = b.y + px(a, 7);
        if (bd.error) draw_rrect(r, rect_make(bx - px(a, 4), by - px(a, 4), px(a, 8), px(a, 8)), px(a, 4), color_hex(T_RED));
        else if (bd.loading) draw_rect_outline(r, rect_make(bx - px(a, 4), by - px(a, 4), px(a, 8), px(a, 8)), px(a, 1.5f), color_hex(T_ACCENT));
        else if (bd.warning) draw_triangle(r, bx, by, px(a, 8), px(a, 7), false, color_hex(T_YELLOW));
        else if (bd.unsaved) draw_rrect(r, rect_make(bx - px(a, 3), by - px(a, 3), px(a, 6), px(a, 6)), px(a, 3), color_hex(T_YELLOW));
        { char tg[64]; snprintf(tg, sizeof tg, "bar:%s", P->dock.desc[p].title); ui_record(tg, b); }
        char tip[220]; describe_panel(P, a, p, tip, sizeof tip);
        size_t tl = strlen(tip); snprintf(tip + tl, sizeof tip - tl, "  \xC2\xB7  click: %s", vis ? "hide" : col ? "expand" : "show");
        tooltip(P, a, 1000 + p, b, tip);
        if (hov) a->want_cursor = CURSOR_HAND;
        if (hov && in->mouse_pressed && !in->mouse_consumed) {
            in->mouse_consumed = true;
            P->drag = DRAG_BAR_PENDING; P->dpanel = p; P->dx0 = in->mouse_x; P->dy0 = in->mouse_y;
        }
        if (hov && in->mouse_rpressed && !in->mouse_consumed) { in->mouse_consumed = true; open_panel_menu(P, a, p, b.x + b.w, b.y); }
        y += bs + px(a, 4);
    }
    /* More panels: registered panels without a bar button (PNL-037) */
    Rect mb = rect_make(rail.x + (rail.w - bs) / 2, rail.y + rail.h - bs - px(a, 6), bs, bs);
    bool mh = rect_contains(mb, in->mouse_x, in->mouse_y);
    if (mh) draw_rrect(r, mb, px(a, 6), color_hex(T_HOVER));
    for (int i = 0; i < 3; i++) draw_rrect(r, rect_make(mb.x + bs / 2 - px(a, 7) + i * px(a, 6), mb.y + bs / 2 - px(a, 1.5f), px(a, 3), px(a, 3)), px(a, 1.5f), color_hex(T_TEXT_MUTED));
    ui_record("bar:More", mb);
    tooltip(P, a, 999, mb, "More panels: every registered panel, including those not on the bar");
    if (mh && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; open_panels_list(P, a, mb.x + mb.w, mb.y - px(a, 120), false); }
}

/* ======================================================================== */
/* 5. collapsed strips (PNL-004 / 025 / 029)                                 */
/* ======================================================================== */

/* Draws the collapsed strip of each region: one entry per panel of every collapsed (or
 * auto-collapsed) group, with title and badge on bottom / top, icon only on the sides. A
 * click expands the group, or shows the panel as an overlay when the window is too small. */
static void draw_strips(Panels *P, App *a) {
    SDL_Renderer *r = a->renderer;
    Input *in = &a->in;
    Dock *d = &P->dock;
    for (int reg = 0; reg < DOCK_REGIONS; reg++) {
        Rect st = R(P, P->lay.strip[reg]);
        if (st.w <= 0 || st.h <= 0) continue;
        draw_rect(r, st, color_hex(T_PANEL_ALT));
        bool horiz = reg == DOCK_BOTTOM || reg == DOCK_TOP;
        draw_rect(r, horiz ? rect_make(st.x, reg == DOCK_BOTTOM ? st.y : st.y + st.h - px(a, 1), st.w, px(a, 1)) : rect_make(reg == DOCK_LEFT ? st.x + st.w - px(a, 1) : st.x, st.y, px(a, 1), st.h), color_hex(T_BORDER));
        float pos = horiz ? st.x + px(a, 8) : st.y + px(a, 8);
        for (int k = 0; k < d->st.ngroups_in[reg]; k++) {
            int g = d->st.group_order[reg][k];
            if (!d->st.group[g].used || !(d->st.group[g].collapsed || d->auto_collapsed[reg])) continue;
            for (int i = 0; i < d->st.group[g].ntabs; i++) {
                int p = d->st.group[g].tabs[i];
                DockBadge bd = badge_of(P, a, p);
                Rect it;
                if (horiz) {
                    char lbl[96];
                    snprintf(lbl, sizeof lbl, "%s", d->desc[p].title);
                    float tw = font_width(a->font_small, lbl, -1);
                    float bw = (bd.count || bd.unsaved || bd.error || bd.warning || bd.loading) ? px(a, 46) : 0;
                    it = rect_make(pos, st.y + px(a, 2), tw + bw + px(a, 34), st.h - px(a, 4));
                    bool hov = rect_contains(it, in->mouse_x, in->mouse_y);
                    if (hov) draw_rrect(r, it, px(a, 4), color_hex(T_HOVER));
                    draw_chevron(r, it.x + px(a, 10), it.y + it.h / 2, px(a, 7), px(a, 1.5f), false, color_hex(T_TEXT_MUTED));
                    draw_text_fit(a->font_small, r, lbl, -1, rect_make(it.x + px(a, 20), it.y, tw + px(a, 2), it.h), color_hex(T_TEXT), ALIGN_LEFT);
                    draw_badge(a, &bd, it.x + px(a, 26) + tw, it.y + it.h / 2);
                    pos += it.w + px(a, 6);
                } else {
                    it = rect_make(st.x + px(a, 1), pos, st.w - px(a, 2), px(a, 28));
                    bool hov = rect_contains(it, in->mouse_x, in->mouse_y);
                    if (hov) draw_rrect(r, it, px(a, 4), color_hex(T_HOVER));
                    draw_panel_icon(a, d->desc[p].icon ? d->desc[p].icon : "?", it.x + it.w / 2, it.y + it.h / 2, px(a, 13), color_hex(T_TEXT_MUTED), color_hex(T_PANEL_ALT));
                    if (bd.unsaved || bd.error || bd.warning) draw_rrect(r, rect_make(it.x + it.w - px(a, 7), it.y + px(a, 3), px(a, 5), px(a, 5)), px(a, 2.5f), color_hex(bd.error ? T_RED : T_YELLOW));
                    pos += it.h + px(a, 4);
                }
                { char tg[64]; snprintf(tg, sizeof tg, "strip:%s", d->desc[p].title); ui_record(tg, it); }
                char tip[220]; describe_panel(P, a, p, tip, sizeof tip);
                size_t tl = strlen(tip); snprintf(tip + tl, sizeof tip - tl, "%s", d->auto_collapsed[reg] ? "  \xC2\xB7  folded: the window is too small" : "  \xC2\xB7  click to expand");
                tooltip(P, a, 1100 + p, it, tip);
                if (rect_contains(it, in->mouse_x, in->mouse_y)) a->want_cursor = CURSOR_HAND;
                if (rect_contains(it, in->mouse_x, in->mouse_y) && in->mouse_pressed && !in->mouse_consumed) {
                    in->mouse_consumed = true;
                    if (d->auto_collapsed[reg]) dock_open_overlay(d, p);    /* no room: show it over the editor */
                    else panel_op(P, a, p, OP_SHOW);
                }
                if (rect_contains(it, in->mouse_x, in->mouse_y) && in->mouse_rpressed && !in->mouse_consumed) { in->mouse_consumed = true; open_panel_menu(P, a, p, it.x, it.y + it.h); }
            }
        }
    }
}

/* ======================================================================== */
/* 6. group headers and panel tabs (PNL-005 / 007 / 010)                     */
/* ======================================================================== */

/* Draws the header of group g in `hd` (pixels): tabs, then the actions / collapse / maximize
 * buttons, and handles their clicks. Records each tab's centre x in P->tab_cx so a drop can
 * find its index. overlay_hdr: header of the auto-hide overlay, where collapse means "put
 * away" and maximize means "dock and pin back", and tabs cannot be dragged. */
static void draw_group_header(Panels *P, App *a, int g, Rect hd, bool overlay_hdr) {
    SDL_Renderer *r = a->renderer;
    Input *in = &a->in;
    Dock *d = &P->dock;
    const DockGroup *gr = &d->st.group[g];
    draw_rect(r, hd, color_hex(T_PANEL_ALT));
    draw_rect(r, rect_make(hd.x, hd.y + hd.h - px(a, 1), hd.w, px(a, 1)), color_hex(T_BORDER));
    /* The layout of this frame was computed before input ran: a hide / move earlier in the frame
     * can leave the group empty (active = -1) while it is still drawn once. Nothing to show then. */
    if (gr->ntabs <= 0 || gr->active < 0 || gr->active >= d->ndesc) return;
    /* buttons on the right: panel actions, collapse, maximize (never a close-like cross: R-05) */
    float bw = px(a, 26), bx = hd.x + hd.w - px(a, 6) - bw;
    Rect bmax = rect_make(bx, hd.y + (hd.h - bw) / 2, bw, bw); bx -= bw + px(a, 2);
    Rect bcol = rect_make(bx, bmax.y, bw, bw); bx -= bw + px(a, 2);
    Rect bmenu = rect_make(bx, bmax.y, bw, bw);
    float tabs_end = bmenu.x - px(a, 6);
    /* tabs, left to right, until they run out of room */
    float x = hd.x + px(a, 6);
    for (int i = 0; i < gr->ntabs; i++) {
        int p = gr->tabs[i];
        if (p < 0 || p >= d->ndesc) continue;
        const DockPanelDesc *ds = &d->desc[p];
        DockBadge bd = badge_of(P, a, p);
        float tw = font_width(a->font_ui, ds->title, -1);
        float bwid = (bd.count || bd.unsaved || bd.error || bd.warning || bd.loading) ? px(a, 40) : 0;
        Rect t = rect_make(x, hd.y + px(a, 3), tw + bwid + px(a, 36), hd.h - px(a, 4));
        if (t.x + t.w > tabs_end) t.w = tabs_end - t.x;
        if (t.w < px(a, 40)) break;
        bool active = gr->active == p, hov = rect_contains(t, in->mouse_x, in->mouse_y);
        if (g < DOCK_MAX_GROUPS && i < DOCK_MAX_TABS) P->tab_cx[g][i] = t.x + t.w / 2;
        if (active) { draw_rrect(r, t, px(a, 5), color_hex(T_PANEL)); draw_rect(r, rect_make(t.x, t.y + t.h - px(a, 2), t.w, px(a, 2)), color_hex(T_ACCENT)); }
        else if (hov) draw_rrect(r, t, px(a, 5), color_hex(T_HOVER));
        draw_panel_icon(a, ds->icon ? ds->icon : "?", t.x + px(a, 13), t.y + t.h / 2, px(a, 12), color_hex(active ? T_TEXT : T_TEXT_MUTED), color_hex(active ? T_PANEL : T_PANEL_ALT));
        draw_text_fit(a->font_ui, r, ds->title, -1, rect_make(t.x + px(a, 26), t.y, t.w - px(a, 30) - bwid, t.h), color_hex(active ? T_TEXT : T_TEXT_MUTED), ALIGN_LEFT);
        if (bwid > 0) draw_badge(a, &bd, t.x + px(a, 30) + tw, t.y + t.h / 2);
        { char tg[64]; snprintf(tg, sizeof tg, "tab:%s", ds->title); ui_record(tg, t); }
        char tip[220]; describe_panel(P, a, p, tip, sizeof tip);
        tooltip(P, a, 1200 + p, t, tip);
        if (hov && in->mouse_pressed && !in->mouse_consumed) {
            in->mouse_consumed = true;
            if (in->clicks >= 2 && !overlay_hdr) panel_op(P, a, p, OP_MAXIMIZE);
            else {
                dock_activate(d, p);            /* only this group changes (PNL-005); no focus change */
                if (!overlay_hdr) { P->drag = DRAG_PANEL_PENDING; P->dpanel = p; P->dx0 = in->mouse_x; P->dy0 = in->mouse_y; }
            }
        }
        if (hov && in->mouse_rpressed && !in->mouse_consumed) { in->mouse_consumed = true; open_panel_menu(P, a, p, in->mouse_x, in->mouse_y); }
        x += t.w + px(a, 2);
    }
    int act = gr->active;
    if (gr->ntabs <= 0 || act < 0 || act >= d->ndesc) return;   /* a tab click above may have emptied the group */
    { char tg[64];
      snprintf(tg, sizeof tg, "actions:%s", d->desc[act].title); ui_record(tg, bmenu);
      snprintf(tg, sizeof tg, "collapse:%s", d->desc[act].title); ui_record(tg, bcol);
      snprintf(tg, sizeof tg, "maximize:%s", d->desc[act].title); ui_record(tg, bmax); }
    /* ⋮ */
    bool hm = rect_contains(bmenu, in->mouse_x, in->mouse_y);
    if (hm) draw_rrect(r, bmenu, px(a, 4), color_hex(T_HOVER));
    for (int i = 0; i < 3; i++) draw_rrect(r, rect_make(bmenu.x + bw / 2 - px(a, 1.5f), bmenu.y + bw / 2 - px(a, 6) + i * px(a, 5), px(a, 3), px(a, 3)), px(a, 1.5f), color_hex(T_TEXT_MUTED));
    tooltip(P, a, 1300 + g, bmenu, "Panel actions: hide, move, split, mode, maximize, reset");
    if (hm && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; open_panel_menu(P, a, act, bmenu.x, bmenu.y + bmenu.h); }
    /* collapse: a minimize bar */
    bool hc = rect_contains(bcol, in->mouse_x, in->mouse_y);
    if (hc) draw_rrect(r, bcol, px(a, 4), color_hex(T_HOVER));
    draw_rect(r, rect_make(bcol.x + px(a, 8), bcol.y + bw / 2 + px(a, 3), bw - px(a, 16), px(a, 1.5f)), color_hex(T_TEXT_MUTED));
    tooltip(P, a, 1400 + g, bcol, overlay_hdr ? "Put away (the panel stays in auto-hide)" : "Collapse to the edge: content, tabs and size are kept");
    if (hc && in->mouse_pressed && !in->mouse_consumed) {
        in->mouse_consumed = true;
        if (overlay_hdr) dock_close_overlay(d); else panel_op(P, a, act, OP_COLLAPSE);
    }
    /* maximize / restore */
    bool hx = rect_contains(bmax, in->mouse_x, in->mouse_y);
    if (hx) draw_rrect(r, bmax, px(a, 4), color_hex(T_HOVER));
    bool maxed = d->maximized_group == g;
    if (overlay_hdr) {   /* in the overlay this button docks the panel back (pin) */
        draw_rect_outline(r, rect_make(bmax.x + px(a, 8), bmax.y + px(a, 8), bw - px(a, 16), bw - px(a, 16)), px(a, 1.5f), color_hex(T_TEXT_MUTED));
        draw_rect(r, rect_make(bmax.x + px(a, 8), bmax.y + px(a, 8), bw - px(a, 16), px(a, 3)), color_hex(T_TEXT_MUTED));
        tooltip(P, a, 1500 + g, bmax, "Dock and pin it back in its region");
        if (hx && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; int op = d->overlay_panel; dock_close_overlay(d); if (op >= 0) panel_op(P, a, op, OP_MODE_DOCKED); }
    } else {
        draw_rect_outline(r, rect_make(bmax.x + px(a, 7), bmax.y + px(a, 7), bw - px(a, 14), bw - px(a, 14)), px(a, 1.5f), color_hex(T_TEXT_MUTED));
        if (maxed) draw_rect_outline(r, rect_make(bmax.x + px(a, 10), bmax.y + px(a, 4), bw - px(a, 14), bw - px(a, 14)), px(a, 1.2f), color_hex(T_TEXT_MUTED));
        tooltip(P, a, 1500 + g, bmax, maxed ? "Restore the layout (Cmd+Alt+M)" : "Maximize this panel (Cmd+Alt+M); the saved layout is not changed");
        if (hx && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; panel_op(P, a, act, OP_MAXIMIZE); }
    }
}

/* ======================================================================== */
/* editor groups (PNL-027 / 028)                                             */
/* ======================================================================== */

/* Draws editor group eg in rc (pixels): the tab strip (title, connection name, close
 * button), the run hint and split button, then the active tab's editor. A press on a tab
 * activates it and may start an editor-tab drag. */
static void draw_editor_group(Panels *P, App *a, int eg, Rect rc) {
    SDL_Renderer *r = a->renderer;
    Input *in = &a->in;
    DockEditorGroup *e = &P->dock.st.editor[eg];
    float hh = px(a, M_HEADER);
    Rect strip = rect_make(rc.x, rc.y, rc.w, hh);
    draw_rect(r, strip, color_hex(T_BG));
    draw_rect(r, rect_make(strip.x, strip.y + strip.h - px(a, 1), strip.w, px(a, 1)), color_hex(T_BORDER));
    bool group_active = P->dock.st.active_editor_group == eg;
    float x = strip.x;
    /* right side: run hint (active group) and the split button */
    Rect split = rect_make(strip.x + strip.w - px(a, 32), strip.y + px(a, 4), px(a, 26), hh - px(a, 8));
    float right_edge = split.x - px(a, 6);
    if (group_active && a->editor && !editor_object_mode(a->editor) && !panels_active_report(P, a) && !panels_active_designer(P, a)) {   /* an object, report or designer tab never runs its text */
        int f0, l0; bool has_stmt = editor_statement_lines(a->editor, &f0, &l0);
#ifdef __APPLE__
        const char *keys = "\xE2\x8C\x98\xE2\x86\xA9";
#else
        const char *keys = "Ctrl+Enter";
#endif
        const char *label = has_stmt ? "Run statement" : "No statement under cursor";
        float lw = font_width(a->font_small, label, -1), kw = font_width(a->font_small, keys, -1) + px(a, 12);
        float lx = right_edge - lw;
        if (lx - kw - px(a, 8) > strip.x + px(a, 200)) {
            draw_text_fit(a->font_small, r, label, -1, rect_make(lx, strip.y, lw + px(a, 2), hh), color_hex(has_stmt ? T_TEXT_MUTED : T_TEXT_DIM), ALIGN_LEFT);
            Rect chip = rect_make(lx - px(a, 8) - kw, strip.y + px(a, 7), kw, hh - px(a, 14));
            draw_rrect(r, chip, px(a, 4), color_hex(T_BORDER));
            draw_rrect(r, rect_inset(chip, px(a, 1), px(a, 1)), px(a, 3), color_hex(T_PANEL_ALT));
            draw_text_fit(a->font_small, r, keys, -1, chip, color_hex(has_stmt ? T_TEXT : T_TEXT_DIM), ALIGN_CENTER);
            right_edge = chip.x - px(a, 10);
        }
    }
    bool hs = rect_contains(split, in->mouse_x, in->mouse_y);
    if (hs) draw_rrect(r, split, px(a, 4), color_hex(T_HOVER));
    draw_rect_outline(r, rect_make(split.x + px(a, 6), split.y + px(a, 5), split.w - px(a, 12), split.h - px(a, 10)), px(a, 1.2f), color_hex(T_TEXT_MUTED));
    draw_rect(r, rect_make(split.x + split.w / 2 - px(a, 0.6f), split.y + px(a, 5), px(a, 1.2f), split.h - px(a, 10)), color_hex(T_TEXT_MUTED));
    ui_record("split-editor", split);
    tooltip(P, a, 1600 + eg, split, "Split the editor right (Cmd+\\); Shift+Cmd+\\ splits down");
    if (hs && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; P->dock.st.active_editor_group = eg; panels_action(P, a, ACT_SPLIT_RIGHT); }
    /* tabs */
    for (int i = 0; i < e->ntabs; i++) {
        EdTab *t = tab_by_id(P, e->tabs[i]);
        if (!t) continue;
        Connection *c = t->conn_id[0] ? conn_by_id(a, t->conn_id) : NULL;
        char cn[80]; snprintf(cn, sizeof cn, "%s", c ? c->name : t->rv ? "report" : t->gd ? "diff" : t->dg ? "diagram" : t->path[0] ? "file" : "no connection");   /* a report reads each dataset's own connection */
        bool tdirty = (t->obj && objed_doc_dirty(t->obj)) || (t->td && tdes_doc_dirty(t->td)) || (t->rv && rptview_doc_dirty(t->rv)) || (t->dg && dgmview_doc_dirty(t->dg)) || (t->path[0] && editor_revision(t->ed) != t->saved_rev);   /* unpublished object / table edit: a dot after the title */
        char ttl[64]; snprintf(ttl, sizeof ttl, "%s%s", t->title, tdirty ? "   " : "");
        float tw0 = font_width(a->font_ui, t->title, -1), tw = tw0 + (tdirty ? px(a, 16) : 0), cw = font_width(a->font_small, cn, -1);   /* room for the dot, apart from the title */
        Rect tab = rect_make(x, strip.y, tw + cw + px(a, 66), hh);
        if (tab.x + tab.w > right_edge) tab.w = right_edge - tab.x;
        if (tab.w < px(a, 60)) break;
        bool active = e->active == t->id, hov = rect_contains(tab, in->mouse_x, in->mouse_y);
        draw_rect(r, tab, color_hex(active ? T_EDITOR_BG : hov ? T_HOVER : T_BG));
        if (active) draw_rect(r, rect_make(tab.x, tab.y, tab.w, px(a, 2)), color_hex(group_active ? T_ACCENT : T_TEXT_DIM));
        draw_rect(r, rect_make(tab.x + tab.w, tab.y, px(a, 1), tab.h), color_hex(T_BORDER));
        draw_text_fit(a->font_ui, r, ttl, -1, rect_make(tab.x + px(a, 14), tab.y, tw + px(a, 2), tab.h), color_hex(active ? T_TEXT : T_TEXT_MUTED), ALIGN_LEFT);
        if (tdirty) { float dx = tab.x + px(a, 14) + tw0 + px(a, 7); draw_rrect(r, rect_make(dx, tab.y + tab.h / 2 - px(a, 3.5f), px(a, 7), px(a, 7)), px(a, 3.5f), color_hex(T_ACCENT)); }
        draw_text_fit(a->font_small, r, cn, -1, rect_make(tab.x + px(a, 20) + tw, tab.y, tab.w - tw - px(a, 46), tab.h), color_hex((c && c->db) || t->rv || t->gd ? T_TEXT_DIM : T_YELLOW), ALIGN_LEFT);
        Rect cx = rect_make(tab.x + tab.w - px(a, 24), tab.y + (tab.h - px(a, 18)) / 2, px(a, 18), px(a, 18));
        bool hx = rect_contains(cx, in->mouse_x, in->mouse_y);
        if (hx) draw_rrect(r, cx, px(a, 9), color_hex(T_HOVER));
        draw_cross(r, cx.x + cx.w / 2, cx.y + cx.h / 2, px(a, 7), px(a, 1.4f), color_hex(hx ? T_TEXT : active || hov ? T_TEXT_MUTED : T_BORDER));
        { char tg[80]; snprintf(tg, sizeof tg, "edtab:%s", t->title); ui_record(tg, tab); snprintf(tg, sizeof tg, "edclose:%s", t->title); ui_record(tg, cx); }
        char tip[200]; snprintf(tip, sizeof tip, "%s  \xC2\xB7  runs on %s%s", t->title, cn, c && !c->db ? " (not connected)" : "");
        tooltip(P, a, 1700 + t->id, tab, tip);
        if (hov && in->mouse_pressed && !in->mouse_consumed) {
            in->mouse_consumed = true;
            if (hx) request_close_tab(P, a, t->id);
            else { activate_tab(P, a, t->id, true); P->drag = DRAG_EDTAB_PENDING; P->dtab = t->id; P->dx0 = in->mouse_x; P->dy0 = in->mouse_y; }
        }
        x += tab.w + px(a, 1);
    }
    /* body: a click activates this group's tab before the editor sees it */
    Rect body = rect_make(rc.x, rc.y + hh, rc.w, rc.h - hh);
    EdTab *at = tab_by_id(P, e->active);
    if (!at) { draw_rect(r, body, color_hex(T_EDITOR_BG)); return; }
    if (rect_contains(body, in->mouse_x, in->mouse_y) && in->mouse_pressed && !in->mouse_consumed && a->editor != at->ed) activate_tab(P, a, at->id, true);
    if (at->obj) objed_tab_body(a, at->obj, at->ed, body);   /* header, toolbar, editor, Problems */
    else if (at->td) tdes_tab_body(a, at->td, at->ed, body);   /* structure, properties, SQL preview */
    else if (at->rv) rptview_tab_body(a, at->rv, body);      /* report canvas */
    else if (at->gd) gitview_diff_body(a, at->gd, body);     /* Git diff */
    else if (at->dg) dgmview_tab_body(a, at->dg, body);      /* ER diagram */
    else {
        if (at->path[0]) {
            check_disk(P, a, at);
            if (a->editor == at->ed && input_key(&a->in, SDLK_S, SDL_KMOD_GUI)) save_tab(P, a, at);
            if (at->disk_changed) {   /* the disk changed under unsaved edits: the user decides (GIT-017) */
                float bh = px(a, 30); Rect bn = rect_make(body.x, body.y, body.w, bh);
                draw_rect(a->renderer, bn, color_hex(T_WARN_BG));
                char m[1200]; snprintf(m, sizeof m, "%s changed on disk while you have unsaved edits.", at->title);
                draw_text_fit(a->font_small, a->renderer, m, -1, rect_make(bn.x + px(a, 10), bn.y, bn.w - px(a, 390), bh), color_hex(T_YELLOW), ALIGN_LEFT);
                float bx = bn.x + bn.w - px(a, 380);
                if (ui_button(a, rect_make(bx, bn.y + px(a, 3), px(a, 120), bh - px(a, 6)), "Compare", BTN_NORMAL, true)) panels_compare_buffer_disk(a, at->id);
                if (ui_button(a, rect_make(bx + px(a, 124), bn.y + px(a, 3), px(a, 120), bh - px(a, 6)), "Keep mine", BTN_NORMAL, true)) { long long mt, sz; if (file_stat(at->path, &mt, &sz)) { at->disk_mtime = mt; at->disk_size = sz; } at->disk_changed = false; }
                if (ui_button(a, rect_make(bx + px(a, 248), bn.y + px(a, 3), px(a, 124), bh - px(a, 6)), "Reload (lose mine)", BTN_DANGER, true)) reload_tab(a, at);
                body.y += bh; body.h -= bh;
            }
        }
        editor_update(at->ed, a, body);
    }
}

/* ======================================================================== */
/* 7. splitters (PNL-013 / 015)                                              */
/* ======================================================================== */

/* Splitter hover / press / drag. Region edges resize the region, inner splitters move
 * weight between two groups, editor splitters between two editor groups. A double-click
 * resets sizes; a drag pushes one undo snapshot at press. */
static void splitters_input(Panels *P, App *a) {
    Input *in = &a->in;
    Dock *d = &P->dock;
    float grab = px(a, 3);
    for (int i = 0; i < P->lay.nsplit; i++) {
        DockSplitter *sp = &P->lay.split[i];
        Rect rr = R(P, sp->r);
        Rect hit = sp->vertical ? rect_make(rr.x - grab, rr.y, rr.w + 2 * grab, rr.h) : rect_make(rr.x, rr.y - grab, rr.w, rr.h + 2 * grab);
        bool hov = rect_contains(hit, in->mouse_x, in->mouse_y);
        { char tg[40];
          if (sp->kind == 3) snprintf(tg, sizeof tg, "split:%s-lanes:%d", region_word(sp->region), sp->index);   /* one name per lane splitter */
          else snprintf(tg, sizeof tg, "split:%s%s", sp->kind == 2 ? "editor" : region_word(sp->region), sp->kind == 1 ? "-inner" : "");
          ui_record(tg, rr); }
        if (hov && P->drag == DRAG_NONE) a->want_cursor = sp->vertical ? CURSOR_EW : CURSOR_NS;
        if (hov && in->mouse_pressed && !in->mouse_consumed) {
            in->mouse_consumed = true;
            if (in->clicks >= 2) {   /* double-click: preferred size / equal shares (spec 14) */
                if (sp->kind == 0) { dock_reset_region_size(d, sp->region); app_status(a, "Panel size reset to its default"); }
                else if (sp->kind == 1) { dock_begin_gesture(d); for (int k = 0; k < d->st.ngroups_in[sp->region]; k++) d->st.group[d->st.group_order[sp->region][k]].weight = 1; d->dirty = true; d->change_seq++; }
                else if (sp->kind == 3) { dock_begin_gesture(d); for (int l = 0; l < DOCK_MAX_LANES; l++) d->st.lane_weight[sp->region][l] = 1; d->dirty = true; d->change_seq++; }   /* equal lanes */
                else { for (int k = 0; k < d->st.neditors; k++) d->st.editor[k].weight = 1; d->change_seq++; }
                continue;
            }
            P->drag = DRAG_SPLIT; P->dsplit = *sp; P->dx0 = in->mouse_x; P->dy0 = in->mouse_y;
            P->dsize0 = sp->kind == 0 ? d->st.region_size[sp->region] : 0;
            dock_begin_gesture(d);   /* one undo step for the whole drag */
        }
    }
    /* drag in progress: mouse delta converted to points */
    if (P->drag != DRAG_SPLIT) return;
    DockSplitter *sp = &P->dsplit;
    a->want_cursor = sp->vertical ? CURSOR_EW : CURSOR_NS;
    float dx = (in->mouse_x - P->dx0) / P->s, dy = (in->mouse_y - P->dy0) / P->s;
    if (sp->kind == 0) {
        float delta = sp->region == DOCK_LEFT ? dx : sp->region == DOCK_RIGHT ? -dx : sp->region == DOCK_BOTTOM ? -dy : dy;
        dock_set_region_size(d, sp->region, P->dsize0 + delta);
    } else if (sp->kind == 1) {
        if (dock_set_group_pair_weight(d, sp->region, sp->index, sp->index2, sp->vertical ? dx : dy)) { P->dx0 = in->mouse_x; P->dy0 = in->mouse_y; }
    } else if (sp->kind == 3) {   /* PNL-011: between two lanes of a region */
        float delta = sp->vertical ? dx : dy;
        if (dock_set_lane_weight(d, sp->region, sp->index, sp->index2, delta)) { P->dx0 = in->mouse_x; P->dy0 = in->mouse_y; }
    } else {
        int k = sp->index;
        if (k + 1 < d->st.neditors) {
            float len = d->st.editor_split_down ? P->lay.center.h : P->lay.center.w;
            float total = 0; for (int i = 0; i < d->st.neditors; i++) total += d->st.editor[i].weight;
            float dw = (d->st.editor_split_down ? dy : dx) / (len > 1 ? len : 1) * total;
            float wa = d->st.editor[k].weight + dw, wb = d->st.editor[k + 1].weight - dw, minw = total * 160.0f / (len > 1 ? len : 1);
            if (wa >= minw && wb >= minw) { d->st.editor[k].weight = wa; d->st.editor[k + 1].weight = wb; d->change_seq++; P->dx0 = in->mouse_x; P->dy0 = in->mouse_y; }
        }
    }
    if (!in->mouse_down) P->drag = DRAG_NONE;
}

/* Preview caption of a lane split (PNL-011): which way the new group goes. */
static const char *side_split_word(const struct Panels *P) {
    int r = P->drop.group >= 0 ? P->dock.st.group[P->drop.group].region : DOCK_LEFT;
    bool side = r == DOCK_LEFT || r == DOCK_RIGHT;
    return side ? (P->drop.after ? "Split: new group on the right" : "Split: new group on the left") : (P->drop.after ? "Split: new group below" : "Split: new group above");
}

/* Draws every splitter as a 1-pixel line, accent-coloured when hovered or dragged. */
static void draw_splitters(Panels *P, App *a) {
    for (int i = 0; i < P->lay.nsplit; i++) {
        DockSplitter *sp = &P->lay.split[i];
        Rect rr = R(P, sp->r);
        bool hot = P->drag == DRAG_SPLIT && P->dsplit.kind == sp->kind && P->dsplit.region == sp->region && P->dsplit.index == sp->index;
        if (!hot) { float grab = px(a, 3); Rect hit = sp->vertical ? rect_make(rr.x - grab, rr.y, rr.w + 2 * grab, rr.h) : rect_make(rr.x, rr.y - grab, rr.w, rr.h + 2 * grab); hot = P->drag == DRAG_NONE && rect_contains(hit, a->in.mouse_x, a->in.mouse_y); }
        draw_rect(a->renderer, rr, color_hex(T_BG));
        Rect line = sp->vertical ? rect_make(rr.x + rr.w / 2 - px(a, 0.5f), rr.y, px(a, 1), rr.h) : rect_make(rr.x, rr.y + rr.h / 2 - px(a, 0.5f), rr.w, px(a, 1));
        draw_rect(a->renderer, line, color_hex(hot ? T_ACCENT : T_BORDER));
    }
}

/* ======================================================================== */
/* 8. drag and drop (PNL-007 / 009 / 010 / 012 / 028)                        */
/* ======================================================================== */

/* Lower-case region name for messages and harness tags. */
static const char *region_word(int r) { static const char *const w[] = { "left", "right", "bottom", "top" }; return r >= 0 && r < 4 ? w[r] : "?"; }

/* Editor-tab drop target under (mx, my), pixels: another group (join), or the right / lower
 * quarter of a group's body (split). Result in P->ed_target_*; group -1 = none. */
static void editor_drop_target(Panels *P, App *a, float mx, float my) {
    P->ed_target_group = -1; P->ed_target_index = -1; P->ed_target_split = 0;
    for (int g = 0; g < P->lay.neditors; g++) {
        Rect rc = R(P, P->lay.editor[g]);
        if (!rect_contains(rc, mx, my)) continue;
        float hh = px(a, M_HEADER);
        if (my >= rc.y + hh) {
            if (mx > rc.x + rc.w * 0.75f) { P->ed_target_group = g; P->ed_target_split = 1; return; }
            if (my > rc.y + hh + (rc.h - hh) * 0.75f) { P->ed_target_group = g; P->ed_target_split = 2; return; }
        }
        P->ed_target_group = g; P->ed_target_index = -1;
        return;
    }
}

/* Advances the current drag each frame: promotes a pending press to a drag past the
 * threshold, tracks the drop target, and on release applies it (panel move, editor-tab move
 * or split, bar reorder or dock). An unused Escape cancels. A release on a pending bar
 * press is a click: toggle the panel. */
static void drag_update(Panels *P, App *a) {
    Input *in = &a->in;
    Dock *d = &P->dock;
    float dist = fmaxf(fabsf(in->mouse_x - P->dx0), fabsf(in->mouse_y - P->dy0)) / P->s;
    bool esc = false;
    for (int k = 0; k < in->nkeys; k++) if (!in->keys[k].used && in->keys[k].key == SDLK_ESCAPE && (P->drag == DRAG_PANEL || P->drag == DRAG_EDTAB || P->drag == DRAG_BAR)) { in->keys[k].used = true; esc = true; }
    switch (P->drag) {
    case DRAG_PANEL_PENDING:
        if (!in->mouse_down) P->drag = DRAG_NONE;                  /* a click: the tab was activated */
        else if (dist > DRAG_THRESHOLD) P->drag = DRAG_PANEL;
        break;
    case DRAG_EDTAB_PENDING:
        if (!in->mouse_down) P->drag = DRAG_NONE;
        else if (dist > DRAG_THRESHOLD) P->drag = DRAG_EDTAB;
        break;
    case DRAG_BAR_PENDING:
        if (!in->mouse_down) {                                      /* a click on a bar button: show / hide */
            int p = P->dpanel;
            P->drag = DRAG_NONE;
            if (dock_is_visible(d, p) && d->overlay_panel != p) panel_op(P, a, p, OP_HIDE);
            else if (d->overlay_panel == p) dock_close_overlay(d);
            else panel_op(P, a, p, OP_FOCUS);
        } else if (dist > DRAG_THRESHOLD) P->drag = DRAG_BAR;
        break;
    case DRAG_PANEL:
        if (esc) { P->drag = DRAG_NONE; app_status(a, "Move cancelled: the layout is unchanged"); break; }
        P->drop = dock_drop_target(d, &P->lay, &P->m, P->dpanel, in->mouse_x / P->s, in->mouse_y / P->s);
        if (!in->mouse_down) {
            P->drag = DRAG_NONE;
            if (P->drop.kind == DOCK_DROP_NONE) { app_status(a, "No valid place there: the layout is unchanged"); break; }
            const char *t = d->desc[P->dpanel].title;
            /* PNL-010: on a group's tab row the tab lands where it was dropped, not at the end */
            if (P->drop.kind == DOCK_DROP_TAB) {
                int g = P->drop.group, idx = 0;
                for (int i = 0; i < d->st.group[g].ntabs; i++) if (in->mouse_x > P->tab_cx[g][i]) idx = i + 1;
                if (dock_group_of(d, P->dpanel) == g && idx > 0) {   /* removing it first shifts the later ones */
                    int from = -1; for (int i = 0; i < d->st.group[g].ntabs; i++) if (d->st.group[g].tabs[i] == P->dpanel) from = i;
                    if (from >= 0 && from < idx) idx--;
                }
                P->drop.index = idx;
            }
            if (dock_apply_drop(d, P->dpanel, &P->drop)) {
                if (P->drop.kind == DOCK_DROP_REGION) app_status(a, "%s moved to the %s; its content and running work are unchanged", t, region_word(P->drop.region));
                else if (P->drop.kind == DOCK_DROP_TAB) app_status(a, "%s joined the group as a tab", t);
                else app_status(a, "%s now has its own group beside the other one", t);
            } else app_status(a, "%s not moved: %s", t, d->err);
        }
        break;
    case DRAG_EDTAB:
        if (esc) { P->drag = DRAG_NONE; app_status(a, "Tab move cancelled"); break; }
        editor_drop_target(P, a, in->mouse_x, in->mouse_y);
        if (!in->mouse_down) {
            P->drag = DRAG_NONE;
            int g = P->ed_target_group;
            if (g < 0) break;
            bool ok;
            if (P->ed_target_split) {
                int src = dock_editor_group_of(d, P->dtab);
                if (src == g) ok = dock_editor_split(d, P->dtab, P->ed_target_split == 2);
                else { ok = dock_editor_move_tab(d, P->dtab, g, -1) && dock_editor_split(d, P->dtab, P->ed_target_split == 2); }
            } else ok = dock_editor_move_tab(d, P->dtab, g, P->ed_target_index);
            if (!ok) app_status(a, "Tab not moved: %s", d->err);
            else activate_tab(P, a, P->dtab, true);   /* same editor: text, undo and selection kept */
        }
        break;
    case DRAG_BAR: {
        if (esc) { P->drag = DRAG_NONE; break; }
        if (!in->mouse_down) {
            P->drag = DRAG_NONE;
            Rect rail = R(P, P->lay.rail);
            int k = (int)((in->mouse_y - rail.y - px(a, 6)) / px(a, 36));
            if (rect_contains(rect_make(rail.x, rail.y, rail.w + px(a, 40), rail.h), in->mouse_x, in->mouse_y)) dock_move_bar_button(d, P->dpanel, k < 0 ? 0 : k);
            else {   /* dropped on the workspace: dock it there */
                DockDrop t = dock_drop_target(d, &P->lay, &P->m, P->dpanel, in->mouse_x / P->s, in->mouse_y / P->s);
                if (t.kind != DOCK_DROP_NONE && !dock_apply_drop(d, P->dpanel, &t)) app_status(a, "%s", d->err);
            }
        }
        break;
    }
    default: break;
    }
}

/* Drop preview (tinted rectangle + label) and the ghost label following the mouse, for a
 * panel drag (accent) or an editor-tab drag (green). */
static void draw_drag(Panels *P, App *a) {
    SDL_Renderer *r = a->renderer;
    Input *in = &a->in;
    if (P->drag == DRAG_PANEL || (P->drag == DRAG_BAR && in->mouse_x > R(P, P->lay.rail).w + px(a, 20))) {
        if (P->drag == DRAG_BAR) P->drop = dock_drop_target(&P->dock, &P->lay, &P->m, P->dpanel, in->mouse_x / P->s, in->mouse_y / P->s);
        if (P->drop.kind != DOCK_DROP_NONE) {
            Rect pv = R(P, P->drop.preview);
            draw_rect(r, pv, color_alpha(color_hex(T_ACCENT), 46));
            draw_rect_outline(r, pv, px(a, 2), color_hex(T_ACCENT));
            const char *what = P->drop.kind == DOCK_DROP_TAB ? "Join as a tab" : P->drop.kind == DOCK_DROP_SPLIT ? (P->drop.cross ? (side_split_word(P) ) : "New group beside") : "Dock here";
            char lbl[80]; snprintf(lbl, sizeof lbl, "%s%s%s", what, P->drop.kind == DOCK_DROP_REGION ? ": " : "", P->drop.kind == DOCK_DROP_REGION ? region_word(P->drop.region) : "");
            float w = font_width(a->font_small, lbl, -1) + px(a, 20);
            Rect chip = rect_make(pv.x + (pv.w - w) / 2, pv.y + (pv.h - px(a, 24)) / 2, w, px(a, 24));
            draw_rrect(r, chip, px(a, 5), color_hex(T_ACCENT));
            draw_text_fit(a->font_small, r, lbl, -1, chip, color_hex(T_ON_ACCENT), ALIGN_CENTER);
        }
        /* what is being moved, named so a panel drag is never confused with an editor tab (PNL-028) */
        char g[96]; snprintf(g, sizeof g, "Panel: %s  \xC2\xB7  Esc cancels", P->dock.desc[P->dpanel].title);
        float w = font_width(a->font_small, g, -1) + px(a, 20);
        Rect gh = rect_make(in->mouse_x + px(a, 14), in->mouse_y + px(a, 10), w, px(a, 24));
        if (gh.x + gh.w > a->width - px(a, 4)) gh.x = in->mouse_x - px(a, 14) - w;   /* stays on screen near the right edge */
        draw_rrect(r, gh, px(a, 5), color_hex(T_PANEL_ALT)); draw_rect_outline(r, gh, px(a, 1), color_hex(T_BORDER));
        draw_text_fit(a->font_small, r, g, -1, gh, color_hex(T_TEXT), ALIGN_CENTER);
    }
    if (P->drag == DRAG_EDTAB) {
        if (P->ed_target_group >= 0) {
            Rect rc = R(P, P->lay.editor[P->ed_target_group]);
            Rect pv = P->ed_target_split == 1 ? rect_make(rc.x + rc.w / 2, rc.y, rc.w / 2, rc.h) : P->ed_target_split == 2 ? rect_make(rc.x, rc.y + rc.h / 2, rc.w, rc.h / 2) : rc;
            draw_rect(r, pv, color_alpha(color_hex(T_GREEN), 36));
            draw_rect_outline(r, pv, px(a, 2), color_hex(T_GREEN));
        }
        EdTab *t = tab_by_id(P, P->dtab);
        char g[96]; snprintf(g, sizeof g, "Editor tab: %s  \xC2\xB7  Esc cancels", t ? t->title : "?");
        float w = font_width(a->font_small, g, -1) + px(a, 20);
        Rect gh = rect_make(in->mouse_x + px(a, 14), in->mouse_y + px(a, 10), w, px(a, 24));
        if (gh.x + gh.w > a->width - px(a, 4)) gh.x = in->mouse_x - px(a, 14) - w;
        draw_rrect(r, gh, px(a, 5), color_hex(T_PANEL_ALT)); draw_rect_outline(r, gh, px(a, 1), color_hex(T_GREEN));
        draw_text_fit(a->font_small, r, g, -1, gh, color_hex(T_TEXT), ALIGN_CENTER);
    }
}

/* ======================================================================== */
/* 9. overlay and floating panels (PNL-017 / 018 / 019)                      */
/* ======================================================================== */

/* Draws panel p as a framed box with a shadow, its own header and its content (used for the
 * auto-hide / focus-mode overlay). A panel outside any group gets a temporary one-tab group
 * borrowed in the last group slot for the header, restored right after. */
static void draw_panel_frame(Panels *P, App *a, int p, Rect box, bool overlay) {
    SDL_Renderer *r = a->renderer;
    for (int s = 3; s >= 1; s--) draw_rrect(r, rect_make(box.x - s * px(a, 2), box.y + px(a, 2), box.w + s * px(a, 4), box.h + s * px(a, 3)), px(a, 6) + s * px(a, 2), color_alpha(color_hex(0x000000), 30));
    draw_rect(r, box, color_hex(T_PANEL));
    draw_rect_outline(r, box, px(a, 1), color_hex(T_ACCENT_DIM));
    float hh = px(a, P->m.header_h);
    /* a one-tab header for the panel itself */
    int g = dock_group_of(&P->dock, p);
    DockGroup tmp; memset(&tmp, 0, sizeof tmp);
    tmp.used = true; tmp.ntabs = 1; tmp.tabs[0] = p; tmp.active = p;
    DockGroup saved; int gi = g >= 0 ? g : DOCK_MAX_GROUPS - 1;
    saved = P->dock.st.group[gi];
    P->dock.st.group[gi] = g >= 0 ? saved : tmp;
    draw_group_header(P, a, gi, rect_make(box.x, box.y, box.w, hh), overlay);
    P->dock.st.group[gi] = saved;
    Rect content = rect_make(box.x, box.y + hh, box.w, box.h - hh);
    if (rect_contains(content, a->in.mouse_x, a->in.mouse_y) && a->in.mouse_pressed) P->last_panel = p;
    clip_push(r, content);
    panel_draw(P, a, p, content, panel_focused(P, a, p));
    clip_pop(r);
}

/* ======================================================================== */
/* 14. actions and shortcuts                                                 */
/* ======================================================================== */

/* Panel addressed by ACT_PANEL_* (Cmd+1..4), or -1. */
static int panel_for_action(Panels *P, int act) {
    return act == ACT_PANEL_EXPLORER ? P->P_EX : act == ACT_PANEL_RESULTS ? P->P_RS : act == ACT_PANEL_OUTPUT ? P->P_OUT : act == ACT_PANEL_INSPECTOR ? P->P_IN : -1;
}

/* the panel a panel command acts on: the one focused last, else Results */
static int active_panel(Panels *P, App *a) {
    if (a->focus == FOCUS_EXPLORER) return P->P_EX;
    if (P->last_panel >= 0 && P->last_panel < P->dock.ndesc && dock_is_visible(&P->dock, P->last_panel)) return P->last_panel;
    return P->P_RS;
}

/* SDL file dialog callback (may run on another thread): posts the chosen path as a user
 * event of type *ud; the path is SDL_strdup'd and freed by panels_handle_event(). */
static void SDLCALL on_layout_dialog(void *ud, const char *const *files, int filter) {
    (void)filter;
    Uint32 *ev = ud;
    if (!files || !files[0]) return;
    SDL_Event e; SDL_zero(e); e.type = *ev; e.user.data1 = SDL_strdup(files[0]);
    SDL_PushEvent(&e);
}

/* Runs the panel / layout / editor-group actions; false lets main.c handle the rest. */
bool panels_action(Panels *P, App *a, int act) {
    Dock *d = &P->dock;
    int p = panel_for_action(P, act);
    if (p >= 0) {
        /* Cmd+1..4: open and focus; when it already has the focus, hide it (toggle) */
        if (dock_is_visible(d, p) && panel_focused(P, a, p) && d->overlay_panel != p) panel_op(P, a, p, OP_HIDE);
        else { panel_op(P, a, p, OP_FOCUS); if (d->st.focus_mode && d->overlay_panel != p) dock_open_overlay(d, p); }
        return true;
    }
    switch (act) {
    case ACT_PALETTE: open_palette(P, a); return true;
    case ACT_PANELS: open_panels_list(P, a, px(a, 60), px(a, 40), false); return true;
    case ACT_TOGGLE_SIDEBAR: dock_toggle(d, P->P_EX); return true;
    case ACT_TOGGLE_RESULTS: {
        int g = dock_group_of(d, P->P_RS);
        if (g >= 0 && !d->st.group[g].collapsed) dock_collapse_group(d, g); else dock_show(d, P->P_RS);
        return true;
    }
    case ACT_SHOW_OUTPUT: panel_op(P, a, P->P_OUT, OP_FOCUS); output_mark_seen(a->output, a); return true;
    case ACT_FIND:   /* Cmd+F searches the panel being used: the Output when it has the focus, else the result grid */
        if (dock_is_visible(d, P->P_OUT) && a->focus == FOCUS_RESULTS && P->last_panel == P->P_OUT) { output_find(a->output); return true; }
        results_find(a->results); a->focus = FOCUS_RESULTS; dock_show(d, P->P_RS); P->last_panel = P->P_RS;
        return true;
    case ACT_FOCUS_MODE:
        if (dock_toggle_focus_mode(d)) app_status(a, d->st.focus_mode ? "Focus mode: only the editor; panels open over it with their shortcut; the same command restores them" : "Focus mode off: panels restored where they were");
        return true;
    case ACT_MAXIMIZE_PANEL: panel_op(P, a, active_panel(P, a), OP_MAXIMIZE); return true;
    case ACT_COLLAPSE_PANEL: { int ap = active_panel(P, a); panel_op(P, a, ap, dock_is_collapsed(d, ap) ? OP_EXPAND : OP_COLLAPSE); return true; }
    case ACT_HIDE_PANEL: { int ap = active_panel(P, a); if (d->overlay_panel >= 0) dock_close_overlay(d); else if (panel_op(P, a, ap, OP_HIDE)) a->focus = FOCUS_EDITOR; return true; }
    case ACT_UNDO_LAYOUT:
        if (dock_undo(d)) app_status(a, "Layout change undone (editor text and data are not affected)"); else app_status(a, "%s", d->err);
        return true;
    case ACT_RESIZE_GROW: case ACT_RESIZE_SHRINK: {
        int ap = active_panel(P, a), reg = dock_region_of(d, ap);
        float before = d->st.region_size[reg];
        dock_set_region_size(d, reg, before + (act == ACT_RESIZE_GROW ? 20 : -20));
        float now = d->st.region_size[reg];
        if (fabsf(now - before) < 0.5f) app_status(a, "%s: %.0f pt, at its %s", d->desc[ap].title, now, act == ACT_RESIZE_GROW ? "limit (the editor keeps its minimum)" : "minimum");
        else app_status(a, "%s (%s): %.0f pt", d->desc[ap].title, region_word(reg), now);
        return true;
    }
    case ACT_LAST_PANEL: {   /* PNL-022: back to the panel focused last, or to the editor */
        if (a->focus != FOCUS_EDITOR) { a->focus = FOCUS_EDITOR; return true; }
        int lp = P->last_panel >= 0 ? P->last_panel : P->P_RS;
        panel_op(P, a, lp, OP_FOCUS);
        return true;
    }
    case ACT_LAYOUT_DEFAULT: {
        int ni = P->active_name[0] ? named_find(P, P->active_name) : -1;
        if (ni >= 0) {
            char *cur = dock_serialize(d, &d->st, NULL), *sv = dock_serialize(d, &P->named[ni].st, NULL);
            bool changed = strcmp(cur, sv) != 0;
            free(cur); free(sv);
            if (changed) { char msg[200]; snprintf(msg, sizeof msg, "Reset to Default? The changes made to \"%s\" since it was saved are not kept (save them first if you want them).", P->active_name); open_confirm(P, CONFIRM_RESET, 0, msg); return true; }
        }
        P->confirm_kind = CONFIRM_RESET;   /* set before confirm_yes: it dispatches on confirm_kind */
        confirm_yes(P, a);   /* CONFIRM_RESET path without a question */
        return true;
    }
    case ACT_LAYOUT_QUERY_FOCUS: case ACT_LAYOUT_DATA: {
        int pr = act == ACT_LAYOUT_QUERY_FOCUS ? DOCK_PRESET_QUERY_FOCUS : DOCK_PRESET_DATA_EXPLORATION;
        if (dock_apply_preset(d, pr)) { P->active_name[0] = 0; app_status(a, "Layout: %s (panels rearranged; queries, tabs and edits untouched)", dock_preset_name(pr)); }
        return true;
    }
    case ACT_LAYOUT_SAVE_AS:
        P->modal = MODAL_NAME; P->name_purpose = NAME_SAVE_AS; textfield_set(&P->field, P->active_name); P->field.blink = SDL_GetTicks();
        return true;
    case ACT_LAYOUTS: open_layouts_menu(P, a, px(a, 120), px(a, 40)); return true;
    case ACT_LAYOUT_EXPORT: {
        static const SDL_DialogFileFilter f[] = { { "Lume layout", "json" } };
        SDL_ShowSaveFileDialog(on_layout_dialog, &P->ev_export, a->window, f, 1, "lume-layout.json");
        return true;
    }
    case ACT_LAYOUT_IMPORT: {
        static const SDL_DialogFileFilter f[] = { { "Lume layout", "json" } };
        SDL_ShowOpenFileDialog(on_layout_dialog, &P->ev_import, a->window, f, 1, NULL, false);
        return true;
    }
    case ACT_NEW_EDITOR_TAB: case ACT_NEW_QUERY: panels_new_editor_tab(P, a, NULL); return true;
    case ACT_SPLIT_RIGHT: case ACT_SPLIT_DOWN: {
        bool down = act == ACT_SPLIT_DOWN;
        EdTab *t = tab_of_editor(P, a->editor);
        if (!t) return true;
        int g = dock_editor_group_of(d, t->id);
        if (g >= 0 && d->st.editor[g].ntabs < 2) {
            /* one tab: a fresh query tab goes to the new group */
            panels_new_editor_tab(P, a, NULL);
            t = tab_of_editor(P, a->editor);
        }
        if (t && dock_editor_split(d, t->id, down)) { activate_tab(P, a, t->id, true); app_status(a, "Editor split %s: each group has its own tabs; a run uses the active tab's connection", down ? "down" : "right"); }
        else app_status(a, "%s", d->err);
        return true;
    }
    case ACT_REDUCE_MOTION:
        a->reduce_motion = !a->reduce_motion;
        app_status(a, a->reduce_motion ? "Reduce motion: scrolling jumps, nothing animates" : "Reduce motion off: smooth scrolling");
        return true;
    case ACT_COMPACT: d->st.compact = !d->st.compact; d->dirty = true; d->change_seq++; app_status(a, d->st.compact ? "Compact panel headers" : "Regular panel headers"); return true;
    case ACT_TOP_EDGE: {
        if (d->st.top_enabled) {
            int any = d->st.ngroups_in[DOCK_TOP];
            if (any) { app_status(a, "Move the panels off the top edge first"); return true; }
        }
        d->st.top_enabled = !d->st.top_enabled; d->dirty = true; d->change_seq++;
        app_status(a, d->st.top_enabled ? "The top edge accepts panels" : "The top edge no longer accepts panels");
        return true;
    }
    default: return false;
    }
}

/* First keymap binding pressed this frame, as ACT_*; ACT_NONE otherwise. */
int panels_shortcut(Panels *P, App *a) {
    Input *in = &a->in;
    for (int i = 0; i < P->nkeys; i++) {
        /* Shift+Esc (hide panel) must not steal Esc from text fields and menus */
        if (P->keys[i].key == SDLK_ESCAPE && (a->focus == FOCUS_EDITOR || results_modal_open(a->results))) continue;
        if (input_key(in, P->keys[i].key, P->keys[i].mods)) return P->keys[i].action;
    }
    return ACT_NONE;
}

/* Export: the arrangement only (ids, modes, sizes), never SQL, data or credentials (PNL-046). */
bool panels_export_layout(Panels *P, App *a, const char *path) {
    char *txt = dock_serialize(&P->dock, &P->dock.st, P->active_name[0] ? P->active_name : "Exported");
    FILE *f = fopen(path, "w");
    bool ok = f && fputs(txt, f) >= 0;
    if (f) ok = fclose(f) == 0 && ok;
    free(txt);
    app_status(a, ok ? "Layout exported (arrangement only: no SQL, data or credentials)" : "Could not write the layout file");
    return ok;
}

/* Import: validated first; the user sees its name and version and confirms before anything is
 * replaced; panels this installation does not have are skipped (PNL-046 / R-12). */
bool panels_import_layout(Panels *P, App *a, const char *path) {
    char err[200];
    FILE *f = fopen(path, "rb");
    char *buf = NULL; size_t got = 0;
    if (f) { buf = malloc(1 << 20); got = fread(buf, 1, (1 << 20) - 1, f); buf[got] = 0; fclose(f); }
    char name[64] = "";
    bool ok = buf && dock_parse(&P->dock, buf, got, &P->import_st, name, sizeof name, err, sizeof err);
    if (ok) {
        int np = 0; for (int p = 0; p < P->import_st.npanels; p++) np += P->import_st.panel[p].mode == DOCK_DOCKED;
        snprintf(P->import_name, sizeof P->import_name, "%s", name[0] ? name : "imported");
        char msg[300]; snprintf(msg, sizeof msg, "Replace the current layout with \"%s\" (schema %d, %d docked panel%s; panels this Lume does not have are skipped)?", P->import_name, DOCK_SCHEMA_VERSION, np, np == 1 ? "" : "s");
        open_confirm(P, CONFIRM_IMPORT, 0, msg);
    } else app_status(a, "Not imported: %s", buf ? err : "cannot read the file");
    free(buf);
    return ok;
}

static bool floats_event(Panels *P, App *a, const SDL_Event *e);
static void fw_close(Panels *P, int i);

/* Events of the native panel windows and the export / import dialog results; true when
 * consumed. */
bool panels_handle_event(Panels *P, App *a, const SDL_Event *e) {
    if (floats_event(P, a, e)) return true;
    if (e->type != P->ev_export && e->type != P->ev_import) return false;
    char *path = e->user.data1;
    if (e->type == P->ev_export) panels_export_layout(P, a, path); else panels_import_layout(P, a, path);
    SDL_free(path);
    return true;
}

/* ======================================================================== */
/* public helpers                                                            */
/* ======================================================================== */

/* A palette, prompt, confirmation or popup is open. */
bool panels_modal_open(Panels *P) { return P->modal != MODAL_NONE || P->menu.open; }
/* Shows the panel with this id (no-op when unknown). */
void panels_show(Panels *P, App *a, const char *id) { int p = dock_find(&P->dock, id); if (p >= 0) panel_op(P, a, p, OP_SHOW); }
/* True when the panel with this id is on screen in the main window. */
bool panels_is_visible(Panels *P, const char *id) { int p = dock_find(&P->dock, id); return p >= 0 && dock_is_visible(&P->dock, p); }
/* Hidden Results is shown; otherwise only brought to the front of its group. */
/* a panel the user must see now (a run's result, its error): back when hidden, unfolded when its group
 * was collapsed, made the active tab; the keyboard focus stays where it was (Isa, 01/11/2026: a folded
 * Results made a run look like it did nothing) */
static void reveal(Panels *P, int p) {
    if (p < 0) return;
    if (P->dock.st.panel[p].mode == DOCK_HIDDEN) { dock_show(&P->dock, p); return; }
    int g = dock_group_of(&P->dock, p);
    if (g >= 0 && dock_is_collapsed(&P->dock, p)) dock_expand_group(&P->dock, g);
    dock_activate(&P->dock, p);
}
void panels_reveal_results(Panels *P, App *a) { (void)a; reveal(P, P->P_RS); }
void panels_reveal(Panels *P, App *a, const char *panel_id) { (void)a; reveal(P, dock_find(&P->dock, panel_id)); }
/* The engine, borrowed. */
Dock *panels_dock(Panels *P) { return &P->dock; }

/* See panels.h (toolbar Panels menu, TBR-036): the registry and the real state of each panel. */
int panels_panel_count(Panels *P) { return P->dock.ndesc; }
bool panels_panel_info(Panels *P, App *a, int p, PanelInfo *out) {
    if (p < 0 || p >= P->dock.ndesc) return false;
    memset(out, 0, sizeof *out);
    const DockPanelDesc *d = &P->dock.desc[p];
    int mode = P->dock.st.panel[p].mode;
    snprintf(out->id, sizeof out->id, "%s", d->id);
    snprintf(out->title, sizeof out->title, "%s", d->title);
    out->visible = dock_is_visible(&P->dock, p);
    snprintf(out->state, sizeof out->state, "%s", mode == DOCK_HIDDEN ? "hidden" : mode == DOCK_AUTOHIDE ? "auto-hide" : mode == DOCK_FLOATING ? "floating" :
             mode == DOCK_DETACHED ? "separate window" : dock_is_collapsed(&P->dock, p) ? "collapsed" : "visible");
    out->badge = badge_of(P, a, p);
    return true;
}
/* Shows a hidden / collapsed panel, hides a visible one: the Panel Manager's own operations. */
void panels_panel_toggle(Panels *P, App *a, int p) {
    if (p < 0 || p >= P->dock.ndesc) return;
    bool vis = dock_is_visible(&P->dock, p) && !dock_is_collapsed(&P->dock, p);
    panel_op(P, a, p, vis ? OP_HIDE : OP_SHOW);
}
/* Status bar hint for focus mode / a maximized panel, or "". Valid until the next call. */
const char *panels_status(Panels *P) {
    if (P->dock.st.focus_mode) { char k[32]; chord_text(P, ACT_FOCUS_MODE, k, sizeof k); snprintf(P->status, sizeof P->status, "Focus mode  \xC2\xB7  %s restores the panels", k); return P->status; }
    if (P->dock.maximized_group >= 0) { snprintf(P->status, sizeof P->status, "Panel maximized  \xC2\xB7  Cmd+Alt+M restores"); return P->status; }
    return "";
}

/* ======================================================================== */
/* 15. frame                                                                 */
/* ======================================================================== */

/* One frame of the workspace in `body` (pixels). Order: requests from the modules,
 * connection change, modal input, popup, layout, overlay masking, drags and splitters, then
 * drawing (bar, strips, groups, editors, splitters, overlay), Esc handling and the coalesced
 * save. */
void panels_frame(Panels *P, App *a, Rect body) {
    Input *in = &a->in;
    Dock *d = &P->dock;
    P->s = a->scale;
    P->tt_show = false;
    panels_lifecycle(P, a);   /* visibility hooks for what appeared / disappeared last frame */

    /* requests from Results / Output (show the Output panel, back to Results, the Inspector) */
    bool want_out, want_res, toggle_in;
    results_take_requests(a->results, &want_out, &want_res, &toggle_in);
    if (want_out) { if (d->st.focus_mode) dock_open_overlay(d, P->P_OUT); else dock_show(d, P->P_OUT); P->last_panel = P->P_OUT; }
    if (want_res) { dock_show(d, P->P_RS); P->last_panel = P->P_RS; }
    if (toggle_in) { if (dock_is_visible(d, P->P_IN)) dock_hide(d, P->P_IN); else panel_op(P, a, P->P_IN, OP_SHOW); }
    results_set_inspector_shown(a->results, dock_is_visible(d, P->P_IN));

    /* the connection chosen in the Explorer belongs to the current query tab (PNL-027) */
    if (a->active != P->last_active) {
        if (!P->switching_conn && a->active) {
            /* a tab that already holds SQL for another connection keeps it: the newly chosen
             * connection gets its own query tab; an empty or unbound tab simply adopts it */
            EdTab *t = tab_of_editor(P, a->editor);
            if (t && t->conn_id[0] && strcmp(t->conn_id, a->active->id) && tab_has_text(t)) {
                P->last_active = a->active;
                panels_new_editor_tab(P, a, NULL);
                app_status(a, "%s opened in a new query tab; %s keeps its own connection", a->active->name, t->title);
            } else if (t) snprintf(t->conn_id, sizeof t->conn_id, "%s", a->active->id);
        }
        apply_binding_for(P, a, a->active);   /* PNL-034: the connection's own layout, however it became active */
        P->switching_conn = false;
        P->last_active = a->active;
    }

    bool modal = modal_input(P, a);
    if (modal) {
        /* the palette / prompt types; the editor or a grid below must not (it used to receive
         * the text typed into the palette) */
        P->nheld = in->nkeys; memcpy(P->held_keys, in->keys, sizeof in->keys[0] * (size_t)in->nkeys);
        P->nheld_text = in->ntext; memcpy(P->held_text, in->text, (size_t)in->ntext);
        in->nkeys = 0; in->ntext = 0;
    }
    int chosen = popup_update(a, &P->menu);
    if (chosen >= 0) handle_menu_choice(P, a, chosen);

    /* layout of this frame (the engine works in points) */
    P->m = dock_default_metrics(d);
    bool results_floating = d->st.panel[P->P_RS].mode == DOCK_FLOATING || d->st.panel[P->P_RS].mode == DOCK_DETACHED;
    if (a->results) results_follow_tab(a->results, a->editor);   /* Results shows the active query tab's last run */
    {   /* a report or a designer in front: the query panels below (Results, Plan) fold away, and come back
         * as they were when a query tab is in front again (only what Lume folded itself) */
        static int last_kind = -1; static bool folded[2];
        int kind = panels_active_report(P, a) || panels_active_designer(P, a) ? 1 : 0;
        if (kind != last_kind) {
            const char *ids[2] = { "lume.data.results", "lume.data.plan" };
            for (int k = 0; k < 2; k++) {
                int pp = dock_find(d, ids[k]);
                if (pp < 0) continue;
                if (kind == 1) {
                    folded[k] = false;
                    if (d->st.panel[pp].mode == DOCK_DOCKED && dock_is_visible(d, pp) && !dock_is_collapsed(d, pp) && dock_collapse_group(d, dock_group_of(d, pp))) folded[k] = true;
                } else if (folded[k]) { if (dock_is_collapsed(d, pp)) reveal(P, pp); folded[k] = false; }
            }
            last_kind = kind;
        }
    }
    if (a->results && !results_floating) results_pre_update(a->results, a);   /* else done with its window's input */
    dock_layout(d, body.x / P->s, body.y / P->s, body.w / P->s, body.h / P->s, &P->m, &P->lay);

    /* an open overlay keeps clicks and wheel for itself */
    Rect ov = P->lay.overlay_panel >= 0 ? R(P, P->lay.overlay) : rect_make(0, 0, 0, 0);
    bool mask = ov.w > 0 && rect_contains(ov, in->mouse_x, in->mouse_y);
    if (ov.w > 0 && !mask && in->mouse_pressed && !in->mouse_consumed && !modal && !P->menu.open && !rect_contains(R(P, P->lay.rail), in->mouse_x, in->mouse_y)) {
        dock_close_overlay(d);   /* click elsewhere puts an auto-hide panel away (R-09: pinned ones never) */
        ov.w = 0;
    }
    bool saved_consumed = in->mouse_consumed; float saved_wheel_x = in->wheel_x, saved_wheel_y = in->wheel_y;
    bool saved_r = in->mouse_rpressed;
    if (mask || modal) { in->mouse_consumed = true; in->wheel_x = in->wheel_y = 0; in->mouse_rpressed = false; }

    /* input of drags and splitters, then everything below the overlay */
    drag_update(P, a);
    if (P->drag == DRAG_NONE || P->drag == DRAG_SPLIT) splitters_input(P, a);

    draw_bar(P, a);
    draw_strips(P, a);
    /* panel groups: header + content */
    for (int g = 0; g < DOCK_MAX_GROUPS; g++) {
        if (!P->lay.group_shown[g]) continue;
        /* drag_update above may have hidden / moved this group's last panel (a bar click):
         * the layout is from before that, so skip a group that is now empty */
        if (!d->st.group[g].used || d->st.group[g].ntabs <= 0) continue;
        Rect gr = R(P, P->lay.group[g]);
        float hh = px(a, P->m.header_h);
        draw_group_header(P, a, g, rect_make(gr.x, gr.y, gr.w, hh), false);
        int p = d->st.group[g].active;
        if (p < 0) continue;
        Rect content = rect_make(gr.x, gr.y + hh, gr.w, gr.h - hh);
        if (rect_contains(content, in->mouse_x, in->mouse_y) && in->mouse_pressed && !in->mouse_consumed) P->last_panel = p;
        clip_push(a->renderer, content);
        panel_draw(P, a, p, content, panel_focused(P, a, p));
        clip_pop(a->renderer);
    }
    /* editor groups */
    if (!P->lay.editor_hidden)
        for (int eg = 0; eg < P->lay.neditors; eg++) draw_editor_group(P, a, eg, R(P, P->lay.editor[eg]));
    draw_splitters(P, a);

    if (mask || modal) { in->mouse_consumed = saved_consumed; in->wheel_x = saved_wheel_x; in->wheel_y = saved_wheel_y; in->mouse_rpressed = saved_r; }
    /* the modal is drawn last and owns the mouse: nothing until then may take this frame's click,
     * but its own widgets (field, rows, click-outside) get it back in draw_modal */
    P->modal_mouse_free = modal && !in->mouse_consumed;
    if (modal) in->mouse_consumed = true;

    /* the overlay on top (floating / detached panels live in their own windows: panels_render_floats) */
    if (P->lay.overlay_panel >= 0 && ov.w > 0) draw_panel_frame(P, a, P->lay.overlay_panel, ov, true);

    /* PNL-022: Esc that nothing used sends the keyboard back to the editor */
    if (!modal && !P->menu.open && a->focus != FOCUS_EDITOR && !results_modal_open(a->results))
        for (int k = 0; k < in->nkeys; k++) if (!in->keys[k].used && in->keys[k].key == SDLK_ESCAPE && !in->keys[k].mods) {
            in->keys[k].used = true;
            if (d->overlay_panel >= 0) dock_close_overlay(d); else a->focus = FOCUS_EDITOR;
        }
    maybe_save(P, a);
}

/* Last layer of the frame: drag preview, popup menus, the modal (after giving it back the
 * keys held from the panels) and the pending tooltip. */
void panels_draw_overlay(Panels *P, App *a) {
    draw_drag(P, a);
    popup_draw(a, &P->menu);
    if (P->modal != MODAL_NONE && (P->nheld || P->nheld_text)) {   /* give the held input back to the modal */
        Input *in = &a->in;
        memcpy(in->keys, P->held_keys, sizeof in->keys[0] * (size_t)P->nheld); in->nkeys = P->nheld;
        memcpy(in->text, P->held_text, (size_t)P->nheld_text); in->ntext = P->nheld_text;
        P->nheld = P->nheld_text = 0;
    }
    bool had_modal = P->modal != MODAL_NONE;
    draw_modal(P, a);
    if (had_modal) a->in.mouse_consumed = true;   /* nothing drawn later reacts to it */
    if (P->tt_show && P->drag == DRAG_NONE && !P->menu.open && P->modal == MODAL_NONE) {
        SDL_Renderer *r = a->renderer;
        float w = font_width(a->font_small, P->tt_text, -1) + px(a, 20);
        float x = P->tt_rect.x + P->tt_rect.w + px(a, 6), y = P->tt_rect.y + (P->tt_rect.h - px(a, 26)) / 2;
        if (x + w > a->width - px(a, 6)) x = P->tt_rect.x - w - px(a, 6);
        if (x < px(a, 6)) { x = P->tt_rect.x; y = P->tt_rect.y + P->tt_rect.h + px(a, 4); }
        if (y + px(a, 26) > a->height) y = P->tt_rect.y - px(a, 30);
        Rect tr = rect_make(x, y, w, px(a, 26));
        draw_rrect(r, tr, px(a, 5), color_hex(T_BORDER));
        draw_rrect(r, rect_inset(tr, px(a, 1), px(a, 1)), px(a, 4), color_hex(T_PANEL_ALT));
        draw_text_fit(a->font_small, r, P->tt_text, -1, tr, color_hex(T_TEXT), ALIGN_CENTER);
    }
}

/* ======================================================================== */
/* create / destroy                                                          */
/* ======================================================================== */

/* Builds the shell: engine, panel registration, keymap, the existing a->editor as the first
 * query tab (now owned by Panels), the saved layout (or Default if it was invalid), named
 * layouts and the dialog event types. Free with panels_destroy(). */
/* ---- extension modules (PNL-047) ---------------------------------------- */

#define MAX_MODULES 16
static struct { const char *id; PanelsModuleInit init; } modules[MAX_MODULES];
static int nmodules;

void panels_add_module(const char *module_id, PanelsModuleInit init) {
    if (nmodules < MAX_MODULES && module_id && init) { modules[nmodules].id = module_id; modules[nmodules].init = init; nmodules++; }
}

/* A module is off when LUME_DISABLE_MODULES lists its id (comma or space separated) or when
 * <prefs>/modules.txt has the line "<id>=off". */
static bool module_disabled(Panels *P, const char *id) {
    const char *env = SDL_getenv("LUME_DISABLE_MODULES");
    if (env) {
        size_t n = strlen(id);
        for (const char *s = env; (s = strstr(s, id)) != NULL; s += n)
            if ((s == env || s[-1] == ',' || s[-1] == ' ') && (s[n] == 0 || s[n] == ',' || s[n] == ' ')) return true;
    }
    char path[1100]; snprintf(path, sizeof path, "%smodules.txt", P->dir);
    FILE *f = fopen(path, "r");
    bool off = false;
    if (f) {
        char line[200], want[160]; snprintf(want, sizeof want, "%s=off", id);
        while (fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; if (!strcmp(line, want)) off = true; }
        fclose(f);
    }
    return off;
}

/* Runs every enabled module's init (it calls panels_register for its panels). */
static void init_modules(Panels *P, App *a) {
    for (int i = 0; i < nmodules; i++) {
        if (module_disabled(P, modules[i].id)) { LOG_I("module %s disabled: its panels are not registered", modules[i].id); continue; }
        LOG_I("module %s: init", modules[i].id);
        modules[i].init(P, a);
    }
}

bool panels_register(Panels *P, App *a, const DockPanelDesc *desc) {
    (void)a;
    if (!dock_register(&P->dock, desc)) { LOG_E("panel registration failed: %s", P->dock.err); return false; }
    LOG_I("panel %s registered", desc->id);
    return true;
}

/* Index p is going away: shift every per-panel array of the shell down and fix stored indices. */
static void forget_panel_index(Panels *P, int p) {
    int n = P->dock.ndesc;
    for (int i = p; i < n - 1; i++) {
        P->view[i] = P->view[i + 1]; P->created[i] = P->created[i + 1];
        P->shown_prev[i] = P->shown_prev[i + 1]; P->shown_now[i] = P->shown_now[i + 1];
    }
    if (n > 0) { P->view[n - 1] = NULL; P->created[n - 1] = P->shown_prev[n - 1] = P->shown_now[n - 1] = false; }
    P->tt_id = -1;   /* tooltip of a widget that may be gone */
    int *idx[] = { &P->last_panel, &P->menu_panel, &P->ext_panel };
    for (size_t k = 0; k < sizeof idx / sizeof idx[0]; k++) { if (*idx[k] == p) *idx[k] = -1; else if (*idx[k] > p) (*idx[k])--; }
    for (int i = 0; i < MAX_FLOATS; i++) if (P->fw[i].used && P->fw[i].panel > p) P->fw[i].panel--;
}

bool panels_unregister(Panels *P, App *a, const char *panel_id) {
    Dock *d = &P->dock;
    int p = dock_find(d, panel_id);
    if (p < 0) return false;
    if (p == P->P_EX || p == P->P_RS || p == P->P_OUT || p == P->P_IN) { LOG_W("refused to unregister %s: a Lume Data panel", panel_id); return false; }
    /* 1. close its native window, end a drag or an open menu that refers to it */
    for (int i = 0; i < MAX_FLOATS; i++) if (P->fw[i].used && P->fw[i].panel == p) fw_close(P, i);
    if (P->drag != DRAG_NONE && P->dpanel == p) P->drag = DRAG_NONE;
    if (P->menu.open && (P->menu_panel == p || P->ext_panel == p)) popup_close(&P->menu);
    /* 2. lifecycle: hidden, then destroyed */
    const DockPanelDesc ds = d->desc[p];   /* copy: the registry entry is removed below */
    if (P->created[p] || !ds.create) {
        void *view = ds.create ? P->view[p] : ds.ud;
        if (P->shown_prev[p] && ds.visibility) ds.visibility(view, a, false);
        if (P->created[p] && ds.destroy) ds.destroy(view, a);
    }
    /* 3. the tree: the engine compacts indices; the shell follows */
    forget_panel_index(P, p);
    if (!dock_unregister(d, panel_id)) { LOG_E("unregister %s failed: %s", panel_id, d->err); return false; }
    P->P_EX = dock_find(d, "lume.data.explorer"); P->P_RS = dock_find(d, "lume.data.results");
    P->P_OUT = dock_find(d, "lume.data.output"); P->P_IN = dock_find(d, "lume.data.inspector");
    char err[200];
    if (!dock_validate(d, &d->st, err, sizeof err)) LOG_E("layout invalid after unregistering %s: %s", panel_id, err);
    LOG_I("panel %s unregistered (module disabled); layout valid", panel_id);
    return true;
}

Panels *panels_create(App *a) {
    Panels *P = calloc(1, sizeof *P);
    dock_init(&P->dock);
    P->last_panel = -1; P->tt_id = -1;
    snprintf(P->dir, sizeof P->dir, "%s", a->conns ? a->conns->dir : "");
    register_panels(P);
    init_modules(P, a);   /* extension modules register before the layout file is read: their panels come back where they were */
    results_set_panels_external(a->results, true);
    load_keymap(P);
    /* the existing editor becomes the first query tab */
    add_tab(P, a, a->editor, "query.sql", 0);
    dock_editor_activate(&P->dock, P->tabs[0].id);
    /* layout: saved file, else the Default (a broken file is kept aside: PNL-032) */
    char path[1100], err[240];
    layout_path(P, path, sizeof path);
    FILE *probe = fopen(path, "r");
    bool existed = probe != NULL; if (probe) fclose(probe);
    if (dock_load_file(&P->dock, path, err, sizeof err)) LOG_I("layout restored from %s", path);
    else if (existed) {
        LOG_W("layout file rejected: %s; using the default layout", err);
        app_status(a, "The saved layout was invalid and was set aside; the default layout is in use (%s)", err);
        dock_apply_preset(&P->dock, DOCK_PRESET_DEFAULT);
        P->dock.nundo = 0;
    }
    P->saved_seq = P->seen_seq = P->dock.change_seq;
    load_named(P);
    P->ev_export = SDL_RegisterEvents(1); P->ev_import = SDL_RegisterEvents(1);
    P->last_active = a->active;
    return P;
}

/* Saves the layout if it changed, closes the panel windows, frees every editor (a->editor
 * becomes NULL) and the shell. */
void panels_destroy(Panels *P, App *a) {
    if (P->dock.change_seq != P->saved_seq || P->dock.dirty) save_now(P, a);
    for (int i = 0; i < MAX_FLOATS; i++) fw_close(P, i);   /* still floating: reopened next launch */
    for (int p = 0; p < P->dock.ndesc; p++)                 /* extension views (PNL-047) */
        if (P->created[p] && P->dock.desc[p].destroy) P->dock.desc[p].destroy(P->view[p], a);
    for (int i = 0; i < MAX_TABS; i++) if (P->tabs[i].used) { editor_destroy(P->tabs[i].ed); P->tabs[i].used = false; }
    a->editor = NULL;
    free(P);
}

/* Harness: centre of the open popup entry `label`, in window units; false when absent. */
bool panels_menu_pos(Panels *P, App *a, const char *label, float *x, float *y) {
    Rect rc;
    if (!popup_item_rect(a, &P->menu, label, &rc)) return false;
    *x = (rc.x + rc.w / 2) / a->density; *y = (rc.y + rc.h / 2) / a->density;
    return true;
}

/* ======================================================================== */
/* 16. native windows for floating / detached panels (PNL-019 / 020 / 021)   */
/* ======================================================================== */

/* True when the panel is floating or detached (drawn in its own window). */
bool panels_is_floating(Panels *P, const char *id) {
    int p = dock_find(&P->dock, id);
    return p >= 0 && (P->dock.st.panel[p].mode == DOCK_FLOATING || P->dock.st.panel[p].mode == DOCK_DETACHED);
}

/* (Re)loads the window's fonts for its renderer at its display scale (sizes in pixels). */
static void fw_fonts(FloatWin *f, App *a) {
    font_destroy(f->ui); font_destroy(f->small); font_destroy(f->mono);
    char ui[1200], mono[1200];
    snprintf(ui, sizeof ui, "%s/fonts/Inter-Variable.ttf", a->assets_dir);
    snprintf(mono, sizeof mono, "%s/fonts/JetBrainsMono-Regular.ttf", a->assets_dir);
    f->ui = font_load(f->ren, ui, roundf(M_FONT_UI * f->scale));
    f->small = font_load(f->ren, ui, roundf(M_FONT_SMALL * f->scale));
    f->mono = font_load(f->ren, mono, roundf(M_FONT_MONO * a->zoom * f->scale));
}

/* Refreshes pixel size, density and scale of a panel window; reloads fonts when asked or
 * when the display scale changed. */
static void fw_metrics(FloatWin *f, App *a, bool fonts) {
    int ww = 1, wh = 1;
    SDL_GetWindowSize(f->win, &ww, &wh);
    SDL_GetRenderOutputSize(f->ren, &f->w, &f->h);
    f->density = (float)f->w / (float)(ww > 0 ? ww : 1);
    float sc = SDL_GetWindowDisplayScale(f->win);
    if (sc <= 0) sc = f->density;
    /* a move to a display with another scale rebuilds the glyphs: no blurry text, no hitboxes off */
    if (fonts || fabsf(sc - f->scale) > 0.01f) { f->scale = sc; fw_fonts(f, a); }
}

/* PNL-020: a window must sit on a connected display; otherwise it moves to the primary one. */
static void fw_keep_visible(FloatWin *f) {
    int x, y, w, h; SDL_GetWindowPosition(f->win, &x, &y); SDL_GetWindowSize(f->win, &w, &h);
    int n = 0; SDL_DisplayID *ids = SDL_GetDisplays(&n);
    bool ok = false;
    for (int i = 0; ids && i < n && !ok; i++) {
        SDL_Rect b; if (!SDL_GetDisplayUsableBounds(ids[i], &b)) continue;
        /* at least 80 x 40 points of the window (its title bar) inside the display */
        int ix = SDL_max(x, b.x), iy = SDL_max(y, b.y), ax = SDL_min(x + w, b.x + b.w), ay = SDL_min(y + h, b.y + b.h);
        if (ax - ix >= 80 && ay - iy >= 40) ok = true;
    }
    if (!ok) {
        SDL_Rect b;
        if (SDL_GetDisplayUsableBounds(SDL_GetPrimaryDisplay(), &b)) {
            if (w > b.w - 40) w = b.w - 40;
            if (h > b.h - 40) h = b.h - 40;
            SDL_SetWindowSize(f->win, w, h);
            SDL_SetWindowPosition(f->win, b.x + (b.w - w) / 2, b.y + (b.h - h) / 2);
            LOG_I("a panel window was off every display and moved back to the primary display");
        }
    }
    SDL_free(ids);
}

/* Destroys panel window slot i (fonts, renderer, window). The panel's mode is not changed. */
static void fw_close(Panels *P, int i) {
    FloatWin *f = &P->fw[i];
    if (!f->used) return;
    font_destroy(f->ui); font_destroy(f->small); font_destroy(f->mono);
    SDL_DestroyRenderer(f->ren); SDL_DestroyWindow(f->win);
    memset(f, 0, sizeof *f);
}

/* Opens a native window for floating / detached panel p at its saved geometry. Floating
 * windows are utility children of the main window. On failure the panel is docked back. */
static void fw_open(Panels *P, App *a, int p) {
    int i = 0; while (i < MAX_FLOATS && P->fw[i].used) i++;
    if (i == MAX_FLOATS) { app_status(a, "Up to %d panel windows", MAX_FLOATS); dock_set_mode(&P->dock, p, DOCK_DOCKED); return; }
    FloatWin *f = &P->fw[i];
    memset(f, 0, sizeof *f);
    DockPanelState *ps = &P->dock.st.panel[p];
    f->panel = p; f->mode = ps->mode;
    SDL_WindowFlags fl = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (ps->mode == DOCK_FLOATING ? SDL_WINDOW_UTILITY : 0);
    if (SDL_getenv("LUME_SCRIPT") && !SDL_getenv("LUME_FOREGROUND")) fl |= SDL_WINDOW_HIDDEN;   /* test runs never put a window on screen (main.c) */
    char title[96]; snprintf(title, sizeof title, "%s \xE2\x80\x94 Lume", P->dock.desc[p].title);
    f->win = SDL_CreateWindow(title, (int)ps->float_w, (int)ps->float_h, fl);
    if (!f->win) { LOG_E("panel window: %s", SDL_GetError()); dock_set_mode(&P->dock, p, DOCK_DOCKED); return; }
    if (ps->mode == DOCK_FLOATING) SDL_SetWindowParent(f->win, a->window);   /* stays above Lume, minimizes with it */
    SDL_SetWindowPosition(f->win, (int)ps->float_x, (int)ps->float_y);
    SDL_SetWindowMinimumSize(f->win, 240, 160);
    f->ren = SDL_CreateRenderer(f->win, NULL);
    SDL_SetRenderVSync(f->ren, 0);   /* the main window paces the frames */
    f->used = true;
    fw_metrics(f, a, true);
    fw_keep_visible(f);
    SDL_StartTextInput(f->win);
    LOG_I("panel %s opened in a %s window", P->dock.desc[p].id, ps->mode == DOCK_FLOATING ? "floating" : "separate");
}

/* Windows follow the panels' modes: closes windows whose panel changed mode and opens one
 * for each floating / detached panel without a window. Called before drawing them. */
static void floats_sync(Panels *P, App *a) {
    for (int i = 0; i < MAX_FLOATS; i++) {
        FloatWin *f = &P->fw[i];
        if (!f->used) continue;
        int m = P->dock.st.panel[f->panel].mode;
        if (m != f->mode) fw_close(P, i);   /* docked again, hidden, or float <-> separate */
    }
    for (int p = 0; p < P->dock.ndesc; p++) {
        int m = P->dock.st.panel[p].mode;
        if (m != DOCK_FLOATING && m != DOCK_DETACHED) continue;
        bool open = false; for (int i = 0; i < MAX_FLOATS; i++) if (P->fw[i].used && P->fw[i].panel == p) open = true;
        if (!open) fw_open(P, a, p);
    }
}

/* Panel window slot of SDL window w, or NULL. */
static FloatWin *fw_of_window(Panels *P, SDL_Window *w) {
    for (int i = 0; w && i < MAX_FLOATS; i++) if (P->fw[i].used && P->fw[i].win == w) return &P->fw[i];
    return NULL;
}

/* Per-frame reset of each panel window's input (like app_input_begin for the main window). */
void panels_input_begin(Panels *P) { for (int i = 0; i < MAX_FLOATS; i++) if (P->fw[i].used) app_input_begin(&P->fw[i].in); }

/* Events of panel windows; true when consumed (they never reach the main window's input). */
static bool floats_event(Panels *P, App *a, const SDL_Event *e) {
    if (e->type == SDL_EVENT_DISPLAY_REMOVED || e->type == SDL_EVENT_DISPLAY_ADDED || e->type == SDL_EVENT_DISPLAY_ORIENTATION) {
        for (int i = 0; i < MAX_FLOATS; i++) if (P->fw[i].used) { fw_keep_visible(&P->fw[i]); fw_metrics(&P->fw[i], a, false); }
        return false;
    }
    FloatWin *f = fw_of_window(P, SDL_GetWindowFromEvent(e));
    if (!f) return false;
    Dock *d = &P->dock;
    DockPanelState *ps = &d->st.panel[f->panel];
    switch (e->type) {
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        /* PNL-021: closing the window puts the panel back; its content and work are untouched */
        dock_set_mode(d, f->panel, DOCK_DOCKED);
        app_status(a, "%s docked back into the main window", d->desc[f->panel].title);
        return true;
    case SDL_EVENT_WINDOW_MOVED: case SDL_EVENT_WINDOW_RESIZED: {
        int x, y, w, h; SDL_GetWindowPosition(f->win, &x, &y); SDL_GetWindowSize(f->win, &w, &h);
        ps->float_x = (float)x; ps->float_y = (float)y; ps->float_w = (float)w; ps->float_h = (float)h;
        d->dirty = true; d->change_seq++;
        fw_metrics(f, a, false);
        return true;
    }
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
        fw_metrics(f, a, false);
        return true;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        P->last_panel = f->panel;
        a->focus = f->panel == P->P_EX ? FOCUS_EXPLORER : FOCUS_RESULTS;
        return true;
    default: break;
    }
    /* input events: decoded into the window's own Input by swapping it into the App */
    if (e->type == SDL_EVENT_MOUSE_MOTION || e->type == SDL_EVENT_MOUSE_BUTTON_DOWN || e->type == SDL_EVENT_MOUSE_BUTTON_UP ||
        e->type == SDL_EVENT_MOUSE_WHEEL || e->type == SDL_EVENT_KEY_DOWN || e->type == SDL_EVENT_TEXT_INPUT) {
        Input keep = a->in; float kd = a->density;
        a->in = f->in; a->density = f->density;
        app_input_event(a, e);
        f->in = a->in; a->in = keep; a->density = kd;
        if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN) { P->last_panel = f->panel; a->focus = f->panel == P->P_EX ? FOCUS_EXPLORER : FOCUS_RESULTS; }
        app_wake_in(a, 0);
        return true;
    }
    return true;
}

/* Draws every panel window: the App's renderer, fonts, size and input are swapped for the
 * window's, the panel's own draw function runs, then everything is restored. */
void panels_render_floats(Panels *P, App *a) {
    floats_sync(P, a);
    for (int i = 0; i < MAX_FLOATS; i++) {
        FloatWin *f = &P->fw[i];
        if (!f->used) continue;
        SDL_Renderer *kr = a->renderer; Font *ku = a->font_ui, *ks = a->font_small, *km = a->font_mono;
        int kw = a->width, kh = a->height; float kd = a->density, ksc = a->scale; Input kin = a->in;
        a->renderer = f->ren; a->font_ui = f->ui; a->font_small = f->small; a->font_mono = f->mono;
        a->width = f->w; a->height = f->h; a->density = f->density; a->scale = f->scale; a->in = f->in;
        SDL_Renderer *r = f->ren;
        Color bg = color_hex(T_PANEL);
        SDL_SetRenderDrawColor(r, bg.r, bg.g, bg.b, 255);
        SDL_RenderClear(r);
        int p = f->panel;
        /* a thin bar: what this window is and how to put it back (closing it does the same) */
        Rect bar = rect_make(0, 0, (float)f->w, px(a, 30));
        draw_rect(r, bar, color_hex(T_PANEL_ALT));
        draw_rect(r, rect_make(0, bar.h - px(a, 1), bar.w, px(a, 1)), color_hex(T_BORDER));
        draw_panel_icon(a, P->dock.desc[p].icon ? P->dock.desc[p].icon : "?", px(a, 18), bar.h / 2, px(a, 13), color_hex(T_TEXT_MUTED), color_hex(T_PANEL_ALT));
        char t[96]; snprintf(t, sizeof t, "%s  \xC2\xB7  %s", P->dock.desc[p].title, f->mode == DOCK_FLOATING ? "floating" : "separate window");
        draw_text_fit(a->font_ui, r, t, -1, rect_make(px(a, 32), 0, bar.w - px(a, 180), bar.h), color_hex(T_TEXT), ALIGN_LEFT);
        Rect back = rect_make(bar.w - px(a, 146), px(a, 4), px(a, 140), bar.h - px(a, 8));
        if (ui_button(a, back, "Dock back in Lume", BTN_QUIET, true)) { dock_set_mode(&P->dock, p, DOCK_DOCKED); }
        Rect content = rect_make(0, bar.h, (float)f->w, (float)f->h - bar.h);
        if (p == P->P_RS && a->results) results_pre_update(a->results, a);
        panel_draw(P, a, p, content, P->last_panel == p);
        if (p == P->P_RS) results_draw_overlay(a->results, a);
        if (p == P->P_OUT) output_draw_overlay(a->output, a);
        if (P->float_shot[0]) {
            SDL_Surface *sf = SDL_RenderReadPixels(r, NULL);
            if (sf) { SDL_SaveBMP(sf, P->float_shot); SDL_DestroySurface(sf); }
            P->float_shot[0] = 0;
        }
        SDL_RenderPresent(r);
        f->in = a->in;
        a->renderer = kr; a->font_ui = ku; a->font_small = ks; a->font_mono = km;
        a->width = kw; a->height = kh; a->density = kd; a->scale = ksc; a->in = kin;
    }
}

/* Harness: saves the first panel window as a BMP at `path` after its next draw. */
void panels_shot_float(Panels *P, const char *path) { snprintf(P->float_shot, sizeof P->float_shot, "%s", path); }

/* harness: moves the first panel window to (x, y), window units, so a script can "unplug a
 * monitor" by moving it off every display */
void panels_move_float(Panels *P, int x, int y) { for (int i = 0; i < MAX_FLOATS; i++) if (P->fw[i].used) { SDL_SetWindowPosition(P->fw[i].win, x, y); return; } }
/* Harness / display change: brings every panel window back onto a connected display. */
void panels_revalidate_floats(Panels *P) { for (int i = 0; i < MAX_FLOATS; i++) if (P->fw[i].used) fw_keep_visible(&P->fw[i]); }
/* Harness: position of the first panel window, window units; false when none is open. */
bool panels_float_pos(Panels *P, int *x, int *y) { for (int i = 0; i < MAX_FLOATS; i++) if (P->fw[i].used) { SDL_GetWindowPosition(P->fw[i].win, x, y); return true; } return false; }

/* Harness: SDL id of the first panel window, 0 when none is open. */
SDL_WindowID panels_float_window_id(Panels *P) { for (int i = 0; i < MAX_FLOATS; i++) if (P->fw[i].used) return SDL_GetWindowID(P->fw[i].win); return 0; }

bool panels_flag(Panels *P, int which) { return P && (which == 0 ? P->dock.st.compact : P->dock.st.top_enabled); }
