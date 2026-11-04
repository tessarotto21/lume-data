/* SPDX-License-Identifier: AGPL-3.0-only */
/* main.c: application entry point and the glue between modules.
 *
 * Owns the single App (lume.h): creates the window, renderer, fonts, cursors and every module
 * (connections, explorer, editor, results, Output, menu, dialog, completer, panels shell), runs
 * the SDL event loop and calls frame() once per iteration. It also implements the "app_*"
 * services that other modules call back into (status bar messages, wake-ups, running SQL,
 * connecting / disconnecting, native open / save dialogs) and the mapping from menu commands
 * and keyboard shortcuts (ACT_* from menu.h) to actions.
 *
 * Loop model: the app redraws only when needed. Each iteration resets the per-frame Input,
 * waits for an event (or until App.wake_at, or not at all while App.animating), feeds all
 * pending events into App.in, then draws a full frame. Widgets ask for future redraws with
 * app_wake_in (caret blink, status timeout, the harness) or by setting App.animating.
 *
 * Units: SDL events arrive in window units and are multiplied by App.density into render
 * pixels; layout constants are UI points converted with px() (App.scale).
 *
 * Logging: log_init() runs first so start-up failures are logged; app_status() mirrors every
 * status-bar message into the log. Native file dialogs answer on another thread, so their
 * callbacks only push a user event (ev_open_path / ev_save_path) back to the main thread.
 * Everything else here runs on the main thread. */
#include "lume.h"
#include "db.h"
#include "draw.h"
#include "editor.h"
#include "complete.h"
#include "conn.h"
#include "dialog.h"
#include "explorer.h"
#include "font.h"
#include "menu.h"
#include "results.h"
#include "output.h"
#include "script.h"
#include "sysinfo.h"
#include "telemetry.h"
#include "crashrep.h"
#include "theme.h"
#include "log.h"
#include "panels.h"
#include "objed.h"
#include "tdes.h"
#include "planview.h"
#include "rptview.h"
#include <ctype.h>
#include "demo_shop.h"
#include "gitview.h"
#include "nsx.h"
#include "objcat.h"
#include "about.h"
#include "settings.h"
#include "brand.h"
#include "ui.h"
#include "mod_markdown.h"
#include "history_panel.h"
#include "caps.h"
#include "commands.h"
#include "toolbar.h"
#include "exec.h"
#include "update.h"
#include "dgmview.h"
#include "sshtun.h"
#include <stdarg.h>

/* LUME_VERSION comes from CMake (project VERSION): the single source for About, Copy Info and the log. */
#ifndef LUME_VERSION
#define LUME_VERSION "unknown"
#endif

/* ---- input helpers ----------------------------------------------------- */

/* Consume (mark used) the first unused key press of `key` with exactly the KM_* modifiers `mods`. */
bool input_key(Input *in, SDL_Keycode key, unsigned mods) {
    for (int i = 0; i < in->nkeys; i++) {
        KeyPress *k = &in->keys[i];
        if (!k->used && k->key == key && k->mods == mods) { k->used = true; return true; }
    }
    return false;
}

/* Consume the first unused press of `key` whatever its modifiers; *mods_out gets them (may be NULL). */
bool input_key_any(Input *in, SDL_Keycode key, unsigned *mods_out) {
    for (int i = 0; i < in->nkeys; i++) {
        KeyPress *k = &in->keys[i];
        if (!k->used && k->key == key) { k->used = true; if (mods_out) *mods_out = k->mods; return true; }
    }
    return false;
}

/* SDL modifier state -> KM_* flags. KM_PRIMARY is Cmd on macOS and Ctrl elsewhere; KM_CTRL is
 * only set on macOS (where Control is a separate modifier). */
static unsigned normalize_mods(SDL_Keymod m) {
    unsigned out = 0;
#ifdef __APPLE__
    if (m & SDL_KMOD_GUI) out |= KM_PRIMARY;
    if (m & SDL_KMOD_CTRL) out |= KM_CTRL;
#else
    if (m & SDL_KMOD_CTRL) out |= KM_PRIMARY;
#endif
    if (m & SDL_KMOD_SHIFT) out |= KM_SHIFT;
    if (m & SDL_KMOD_ALT) out |= KM_ALT;
    return out;
}

/* ---- app services ------------------------------------------------------ */

/* Shows a message in the status bar for 6 s and records it in the application log: what the
 * user saw is what the log says. Messages about failures are logged as warnings. */
void app_status(App *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a->status, sizeof a->status, fmt, ap);
    va_end(ap);
    bool bad = SDL_strcasestr(a->status, "failed") || SDL_strcasestr(a->status, "error") || SDL_strcasestr(a->status, "cannot");
    log_write(bad ? LOG_WARN : LOG_INFO, "status: %s", a->status);
    a->status_until = SDL_GetTicks() + 6000;
    app_wake_in(a, 6000);
}

/* Ask the event loop to redraw within `ms` milliseconds (keeps the earliest pending request). */
void app_wake_in(App *a, Uint32 ms) {
    Uint64 t = SDL_GetTicks() + ms;
    if (a->wake_at == 0 || t < a->wake_at) a->wake_at = t;
}

/* Run SQL on db (NULL = the active connection) and show it in the results according to `mode`
 * (RUN_*): table previews, reloads after Apply, Re-run, FK navigation. It runs on the execution
 * worker like an editor run, so a slow fetch (LOB columns over a slow link) never freezes the
 * window; ⌘. cancels it. The query tab's other results stay, and it is not added to Query
 * History. Refused while the connection is busy. */
bool app_run_sql_mode(App *a, Db *db, const char *sql, int len, const char *title, int mode, const char *qtable) {
    if (!db) db = a->db;
    if (!db) { app_status(a, "No connection. Open a database first."); return false; }
    OutRunOpts op = { .mode = mode, .title = title, .qtable = qtable, .keep_tabs = true, .no_history = mode != RUN_EDITOR,
                      .replace_exec = mode == RUN_REPLACE ? results_current_exec_id(a->results) : 0 };   /* replace the tab acted on, even if another is shown when it lands */
    return output_run_opts(a->output, a, db, sql, len, 0, -1, &op) > 0;   /* refusals (busy, Explorer listing) are explained in the status bar */
}

/* Run SQL against the active connection into the editor's result tab. */
void app_run_sql(App *a, const char *sql, int len, const char *title) { app_run_sql_mode(a, NULL, sql, len, title, RUN_EDITOR, NULL); }

/* editor runs (OUT-007 / OUT-018): split into statements, executed on the worker thread,
 * every step recorded in the Output; results land as the statements finish */
void app_run_async(App *a, const char *sql, int len, int mode, int sql_base) {
    if (!a->db) { app_status(a, "No connection. Open a database first."); return; }
    output_run(a->output, a, a->db, sql, len, mode, editor_revision(a->editor), sql_base);
}

/* See lume.h: an execution ended. The toolbar reads the running state each frame; this only
 * makes sure a frame follows so its Stop / Running indicator updates without waiting. */
void app_execution_finished(App *a, Db *db, int exec_id, int status) {
    if (a->toolbar) toolbar_execution_finished(a->toolbar, a, db, exec_id, status);
    app_wake_in(a, 0);
}

/* Insert text at the editor caret (e.g. a name dragged from the Explorer) and focus the editor. */
void app_editor_insert(App *a, const char *text) {
    editor_insert(a->editor, text, -1);
    a->focus = FOCUS_EDITOR;
}

/* ---- connections ------------------------------------------------------- */

/* Schema and sample rows created in the in-memory demo database on first connect. */
static const char *demo_sql =
    "CREATE TABLE customers(id INTEGER PRIMARY KEY, name TEXT NOT NULL, city TEXT, created_at TEXT);"
    "CREATE TABLE orders(id INTEGER PRIMARY KEY, customer_id INTEGER NOT NULL REFERENCES customers(id),"
    "  total REAL NOT NULL, status TEXT NOT NULL DEFAULT 'open', created_at TEXT);"
    "CREATE VIEW v_revenue AS SELECT c.name, SUM(o.total) AS revenue FROM customers c"
    "  JOIN orders o ON o.customer_id = c.id GROUP BY c.id;"
    "INSERT INTO customers(name, city, created_at) VALUES"
    "  ('Juliette Freire','Campina Grande','2024-01-12'),('Ana Souza','Recife','2024-02-03'),"
    "  ('Bruno Lima','Fortaleza','2024-02-19'),('Carla Mendes','Sao Paulo','2024-03-07'),"
    "  ('Diego Rocha','Curitiba','2024-03-21'),('Elisa Castro','Porto Alegre','2024-04-02'),"
    "  ('Fabio Nunes','Belo Horizonte','2024-04-18'),('Gabriela Reis','Salvador','2024-05-05');"
    "INSERT INTO orders(customer_id, total, status, created_at) VALUES"
    "  (1, 1250.00,'paid','2024-05-01'),(1, 320.50,'paid','2024-05-09'),(2, 89.90,'open','2024-05-10'),"
    "  (3, 450.00,'paid','2024-05-11'),(3, 1200.00,'cancelled','2024-05-12'),(4, 75.25,'paid','2024-05-13'),"
    "  (5, 999.99,'paid','2024-05-14'),(5, 15.00,'open','2024-05-15'),(6, 640.00,'paid','2024-05-16'),"
    "  (7, 210.10,'paid','2024-05-17'),(8, 1890.00,'paid','2024-05-18'),(8, 42.00,'open','2024-05-19'),"
    "  (2, 130.00,'paid','2024-05-20'),(4, 560.00,'open','2024-05-21');";

/* Text placed in the editor when the demo database is opened at start-up. */
static const char *welcome_sql =
    "-- Welcome to Lume. Put the cursor on a statement and run it.\n"
    "SELECT c.name, c.city, COUNT(o.id) AS orders, ROUND(SUM(o.total), 2) AS revenue\n"
    "FROM customers c\n"
    "LEFT JOIN orders o ON o.customer_id = c.id AND o.status = 'paid'\n"
    "GROUP BY c.id\n"
    "ORDER BY revenue DESC;\n"
    "\n"
    "SELECT * FROM orders WHERE status = 'open';\n";

/* Window title: "Lume - <active connection>" or just "Lume". */
static void set_title(App *a) {
    char t[300];
    snprintf(t, sizeof t, a->active ? "Lume \xE2\x80\x94 %s" : "Lume", a->active ? a->active->name : "");
    SDL_SetWindowTitle(a->window, t);
}

static App *g_app;   /* for the completer callbacks below */
/* Completer callback: a table's columns. Through the catalog session in the background when there
 * is one (false now, the editor asks again when they arrive); otherwise read directly, only while
 * the session is idle (SQLite, or the second session turned off). */
static int load_cols_cb(void *ctx, DbTable *t) {
    Connection *c = ctx;
    int st = g_app ? objcat_columns_into(g_app, c, t) : OCS_UNAVAILABLE;
    if (st == OCS_LOADED) return 1;
    if (st == OCS_LOADING) return 0;
    if (st == OCS_FAILED) return -1;
    if (!c->db || __atomic_load_n(&c->db->busy, __ATOMIC_SEQ_CST)) return -1;   /* the session is running something: not now */
    return conn_load_columns(c, t) ? 1 : -1;
}
/* Completer callback: another schema's object (or a synonym) as a table, columns in the background */
static DbTable *cat_table_cb(void *ctx, const char *schema, const char *name, int *state) { return g_app ? objcat_table(g_app, (Connection *)ctx, schema, name, state) : NULL; }
/* Completer callback: a table's foreign keys (JOIN ... ON), read once through the connection */
static int load_fks_cb(void *ctx, const char *table, DbFk **out, int *n) {
    Connection *c = ctx; char err[200];
    *out = NULL; *n = 0;
    if (!c || !c->db || !c->db->drv->list_fks) return -1;
    /* through the catalog session when there is one: never a round trip on this thread */
    const char *dot = strrchr(table, '.');
    char schema[160] = "";
    if (dot) snprintf(schema, sizeof schema, "%.*s", (int)(dot - table), table);
    else snprintf(schema, sizeof schema, "%s", c->schema[0] ? c->schema : "");
    DbFk *src = NULL; int ns = 0;
    int st = g_app ? objcat_fks(g_app, c, schema, dot ? dot + 1 : table, &src, &ns) : OCS_UNAVAILABLE;
    if (st == OCS_LOADING) return 0;
    if (st == OCS_FAILED) return -1;
    if (st == OCS_LOADED) {   /* the completer owns its copy */
        DbFk *cp = ns ? calloc((size_t)ns, sizeof *cp) : NULL;
        for (int i = 0; cp && i < ns; i++) { cp[i].name = src[i].name ? strdup(src[i].name) : NULL; cp[i].from_col = strdup(src[i].from_col ? src[i].from_col : ""); cp[i].to_table = strdup(src[i].to_table ? src[i].to_table : ""); cp[i].to_col = src[i].to_col ? strdup(src[i].to_col) : NULL; }
        *out = cp; *n = cp ? ns : 0;
        return 1;
    }
    if (__atomic_load_n(&c->db->busy, __ATOMIC_SEQ_CST)) return -1;   /* no catalog session: only while the editor's session is idle */
    return c->db->drv->list_fks(c->db, table, out, n, err, sizeof err) ? 1 : -1;
}

/* Point the completer at the active connection's cached schema (or clear it). */
void app_refresh_schema(App *a) {
    Connection *c = a->active;
    if (c && c->db) { complete_set_schema(a->completer, c->tables, c->ntables, c->id, load_cols_cb, c); complete_set_fk_loader(a->completer, load_fks_cb); }
    else complete_set_schema(a->completer, NULL, 0, NULL, NULL, NULL);
    /* every object the user may see (other schemas, synonyms, routines...), from objcat */
    g_app = a;
    complete_set_catalog(a->completer, c && c->db ? objcat_for(a, c) : NULL, c ? c->schema : NULL, cat_table_cb, c);
}

/* Make c (if connected) the connection the editor runs against; updates completer, title and
 * Explorer. NULL or a disconnected c leaves no active connection. */
void app_set_active(App *a, Connection *c) {
    a->active = c && c->db ? c : NULL;
    a->db = a->active ? a->active->db : NULL;
    app_refresh_schema(a);
    set_title(a);
    explorer_rebuild(a->explorer, a);
}

/* Connect c if needed and make it active. The demo connection gets its sample schema on first
 * use. On failure, logs, shows the error in the status bar and returns false. */
bool app_connect(App *a, Connection *c) {
    if (!c) return false;
    /* server drivers connect on a worker (TBR-045): the UI never waits for the network; the
     * connection becomes active when it is ready. SQLite opens a local file: done in place. */
    if (!c->db && strcmp(c->driver, "sqlite") != 0) {
        a->pending_activate = c;
        return app_connect_async(a, c, false);
    }
    if (!c->db && !conn_connect(c)) {
        LOG_E("connect failed: %s (%s): %s", c->name, c->driver, c->error);
        app_status(a, "%s: %s", c->name, c->error);
        explorer_rebuild(a->explorer, a);
        return false;
    }
    if (c->demo && c->ntables == 0) {
        ResultSet rs;
        c->db->drv->exec(c->db, c->demo == 2 ? demo_shop_sql : demo_sql, -1, 0, &rs);
        rs_free(&rs);
        conn_refresh(c);
    }
    if (!c->ro_level) c->ro_level = caps_apply_read_only(c);   /* TBR-013 */
    app_set_active(a, c);
    LOG_I("connected: %s (%s), %d tables", c->name, c->driver, c->ntables);
    app_status(a, "Connected to %s", c->name);
    return true;
}

/* See lume.h. Starts connecting c on a worker; the toolbar shows "connecting" meanwhile. */
bool app_connect_async(App *a, Connection *c, bool reconnect) {
    if (!c) return false;
    if (c->db) { app_set_active(a, c); return true; }
    if (!conn_connect_async(c, reconnect)) { app_status(a, "%s: %s", c->name, c->error[0] ? c->error : "already connecting"); return false; }
    LOG_I("%s %s (%s)", reconnect ? "reconnecting" : "connecting", c->name, c->driver);
    app_status(a, "%s %s\xE2\x80\xA6", reconnect ? "Reconnecting to" : "Connecting to", c->name);
    explorer_rebuild(a->explorer, a);
    app_wake_in(a, 50);
    return true;
}

/* See lume.h. Every frame: finishes background connects (catalog included). */
void app_poll_connections(App *a) {
    bool pending = false;
    for (int i = 0; i < a->conns->n; i++) {
        Connection *c = a->conns->items[i];
        if (!c->attempt) continue;
        if (!conn_poll(c)) { pending = true; continue; }
        if (c->db) {
            if (c->demo && c->ntables == 0) { ResultSet rs; c->db->drv->exec(c->db, c->demo == 2 ? demo_shop_sql : demo_sql, -1, 0, &rs); rs_free(&rs); conn_refresh(c); }
            c->ro_level = caps_apply_read_only(c);   /* TBR-013: as far as the engine allows */
            LOG_I("connected: %s (%s), %d tables", c->name, c->driver, c->ntables);
            app_status(a, "Connected to %s", c->name);
            if (a->pending_activate == c) { a->pending_activate = NULL; app_set_active(a, c); }   /* asked from the Explorer / a dialog */
            else if (a->panels) panels_connection_ready(a->panels, a, c);
        } else {
            if (a->pending_activate == c) a->pending_activate = NULL;
            LOG_E("connect failed: %s (%s): %s", c->name, c->driver, c->error);   /* the driver's message, never the location */
            app_status(a, "%s: %s", c->name, c->error);
            if (a->toolbar && !c->demo) toolbar_connect_failed(a->toolbar, a, c);   /* hard to miss: a dialog, not only the status bar */
        }
        explorer_rebuild(a->explorer, a);
    }
    if (pending) app_wake_in(a, 100);
}

/* See lume.h. Reconnect = close the session (its cursors first) and open a new one; never
 * reuses the old handle. An open transaction is reported as lost: the server decides its
 * fate, Lume does not claim it was committed (TBR-012 / 027). Refused while it is running. */
bool app_reconnect(App *a, Connection *c) {
    if (!c) return false;
    if (c->db && output_db_running(a->output, c->db)) { app_status(a, "%s is running a statement: stop it before reconnecting", c->name); return false; }
    bool was_active = a->active == c;
    if (c->db) {
        int tx = c->db->drv->tx_state ? c->db->drv->tx_state(c->db) : -1;
        c->tx_lost = tx == 1;
        if (c->tx_lost) {
            LOG_W("reconnect %s: an open transaction was dropped with the old session", c->name);
            output_note_tx_lost(a->output, a, c);
        }
        results_release_db(a->results, c->db);
        conn_disconnect(c);
        if (was_active) { a->active = NULL; a->db = NULL; }
    }
    return app_connect_async(a, c, true);
}

/* Close c (dropping result cursors that use it first); clears the active connection if it was c. */
void app_disconnect(App *a, Connection *c) {
    if (!c) return;
    if (a->pending_activate == c) a->pending_activate = NULL;
    if (c->attempt) { conn_cancel_connect(c); explorer_rebuild(a->explorer, a); app_status(a, "Connect to %s cancelled", c->name); return; }
    bool was_active = a->active == c;
    results_release_db(a->results, c->db);
    conn_disconnect(c);
    if (was_active) { a->active = NULL; a->db = NULL; app_refresh_schema(a); set_title(a); }
    explorer_rebuild(a->explorer, a);
    app_status(a, "Disconnected from %s", c->name);
}

/* Open the connection dialog for c, or for a new connection when c is NULL. */
void app_edit_connection(App *a, Connection *c) { dialog_open(a->dialog, a, c); }
void app_new_connection_in(App *a, const char *folder) { dialog_open_in_folder(a->dialog, a, folder); }

/* Persist the connection list and rebuild the Explorer after an add / edit / delete. */
void app_connections_changed(App *a) {
    conns_save(a->conns);
    explorer_rebuild(a->explorer, a);
}

/* Opens a SQLite file: reuses a saved connection for that path or creates one. */
static void open_sqlite_path(App *a, const char *path) {
    Connection *c = NULL;
    for (int i = 0; i < a->conns->n && !c; i++) {
        Connection *k = a->conns->items[i];
        if (strcmp(k->driver, "sqlite") == 0 && strcmp(conn_value(k, "path"), path) == 0) c = k;
    }
    if (!c) {
        c = conns_add(a->conns, "sqlite");
        conn_set_value(c, "path", path);
        const char *base = strrchr(path, '/');
        snprintf(c->name, sizeof c->name, "%s", base ? base + 1 : path);
        conns_save(a->conns);
    }
    app_connect(a, c);
}

/* Return the demo (in-memory SQLite) connection, creating and saving it when missing. */
static Connection *ensure_demo(App *a) {
    for (int i = 0; i < a->conns->n; i++) if (a->conns->items[i]->demo == 1) return a->conns->items[i];
    Connection *c = conns_add(a->conns, "sqlite");
    snprintf(c->name, sizeof c->name, "Demo (in-memory)");
    conn_set_value(c, "path", ":memory:");
    c->demo = 1;
    conns_save(a->conns);
    return c;
}
/* The e-commerce demo (in-memory SQLite, demo_shop.c): created once; reports and the assistant can be
 * tried on it without a database of one's own. Tests add it on purpose (command demo.shop). */
static App *g_app_for_cmd;
static Connection *ensure_shop_demo(App *a) {
    for (int i = 0; i < a->conns->n; i++) if (a->conns->items[i]->demo == 2) return a->conns->items[i];
    Connection *c = conns_add(a->conns, "sqlite");
    snprintf(c->name, sizeof c->name, "Shop Demo (e-commerce)");
    conn_set_value(c, "path", ":memory:");
    c->demo = 2;
    conns_save(a->conns);
    return c;
}
static void cmd_shop_demo(void *ud, const LumeCtx *cx) {
    (void)ud; (void)cx;
    App *a = g_app_for_cmd; if (!a) return;
    Connection *c = ensure_shop_demo(a);
    explorer_rebuild(a->explorer, a);
    app_connect(a, c);
}

/* Open-dialog callback (may run on another thread): forwards the chosen path to the main loop
 * as an ev_open_path user event; data1 is SDL_strdup'd and freed by handle_event. */
static void SDLCALL on_open_dialog(void *userdata, const char *const *files, int filter) {
    App *a = userdata;
    if (!files || !files[0]) return;
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = a->ev_open_path;
    ev.user.data1 = SDL_strdup(files[0]);
    SDL_PushEvent(&ev);
}

/* Show the native "open SQLite database" dialog (asynchronous; see on_open_dialog): the connection dialog's Browse. */
static void show_open_dialog(App *a) {
    static const SDL_DialogFileFilter filters[] = {
        { "SQLite database", "db;sqlite;sqlite3;db3" },
        { "All files", "*" },
    };
    SDL_ShowOpenFileDialog(on_open_dialog, a, a->window, filters, 2, SDL_GetUserFolder(SDL_FOLDER_HOME), false);
}
/* File > Open: any file Lume knows; app_open_path decides by its kind */
static void show_open_any(App *a) {
    static const SDL_DialogFileFilter filters[] = {
        { "Files Lume opens", "sql;lrpt;db;sqlite;sqlite3;db3;md;txt" },
        { "SQL", "sql" }, { "Lume report", "lrpt" }, { "SQLite database", "db;sqlite;sqlite3;db3" }, { "Markdown", "md" },
        { "All files", "*" },
    };
    SDL_ShowOpenFileDialog(on_open_dialog, a, a->window, filters, 6, SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS), false);
}

/* Open-file dialog requested by the connection dialog or the File menu. */
void app_browse_file(App *a) { show_open_dialog(a); }

/* Opens a file by what it is: a .sql (or any text) as a query tab bound to the file, a .lrpt as a
 * report, a SQLite database as a connection. An unknown extension is decided by the content. */
void app_open_path(App *a, const char *path) {
    const char *dot = strrchr(path, '.'), *slash = strrchr(path, '/');
    char ext[16] = "";
    if (dot && (!slash || dot > slash)) { size_t k = 0; for (const char *q = dot + 1; *q && k < sizeof ext - 1; q++) ext[k++] = (char)tolower((unsigned char)*q); ext[k] = 0; }
    int kind = 0;   /* 1 query / text, 2 report, 3 database */
    if (!strcmp(ext, "lrpt")) kind = 2;
    else if (!strcmp(ext, "db") || !strcmp(ext, "sqlite") || !strcmp(ext, "sqlite3") || !strcmp(ext, "db3")) kind = 3;
    else if (!strcmp(ext, "sql") || !strcmp(ext, "md") || !strcmp(ext, "txt") || !strcmp(ext, "psql") || !strcmp(ext, "ddl")) kind = 1;
    else {
        FILE *f = fopen(path, "rb"); char head[4097] = ""; size_t n = 0;
        if (f) { n = fread(head, 1, sizeof head - 1, f); fclose(f); head[n] = 0; }
        if (n >= 16 && !memcmp(head, "SQLite format 3", 16)) kind = 3;
        else if (n && head[strspn(head, " \t\r\n")] == '{' && strstr(head, "\"formatVersion\"") && strstr(head, "\"pages\"")) kind = 2;
        else kind = 1;
    }
    LOG_I("open: %s file (%s)", kind == 2 ? "report" : kind == 3 ? "database" : "text", ext[0] ? ext : "no extension");
    if (kind == 2) { if (!rptview_open_path(a, path)) app_status(a, "Could not open the report"); }
    else if (kind == 3) open_sqlite_path(a, path);
    else if (panels_open_file_tab(a->panels, a, path) < 0) app_status(a, "Could not open %s", path);
}

/* Current keyboard modifiers, or the ones forced by the harness ("mods" command). */
SDL_Keymod app_modstate(App *a) { return a->mod_override >= 0 ? (SDL_Keymod)a->mod_override : SDL_GetModState(); }

/* Save-dialog callback (may run on another thread): forwards the path as an ev_save_path event. */
static void SDLCALL on_save_dialog(void *userdata, const char *const *files, int filter) {
    App *a = userdata;
    if (!files || !files[0]) return;
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = a->ev_save_path;
    ev.user.data1 = SDL_strdup(files[0]);
    SDL_PushEvent(&ev);
}

/* Show the native save dialog for exporting results; `scope` (0 selection, 1 loaded rows) is
 * remembered in App.export_scope for when the path comes back. */
void app_export_results(App *a, int scope) {
    static const SDL_DialogFileFilter filters[] = {
        { "CSV", "csv" }, { "TSV", "tsv" }, { "JSON", "json" }, { "Markdown", "md" }, { "SQL INSERT", "sql" }, { "Excel", "xlsx" },
    };
    a->export_scope = scope;
    SDL_ShowSaveFileDialog(on_save_dialog, a, a->window, filters, 6, NULL);
}

/* ---- fonts / scale ----------------------------------------------------- */

/* True when path exists and is a regular file. */
static bool file_exists(const char *path) {
    SDL_PathInfo info;
    return SDL_GetPathInfo(path, &info) && info.type == SDL_PATHTYPE_FILE;
}

/* Load assets/fonts/<name> at `pt` UI points (converted to render pixels). Exits the process
 * when the font can't be loaded: the UI can't work without it. */
static Font *load_font(App *a, const char *name, float pt) {
    char path[1200];
    snprintf(path, sizeof path, "%s/fonts/%s", a->assets_dir, name);
    Font *f = font_load(a->renderer, path, roundf(pt * a->scale));
    if (!f) { SDL_Log("Could not load font %s", path); exit(1); }
    return f;
}

/* (Re)load the three fonts for the current scale and editor zoom, freeing the old ones. */
static void rebuild_fonts(App *a) {
    font_destroy(a->font_ui);
    font_destroy(a->font_small);
    font_destroy(a->font_mono);
    a->font_ui = load_font(a, "Inter-Variable.ttf", M_FONT_UI);
    a->font_small = load_font(a, "Inter-Variable.ttf", M_FONT_SMALL);
    a->font_mono = load_font(a, "JetBrainsMono-Regular.ttf", M_FONT_MONO * a->zoom);
}

/* Refresh render size, density and UI scale after a resize or a move to another display;
 * reloads the fonts when the scale changed (or on first call). */
static void update_metrics(App *a) {
    int ww = 1, wh = 1;
    SDL_GetWindowSize(a->window, &ww, &wh);
    SDL_GetRenderOutputSize(a->renderer, &a->width, &a->height);
    a->density = (float)a->width / (float)ww;
    /* Display scale already accounts for pixel density (2.0 on Retina, 1.5 at 150% on Windows). */
    float scale = SDL_GetWindowDisplayScale(a->window);
    if (scale <= 0) scale = a->density;
    /* LUME_UI_SCALE multiplies it (screenshots at other densities, TBR-051; larger UI on demand) */
    const char *us = SDL_getenv("LUME_UI_SCALE");
    if (us) { float m = (float)atof(us); if (m >= 0.5f && m <= 3.0f) scale *= m; }
    if (fabsf(scale - a->scale) > 0.01f || !a->font_ui) {
        a->scale = scale;
        rebuild_fonts(a);
    }
}

/* ---- events ------------------------------------------------------------ */

/* Mouse, wheel, keys and text into a->in (pixels of a->window). The shell calls it with a
 * floating panel's own Input and density swapped in, so every window gets the same handling. */
void app_input_event(App *a, const SDL_Event *e) {
    Input *in = &a->in;
    switch (e->type) {
    case SDL_EVENT_MOUSE_MOTION:
        in->mouse_x = e->motion.x * a->density;
        in->mouse_y = e->motion.y * a->density;
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        in->mouse_x = e->button.x * a->density;
        in->mouse_y = e->button.y * a->density;
        if (e->button.button == SDL_BUTTON_LEFT) {
            in->mouse_down = true;
            in->mouse_pressed = true;
            in->clicks = e->button.clicks;
        } else if (e->button.button == SDL_BUTTON_RIGHT) in->mouse_rpressed = true;
        break;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e->button.button == SDL_BUTTON_LEFT) { in->mouse_down = false; in->mouse_released = true; }
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        float step = px(a, 18);
        /* SDL already delivers x / y in the direction the user chose in System Settings
         * ("natural scrolling" sets direction = FLIPPED and pre-flips the values). Using them
         * as they come makes Lume scroll like every other macOS app; undoing the flip here
         * inverted the trackpad.
         * Axis convention differs, though: SDL's y > 0 means "toward the top" but x > 0 means
         * "toward the right". Views treat both wheel_x and wheel_y as "toward the start"
         * (scroll -= wheel), so x is negated here; without it sideways scrolling ran backwards. */
        float sx = e->wheel.x, sy = e->wheel.y;
        in->wheel_x -= sx * step;
        in->wheel_y += sy * step;
        break;
    }
    case SDL_EVENT_PINCH_UPDATE:   /* trackpad pinch: views that zoom read the factor (diagrams) */
        if (e->pinch.scale > 0) in->pinch = (in->pinch > 0 ? in->pinch : 1) * e->pinch.scale;
        break;
    case SDL_EVENT_KEY_DOWN:
        if (in->nkeys < LUME_MAX_KEYS) {
            KeyPress *k = &in->keys[in->nkeys++];
            k->key = e->key.key;
            k->mods = normalize_mods(e->key.mod);
            k->used = false;
        }
        break;
    case SDL_EVENT_TEXT_INPUT: {
        int l = (int)strlen(e->text.text);
        if (in->ntext + l < (int)sizeof in->text) { memcpy(in->text + in->ntext, e->text.text, l); in->ntext += l; }
        break;
    }
    default: break;
    }
}

/* Reset the per-frame parts of an Input (presses, releases, wheel, keys, text); mouse position
 * and button-held state carry over. */
void app_input_begin(Input *in) {
    in->mouse_pressed = in->mouse_released = in->mouse_consumed = in->mouse_rpressed = false;
    in->clicks = 0;
    in->wheel_x = in->wheel_y = 0;
    in->pinch = 1;
    in->nkeys = 0;
    in->ntext = 0;
}

/* Dispatch one SDL event: panel windows first, then quit / resize / input / file drop and the
 * app's own user events (paths from the native dialogs). */
static void handle_event(App *a, SDL_Event *e) {
    if (a->panels && panels_handle_event(a->panels, a, e)) return;   /* layout export / import dialogs */
    switch (e->type) {
    case SDL_EVENT_QUIT: a->running = false; break;
    case SDL_EVENT_SYSTEM_THEME_CHANGED: if (theme_follow_system()) LOG_I("theme: the system appearance changed, now %s", g_theme->name); break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
        update_metrics(a);
        break;
    case SDL_EVENT_MOUSE_MOTION: case SDL_EVENT_MOUSE_BUTTON_DOWN: case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL: case SDL_EVENT_KEY_DOWN: case SDL_EVENT_TEXT_INPUT: case SDL_EVENT_PINCH_UPDATE:
        app_input_event(a, e);
        break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        /* with floating panel windows open SDL sends no QUIT until the last one closes: closing
         * the main window ends the app */
        if (SDL_GetWindowFromEvent(e) == a->window) a->running = false;
        break;
    case SDL_EVENT_DROP_FILE:
        if (rptview_drop_file(a, e->drop.data, e->drop.windowID, e->drop.x * a->density, e->drop.y * a->density)) break;   /* onto the assistant's chat: attached */
        app_open_path(a, e->drop.data);   /* a dropped .sql / .lrpt / database opens by its kind */
        break;
    default:
        if (e->type == a->ev_open_path) {
            char *path = e->user.data1;
            if (dialog_is_open(a->dialog)) dialog_file_chosen(a->dialog, path);
            else app_open_path(a, path);
            SDL_free(path);
        } else if (e->type == a->ev_save_path) {
            char *path = e->user.data1;
            results_export(a->results, a, path);
            SDL_free(path);
        } else if (e->type == about_event_type() && e->type) about_open(a);   /* native app menu > About */
        else if (e->type == settings_event_type() && e->type) settings_open(a);   /* native app menu > Settings... */
        else objed_handle_event(a, e);   /* the object editor's own save dialog */
    }
}

/* Per-frame reset of the main window's Input (same as app_input_begin). */
static void input_begin_frame(Input *in) {
    in->mouse_pressed = in->mouse_released = in->mouse_consumed = in->mouse_rpressed = false;
    in->clicks = 0;
    in->wheel_x = in->wheel_y = 0;
    in->pinch = 1;
    in->nkeys = 0;
    in->ntext = 0;
}

/* ---- actions ----------------------------------------------------------- */

/* Run the selection, or else the statement under the caret, asynchronously (Cmd+Return).
 * The text is also learnt by the completer. `mode` is RUN_*. */
/* See lume.h. The SQL a run takes from the active editor:
 *   AUTO       the selection when there is one, else the statement under the caret (TBR-017)
 *   SELECTION  exactly the selection; refused when it cuts a statement in two, unless allow_partial
 *   STATEMENT  the statement under the caret
 *   FILE       the whole text (Run All in File)
 * Returns malloc'd text with *len and *base (offset in the editor), or NULL with why. */
char *app_editor_sql(App *a, int src, bool allow_partial, int *len, int *base, char *why, size_t n) {
    *base = 0; *len = 0;
    if (!a->editor) { snprintf(why, n, "No query tab"); return NULL; }
    if (editor_object_mode(a->editor)) {
        /* OBJ-062: a definition tab is not a console; publishing goes through Apply's review */
        snprintf(why, n, "This tab edits a database object: use Apply\xE2\x80\xA6 to publish it, or Open in Query Console to run a copy");
        return NULL;
    }
    if (src == RUNSRC_FILE) {
        char *t = editor_text(a->editor, len);
        if (!t || !*len) { free(t); snprintf(why, n, "The query tab is empty"); return NULL; }
        return t;
    }
    char *sel = (src == RUNSRC_AUTO || src == RUNSRC_SELECTION) ? editor_selection(a->editor, len) : NULL;
    if (sel && *len > 0) {
        *base = editor_selection_start(a->editor);
        if (src == RUNSRC_SELECTION && !allow_partial) {
            /* does the selection cut a statement of the whole text in two? */
            int tl = 0; char *t = editor_text(a->editor, &tl);
            SqlStmt *parts = malloc(sizeof *parts * 1024);
            int np = t && parts ? sql_split(t, tl, a->db ? sql_dialect_for(a->db->drv) : SQL_DIALECT_GENERIC, parts, 1024) : 0;
            int s0 = *base, s1 = *base + *len; bool partial = false;
            for (int i = 0; i < np; i++) {
                bool overlaps = s0 < parts[i].end && s1 > parts[i].start;
                if (overlaps && (s0 > parts[i].start || s1 < parts[i].end)) partial = true;
            }
            free(parts); free(t);
            if (partial) { free(sel); snprintf(why, n, "The selection covers only part of a statement"); return NULL; }
        }
        return sel;
    }
    free(sel);
    if (src == RUNSRC_SELECTION) { snprintf(why, n, "Nothing is selected"); return NULL; }
    char *st = editor_statement_at_cursor_pos(a->editor, len, base);
    if (!st) snprintf(why, n, "Nothing to run under the caret");
    return st;
}

/* See lume.h: starts an execution of sql on the active session with options (title, note for
 * the Output, display text, export file). Returns the execution id, 0 when it did not start. */
int app_run_sql_opts(App *a, const char *sql, int len, int base, const OutRunOpts *op) {
    if (!a->db) { app_status(a, "No connection. Open a database first."); return 0; }
    complete_learn_statement(a->completer, sql, len);
    return output_run_opts(a->output, a, a->db, sql, len, editor_revision(a->editor), base, op);
}

static void run_statement(App *a, int mode) {
    int len, base = 0;
    char *sql = editor_selection(a->editor, &len);
    if (sql) base = editor_selection_start(a->editor);
    if (!sql) sql = editor_statement_at_cursor_pos(a->editor, &len, &base);
    if (!sql) { app_status(a, "Nothing to run"); return; }
    complete_learn_statement(a->completer, sql, len);
    app_run_async(a, sql, len, mode, base);
    free(sql);
}

/* Status message after a display-format change. */
static void set_format(App *a, const char *what) { app_status(a, "Display format: %s", what); }

/* Set the editor font zoom (clamped to 60%..250%) and reload the fonts. */
static void set_zoom(App *a, float z);
void app_set_zoom(App *a, float z) { set_zoom(a, z); }
/* ---- app-level settings (settings.txt); learning and data stay in their own files ---- */
static void prefs_path(App *a, char *o, size_t n) { snprintf(o, n, "%ssettings.txt", a->conns ? a->conns->dir : ""); }
static void app_prefs_load(App *a) {
    char p[1200]; prefs_path(a, p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[300]; float z = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '='); if (!eq || line[0] == '#') continue; *eq++ = 0;
        int v = atoi(eq);
        if (!strcmp(line, "fetch_size") && v >= 10 && v <= 1000000) a->fetch_size = v;
        else if (!strcmp(line, "continue_on_error")) a->continue_on_error = v != 0;
        else if (!strcmp(line, "results_budget_mb") && v >= 16 && v <= 16384) a->results_budget_mb = v;
        else if (!strcmp(line, "zoom")) z = (float)atof(eq);
        else if (!strcmp(line, "fmt_group")) a->fmt_group = v != 0;
        else if (!strcmp(line, "fmt_decimals") && v >= -1 && v <= 10) a->fmt_decimals = v;
        else if (!strcmp(line, "fmt_date") && v >= 0 && v <= 2) a->fmt_date = v;
        else if (!strcmp(line, "fmt_tz") && v >= 0 && v <= 2) a->fmt_tz = v;
        else if (!strcmp(line, "telemetry")) a->telemetry = v != 0;
        else if (!strcmp(line, "crash_reports") && v >= 0 && v <= 2) a->crash_reports = v;
        else if (!strcmp(line, "update_check")) a->update_check = v != 0;
        else if (!strcmp(line, "query_timeout_s") && v >= 0 && v <= 86400) a->query_timeout_s = v;
        else if (!strcmp(line, "connect_timeout_s") && v >= 2 && v <= 600) a->connect_timeout_s = v;
        else if (!strcmp(line, "ssh_timeout_s") && v >= 2 && v <= 600) a->ssh_timeout_s = v;
        else if (!strcmp(line, "explorer_timeout_s") && v >= 1 && v <= 600) a->explorer_timeout_s = v;
        else if (!strcmp(line, "explorer_list_min") && v >= 1 && v <= 240) a->explorer_list_min = v;
        else if (!strcmp(line, "report_timeout_s") && v >= 5 && v <= 3600) a->report_timeout_s = v;
        else if (!strcmp(line, "git_timeout_min") && v >= 1 && v <= 120) a->git_timeout_min = v;
        else if (!strcmp(line, "git_clone_min") && v >= 1 && v <= 1440) a->git_clone_min = v;
        else if (!strcmp(line, "catalog_session")) a->catalog_session = v != 0;
        else if (!strcmp(line, "theme")) { if (!theme_select(eq)) LOG_W("settings: unknown theme \"%.40s\", keeping %s", eq, theme_choice()); }
        else if (!strcmp(line, "report_locale") && (!strcmp(eq, "pt-BR") || !strcmp(eq, "en-US"))) snprintf(a->report_locale, sizeof a->report_locale, "%s", eq);
    }
    fclose(f);
    if (z >= 0.6f && z <= 2.5f && z != a->zoom) set_zoom(a, z);
}
/* Hands the timeouts to the modules that use them (they keep no App pointer). */
static void app_apply_timeouts(const App *a) {
    db_connect_timeout_s = a->connect_timeout_s;
    conn_ssh_timeout_s = a->ssh_timeout_s;
    nsx_set_timeouts(a->explorer_timeout_s * 1000, a->explorer_list_min * 60 * 1000);
    gitview_set_timeouts(a->git_timeout_min * 60 * 1000, a->git_clone_min * 60 * 1000);
}
static unsigned prefs_sig(const App *a) {
    char b[360]; snprintf(b, sizeof b, "%d|%d|%d|%.2f|%d|%d|%d|%d|%s|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%s", a->fetch_size, a->continue_on_error, a->results_budget_mb, a->zoom, a->fmt_group, a->fmt_decimals, a->fmt_date, a->fmt_tz, a->report_locale, a->telemetry, a->crash_reports, a->update_check,
                             a->query_timeout_s, a->connect_timeout_s, a->ssh_timeout_s, a->explorer_timeout_s, a->explorer_list_min, a->report_timeout_s, a->git_timeout_min, a->git_clone_min, a->catalog_session, theme_choice());
    unsigned h = 2166136261u; for (const char *q = b; *q; q++) { h ^= (unsigned char)*q; h *= 16777619u; } return h;
}
static unsigned g_prefs_sig; static bool g_prefs_loaded;
void app_prefs_save(App *a) {
    char p[1200], tmp[1300]; prefs_path(a, p, sizeof p); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "w"); if (!f) return;
    fprintf(f, "# Lume settings (Tools > Settings...). Rules live here; learned data (history, usage, caches) stays in its own files.\n");
    fprintf(f, "fetch_size=%d\ncontinue_on_error=%d\nresults_budget_mb=%d\nzoom=%.2f\nfmt_group=%d\nfmt_decimals=%d\nfmt_date=%d\nfmt_tz=%d\n", a->fetch_size, a->continue_on_error, a->results_budget_mb, a->zoom, a->fmt_group, a->fmt_decimals, a->fmt_date, a->fmt_tz);
    if (a->report_locale[0]) fprintf(f, "report_locale=%s\n", a->report_locale);
    fprintf(f, "telemetry=%d\ncrash_reports=%d\nupdate_check=%d\n", a->telemetry ? 1 : 0, a->crash_reports, a->update_check ? 1 : 0);
    fprintf(f, "query_timeout_s=%d\nconnect_timeout_s=%d\nssh_timeout_s=%d\nexplorer_timeout_s=%d\nexplorer_list_min=%d\nreport_timeout_s=%d\ngit_timeout_min=%d\ngit_clone_min=%d\n",
            a->query_timeout_s, a->connect_timeout_s, a->ssh_timeout_s, a->explorer_timeout_s, a->explorer_list_min, a->report_timeout_s, a->git_timeout_min, a->git_clone_min);
    fprintf(f, "catalog_session=%d\n", a->catalog_session ? 1 : 0);
    fprintf(f, "theme=%s\n", theme_choice());
    fclose(f); rename(tmp, p);
    g_prefs_sig = prefs_sig(a);
    app_apply_timeouts(a);
}
static void app_prefs_track(App *a) {   /* once per frame: a setting changed (menu, shortcut, Settings) is saved */
    if (!g_prefs_loaded) return;
    unsigned sg = prefs_sig(a);
    if (sg != g_prefs_sig) app_prefs_save(a);
}
static void set_zoom(App *a, float z) {
    a->zoom = clampf(z, 0.6f, 2.5f);
    rebuild_fonts(a);
    app_status(a, "Editor zoom %d%%", (int)roundf(a->zoom * 100));
}

/* Execute a menu / shortcut / palette command (ACT_*). The panels shell gets the first chance
 * (it owns panel, layout and editor-tab commands); everything else is handled here. */
static void do_action(App *a, int act) {
    /* one rule per command for every entry point: a disabled command does nothing and says why (TBR-048) */
    if (!commands_gate(a, act)) return;
    /* the toolbar owns its commands and the safety checks of every run (PROD, read-only, target) */
    if (a->toolbar && toolbar_action(a->toolbar, a, act)) return;
    /* panels, layouts, editor tabs and the commands that now act on panels */
    if (a->panels && panels_action(a->panels, a, act)) return;
    switch (act) {
    case ACT_NEW_QUERY: editor_set_text(a->editor, ""); a->focus = FOCUS_EDITOR; break;
    case ACT_OPEN_DB: if (SDL_getenv("LUME_TEST_OPEN")) app_open_path(a, SDL_getenv("LUME_TEST_OPEN")); else show_open_any(a); break;
    case ACT_NEW_CONN: app_edit_connection(a, NULL); break;
    case ACT_EDIT_CONN: {
        Connection *c = a->focus == FOCUS_EXPLORER ? explorer_selected_conn(a->explorer) : NULL;
        if (!c) c = a->active;
        if (c) app_edit_connection(a, c); else app_status(a, "Select a connection first");
        break;
    }
    case ACT_TOGGLE_CONN: {
        Connection *c = a->focus == FOCUS_EXPLORER ? explorer_selected_conn(a->explorer) : NULL;
        if (!c) c = a->active;
        if (!c) app_status(a, "Select a connection first");
        else if (c->db) app_disconnect(a, c);
        else app_connect(a, c);
        break;
    }
    case ACT_QUIT: a->running = false; break;
    case ACT_UNDO: editor_undo(a->editor); a->focus = FOCUS_EDITOR; break;
    case ACT_REDO: editor_redo(a->editor); a->focus = FOCUS_EDITOR; break;
    case ACT_COMPLETE: editor_complete(a->editor, a); a->focus = FOCUS_EDITOR; break;
    case ACT_CUT: editor_cut(a->editor); break;
    /* with the Output tab shown, copy / select all / Shift+Cmd+C act on its events, not on the
     * result grid hidden behind it (which used to be copied silently) */
    case ACT_COPY:
        if (a->focus == FOCUS_RESULTS && output_copy_text_selection(a->output, a)) {}   /* text selected in the Output's text view */
        else if (a->focus == FOCUS_RESULTS && results_output_active(a->results)) output_copy(a->output, a, false);
        else if (a->focus == FOCUS_RESULTS) results_copy_selection(a->results); else editor_copy(a->editor);
        break;
    case ACT_PASTE: editor_paste(a->editor); a->focus = FOCUS_EDITOR; break;
    case ACT_SELECT_ALL: if (a->focus == FOCUS_RESULTS && results_output_active(a->results)) output_select_all(a->output); else if (a->focus == FOCUS_RESULTS) results_select_all(a->results); else { editor_select_all(a->editor); a->focus = FOCUS_EDITOR; } break;
    case ACT_FIND: results_find(a->results); a->focus = FOCUS_RESULTS; if (a->panels) panels_show(a->panels, a, "lume.data.results"); break;
    case ACT_EXPORT: results_export_dialog(a->results, a, 1); break;
    case ACT_RUN: run_statement(a, RUN_EDITOR); break;
    case ACT_RUN_NEW_TAB: run_statement(a, RUN_NEW_TAB); break;
    case ACT_HISTORY: results_history_menu(a->results, a); break;
    case ACT_NEXT_TAB: results_next_tab(a->results, 1); break;
    case ACT_PREV_TAB: results_next_tab(a->results, -1); break;
    case ACT_CLOSE_TAB: if (!results_close_tab(a->results, a)) app_status(a, "No result tab to close"); break;
    case ACT_COLUMNS:   /* Shift+Cmd+C: Columns on a result tab, "copy details" on the Output */
        if (a->focus == FOCUS_RESULTS && results_output_active(a->results)) output_copy(a->output, a, true);
        else { results_columns_panel(a->results); a->focus = FOCUS_RESULTS; }
        break;
    case ACT_BUDGET_64: a->results_budget_mb = 64; app_status(a, "Results memory budget: 64 MB"); break;
    case ACT_BUDGET_256: a->results_budget_mb = 256; app_status(a, "Results memory budget: 256 MB"); break;
    case ACT_BUDGET_1024: a->results_budget_mb = 1024; app_status(a, "Results memory budget: 1 GB"); break;
    case ACT_FMT_GROUP: a->fmt_group = !a->fmt_group; set_format(a, a->fmt_group ? "thousands separator on" : "thousands separator off"); break;
    case ACT_FMT_DEC_RAW: a->fmt_decimals = -1; set_format(a, "decimals as stored"); break;
    case ACT_FMT_DEC_2: a->fmt_decimals = 2; set_format(a, "2 decimals"); break;
    case ACT_FMT_DEC_4: a->fmt_decimals = 4; set_format(a, "4 decimals"); break;
    case ACT_FMT_DATE_ISO: a->fmt_date = 0; set_format(a, "dates as stored (ISO)"); break;
    case ACT_FMT_DATE_DMY: a->fmt_date = 1; set_format(a, "dates DD/MM/YYYY"); break;
    case ACT_FMT_DATE_MDY: a->fmt_date = 2; set_format(a, "dates MM/DD/YYYY"); break;
    case ACT_FMT_TZ_STORED: a->fmt_tz = 0; set_format(a, "time zone as stored"); break;
    case ACT_FMT_TZ_UTC: a->fmt_tz = 1; set_format(a, "timestamps with an offset shown in UTC"); break;
    case ACT_FMT_TZ_LOCAL: a->fmt_tz = 2; set_format(a, "timestamps with an offset shown in local time"); break;
    case ACT_RUN_ALL: {
        int len;
        char *sql = editor_text(a->editor, &len);
        complete_learn_statement(a->completer, sql, len);
        app_run_async(a, sql, len, RUN_EDITOR, 0);
        free(sql);
        break;
    }
    case ACT_REFRESH: explorer_refresh(a->explorer, a); app_status(a, "Explorer refreshed"); break;
    case ACT_CANCEL: output_cancel(a->output, a); break;
    case ACT_SHOW_OUTPUT: results_show_output(a->results, true); a->results_visible = true; a->focus = FOCUS_RESULTS; output_mark_seen(a->output, a); break;
    case ACT_CONTINUE_ON_ERROR: a->continue_on_error = !a->continue_on_error; app_status(a, a->continue_on_error ? "Scripts: continue on error (every statement runs; failures are listed in the Output)" : "Scripts: stop on error (statements after a failure are skipped)"); break;
    case ACT_FETCH_100: a->fetch_size = 100; app_status(a, "Fetch size: 100 rows"); break;
    case ACT_FETCH_500: a->fetch_size = 500; app_status(a, "Fetch size: 500 rows"); break;
    case ACT_FETCH_1000: a->fetch_size = 1000; app_status(a, "Fetch size: 1,000 rows"); break;
    case ACT_FETCH_5000: a->fetch_size = 5000; app_status(a, "Fetch size: 5,000 rows"); break;
    case ACT_FETCH_MORE: if (!results_fetch_more(a->results, a, a->fetch_size)) app_status(a, "No more rows to fetch"); break;
    case ACT_FETCH_ALL: if (!results_fetch_all(a->results, a)) app_status(a, "No more rows to fetch"); break;
    case ACT_TOGGLE_SIDEBAR: a->sidebar_visible = !a->sidebar_visible; break;
    case ACT_TOGGLE_RESULTS: a->results_visible = !a->results_visible; break;
    case ACT_ZOOM_IN: set_zoom(a, a->zoom + 0.1f); break;
    case ACT_ZOOM_OUT: set_zoom(a, a->zoom - 0.1f); break;
    case ACT_ZOOM_RESET: set_zoom(a, 1.0f); break;
    case ACT_CHECK_UPDATES: update_check_now(a); break;
    case ACT_ABOUT: about_open(a); break;   /* the same instance from every entry point */
    default: break;
    }
}

/* Map this frame's unused key presses to an ACT_* command (consuming the key), or ACT_NONE.
 * Panels shortcuts come first; editing shortcuts (undo, select all, clipboard) are skipped
 * while a text field owns the keyboard. */
static int global_shortcut(App *a) {
    Input *in = &a->in;
    if (a->panels && panels_modal_open(a->panels)) return ACT_NONE;   /* the palette / a prompt owns the keys */
    if (a->panels) { int pa = panels_shortcut(a->panels, a); if (pa != ACT_NONE) return pa; }
    /* a focused text field (search bars, prompts, the grid's editors) owns the editing keys */
    bool grid_text = (a->focus == FOCUS_RESULTS && results_text_focus(a->results)) || ui_text_editing();
    /* toolbar spec: focus the toolbar (macOS Ctrl+F5), Switch Tab, Quick Open, Explain */
    if (input_key(in, SDLK_F5, KM_CTRL) || input_key(in, SDLK_F5, KM_PRIMARY)) return ACT_TOOLBAR_FOCUS;
    if (input_key(in, SDLK_TAB, KM_CTRL) || input_key(in, SDLK_TAB, KM_PRIMARY)) return ACT_SWITCH_TAB;
    if (input_key(in, SDLK_P, KM_PRIMARY)) return ACT_QUICK_OPEN;
    if (input_key(in, SDLK_E, KM_PRIMARY | KM_ALT)) return ACT_EXPLAIN;
    if (input_key(in, SDLK_RETURN, KM_PRIMARY) || input_key(in, SDLK_KP_ENTER, KM_PRIMARY)) return ACT_RUN;
    if (input_key(in, SDLK_RETURN, KM_PRIMARY | KM_SHIFT)) return ACT_RUN_ALL;
    if (input_key(in, SDLK_RETURN, KM_PRIMARY | KM_ALT)) return ACT_RUN_NEW_TAB;
    if (input_key(in, SDLK_Y, KM_PRIMARY)) return ACT_HISTORY;
    if (input_key(in, SDLK_RIGHTBRACKET, KM_PRIMARY | KM_ALT)) return ACT_NEXT_TAB;
    if (input_key(in, SDLK_LEFTBRACKET, KM_PRIMARY | KM_ALT)) return ACT_PREV_TAB;
    if (input_key(in, SDLK_W, KM_PRIMARY)) return ACT_CLOSE_TAB;
    if (input_key(in, SDLK_PERIOD, KM_PRIMARY)) return ACT_CANCEL;
    if (input_key(in, SDLK_O, KM_PRIMARY | KM_SHIFT)) return ACT_SHOW_OUTPUT;
    if (input_key(in, SDLK_C, KM_PRIMARY | KM_SHIFT)) return ACT_COLUMNS;
    if (input_key(in, SDLK_O, KM_PRIMARY)) return ACT_OPEN_DB;
    if (input_key(in, SDLK_N, KM_PRIMARY | KM_SHIFT)) return ACT_NEW_CONN;
    if (input_key(in, SDLK_E, KM_PRIMARY)) return ACT_EDIT_CONN;
    if (input_key(in, SDLK_D, KM_PRIMARY)) return ACT_TOGGLE_CONN;
    if (input_key(in, SDLK_N, KM_PRIMARY)) return ACT_NEW_QUERY;
    if (input_key(in, SDLK_Q, KM_PRIMARY)) return ACT_QUIT;
    if (input_key(in, SDLK_B, KM_PRIMARY)) return ACT_TOGGLE_SIDEBAR;
    if (input_key(in, SDLK_J, KM_PRIMARY)) return ACT_TOGGLE_RESULTS;
    if (input_key(in, SDLK_R, KM_PRIMARY)) return ACT_REFRESH;
    if (input_key(in, SDLK_M, KM_PRIMARY | KM_SHIFT)) return ACT_FETCH_MORE;
    if (input_key(in, SDLK_F, KM_PRIMARY)) return ACT_FIND;
    if (input_key(in, SDLK_E, KM_PRIMARY | KM_SHIFT)) return ACT_EXPORT;
    if (input_key(in, SDLK_EQUALS, KM_PRIMARY) || input_key(in, SDLK_PLUS, KM_PRIMARY | KM_SHIFT)) return ACT_ZOOM_IN;
    if (input_key(in, SDLK_MINUS, KM_PRIMARY)) return ACT_ZOOM_OUT;
    if (input_key(in, SDLK_0, KM_PRIMARY)) return ACT_ZOOM_RESET;
    if (grid_text) return ACT_NONE;   /* the field undoes, selects and uses the clipboard itself */
    if (input_key(in, SDLK_Z, KM_PRIMARY | KM_SHIFT)) return ACT_REDO;
    if (input_key(in, SDLK_Z, KM_PRIMARY)) return ACT_UNDO;
    if (input_key(in, SDLK_A, KM_PRIMARY)) return ACT_SELECT_ALL;
    if (input_key(in, SDLK_X, KM_PRIMARY)) return ACT_CUT;
    if (input_key(in, SDLK_C, KM_PRIMARY)) return ACT_COPY;
    if (input_key(in, SDLK_V, KM_PRIMARY)) return ACT_PASTE;
    return ACT_NONE;
}

/* ---- status bar -------------------------------------------------------- */

/* Status bar: connection dot and name, the transient status message (or the shell's status),
 * and on the right caret position, process memory (refreshed once a second) and zoom. */
static void draw_statusbar(App *a, Rect bar) {
    SDL_Renderer *r = a->renderer;
    draw_rect(r, bar, color_hex(T_PANEL_ALT));
    draw_rect(r, rect_make(bar.x, bar.y, bar.w, px(a, 1)), color_hex(T_BORDER));

    float x = bar.x + px(a, 14);
    float dot = px(a, 7);
    draw_rrect(r, rect_make(x, bar.y + (bar.h - dot) / 2, dot, dot), dot / 2, color_hex(a->db ? T_GREEN : T_TEXT_DIM));
    x += dot + px(a, 8);
    char connbuf[160];
    bool rpt_front = a->panels && panels_active_report(a->panels, a), td_front = a->panels && panels_active_designer(a->panels, a);
    if (rpt_front) snprintf(connbuf, sizeof connbuf, "Report  \xC2\xB7  each card runs on its own data source");   /* not the query tabs' connection */
    else if (td_front) snprintf(connbuf, sizeof connbuf, "Table Designer");
    else if (a->active) {
        const DriverInfo *di = conn_driver(a->active);
        snprintf(connbuf, sizeof connbuf, "%s  \xC2\xB7  %s", a->active->name, di ? di->name : a->active->driver);
    } else snprintf(connbuf, sizeof connbuf, "Not connected");
    const char *conn = connbuf;
    float cw = font_width(a->font_small, conn, -1);
    draw_text_fit(a->font_small, r, conn, -1, rect_make(x, bar.y, cw + px(a, 4), bar.h), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    x += cw + px(a, 18);

    const char *ps = a->panels ? panels_status(a->panels) : "";
    if (a->status[0] && SDL_GetTicks() < a->status_until)
        draw_text_fit(a->font_small, r, a->status, -1, rect_make(x, bar.y, bar.w * 0.5f - x, bar.h), color_hex(T_TEXT_DIM), ALIGN_LEFT);
    else if (ps[0]) draw_text_fit(a->font_small, r, ps, -1, rect_make(x, bar.y, bar.w * 0.5f - x, bar.h), color_hex(T_ACCENT), ALIGN_LEFT);

    /* right side: cursor, memory, zoom */
    static size_t rss = 0;
    static Uint64 rss_at = 0;
    if (SDL_GetTicks() - rss_at > 1000) { rss = sys_rss_bytes(); rss_at = SDL_GetTicks(); }
    int line, col;
    editor_cursor_pos(a->editor, &line, &col);
    char gs[300]; gitview_statusbar(a, gs, sizeof gs);   /* Git: branch ↑ahead ↓behind */
    if (gs[0]) { float gw = font_width(a->font_small, gs, -1); draw_text_fit(a->font_small, r, gs, -1, rect_make(bar.x + bar.w / 2 - gw - px(a, 260), bar.y, gw + px(a, 4), bar.h), color_hex(T_TEXT_MUTED), ALIGN_LEFT); }
    char right[160];
    if (rpt_front || td_front) snprintf(right, sizeof right, "Mem %.0f MB", rss / (1024.0 * 1024.0));   /* no editor cursor there */
    else if (fabsf(a->zoom - 1.0f) > 0.01f)
        snprintf(right, sizeof right, "Ln %d, Col %d    Mem %.0f MB    Zoom %d%%", line, col, rss / (1024.0 * 1024.0), (int)roundf(a->zoom * 100));
    else
        snprintf(right, sizeof right, "Ln %d, Col %d    Mem %.0f MB", line, col, rss / (1024.0 * 1024.0));
    draw_text_fit(a->font_small, r, right, -1, rect_make(bar.x + bar.w / 2, bar.y, bar.w / 2 - px(a, 14), bar.h), color_hex(T_TEXT_MUTED), ALIGN_RIGHT);
}

/* ---- frame ------------------------------------------------------------- */

/* Build and present one frame: input handling and drawing happen together (immediate mode).
 * Order matters: dialog and menu bar first (they may claim input), then shortcuts, the panels
 * shell, the status bar, then overlays from bottom to top, and finally the optional harness
 * screenshot, present, and the floating panel windows. */
static void frame(App *a) {
    ui_new_frame();   /* harness targets are re-recorded every frame */
    SDL_Renderer *r = a->renderer;
    a->animating = false;
    a->wake_at = 0;
    a->want_cursor = CURSOR_ARROW;

    /* clear and lay out: menu bar on top, status bar at the bottom, workspace in between */
    Color bg = color_hex(T_BG);
    SDL_SetRenderDrawColor(r, bg.r, bg.g, bg.b, 255);
    SDL_RenderClear(r);

    float W = (float)a->width, H = (float)a->height;
    Rect bar = rect_make(0, 0, W, px(a, M_MENUBAR));
    Rect status = rect_make(0, H - px(a, M_STATUSBAR), W, px(a, M_STATUSBAR));
    /* the main toolbar has a row of its own under the menu bar; hidden, it takes no space (TBR-001 / 006) */
    float tbh = toolbar_height(a->toolbar, a);
    Rect tbar = rect_make(0, bar.h, W, tbh);
    Rect body = rect_make(0, bar.h + tbh, W, H - bar.h - tbh - status.h);

    /* input that wins over the workspace: a text field's right-click menu, modal dialog, menu
     * bar, async Output events, shortcuts */
    ui_textfield_menu_update(a);
    dialog_update(a->dialog, a);
    about_pre_update(a);       /* the About, when open, owns the frame's input */
    settings_pre_update(a);    /* Settings (Cmd+,), when open, too */
    objed_pre_update(a);       /* object editor: jobs, its menus and dialogs take the input first */
    tdes_pre_update(a);        /* table designer: jobs, its review / close dialogs */
    planview_pre_update(a);    /* execution plans: capture and AI jobs, node menu */
    rptview_pre_update(a);     /* Report Studio: queries, AI, autosave, menus */
    dgmview_pre_update(a);     /* ER diagrams: catalog reads, layout workers, autosave, shortcuts */
    gitview_pre_update(a);     /* Source Control: Git jobs, watcher, menus */
    app_poll_connections(a);   /* background connects that finished */
    int action = menu_update(a->menu, a, bar);
    output_pump(a->output, a);
    objcat_pump(a);
    /* a toolbar dialog / dropdown / keyboard focus holds the keys: no global shortcut fires under it */
    if (action == ACT_NONE && !toolbar_modal_open(a->toolbar)) action = global_shortcut(a);
    if (action != ACT_NONE) do_action(a, action);
    toolbar_frame(a->toolbar, a, tbar);   /* before the panels: its dialogs and dropdowns take the input first */

    /* the workspace: panel bar, docked panels, editor groups (panels.c over the dock engine) */
    panels_frame(a->panels, a, body);
    { int pa = panels_take_action(a->panels); if (pa != ACT_NONE) do_action(a, pa); }
    /* status bar, then popups and overlays above every panel (dialog last, on top of all) */
    draw_statusbar(a, status);
    editor_draw_overlay(a->editor, a);
    explorer_draw_overlay(a->explorer, a);
    if (!panels_is_floating(a->panels, "lume.data.results")) results_draw_overlay(a->results, a);   /* else in its own window */
    if (panels_is_visible(a->panels, "lume.data.output")) output_draw_overlay(a->output, a);
    panels_draw_overlay(a->panels, a);
    toolbar_draw_overlay(a->toolbar, a);   /* dropdowns, datasource popover, tooltips, dialogs */
    menu_draw_overlay(a->menu, a);
    objed_draw_overlay(a);     /* review / diff / conflict dialogs and the object menus */
    tdes_draw_overlay(a);      /* table designer review / close dialogs, its menus */
    planview_draw_overlay(a);  /* execution plan node menu */
    rptview_draw_overlay(a);   /* Report Studio menus */
    dgmview_draw_overlay(a);   /* ER diagram menus */
    gitview_draw_overlay(a);   /* Source Control menus */
    about_draw(a);
    settings_draw(a);
    app_prefs_track(a);
    dialog_draw(a->dialog, a);
    ui_textfield_menu_draw(a);   /* above the dialog: it can belong to one of its fields */

    /* mouse cursor requested by whatever is under the pointer this frame */
    if (a->want_cursor != a->cur_cursor) {
        SDL_SetCursor(a->cursors[a->want_cursor]);
        a->cur_cursor = a->want_cursor;
    }
    /* harness / LUME_SCREENSHOT: save the back buffer before presenting it */
    if (a->shot_path) {
        SDL_Surface *s = SDL_RenderReadPixels(r, NULL);
        if (s) { SDL_SaveBMP(s, a->shot_path); SDL_DestroySurface(s); }
        a->shot_path = NULL;
    }
    Uint64 p0 = SDL_GetPerformanceCounter();
    SDL_RenderPresent(r);   /* blocks until a vblank when the previous frame is still queued */
    a->last_present_ms = (SDL_GetPerformanceCounter() - p0) * 1000.0 / SDL_GetPerformanceFrequency();
    panels_render_floats(a->panels, a);   /* floating / separate panel windows */
}

/* ---- main -------------------------------------------------------------- */

/* Locate the assets directory: next to the executable (installed / bundled) or, failing that,
 * the source tree's assets folder compiled in as LUME_SOURCE_ASSETS. */
static void find_assets(App *a) {
    const char *base = SDL_GetBasePath();
    char probe[1200];
    if (base) {
        snprintf(a->assets_dir, sizeof a->assets_dir, "%sassets", base);
        snprintf(probe, sizeof probe, "%s/fonts/Inter-Variable.ttf", a->assets_dir);
        if (file_exists(probe)) return;
    }
    snprintf(a->assets_dir, sizeof a->assets_dir, "%s", LUME_SOURCE_ASSETS);
}

/* Start-up, event loop and shutdown. argv[1], if given, is a SQLite file to open. Environment:
 * LUME_SCRIPT (test harness), LUME_SCREENSHOT / LUME_EXIT_AFTER_SHOT (visual checks),
 * LUME_REDUCE_MOTION, LUME_PREF_DIR (see complete.c). */
/* check marks of the settings in the menus: the current state, read when the menu is drawn */
static App *g_menu_app;
static bool menu_checked(int act) {
    App *a = g_menu_app; if (!a) return false;
    switch (act) {
    case ACT_FETCH_100: return a->fetch_size == 100;
    case ACT_FETCH_500: return a->fetch_size == 500;
    case ACT_FETCH_1000: return a->fetch_size == 1000;
    case ACT_FETCH_5000: return a->fetch_size == 5000;
    case ACT_BUDGET_64: return a->results_budget_mb == 64;
    case ACT_BUDGET_256: return a->results_budget_mb == 256;
    case ACT_BUDGET_1024: return a->results_budget_mb == 1024;
    case ACT_FMT_GROUP: return a->fmt_group;
    case ACT_FMT_DEC_RAW: return a->fmt_decimals == -1;
    case ACT_FMT_DEC_2: return a->fmt_decimals == 2;
    case ACT_FMT_DEC_4: return a->fmt_decimals == 4;
    case ACT_FMT_DATE_ISO: return a->fmt_date == 0;
    case ACT_FMT_DATE_DMY: return a->fmt_date == 1;
    case ACT_FMT_DATE_MDY: return a->fmt_date == 2;
    case ACT_FMT_TZ_STORED: return a->fmt_tz == 0;
    case ACT_FMT_TZ_UTC: return a->fmt_tz == 1;
    case ACT_FMT_TZ_LOCAL: return a->fmt_tz == 2;
    case ACT_CONTINUE_ON_ERROR: return a->continue_on_error;
    case ACT_REDUCE_MOTION: return a->reduce_motion;
    case ACT_COMPACT: return panels_flag(a->panels, 0);
    case ACT_TOP_EDGE: return panels_flag(a->panels, 1);
    case ACT_TOOLBAR_TOGGLE: return toolbar_flag(a->toolbar, 0);
    case ACT_TOOLBAR_COMPACT: return toolbar_flag(a->toolbar, 1);
    case ACT_TOOLBAR_LABELS: return toolbar_flag(a->toolbar, 2);
    case ACT_TX_AUTO: return a->active && a->active->db && !a->active->manual_tx;
    case ACT_TX_MANUAL: return a->active && a->active->db && a->active->manual_tx;
    default: return false;
    }
}

int main(int argc, char **argv) {
    Uint64 boot_ticks = SDL_GetTicks();   /* for the anonymous startup-time bucket (telemetry.h) */
    SDL_SetAppMetadata("Lume", LUME_VERSION, "dev.lume");
    log_init(LUME_VERSION);                      /* first: a crash during startup is logged too */
    /* automated tests (LUME_SCRIPT) run in the background: no Dock icon, never activated, and the
     * window is never shown (macOS orders a shown window to the front even when it is not
     * activated). It still renders, so screenshots and the harness work. LUME_FOREGROUND=1 to watch. */
    bool test_bg = SDL_getenv("LUME_SCRIPT") && !SDL_getenv("LUME_FOREGROUND");
    if (test_bg) {
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) { LOG_E("SDL_Init failed: %s", SDL_GetError()); return 1; }

    /* defaults */
    App app;
    memset(&app, 0, sizeof app);
    App *a = &app;
    a->running = true;
    a->zoom = 1.0f;
    a->fetch_size = 500;
    a->mod_override = -1;
    a->results_budget_mb = 256;
    a->query_timeout_s = 0; a->connect_timeout_s = 10; a->ssh_timeout_s = 20; a->explorer_timeout_s = 5;   /* timeouts (Settings) */
    a->explorer_list_min = 10; a->report_timeout_s = 30; a->git_timeout_min = 2; a->git_clone_min = 30;
    a->catalog_session = true;
    a->fmt_decimals = -1;
    a->sidebar_w = 250;
    a->results_h = 260;
    a->sidebar_visible = a->results_visible = true;
    a->focus = FOCUS_EDITOR;
    find_assets(a);

    /* window (hidden until the first frame is drawn), vsync renderer, metrics and fonts */
    a->window = SDL_CreateWindow("Lume", 1280, 820, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!a->window) { LOG_E("CreateWindow failed: %s", SDL_GetError()); return 1; }
    SDL_SetWindowMinimumSize(a->window, 720, 480);
    a->renderer = SDL_CreateRenderer(a->window, NULL);
    if (!a->renderer) { LOG_E("CreateRenderer failed: %s", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(a->renderer, 1);
    update_metrics(a);
    brand_set_window_icon(a);   /* the Lume Data symbol in the Dock / title bar */
    about_init(a);
    settings_init(a);
    objcat_init(a);

    /* system cursors and the user events used by the native file dialogs */
    a->cursors[CURSOR_ARROW] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    a->cursors[CURSOR_IBEAM] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_TEXT);
    a->cursors[CURSOR_EW] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_EW_RESIZE);
    a->cursors[CURSOR_NS] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_NS_RESIZE);
    a->cursors[CURSOR_HAND] = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);
    a->ev_open_path = SDL_RegisterEvents(1);
    a->ev_save_path = SDL_RegisterEvents(1);

    /* modules; the panels shell comes last because it registers the others as panels */
    a->completer = complete_create();
    a->conns = conns_load();
    sshtun_init(a->conns ? a->conns->dir : "");   /* stops tunnels a killed earlier run left behind */
    a->update_check = true;   /* on unless turned off (Settings > Privacy) */
    app_prefs_load(a); app_apply_timeouts(a); g_prefs_sig = prefs_sig(a); g_prefs_loaded = true;   /* the settings saved last time */
    a->dialog = dialog_create();
    a->explorer = explorer_create();
    a->editor = editor_create();
    a->results = results_create();
    a->output = output_create();
    a->menu = menu_create();
    /* extension modules (PNL-047): registered before the shell reads the layout file */
    panels_add_module("lume.history", history_module_init);   /* Query History (TBR-034) */
    panels_add_module("lume.markdown", markdown_module_init);
    objed_init(a);                      /* object source editor: commands (before the toolbar / palette read them) */
    tdes_init(a);                       /* visual table designer */
    planview_init(a);                   /* execution plan panel + Lume Intelligence (registers its panel module) */
    rptview_init(a);                    /* Report Studio + assistant panel (LI-*) */
    dgmview_init(a);                    /* ER diagrams (docs/diagrams.md) */
    {
        g_app_for_cmd = a;
        Command c; memset(&c, 0, sizeof c);
        c.group = "Datasource"; c.icon = "database"; c.invoke = cmd_shop_demo; c.module = "lume.core";
        c.id = "demo.shop"; c.label = "Open the E-commerce Demo Database"; c.synonyms = "sample shop store example orders products";
        commands_register(&c);
    }
    gitview_init(a);                    /* Source Control (GIT-*) */
    a->panels = panels_create(a);       /* registers the panels, restores the layout */
    a->toolbar = toolbar_create(a);     /* the command registry, the workspace and its toolbar settings */
    commands_set_dispatch(do_action);
    menu_set_enable_hook(commands_act_enabled);
    g_menu_app = a; menu_set_check_hook(menu_checked);
    /* PNL-039: follow the system "Reduce motion" setting (macOS), or LUME_REDUCE_MOTION=1 */
    a->reduce_motion = SDL_getenv("LUME_REDUCE_MOTION") && SDL_getenv("LUME_REDUCE_MOTION")[0] == '1';
#ifdef __APPLE__
    if (!a->reduce_motion) {
        FILE *rm = popen("defaults read com.apple.universalaccess reduceMotion 2>/dev/null", "r");
        if (rm) { char b[8] = ""; if (fgets(b, sizeof b, rm) && b[0] == '1') a->reduce_motion = true; pclose(rm); }
    }
#endif
    if (a->reduce_motion) LOG_I("reduce motion is on: no smooth scrolling");

    /* initial database: a file from the command line, else the demo with the welcome query */
    bool first_run = a->conns->n == 0;
    Connection *demo = ensure_demo(a);
    if (!SDL_getenv("LUME_SCRIPT")) ensure_shop_demo(a);   /* tests add it on purpose: their connection lists stay as they are */
    explorer_rebuild(a->explorer, a);
    if (argc > 1) {
        open_sqlite_path(a, argv[1]);
    } else if (app_connect(a, demo) && first_run) {
        editor_set_text(a->editor, welcome_sql);
        run_statement(a, RUN_EDITOR);
    } else if (a->active == demo) {
        editor_set_text(a->editor, welcome_sql);
        run_statement(a, RUN_EDITOR);
    }
    SDL_StartTextInput(a->window);
    if (SDL_getenv("LUME_SCRIPT")) script_load(a, SDL_getenv("LUME_SCRIPT"));

    /* LUME_SCREENSHOT=<file.bmp> saves the window after a few frames (used for visual checks). */
    const char *shot = SDL_getenv("LUME_SCREENSHOT");
    int frames = 0;

    bool shown = false;
    /* event loop: sleep until an event, a requested wake-up, or (while animating) not at all */
    telemetry_start(a->conns ? a->conns->dir : "", a->telemetry, (double)(SDL_GetTicks() - boot_ticks));   /* only with consent */
    update_init(a, a->conns ? a->conns->dir : "");   /* the first check runs from update_tick, off the main thread */
    crashrep_startup(a);   /* the previous run crashed: offer / send / drop its report, per the user's choice */
    while (a->running) {
        input_begin_frame(&a->in);
        if (a->panels) panels_input_begin(a->panels);
        SDL_Event e;
        Sint32 timeout = 1000;
        if (a->animating) timeout = 0;
        else if (a->wake_at) {
            Uint64 now = SDL_GetTicks();
            timeout = a->wake_at > now ? (Sint32)(a->wake_at - now) : 0;
        }
        if (SDL_WaitEventTimeout(&e, timeout)) {
            handle_event(a, &e);
            while (SDL_PollEvent(&e)) handle_event(a, &e);
        }
        /* draw, then background chores: completer usage save, harness step, first show */
        { Uint64 f0 = SDL_GetPerformanceCounter(); frame(a); a->last_frame_ms = (SDL_GetPerformanceCounter() - f0) * 1000.0 / SDL_GetPerformanceFrequency(); telemetry_frame(a->last_frame_ms); }
        if (complete_tick(a->completer)) app_wake_in(a, 2100);
        update_tick(a);
        script_tick(a);
        if (!shown) { if (!test_bg) SDL_ShowWindow(a->window); shown = true; }
        if (shot && ++frames == 5) {
            a->shot_path = shot;   /* captured inside frame(), before present */
        } else if (shot && frames == 6 && SDL_getenv("LUME_EXIT_AFTER_SHOT")) {
            a->running = false;
        }
    }

    /* shutdown: order matters where noted (cursors before connections, panels before results) */
    explorer_destroy(a->explorer);
    dialog_destroy(a->dialog);
    complete_destroy(a->completer);
    toolbar_destroy(a->toolbar, a);     /* saves pending toolbar settings */
    a->toolbar = NULL;
    objed_shutdown(a);                  /* keeps unsaved object edits as drafts */
    tdes_shutdown(a);                   /* keeps unsaved table drafts */
    planview_shutdown(a);
    rptview_shutdown(a);                /* autosaves unsaved reports */
    dgmview_shutdown(a);                /* saves diagram layouts */
    gitview_shutdown(a);
    brand_free();
    panels_destroy(a->panels, a);       /* saves the layout, frees every query tab */
    a->panels = NULL;
    output_destroy(a->output);          /* a running job is cancelled and joined first */
    results_destroy(a->results);        /* open cursors must go before their connections */
    telemetry_stop(sys_rss_bytes());   /* the session's summary, sent next launch (only with consent) */
    conns_free(a->conns);
    menu_destroy(a->menu);
    font_destroy(a->font_ui);
    font_destroy(a->font_small);
    font_destroy(a->font_mono);
    SDL_DestroyRenderer(a->renderer);
    SDL_DestroyWindow(a->window);
    SDL_Quit();
    log_shutdown();
    return 0;
}
