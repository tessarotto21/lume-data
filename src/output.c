/* SPDX-License-Identifier: AGPL-3.0-only */
/* output.c - the Execution Output panel (docs/execution-output.md, OUT-002 .. OUT-028).
 *
 * Two jobs live here:
 *   1. Execution driver: output_run() starts an ExecJob (exec.c) on a worker thread and
 *      output_pump(), called by the main loop every frame, turns each finished statement into
 *      OutLog events (outlog.c), hands result sets to the Results tabs (results.c), records
 *      transaction changes and appends the run to the cross-session history file.
 *      output_record_apply() logs table-editor transactions (edit.c) the same way.
 *   2. View: output_update() draws the toolbar (search, level checkboxes, execution filter,
 *      wrap / follow, copy / export / clear), a running banner, a virtualised list grouped by
 *      execution (header row + event rows, optional expanded details), and a footer with
 *      counts and the Pause view button. results.c calls it in place of the grid when the
 *      Output tab is selected.
 *
 * Data: one Session (connection id + OutLog) per connection, up to OUT_MAX_SESSIONS, the
 * oldest recycled. The visible list is a flat Row array rebuilt from the active log only when
 * something that affects it changed (rebuild() keeps a signature), with prefix-summed row tops
 * so scrolling and hit-testing stay O(log n) with wrap and expanded rows.
 *
 * Persistence: <prefs>/output-prefs.txt (view preferences) and <prefs>/history.tsv
 * (execution history, OUT-025), both plain text next to the connection list.
 *
 * Threading: everything here runs on the UI thread. The only shared state is the ExecJob,
 * read through exec.c accessors and __atomic loads of statement status / current index;
 * results are taken from the job with exec_take_result() once a statement is done. */
#include "units.h"
#include "output.h"
#include "telemetry.h"
#include "panels.h"
#include "commands.h"
#include "conn.h"
#include "draw.h"
#include "editor.h"
#include "explorer.h"
#include "edit.h"
#include "font.h"
#include "results.h"
#include "theme.h"
#include "ui.h"
#include <ctype.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include "log.h"

/* The Output's own status messages restate events it already records: status bar only. */
#define app_status app_status_bar

/* Most connections that keep their own log at once; the oldest is recycled past this. */
#define OUT_MAX_SESSIONS 16
/* Layout constants in points (scaled with px()): list row, toolbar, footer, running banner. */
#define ROW_H 22.0f
#define TOOLBAR_H 34.0f
#define FOOTER_H 24.0f
#define BANNER_H 30.0f

/* One connection's log: id is the Connection id ("none" without one). */
typedef struct { char id[16]; OutLog log; } Session;
/* One visible list row: kind 0 = execution header, 1 = event. exec is the execution id, ev the
 * event index in OutLog.ev (for a header, the first event of the group), hidden the number of
 * events of the group left out by filters (headers only). */
typedef struct { uint8_t kind; int exec, ev, hidden; } Row;
static void prefs_reset(Output *o);           /* defined with the preferences, used by the context menu */
static void prefs_load(Output *o, App *a);    /* also from output_pump: an error can arrive before the panel was ever drawn */
static void history_append(Output *o, App *a, const char *session, int status, double ms, int nstmts, const char *sql, int len);   /* OUT-025 */
static bool sql_line(const OutEvent *e);      /* kind 0 = execution header, 1 = event; hidden = events of the group left out by filters */

/* Context-menu / Options-menu item ids (popup ids, handled by menu_run). */
enum { OM_COPY = 1, OM_COPY_DETAILS, OM_GOTO_SQL, OM_OPEN_RESULTS, OM_ONLY_EXEC, OM_ONLY_ERRORS, OM_ALL, OM_EXPORT, OM_CLEAR, OM_COLLAPSE_ALL, OM_EXPAND_ALL,
       OM_AUTO_OPEN, OM_RESET_LAYOUT,
       /* compact toolbar (narrow panel): the controls that no longer fit, as menu items */
       OM_LV_INFO, OM_LV_SUCCESS, OM_LV_WARNING, OM_LV_ERROR, OM_EXEC_TOGGLE, OM_WRAP, OM_FOLLOW, OM_TEXT };

/* One running execution. j: the worker job (owns the statements and their results until
 * taken); sid: session (connection id) it logs to; tx_before: driver tx state before it ran
 * (-1 unknown); rows_landed: a result already went to a tab (later ones open new tabs);
 * touched_schema: some statement was DML / DDL (explorer refresh at the end). */
typedef struct OutJob {
    ExecJob *j; char sid[16]; char conn_name[64]; int tx_before; bool rows_landed, touched_schema;
    char title[64];              /* title of its result tabs ("Results", "Plan \xC2\xB7 ...") */
    char *display;               /* SQL shown in the Output and history instead of the text run (parameters), or NULL */
    SqlStmt dparts[64]; int ndparts;   /* statements of display, matched by index */
    char *export_path;           /* Execute to File: the first result is written there, or NULL */
    const void *editor;          /* the query tab's editor that started it (compared, never dereferenced) */
    char *qtable; bool no_history; int replace_exec;   /* OutRunOpts */
    void (*landed)(App *a, int tab_exec_id, const char *err, void *ud); void *ud; int landed_tab; char landed_err[300];
    int timed_cur; double timed_t0; bool timed_out;   /* query timeout: statement watched, when it was first seen running (monotonic s), cancelled by it */
} OutJob;
#define OUT_MAX_JOBS 8

/* Panel state. Owned by the App; created by output_create, freed by output_destroy. */
struct Output {
    Session sess[OUT_MAX_SESSIONS]; int nsess;   /* per-connection logs, oldest first */
    /* running executions: one per session (connection), so two tabs on different databases run
     * in parallel and Stop targets one of them (TBR-020 / 021) */
    OutJob jobs[OUT_MAX_JOBS]; int njobs;
    /* view state (OUT-002 / OUT-020 / OUT-023) */
    TextField search; bool search_focus;   /* toolbar search text and whether it has focus */
    unsigned levels;             /* bit mask of OL_* shown (mock-up: Info / Success / Warning / Error checkboxes) */
    int exec_filter;             /* 0 all */
    bool wrap, follow, paused; int pending_while_paused;
    /* text view: the same events as plain monospace lines (as the Messages tab of other tools) */
    bool text_mode; char *tv_text; int *tv_off; int tv_n; unsigned long long tv_sig; float tv_scroll;
    int tv_anchor, tv_caret; bool tv_drag;   /* a mouse selection in the text view (byte offsets); anchor == caret: none */
    ScrollBar sb, tv_sb;   /* overlay scrollbars: the list and the text view */   /* paused: follow was stopped by scrolling up */
    float scroll;                /* list scroll offset in pixels */
    int sel; uint64_t expanded_id;   /* selected row (-1 none); id of the event showing details (0 none) */
    int anchor;                  /* other end of a range selection (OUT-021); == sel for one row */
    /* Pause view (OUT-023): the list stops at the last event seen; capture goes on */
    bool frozen; uint64_t frozen_id;
    /* preferences (OUT-005 / OUT-014), stored in <prefs>/output-prefs.txt */
    bool auto_open_errors;       /* a statement error that prevents a result opens the Output */
    bool prefs_loaded; unsigned prefs_sig;
    int history_max;             /* OUT-025 retention (entries kept in history.tsv) */
    long long started_at;        /* this app session's start: older history entries are "earlier sessions" */
    PopupMenu menu; int menu_row;   /* context / Options menu and the row it was opened on */
    char menu_lbl[2][64];        /* "Copy N messages" / "Copy details of N rows" while a range is selected */
    /* rows */
    Row *rows; int nrows, cap_rows;   /* visible list, rebuilt by rebuild() */
    float *tops; int cache_n, cache_nex; float cache_w; unsigned cache_sig; const OutLog *cache_log; bool dirty;
    /* tops: y of each row (nrows + 1 entries, last = total height); cache_*: what the rows were
     * built from; dirty: force a rebuild on the next frame */
    Rect list_rc;                /* list area of the last frame (for reveal) */
    char last_search[128];       /* search text of the last frame, to detect edits */
    int goto_exec;               /* execution to reveal on the next frame */
};


/* Allocates the panel with first-run defaults: follow on, every level shown, history 500.
 * The caller owns the result (output_destroy). */
Output *output_create(void) { Output *o = calloc(1, sizeof *o); o->history_max = 500; o->started_at = (long long)time(NULL); o->follow = true; o->sel = -1; o->anchor = -1; o->auto_open_errors = true; o->dirty = true; o->levels = (1u << OL_COUNT) - 1; return o; }
/* Frees the running job (if any), every session log and the row caches. NULL-safe. */
void output_destroy(Output *o) {
    if (!o) return;
    for (int i = 0; i < o->njobs; i++) { exec_free(o->jobs[i].j); free(o->jobs[i].display); free(o->jobs[i].export_path); }
    for (int i = 0; i < o->nsess; i++) outlog_free(&o->sess[i].log);
    free(o->rows); free(o->tops); free(o->tv_text); free(o->tv_off); free(o);
}

/* Finds the session for connection id, creating it (with display name `name`) if needed.
 * When all slots are used the oldest session is dropped together with its events. */
static Session *session_for(Output *o, const char *id, const char *name) {
    for (int i = 0; i < o->nsess; i++) if (strcmp(o->sess[i].id, id) == 0) return &o->sess[i];
    if (o->nsess == OUT_MAX_SESSIONS) {
        /* recycle the oldest idle session */
        outlog_free(&o->sess[0].log);
        memmove(&o->sess[0], &o->sess[1], sizeof o->sess[0] * (OUT_MAX_SESSIONS - 1));
        o->nsess--;
    }
    Session *s = &o->sess[o->nsess++];
    snprintf(s->id, sizeof s->id, "%s", id);
    outlog_init(&s->log, o->nsess, name);
    return s;
}

/* Id of the active connection, or "none". */
static const char *session_id(App *a) { return a->active ? a->active->id : "none"; }
/* See output.h: the log of the active connection, created on demand. */
OutLog *output_current(Output *o, App *a) { return &session_for(o, session_id(a), a->active ? a->active->name : "no connection")->log; }
/* Log of the session a job belongs to (the user may have switched tab or connection since it
 * started); NULL if that session was recycled. */
static OutLog *job_log(Output *o, const OutJob *oj) { for (int i = 0; i < o->nsess; i++) if (strcmp(o->sess[i].id, oj->sid) == 0) return &o->sess[i].log; return NULL; }
/* The job running on connection db, or NULL. */
static OutJob *job_of_db(Output *o, const Db *db) { for (int i = 0; i < o->njobs; i++) if (db && o->jobs[i].j->db == db) return &o->jobs[i]; return NULL; }
/* The job of execution exec_id, or NULL. */
static OutJob *job_of_exec(Output *o, int exec_id) { for (int i = 0; i < o->njobs; i++) if (o->jobs[i].j->exec_id == exec_id) return &o->jobs[i]; return NULL; }
/* The job to show in the running bar: the active session's, else the first one. */
static OutJob *shown_job(Output *o, App *a) {
    for (int i = 0; i < o->njobs; i++) if (strcmp(o->jobs[i].sid, a->active ? a->active->id : "none") == 0) return &o->jobs[i];
    return o->njobs ? &o->jobs[0] : NULL;
}

/* True while any execution's worker has not finished. */
bool output_job_running(Output *o) { for (int i = 0; i < o->njobs; i++) if (exec_running(o->jobs[i].j)) return true; return false; }
/* See output.h. */
bool output_db_running(Output *o, const Db *db) { OutJob *oj = job_of_db(o, db); return oj && exec_running(oj->j); }
int output_running_count(Output *o) { int n = 0; for (int i = 0; i < o->njobs; i++) if (exec_running(o->jobs[i].j)) n++; return n; }
bool output_running_info(Output *o, int k, OutRunInfo *out) {
    int n = 0;
    for (int i = 0; i < o->njobs; i++) {
        if (!exec_running(o->jobs[i].j)) continue;
        if (n++ != k) continue;
        ExecJob *j = o->jobs[i].j;
        out->db = j->db; out->exec_id = j->exec_id; out->elapsed_ms = exec_elapsed_ms(j); out->cancelling = exec_cancelling(j);
        out->stmt = __atomic_load_n(&j->cur, __ATOMIC_SEQ_CST) + 1; out->nstmts = j->n;
        snprintf(out->conn_name, sizeof out->conn_name, "%s", o->jobs[i].conn_name);
        snprintf(out->conn_id, sizeof out->conn_id, "%s", o->jobs[i].sid);
        out->editor = o->jobs[i].editor;
        return true;
    }
    return false;
}
/* True while the Output context / Options menu is open (it captures input). */
bool output_modal_open(Output *o) { return o->menu.open; }
/* True while the search field has keyboard focus. */
bool output_text_focus(Output *o) { return o->search_focus; }
/* Cmd+F: focuses the search field and restarts the caret blink. */
void output_find(Output *o) { o->search_focus = true; o->search.blink = SDL_GetTicks(); }

/* Warnings + errors in executions not yet marked seen (tab badge); *errors gets errors only. */
/* Warnings and errors of executions (and notes) of l not yet looked at; errors in *errors. */
static int unseen_count(const OutLog *l, int *errors) {
    int n = 0, e = 0;
    for (int i = 0; i < l->nex; i++) if (!l->ex[i].seen) { n += l->ex[i].warnings + l->ex[i].errors; e += l->ex[i].errors; }
    if (errors) *errors = e;
    return n;
}
static int unseen_in(const OutLog *l) { return unseen_count(l, NULL); }
int output_unseen(Output *o, App *a, int *errors) { return unseen_count(output_current(o, a), errors); }
/* Clears the badge: marks every execution of the active log as seen. */
void output_mark_seen(Output *o, App *a) { OutLog *l = output_current(o, a); for (int i = 0; i < l->nex; i++) l->ex[i].seen = true; }
/* Shows the Output tab, clears the execution filter and scrolls to exec_id's header on the
 * next frame (expanding it). */
void output_show_exec(Output *o, App *a, int exec_id) {
    results_show_output(a->results, true);
    a->results_visible = true;
    OutLog *l = output_current(o, a);
    OutExec *x = outlog_exec(l, exec_id);
    if (x) { x->collapsed = false; x->seen = true; }
    o->exec_filter = 0;
    o->goto_exec = exec_id;
    o->dirty = true;
}

/* ---- execution -------------------------------------------------------------- */

/* One-line preview of a statement for messages: comments dropped, whitespace collapsed, cut at
 * 60 bytes with an ellipsis (on a UTF-8 boundary). Same idea as outlog.c's 72-byte summary. */
static void snippet(const char *sql, int len, char *out, size_t n) {
    size_t p = 0; bool space = false; int i = 0;
    while (i < len && p < n - 4) {
        if (sql[i] == '-' && i + 1 < len && sql[i + 1] == '-') { while (i < len && sql[i] != '\n') i++; continue; }
        if (isspace((unsigned char)sql[i])) { space = p > 0; i++; continue; }
        if (space) { out[p++] = ' '; space = false; }
        out[p++] = sql[i++];
        if (p >= 60) break;
    }
    out[p] = 0;
    if (i < len && p >= 60) { while (p > 0 && (out[p - 1] & 0xC0) == 0x80) p--; snprintf(out + p, n - p, "\xE2\x80\xA6"); }
}

/* See output.h. Starts the statements in sql on a worker (one job at a time), opens an
 * execution in the active connection's log and makes the results area visible. mode is a
 * RUN_* value for where the first result goes; editor_rev / sql_base locate the text in the
 * editor for "Go to SQL". Returns false (with a status message) if it could not start. */
bool output_run(Output *o, App *a, Db *db, const char *sql, int len, int mode, uint32_t editor_rev, int sql_base) {
    telemetry_count(TM_QUERY);
    OutRunOpts op = { .mode = mode };
    return output_run_opts(o, a, db, sql, len, editor_rev, sql_base, &op) > 0;
}

/* See output.h. Like output_run, with a result title, a note recorded first in the Output, a
 * display text that replaces the SQL in the log and history (parameter values never appear
 * there) and an optional file the first result is exported to. Returns the execution id. */
int output_run_opts(Output *o, App *a, Db *db, const char *sql, int len, uint32_t editor_rev, int sql_base, const OutRunOpts *op) {
    int mode = op->mode;
    if (!db) { app_status(a, "No connection. Open a database first."); return 0; }
    /* one execution per session: a second Run on the same connection (a double click, a
     * repeated shortcut) is refused, other sessions may run in parallel (TBR-021 / 025) */
    if (job_of_db(o, db)) { app_status(a, "A statement is still running on %s: wait for it or cancel it (\xE2\x8C\x98.)", a->active ? a->active->name : "this connection"); return 0; }
    if (explorer_reading(a, db)) { app_status(a, "The Explorer is still listing tables on this connection: wait for it, or click Cancel under the schema"); return 0; }
    if (o->njobs >= OUT_MAX_JOBS) { app_status(a, "%d executions are running: wait for one to finish", OUT_MAX_JOBS); return 0; }
    if (len < 0) len = (int)strlen(sql);
    /* the job belongs to db's connection, which need not be the active one (a preview, a Re-run or
     * an FK step on another tab): its session, its Output log, its name */
    const Connection *dc = NULL;
    for (int k = 0; a->conns && k < a->conns->n; k++) if (a->conns->items[k]->db == db) dc = a->conns->items[k];
    const char *cid = dc ? dc->id : session_id(a);
    const char *cname = dc ? dc->name : a->active ? a->active->name : "no connection";
    OutLog *l = dc ? &session_for(o, cid, cname)->log : output_current(o, a);
    /* a re-run replaces this tab's results: BEFORE the worker owns the connection, since freeing a
     * result closes its open cursor on this same session (doing it after crashed SQLite, 01/11/2026) */
    if (a->results && op->mode != RUN_NEW_TAB && !op->keep_tabs) results_begin_run(a->results, a, a->editor);
    int tx = db->drv->tx_state ? db->drv->tx_state(db) : -1;      /* before the worker owns the connection */
    ExecJob *j = exec_start(db, sql, len, a->fetch_size, !a->continue_on_error);
    if (!j) { app_status(a, __atomic_load_n(&db->busy, __ATOMIC_SEQ_CST) ? "The connection is busy with another statement" : "Nothing to run"); return 0; }
    OutJob *oj = &o->jobs[o->njobs++];
    memset(oj, 0, sizeof *oj);
    oj->j = j; oj->tx_before = tx;
    snprintf(oj->sid, sizeof oj->sid, "%s", cid);
    snprintf(oj->conn_name, sizeof oj->conn_name, "%s", cname);
    oj->editor = a->editor;
    j->mode = mode; j->editor_rev = editor_rev; j->sql_base = sql_base;
    j->exec_id = outlog_exec_begin(l, op->display ? op->display : sql, op->display ? -1 : len, editor_rev, sql_base);
    OutExec *x = outlog_exec(l, j->exec_id);
    if (x) x->nstmts = j->n;
    if (j->n > 1) app_status(a, "Running %d statements%s  \xC2\xB7  \xE2\x8C\x98. cancels", j->n, a->continue_on_error ? " (continue on error)" : " (stop on error)");
    else app_status(a, "Running\xE2\x80\xA6  \xE2\x8C\x98. cancels");
    a->results_visible = true;
    if (op->title && op->title[0]) snprintf(oj->title, sizeof oj->title, "%s", op->title); else snprintf(oj->title, sizeof oj->title, "Results");
    if (op->display) {
        oj->display = strdup(op->display);
        oj->ndparts = sql_split(oj->display, -1, sql_dialect_for(db->drv), oj->dparts, 64);
        if (oj->ndparts != j->n) oj->ndparts = 0;   /* no reliable match: show the whole display text */
    }
    if (op->export_path) oj->export_path = strdup(op->export_path);
    if (op->qtable && op->qtable[0]) oj->qtable = strdup(op->qtable);
    oj->no_history = op->no_history; oj->replace_exec = op->replace_exec; oj->landed = op->landed; oj->ud = op->ud;
    if (op->note && op->note[0]) { outlog_push(l, j->exec_id, 0, OT_DB_MESSAGE, OL_INFO, op->note); LOG_I("execution %d: %s", j->exec_id, op->note); }   /* what ran and where; never the SQL or values */
    else LOG_I("execution %d on %s: %d statement(s)", j->exec_id, oj->conn_name, j->n);
    o->dirty = true;
    app_wake_in(a, 50);
    return j->exec_id;
}

/* See output.h. Asks the worker to cancel; reports whether the driver can interrupt the
 * running statement or only stop the rest of the script. */
void output_cancel(Output *o, App *a) {
    /* the active session's execution only: another session's keeps running (TBR-020) */
    OutJob *oj = job_of_db(o, a->db);
    if (!oj || !exec_running(oj->j)) {
        int n = output_running_count(o);
        app_status(a, n ? "Nothing is running on this tab's connection (%d running elsewhere: Stop \xE2\x96\xBE lists them)" : "Nothing is running", n);
        return;
    }
    output_cancel_exec(o, a, oj->j->exec_id);
}

/* See output.h. The session of c was closed with a transaction open (reconnect, disconnect):
 * recorded as lost, never as committed: the server decides (TBR-012 / 027). */
void output_note_tx_lost(Output *o, App *a, Connection *c) {
    OutLog *l = &session_for(o, c->id, c->name)->log;
    int x = outlog_exec_begin(l, "-- session closed", -1, 0, -1);
    OutEvent *e = outlog_push(l, x, 0, OT_TX_CHANGED, OL_WARNING, "Transaction lost: the session was closed with a transaction open. Lume did not commit it; the server decides its fate (usually a rollback)");
    if (e) e->tx_state = TXS_UNKNOWN;
    outlog_exec_finish(l, x, ES_FAILED, 0);
    o->dirty = true; (void)a;
}

/* See output.h. Cancels one execution by id, whatever tab is active. */
bool output_cancel_exec(Output *o, App *a, int exec_id) {
    OutJob *oj = job_of_exec(o, exec_id);
    if (!oj || !exec_running(oj->j)) { app_status(a, "Execution #%d is not running", exec_id); return false; }
    if (exec_cancelling(oj->j)) { app_status(a, "Cancel already requested: waiting for the driver to stop"); return true; }
    exec_cancel(oj->j);
    LOG_I("cancel requested: execution %d on %s", exec_id, oj->conn_name);
    app_status(a, oj->j->db->drv->cancel ? "Cancelling #%d on %s\xE2\x80\xA6 the driver was asked to stop the statement" : "Cancel requested for #%d on %s: this driver cannot interrupt a running statement; the rest of the script will not run", exec_id, oj->conn_name);
    o->dirty = true;
    return true;
}

/* Splits a newline-separated notices string into one database_message event per line (lines
 * cut at 500 bytes); lines starting with "WARNING" become warnings. */
static void emit_notices(OutLog *l, int exec_id, int stmt, const char *notices) {
    for (const char *p = notices; p && *p;) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        char line[520]; snprintf(line, sizeof line, "%.*s", (int)(ll > 500 ? 500 : ll), p);
        bool warn = strncasecmp(line, "WARNING", 7) == 0;
        outlog_push(l, exec_id, stmt, OT_DB_MESSAGE, warn ? OL_WARNING : OL_INFO, line);
        p = nl ? nl + 1 : p + ll;
    }
}

/* The SQL of statement k as the Output shows it: the display text's statement when the job has
 * one (parameters: placeholders, never the values), else the text that ran. */
static void shown_sql(const OutJob *oj, int k, const char **p, int *n, int *base) {
    const ExecJob *j = oj->j; const ExecStmt *st = &j->stmts[k];
    if (oj->display) {
        if (oj->ndparts) { *p = oj->display + oj->dparts[k].start; *n = oj->dparts[k].end - oj->dparts[k].start; }
        else { *p = oj->display; *n = (int)strlen(oj->display); }
        *base = -1;   /* not the editor's text: no "Go to SQL" offset */
        return;
    }
    *p = j->text + st->start; *n = st->end - st->start; *base = j->sql_base + st->start;
}

/* Emits statement_started for every statement up to index i not yet reported, so starts are
 * always logged before outcomes. Skipped / never-started statements get no start event. The
 * event time is the worker's start time when known. */
static void emit_started(Output *o, OutJob *oj, OutLog *l, int i) {
    ExecJob *j = oj->j;
    while (j->started_reported <= i && j->started_reported < j->n) {
        int k = j->started_reported++;
        ExecStmt *st = &j->stmts[k];
        int stt = __atomic_load_n(&st->status, __ATOMIC_SEQ_CST);
        if (stt == XS_SKIPPED || (stt == XS_CANCELLED && !st->rs.db)) continue;   /* never ran: no "executing" claim */
        char snip[96], msg[200];
        { const char *sp0; int sn0, sb0; shown_sql(oj, k, &sp0, &sn0, &sb0); snippet(sp0, sn0, snip, sizeof snip); }
        snprintf(msg, sizeof msg, "Executing statement #%d", k + 1);
        const char *sp; int sn, sb; shown_sql(oj, k, &sp, &sn, &sb);
        OutEvent *e = outlog_push_sql(l, j->exec_id, k + 1, OT_STMT_STARTED, OL_INFO, msg, sp, sn, sb, j->editor_rev);
        if (e && st->t_start > 0) e->ts = st->t_start;   /* the worker's clock, not the poll's */
        o->dirty = true;
    }
}

/* Records the outcome of statement i once the worker is done with it: "not run" for skipped
 * or cancelled-before-start, rows loaded + result_set (the ResultSet moves into a Results
 * tab), rows affected, or the failure with every diagnostic field. Also updates the
 * execution's counters and the status line, and opens the Output on errors if configured. */
static void statement_done(Output *o, App *a, OutJob *oj, OutLog *l, int i) {
    ExecJob *j = oj->j;
    const char *sp; int sn, sb; shown_sql(oj, i, &sp, &sn, &sb);
    ExecStmt *st = &j->stmts[i];
    int status = __atomic_load_n(&st->status, __ATOMIC_SEQ_CST);
    OutExec *x = outlog_exec(l, j->exec_id);
    int sid = i + 1;
    char msg[400], snip[96];
    snippet(sp, sn, snip, sizeof snip);
    if (status == XS_SKIPPED) {
        snprintf(msg, sizeof msg, "Not run: an earlier statement failed (stop on error)");
        outlog_push_sql(l, j->exec_id, sid, OT_STMT_COMPLETED, OL_INFO, msg, sp, sn, sb, j->editor_rev);
        if (x) x->skipped++;
        return;
    }
    /* Take ownership of the statement's ResultSet from the job. */
    ResultSet rs;
    if (!exec_take_result(j, i, &rs)) {
        snprintf(msg, sizeof msg, "Not run: the execution was cancelled before it");
        outlog_push_sql(l, j->exec_id, sid, OT_STMT_FAILED, OL_WARNING, msg, sp, sn, sb, j->editor_rev);
        if (x) x->skipped++;
        return;
    }
    if (rs.notices) emit_notices(l, j->exec_id, sid, rs.notices);
    /* Row-returning statement: hand the set to a Results tab (first one per mode, later ones in
     * new tabs) and link the tab to this execution. */
    if (status == XS_OK && rs.has_rows) {
        char nr[32];
        snprintf(nr, sizeof nr, "%d", rs.nrows);
        /* a result of another session (the user switched tab meanwhile) never replaces the tab
         * on screen: it opens its own */
        bool other_session = strcmp(oj->sid, session_id(a)) != 0;
        int mode = oj->rows_landed || other_session ? RUN_NEW_TAB : j->mode;
        results_set_owner(a->results, oj->editor);
        if (mode == RUN_REPLACE && !oj->rows_landed) results_target_replace(a->results, oj->replace_exec);
        results_set(a->results, a, &rs, oj->title, sp, sn, mode, oj->rows_landed ? NULL : oj->qtable);   /* rs moved into the tab (its SQL as shown: no parameter values) */
        if (a->panels) panels_reveal_results(a->panels, a);   /* the result is in view, even when Results was folded */
        oj->rows_landed = true;
        int tab = results_current_exec_id(a->results);
        if (!oj->landed_tab) oj->landed_tab = tab;
        results_tag_output(a->results, tab, j->exec_id);
        if (oj->export_path) {   /* Execute to File: the first result goes to the chosen file */
            bool ok = results_export(a->results, a, oj->export_path);
            const char *base_name = strrchr(oj->export_path, '/');
            char note[300]; snprintf(note, sizeof note, ok ? "Result written to %s" : "Could not write the result to %s", base_name ? base_name + 1 : oj->export_path);
            outlog_push(l, j->exec_id, sid, OT_DB_MESSAGE, ok ? OL_SUCCESS : OL_ERROR, note);
            free(oj->export_path); oj->export_path = NULL;
        }
        OutEvent *e;
        snprintf(msg, sizeof msg, "%s row%s loaded%s", nr, rs.nrows == 1 ? "" : "s", rs.complete ? "" : " (more available)");
        /* the ResultSet now lives in the tab; the numbers were copied before the move */
        e = outlog_push(l, j->exec_id, sid, OT_STMT_COMPLETED, OL_SUCCESS, msg);
        e->exec_ms = st->ms; e->rows_loaded = atol(nr); e->ts = st->t_start + st->ms / 1000.0;
        snprintf(msg, sizeof msg, "Result #%d in the Results panel  \xC2\xB7  statement %d", tab, sid);
        e = outlog_push(l, j->exec_id, sid, OT_RESULT_SET, OL_INFO, msg);
        e->result_tab = tab;
        if (x) x->ok++;
        char dur[24]; app_status(a, "Loaded %s row%s in %s (tab #%d)", nr, atol(nr) == 1 ? "" : "s", fmt_duration(dur, sizeof dur, st->ms), tab);
    /* DML / DDL: rows affected; Oracle's driver commits by itself, so say so. */
    } else if (status == XS_OK) {
        OutEvent *e;
        bool ora = strcmp(j->db->drv->name, "Oracle") == 0;
        snprintf(msg, sizeof msg, "%d row%s affected%s", rs.affected, rs.affected == 1 ? "" : "s", ora ? "  \xC2\xB7  committed by the driver" : "");
        e = outlog_push(l, j->exec_id, sid, OT_ROWS_AFFECTED, OL_SUCCESS, msg);
        e->rows_affected = rs.affected; e->exec_ms = st->ms; e->autocommit = ora ? 1 : -1; e->ts = st->t_start + st->ms / 1000.0;
        if (x) x->ok++;
        oj->touched_schema = true;
        app_status(a, "Statement executed: %d row%s affected  \xC2\xB7  see Output", rs.affected, rs.affected == 1 ? "" : "s");
        rs_free(&rs);
    } else {
        /* failed or cancelled: the error with every field the driver gave (OUT-012) */
        bool cancelled = status == XS_CANCELLED;
        OutEvent *e;
        if (cancelled) snprintf(msg, sizeof msg, "Cancelled by user%s", rs.has_rows ? "  \xC2\xB7  partial result kept" : "");
        else snprintf(msg, sizeof msg, "%s", rs.error[0] ? rs.error : "statement failed");
        e = outlog_push_sql(l, j->exec_id, sid, OT_STMT_FAILED, cancelled ? OL_WARNING : OL_ERROR, msg, sp, sn, sb, j->editor_rev);
        if (!cancelled && a->panels && o->auto_open_errors) panels_reveal(a->panels, a, "lume.data.output");   /* the error is in view (Settings: open the Output on errors) */
        e->exec_ms = st->ms; e->ts = st->t_start + st->ms / 1000.0;
        if (rs.err_state[0]) e->err_state = strdup(rs.err_state);
        if (rs.err_code >= 0) e->err_code = rs.err_code;
        if (rs.err_detail[0]) e->err_detail = strdup(rs.err_detail);
        if (rs.err_hint[0]) e->err_hint = strdup(rs.err_hint);
        if (rs.err_offset >= 0) sql_line_col(j->text, j->len, st->start + rs.err_offset, &e->err_line, &e->err_col);
        /* TBR-013: who refused it, Lume's read-only setting or the database's own rules */
        if (!cancelled) {
            Connection *cc = NULL;
            for (int k = 0; a->conns && k < a->conns->n; k++) if (!strcmp(a->conns->items[k]->id, oj->sid)) cc = a->conns->items[k];
            bool priv = !strcmp(rs.err_state, "42501") || strcasestr(msg, "permission denied") || strstr(msg, "ORA-01031") || strcasestr(msg, "insufficient privilege");
            bool ro = !strcmp(rs.err_state, "25006") || strcasestr(msg, "read-only") || strcasestr(msg, "readonly");
            const char *who = NULL;
            if (priv) who = "Refused by the database: this login lacks the privilege (a server rule, not a Lume setting)";
            else if (ro && cc && cc->read_only) who = cc->ro_level == 1 ? "Refused because the connection is read-only in Lume (enforced by the database engine for this connection)" : "Refused because the connection is read-only in Lume (the session's default)";
            else if (ro) who = "Refused by the database: it is read-only on the server side (not a Lume setting)";
            if (who) { outlog_push(l, j->exec_id, sid, OT_DB_MESSAGE, OL_INFO, who); LOG_I("execution %d: %s", j->exec_id, who); }
        }
        if (cancelled && rs.has_rows) {
            rs.cursor_lost = true; rs.complete = false; rs.cancelled = true;
            if (x) x->partial = true;
            results_set(a->results, a, &rs, "Results (cancelled)", sp, sn, RUN_NEW_TAB, NULL);
            results_tag_output(a->results, results_current_exec_id(a->results), j->exec_id);
        } else rs_free(&rs);
        if (cancelled) LOG_I("execution %d statement %d cancelled after %.1f ms", j->exec_id, sid, st->ms);
        else LOG_E("execution %d statement %d failed on %s after %.1f ms: %s%s%s%s", j->exec_id, sid, j->db->drv->name, st->ms, msg,
                   rs.err_state[0] ? "  sqlstate " : "", rs.err_state, rs.err_code >= 0 ? "  (driver code set)" : "");
        if (log_debug_enabled()) LOG_D("  sql: %.*s", sn > 400 ? 400 : sn, sp);   /* as shown: no parameter values */
        if (x) x->failed += !cancelled;
        if (!cancelled) {
            if (o->auto_open_errors) {                     /* an error that prevents a result opens the Output (OUT-014, configurable) */
                results_show_output(a->results, true);
                output_mark_seen(o, a);
            }
            app_status(a, "Statement %d failed: %s", sid, msg);
        }
    }
}

/* Drains one job: statements finished since the last frame go to its log; when the worker is
 * done, records the transaction change, closes the execution, appends history, refreshes the
 * explorer after DDL / DML and frees the job. Returns true when the job is over (slot free). */
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
/* "30 s", "5 min", "1 h 30 min" */
static const char *fmt_secs(int s) {
    static char b[32];
    if (s < 60 || s % 60) snprintf(b, sizeof b, "%d s", s);
    else if (s < 3600 || s % 3600) snprintf(b, sizeof b, s >= 3600 ? "%d h %d min" : "%d min", s >= 3600 ? s / 3600 : s / 60, (s % 3600) / 60);
    else snprintf(b, sizeof b, "%d h", s / 3600);
    return b;
}
static bool pump_job(Output *o, App *a, OutJob *oj) {
    ExecJob *j = oj->j;
    OutLog *l = job_log(o, oj);
    if (!l) { if (oj->landed) oj->landed(a, 0, "the session was closed", oj->ud); exec_free(j); free(oj->display); free(oj->export_path); free(oj->qtable); return true; }
    int i;
    /* each statement's start is recorded before its outcome, and the outcome before the
     * next statement's start, whatever the polling interval saw */
    while ((i = exec_next_done(j)) >= 0) { emit_started(o, oj, l, i); statement_done(o, a, oj, l, i); o->dirty = true; }
    int cur = __atomic_load_n(&j->cur, __ATOMIC_SEQ_CST);
    if (cur >= 0 && cur < j->n) emit_started(o, oj, l, cur);
    if (exec_running(j)) {
        /* Settings > Query Execution: each statement gets query_timeout_s, timed from when this
         * thread first sees it running; past it the execution is cancelled like Stop */
        if (a->query_timeout_s > 0 && !exec_cancelling(j) && cur >= 0 && cur < j->n) {
            double t = now_s();
            if (cur != oj->timed_cur || oj->timed_t0 <= 0) { oj->timed_cur = cur; oj->timed_t0 = t; }
            else if (t - oj->timed_t0 > a->query_timeout_s) {
                oj->timed_out = true;
                char m[200]; snprintf(m, sizeof m, "Statement %d ran longer than the query timeout (%s, Settings > Query Execution): cancelling", cur + 1, fmt_secs(a->query_timeout_s));
                outlog_push(l, j->exec_id, cur + 1, OT_DB_MESSAGE, OL_WARNING, m);
                LOG_W("execution %d: statement %d passed the query timeout (%d s): cancel requested", j->exec_id, cur + 1, a->query_timeout_s);
                exec_cancel(j); o->dirty = true;
            }
        }
        app_wake_in(a, 100); return false;
    }
    /* finished: transaction state is read from the driver now that the connection is free */
    Db *db = j->db;
    int tx = db->drv->tx_state ? db->drv->tx_state(db) : -1;
    if (tx != oj->tx_before && (tx == 0 || tx == 1)) {
        OutEvent *e = outlog_push(l, j->exec_id, 0, OT_TX_CHANGED, OL_INFO, tx == 1 ? "Transaction open (not committed yet), as reported by the driver" : "Transaction ended, as reported by the driver");
        if (e) { e->tx_action = tx == 1 ? TX_BEGIN : TX_COMMIT; e->tx_state = tx == 1 ? TXS_OPEN : TXS_UNKNOWN; }
    }
    int overall = exec_overall(j);
    outlog_exec_finish(l, j->exec_id, overall, exec_elapsed_ms(j));
    if (!oj->no_history) history_append(o, a, l->session_name[0] ? l->session_name : "no connection", overall, exec_elapsed_ms(j), j->n, oj->display ? oj->display : j->text, oj->display ? (int)strlen(oj->display) : j->len);   /* parameters: the template only */
    if (overall == ES_CANCELLED) { if (oj->timed_out) app_status(a, "Execution #%d on %s cancelled by the query timeout (%s, Settings > Query Execution)", j->exec_id, oj->conn_name, fmt_secs(a->query_timeout_s)); else app_status(a, "Execution #%d on %s cancelled after %.1f s", j->exec_id, oj->conn_name, exec_elapsed_ms(j) / 1000.0); results_show_output(a->results, true); output_mark_seen(o, a); }
    if (oj->touched_schema) explorer_refresh(a->explorer, a);
    app_execution_finished(a, db, j->exec_id, overall);
    if (oj->landed) {
        if (!oj->landed_tab && !oj->landed_err[0]) for (int k = 0; k < j->n; k++) if (j->stmts[k].status == XS_FAILED) { snprintf(oj->landed_err, sizeof oj->landed_err, "the statement failed (see the Output)"); break; }
        oj->landed(a, oj->landed_tab, oj->landed_err[0] ? oj->landed_err : NULL, oj->ud); oj->landed = NULL; oj->ud = NULL;
    }
    exec_free(j); free(oj->display); free(oj->export_path); free(oj->qtable);
    return true;
}

/* Main-loop hook (every frame): loads prefs on first use, drains finished statements from the
 * running job into the log, and when the job is over records the transaction change, closes
 * the execution, appends history, refreshes the explorer after DDL/DML and frees the job. */
void output_pump(Output *o, App *a) {
    if (!o->prefs_loaded) prefs_load(o, a);
    for (int k = 0; k < o->njobs;) {
        if (pump_job(o, a, &o->jobs[k])) { memmove(&o->jobs[k], &o->jobs[k + 1], sizeof o->jobs[0] * (size_t)(o->njobs - k - 1)); o->njobs--; o->dirty = true; }
        else k++;
    }
}

/* Test harness: fills the active log with `executions` synthetic executions of `events_each`
 * events (mixed levels), lifting retention so all of them stay. */
void output_fill(Output *o, App *a, int executions, int events_each) {
    OutLog *l = output_current(o, a);
    l->max_events = 2000000;    /* the benchmark wants them all on screen */
    for (int i = 0; i < executions; i++) {
        char sql[96]; snprintf(sql, sizeof sql, "SELECT * FROM t%d WHERE id = %d", i % 50, i);
        int x = outlog_exec_begin(l, sql, -1, 0, 0);
        OutExec *ex = outlog_exec(l, x); if (ex) ex->nstmts = 1;
        for (int k = 0; k < events_each; k++) {
            int lv = k % 7 == 6 ? OL_WARNING : k % 11 == 10 ? OL_ERROR : OL_INFO;
            char m[160]; snprintf(m, sizeof m, "Synthetic event %d of execution %d: notice text with some length to render", k, i);
            OutEvent *e = outlog_push(l, x, 1, lv == OL_ERROR ? OT_STMT_FAILED : OT_DB_MESSAGE, lv, m);
            e->exec_ms = k * 0.5;
        }
        if (ex) ex->ok = 1;
        outlog_exec_finish(l, x, ES_COMPLETED, 1.0);
    }
    o->dirty = true;
}
/* Test harness: number of rows in the current visible list. */
int output_rows(Output *o) { return o->nrows; }

/* ---- rows ------------------------------------------------------------------- */

/* OutFilter level mask for the current checkboxes: 0 (no filter) when everything is shown;
 * Debug events follow the Info checkbox. */
static unsigned level_mask(Output *o) { return o->levels == (1u << OL_COUNT) - 1 ? 0 : (o->levels | (o->levels & (1u << OL_INFO) ? 1u << OL_DEBUG : 0)); }
/* True when event e passes the toolbar filters (levels, execution, search text). */
static bool pass(Output *o, const OutEvent *e) {
    OutFilter f = { level_mask(o), o->exec_filter, o->search.len ? o->search.buf : NULL, false };
    return outlog_visible(NULL, e, &f);
}

/* Appends a list row, growing rows and the row-top array (which keeps one extra slot). */
static void push_row(Output *o, int kind, int exec, int ev) {
    if (o->nrows == o->cap_rows) { o->cap_rows = o->cap_rows ? o->cap_rows * 2 : 512; o->rows = realloc(o->rows, sizeof *o->rows * o->cap_rows); o->tops = realloc(o->tops, sizeof *o->tops * (o->cap_rows + 1)); }
    o->rows[o->nrows].kind = (uint8_t)kind; o->rows[o->nrows].exec = exec; o->rows[o->nrows].ev = ev; o->rows[o->nrows].hidden = 0; o->nrows++;
}

/* Estimated number of wrapped lines for s in avail pixels (1..12), from its single-line width. */
static int wrap_lines(App *a, Font *f, const char *s, float avail) {
    if (avail < px(a, 40)) return 1;
    float w = font_width(f, s, -1);
    int n = (int)(w / avail) + 1;
    return n > 12 ? 12 : n;
}

/* Number of lines in a newline-separated text. */
static int details_lines(const char *t) { int n = 1; for (; *t; t++) if (*t == '\n') n++; return n; }

/* Rebuilds the visible Row list and their tops (prefix sums of heights) from log l for a list
 * of the given width. Cheap no-op when nothing changed: the cache key covers filters, wrap,
 * expanded event, pause, search text, collapse state, event/execution counts and width. */
static void rebuild(Output *o, App *a, const OutLog *l, float width) {
    unsigned sig = o->levels * 7u + (unsigned)o->exec_filter * 131u + (unsigned)o->wrap * 3u + (unsigned)(o->expanded_id & 0xFFFF) * 17u
                 + (o->frozen ? (unsigned)o->frozen_id * 2654435761u + 1u : 0u);
    for (const char *p = o->search.buf; *p; p++) sig = sig * 31u + (unsigned char)*p;
    for (int i = 0; i < l->nex; i++) sig = sig * 5u + (unsigned)l->ex[i].collapsed;
    if (!o->dirty && o->cache_log == l && o->cache_n == l->n && o->cache_nex == l->nex && o->cache_sig == sig && o->cache_w == width) return;
    o->cache_log = l; o->cache_n = l->n; o->cache_nex = l->nex; o->cache_sig = sig; o->cache_w = width; o->dirty = false;
    o->nrows = 0;
    /* Group events by execution: a header row per execution, then its events that pass the
     * filters (collapsed groups only count their hidden events). */
    int last_exec = INT_MIN, head = -1; bool collapsed = false, skip = false;
    for (int i = 0; i < l->n; i++) {
        const OutEvent *e = &l->ev[i];
        if (o->frozen && e->id > o->frozen_id) continue;     /* Pause view: newer events wait */
        if (e->type == OT_NOTE) {   /* a Lume message: one row of its own, outside the grouping (it may land inside a running execution) */
            if ((!o->exec_filter || e->exec_id == o->exec_filter) && pass(o, e)) push_row(o, 1, e->exec_id, i);
            continue;
        }
        if (e->exec_id != last_exec) {
            last_exec = e->exec_id;
            OutExec *x = outlog_exec((OutLog *)l, e->exec_id);
            if (o->exec_filter && e->exec_id != o->exec_filter) { skip = true; continue; }
            skip = false;
            head = o->nrows;
            push_row(o, 0, e->exec_id, i);
            collapsed = x ? x->collapsed : false;
        }
        if (skip) continue;
        if (e->type == OT_EXEC_STARTED) continue;              /* the group header is that row (mock-up) */
        if (!pass(o, e)) { if (!collapsed && head >= 0) o->rows[head].hidden++; continue; }
        if (collapsed) continue;
        push_row(o, 1, e->exec_id, i);
    }
    /* heights: prefix sums so the list stays virtualised with wrap and one expanded event */
    float y = 0, rh = px(a, ROW_H), avail = width - px(a, 150);
    for (int r = 0; r < o->nrows; r++) {
        o->tops[r] = y;
        Row *row = &o->rows[r];
        float h = rh;
        if (row->kind == 1) {
            const OutEvent *e = &l->ev[row->ev];
            if (o->wrap) h = rh * wrap_lines(a, a->font_ui, e->message, avail);
            if (sql_line(e)) h += rh;      /* the SQL text under the row (mock-up) */
            if (e->id == o->expanded_id) {
                char *d = outlog_event_text(l, e, true);
                h += px(a, 18) * details_lines(d) + px(a, 8);
                free(d);
            }
        }
        y += h;
    }
    o->tops[o->nrows] = y;
}

/* Binary search of the row containing list-space y (pixels from the list top); -1 if none. */
static int row_at(Output *o, float y) {
    int lo = 0, hi = o->nrows - 1;
    while (lo <= hi) { int mid = (lo + hi) / 2; if (y < o->tops[mid]) hi = mid - 1; else if (y >= o->tops[mid + 1]) lo = mid + 1; else return mid; }
    return -1;
}

/* ---- actions ---------------------------------------------------------------- */

/* Copies one row to the clipboard: an execution header as a one-line summary, an event as
 * its message or full details (redacted by outlog_event_text). */
static void copy_row(Output *o, App *a, OutLog *l, int r, bool details) {
    if (r < 0 || r >= o->nrows) return;
    Row *row = &o->rows[r];
    if (row->kind == 0) {
        OutExec *x = outlog_exec(l, row->exec);
        char t[400]; snprintf(t, sizeof t, "#%d %s \xC2\xB7 %s \xC2\xB7 %d statements (%d ok, %d failed, %d skipped)", x->id, x->summary, outlog_status_name(x->status), x->nstmts, x->ok, x->failed, x->skipped);
        SDL_SetClipboardText(t);
    } else {
        char *t = outlog_event_text(l, &l->ev[row->ev], details);
        SDL_SetClipboardText(t); free(t);
    }
    app_status(a, details ? "Copied event details (secrets masked)" : "Copied message (secrets masked)");
}

/* Selected rows as [lo, hi] in display order; false when nothing is selected. */
static bool sel_range(const Output *o, int *lo, int *hi) {
    if (o->sel < 0 || o->sel >= o->nrows) return false;
    int an = o->anchor >= 0 && o->anchor < o->nrows ? o->anchor : o->sel;
    *lo = an < o->sel ? an : o->sel; *hi = an < o->sel ? o->sel : an;
    return true;
}

/* Copies every selected row in display order (OUT-021): one line per message, or the
 * structured details separated by a blank line. Same redaction as a single copy. */
static void copy_selection(Output *o, App *a, OutLog *l, bool details) {
    int lo, hi;
    if (!sel_range(o, &lo, &hi)) return;
    if (lo == hi) { copy_row(o, a, l, lo, details); return; }
    size_t cap = 4096, len = 0; char *buf = malloc(cap); buf[0] = 0;
    int n = 0;
    for (int r = lo; r <= hi; r++) {
        Row *row = &o->rows[r];
        char *t;
        if (row->kind == 0) {
            OutExec *x = outlog_exec(l, row->exec);
            char h[400]; snprintf(h, sizeof h, "#%d %s \xC2\xB7 %s", x->id, x->summary, outlog_status_name(x->status));
            t = strdup(h);
        } else t = outlog_event_text(l, &l->ev[row->ev], details);
        size_t tl = strlen(t);
        while (len + tl + 3 > cap) { cap *= 2; buf = realloc(buf, cap); }
        if (n++) { buf[len++] = '\n'; if (details) buf[len++] = '\n'; }
        memcpy(buf + len, t, tl); len += tl; buf[len] = 0;
        free(t);
    }
    SDL_SetClipboardText(buf); free(buf);
    app_status(a, "Copied %d rows in order%s (secrets masked)", n, details ? " with details" : "");
}

/* See output.h. */
void output_copy(Output *o, App *a, bool details) {
    OutLog *l = output_current(o, a);
    if (o->sel < 0) { app_status(a, "Select an event first (click it, Shift+click or Shift+arrows for a range)"); return; }
    copy_selection(o, a, l, details);
}
/* Test harness: centre (window points) of the Output context-menu item starting with `label`. */
bool output_menu_pos(Output *o, App *a, const char *label, float *x, float *y) {
    Rect rc;
    if (!popup_item_rect(a, &o->menu, label, &rc)) return false;
    *x = (rc.x + rc.w / 2) / a->density; *y = (rc.y + rc.h / 2) / a->density;
    return true;
}
/* See output.h: selects every visible row (anchor at the first, selection at the last). */
void output_select_all(Output *o) { if (o->nrows) { o->anchor = 0; o->sel = o->nrows - 1; } }

/* See output.h. Logs a table-editor apply as one execution on the session of the connection
 * that owns db: BEGIN, each statement that ran, then the failure + ROLLBACK, the COMMIT, an
 * unknown outcome or "not applied". */
void output_record_apply(Output *o, App *a, Db *db, const char *table, const EditSet *e, int status, const ApplyResult *res, double ms) {
    /* the session of the connection that owns this Db, like an editor run on it */
    Connection *c = NULL;
    for (int i = 0; a->conns && i < a->conns->n; i++) if (a->conns->items[i]->db == db) c = a->conns->items[i];
    OutLog *l = &session_for(o, c ? c->id : session_id(a), c ? c->name : "no connection")->log;
    char title[300];
    snprintf(title, sizeof title, "Table editor \xC2\xB7 %d statement%s on %s", e->nstmts, e->nstmts == 1 ? "" : "s", table);
    int x = outlog_exec_begin(l, title, -1, 0, 0);
    OutExec *xe = outlog_exec(l, x);
    if (xe) xe->nstmts = e->nstmts;
    char msg[600];
    /* statements: those before the failing one ran; with success all of them did */
    int ran = status == APPLY_OK || status == APPLY_COMMIT_FAILED || status == APPLY_UNKNOWN ? e->nstmts
            : status == APPLY_STMT_FAILED || status == APPLY_CONFLICT ? res->stmt : 0;
    OutEvent *ev = outlog_push(l, x, 0, OT_TX_CHANGED, OL_INFO, "BEGIN \xC2\xB7 table editor transaction");
    if (ev) { ev->tx_action = TX_BEGIN; ev->tx_state = TXS_OPEN; ev->autocommit = 0; }
    for (int i = 0; i < ran && i < e->nstmts; i++) {
        snprintf(msg, sizeof msg, "%s applied, not yet committed", e->stmts[i].kind == 1 ? "DELETE" : e->stmts[i].kind == 2 ? "INSERT" : "UPDATE");
        OutEvent *se = outlog_push_sql(l, x, i + 1, OT_STMT_COMPLETED, OL_SUCCESS, msg, e->stmts[i].shown ? e->stmts[i].shown : e->stmts[i].sql, -1, -1, 0);   /* start -1: not from the SQL editor */
        if (xe) xe->ok++;
        (void)se;
    }
    if (status == APPLY_STMT_FAILED || status == APPLY_CONFLICT) {
        int k = res->stmt;
        const char *sql = k >= 0 && k < e->nstmts ? (e->stmts[k].shown ? e->stmts[k].shown : e->stmts[k].sql) : NULL;
        snprintf(msg, sizeof msg, "%s%s", status == APPLY_CONFLICT ? "Conflict: " : "", status == APPLY_CONFLICT && res->conflict[0] ? res->conflict : res->message);
        OutEvent *fe = sql ? outlog_push_sql(l, x, k + 1, OT_STMT_FAILED, OL_ERROR, msg, sql, -1, -1, 0) : outlog_push(l, x, k + 1, OT_STMT_FAILED, OL_ERROR, msg);
        (void)fe;
        if (xe) { xe->failed++; xe->skipped = e->nstmts - k - 1; }
        /* the rollback is its own event, linked to the statement that caused it */
        OutEvent *rb = outlog_push(l, x, k + 1, OT_TX_CHANGED, OL_WARNING, "ROLLBACK \xC2\xB7 nothing from this batch was written; the changes stay pending");
        if (rb) { rb->tx_action = TX_ROLLBACK; rb->tx_state = TXS_ROLLED_BACK; }
    } else if (status == APPLY_OK) {
        snprintf(msg, sizeof msg, "COMMIT confirmed by %s \xC2\xB7 %d statement%s persisted", db->drv->name, e->nstmts, e->nstmts == 1 ? "" : "s");
        OutEvent *ce = outlog_push(l, x, 0, OT_TX_CHANGED, OL_SUCCESS, msg);
        if (ce) { ce->tx_action = TX_COMMIT; ce->tx_state = TXS_COMMITTED; }
    } else if (status == APPLY_UNKNOWN) {
        OutEvent *ue = outlog_push(l, x, 0, OT_TX_CHANGED, OL_WARNING, "COMMIT sent, connection lost before the answer: the result is unknown; the changes stay pending");
        if (ue) { ue->tx_action = TX_COMMIT; ue->tx_state = TXS_UNKNOWN; }
    } else {
        snprintf(msg, sizeof msg, "Not applied (%s): %s", edit_apply_status_name(status), res->message);
        OutEvent *ne = outlog_push(l, x, 0, OT_TX_CHANGED, OL_ERROR, msg);
        if (ne) { ne->tx_action = status == APPLY_COMMIT_FAILED ? TX_COMMIT : TX_ROLLBACK; ne->tx_state = TXS_ROLLED_BACK; }
    }
    outlog_exec_finish(l, x, status == APPLY_OK ? ES_COMPLETED : ES_FAILED, ms);
    o->dirty = true;
}

/* See output.h. Searches every session, newest events first, for the result_set event that
 * created tab_exec and stores the render time on it. */
void output_note_render(Output *o, int out_exec, int tab_exec, double ms) {
    for (int si = 0; si < o->nsess; si++) {
        OutLog *l = &o->sess[si].log;
        for (int i = l->n - 1; i >= 0; i--)
            if (l->ev[i].exec_id == out_exec && l->ev[i].type == OT_RESULT_SET && l->ev[i].result_tab == tab_exec) { l->ev[i].render_ms = ms; o->dirty = true; return; }
    }
}

/* Scrolls the list so row r is fully visible. */
static void reveal(Output *o, App *a, OutLog *l, int r) {
    if (r < 0 || r >= o->nrows) return;
    rebuild(o, a, l, o->cache_w);
    float top = o->tops[r], bot = o->tops[r + 1];
    if (top < o->scroll) o->scroll = top;
    if (bot > o->scroll + o->list_rc.h) o->scroll = bot - o->list_rc.h;
    o->follow = false;
}

/* Search navigation (OUT-019): with a search active the list holds only matching events, so
 * the next / previous match is the next / previous event row, wrapping at the ends. */
static void search_step(Output *o, App *a, OutLog *l, int dir) {
    if (!o->search.len || !o->nrows) return;
    int start = o->sel;
    for (int k = 1; k <= o->nrows; k++) {
        int r = ((start < 0 ? (dir > 0 ? -1 : 0) : start) + dir * k % o->nrows + o->nrows * 2) % o->nrows;
        if (o->rows[r].kind == 1) { o->sel = o->anchor = r; reveal(o, a, l, r); return; }
    }
    app_status(a, "No event matches \"%s\"", o->search.buf);
}

/* Position of the selected row among the matches (1-based), 0 when it is not one. */
static int search_pos(const Output *o, int *total) {
    int pos = 0, n = 0;
    for (int r = 0; r < o->nrows; r++) if (o->rows[r].kind == 1) { n++; if (r == o->sel) pos = n; }
    *total = n;
    return pos;
}

/* "Go to SQL" for row r: selects the statement in the editor at its recorded offsets if the
 * editor is unchanged, else searches the same text, else expands the event details. Header
 * rows use the first event of the execution that carries SQL. */
static void goto_sql(Output *o, App *a, OutLog *l, int r) {
    if (r < 0 || r >= o->nrows) return;
    const OutEvent *e = o->rows[r].kind == 1 ? &l->ev[o->rows[r].ev] : NULL;
    if (!e || !e->sql) {
        /* the execution header: use its started event */
        for (int i = 0; i < l->n; i++) if (l->ev[i].exec_id == o->rows[r].exec && l->ev[i].sql) { e = &l->ev[i]; break; }
    }
    if (!e || !e->sql) { app_status(a, "This event has no SQL attached"); return; }
    if (e->sql_start < 0) { app_status(a, "This statement came from the table editor, not from the SQL editor: its SQL is in the event details (double-click the row)"); return; }
    if (editor_revision(a->editor) == e->editor_rev && editor_select_range(a->editor, e->sql_start, e->sql_end)) {
        a->focus = FOCUS_EDITOR;
        app_status(a, "Selected the statement as executed (editor unchanged since)");
        return;
    }
    /* the buffer changed: find the same text, else point at the snapshot */
    int len; char *text = editor_text(a->editor, &len);
    const char *hit = text && e->sql && !e->sql_truncated ? strstr(text, e->sql) : NULL;
    if (hit) {
        int start = (int)(hit - text);
        editor_select_range(a->editor, start, start + (int)strlen(e->sql));
        a->focus = FOCUS_EDITOR;
        app_status(a, "The editor changed since this ran: selected the same text where it is now");
    } else {
        o->expanded_id = e->id; o->dirty = true;
        app_status(a, "The editor changed since this ran and the text is gone: the SQL as executed is in the event details");
    }
    free(text);
}

/* "Open results" for row r: switches to the Results tab recorded on the event (or on any event
 * of the execution for a header row), or says it is gone. */
static void open_results(Output *o, App *a, OutLog *l, int r) {
    if (r < 0 || r >= o->nrows) return;
    int tab = 0;
    if (o->rows[r].kind == 1 && l->ev[o->rows[r].ev].result_tab) tab = l->ev[o->rows[r].ev].result_tab;
    else for (int i = 0; i < l->n; i++) if (l->ev[i].exec_id == o->rows[r].exec && l->ev[i].result_tab) { tab = l->ev[i].result_tab; break; }
    if (!tab) { app_status(a, "This execution produced no result set"); return; }
    if (results_select_exec(a->results, tab)) app_status(a, "Opened result tab #%d", tab);
    else app_status(a, "Result tab #%d was closed or discarded; re-run the statement to see it again", tab);
}

/* Opens the context menu for row r (-1 = empty area) at (x, y). Inside a multi-row selection
 * the copy items act on the whole range. */
static void context_menu(Output *o, App *a, OutLog *l, int r, float x, float y) {
    o->menu_row = r;
    popup_open(&o->menu, x, y);
    bool ev = r >= 0 && r < o->nrows && o->rows[r].kind == 1;
    bool any = r >= 0 && r < o->nrows;
    int lo, hi, n = sel_range(o, &lo, &hi) && r >= lo && r <= hi ? hi - lo + 1 : 1;
    if (n > 1) {   /* a right-click inside a range acts on the whole range, in display order */
        snprintf(o->menu_lbl[0], sizeof o->menu_lbl[0], "Copy %d rows", n);
        snprintf(o->menu_lbl[1], sizeof o->menu_lbl[1], "Copy details of %d rows", n);
    }
    popup_add(&o->menu, OM_COPY, n > 1 ? o->menu_lbl[0] : "Copy message", false, any);
    popup_add(&o->menu, OM_COPY_DETAILS, n > 1 ? o->menu_lbl[1] : "Copy details", false, ev || n > 1);
    popup_add(&o->menu, OM_GOTO_SQL, "Go to SQL", true, any);
    popup_add(&o->menu, OM_OPEN_RESULTS, "Open results", false, any);
    popup_add_caption(&o->menu, "FILTER  \xC2\xB7  EVENTS STAY RECORDED", 0, true);
    popup_add(&o->menu, OM_ONLY_EXEC, "Only this execution", false, any && !o->exec_filter);
    popup_add(&o->menu, OM_ONLY_ERRORS, "Only errors and warnings", false, o->levels == (1u << OL_COUNT) - 1);
    popup_add(&o->menu, OM_ALL, "Clear filters", false, o->exec_filter || o->levels != (1u << OL_COUNT) - 1);
    popup_add(&o->menu, OM_COLLAPSE_ALL, "Collapse all", true, l->nex > 0);
    popup_add(&o->menu, OM_EXPAND_ALL, "Expand all", false, l->nex > 0);
    popup_add(&o->menu, OM_EXPORT, "Export\xE2\x80\xA6", true, l->n > 0);
    popup_add(&o->menu, OM_CLEAR, "Clear output", false, l->n > 0);
    popup_add(&o->menu, OM_AUTO_OPEN, o->auto_open_errors ? "Open Output on statement errors: on" : "Open Output on statement errors: off", true, true);
    popup_add(&o->menu, OM_RESET_LAYOUT, "Reset Output layout", false, true);
}

/* Executes a context / Options menu item. r is the row the menu was opened on. */
static void menu_run(Output *o, App *a, OutLog *l, int id) {
    int r = o->menu_row;
    switch (id) {
    case OM_COPY: copy_selection(o, a, l, false); break;
    case OM_COPY_DETAILS: copy_selection(o, a, l, true); break;
    case OM_GOTO_SQL: goto_sql(o, a, l, r); break;
    case OM_OPEN_RESULTS: open_results(o, a, l, r); break;
    case OM_ONLY_EXEC: if (r >= 0 && r < o->nrows) { o->exec_filter = o->rows[r].exec; o->dirty = true; } break;
    case OM_ONLY_ERRORS: o->levels = (1u << OL_WARNING) | (1u << OL_ERROR); o->dirty = true; break;
    case OM_ALL: o->levels = (1u << OL_COUNT) - 1; o->exec_filter = 0; o->dirty = true; break;
    case OM_LV_INFO: case OM_LV_SUCCESS: case OM_LV_WARNING: case OM_LV_ERROR: {
        static const int bits[4] = { OL_INFO, OL_SUCCESS, OL_WARNING, OL_ERROR };
        o->levels ^= 1u << bits[id - OM_LV_INFO]; o->dirty = true; break;
    }
    case OM_EXEC_TOGGLE:
        if (o->exec_filter) o->exec_filter = 0;
        else if (o->sel >= 0 && o->sel < o->nrows) o->exec_filter = o->rows[o->sel].exec;
        else app_status(a, "Select an execution or event first, then filter to it");
        o->dirty = true; break;
    case OM_WRAP: o->wrap = !o->wrap; o->dirty = true; break;
    case OM_TEXT: o->text_mode = !o->text_mode; o->tv_sig = 0; break;
    case OM_FOLLOW: o->follow = !o->follow; o->paused = false; break;
    case OM_COLLAPSE_ALL: for (int i = 0; i < l->nex; i++) l->ex[i].collapsed = true; o->dirty = true; break;
    case OM_EXPAND_ALL: for (int i = 0; i < l->nex; i++) l->ex[i].collapsed = false; o->dirty = true; break;
    case OM_EXPORT: app_export_results(a, 1); break;
    case OM_AUTO_OPEN: o->auto_open_errors = !o->auto_open_errors; app_status(a, o->auto_open_errors ? "The Output opens when a statement fails" : "Statement errors only badge the Output tab; it no longer opens by itself"); break;
    case OM_RESET_LAYOUT: prefs_reset(o); app_status(a, "Output layout reset: wrap off, follow tail on, every severity shown, opens on errors"); break;
    case OM_CLEAR: { long dropped = l->n; outlog_clear(l); o->sel = o->anchor = -1; o->expanded_id = 0; o->dirty = true; app_status(a, "Output view cleared (%ld events dropped from the view; nothing else is deleted)", dropped); break; }
    }
}

/* See output.h. Exports the active log with the toolbar filters applied; ".json" selects the
 * JSON format, anything else plain text. Reports the outcome on the status line. */
bool output_export(Output *o, App *a, const char *path) {
    OutLog *l = output_current(o, a);
    OutFilter f = { level_mask(o), o->exec_filter, o->search.len ? o->search.buf : NULL, false };
    const char *ext = strrchr(path, '.');
    bool json = ext && strcasecmp(ext, ".json") == 0;
    char err[256];
    int hidden = 0; int vis = outlog_count(l, &f, &hidden);
    if (!outlog_export(l, &f, path, json, err, sizeof err)) { app_status(a, "Export failed: %s", err); return false; }
    const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
    app_status(a, "Exported %d event%s as %s to %s%s%s", vis, vis == 1 ? "" : "s", json ? "JSON (schema 1)" : "text", base, hidden ? " (filter active: some events left out)" : "", l->dropped_events ? "; older executions were already dropped by retention" : "");
    return true;
}

/* ---- input + drawing -------------------------------------------------------- */

/* See output.h: runs the popup menu before the rest of the UI sees input. */
void output_pre_update(Output *o, App *a) {
    OutLog *l = output_current(o, a);
    int chosen = popup_update(a, &o->menu);
    if (chosen >= 0) menu_run(o, a, l, chosen);
}

/* Draws the popup menu above everything else. */
void output_draw_overlay(Output *o, App *a) { popup_draw(a, &o->menu); }

/* rows that show the statement text on a second line: started, and skipped / never-run ones */
static bool sql_line(const OutEvent *e) {
    if (!e->sql) return false;
    if (e->type == OT_STMT_STARTED) return true;
    if (e->type == OT_STMT_COMPLETED && e->level == OL_INFO) return true;
    if (e->type == OT_STMT_FAILED && e->level == OL_WARNING && e->exec_ms < 0) return true;
    return false;
}

/* Short uppercase label shown before an event's message (e.g. "ERROR", "NOT RUN"). */
static const char *type_label(const OutEvent *e) {
    switch (e->type) {
    case OT_EXEC_STARTED: return "EXECUTION";
    case OT_STMT_STARTED: return "STARTED";
    case OT_STMT_COMPLETED: return e->level == OL_SUCCESS ? "SUCCESS" : "NOT RUN";
    case OT_STMT_FAILED: return e->level == OL_WARNING ? (e->exec_ms < 0 ? "NOT RUN" : "CANCELLED") : "ERROR";
    case OT_RESULT_SET: return "RESULT SET";
    case OT_ROWS_AFFECTED: return "AFFECTED ROWS";
    case OT_DB_MESSAGE: return e->level == OL_WARNING ? "WARNING" : "NOTICE";
    case OT_TX_CHANGED: return "TRANSACTION";
    case OT_EXEC_CANCELLED: return "CANCELLED";
    case OT_EXEC_FINISHED: return e->level == OL_ERROR ? "FAILED" : "FINISHED";
    case OT_NOTE: return "LUME";
    default: return "EVENT";
    }
}

/* Glyph drawn in front of an event of the given OL_* level. */
static const char *level_glyph(int level) {
    switch (level) { case OL_SUCCESS: return "\xE2\x9C\x93"; case OL_WARNING: return "!"; case OL_ERROR: return "\xC3\x97"; case OL_DEBUG: return "\xC2\xB7"; default: return "\xE2\x80\xA2"; }
}
/* Theme colour for an OL_* level. */
static uint32_t level_color(int level) {
    switch (level) { case OL_SUCCESS: return T_GREEN; case OL_WARNING: return T_YELLOW; case OL_ERROR: return T_RED; case OL_DEBUG: return T_TEXT_DIM; default: return T_TEXT_MUTED; }
}
/* Glyph for an execution's ES_* status on its header row. */
static const char *status_glyph(int st) {
    switch (st) { case ES_COMPLETED: return "\xE2\x9C\x93"; case ES_FAILED: return "\xC3\x97"; case ES_CANCELLED: return "!"; case ES_RUNNING: case ES_CANCELLING: return "\xE2\x80\xA2"; default: return "\xC2\xB7"; }
}
/* Theme colour for an execution's ES_* status. */
static uint32_t status_color(int st) {
    switch (st) { case ES_COMPLETED: return T_GREEN; case ES_FAILED: return T_RED; case ES_CANCELLED: return T_YELLOW; case ES_RUNNING: case ES_CANCELLING: return T_ACCENT; default: return T_TEXT_DIM; }
}
/* Local wall-clock time of ts (epoch seconds) as HH:MM:SS, with .mmm when ms is true. */
static void fmt_clock(double ts, bool ms, char *out, size_t n) {
    time_t t = (time_t)ts; struct tm tm; localtime_r(&t, &tm);
    if (ms) snprintf(out, n, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, (int)((ts - (double)t) * 1000));
    else snprintf(out, n, "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/* Keyboard handling for the list while the results area has focus and the search field does
 * not: arrows / Home / End / PgUp / PgDn move the selection (Shift extends the range), Enter /
 * Space toggle a group or expand an event, Left / Right collapse / expand, Esc clears search,
 * details or selection, Cmd+C / A / G, F3 and Cmd+. act as in the menus. Keeps the selection
 * visible. */

/* ---- text view: plain lines, same filters and search as the structured list ---- */
static void tv_add(char **b, size_t *w, size_t *cap, const char *s) {
    size_t L = strlen(s);
    if (*w + L + 2 > *cap) { while (*w + L + 2 > *cap) *cap = *cap ? *cap * 2 : 65536; *b = realloc(*b, *cap); }
    memcpy(*b + *w, s, L); *w += L; (*b)[*w] = 0;
}
static void tv_ts(double ts, char *out, size_t n, bool ms) { time_t t = (time_t)ts; struct tm tm; localtime_r(&t, &tm); char b[16]; strftime(b, sizeof b, "%H:%M:%S", &tm); if (ms) snprintf(out, n, "%s.%03d", b, (int)((ts - floor(ts)) * 1000)); else snprintf(out, n, "%s", b); }
static void tv_build(Output *o, OutLog *l) {
    unsigned long long sig = (unsigned long long)l->n * 1000003ULL + l->next_id * 31ULL + (unsigned long long)o->levels * 7ULL + (unsigned long long)o->exec_filter * 131ULL + (o->frozen ? o->frozen_id : 0);
    for (const char *p = o->search.buf; *p; p++) sig = sig * 131 + (unsigned char)*p;
    for (int i = 0; i < l->nex; i++) sig = sig * 17 + (unsigned long long)l->ex[i].status + (unsigned long long)(l->ex[i].ms * 1000);
    if (sig == o->tv_sig && o->tv_text) return;
    o->tv_sig = sig;
    char *b = NULL; size_t w = 0, cap = 0; tv_add(&b, &w, &cap, "");
    OutFilter f = { o->levels == (1u << OL_COUNT) - 1 ? 0 : o->levels, o->exec_filter, o->search.buf, false };
    char line[5200], red[4200], ts[32], dur[32];
    int last_exec = INT_MIN;
    for (int i = 0; i < l->n; i++) {
        const OutEvent *e = &l->ev[i];
        if (o->frozen && e->id > o->frozen_id) continue;
        if (e->type == OT_EXEC_STARTED) continue;
        if (!outlog_visible(l, e, &f)) continue;
        if (e->exec_id != last_exec && e->type != OT_NOTE) {   /* notes print as plain lines, no execution header */
            OutExec *x = outlog_exec(l, e->exec_id);
            if (last_exec != INT_MIN) tv_add(&b, &w, &cap, "\n");
            tv_ts(x ? x->started : e->ts, ts, sizeof ts, false);
            char sum[300]; outlog_redact(x && x->summary ? x->summary : "", sum, sizeof sum);
            if (x && x->ms > 0) snprintf(dur, sizeof dur, " \xC2\xB7 %.0f ms", x->ms); else dur[0] = 0;
            snprintf(line, sizeof line, "-- #%d  %s  %s  [%s \xC2\xB7 %d statement(s)%s]\n", e->exec_id, ts, sum, x ? outlog_status_name(x->status) : "", x ? x->nstmts : 0, dur);
            tv_add(&b, &w, &cap, line);
            last_exec = e->exec_id;
        }
        tv_ts(e->ts, ts, sizeof ts, true);
        outlog_redact(e->message, red, sizeof red);
        for (char *q = red; *q; q++) if (*q == '\n') *q = ' ';
        char extra[64] = ""; if (e->exec_ms >= 0) snprintf(extra, sizeof extra, "  (%.1f ms)", e->exec_ms);
        snprintf(line, sizeof line, "%s  %-8s %s%s\n", ts, outlog_level_name(e->level), red, extra);
        tv_add(&b, &w, &cap, line);
        if (e->sql && e->type != OT_STMT_COMPLETED && e->level != OL_ERROR) {   /* the statement once, indented (the error line below it needs no copy) */
            outlog_redact(e->sql, red, sizeof red);
            for (char *ln = red; ln && *ln;) { char *nl = strchr(ln, '\n'); if (nl) *nl = 0; snprintf(line, sizeof line, "                        %s\n", ln); tv_add(&b, &w, &cap, line); ln = nl ? nl + 1 : NULL; }
        }
        if (e->level == OL_ERROR) {   /* what other tools print under an error */
            if (e->err_state) { snprintf(line, sizeof line, "                        SQLSTATE %s%s", e->err_state, e->err_code >= 0 ? "" : "\n"); tv_add(&b, &w, &cap, line); if (e->err_code >= 0) { snprintf(line, sizeof line, ", code %ld\n", e->err_code); tv_add(&b, &w, &cap, line); } }
            else if (e->err_code >= 0) { snprintf(line, sizeof line, "                        code %ld\n", e->err_code); tv_add(&b, &w, &cap, line); }
            if (e->err_detail) { outlog_redact(e->err_detail, red, sizeof red); snprintf(line, sizeof line, "                        detail: %s\n", red); tv_add(&b, &w, &cap, line); }
            if (e->err_hint) { snprintf(line, sizeof line, "                        hint: %s\n", e->err_hint); tv_add(&b, &w, &cap, line); }
            if (e->err_line >= 0) { snprintf(line, sizeof line, "                        at line %d%s\n", e->err_line, ""); tv_add(&b, &w, &cap, line); }
        }
    }
    if (!w) tv_add(&b, &w, &cap, "(no events)\n");
    if (o->tv_anchor > (int)w || o->tv_caret > (int)w) o->tv_anchor = o->tv_caret = 0;   /* the text shrank: the selection is gone */
    free(o->tv_text); o->tv_text = b;
    /* line offsets for drawing only what is visible */
    free(o->tv_off); o->tv_n = 0;
    int capn = 256; o->tv_off = malloc(sizeof(int) * (size_t)capn);
    for (size_t k = 0, st = 0; k <= w; k++) if (k == w || b[k] == '\n') { if (k == w && st == w) break; if (o->tv_n == capn) { capn *= 2; o->tv_off = realloc(o->tv_off, sizeof(int) * (size_t)capn); } o->tv_off[o->tv_n++] = (int)st; st = k + 1; }
}
/* the byte offset under a point of the text view */
static int tv_hit(Output *o, App *a, Rect list, float mx, float my, Font *f, float lh) {
    if (!o->tv_n) return 0;
    int i = (int)((my - list.y - px(a, 4) + o->tv_scroll) / lh);
    if (i < 0) return 0;
    if (i >= o->tv_n) i = o->tv_n - 1;
    const char *s = o->tv_text + o->tv_off[i]; const char *e = strchr(s, '\n'); int len = e ? (int)(e - s) : (int)strlen(s);
    float x0 = list.x + px(a, 10);
    int k = 0;
    while (k < len) {
        int nk = k + 1; while (nk < len && ((unsigned char)s[nk] & 0xC0) == 0x80) nk++;   /* whole UTF-8 characters */
        float mid = x0 + (font_width(f, s, k) + font_width(f, s, nk)) / 2;
        if (mx < mid) break;
        k = nk;
    }
    return o->tv_off[i] + k;
}
static bool tv_word(char c) { return isalnum((unsigned char)c) || c == '_' || c == '.' || (unsigned char)c >= 0x80; }
static void tv_draw(Output *o, App *a, OutLog *l, Rect list) {
    SDL_Renderer *rr = a->renderer; Input *in = &a->in;
    tv_build(o, l);
    draw_rect(rr, list, color_hex(T_EDITOR_BG));
    Font *f = a->font_mono; float lh = font_height(f) + px(a, 2);
    float total = o->tv_n * lh + px(a, 8), maxs = fmaxf(0, total - list.h);
    if (rect_contains(list, in->mouse_x, in->mouse_y) && in->wheel_y != 0) { o->tv_scroll -= in->wheel_y; if (in->wheel_y > 0 && o->follow) { o->follow = false; o->paused = true; } in->wheel_y = 0; }
    if (ui_scrollbar_input(a, &o->tv_sb, list, total, &o->tv_scroll) && o->tv_sb.drag) { o->follow = o->tv_scroll >= maxs - 1; if (!o->follow) o->paused = true; }
    if (o->follow) o->tv_scroll = maxs;
    o->tv_scroll = clampf(o->tv_scroll, 0, maxs);
    int s0 = o->tv_anchor < o->tv_caret ? o->tv_anchor : o->tv_caret, s1 = o->tv_anchor < o->tv_caret ? o->tv_caret : o->tv_anchor;
    if (a->focus == FOCUS_RESULTS && input_key(in, SDLK_C, KM_PRIMARY) && o->tv_text) {   /* the selection, else everything shown */
        if (s1 > s0) { char *t = malloc((size_t)(s1 - s0) + 1); memcpy(t, o->tv_text + s0, (size_t)(s1 - s0)); t[s1 - s0] = 0; SDL_SetClipboardText(t); free(t); app_status(a, "Selection copied"); }
        else { SDL_SetClipboardText(o->tv_text); app_status(a, "Output text copied"); }
    }
    if (a->focus == FOCUS_RESULTS && input_key(in, SDLK_A, KM_PRIMARY) && o->tv_text) { o->tv_anchor = 0; o->tv_caret = (int)strlen(o->tv_text); }
    /* mouse selection: drag; double click a word; triple click a line */
    bool on_sb = o->tv_sb.drag || (list.x + list.w - in->mouse_x < px(a, 14));
    if (rect_contains(list, in->mouse_x, in->mouse_y) && in->mouse_pressed && !in->mouse_consumed && !on_sb) {
        in->mouse_consumed = true; a->focus = FOCUS_RESULTS;
        int at = tv_hit(o, a, list, in->mouse_x, in->mouse_y, f, lh);
        if (in->clicks >= 3) {   /* the line */
            int st = at; while (st > 0 && o->tv_text[st - 1] != '\n') st--;
            int en = at; while (o->tv_text[en] && o->tv_text[en] != '\n') en++;
            o->tv_anchor = st; o->tv_caret = en; o->tv_drag = false;
        } else if (in->clicks == 2) {   /* the word */
            int st = at, en = at; while (st > 0 && tv_word(o->tv_text[st - 1])) st--; while (o->tv_text[en] && tv_word(o->tv_text[en])) en++;
            o->tv_anchor = st; o->tv_caret = en; o->tv_drag = false;
        } else { o->tv_anchor = o->tv_caret = at; o->tv_drag = true; }
        if (o->follow) { o->follow = false; o->paused = true; }   /* selecting stops the tail from moving the text away */
    }
    if (o->tv_drag && in->mouse_down) {
        o->tv_caret = tv_hit(o, a, list, in->mouse_x, in->mouse_y, f, lh);
        if (in->mouse_y < list.y) { o->tv_scroll -= lh; app_wake_in(a, 30); } else if (in->mouse_y > list.y + list.h) { o->tv_scroll += lh; app_wake_in(a, 30); }
        o->tv_scroll = clampf(o->tv_scroll, 0, maxs);
    }
    if (!in->mouse_down) o->tv_drag = false;
    s0 = o->tv_anchor < o->tv_caret ? o->tv_anchor : o->tv_caret; s1 = o->tv_anchor < o->tv_caret ? o->tv_caret : o->tv_anchor;
    clip_push(rr, list);
    int first = (int)(o->tv_scroll / lh); if (first < 0) first = 0;
    int last = (int)((o->tv_scroll + list.h) / lh) + 1; if (last > o->tv_n) last = o->tv_n;
    for (int i = first; i < last; i++) {
        const char *s = o->tv_text + o->tv_off[i]; const char *e = strchr(s, '\n'); int len = e ? (int)(e - s) : (int)strlen(s);
        float y = list.y + px(a, 4) + i * lh - o->tv_scroll;
        int ls = o->tv_off[i], le = ls + len;
        if (s1 > s0 && s0 <= le && s1 >= ls) {   /* this line's part of the selection */
            int a0 = s0 > ls ? s0 - ls : 0, a1 = s1 < le ? s1 - ls : len;
            float x0 = list.x + px(a, 10) + font_width(f, s, a0), x1 = list.x + px(a, 10) + font_width(f, s, a1) + (s1 > le ? px(a, 6) : 0);
            draw_rect(rr, rect_make(x0, y, fmaxf(x1 - x0, px(a, 2)), lh), color_hex(T_ACCENT_DIM));
        }
        uint32_t c = T_TEXT;
        if (len >= 2 && s[0] == '-' && s[1] == '-') c = T_ACCENT;
        else if (len > 14 && !strncmp(s + 14, "error", 5)) c = T_RED;
        else if (len > 14 && !strncmp(s + 14, "warning", 7)) c = T_YELLOW;
        else if (len > 14 && !strncmp(s + 14, "success", 7)) c = T_GREEN;
        else if (len > 0 && s[0] == ' ') c = T_SYN_KEYWORD;
        draw_text_fit(f, rr, s, len, rect_make(list.x + px(a, 10), y, list.w - px(a, 20), lh), color_hex(c), ALIGN_LEFT);
    }
    clip_pop(rr);
    ui_scrollbar_draw(a, &o->tv_sb, list, total, o->tv_scroll);
    ui_record("output:text", list);
}

static void keys(Output *o, App *a, OutLog *l, Rect list) {
    Input *in = &a->in;
    if (a->focus != FOCUS_RESULTS || o->search_focus) return;
    for (int k = 0; k < in->nkeys; k++) {
        KeyPress *kp = &in->keys[k];
        if (kp->used) continue;
        bool prim = kp->mods & KM_PRIMARY, shift = kp->mods & KM_SHIFT;
        int sel = o->sel;
        switch (kp->key) {
        case SDLK_UP: sel = sel < 0 ? o->nrows - 1 : sel > 0 ? sel - 1 : 0; o->follow = false; break;
        case SDLK_DOWN: sel = sel < 0 ? 0 : sel + 1 < o->nrows ? sel + 1 : sel; break;
        case SDLK_HOME: sel = 0; o->follow = false; break;
        case SDLK_END: sel = o->nrows - 1; o->follow = true; break;
        case SDLK_PAGEUP: sel = sel - 20 < 0 ? 0 : sel - 20; o->follow = false; break;
        case SDLK_PAGEDOWN: sel = sel + 20 >= o->nrows ? o->nrows - 1 : sel + 20; break;
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_SPACE:
            if (sel >= 0 && sel < o->nrows) {
                if (o->rows[sel].kind == 0) { OutExec *x = outlog_exec(l, o->rows[sel].exec); if (x) x->collapsed = !x->collapsed; }
                else { uint64_t id = l->ev[o->rows[sel].ev].id; o->expanded_id = o->expanded_id == id ? 0 : id; }
                o->dirty = true;
            }
            break;
        case SDLK_LEFT: case SDLK_RIGHT:
            if (sel >= 0 && sel < o->nrows && o->rows[sel].kind == 0) { OutExec *x = outlog_exec(l, o->rows[sel].exec); if (x) { x->collapsed = kp->key == SDLK_LEFT; o->dirty = true; } }
            break;
        case SDLK_ESCAPE:
            if (o->search.len) { textfield_set(&o->search, ""); o->dirty = true; }
            else if (o->expanded_id) { o->expanded_id = 0; o->dirty = true; }
            else sel = -1;
            break;
        case SDLK_C: if (prim) copy_selection(o, a, l, shift); else continue; break;
        case SDLK_A: if (prim && o->nrows) { o->anchor = 0; sel = o->nrows - 1; shift = true; } else continue; break;   /* select all rows shown */
        case SDLK_G: if (prim) { search_step(o, a, l, shift ? -1 : 1); sel = o->sel; shift = true; } else continue; break;
        case SDLK_F3: search_step(o, a, l, shift ? -1 : 1); sel = o->sel; shift = true; break;
        case SDLK_PERIOD: if (prim) output_cancel(o, a); else continue; break;
        default: continue;
        }
        kp->used = true;
        /* Shift + arrows extend the range from the anchor; any other move starts a new one */
        bool moved = sel != o->sel;
        if (moved && !shift) o->anchor = sel;
        if (moved && shift && o->anchor < 0) o->anchor = o->sel >= 0 ? o->sel : sel;
        if (sel < 0) o->anchor = -1;
        if (moved) {
            o->sel = sel;
            if (sel >= 0 && sel < o->nrows) {
                rebuild(o, a, l, o->cache_w);
                float top = o->tops[sel], bot = o->tops[sel + 1];
                if (top < o->scroll) o->scroll = top;
                if (bot > o->scroll + list.h) o->scroll = bot - list.h;
            }
        }
    }
}

/* ---- preferences (OUT-005) ----------------------------------------------------
 * <prefs>/output-prefs.txt, one key=value per line: wrap, follow, levels (bit mask of shown
 * severities), auto_open_errors. The search text and the execution filter belong to the
 * session and are never restored, so a reopened Output shows every new event; a restored
 * severity filter is announced by the footer ("N of M hidden by filters"). */
/* Path of the preferences file inside the prefs directory. */
static void prefs_path(App *a, char *out, size_t n) { snprintf(out, n, "%soutput-prefs.txt", a->conns ? a->conns->dir : ""); }
/* Bit signature of the saved preferences, to detect changes cheaply. */
static unsigned prefs_signature(const Output *o) { return (unsigned)o->wrap | (unsigned)o->follow << 1 | (unsigned)o->auto_open_errors << 2 | (unsigned)o->text_mode << 30 | o->levels << 3; }

/* Reads output-prefs.txt (missing file = defaults) and marks the prefs as loaded. Values are
 * clamped; a saved empty level mask is ignored. */
static void prefs_load(Output *o, App *a) {
    char path[1200]; prefs_path(a, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char *eq = strchr(line, '='); if (!eq) continue;
            *eq++ = 0; int v = atoi(eq);
            if (!strcmp(line, "wrap")) o->wrap = v != 0;
            else if (!strcmp(line, "text")) o->text_mode = v != 0;
            else if (!strcmp(line, "follow")) o->follow = v != 0;
            else if (!strcmp(line, "auto_open_errors")) o->auto_open_errors = v != 0;
            else if (!strcmp(line, "history_max")) o->history_max = v < 10 ? 10 : v > 100000 ? 100000 : v;
            else if (!strcmp(line, "levels")) { unsigned m = (unsigned)v & ((1u << OL_COUNT) - 1); if (m) o->levels = m; }   /* never restore "show nothing" */
        }
        fclose(f);
        LOG_D("output preferences loaded from %s", path);
    }
    o->prefs_sig = prefs_signature(o);
    o->prefs_loaded = true;
}

/* Writes the file only when a preference actually changed. */
static void prefs_save_if_changed(Output *o, App *a) {
    unsigned sig = prefs_signature(o);
    if (sig == o->prefs_sig) return;
    o->prefs_sig = sig;
    char path[1200]; prefs_path(a, path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) { LOG_W("cannot save output preferences to %s", path); return; }
    fprintf(f, "wrap=%d\nfollow=%d\nlevels=%u\nauto_open_errors=%d\nhistory_max=%d\ntext=%d\n", o->wrap, o->follow, o->levels, o->auto_open_errors, o->history_max, o->text_mode);
    fclose(f);
}

/* ---- history across sessions (OUT-025) -------------------------------------------
 * history.tsv: an optional first line "#expired<TAB>N", then one line per execution:
 * epoch, session, status, ms, statements, SQL (masked; tab / newline / backslash escaped). */
/* Path of history.tsv inside the prefs directory. */
static void history_path(App *a, char *out, size_t n) { snprintf(out, n, "%shistory.tsv", a->conns ? a->conns->dir : ""); }

/* Escapes tab, newline and backslash for one TSV field (drops CR); truncates to fit n. */
static void tsv_escape(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (; *in && o + 3 < n; in++) {
        if (*in == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (*in == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (*in == '\r') continue;
        else if (*in == '\\') { out[o++] = '\\'; out[o++] = '\\'; }
        else out[o++] = *in;
    }
    out[o] = 0;
}
/* Reverses tsv_escape in place. */
static void tsv_unescape(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '\\' && r[1]) { r++; *w++ = *r == 't' ? '\t' : *r == 'n' ? '\n' : *r; }
        else *w++ = *r;
    }
    *w = 0;
}

/* Appends one finished execution, then trims the file to the retention, counting what expired. */
/* Each entry stores redacted SQL (cut at 8000 bytes); a leading \x01 marks that redaction
 * changed something. The trim rewrites the file through a .tmp + rename. */
static void history_append(Output *o, App *a, const char *session, int status, double ms, int nstmts, const char *sql, int len) {
    char path[1200]; history_path(a, path, sizeof path);
    size_t n = len < 0 ? strlen(sql) : (size_t)len;
    if (n > 8000) n = 8000;                               /* very long scripts are kept in part */
    char *raw = malloc(n + 1); memcpy(raw, sql, n); raw[n] = 0;
    char *red = malloc(n * 2 + 64), *esc = malloc(n * 4 + 128);
    outlog_redact(raw, red, n * 2 + 64);
    tsv_escape(red, esc, n * 4 + 128);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%lld\t%s\t%s\t%.1f\t%d\t%s%s\n", (long long)time(NULL), session, outlog_status_name(status), ms, nstmts, strcmp(raw, red) ? "\x01" : "", esc);
        fclose(f);
    } else LOG_W("cannot append to the execution history %s", path);
    free(raw); free(red); free(esc);
    /* retention: keep the last history_max entries */
    f = fopen(path, "r");
    if (!f) return;
    long expired = 0; int lines = 0; char buf[40000];
    char **keep = NULL; int nk = 0, ck = 0;
    while (fgets(buf, sizeof buf, f)) {
        if (!strncmp(buf, "#expired\t", 9)) { expired = atol(buf + 9); continue; }
        if (nk == ck) { ck = ck ? ck * 2 : 256; keep = realloc(keep, sizeof *keep * ck); }
        keep[nk++] = strdup(buf); lines++;
    }
    fclose(f);
    if (lines > o->history_max) {
        int drop = lines - o->history_max;
        char tmp[1220]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
        FILE *w = fopen(tmp, "w");
        if (w) {
            fprintf(w, "#expired\t%ld\n", expired + drop);
            for (int i = drop; i < nk; i++) fputs(keep[i], w);
            fclose(w);
            rename(tmp, path);
        }
    }
    for (int i = 0; i < nk; i++) free(keep[i]);
    free(keep);
}

/* See output.h. Parses history.tsv, skipping entries from this app session (already in
 * memory) and malformed lines; returns at most max entries newest first. Each entry's sql is
 * malloc'd: release with output_history_free. */
int output_history_load(Output *o, App *a, OutHistItem *out, int max, long *expired, int *retention) {
    if (!o->prefs_loaded) prefs_load(o, a);
    *expired = 0; *retention = o->history_max;
    char path[1200]; history_path(a, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    OutHistItem *all = NULL; int n = 0, cap = 0;
    char buf[40000];
    while (fgets(buf, sizeof buf, f)) {
        buf[strcspn(buf, "\n")] = 0;
        if (!strncmp(buf, "#expired\t", 9)) { *expired = atol(buf + 9); continue; }
        char *fld[6]; int k = 0; char *p = buf;
        for (; k < 6 && p; k++) { fld[k] = p; char *t = k < 5 ? strchr(p, '\t') : NULL; if (t) { *t = 0; p = t + 1; } else p = NULL; }
        if (k < 6) continue;
        long long when = atoll(fld[0]);
        if (when >= o->started_at) continue;              /* this session is already in the in-memory list */
        if (n == cap) { cap = cap ? cap * 2 : 64; all = realloc(all, sizeof *all * cap); }
        OutHistItem *h = &all[n++];
        memset(h, 0, sizeof *h);
        h->when = when;
        snprintf(h->session, sizeof h->session, "%s", fld[1]);
        snprintf(h->status, sizeof h->status, "%s", fld[2]);
        h->ms = atof(fld[3]); h->nstmts = atoi(fld[4]);
        char *sql = fld[5];
        if (*sql == 1) { h->masked = true; sql++; }
        tsv_unescape(sql);
        h->sql = strdup(sql);
    }
    fclose(f);
    int m = 0;
    for (int i = n - 1; i >= 0; i--) { if (m < max) out[m++] = all[i]; else free(all[i].sql); }   /* newest first */
    free(all);
    return m;
}

/* See output.h: frees each entry's SQL (not the array itself). */
void output_history_free(OutHistItem *items, int n) { for (int i = 0; i < n; i++) { free(items[i].sql); items[i].sql = NULL; } }

/* "Reset Output layout": the defaults of a first run. */
static void prefs_reset(Output *o) {
    o->wrap = false; o->follow = true; o->paused = false; o->levels = (1u << OL_COUNT) - 1; o->auto_open_errors = true;
    o->exec_filter = 0; o->frozen = false; textfield_set(&o->search, ""); o->dirty = true;
}

/* See output.h. Draws and handles the whole panel inside body for the active connection's log:
 * toolbar, running banner, event list, footer. Saves preferences when they changed. */
void output_update(Output *o, App *a, Rect body) {
    SDL_Renderer *rr = a->renderer;
    Input *in = &a->in;
    OutLog *l = output_current(o, a);
    if (!o->prefs_loaded) prefs_load(o, a);
    bool focused = a->focus == FOCUS_RESULTS;
    if (rect_contains(body, in->mouse_x, in->mouse_y) && in->mouse_pressed && !in->mouse_consumed) { a->focus = FOCUS_RESULTS; focused = true; }

    /* toolbar (OUT-002) */
    Rect tb = rect_make(body.x, body.y, body.w, px(a, TOOLBAR_H));
    draw_rect(rr, tb, color_hex(T_PANEL_ALT));
    draw_rect(rr, rect_make(tb.x, tb.y + tb.h - px(a, 1), tb.w, px(a, 1)), color_hex(T_BORDER));
    float x = tb.x + px(a, 10), by = tb.y + px(a, 5), bh = tb.h - px(a, 10);
    /* the whole toolbar needs ~1,030 pt; a docked Output can be much narrower (side region,
     * split group), so below that the secondary controls move to an "Options" menu */
    bool compact = tb.w < px(a, 1030);
    float sfw = compact ? clampf(tb.w - px(a, 10 + 64 + 120 + 10), px(a, 90), px(a, 220)) : px(a, 160);
    Rect sf = rect_make(x, by, sfw, bh);
    if (ui_textfield(a, &o->search, sf, "Search events (\xE2\x8C\x98" "F)", o->search_focus && focused)) { o->search_focus = true; a->focus = FOCUS_RESULTS; focused = true; }
    if (in->mouse_pressed && !rect_contains(sf, in->mouse_x, in->mouse_y)) o->search_focus = false;
    if (o->search_focus && focused && input_key(in, SDLK_ESCAPE, 0)) { textfield_set(&o->search, ""); o->search_focus = false; o->dirty = true; }
    /* Enter / Shift+Enter in the field step through the matches (OUT-019) */
    if (o->search_focus && focused && o->search.len) {
        if (input_key(in, SDLK_RETURN, KM_SHIFT)) search_step(o, a, l, -1);
        else if (input_key(in, SDLK_RETURN, 0) || input_key(in, SDLK_KP_ENTER, 0)) search_step(o, a, l, 1);
    }
    if (strcmp(o->search.buf, o->last_search) != 0) { snprintf(o->last_search, sizeof o->last_search, "%s", o->search.buf); o->dirty = true; }
    x += sf.w + px(a, 4);
    if (o->search.len) {
        /* counter inside the field: where the selection is among the matches shown, and how
         * many loaded events match at all (filters can hide some of them) */
        int n = 0, tot = 0;
        for (int i = 0; i < l->n; i++) { if (l->ev[i].type == OT_EXEC_STARTED) continue; tot++; OutFilter f = { 0, 0, o->search.buf, false }; if (outlog_visible(l, &l->ev[i], &f)) n++; }
        int shown, pos = search_pos(o, &shown);
        char c[64];
        if (pos) snprintf(c, sizeof c, "%d of %d", pos, shown); else snprintf(c, sizeof c, "%d of %d events", n, tot);
        draw_text_fit(a->font_small, rr, c, -1, rect_make(sf.x + sf.w - px(a, 74), sf.y, px(a, 68), sf.h), color_hex(T_TEXT_DIM), ALIGN_RIGHT);
    }
    /* previous / next match buttons (also Enter, Shift+Enter, Cmd+G, Shift+Cmd+G, F3) */
    {
        Rect bp = rect_make(x, by, px(a, 26), bh), bn = rect_make(x + px(a, 28), by, px(a, 26), bh);
        bool on = o->search.len > 0 && o->nrows > 0;
        if (ui_button(a, bp, "", BTN_QUIET, on)) search_step(o, a, l, -1);
        if (ui_button(a, bn, "", BTN_QUIET, on)) search_step(o, a, l, 1);
        Color cc = color_hex(on ? T_TEXT_MUTED : T_BORDER);
        /* drawn (the font has no arrow glyphs): up = previous match, down = next */
        draw_chevron_back(rr, bp.x + bp.w / 2, bp.y + bp.h / 2, px(a, 8), px(a, 1.5f), true, cc);
        draw_chevron(rr, bn.x + bn.w / 2, bn.y + bn.h / 2, px(a, 8), px(a, 1.5f), true, cc);
        x += px(a, 60);
    }
    if (compact) {
        Rect ob = rect_make(tb.x + tb.w - px(a, 10) - px(a, 112), by, px(a, 112), bh);
        bool filtered = o->exec_filter || o->levels != (1u << OL_COUNT) - 1;
        if (ui_button(a, ob, filtered ? "Options \xE2\x80\xA2    " : "Options    ", filtered ? BTN_SELECTED : BTN_QUIET, true)) {
            static const char *const lv[4] = { "Info", "Success", "Warning", "Error" };
            static const int bits[4] = { OL_INFO, OL_SUCCESS, OL_WARNING, OL_ERROR };
            static char lbl[8][48];
            popup_open(&o->menu, ob.x, ob.y + ob.h);
            o->menu_row = o->sel;
            popup_add_caption(&o->menu, "SHOW LEVELS", 0, false);
            for (int i = 0; i < 4; i++) { snprintf(lbl[i], sizeof lbl[i], "%s  %s", (o->levels >> bits[i]) & 1u ? "\xE2\x9C\x93" : "    ", lv[i]); popup_add(&o->menu, OM_LV_INFO + i, lbl[i], false, true); }
            snprintf(lbl[4], sizeof lbl[4], o->exec_filter ? "All executions (now #%d only)" : "Only the selected execution", o->exec_filter);
            popup_add(&o->menu, OM_EXEC_TOGGLE, lbl[4], true, true);
            snprintf(lbl[5], sizeof lbl[5], "%s  Wrap", o->wrap ? "\xE2\x9C\x93" : "    ");
            snprintf(lbl[6], sizeof lbl[6], "%s  Follow tail", o->follow ? "\xE2\x9C\x93" : "    ");
            popup_add(&o->menu, OM_WRAP, lbl[5], true, true);
            popup_add(&o->menu, OM_FOLLOW, lbl[6], false, true);
            snprintf(lbl[7], sizeof lbl[7], "%s  Text view", o->text_mode ? "\xE2\x9C\x93" : "    ");
            popup_add(&o->menu, OM_TEXT, lbl[7], false, true);
            popup_add(&o->menu, OM_COPY_DETAILS, "Copy selection", true, o->sel >= 0);
            popup_add(&o->menu, OM_EXPORT, "Export\xE2\x80\xA6", false, l->n > 0);
            popup_add(&o->menu, OM_CLEAR, "Clear output", false, l->n > 0);
        }
        draw_chevron(rr, ob.x + ob.w - px(a, 16), ob.y + ob.h / 2, px(a, 8), px(a, 1.5f), true, color_hex(T_TEXT_MUTED));
    } else {
        /* level checkboxes (mock-up): each one hides a level without dropping anything */
        static const char *const lv_names[4] = { "Info", "Success", "Warning", "Error" };
        static const int lv_bits[4] = { OL_INFO, OL_SUCCESS, OL_WARNING, OL_ERROR };
        for (int i = 0; i < 4; i++) {
            float cw2 = font_width(a->font_ui, lv_names[i], -1) + px(a, 26);
            if (ui_checkbox(a, rect_make(x, by, cw2, bh), lv_names[i], (o->levels >> lv_bits[i]) & 1u, true)) { o->levels ^= 1u << lv_bits[i]; o->dirty = true; }
            x += cw2 + px(a, 2);
        }
        x += px(a, 8);
        char xl[40]; snprintf(xl, sizeof xl, o->exec_filter ? "Execution #%d  \xC3\x97" : "All executions    ", o->exec_filter);
        float xw = font_width(a->font_ui, xl, -1) + px(a, 22);
        Rect xr = rect_make(x, by, xw, bh);
        bool xclick = ui_button(a, xr, xl, o->exec_filter ? BTN_SELECTED : BTN_QUIET, true);
        if (!o->exec_filter) draw_chevron(rr, xr.x + xr.w - px(a, 14), xr.y + xr.h / 2, px(a, 8), px(a, 1.5f), true, color_hex(T_TEXT_MUTED));
        if (xclick) {
            if (o->exec_filter) o->exec_filter = 0;
            else if (o->sel >= 0 && o->sel < o->nrows) o->exec_filter = o->rows[o->sel].exec;
            else app_status(a, "Select an execution or event first, then filter to it");
            o->dirty = true;
        }
        x += xw + px(a, 6);
        if (ui_button(a, rect_make(x, by, px(a, 58), bh), "Wrap", o->wrap ? BTN_SELECTED : BTN_QUIET, true)) { o->wrap = !o->wrap; o->dirty = true; }
        x += px(a, 64);
        if (ui_button(a, rect_make(x, by, px(a, 58), bh), "Text", o->text_mode ? BTN_SELECTED : BTN_QUIET, true)) { o->text_mode = !o->text_mode; o->tv_sig = 0; }
        x += px(a, 64);
        const char *fl_lbl = o->follow ? "Follow tail" : o->paused ? "Follow tail \xC2\xB7 paused" : "Follow tail";
        float flw = font_width(a->font_ui, fl_lbl, -1) + px(a, 22);
        if (ui_button(a, rect_make(x, by, flw, bh), fl_lbl, o->follow ? BTN_SELECTED : BTN_QUIET, true)) { o->follow = !o->follow; o->paused = false; }
        x += flw + px(a, 6);
        float rx = tb.x + tb.w - px(a, 10);
        Rect bc = rect_make(rx - px(a, 96), by, px(a, 96), bh); rx -= px(a, 102);
        Rect be = rect_make(rx - px(a, 76), by, px(a, 76), bh); rx -= px(a, 82);
        Rect bcp = rect_make(rx - px(a, 60), by, px(a, 60), bh); rx -= px(a, 66);
        if (ui_button(a, bc, "Clear output", BTN_QUIET, l->n > 0)) { outlog_clear(l); o->sel = o->anchor = -1; o->expanded_id = 0; o->dirty = true; app_status(a, "Output view cleared; executions already recorded are not undone"); }
        if (ui_button(a, be, "Export\xE2\x80\xA6", BTN_QUIET, l->n > 0)) app_export_results(a, 1);
        if (o->text_mode) { if (ui_button(a, bcp, "Copy all", BTN_QUIET, o->tv_text && o->tv_text[0])) { SDL_SetClipboardText(o->tv_text); app_status(a, "Output text copied (secrets in SQL are redacted)"); } }
        else if (ui_button(a, bcp, "Copy", BTN_QUIET, o->sel >= 0)) copy_selection(o, a, l, true);
    }
    body.y += tb.h; body.h -= tb.h;

    /* running banner (OUT-004 / OUT-018): the active session's execution, else the first one;
     * with several sessions running, how many */
    OutJob *bj = shown_job(o, a);
    if (bj) {
        ExecJob *jb = bj->j;
        Rect bn = rect_make(body.x, body.y, body.w, px(a, BANNER_H));
        bool cancelling = exec_cancelling(jb);
        draw_rect(rr, bn, color_alpha(color_hex(cancelling ? T_YELLOW : T_ACCENT), 26));
        draw_rect(rr, rect_make(bn.x, bn.y + bn.h - px(a, 1), bn.w, px(a, 1)), color_hex(T_BORDER));
        OutLog *bl = job_log(o, bj);
        OutExec *x2 = bl ? outlog_exec(bl, jb->exec_id) : NULL;
        char t[300], more[48] = "";
        int nrun = output_running_count(o);
        if (nrun > 1) snprintf(more, sizeof more, "  \xC2\xB7  %d running", nrun);
        int cur = __atomic_load_n(&jb->cur, __ATOMIC_SEQ_CST);
        if (cancelling) snprintf(t, sizeof t, "!  Cancelling #%d on %s: waiting for %s to stop the statement\xE2\x80\xA6  %.1f s%s", jb->exec_id, bj->conn_name, jb->db->drv->name, exec_elapsed_ms(jb) / 1000.0, more);
        else snprintf(t, sizeof t, "\xE2\x80\xA2  Running #%d on %s: statement %d of %d  \xC2\xB7  %s  \xC2\xB7  %.1f s elapsed%s", jb->exec_id, bj->conn_name, cur + 1, jb->n, x2 ? x2->summary : "", exec_elapsed_ms(jb) / 1000.0, more);
        draw_text_fit(a->font_ui, rr, t, -1, rect_make(bn.x + px(a, 14), bn.y, bn.w - px(a, 140), bn.h), color_hex(T_TEXT), ALIGN_LEFT);
        bool can = jb->db->drv->cancel != NULL;
        if (ui_button(a, rect_make(bn.x + bn.w - px(a, 110), bn.y + px(a, 3), px(a, 96), bn.h - px(a, 6)), cancelling ? "Cancelling\xE2\x80\xA6" : can ? "Cancel  \xE2\x8C\x98." : "Stop script", BTN_DANGER, !cancelling)) output_cancel_exec(o, a, jb->exec_id);
        body.y += bn.h; body.h -= bn.h;
        app_wake_in(a, 100);
    }

    /* Layout: the footer takes the bottom strip, the list the rest; then build rows, honour a
     * pending "show execution" request and clamp / follow the scroll position. */
    /* footer */
    Rect ft = rect_make(body.x, body.y + body.h - px(a, FOOTER_H), body.w, px(a, FOOTER_H));
    body.h -= ft.h;
    Rect list = body;
    o->list_rc = list;
    if (o->text_mode) {   /* plain text instead of the structured list (same events, filters and search) */
        tv_draw(o, a, l, list);
        char ft_t[200]; snprintf(ft_t, sizeof ft_t, "Text view \xC2\xB7 %d line(s) \xC2\xB7 select with the mouse; \xE2\x8C\x98" "C copies the selection (everything shown when nothing is selected)", o->tv_n);
        draw_rect(rr, ft, color_hex(T_PANEL_ALT));
        draw_text_fit(a->font_small, rr, ft_t, -1, rect_make(ft.x + px(a, 10), ft.y, ft.w - px(a, 20), ft.h), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        prefs_save_if_changed(o, a);   /* the text-view choice persists like wrap / follow */
        return;
    }
    rebuild(o, a, l, list.w);
    if (o->goto_exec) {
        for (int r = 0; r < o->nrows; r++) if (o->rows[r].kind == 0 && o->rows[r].exec == o->goto_exec) { o->sel = r; o->scroll = o->tops[r]; o->follow = false; break; }
        o->goto_exec = 0;
    }
    float total = o->nrows ? o->tops[o->nrows] : 0;
    float maxs = fmaxf(0, total - list.h);
    if (rect_contains(list, in->mouse_x, in->mouse_y) && in->wheel_y != 0) {
        o->scroll -= in->wheel_y;
        if (in->wheel_y > 0 && o->follow) { o->follow = false; o->paused = true; }   /* scrolling up pauses following (OUT-023) */
        in->wheel_y = 0;
    }
    if (ui_scrollbar_input(a, &o->sb, list, total, &o->scroll) && o->sb.drag) { o->follow = o->scroll >= maxs - 1; if (!o->follow) o->paused = true; }
    if (o->follow) o->scroll = maxs;
    o->scroll = clampf(o->scroll, 0, maxs);
    /* while following the tail, start the viewport on a row boundary: no row is ever cut
     * at the top under the banner (a little empty space may show below the last row) */
    if (o->follow && o->nrows && total > list.h) {
        int f = row_at(o, o->scroll);
        if (f >= 0 && o->tops[f] < o->scroll - 0.5f) o->scroll = o->tops[f + 1];
    }
    keys(o, a, l, list);

    draw_rect(rr, list, color_hex(T_PANEL));
    if (!o->nrows) {
        const char *t1 = l->n ? "No event matches the current filters" : "No execution output yet";
        const char *t2 = l->n ? "Clear the search or the severity / execution filter to see the recorded events" : "Run a statement to see messages, warnings, errors and timings here";
        draw_text_fit(a->font_ui, rr, t1, -1, rect_make(list.x, list.y + list.h / 2 - px(a, 24), list.w, px(a, 24)), color_hex(T_TEXT_MUTED), ALIGN_CENTER);
        draw_text_fit(a->font_small, rr, t2, -1, rect_make(list.x, list.y + list.h / 2 + px(a, 2), list.w, px(a, 20)), color_hex(T_TEXT_DIM), ALIGN_CENTER);
    } else {
        /* Visible rows only (virtualised): from the first row at the scroll offset until past the
         * bottom edge. */
        clip_push(rr, list);
        int first = row_at(o, o->scroll); if (first < 0) first = 0;
        float rh = px(a, ROW_H), tw = px(a, 92), gx = list.x + px(a, 12);
        bool hover_any = false;
        for (int r = first; r < o->nrows; r++) {
            float y = list.y + o->tops[r] - o->scroll, h = o->tops[r + 1] - o->tops[r];
            if (y > list.y + list.h) break;
            Row *row = &o->rows[r];
            Rect rr_ = rect_make(list.x, y, list.w, h);
            bool hover = rect_contains(rr_, in->mouse_x, in->mouse_y);
            if (hover) hover_any = true;
            int slo = -1, shi = -1; sel_range(o, &slo, &shi);
            if (r >= slo && r <= shi) draw_rect(rr, rr_, color_hex(T_SELECTED));
            else if (hover) draw_rect(rr, rr_, color_hex(T_HOVER));
            /* Execution header: chevron, start time, "#id summary", and status / counts on the right. */
            if (row->kind == 0) {
                OutExec *x = outlog_exec(l, row->exec);
                char tm[32], head[220], right[160];
                if (r > 0) draw_rect(rr, rect_make(list.x + px(a, 12), y, list.w - px(a, 24), px(a, 1)), color_hex(T_BORDER));   /* divider between executions */
                fmt_clock(x->started, false, tm, sizeof tm);
                bool empty_group = !x->collapsed && (r + 1 >= o->nrows || o->rows[r + 1].kind == 0);   /* nothing visible under it */
                draw_chevron(rr, gx + px(a, 6), y + rh / 2, px(a, 8), px(a, 1.5f), !x->collapsed, color_hex(empty_group ? T_BORDER : T_TEXT_MUTED));
                draw_text_fit(a->font_small, rr, tm, -1, rect_make(gx + px(a, 18), y, tw, rh), color_hex(T_TEXT_DIM), ALIGN_LEFT);
                snprintf(head, sizeof head, "#%d  %s", x->id, x->summary);
                draw_text_fit(a->font_ui, rr, head, -1, rect_make(gx + px(a, 18) + tw, y, list.w - tw - px(a, 400), rh), color_hex(T_TEXT), ALIGN_LEFT);
                int counted = x->ok + x->failed + x->skipped;
                char dur[24];
                if (x->status == ES_RUNNING || x->status == ES_CANCELLING) snprintf(right, sizeof right, "%s  \xC2\xB7  %d of %d done  \xC2\xB7  %.1f s", outlog_status_name(x->status), counted, x->nstmts, job_of_exec(o, x->id) ? exec_elapsed_ms(job_of_exec(o, x->id)->j) / 1000.0 : 0);
                else if (x->nstmts > 1) snprintf(right, sizeof right, "%s  \xC2\xB7  %d of %d ok  \xC2\xB7  %d failed  \xC2\xB7  %d skipped  \xC2\xB7  %s", outlog_status_name(x->status), x->ok, x->nstmts, x->failed, x->skipped, fmt_duration(dur, sizeof dur, x->ms));
                else snprintf(right, sizeof right, "%s  \xC2\xB7  1 statement  \xC2\xB7  %s", outlog_status_name(x->status), fmt_duration(dur, sizeof dur, x->ms));
                char hid[32] = "";
                if (row->hidden) snprintf(hid, sizeof hid, "  \xC2\xB7  %d hidden", row->hidden);
                size_t rl = strlen(right); snprintf(right + rl, sizeof right - rl, "%s", hid);
                float rw = font_width(a->font_small, right, -1);
                Rect rr2 = rect_make(list.x + list.w - px(a, 12) - rw, y, rw + px(a, 2), rh);
                draw_text_fit(a->font_small, rr, right, -1, rr2, color_hex(status_color(x->status)), ALIGN_RIGHT);
                draw_text(a->font_small, rr, status_glyph(x->status), rr2.x - px(a, 14), y + (rh - font_height(a->font_small)) / 2, color_hex(status_color(x->status)));
            /* Event row: time, level glyph, type label, message, metadata and action buttons, then the
             * SQL line and the expanded details box when applicable. */
            } else {
                const OutEvent *e = &l->ev[row->ev];
                char tm[32];
                fmt_clock(e->ts, true, tm, sizeof tm);
                draw_text_fit(a->font_small, rr, tm, -1, rect_make(gx + px(a, 18), y, tw, rh), color_hex(T_TEXT_DIM), ALIGN_LEFT);
                bool started = e->type == OT_STMT_STARTED;
                draw_text(a->font_ui, rr, started ? "\xE2\x80\xBA" : level_glyph(e->level), gx + px(a, 20) + tw, y + (rh - font_height(a->font_ui)) / 2, color_hex(started ? T_ACCENT : level_color(e->level)));
                /* uppercase type label (mock-up), then the message; metadata right-aligned */
                const char *tl = type_label(e);
                float tlw = px(a, 96);
                draw_text_fit(a->font_small, rr, tl, -1, rect_make(gx + px(a, 40) + tw, y, tlw, rh), color_hex(level_color(e->level)), ALIGN_LEFT);
                /* "Go to SQL" only for statements that came from the SQL editor (table-editor
                 * statements carry sql_start -1: their SQL was never in the editor) */
                bool from_editor = e->sql && e->sql_start >= 0;
                bool has_btn = (e->result_tab && e->type == OT_RESULT_SET) || (e->type == OT_STMT_FAILED && from_editor);
                char meta[120] = "";
                if (e->err_state) snprintf(meta, sizeof meta, "SQLSTATE %s%s", e->err_state, "");
                if (e->err_line >= 0) { size_t ml = strlen(meta); snprintf(meta + ml, sizeof meta - ml, "%sline %d", ml ? "  \xC2\xB7  " : "", e->err_line); }
                if (!meta[0] && e->exec_ms >= 0) fmt_duration(meta, sizeof meta, e->exec_ms);
                if (!meta[0] && e->type == OT_TX_CHANGED) snprintf(meta, sizeof meta, "%s", e->tx_state == TXS_OPEN ? "open" : "ended");
                float metaw = meta[0] ? font_width(a->font_small, meta, -1) + px(a, 10) : 0;
                /* "Fix with AI" beside "Go to SQL" when Lume Intelligence is on (roadmap item 12) */
                const Command *fixc = e->type == OT_STMT_FAILED && from_editor ? commands_find("intelligence.fix") : NULL;
                char fixwhy[80]; bool fix_on = fixc && (!fixc->enabled || fixc->enabled(commands_ctx(), fixwhy, sizeof fixwhy));
                float right_edge = list.x + list.w - px(a, 12) - (has_btn ? px(a, 132) : 0) - (fix_on ? px(a, 112) : 0);
                if (meta[0]) draw_text_fit(a->font_small, rr, meta, -1, rect_make(right_edge - metaw, y, metaw, rh), color_hex(T_TEXT_DIM), ALIGN_RIGHT);
                Rect mr = rect_make(gx + px(a, 44) + tw + tlw, y, right_edge - metaw - (gx + px(a, 44) + tw + tlw) - px(a, 8), o->wrap ? rh * wrap_lines(a, a->font_ui, e->message, list.w - px(a, 150)) : rh);
                Color mc = color_hex(e->level == OL_ERROR ? T_RED : e->level == OL_WARNING ? T_YELLOW : e->level == OL_SUCCESS ? T_TEXT : T_TEXT_MUTED);
                /* search highlight (OUT-019): the matched text on a single line; with wrap on,
                 * a marker in the gutter, since the wrapped line breaks are not known here */
                if (o->search.len) {
                    const char *hit = strcasestr(e->message, o->search.buf);
                    if (hit && !(o->wrap && mr.h > rh)) {
                        float hx = mr.x + font_width(a->font_ui, e->message, (int)(hit - e->message));
                        float hw = font_width(a->font_ui, hit, (int)o->search.len);
                        if (hx + hw <= mr.x + mr.w) draw_rrect(rr, rect_make(hx - px(a, 1), y + px(a, 3), hw + px(a, 2), rh - px(a, 6)), px(a, 2), color_alpha(color_hex(T_YELLOW), 70));
                    } else if (hit) draw_rect(rr, rect_make(list.x + px(a, 2), y + px(a, 3), px(a, 3), mr.h - px(a, 6)), color_hex(T_YELLOW));
                }
                if (o->wrap && mr.h > rh) results_draw_wrapped(a->font_ui, rr, e->message, rect_make(mr.x, mr.y + px(a, 3), mr.w, mr.h), mc, 0, true);
                else draw_text_fit(a->font_ui, rr, e->message, -1, mr, mc, ALIGN_LEFT);
                if (sql_line(e)) {
                    /* the statement as executed, one line, keyword colour like the editor */
                    char one[400]; size_t op = 0;
                    for (const char *p = e->sql; *p && op < sizeof one - 2; p++) one[op++] = (*p == '\n' || *p == '\r' || *p == '\t') ? ' ' : *p;
                    one[op] = 0;
                    draw_text_fit(a->font_mono, rr, one, -1, rect_make(gx + px(a, 44) + tw, y + rh, list.w - tw - px(a, 70), rh), color_hex(T_SYN_KEYWORD), ALIGN_LEFT);
                }
                if (e->result_tab && e->type == OT_RESULT_SET) {
                    Rect ob = rect_make(list.x + list.w - px(a, 132), y + px(a, 2), px(a, 120), rh - px(a, 4));
                    if (ui_button(a, ob, "Open results", BTN_NORMAL, true)) open_results(o, a, l, r);
                }
                if (e->type == OT_STMT_FAILED && from_editor) {
                    Rect gb = rect_make(list.x + list.w - px(a, 132), y + px(a, 2), px(a, 120), rh - px(a, 4));
                    bool same = editor_revision(a->editor) == e->editor_rev;
                    if (ui_button(a, gb, same ? "Go to SQL" : "Go to SQL (changed)", BTN_NORMAL, true)) goto_sql(o, a, l, r);
                    if (fix_on) { Rect fb = rect_make(gb.x - px(a, 112), gb.y, px(a, 104), gb.h); ui_record("output:fix-with-ai", fb); if (ui_button(a, fb, "Fix with AI", BTN_NORMAL, true)) commands_invoke(a, "intelligence.fix"); }
                }
                if (o->search.len && !strcasestr(e->message, o->search.buf) && !sql_line(e)) {
                    float mw = font_width(a->font_ui, e->message, -1);
                    draw_text_fit(a->font_small, rr, "\xC2\xB7 matched in SQL / details", -1, rect_make(mr.x + mw + px(a, 8), y, px(a, 200), rh), color_hex(T_TEXT_DIM), ALIGN_LEFT);
                }
                if (e->id == o->expanded_id) {
                    char *d = outlog_event_text(l, e, true);
                    float dy = y + mr.h + (sql_line(e) ? rh : 0) + px(a, 4);
                    Rect box = rect_make(gx + px(a, 18) + tw, dy, list.w - tw - px(a, 50), h - mr.h - px(a, 8));
                    draw_rrect(rr, box, px(a, 4), color_hex(T_BG));
                    float ly = box.y + px(a, 4);
                    for (char *p = d, *nl; p && *p; p = nl ? nl + 1 : NULL) {
                        nl = strchr(p, '\n'); if (nl) *nl = 0;
                        draw_text_fit(a->font_mono, rr, p, -1, rect_make(box.x + px(a, 10), ly, box.w - px(a, 20), px(a, 18)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
                        ly += px(a, 18);
                    }
                    free(d);
                }
            }
            /* Mouse: click selects (Shift extends), double-click / chevron toggles a group or expands an
             * event, right-click opens the context menu. */
            if (hover && in->mouse_pressed && !in->mouse_consumed) {
                in->mouse_consumed = true;
                /* Shift+click extends the range from the anchor (OUT-021); a plain click starts over */
                if ((app_modstate(a) & SDL_KMOD_SHIFT) && o->sel >= 0) { if (o->anchor < 0) o->anchor = o->sel; }
                else o->anchor = r;
                o->sel = r; o->search_focus = false;
                if (row->kind == 0 && (in->clicks >= 2 || in->mouse_x < gx + px(a, 18))) { OutExec *x = outlog_exec(l, row->exec); x->collapsed = !x->collapsed; o->dirty = true; }
                else if (row->kind == 1 && in->clicks >= 2) { uint64_t id = l->ev[row->ev].id; o->expanded_id = o->expanded_id == id ? 0 : id; o->dirty = true; }
            }
            if (hover && in->mouse_rpressed && !in->mouse_consumed) {
                in->mouse_consumed = true;
                int lo2, hi2;
                if (!(sel_range(o, &lo2, &hi2) && r >= lo2 && r <= hi2)) { o->sel = o->anchor = r; }   /* right-click inside a range keeps it */
                context_menu(o, a, l, r, in->mouse_x, in->mouse_y);
            }
        }
        (void)hover_any;
        clip_pop(rr);
        ui_scrollbar_draw(a, &o->sb, list, total, o->scroll);
        if (in->mouse_rpressed && !in->mouse_consumed && rect_contains(list, in->mouse_x, in->mouse_y)) { in->mouse_consumed = true; context_menu(o, a, l, -1, in->mouse_x, in->mouse_y); }
    }

    /* footer (OUT-004 / OUT-024) */
    draw_rect(rr, ft, color_hex(T_PANEL_ALT));
    draw_rect(rr, rect_make(ft.x, ft.y, ft.w, px(a, 1)), color_hex(T_BORDER));
    /* Footer text: counts, errors / warnings, retention, job state and what the filters hide. */
    int execs, errors, warnings; outlog_counts(l, &execs, &errors, &warnings);
    int hidden = 0, renderable = 0;
    for (int i = 0; i < l->n; i++) { if (l->ev[i].type == OT_EXEC_STARTED) continue; renderable++; if (!pass(o, &l->ev[i])) hidden++; }
    bool by_search = o->search.len > 0, by_filter = o->exec_filter || o->levels != (1u << OL_COUNT) - 1;
    int stmts = 0; for (int i = 0; i < l->nex; i++) stmts += l->ex[i].nstmts;
    char fl[300], et[40];
    float fx = ft.x + px(a, 14);
    draw_text(a->font_small, rr, errors ? "\xC3\x97" : "\xE2\x9C\x93", fx, ft.y + (ft.h - font_height(a->font_small)) / 2, color_hex(errors ? T_RED : T_GREEN));
    fx += px(a, 16);
    snprintf(fl, sizeof fl, "%d execution%s  \xC2\xB7  %d statement%s  \xC2\xB7  ", execs, execs == 1 ? "" : "s", stmts, stmts == 1 ? "" : "s");
    float w1 = font_width(a->font_small, fl, -1);
    draw_text_fit(a->font_small, rr, fl, -1, rect_make(fx, ft.y, w1 + px(a, 2), ft.h), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    fx += w1;
    snprintf(et, sizeof et, "%d error%s", errors, errors == 1 ? "" : "s");
    float w2 = font_width(a->font_small, et, -1);
    draw_text_fit(a->font_small, rr, et, -1, rect_make(fx, ft.y, w2 + px(a, 2), ft.h), color_hex(errors ? T_RED : T_TEXT_MUTED), ALIGN_LEFT);
    fx += w2;
    fl[0] = 0;
    if (warnings) snprintf(fl, sizeof fl, "  \xC2\xB7  %d warning%s", warnings, warnings == 1 ? "" : "s");
    size_t fl2 = strlen(fl);
    snprintf(fl + fl2, sizeof fl - fl2, "%s%s%s", l->dropped_events ? "  \xC2\xB7  older executions dropped by retention" : "", o->njobs ? (shown_job(o, a) && exec_cancelling(shown_job(o, a)->j) ? "  \xC2\xB7  cancelling" : "  \xC2\xB7  running") : "",
             o->paused && !o->follow ? "  \xC2\xB7  follow tail paused (End resumes)" : "");
    if (hidden) { fl2 = strlen(fl); snprintf(fl + fl2, sizeof fl - fl2, "  \xC2\xB7  %d of %d hidden by %s", hidden, renderable, by_search && by_filter ? "search and filters" : by_search ? "search" : "filters"); }
    /* narrow panel: the session label gives way and the pause button takes the right edge */
    bool narrow = ft.w < px(a, 640);
    float pause_w = px(a, 130), right_res = narrow ? pause_w + px(a, 8) : px(a, 260) + pause_w;
    draw_text_fit(a->font_small, rr, fl, -1, rect_make(fx, ft.y, ft.x + ft.w - right_res - fx, ft.h), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    char sess[120]; snprintf(sess, sizeof sess, "Output  \xC2\xB7  %s", l->session_name[0] ? l->session_name : "no connection");
    if (!narrow) draw_text_fit(a->font_small, rr, sess, -1, rect_make(ft.x + ft.w - px(a, 244), ft.y, px(a, 230), ft.h), color_hex(T_TEXT_DIM), ALIGN_RIGHT);
    /* Pause view (OUT-023): freezes what the list shows, never the capture; says how many
     * events arrived meanwhile */
    {
        int fresh = 0;
        if (o->frozen) for (int i = 0; i < l->n; i++) if (l->ev[i].id > o->frozen_id && l->ev[i].type != OT_EXEC_STARTED) fresh++;
        char pl[48];
        if (o->frozen) snprintf(pl, sizeof pl, fresh ? "Resume view \xC2\xB7 %d new" : "Resume view", fresh); else snprintf(pl, sizeof pl, "Pause view");
        float pw = font_width(a->font_small, pl, -1) + px(a, 20);
        Rect pb = rect_make(ft.x + ft.w - (narrow ? px(a, 6) : px(a, 252)) - pw, ft.y + px(a, 2), pw, ft.h - px(a, 4));
        if (ui_button(a, pb, pl, o->frozen ? BTN_SELECTED : BTN_QUIET, true)) {
            o->frozen = !o->frozen;
            if (o->frozen) { o->frozen_id = 0; for (int i = 0; i < l->n; i++) if (l->ev[i].id > o->frozen_id) o->frozen_id = l->ev[i].id; }
            o->dirty = true;
        }
        if (o->frozen && fresh) app_wake_in(a, 250);
    }
    prefs_save_if_changed(o, a);
}

/* ---- object editor events (OBJ-061): validate / deploy / compile, one execution each ---- */
static OutLog *obj_log(Output *o, App *a, Connection *c) { return &session_for(o, c ? c->id : session_id(a), c ? c->name : "no connection")->log; }
int output_obj_begin(Output *o, App *a, Connection *c, const char *title, int nstmts) {
    OutLog *l = obj_log(o, a, c);
    int x = outlog_exec_begin(l, title, -1, 0, 0);
    OutExec *xe = outlog_exec(l, x);
    if (xe) xe->nstmts = nstmts;
    o->dirty = true;
    return x;
}
void output_obj_event(Output *o, App *a, Connection *c, int x, int stmt, int level, const char *msg, const char *sql) {
    OutLog *l = obj_log(o, a, c);
    int type = level == OL_ERROR ? OT_STMT_FAILED : stmt > 0 ? OT_STMT_COMPLETED : OT_DB_MESSAGE;
    if (sql) outlog_push_sql(l, x, stmt, type, level, msg, sql, -1, -1, 0);
    else outlog_push(l, x, stmt, type, level, msg);
    OutExec *xe = outlog_exec(l, x);
    if (xe && stmt > 0) { if (level == OL_ERROR) xe->failed++; else xe->ok++; }
    o->dirty = true;
}
void output_obj_finish(Output *o, App *a, Connection *c, int x, int status, double ms) {
    outlog_exec_finish(obj_log(o, a, c), x, status, ms);
    o->dirty = true;
}
void output_note(Output *o, App *a, Connection *c, int level, const char *msg) {
    if (!o || !msg || !msg[0]) return;
    outlog_note(obj_log(o, a, c), level, msg);
    o->dirty = true;
}
void output_notes_dump(Output *o, App *a, Connection *c) {
    OutLog *l = obj_log(o, a, c);
    int n = 0;
    for (int i = 0; i < l->n; i++) {
        const OutEvent *e = &l->ev[i];
        if (e->type != OT_NOTE) continue;
        char red[600]; outlog_redact(e->message, red, sizeof red);
        printf("outnote %s %s\n", outlog_level_name(e->level), red); n++;
    }
    printf("outnotes %d in %s (unseen warnings/errors: %d)\n", n, l->session_name, unseen_in(l));
    fflush(stdout);
}
int output_refused(Output *o, App *a, Connection *c, const char *sql, int len, const char *why) {
    if (!o) return 0;
    OutLog *l = obj_log(o, a, c);
    int x = outlog_exec_begin(l, sql ? sql : "", sql ? len : 0, 0, -1);
    OutExec *xe = outlog_exec(l, x);
    if (xe) { xe->nstmts = 1; xe->skipped = 1; }
    outlog_push(l, x, 0, OT_DB_MESSAGE, OL_WARNING, why);
    outlog_exec_finish(l, x, ES_SKIPPED, 0);
    o->dirty = true;
    return x;
}

/* Cmd+C with text selected in the text view: the selection goes to the clipboard (true when copied) */
bool output_copy_text_selection(Output *o, App *a) {
    if (!o || !o->text_mode || !o->tv_text || o->tv_anchor == o->tv_caret) return false;
    int s0 = o->tv_anchor < o->tv_caret ? o->tv_anchor : o->tv_caret, s1 = o->tv_anchor < o->tv_caret ? o->tv_caret : o->tv_anchor;
    if (s1 > (int)strlen(o->tv_text)) return false;
    char *t = malloc((size_t)(s1 - s0) + 1); memcpy(t, o->tv_text + s0, (size_t)(s1 - s0)); t[s1 - s0] = 0;
    SDL_SetClipboardText(t); free(t); app_status(a, "Selection copied");
    return true;
}

void output_get_prefs(Output *o, int *history_max, bool *auto_open_errors) { *history_max = o->history_max; *auto_open_errors = o->auto_open_errors; }
void output_set_prefs(Output *o, App *a, int history_max, bool auto_open_errors) {
    if (history_max < 10) history_max = 10; if (history_max > 100000) history_max = 100000;
    o->history_max = history_max; o->auto_open_errors = auto_open_errors;
    prefs_save_if_changed(o, a);
}
