/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Execution Output model (docs/execution-output.md, OUT-006 .. OUT-028): structured events
 * per session, grouped by execution, with a state machine per execution that reconciles
 * late events. No UI, no driver code: results.c renders it, exec.c feeds it.
 * Schema version 1; every value that was not measured stays absent (-1 / NULL).
 *
 * Today output.c both feeds the log (editor runs, table-editor applies) and renders it.
 * Ownership: the OutLog owns all event strings and execution summaries. Pointers to events or
 * executions are invalidated by any call that adds or removes events. UI thread only. */

#define OUTLOG_SCHEMA 1                      /* version of the JSON export format */
#define OUTLOG_DEFAULT_MAX_EVENTS 20000      /* retention threshold (OutLog.max_events) */

/* Severity of an event (OutEvent.level); also the bit index in OutFilter.levels. */
enum { OL_DEBUG, OL_INFO, OL_SUCCESS, OL_WARNING, OL_ERROR, OL_COUNT };
/* Event types (OutEvent.type); names in outlog_type_name match the export schema. */
enum { OT_EXEC_STARTED, OT_STMT_STARTED, OT_STMT_COMPLETED, OT_STMT_FAILED, OT_RESULT_SET, OT_ROWS_AFFECTED,
       OT_DB_MESSAGE, OT_TX_CHANGED, OT_EXEC_CANCELLED, OT_EXEC_FINISHED, OT_UNKNOWN,
       OT_NOTE /* a Lume message (what the status bar said), outside any execution */, OT_COUNT };
/* Execution status (OutExec.status). COMPLETED / FAILED / CANCELLED / SKIPPED are terminal. */
enum { ES_PENDING, ES_RUNNING, ES_CANCELLING, ES_COMPLETED, ES_FAILED, ES_CANCELLED, ES_SKIPPED, ES_COUNT };
/* Transaction action an event reports (OutEvent.tx_action). */
enum { TX_NONE, TX_BEGIN, TX_COMMIT, TX_ROLLBACK };
/* Transaction state after that action (OutEvent.tx_state). */
enum { TXS_UNKNOWN, TXS_OPEN, TXS_COMMITTED, TXS_ROLLED_BACK };

/* One structured event. Heap strings (message, sql, err_*) are owned by the log. */
typedef struct OutEvent {
    uint64_t id;                     /* unique within the log, increasing; dedup key for ingest */
    int exec_id, stmt_id;            /* stmt_id 0 = execution-level */
    uint32_t seq;                    /* emission order; events of an execution are kept sorted by it */
    double ts;                       /* client clock, seconds since the epoch */
    uint8_t level, type;             /* OL_* severity, OT_* type */
    char *message;                   /* human-readable text, never NULL */
    /* sql + where it came from (OUT-008); offsets are into the editor revision executed */
    char *sql; int sql_start, sql_end; uint32_t editor_rev; bool sql_truncated;
    /* metrics (OUT-010): -1 = not measured / not provided */
    double exec_ms, fetch_ms; long rows_affected, rows_returned, rows_loaded;
    double render_ms;                /* OUT-026: CPU time of the first draw of the result it produced, -1 = not measured */
    /* error (OUT-012) */
    char *err_state, *err_detail, *err_hint; long err_code; int err_line, err_col;
    /* transaction (OUT-017) */
    uint8_t tx_action, tx_state; int8_t autocommit;   /* -1 unknown */
    int result_tab;                  /* exec id of the Results tab created (OUT-009), 0 = none */
} OutEvent;

/* One execution (an editor run or a table-editor apply) and its running totals. */
typedef struct OutExec {
    int id, session;                 /* execution id (per log) and OutLog.session */
    int status;                      /* ES_* */
    double started, finished;        /* finished 0 while running */
    int nstmts, ok, failed, skipped; /* statement counts, filled in by the caller */
    char *summary;                   /* first statement, one line */
    bool collapsed, seen;            /* seen: badge cleared */
    uint32_t last_seq;               /* highest seq seen for this execution */
    int warnings, errors;            /* events at that level, excluding the closing summary */
    double ms;                       /* measured duration of the whole execution */
    bool partial;                    /* a cancelled statement kept a partial result set */
    bool note;                       /* a Lume message (outlog_note): one OT_NOTE event, negative id, no header row */
    int repeat;                      /* note: how many times the same message came in a row (1 = once) */
} OutExec;

/* The event log of one session (connection). */
typedef struct OutLog {
    int session;                     /* connection id; events of another session never land here */
    char session_name[64];           /* display name used in exports and history */
    OutEvent *ev; int n, cap;        /* events, ordered by execution then seq */
    OutExec *ex; int nex, capex;     /* executions, oldest first */
    uint64_t next_id; uint32_t next_seq;
    int next_exec;                   /* next ids to hand out */
    int next_note;                   /* notes count down from -1, so execution numbers have no gaps */
    int max_events; long dropped_events, dropped_execs;   /* retention threshold and what it removed */
    long dedup_dropped, out_of_order;   /* ingest statistics: duplicates ignored, late events inserted */
} OutLog;

/* Initialises an empty log for connection `session` with display name `name` (may be NULL). */
void outlog_init(OutLog *l, int session, const char *name);
/* Frees all events and executions and zeroes the log. */
void outlog_free(OutLog *l);
void outlog_clear(OutLog *l);                      /* clears the view's events; counts stay */

/* executions and statements */
/* Opens a running execution for sql (len < 0 = NUL-terminated); editor_rev / sql_start locate
 * it in the editor. Returns the new execution id. */
int outlog_exec_begin(OutLog *l, const char *sql, int len, uint32_t editor_rev, int sql_start);   /* emits execution_started */
/* Execution by id, or NULL if unknown / dropped by retention. */
OutExec *outlog_exec(OutLog *l, int exec_id);
void outlog_exec_status(OutLog *l, int exec_id, int status);   /* state machine: terminal states never regress */
/* Ends a running execution with a terminal status and its duration in ms. */
void outlog_exec_finish(OutLog *l, int exec_id, int status, double ms);   /* emits execution_finished / cancelled */
/* generic event: the log fills id / seq / ts; returns the stored copy */
OutEvent *outlog_push(OutLog *l, int exec_id, int stmt_id, int type, int level, const char *message);
/* Records a Lume message (the status bar's text) as a note: its own finished record with a
 * negative id and one OT_NOTE event at `level`. The same message at the same level right after
 * the previous note bumps that note's counter ("(x3)") and time instead. Returns the note id. */
int outlog_note(OutLog *l, int level, const char *message);
/* Same with a copy of the statement text (len < 0 = NUL-terminated), its start offset in editor
 * revision rev (-1 = not from the editor). The returned pointer may be NULL and is valid only
 * until the next push. */
OutEvent *outlog_push_sql(OutLog *l, int exec_id, int stmt_id, int type, int level, const char *message, const char *sql, int len, int start, uint32_t rev);
/* ingest an already-built event (transport path, OUT-027): dedup by id, ordered by seq */
/* Returns false when the event was a duplicate and was ignored. */
bool outlog_ingest(OutLog *l, const OutEvent *e);

/* views */
/* View filter: levels mask, one execution (0 = all), case-insensitive text (NULL / "" = none);
 * errors_only keeps warnings and errors. */
typedef struct OutFilter { unsigned levels; int exec_id; const char *text; bool errors_only; } OutFilter;   /* levels: bit mask, 0 = all */
/* True when e passes f (f NULL = everything). l is unused and may be NULL. */
bool outlog_visible(const OutLog *l, const OutEvent *e, const OutFilter *f);
/* Number of events passing f; *hidden (optional) gets the number filtered out. */
int outlog_count(const OutLog *l, const OutFilter *f, int *hidden);
/* Fills hits with up to max indices into l->ev of events matching needle; returns the count. */
int outlog_search(const OutLog *l, const char *needle, int *hits, int max);   /* event indices */
/* Totals for the footer: executions held, error and warning events. Out pointers may be NULL. */
void outlog_counts(const OutLog *l, int *execs, int *errors, int *warnings);

/* text (OUT-021 / OUT-022 / OUT-028): redaction masks password-like literals and
 * key=value secrets; the clipboard and files never see them */
/* Copies in to out (n bytes, always NUL-terminated) with secret values replaced by ***. */
void outlog_redact(const char *in, char *out, size_t n);
/* One event as text: one line, or with details every known field on its own line.
 * Redacted; the caller frees the result. */
char *outlog_event_text(const OutLog *l, const OutEvent *e, bool details);    /* malloc'd */
/* Writes the events passing f to path, as JSON (schema OUTLOG_SCHEMA) or text, atomically via
 * "<path>.part". Redacted. On false, err (errn bytes) explains why. */
bool outlog_export(const OutLog *l, const OutFilter *f, const char *path, bool json, char *err, size_t errn);
/* Static display / export names for OL_*, OT_* and ES_* values. */
const char *outlog_level_name(int level);
const char *outlog_type_name(int type);
const char *outlog_status_name(int status);
