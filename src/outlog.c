/* SPDX-License-Identifier: AGPL-3.0-only */
/* outlog.c - Execution Output event model (docs/execution-output.md, OUT-006 .. OUT-028).
 *
 * An OutLog is the per-connection record of what executions did: an ordered array of
 * OutEvent (each owning its heap strings) plus one OutExec per execution with its status
 * and counters. output.c creates one log per connection, feeds it while running statements
 * and table-editor transactions, and renders it; this file has no UI or driver code.
 *
 * Rules implemented here:
 *   - ids / sequence numbers / timestamps are assigned by the log (event_init);
 *   - ordering: events of one execution stay sorted by seq; a late event is inserted, not
 *     appended (append); transported events are de-duplicated by id (outlog_ingest);
 *   - state machine: a terminal execution status never changes again (outlog_exec_status);
 *   - retention: past max_events, whole finished executions are dropped oldest first and
 *     counted (retain), never the running one;
 *   - text: copies and exports go through outlog_redact so secrets never leave the app;
 *     exports are written to "<path>.part" and renamed into place.
 *
 * Ownership: the log owns every event string and execution summary; pointers returned by
 * outlog_push* / outlog_exec stay valid only until the next call that adds or removes events
 * (the arrays are realloc'd and compacted). Threading: none; UI thread only. */
#include "units.h"
#include "outlog.h"
#include <ctype.h>
#include <stdarg.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Current wall-clock time in seconds since the epoch (sub-second precision). */
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
/* strdup that passes NULL through. */
static char *ol_dup(const char *s) { return s ? strdup(s) : NULL; }

/* Static names used in text and JSON output; "?" / "unknown" for out-of-range values. */
const char *outlog_level_name(int l) { static const char *const n[] = { "debug", "info", "success", "warning", "error" }; return l >= 0 && l < OL_COUNT ? n[l] : "?"; }
/* Event type names as used in the JSON export. */
const char *outlog_type_name(int t) {
    static const char *const n[] = { "execution_started", "statement_started", "statement_completed", "statement_failed", "result_set_created", "rows_affected",
                                     "database_message", "transaction_changed", "execution_cancelled", "execution_finished", "unknown", "lume_message" };
    return t >= 0 && t < OT_COUNT ? n[t] : "unknown";
}
/* Execution status names. */
const char *outlog_status_name(int s) { static const char *const n[] = { "pending", "running", "cancelling", "completed", "failed", "cancelled", "skipped" }; return s >= 0 && s < ES_COUNT ? n[s] : "?"; }

/* See outlog.h: empty log for a session id / display name, ids starting at 1, default retention. */
void outlog_init(OutLog *l, int session, const char *name) {
    memset(l, 0, sizeof *l);
    l->session = session;
    snprintf(l->session_name, sizeof l->session_name, "%s", name ? name : "");
    l->next_id = 1; l->next_seq = 1; l->next_exec = 1; l->next_note = -1;
    l->max_events = OUTLOG_DEFAULT_MAX_EVENTS;
}

/* Frees the heap strings owned by one event (not the event itself). */
static void event_free(OutEvent *e) { free(e->message); free(e->sql); free(e->err_state); free(e->err_detail); free(e->err_hint); }

/* Frees everything and zeroes the log. */
void outlog_free(OutLog *l) {
    for (int i = 0; i < l->n; i++) event_free(&l->ev[i]);
    for (int i = 0; i < l->nex; i++) free(l->ex[i].summary);
    free(l->ev); free(l->ex);
    memset(l, 0, sizeof *l);
}

/* Drops every event and execution from the view. Id / seq counters and the dropped counters
 * are kept, so new events keep increasing ids. */
static bool terminal(int st);   /* defined with the executions below */
void outlog_clear(OutLog *l) {
    for (int i = 0; i < l->n; i++) event_free(&l->ev[i]);
    l->n = 0;
    /* executions still running keep their record: their next events must find it (otherwise
     * every push for them returns NULL); finished ones are dropped with their events */
    int keep = 0;
    for (int i = 0; i < l->nex; i++) {
        if (!terminal(l->ex[i].status)) { l->ex[keep++] = l->ex[i]; continue; }
        free(l->ex[i].summary);
    }
    l->nex = keep;
}

/* ---- executions ------------------------------------------------------------ */

/* Execution record by id (searched from the newest), or NULL if unknown or already dropped.
 * The pointer is invalidated by the next push / retention. */
OutExec *outlog_exec(OutLog *l, int exec_id) {
    for (int i = l->nex - 1; i >= 0; i--) if (l->ex[i].id == exec_id) return &l->ex[i];
    return NULL;
}

/* One-line summary of SQL for an execution: comments dropped, whitespace collapsed, cut at
 * 72 bytes with an ellipsis on a UTF-8 boundary. */
static void snippet(const char *sql, int len, char *out, size_t n) {
    size_t p = 0; bool space = false; int i = 0;
    while (i < len && p < n - 4) {
        if (sql[i] == '-' && i + 1 < len && sql[i + 1] == '-') { while (i < len && sql[i] != '\n') i++; continue; }
        if (isspace((unsigned char)sql[i])) { space = p > 0; i++; continue; }
        if (space) { out[p++] = ' '; space = false; }
        out[p++] = sql[i++];
        if (p >= 72) break;
    }
    out[p] = 0;
    if (i < len && p >= 72) { while (p > 0 && (out[p - 1] & 0xC0) == 0x80) p--; snprintf(out + p, n - p, "\xE2\x80\xA6"); }
}

/* True for final execution states (completed, failed, cancelled, skipped). */
static bool terminal(int st) { return st == ES_COMPLETED || st == ES_FAILED || st == ES_CANCELLED || st == ES_SKIPPED; }

/* retention (OUT-024): whole executions go first, oldest first, never the running one */
static void retain(OutLog *l) {
    while (l->n > l->max_events && l->nex > 1) {
        OutExec *old = &l->ex[0];
        if (!terminal(old->status)) break;
        int id = old->id, keep = 0;
        for (int i = 0; i < l->n; i++) {
            if (l->ev[i].exec_id == id) { event_free(&l->ev[i]); l->dropped_events++; continue; }
            l->ev[keep++] = l->ev[i];
        }
        l->n = keep;
        free(old->summary);
        memmove(&l->ex[0], &l->ex[1], sizeof *l->ex * (l->nex - 1));
        l->nex--; l->dropped_execs++;
    }
}

/* Stores a fully initialised event (taking ownership of its strings), keeping each
 * execution's events in seq order, updates the execution's last_seq and warning / error
 * counts, then applies retention. Returns the stored event, or NULL if its execution is
 * unknown (e.g. it was dropped or the log cleared); the event is stored either way. */
static OutEvent *append(OutLog *l, OutEvent *e) {
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 256; l->ev = realloc(l->ev, sizeof *l->ev * l->cap); }
    /* ordered by seq within the execution: a late event is inserted, not appended (OUT-027) */
    int pos = l->n;
    while (pos > 0 && l->ev[pos - 1].exec_id == e->exec_id && l->ev[pos - 1].seq > e->seq) pos--;
    if (pos < l->n) { memmove(&l->ev[pos + 1], &l->ev[pos], sizeof *l->ev * (l->n - pos)); l->out_of_order++; }
    l->ev[pos] = *e;
    l->n++;
    OutExec *x = outlog_exec(l, e->exec_id);
    if (x) {
        if (e->seq > x->last_seq) x->last_seq = e->seq;
        bool summary = e->type == OT_EXEC_FINISHED || e->type == OT_EXEC_CANCELLED;   /* summaries restate, they do not add */
        if (e->level == OL_ERROR && !summary) x->errors++;
        if (e->level == OL_WARNING && !summary) x->warnings++;
    }
    uint64_t id = e->id; int exec_id = e->exec_id;
    retain(l);   /* may compact the array: pos no longer names this event */
    if (!outlog_exec(l, exec_id)) return NULL;
    for (int i = l->n - 1; i >= 0; i--) if (l->ev[i].id == id) return &l->ev[i];
    return NULL;
}

/* See outlog.h. */
int outlog_note(OutLog *l, int level, const char *message) {
    if (!message) message = "";
    /* only a repeat right after the same note (nothing recorded between) is folded in */
    if (l->nex > 0 && l->ex[l->nex - 1].note && !strcmp(l->ex[l->nex - 1].summary, message) && l->n > 0 && l->ev[l->n - 1].exec_id == l->ex[l->nex - 1].id) {
        OutExec *p = &l->ex[l->nex - 1];
        for (int k = l->n - 1; k >= 0; k--) {
            OutEvent *e = &l->ev[k];
            if (e->exec_id != p->id || e->level != level) continue;
            p->repeat++; p->finished = now_s(); e->ts = p->finished;
            char *m = NULL; size_t n = strlen(message) + 32;
            if ((m = malloc(n))) { snprintf(m, n, "%s (\xC3\x97%d)", message, p->repeat); free(e->message); e->message = m; }
            return p->id;
        }
    }
    if (l->nex == l->capex) { l->capex = l->capex ? l->capex * 2 : 32; l->ex = realloc(l->ex, sizeof *l->ex * l->capex); }
    OutExec *x = &l->ex[l->nex++];
    memset(x, 0, sizeof *x);
    x->id = l->next_note--; x->session = l->session; x->status = ES_COMPLETED; x->started = x->finished = now_s();
    x->note = true; x->repeat = 1;
    x->summary = strdup(message);
    int id = x->id;   /* outlog_push may run retention, which moves the executions array */
    outlog_push(l, id, 0, OT_NOTE, level, message);
    return id;
}

/* See outlog.h. Opens a new running execution with a one-line summary of sql and emits its
 * execution_started event (carrying the SQL and editor position). Returns the execution id. */
int outlog_exec_begin(OutLog *l, const char *sql, int len, uint32_t editor_rev, int sql_start) {
    if (l->nex == l->capex) { l->capex = l->capex ? l->capex * 2 : 32; l->ex = realloc(l->ex, sizeof *l->ex * l->capex); }
    OutExec *x = &l->ex[l->nex++];
    memset(x, 0, sizeof *x);
    x->id = l->next_exec++; x->session = l->session; x->status = ES_RUNNING; x->started = now_s();
    char s[120]; snippet(sql, len < 0 ? (int)strlen(sql) : len, s, sizeof s);
    x->summary = strdup(s);
    char msg[200]; snprintf(msg, sizeof msg, "Execution #%d started", x->id);
    outlog_push_sql(l, x->id, 0, OT_EXEC_STARTED, OL_INFO, msg, sql, len, sql_start, editor_rev);
    return x->id;
}

/* See outlog.h. Moves an execution to `status` unless it is already in a terminal state. */
void outlog_exec_status(OutLog *l, int exec_id, int status) {
    OutExec *x = outlog_exec(l, exec_id);
    if (!x) return;
    if (terminal(x->status) && status != x->status) return;   /* a late event never flips a terminal state */
    x->status = status;
}

/* See outlog.h. Finalises a running execution: status, end time, duration in ms, and a
 * summary event (execution_cancelled or execution_finished with ok / failed / not-run counts).
 * No-op if it already ended. */
void outlog_exec_finish(OutLog *l, int exec_id, int status, double ms) {
    OutExec *x = outlog_exec(l, exec_id);
    if (!x) return;
    if (terminal(x->status)) return;
    x->status = status; x->finished = now_s(); x->ms = ms;
    char msg[200];
    if (status == ES_CANCELLED) {
        snprintf(msg, sizeof msg, "%d of %d statement%s completed before the cancel", x->ok, x->nstmts, x->nstmts == 1 ? "" : "s");
        OutEvent *e = outlog_push(l, exec_id, 0, OT_EXEC_CANCELLED, OL_INFO, msg);   /* the statement row is the warning; this is the summary */
        if (e) e->exec_ms = ms;
    } else {
        snprintf(msg, sizeof msg, "%d of %d statement%s ok \xC2\xB7 %d failed \xC2\xB7 %d not run",
                 x->ok, x->nstmts, x->nstmts == 1 ? "" : "s", x->failed, x->skipped);
        OutEvent *e = outlog_push(l, exec_id, 0, OT_EXEC_FINISHED, status == ES_FAILED ? OL_ERROR : status == ES_SKIPPED ? OL_WARNING : OL_SUCCESS, msg);   /* a refused run is not a success */
        if (e) e->exec_ms = ms;
    }
}

/* Fills a new event with the next id / seq, the current time, a copy of message and every
 * metric / diagnostic marked absent (-1). */
static void event_init(OutLog *l, OutEvent *e, int exec_id, int stmt_id, int type, int level, const char *message) {
    memset(e, 0, sizeof *e);
    e->id = l->next_id++; e->seq = l->next_seq++; e->ts = now_s();
    e->exec_id = exec_id; e->stmt_id = stmt_id; e->type = (uint8_t)type; e->level = (uint8_t)level;
    e->message = ol_dup(message ? message : "");
    e->exec_ms = e->fetch_ms = e->render_ms = -1; e->rows_affected = e->rows_returned = e->rows_loaded = -1;
    e->err_line = e->err_col = -1; e->err_code = -1; e->autocommit = -1;
}

/* See outlog.h: creates and stores an event without SQL. */
OutEvent *outlog_push(OutLog *l, int exec_id, int stmt_id, int type, int level, const char *message) {
    OutEvent e; event_init(l, &e, exec_id, stmt_id, type, level, message);
    return append(l, &e);
}

/* Longest SQL text copied into an event; longer statements are flagged sql_truncated. */
#define SQL_KEEP 4000
/* See outlog.h: like outlog_push, plus a copy of the statement (at most SQL_KEEP bytes) and its
 * position: start / start + len are offsets in editor revision rev (start -1 = not from the
 * editor). len < 0 means NUL-terminated. */
OutEvent *outlog_push_sql(OutLog *l, int exec_id, int stmt_id, int type, int level, const char *message, const char *sql, int len, int start, uint32_t rev) {
    OutEvent e; event_init(l, &e, exec_id, stmt_id, type, level, message);
    if (sql) {
        if (len < 0) len = (int)strlen(sql);
        int keep = len > SQL_KEEP ? SQL_KEEP : len;
        e.sql = malloc(keep + 1); memcpy(e.sql, sql, keep); e.sql[keep] = 0;
        e.sql_truncated = keep < len;
        e.sql_start = start; e.sql_end = start + len; e.editor_rev = rev;
    }
    return append(l, &e);
}

/* See outlog.h. Copies an event built elsewhere (all strings duplicated). Drops it if the same
 * id is among the last 4096 events; creates the execution record on first sight; advances the
 * id / seq / execution counters past it. Returns false for a duplicate. */
bool outlog_ingest(OutLog *l, const OutEvent *src) {
    for (int i = l->n - 1; i >= 0 && i >= l->n - 4096; i--) if (l->ev[i].id == src->id) { l->dedup_dropped++; return false; }
    OutEvent e = *src;
    e.message = ol_dup(src->message); e.sql = ol_dup(src->sql); e.err_state = ol_dup(src->err_state); e.err_detail = ol_dup(src->err_detail); e.err_hint = ol_dup(src->err_hint);
    if (e.id >= l->next_id) l->next_id = e.id + 1;
    if (e.seq >= l->next_seq) l->next_seq = e.seq + 1;
    if (!outlog_exec(l, e.exec_id)) {
        if (l->nex == l->capex) { l->capex = l->capex ? l->capex * 2 : 32; l->ex = realloc(l->ex, sizeof *l->ex * l->capex); }
        OutExec *x = &l->ex[l->nex++]; memset(x, 0, sizeof *x);
        x->id = e.exec_id; x->session = l->session; x->status = ES_RUNNING; x->started = e.ts; x->summary = strdup(e.sql ? e.sql : e.message ? e.message : "");
        if (e.type == OT_NOTE && e.exec_id < 0) {   /* a note: finished on arrival */
            x->note = true; x->repeat = 1; x->status = ES_COMPLETED; x->finished = e.ts;
            if (e.exec_id <= l->next_note) l->next_note = e.exec_id - 1;
        } else if (e.exec_id >= l->next_exec) l->next_exec = e.exec_id + 1;
    }
    append(l, &e);
    return true;
}

/* ---- views ---------------------------------------------------------------- */

/* Case-insensitive substring test; false for NULL inputs or an empty needle. */
static bool contains_ci(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return false;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++) if (strncasecmp(p, needle, n) == 0) return true;
    return false;
}

/* See outlog.h. f NULL = everything visible. Text matches message, SQL, SQLSTATE or detail. */
bool outlog_visible(const OutLog *l, const OutEvent *e, const OutFilter *f) {
    (void)l;
    if (!f) return true;
    if (f->levels && !(f->levels & (1u << e->level))) return false;
    if (f->errors_only && e->level < OL_WARNING) return false;
    if (f->exec_id && e->exec_id != f->exec_id) return false;
    if (f->text && *f->text && !contains_ci(e->message, f->text) && !contains_ci(e->sql, f->text) && !contains_ci(e->err_state, f->text) && !contains_ci(e->err_detail, f->text)) return false;
    return true;
}

/* See outlog.h: visible events under f; *hidden (optional) gets the rest. */
int outlog_count(const OutLog *l, const OutFilter *f, int *hidden) {
    int vis = 0;
    for (int i = 0; i < l->n; i++) if (outlog_visible(l, &l->ev[i], f)) vis++;
    if (hidden) *hidden = l->n - vis;
    return vis;
}

/* See outlog.h: indices of up to max events whose message, SQL, SQLSTATE or detail contain
 * needle (case-insensitive). Returns the number of hits. */
int outlog_search(const OutLog *l, const char *needle, int *hits, int max) {
    int n = 0;
    if (!needle || !*needle) return 0;
    for (int i = 0; i < l->n && n < max; i++) {
        const OutEvent *e = &l->ev[i];
        if (contains_ci(e->message, needle) || contains_ci(e->sql, needle) || contains_ci(e->err_state, needle) || contains_ci(e->err_detail, needle)) hits[n++] = i;
    }
    return n;
}

/* See outlog.h: executions held, and errors / warnings summed over them. Out pointers may be NULL. */
void outlog_counts(const OutLog *l, int *execs, int *errors, int *warnings) {
    int er = 0, wa = 0;
    for (int i = 0; i < l->nex; i++) { er += l->ex[i].errors; wa += l->ex[i].warnings; }
    if (execs) *execs = l->nex;
    if (errors) *errors = er;
    if (warnings) *warnings = wa;
}

/* ---- text ----------------------------------------------------------------- */

/* masks the value after password-like keywords and in key=value pairs (connection strings),
 * e.g. "IDENTIFIED BY x", "PASSWORD 'x'", "password=x", "token: x" */
/* out has n bytes and is always NUL-terminated; output is cut when it fills up. Quoted values
 * become 'q***q', bare ones ***. Keywords only match at word boundaries. */
void outlog_redact(const char *in, char *out, size_t n) {
    static const char *const keys[] = { "identified by", "password", "passwd", "secret", "token", "api_key", "apikey", "pwd", NULL };
    size_t o = 0;
    for (size_t i = 0; in[i] && o < n - 1;) {
        bool hit = false;
        for (int k = 0; keys[k]; k++) {
            size_t kl = strlen(keys[k]);
            if (strncasecmp(in + i, keys[k], kl) == 0 && (i == 0 || (!isalnum((unsigned char)in[i - 1]) && in[i - 1] != '_')) && !isalnum((unsigned char)in[i + kl]) && in[i + kl] != '_') {
                /* copy the keyword, then skip separators and mask the value token / literal */
                for (size_t j = 0; j < kl && o < n - 1; j++) out[o++] = in[i + j];
                size_t p = i + kl;
                /* a quoted keyword ('password', "password", `password`) ends with its own quote:
                 * copy it, or it would be taken for the start of the value and the real literal
                 * after the operator would be left in clear */
                if (i > 0 && (in[i - 1] == '\'' || in[i - 1] == '"' || in[i - 1] == '`') && in[p] == in[i - 1]) { if (o < n - 1) out[o++] = in[p]; p++; }
                while (in[p] == ' ' || in[p] == '=' || in[p] == ':' || in[p] == '\t') { if (o < n - 1) out[o++] = in[p]; p++; }
                if (in[p] == '\'' || in[p] == '"') { char q = in[p++]; while (in[p] && in[p] != q) p++; if (in[p] == q) p++; if (o + 5 < n) { out[o++] = q; memcpy(out + o, "***", 3); o += 3; out[o++] = q; } }
                else if (in[p] && !isspace((unsigned char)in[p]) && in[p] != ';' && in[p] != ',') { while (in[p] && !isspace((unsigned char)in[p]) && in[p] != ';' && in[p] != ',' && in[p] != ')') p++; if (o + 3 < n) { memcpy(out + o, "***", 3); o += 3; } }
                i = p; hit = true; break;
            }
        }
        if (!hit) out[o++] = in[i++];
    }
    out[o] = 0;
}

/* Growable text buffer used to build event text; s is malloc'd and handed to the caller. */
typedef struct { char *s; size_t len, cap; } TB;
/* Appends a NUL-terminated string, growing the buffer. */
static void tb_put(TB *b, const char *s) { size_t n = strlen(s); if (b->len + n + 1 > b->cap) { b->cap = (b->len + n + 1) * 2 + 128; b->s = realloc(b->s, b->cap); } memcpy(b->s + b->len, s, n); b->len += n; b->s[b->len] = 0; }
/* printf-style append; each call is limited to about 1200 bytes. */
static void tb_printf(TB *b, const char *fmt, ...) { char tmp[1200]; va_list ap; va_start(ap, fmt); vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap); tb_put(b, tmp); }
/* Formats ts (epoch seconds) in local time: ISO-8601 with ms and UTC offset when iso, else
 * HH:MM:SS.mmm. */
static void fmt_ts(double ts, char *out, size_t n, bool iso) {
    time_t t = (time_t)ts; struct tm tm; localtime_r(&t, &tm);
    int ms = (int)((ts - (double)t) * 1000);
    if (iso) { char z[8]; strftime(z, sizeof z, "%z", &tm); snprintf(out, n, "%04d-%02d-%02dT%02d:%02d:%02d.%03d%s", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, ms, z); }
    else snprintf(out, n, "%02d:%02d:%02d.%03d", tm.tm_hour, tm.tm_min, tm.tm_sec, ms);
}

/* See outlog.h. Builds "time  level  message" and, with details, one indented line per known
 * field (type, ids, SQL, metrics, diagnostics, transaction, result tab). Message, SQL and
 * detail are redacted. Returns a malloc'd string the caller frees. */
char *outlog_event_text(const OutLog *l, const OutEvent *e, bool details) {
    (void)l;
    TB b = { 0 };
    char ts[48], red[4200];
    fmt_ts(e->ts, ts, sizeof ts, false);
    outlog_redact(e->message, red, sizeof red);
    tb_printf(&b, "%s  %-7s  %s", ts, outlog_level_name(e->level), red);
    if (!details) return b.s;
    if (e->type == OT_NOTE) tb_printf(&b, "\n  type: %s  seq: %u", outlog_type_name(e->type), e->seq);   /* a note belongs to no execution */
    else tb_printf(&b, "\n  type: %s  execution: #%d%s  seq: %u", outlog_type_name(e->type), e->exec_id, e->stmt_id ? "" : "", e->seq);
    if (e->stmt_id) tb_printf(&b, "  statement: %d", e->stmt_id);
    if (e->sql) { outlog_redact(e->sql, red, sizeof red); tb_printf(&b, "\n  sql: %s%s", red, e->sql_truncated ? " \xE2\x80\xA6(truncated)" : ""); }
    char dur[24];
    if (e->exec_ms >= 0) tb_printf(&b, "\n  execution: %s (client-measured)", fmt_duration(dur, sizeof dur, e->exec_ms));
    if (e->fetch_ms >= 0) tb_printf(&b, "\n  fetch: %s (client-measured)", fmt_duration(dur, sizeof dur, e->fetch_ms));
    if (e->render_ms >= 0) tb_printf(&b, "\n  render: %s (client, CPU side of the first draw of the results panel; GPU time not included)", fmt_duration(dur, sizeof dur, e->render_ms));
    if (e->rows_affected >= 0) tb_printf(&b, "\n  rows affected: %ld", e->rows_affected);
    if (e->rows_returned >= 0) tb_printf(&b, "\n  rows returned: %ld", e->rows_returned);
    if (e->rows_loaded >= 0) tb_printf(&b, "\n  rows loaded: %ld (total %s)", e->rows_loaded, e->rows_returned >= 0 ? "known" : "unknown");
    if (e->err_state) tb_printf(&b, "\n  sqlstate: %s", e->err_state);
    if (e->err_code >= 0) tb_printf(&b, "\n  native code: %ld", e->err_code);
    if (e->err_detail) { outlog_redact(e->err_detail, red, sizeof red); tb_printf(&b, "\n  detail: %s", red); }
    if (e->err_hint) tb_printf(&b, "\n  hint: %s", e->err_hint);
    if (e->err_line >= 0) tb_printf(&b, "\n  position: line %d%s", e->err_line, e->err_col >= 0 ? "" : "");
    if (e->err_line >= 0 && e->err_col >= 0) tb_printf(&b, ", column %d", e->err_col);
    if (e->tx_action) tb_printf(&b, "\n  transaction: %s \xC2\xB7 state %s%s", e->tx_action == TX_BEGIN ? "begin" : e->tx_action == TX_COMMIT ? "commit" : "rollback",
                                e->tx_state == TXS_OPEN ? "open" : e->tx_state == TXS_COMMITTED ? "committed" : e->tx_state == TXS_ROLLED_BACK ? "rolled back" : "unknown", e->autocommit == 1 ? " \xC2\xB7 autocommit" : "");
    if (e->result_tab) tb_printf(&b, "\n  result set: tab #%d", e->result_tab);
    return b.s;
}

/* Writes s as a JSON string literal (escaping quotes, backslashes and control characters). */
static void json_str(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\') { fputc('\\', f); fputc(*p, f); }
        else if (*p == '\n') fputs("\\n", f); else if (*p == '\r') fputs("\\r", f); else if (*p == '\t') fputs("\\t", f);
        else if (*p < 0x20) fprintf(f, "\\u%04x", *p); else fputc(*p, f);
    }
    fputc('"', f);
}

/* See outlog.h. Writes the visible events to "<path>.part" and renames it over path, so a
 * failed export never leaves a half-written file. JSON: a header (schema, session, counts,
 * whether retention dropped history) and one object per event; text: a header line and the
 * detailed text of each event. err gets a message on failure. */
bool outlog_export(const OutLog *l, const OutFilter *f, const char *path, bool json, char *err, size_t errn) {
    char tmp[1100]; snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *out = fopen(tmp, "wb");
    if (!out) { snprintf(err, errn, "cannot create %s: %s", tmp, strerror(errno)); return false; }
    int hidden = 0; int vis = outlog_count(l, f, &hidden);
    char red[4200], ts[48];
    if (json) {
        fprintf(out, "{\"schemaVersion\": %d, \"session\": ", OUTLOG_SCHEMA); json_str(out, l->session_name);
        fprintf(out, ", \"events\": %d, \"filteredOut\": %d, \"droppedByRetention\": %ld, \"historyComplete\": %s, \"events_list\": [", vis, hidden, l->dropped_events, l->dropped_events ? "false" : "true");
        /* One JSON object per visible event; absent values are omitted. */
        bool first = true;
        for (int i = 0; i < l->n; i++) {
            const OutEvent *e = &l->ev[i];
            if (!outlog_visible(l, e, f)) continue;
            fmt_ts(e->ts, ts, sizeof ts, true);
            fprintf(out, "%s\n  {\"id\": %llu, \"executionId\": %d, \"sessionId\": %d, \"sequence\": %u, \"timestamp\": \"%s\", \"level\": \"%s\", \"type\": \"%s\", \"message\": ",
                    first ? "" : ",", (unsigned long long)e->id, e->exec_id, l->session, e->seq, ts, outlog_level_name(e->level), outlog_type_name(e->type));
            first = false;
            outlog_redact(e->message, red, sizeof red); json_str(out, red);
            if (e->stmt_id) fprintf(out, ", \"statementId\": %d", e->stmt_id);
            if (e->sql) { outlog_redact(e->sql, red, sizeof red); fputs(", \"sql\": {\"text\": ", out); json_str(out, red); fprintf(out, ", \"editorRevision\": %u, \"startOffset\": %d, \"endOffset\": %d}", e->editor_rev, e->sql_start, e->sql_end); }
            bool m = e->exec_ms >= 0 || e->fetch_ms >= 0 || e->render_ms >= 0 || e->rows_affected >= 0 || e->rows_returned >= 0 || e->rows_loaded >= 0;
            if (m) {
                fputs(", \"metrics\": {", out); bool c = false;
                if (e->exec_ms >= 0) { fprintf(out, "\"executionMs\": %.3f", e->exec_ms); c = true; }
                if (e->fetch_ms >= 0) { fprintf(out, "%s\"fetchMs\": %.3f", c ? ", " : "", e->fetch_ms); c = true; }
                if (e->render_ms >= 0) { fprintf(out, "%s\"renderMs\": %.3f", c ? ", " : "", e->render_ms); c = true; }
                if (e->rows_affected >= 0) { fprintf(out, "%s\"rowsAffected\": %ld", c ? ", " : "", e->rows_affected); c = true; }
                if (e->rows_returned >= 0) { fprintf(out, "%s\"rowsReturned\": %ld", c ? ", " : "", e->rows_returned); c = true; }
                if (e->rows_loaded >= 0) { fprintf(out, "%s\"rowsLoaded\": %ld", c ? ", " : "", e->rows_loaded); }
                fputc('}', out);
            }
            if (e->type == OT_STMT_FAILED || e->err_state || e->err_code >= 0) {
                fputs(", \"error\": {\"message\": ", out); json_str(out, red);
                if (e->err_state) { fputs(", \"sqlState\": ", out); json_str(out, e->err_state); }
                if (e->err_code >= 0) fprintf(out, ", \"nativeCode\": %ld", e->err_code);
                if (e->err_detail) { outlog_redact(e->err_detail, red, sizeof red); fputs(", \"detail\": ", out); json_str(out, red); }
                if (e->err_hint) { fputs(", \"hint\": ", out); json_str(out, e->err_hint); }
                if (e->err_line >= 0) fprintf(out, ", \"line\": %d", e->err_line);
                if (e->err_col >= 0) fprintf(out, ", \"column\": %d", e->err_col);
                fputc('}', out);
            }
            if (e->tx_action) fprintf(out, ", \"transaction\": {\"action\": \"%s\", \"state\": \"%s\"%s}", e->tx_action == TX_BEGIN ? "begin" : e->tx_action == TX_COMMIT ? "commit" : "rollback",
                                      e->tx_state == TXS_OPEN ? "open" : e->tx_state == TXS_COMMITTED ? "committed" : e->tx_state == TXS_ROLLED_BACK ? "rolled_back" : "unknown", e->autocommit == 1 ? ", \"autocommit\": true" : e->autocommit == 0 ? ", \"autocommit\": false" : "");
            if (e->result_tab) fprintf(out, ", \"resultSetId\": \"tab-%d\"", e->result_tab);
            fputc('}', out);
        }
        fputs("\n]}\n", out);
    } else {
        fprintf(out, "Lume execution output \xC2\xB7 session %s \xC2\xB7 %d event%s%s%s\n", l->session_name, vis, vis == 1 ? "" : "s", hidden ? " (filter active, some hidden)" : "", l->dropped_events ? " \xC2\xB7 history incomplete: older executions were dropped by retention" : "");
        for (int i = 0; i < l->n; i++) {
            const OutEvent *e = &l->ev[i];
            if (!outlog_visible(l, e, f)) continue;
            char *t = outlog_event_text(l, e, true);
            fputs(t, out); fputc('\n', out);
            free(t);
        }
    }
    bool ok = fflush(out) == 0 && !ferror(out);
    if (fclose(out) != 0) ok = false;
    if (!ok) { snprintf(err, errn, "write failed: %s", strerror(errno)); remove(tmp); return false; }
    if (rename(tmp, path) != 0) { snprintf(err, errn, "cannot replace %s: %s", path, strerror(errno)); remove(tmp); return false; }
    return true;
}
