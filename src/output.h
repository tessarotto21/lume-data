/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include "lume.h"
#include "exec.h"
#include "outlog.h"

/* Execution Output panel (docs/execution-output.md): one OutLog per connection, the
 * running job, and the view (toolbar, grouped virtualised list, footer, context menu).
 * results.c draws it in place of the grid when the Output tab is selected. */
/* Allocates the panel with first-run defaults; free it with output_destroy. Output itself also
 * runs editor executions (see output_run) and keeps the per-connection logs.
 * All functions here are UI-thread only. */
Output *output_create(void);
/* Frees the panel, its logs and any running job. NULL-safe. */
void output_destroy(Output *o);
OutLog *output_current(Output *o, App *a);
void output_get_prefs(Output *o, int *history_max, bool *auto_open_errors);   /* Settings */
void output_set_prefs(Output *o, App *a, int history_max, bool auto_open_errors);
bool output_copy_text_selection(Output *o, App *a);   /* Cmd+C over a text-view selection */            /* log of the active connection (created on demand) */
void output_pre_update(Output *o, App *a);             /* own menus / dialogs take input first */
/* Draws and handles the panel (toolbar, banner, event list, footer) inside body. */
void output_update(Output *o, App *a, Rect body);
/* Draws the panel's popup menu on top of everything (call after the main UI). */
void output_draw_overlay(Output *o, App *a);
/* True while the panel's popup menu is open and captures input. */
bool output_modal_open(Output *o);
/* True while the search field has keyboard focus (global shortcuts should not act). */
bool output_text_focus(Output *o);
/* Focuses the search field (Cmd+F while the Output tab is shown). */
void output_find(Output *o);
/* Keyboard actions routed here while the Output tab is shown (the same shortcuts act on the
 * result grid otherwise): copy the selected rows (messages, or details with Shift), select
 * every row shown. */
void output_copy(Output *o, App *a, bool details);
void output_select_all(Output *o);
/* Test harness: centre of the context-menu item whose label starts with `label`, in window
 * points; false when no such item is open. */
bool output_menu_pos(Output *o, App *a, const char *label, float *x, float *y);   /* test harness */
/* Table editor transaction (OUT-017): records an apply in the same Output log as editor runs,
 * as one execution on the connection that owns `db`: one event per statement that ran, the
 * failure and the rollback as separate linked events, and the outcome the driver reported
 * (commit confirmed / rolled back / unknown). `status` and `res` come from edit_apply. */
/* OUT-026: CPU time of the first draw of the result tab `tab_exec` produced by execution
 * `out_exec`; stored on that execution's result_set event. */
void output_note_render(Output *o, int out_exec, int tab_exec, double ms);
/* Execution history across sessions (OUT-025): every finished editor execution is appended to
 * <prefs>/history.tsv with its date, session, status, duration and SQL (masked by the same
 * redaction as copy / export). Retention: "history_max" in output-prefs.txt (default 500);
 * what is trimmed is counted, never recoverable. output_history_load returns entries from
 * earlier app sessions, newest first. */
typedef struct OutHistItem { long long when; char session[64]; char status[16]; double ms; int nstmts; char *sql; bool masked; } OutHistItem;
int output_history_load(Output *o, App *a, OutHistItem *out, int max, long *expired, int *retention);
/* Frees the sql of n items filled by output_history_load (the array belongs to the caller). */
void output_history_free(OutHistItem *items, int n);
struct EditSet; struct ApplyResult;
/* Table editor transaction: see the OUT-017 comment above output_note_render. */
/* Object editor events (OBJ-061): an execution per validate / deploy / compile on c's session.
 * level is OL_*; sql (optional) is shown with the event; status of finish is ES_*. */
int output_obj_begin(Output *o, App *a, Connection *c, const char *title, int nstmts);
void output_obj_event(Output *o, App *a, Connection *c, int exec_id, int stmt, int level, const char *msg, const char *sql);
void output_obj_finish(Output *o, App *a, Connection *c, int exec_id, int status, double ms);
/* A run Lume refused before sending anything (read-only check): an execution on c's session,
 * status skipped, with the SQL and `why` as a warning, so the reason outlives the status bar. */
int output_refused(Output *o, App *a, Connection *c, const char *sql, int len, const char *why);
/* A Lume message (what the status bar said) kept as a note on c's session (NULL = the active
 * one); level is OL_*. Only warnings and errors badge the Output; a note never opens it. */
void output_note(Output *o, App *a, Connection *c, int level, const char *msg);
/* Test harness: prints c's notes ("outnote LEVEL message") and their unseen count to stdout. */
void output_notes_dump(Output *o, App *a, Connection *c);
void output_record_apply(Output *o, App *a, Db *db, const char *table, const struct EditSet *e, int status, const struct ApplyResult *res, double ms);
/* Writes the active log, with the current filters, to path: JSON (schema 1) when the path
 * ends in ".json", text otherwise. Secrets are redacted. Returns false on I/O error. */
bool output_export(Output *o, App *a, const char *path);
int output_unseen(Output *o, App *a, int *errors);     /* warnings / errors not yet looked at */
/* Clears the unseen badge of the active log. */
void output_mark_seen(Output *o, App *a);
void output_show_exec(Output *o, App *a, int exec_id); /* opens the tab on that execution */
/* execution (OUT-007 / OUT-018): one job at a time; the main loop pumps it every frame */
/* Starts sql (len < 0 = NUL-terminated) on db: mode is a RUN_* value for the first result;
 * editor_rev / sql_base tie statements back to the editor text. False if a job is running,
 * there is no connection, it is busy or there is nothing to run. */
bool output_run(Output *o, App *a, Db *db, const char *sql, int len, int mode, uint32_t editor_rev, int sql_base);
/* Options of an execution (toolbar: Explain, Parameters, Execute to File, Re-run). */
typedef struct OutRunOpts {
    int mode;                  /* RUN_* for the first result */
    const char *title;         /* result tab title, NULL = "Results" */
    const char *note;          /* recorded first in the Output (what ran, where), or NULL */
    const char *display;       /* shown in the Output / history instead of the SQL (no parameter values), or NULL */
    const char *export_path;   /* write the first result to this file, or NULL */
    const char *qtable;        /* the first result previews this table (editable with its key), or NULL */
    bool keep_tabs;            /* not an editor run: the query tab's other results stay */
    bool no_history;           /* a preview / reload: not added to Query History */
    int replace_exec;          /* RUN_REPLACE: the result tab (exec_id) to replace, 0 = the current one when it lands */
    /* called once when the execution ends: the exec_id of the result tab it opened (0 none: failed
     * or no rows) and its error; ud is the caller's, freed by the callback */
    void (*landed)(App *a, int tab_exec_id, const char *err, void *ud);
    void *ud;
} OutRunOpts;
int output_run_opts(Output *o, App *a, Db *db, const char *sql, int len, uint32_t editor_rev, int sql_base, const OutRunOpts *op);   /* execution id, 0 when it did not start */
/* Per-frame: logs finished statements of the running job and finalises it when done. */
void output_pump(Output *o, App *a);
/* True while a job exists and is still running. */
bool output_job_running(Output *o);                  /* any session has an execution running */
/* Per-session executions (TBR-020 / 021): one per connection, several sessions in parallel. */
typedef struct OutRunInfo { Db *db; int exec_id; double elapsed_ms; bool cancelling; int stmt, nstmts; char conn_name[64], conn_id[16]; const void *editor; } OutRunInfo;   /* editor: the tab that started it (compare only) */
bool output_db_running(Output *o, const Db *db);     /* this connection has an execution running */
int output_running_count(Output *o);
bool output_running_info(Output *o, int k, OutRunInfo *out);   /* k-th running execution, false past the end */
bool output_cancel_exec(Output *o, App *a, int exec_id);       /* cancels that execution only */
void output_note_tx_lost(Output *o, App *a, Connection *c);  /* records a transaction lost with its session */
/* Requests cancellation of the running job (status message explains what the driver can do). */
void output_cancel(Output *o, App *a);
/* test harness: synthetic events for the rendering benchmark; the visible row count */
void output_fill(Output *o, App *a, int executions, int events_each);
int output_rows(Output *o);
