/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Visual Table Designer tab: see tdes.h.
 *
 * Layout (spec 2.1): header (target, state, Review Changes, more menu) / banners / structure tree
 * on the left and properties on the right / SQL preview (the tab's own editor, read-only) /
 * footer (changes, errors, warnings, Discard, Review & Apply).
 *
 * Executor phases (TBL-053..061, 096): DRIFT (the table read again: its fingerprint must be the one
 * the draft was made on) -> RUN ([PRAGMA foreign_keys = OFF] [BEGIN] steps, stop at the first
 * failure) -> END (COMMIT, or ROLLBACK, [PRAGMA ON]) -> VERIFY (the table read back: what the
 * server has is what the tab shows). The outcome is never presumed: a rollback is reported only
 * when the ROLLBACK statement (or the session's transaction count) confirms it; Oracle / MySQL /
 * MariaDB failures list the steps that stayed applied.
 */
#include "tdes.h"
#include "commands.h"
#include "caps.h"
#include "conn.h"
#include "draw.h"
#include "exec.h"
#include "explorer.h"
#include "font.h"
#include "log.h"
#include "nsx.h"
#include "objed.h"
#include "outlog.h"
#include "output.h"
#include "panels.h"
#include "sqllex.h"
#include "tbl.h"
#include "theme.h"
#include "ui.h"
#include "editor.h"
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_TDOCS 16
#define UNDO_MAX 64
enum { DS_LOADING, DS_READY, DS_LOAD_FAILED, DS_APPLYING };
enum { PH_NONE, PH_LOAD, PH_DRIFT, PH_RUN, PH_END, PH_VERIFY, PH_DATACHECK };
enum { SEL_TABLE, SEL_COL, SEL_KEY, SEL_GONE_COL, SEL_GONE_KEY };
enum { F_NAME, F_TYPE, F_DEFAULT, F_COMMENT, F_GEN, F_COLL, F_KCOLS, F_REFTAB, F_REFCOLS, F_WHERE, F_INCLUDE, F_METHOD, F_CHECK, F_ENGINE, F_CHARSET, F_SCHEMA, F_USING, F_N };
enum { M_NONE, M_REVIEW, M_CLOSE, M_DROP };
enum { MI_COPY, MI_CONSOLE, MI_MIGRATION, MI_SAVE_DRAFT, MI_DISCARD_DRAFT, MI_RELOAD, MI_UNDO, MI_REDO, MI_CREATE_DDL, MI_AI_REVIEW, MI_TYPE0 = 100 };

struct TdDoc {
    int tab_id;
    char conn_id[32], conn_name[64];
    int dialect, version;
    char schema[128], name[128];      /* what was opened (an existing table) */
    TdTable orig, draft;
    bool is_new, sqlite_fk_on;
    TdTable undo[UNDO_MAX]; int nundo; TdTable redo[UNDO_MAX]; int nredo; char undo_key[64];
    uint32_t rev, plan_rev, field_rev, preview_rev;
    TdPlan plan; char *script;
    TdChange changes[128]; int nchanges;
    TdIssue issues[128]; int nissues;
    int state; char msg[600];
    /* jobs */
    ExecJob *job; int phase; double t0; double retry_at; int after;
    /* apply */
    int out_exec; uint64_t approved; int ran, failed_at; char failed_err[600]; bool rb_confirmed, apply_ok; uint64_t fp_before;
    char deploy_id[32]; int pre;   /* statements before the first step (PRAGMA, BEGIN) */
    TdPlan run;                    /* the plan being applied (owned copy) */
    /* selection, tree */
    int sel_kind, sel_id; float tree_scroll; ScrollBar tree_sb; TextField filter;
    bool closed_grp[TK_NKINDS + 1];
    /* property fields bound to the selection */
    TextField f[F_N]; int f_kind, f_id; uint32_t f_loaded_rev;
    /* banners */
    bool drift, other_tab; char banner[400];
    bool draft_on_disk; double draft_saved_at; uint32_t draft_rev_saved;
    bool preview_open;
    /* review options */
    TextField lock_timeout, prod_name; bool consent;
    char using_expr[TD_MAXKC][256]; int using_col[TD_MAXKC]; int nusing;
    char datacheck[1200]; int dc_n, dc_i; char dc_sql[8][800]; char dc_what[8][200];
    bool drop_mode;                /* the review is the Drop Table flow */
    char pending_index[600], pending_index_name[128], pending_why[300];   /* an index proposed from outside (execution plan) */
};

static struct {
    App *a;
    TdDoc *docs[MAX_TDOCS];
    int modal; TdDoc *mdoc; int close_tab; float mscroll; ScrollBar msb; float mtotal;
    PopupMenu menu, types; TdDoc *menu_doc;
    char type_labels[64][96];
    Input stash; bool stashed;
    int seq;
} G;

/* ---- helpers ---- */

static void sel(TdDoc *d, int kind, int id);
static int parse_cols(TdDoc *d, const char *s, int *ids, bool *desc, int max, char *bad, int bn);

static double now_s(void) { return SDL_GetTicks() / 1000.0; }
static Connection *conn_of(App *a, const char *id) { for (int i = 0; a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->id, id)) return a->conns->items[i]; return NULL; }
static bool alive(TdDoc *d) { for (int i = 0; i < MAX_TDOCS; i++) if (G.docs[i] == d && d) return true; return false; }
static void mkdir_p(const char *p) { char b[1200]; snprintf(b, sizeof b, "%s", p); for (char *s = b + 1; *s; s++) if (*s == '/') { *s = 0; mkdir(b, 0700); *s = '/'; } mkdir(b, 0700); }
static void pref_dir(App *a, char *o, int n) { snprintf(o, (size_t)n, "%s", a->conns && a->conns->dir[0] ? a->conns->dir : "."); size_t L = strlen(o); if (L > 1 && o[L - 1] == '/') o[L - 1] = 0; }
static void sanitize(char *s) { for (; *s; s++) if (*s == '/' || *s == ' ' || *s == '\\' || *s == ':') *s = '_'; }
static void draft_path(App *a, TdDoc *d, char *o, int n) {
    char dir[1100]; pref_dir(a, dir, sizeof dir);
    char f[400]; snprintf(f, sizeof f, "%s.%s", d->is_new ? d->draft.schema : d->schema, d->is_new ? "new-table" : d->name); sanitize(f);
    char cid[64]; snprintf(cid, sizeof cid, "%s", d->conn_id); sanitize(cid);
    snprintf(o, (size_t)n, "%s/table-drafts/%s/%s.draft", dir, cid, f);
}
bool tdes_supported(const Connection *c) { return c && td_dialect_for_driver(c->driver) >= 0; }
static bool read_only(TdDoc *d) { return d->state != DS_READY || (!d->is_new && (!d->orig.can_alter || d->orig.special[0])); }
static void set_msg(TdDoc *d, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsnprintf(d->msg, sizeof d->msg, fmt, ap); va_end(ap); }
static void title_of(TdDoc *d, char *o, int n) { snprintf(o, (size_t)n, "%s%s", d->is_new ? "New: " : "", d->draft.name[0] ? d->draft.name : d->name); }
static void qualified(TdDoc *d, const TdTable *t, char *o, int n) { snprintf(o, (size_t)n, "%s%s%s", t->schema, t->schema[0] ? "." : "", t->name); }

/* ---- undo (TBL-007): a copy of the draft before each change; typing in one field is one step ---- */

static void push_undo(TdDoc *d, const char *key) {
    if (key && key[0] && !strcmp(key, d->undo_key)) return;   /* the same field keeps typing */
    snprintf(d->undo_key, sizeof d->undo_key, "%s", key ? key : "");
    if (d->nundo == UNDO_MAX) { td_free(&d->undo[0]); memmove(d->undo, d->undo + 1, sizeof d->undo[0] * (UNDO_MAX - 1)); d->nundo--; }
    td_copy(&d->undo[d->nundo++], &d->draft);
    for (int i = 0; i < d->nredo; i++) td_free(&d->redo[i]);
    d->nredo = 0;   /* a new change drops the redo history */
}
static void changed(TdDoc *d) { d->rev++; d->approved = 0; }
static void do_undo(TdDoc *d) {
    if (!d->nundo || read_only(d)) return;
    if (d->nredo < UNDO_MAX) td_copy(&d->redo[d->nredo++], &d->draft);
    td_free(&d->draft); d->draft = d->undo[--d->nundo]; memset(&d->undo[d->nundo], 0, sizeof d->undo[0]);
    d->undo_key[0] = 0; changed(d); d->f_loaded_rev = 0;
}
static void do_redo(TdDoc *d) {
    if (!d->nredo || read_only(d)) return;
    if (d->nundo < UNDO_MAX) td_copy(&d->undo[d->nundo++], &d->draft);
    td_free(&d->draft); d->draft = d->redo[--d->nredo]; memset(&d->redo[d->nredo], 0, sizeof d->redo[0]);
    d->undo_key[0] = 0; changed(d); d->f_loaded_rev = 0;
}
static void clear_history(TdDoc *d) {
    for (int i = 0; i < d->nundo; i++) td_free(&d->undo[i]);
    for (int i = 0; i < d->nredo; i++) td_free(&d->redo[i]);
    d->nundo = d->nredo = 0; d->undo_key[0] = 0;
}

/* ---- plan, recomputed per revision only (TBL-093: no network, coalesced) ---- */

static void opts_of(TdDoc *d, TdPlanOpts *o) {
    memset(o, 0, sizeof o[0]);
    o->version = d->version; o->sqlite_fk_on = d->sqlite_fk_on;
    if (d->dialect == TD_PG) snprintf(o->lock_timeout, sizeof o->lock_timeout, "%s", d->lock_timeout.buf);
    for (int i = 0; i < d->nusing; i++) if (d->using_expr[i][0]) { o->using_col[o->nusing] = d->using_col[i]; snprintf(o->using_expr[o->nusing], 256, "%s", d->using_expr[i]); o->nusing++; }
}
static void replan(TdDoc *d) {
    if (d->plan_rev == d->rev && d->script) return;
    td_plan_free(&d->plan); free(d->script); d->script = NULL;
    TdPlanOpts o; opts_of(d, &o);
    td_plan(d->dialect, d->is_new ? NULL : &d->orig, &d->draft, &o, &d->plan);
    d->nchanges = td_diff(d->is_new ? NULL : &d->orig, &d->draft, d->changes, 128);
    if (d->nchanges > 128) d->nchanges = 128;
    d->nissues = td_validate(d->dialect, d->version, d->is_new ? NULL : &d->orig, &d->draft, d->issues, 128);
    if (d->nissues > 128) d->nissues = 128;
    if (d->is_new && d->draft.ncols == 0) d->script = strdup("-- Add the first column to see the CREATE TABLE\n");   /* a fresh table is not an error */
    else if (d->plan.blocked) {
        size_t n = strlen(d->plan.block_reason) + 200; d->script = malloc(n);
        snprintf(d->script, n, "-- No SQL: %s\n-- Fix the draft (or open it in a console); Apply stays disabled.\n", d->plan.block_reason);
    } else if (!d->plan.n) d->script = strdup(d->is_new ? "-- Add columns to see the CREATE TABLE\n" : "-- No change: nothing to run\n");
    else d->script = td_plan_script(d->dialect, &d->plan);
    d->plan_rev = d->rev;
}
static int count_level(TdDoc *d, int level) { int n = 0; for (int i = 0; i < d->nissues; i++) n += d->issues[i].level == level; return n; }
bool tdes_doc_dirty(const TdDoc *d) { return d && (d->is_new ? d->draft.ncols > 0 && d->rev > 1 : !td_equal(&d->orig, &d->draft)); }

/* ---- drafts on disk (TBL-009): no credentials, the fingerprint they were made on ---- */

static void save_draft(App *a, TdDoc *d) {
    char p[1400]; draft_path(a, d, p, sizeof p);
    char dir[1400]; snprintf(dir, sizeof dir, "%s", p); char *sl = strrchr(dir, '/'); if (sl) *sl = 0; mkdir_p(dir);
    TdTable t; td_copy(&t, &d->draft); t.fingerprint = d->is_new ? 0 : d->orig.fingerprint;
    char *s = td_serialize(&t); td_free(&t);
    FILE *f = fopen(p, "w");
    if (f) { fputs(s, f); fclose(f); d->draft_on_disk = true; d->draft_saved_at = now_s(); d->draft_rev_saved = d->rev; }
    free(s);
}
static void delete_draft(App *a, TdDoc *d) { char p[1400]; draft_path(a, d, p, sizeof p); remove(p); d->draft_on_disk = false; }
static char *read_file(const char *p) {
    FILE *f = fopen(p, "r"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > 8 << 20) { fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1); size_t r = fread(b, 1, (size_t)n, f); b[r] = 0; fclose(f);
    return b;
}
/* a draft from an earlier session: restored, compared with the table as it is now (TBL-009.2 / 3) */
static void restore_draft(App *a, TdDoc *d) {
    char p[1400]; draft_path(a, d, p, sizeof p);
    char *s = read_file(p); if (!s) return;
    TdTable t;
    if (td_deserialize(s, &t)) {
        bool same_base = d->is_new || t.fingerprint == d->orig.fingerprint;
        push_undo(d, "restore");
        td_free(&d->draft); d->draft = t; d->draft.fingerprint = d->orig.fingerprint; d->draft.exists = !d->is_new;
        if (!d->is_new) { d->draft.can_alter = d->orig.can_alter; snprintf(d->draft.special, sizeof d->draft.special, "%s", d->orig.special); snprintf(d->draft.triggers, sizeof d->draft.triggers, "%s", d->orig.triggers); }
        changed(d); d->f_loaded_rev = 0; d->draft_on_disk = true;
        snprintf(d->banner, sizeof d->banner, same_base ? "A saved draft was restored. Review it, or Discard Draft from the menu."
                                                        : "A saved draft was restored, but the table changed on the server since it was saved: review the diff before applying (or Discard Draft).");
    } else td_free(&t);
    free(s);
}

/* ---- jobs ---- */

static void job_free(TdDoc *d) { if (d->job) { exec_free(d->job); d->job = NULL; } }
static bool start_list(App *a, TdDoc *d, const char *const *sql, int n, int phase, bool stop_on_error) {
    Connection *c = conn_of(a, d->conn_id);
    if (c && !c->db && (c->state == CONN_CONNECTING || c->state == CONN_RECONNECTING)) { d->retry_at = now_s() + 0.3; set_msg(d, "Waiting for the connection to %s...", d->conn_name); return false; }   /* a client library may take seconds */
    if (!c || !c->db) { set_msg(d, "Not connected: connect to %s.", d->conn_name); return false; }
    for (int i = 0; i < n; i++) if (phase != PH_RUN && phase != PH_END && caps_sql_may_write(sql[i], -1) && strncasecmp(sql[i], "PRAGMA", 6)) { set_msg(d, "Refused: a catalog read must be a plain read"); return false; }
    job_free(d);
    d->job = exec_start_list(c->db, sql, n, 5000, stop_on_error, false);
    if (!d->job) { d->retry_at = now_s() + 0.4; return false; }   /* the session is busy: try again shortly */
    d->phase = phase; d->retry_at = 0;
    return true;
}
static void start_snapshot(App *a, TdDoc *d, int phase, const char *schema, const char *name) {
    TdSql q; td_snapshot_sql(d->dialect, schema, name, &q);
    if (d->dialect == TD_SQLITE && q.n < TD_SNAP_MAX) { q.sql[q.n++] = strdup("SELECT foreign_keys FROM pragma_foreign_keys"); }
    if (!start_list(a, d, (const char *const *)q.sql, q.n, phase, false) && !d->retry_at) { if (phase == PH_LOAD) d->state = DS_LOAD_FAILED; }
    if (d->retry_at) d->phase = phase;
    td_sql_free(&q);
}
/* parses the snapshot job's results into t; false with d->msg */
static bool take_snapshot(TdDoc *d, const char *schema, const char *name, TdTable *t) {
    ResultSet r[TD_SNAP_MAX]; ResultSet *rp[TD_SNAP_MAX]; int n = 0;
    char err[400] = "";
    for (int i = 0; i < d->job->n && i < TD_SNAP_MAX; i++) {
        rs_init(&r[i]);
        if (!exec_take_result(d->job, i, &r[i]) || !r[i].ok) { if (!err[0]) snprintf(err, sizeof err, "catalog read %d: %s", i + 1, r[i].error[0] ? r[i].error : "did not run"); }
        rp[i] = &r[i]; n++;
    }
    bool ok = !err[0];
    int ns = d->dialect == TD_SQLITE ? n - 1 : n;
    if (ok) ok = td_snapshot_parse(d->dialect, schema, name, rp, ns, t, err, sizeof err);
    if (ok && d->dialect == TD_SQLITE && n > 0 && r[n - 1].nrows) d->sqlite_fk_on = rs_cell(&r[n - 1], 0, 0) && !strcmp(rs_cell(&r[n - 1], 0, 0), "1");
    if (ok && d->dialect == TD_SQLITE) { const char *v = r[4].nrows ? rs_cell(&r[4], 0, 0) : NULL; int x = 0, y = 0, z = 0; if (v && sscanf(v, "%d.%d.%d", &x, &y, &z) >= 2) d->version = x * 10000 + y * 100 + z; }
    if (ok && d->dialect == TD_PG && r[0].nrows) { const char *v = rs_cell(&r[0], 0, 9); int vn = v ? atoi(v) : 0; d->version = vn / 10000 * 10000 + vn % 10000 / 100 * 100; }
    if (ok && (d->dialect == TD_MYSQL || d->dialect == TD_MARIADB) && r[0].nrows) { const char *v = rs_cell(&r[0], 0, 7); int x = 0, y = 0; if (v && sscanf(v, "%d.%d", &x, &y) == 2) d->version = x * 10000 + y * 100; }
    if (ok && d->dialect == TD_MSSQL && r[0].nrows) { const char *v = rs_cell(&r[0], 0, 7); int x = 0, y = 0; if (v && sscanf(v, "%d.%d", &x, &y) == 2) d->version = x * 10000 + y * 100; }
    if (!ok) set_msg(d, "%s", err);
    for (int i = 0; i < n; i++) rs_free(&r[i]);
    return ok;
}

/* ---- Output and audit (TBL-056 / 072) ---- */

static void audit(App *a, TdDoc *d, const char *result) {
    char dir[1100]; pref_dir(a, dir, sizeof dir); strncat(dir, "/audit", sizeof dir - strlen(dir) - 1); mkdir_p(dir);
    char p[1300]; snprintf(p, sizeof p, "%s/table-designer.log", dir);
    FILE *f = fopen(p, "a");
    if (!f) { LOG_W("table designer: the audit line could not be written (%s)", strerror(errno)); return; }   /* the apply already ran: said so, nothing hidden */
    time_t t = time(NULL); struct tm tm; gmtime_r(&t, &tm); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    char q[400]; qualified(d, &d->draft, q, sizeof q);
    const char *user = getenv("USER");
    fprintf(f, "%s deployment=%s user=%s connection=%s dialect=%s table=%s before=%016llx after=%016llx steps=%d/%d result=%s\n", ts, d->deploy_id, user ? user : "?", d->conn_name,
            td_dialect_name(d->dialect), q, (unsigned long long)d->fp_before, (unsigned long long)d->orig.fingerprint, d->ran, d->run.n, result);
    fclose(f);
}

static void finish_apply(App *a, TdDoc *d) {
    Connection *c = conn_of(a, d->conn_id);
    double ms = (now_s() - d->t0) * 1000.0;
    char res[600];
    if (d->apply_ok) snprintf(res, sizeof res, "Applied: %d statement(s); the table was read back from the server", d->run.n);
    else if (d->run.transactional) snprintf(res, sizeof res, "Step %d failed. %s", d->failed_at + 1, d->rb_confirmed ? "ROLLBACK confirmed: nothing was applied" : "The ROLLBACK could not be confirmed: the table as read back is shown");
    else snprintf(res, sizeof res, "Step %d failed. Steps 1..%d stayed applied (%s commits DDL implicitly; there is no rollback); steps %d..%d were not run", d->failed_at + 1, d->ran, td_dialect_name(d->dialect), d->failed_at + 1, d->run.n);
    set_msg(d, "%s", res);
    if (c) {
        output_obj_event(a->output, a, c, d->out_exec, 0, d->apply_ok ? OL_SUCCESS : OL_ERROR, res, NULL);
        output_obj_finish(a->output, a, c, d->out_exec, d->apply_ok ? ES_COMPLETED : ES_FAILED, ms);
    }
    audit(a, d, d->apply_ok ? "applied" : d->run.transactional && d->rb_confirmed ? "failed-rolled-back" : "failed-partial");
    LOG_I("table designer: deployment %s on %s: %s (%d/%d)", d->deploy_id, d->conn_name, d->apply_ok ? "applied" : "failed", d->ran, d->run.n);
    app_status(a, "%s", res);
    /* TBL-064: the Explorer, completion and the object lists see the new structure */
    if (c && c->db && !c->db->busy) { objed_list_invalidate(c); conn_refresh(c); if (a->active == c) app_refresh_schema(a); NsState *s = nsx_find(c->id); if (s) nsx_refresh_schema(s, d->draft.schema); explorer_rebuild(a->explorer, a); }
    td_plan_free(&d->run);
}

/* RUN: [PRAGMA foreign_keys = OFF] [BEGIN] steps; stop at the first failure */
static void start_run(App *a, TdDoc *d) {
    Connection *c = conn_of(a, d->conn_id);
    const char *list[TD_MAXSTEPS + 3]; int n = 0;
    if (d->run.fk_toggle) list[n++] = "PRAGMA foreign_keys = OFF";
    if (d->run.transactional) list[n++] = d->dialect == TD_MSSQL ? "BEGIN TRANSACTION" : "BEGIN";
    d->pre = n;
    for (int i = 0; i < d->run.n; i++) list[n++] = d->run.steps[i].sql;
    if (!start_list(a, d, list, n, PH_RUN, true)) {
        d->state = DS_READY; set_msg(d, "The session is busy: nothing was run; try Apply again.");
        if (c) output_obj_finish(a->output, a, c, d->out_exec, ES_FAILED, 0);
        td_plan_free(&d->run);
    }
}

static void start_datacheck(App *a, TdDoc *d);
/* the job finished: next phase */
static void poll_doc(App *a, TdDoc *d) {
    if (!d->job) {
        if (d->retry_at > 0 && now_s() >= d->retry_at) {
            d->retry_at = 0;
            if (d->phase == PH_LOAD) start_snapshot(a, d, PH_LOAD, d->schema, d->name);
            else if (d->phase == PH_DATACHECK) start_datacheck(a, d);
        }
        if (d->retry_at > 0) app_wake_in(a, 100);
        return;
    }
    if (exec_running(d->job)) { app_wake_in(a, 50); return; }
    Connection *c = conn_of(a, d->conn_id);
    int ph = d->phase;
    switch (ph) {
    case PH_LOAD: {
        TdTable t;
        if (take_snapshot(d, d->schema, d->name, &t)) {
            td_free(&d->orig); d->orig = t; td_free(&d->draft); td_copy(&d->draft, &d->orig);
            d->state = DS_READY; changed(d); d->f_loaded_rev = 0; d->msg[0] = 0;
            restore_draft(a, d);
            if (d->pending_index[0]) {
                int ids[TD_MAXKC]; bool ds[TD_MAXKC]; char bad[128];
                int n = parse_cols(d, d->pending_index, ids, ds, TD_MAXKC, bad, sizeof bad);
                if (n) {
                    push_undo(d, "proposal");
                    TdKey *k = td_add_key(&d->draft, TK_INDEX, d->pending_index_name);
                    for (int i = 0; i < n; i++) { k->cols[k->ncols] = ids[i]; k->desc[k->ncols++] = ds[i]; }
                    changed(d); sel(d, SEL_KEY, k->id);
                    snprintf(d->banner, sizeof d->banner, "Index proposed by the execution plan analysis (%s). It is a hypothesis: review its cost and the existing indexes before applying.", d->pending_why);
                } else snprintf(d->banner, sizeof d->banner, "The proposed index names a column this table does not have (%s): nothing was added.", bad);
                d->pending_index[0] = 0;
            }
            if (d->after == 1) {   /* Generate SQL > CREATE: a copy in a SQL tab, never run */
                TdPlan p; TdPlanOpts o; memset(&o, 0, sizeof o); o.version = d->version;
                td_plan(d->dialect, NULL, &d->orig, &o, &p);
                char *s = td_plan_script(d->dialect, &p); td_plan_free(&p);
                char title[160]; snprintf(title, sizeof title, "%s.sql", d->name);
                panels_open_tab(a->panels, a, title, d->conn_id, s); free(s);
            } else if (d->after == 2) {   /* Duplicate Structure: a new draft, no data (TBL-049) */
                d->is_new = true; td_free(&d->orig); td_init(&d->orig);
                d->draft.exists = false; snprintf(d->draft.name, sizeof d->draft.name, "%s_copy", d->name);
                for (int i = 0; i < d->draft.nkeys; i++) { d->draft.keys[i].name[0] = 0; if (d->draft.keys[i].raw[0]) { td_del_key(&d->draft, d->draft.keys[i].id); i--; } }
                d->draft.deps[0] = 0; d->draft.can_alter = true; d->draft.special[0] = 0; d->draft.fingerprint = 0;
                snprintf(d->banner, sizeof d->banner, "Duplicate of %s: structure only (no rows). Key names are left to the engine; rename the table before applying.", d->name);
                changed(d);
            } else if (d->after == 3) { d->drop_mode = true; G.modal = M_REVIEW; G.mdoc = d; d->consent = false; textfield_set(&d->prod_name, ""); }
            d->after = 0;
        } else d->state = DS_LOAD_FAILED;   /* never an empty "valid" designer (TBL-002.3) */
        job_free(d);
        break;
    }
    case PH_DRIFT: {
        TdTable t;
        bool ok = take_snapshot(d, d->schema, d->name, &t);
        job_free(d);
        if (!ok || t.fingerprint != d->orig.fingerprint) {
            d->state = DS_READY; d->drift = true;
            if (ok) { td_free(&t); }
            snprintf(d->banner, sizeof d->banner, ok ? "The table changed on the server since it was opened: Apply was stopped. Reload to see the new definition and review your draft against it."
                                                      : "The table could not be read again before Apply: nothing was run.");
            output_obj_event(a->output, a, c, d->out_exec, 0, OL_WARNING, d->banner, NULL);
            output_obj_finish(a->output, a, c, d->out_exec, ES_FAILED, (now_s() - d->t0) * 1000);
            td_plan_free(&d->run);
            break;
        }
        td_free(&t);
        goto run;
    }
    case PH_RUN: {
        /* which statement stopped (a data-check PRAGMA that returned rows counts as a failure) */
        d->ran = 0; d->failed_at = -1; d->failed_err[0] = 0;
        for (int i = 0; i < d->job->n; i++) {
            int step = i - d->pre;
            ExecStmt *st = &d->job->stmts[i];
            if (step < 0) { if (st->status == XS_FAILED) { d->failed_at = 0; snprintf(d->failed_err, sizeof d->failed_err, "%s", st->rs.error); break; } continue; }
            if (st->status == XS_OK) {
                if ((d->run.steps[step].flags & TS_DATA_CHECK) && !strncmp(d->run.steps[step].sql, "PRAGMA", 6) && st->rs.nrows > 0) { d->failed_at = step; snprintf(d->failed_err, sizeof d->failed_err, "foreign_key_check returned %d row(s): a foreign key would be broken", st->rs.nrows); break; }
                d->ran++;
                output_obj_event(a->output, a, c, d->out_exec, step + 1, OL_SUCCESS, d->run.steps[step].why[0] ? d->run.steps[step].why : "Statement completed", d->run.steps[step].sql);
            } else if (st->status == XS_FAILED) { d->failed_at = step; snprintf(d->failed_err, sizeof d->failed_err, "%s", st->rs.error); break; }
            else break;
        }
        if (d->failed_at >= 0) {
            output_obj_event(a->output, a, c, d->out_exec, d->failed_at + 1, OL_ERROR, d->failed_err, d->failed_at < d->run.n ? d->run.steps[d->failed_at].sql : NULL);
            for (int s = d->failed_at + 1; s < d->run.n; s++) output_obj_event(a->output, a, c, d->out_exec, s + 1, OL_INFO, "Not run (an earlier step failed)", d->run.steps[s].sql);
        }
        d->apply_ok = d->failed_at < 0 && d->ran == d->run.n;
        job_free(d);
        /* END: COMMIT or ROLLBACK, then foreign keys back on */
        const char *end[3]; int ne = 0;
        if (d->run.transactional) end[ne++] = d->apply_ok ? "COMMIT" : "ROLLBACK";
        if (d->run.fk_toggle) end[ne++] = "PRAGMA foreign_keys = ON";
        if (d->dialect == TD_MSSQL && !d->apply_ok && d->run.transactional) end[ne++] = "SELECT @@TRANCOUNT";
        if (ne) { if (!start_list(a, d, end, ne, PH_END, false)) { d->rb_confirmed = false; goto verify; } break; }
        goto verify;
    }
    case PH_END: {
        if (d->run.transactional) {
            ExecStmt *st = &d->job->stmts[0];
            if (d->apply_ok && st->status != XS_OK) { d->apply_ok = false; d->failed_at = d->run.n - 1; snprintf(d->failed_err, sizeof d->failed_err, "COMMIT failed: %s", st->rs.error); }
            if (!d->apply_ok) {
                d->rb_confirmed = st->status == XS_OK;
                ExecStmt *tc = &d->job->stmts[d->job->n - 1];   /* SQL Server may have rolled back by itself */
                if (!d->rb_confirmed && d->dialect == TD_MSSQL && tc->status == XS_OK && tc->rs.nrows && rs_cell(&tc->rs, 0, 0) && !strcmp(rs_cell(&tc->rs, 0, 0), "0")) d->rb_confirmed = true;
                output_obj_event(a->output, a, c, d->out_exec, 0, d->rb_confirmed ? OL_INFO : OL_WARNING, d->rb_confirmed ? "ROLLBACK confirmed by the server" : "ROLLBACK not confirmed: the state is read back from the catalog", NULL);
            }
        }
        job_free(d);
        goto verify;
    }
    case PH_VERIFY: {
        TdTable t;
        const char *nm = d->apply_ok ? d->draft.name : d->is_new ? d->draft.name : d->name;
        bool ok = take_snapshot(d, d->draft.schema, nm, &t);
        job_free(d);
        if (ok) {
            td_free(&d->orig); d->orig = t;
            snprintf(d->schema, sizeof d->schema, "%s", t.schema); snprintf(d->name, sizeof d->name, "%s", t.name);
            if (d->apply_ok) { td_free(&d->draft); td_copy(&d->draft, &d->orig); clear_history(d); delete_draft(a, d); d->is_new = false; }
            else if (d->is_new) { td_free(&d->orig); td_init(&d->orig); }
        } else if (d->is_new && !d->apply_ok) { /* nothing was created: the draft stays a new table */ }
        else if (!d->apply_ok) snprintf(d->banner, sizeof d->banner, "The table could not be read back after the failure: Reload before trying again.");
        d->state = DS_READY; d->drift = false; d->approved = 0; changed(d); d->f_loaded_rev = 0;
        { char ttl[160]; title_of(d, ttl, sizeof ttl); (void)ttl; }
        finish_apply(a, d);
        break;
    }
    case PH_DATACHECK: {
        int w = (int)strlen(d->datacheck);
        for (int i = 0; i < d->job->n && i < d->dc_n; i++) {
            ExecStmt *st = &d->job->stmts[i];
            w += snprintf(d->datacheck + w, sizeof d->datacheck - (size_t)w, "%s: %s\n", d->dc_what[i],
                          st->status != XS_OK ? st->rs.error : st->rs.nrows ? "rows that do not fit exist: the step will fail" : "no violating row found");
        }
        job_free(d);
        break;
    }
    }
    return;
run:
    start_run(a, d);
    return;
verify:
    d->phase = PH_VERIFY;
    start_snapshot(a, d, PH_VERIFY, d->draft.schema, d->apply_ok || d->is_new ? d->draft.name : d->name);
    if (!d->job && !d->retry_at) { d->state = DS_READY; finish_apply(a, d); }
}

/* Apply Changes in the review: only the reviewed plan, only with the approval of this revision */
static void apply(App *a, TdDoc *d) {
    Connection *c = conn_of(a, d->conn_id);
    if (!c || !c->db) { set_msg(d, "Not connected: connect to %s to apply.", d->conn_name); return; }
    replan(d);
    if (d->plan.blocked || !d->plan.n) return;
    if (td_plan_hash(&d->plan, d->rev) != d->approved) { set_msg(d, "The draft changed after the review: review it again."); return; }
    if (c->read_only) { set_msg(d, "%s is read-only in Lume: structure changes are refused (policy checked at execution)", c->name); LOG_I("table designer: refused on read-only connection %s", c->name); return; }   /* TBL-071 */
    td_plan_free(&d->run);
    d->run = d->plan;   /* owned copy of the steps */
    for (int i = 0; i < d->plan.n; i++) d->run.steps[i].sql = strdup(d->plan.steps[i].sql);
    snprintf(d->deploy_id, sizeof d->deploy_id, "%lx-%d", (long)time(NULL), ++G.seq);
    char q[400]; qualified(d, &d->draft, q, sizeof q);
    char t[600]; snprintf(t, sizeof t, "Table Designer: %s %s \xC2\xB7 %d step(s) \xC2\xB7 deployment %s", d->is_new ? "create" : "alter", q, d->run.n, d->deploy_id);
    d->out_exec = output_obj_begin(a->output, a, c, t, d->run.n);
    for (int i = 0; i < d->run.nwarn; i++) output_obj_event(a->output, a, c, d->out_exec, 0, OL_INFO, d->run.warnings[i], NULL);
    d->t0 = now_s(); d->state = DS_APPLYING; d->apply_ok = false; d->rb_confirmed = false; d->ran = 0; d->failed_at = -1;
    d->fp_before = d->orig.fingerprint;
    set_msg(d, "Applying %d statement(s)...", d->run.n);
    LOG_I("table designer: deployment %s of %s on %s started (%d statement(s))", d->deploy_id, q, d->conn_name, d->run.n);
    G.modal = M_NONE;
    if (d->is_new) { start_run(a, d); return; }
    /* drift first (TBL-057): the table must be the one the draft was made on */
    start_snapshot(a, d, PH_DRIFT, d->schema, d->name);
    if (!d->job && !d->retry_at) start_run(a, d);
}

/* the Drop Table flow (TBL-083): its own reviewed statement */
static void apply_drop(App *a, TdDoc *d) {
    Connection *c = conn_of(a, d->conn_id);
    if (!c || !c->db || c->read_only) { set_msg(d, c && c->read_only ? "%s is read-only in Lume" : "Not connected", d->conn_name); return; }
    td_plan_free(&d->run); memset(&d->run, 0, sizeof d->run);
    char qt[600]; td_qname(d->dialect, d->dialect == TD_SQLITE && !d->orig.schema[0] ? "main" : d->orig.schema, d->orig.name, qt, sizeof qt);
    char *s = malloc(700); snprintf(s, 700, "DROP TABLE %s", qt);
    d->run.steps[0].sql = s; d->run.steps[0].flags = TS_DESTRUCTIVE; snprintf(d->run.steps[0].why, sizeof d->run.steps[0].why, "drop the table and all its rows"); d->run.n = 1;
    d->run.transactional = td_caps(d->dialect, d->version).transactional_ddl == TCAP_YES;
    snprintf(d->deploy_id, sizeof d->deploy_id, "%lx-%d", (long)time(NULL), ++G.seq);
    char t[700]; snprintf(t, sizeof t, "Table Designer: drop %s.%s \xC2\xB7 deployment %s", d->orig.schema, d->orig.name, d->deploy_id);
    d->out_exec = output_obj_begin(a->output, a, c, t, 1);
    d->t0 = now_s(); d->state = DS_APPLYING; d->fp_before = d->orig.fingerprint; G.modal = M_NONE; d->drop_mode = false;
    start_run(a, d);
}

/* optional, bounded checks of existing rows for NOT NULL / UNIQUE additions (TBL-068) */
static void start_datacheck(App *a, TdDoc *d) {
    d->dc_n = 0; d->datacheck[0] = 0;
    char qt[600]; td_qname(d->dialect, d->dialect == TD_SQLITE && !d->orig.schema[0] ? "main" : d->orig.schema, d->orig.name, qt, sizeof qt);
    for (int i = 0; i < d->draft.ncols && d->dc_n < 8; i++) {
        const TdCol *b = &d->draft.cols[i], *o = td_col_c(&d->orig, b->id);
        if (!o || !b->notnull || o->notnull) continue;
        char qc[300]; td_quote(d->dialect, o->name, qc, sizeof qc);
        snprintf(d->dc_sql[d->dc_n], sizeof d->dc_sql[0], "SELECT 1 FROM %s WHERE %s IS NULL", qt, qc);
        snprintf(d->dc_what[d->dc_n++], sizeof d->dc_what[0], "NOT NULL %s", b->name);
    }
    for (int i = 0; i < d->draft.nkeys && d->dc_n < 8; i++) {
        const TdKey *k = &d->draft.keys[i];
        if ((k->kind != TK_UNIQUE && k->kind != TK_PK && !(k->kind == TK_INDEX && k->unique)) || td_key(&d->orig, k->id)) continue;
        char cols[1000] = ""; int w = 0;
        for (int j = 0; j < k->ncols; j++) { const TdCol *c = td_col_c(&d->orig, k->cols[j]); if (!c) { w = 0; break; } char qc[300]; td_quote(d->dialect, c->name, qc, sizeof qc); w += snprintf(cols + w, sizeof cols - (size_t)w, "%s%s", j ? ", " : "", qc); }
        if (!w) continue;
        snprintf(d->dc_sql[d->dc_n], sizeof d->dc_sql[0], "SELECT %s FROM %s GROUP BY %s HAVING COUNT(*) > 1", cols, qt, cols);
        snprintf(d->dc_what[d->dc_n++], sizeof d->dc_what[0], "%s %s", k->kind == TK_PK ? "primary key" : "unique", k->name[0] ? k->name : "(new)");
    }
    if (!d->dc_n) { snprintf(d->datacheck, sizeof d->datacheck, "Nothing to check: no new NOT NULL / UNIQUE / primary key on existing columns.\n"); return; }
    const char *l[8]; for (int i = 0; i < d->dc_n; i++) l[i] = d->dc_sql[i];
    if (start_list(a, d, l, d->dc_n, PH_DATACHECK, false)) {
        if (d->orig.approx_rows >= 0) snprintf(d->datacheck, sizeof d->datacheck, "Checking (reads the table, about %lld row(s))...\n", d->orig.approx_rows);
        else snprintf(d->datacheck, sizeof d->datacheck, "Checking (reads the table; its number of rows is unknown)...\n");
    }
}

/* ---- opening ---- */

static TdDoc *doc_new(App *a, Connection *c) {
    int slot = -1; for (int i = 0; i < MAX_TDOCS; i++) if (!G.docs[i]) { slot = i; break; }
    if (slot < 0) { app_status(a, "Too many table designer tabs open (%d)", MAX_TDOCS); return NULL; }
    TdDoc *d = calloc(1, sizeof *d);
    snprintf(d->conn_id, sizeof d->conn_id, "%s", c->id); snprintf(d->conn_name, sizeof d->conn_name, "%s", c->name);
    d->dialect = td_dialect_for_driver(c->driver); d->rev = 1; d->preview_open = true; d->sel_kind = SEL_TABLE;
    td_init(&d->orig); td_init(&d->draft);
    G.docs[slot] = d;
    return d;
}
static void open_tab(App *a, TdDoc *d) {
    char title[160]; title_of(d, title, sizeof title);
    d->tab_id = panels_open_designer_tab(a->panels, a, title, d->conn_id, d);
    struct Editor *ed = panels_tab_editor(a->panels, d->tab_id);
    if (ed) { editor_set_object_mode(ed, true); editor_set_read_only(ed, true); editor_set_dialect(ed, dialect_for_driver(conn_of(a, d->conn_id)->driver)); }
}
bool tdes_open_table(App *a, Connection *c, const char *schema, const char *name, int after) {
    if (!tdes_supported(c)) { app_status(a, "No table designer for %s", c ? c->driver : "this connection"); return false; }
    /* the same table already open: that tab, with a note about concurrent tabs (TBL-008.2) */
    for (int i = 0; i < MAX_TDOCS; i++) {
        TdDoc *o = G.docs[i];
        if (o && !o->is_new && !strcmp(o->conn_id, c->id) && !strcmp(o->schema, schema) && !strcmp(o->name, name) && after == 0 && panels_tab_exists(a->panels, o->tab_id)) { panels_activate_tab(a->panels, a, o->tab_id); return true; }
    }
    TdDoc *d = doc_new(a, c);
    if (!d) return false;
    snprintf(d->schema, sizeof d->schema, "%s", schema); snprintf(d->name, sizeof d->name, "%s", name);
    snprintf(d->draft.schema, sizeof d->draft.schema, "%s", schema); snprintf(d->draft.name, sizeof d->draft.name, "%s", name);
    d->state = DS_LOADING; d->after = after;
    set_msg(d, "Reading the definition of %s.%s (catalog only, no rows)...", schema, name);
    open_tab(a, d);
    start_snapshot(a, d, PH_LOAD, schema, name);
    if (d->state == DS_LOAD_FAILED) set_msg(d, "%s", d->msg);
    LOG_I("table designer: open %s.%s on %s", schema, name, c->name);
    return true;
}
bool tdes_open_with_index(App *a, Connection *c, const char *schema, const char *name, const char *cols, const char *iname, const char *why) {
    if (!tdes_open_table(a, c, schema, name, 0)) return false;
    for (int i = 0; i < MAX_TDOCS; i++) {
        TdDoc *d = G.docs[i];
        if (d && !d->is_new && !strcmp(d->conn_id, c->id) && !strcmp(d->schema, schema) && !strcmp(d->name, name)) {
            snprintf(d->pending_index, sizeof d->pending_index, "%s", cols); snprintf(d->pending_index_name, sizeof d->pending_index_name, "%s", iname ? iname : "");
            snprintf(d->pending_why, sizeof d->pending_why, "%s", why ? why : "");
            if (d->state == DS_READY) { d->state = DS_LOADING; start_snapshot(a, d, PH_LOAD, d->schema, d->name); }   /* an open tab: re-read so the proposal lands on the current definition */
            return true;
        }
    }
    return true;
}
bool tdes_new_table(App *a, Connection *c, const char *schema) {
    if (!tdes_supported(c)) { app_status(a, "No table designer for %s", c ? c->driver : "this connection"); return false; }
    TdDoc *d = doc_new(a, c);
    if (!d) return false;
    d->is_new = true; d->state = DS_READY;
    snprintf(d->draft.schema, sizeof d->draft.schema, "%s", schema ? schema : "");
    snprintf(d->draft.name, sizeof d->draft.name, "new_table");
    if (d->dialect == TD_ORACLE) for (char *p = d->draft.name; *p; p++) *p = (char)toupper((unsigned char)*p);
    snprintf(d->schema, sizeof d->schema, "%s", d->draft.schema);
    d->draft.can_alter = true;
    open_tab(a, d);
    restore_draft(a, d);
    LOG_I("table designer: new table in %s on %s", schema ? schema : "(default)", c->name);
    return true;
}

bool tdes_new_table_from(App *a, Connection *c, const TdTable *t, const char *note) {
    if (!tdes_supported(c)) { app_status(a, "No table designer for %s", c ? c->driver : "this connection"); return false; }
    TdDoc *d = doc_new(a, c);
    if (!d) return false;
    d->is_new = true; d->state = DS_READY;
    td_free(&d->draft); td_copy(&d->draft, t);
    d->draft.exists = false; d->draft.can_alter = true;
    if (d->dialect == TD_ORACLE) for (char *p = d->draft.name; *p; p++) *p = (char)toupper((unsigned char)*p);
    snprintf(d->schema, sizeof d->schema, "%s", d->draft.schema);
    open_tab(a, d);
    changed(d);
    if (note && *note) snprintf(d->msg, sizeof d->msg, "%s", note);
    LOG_I("table designer: new table from a prepared draft on %s (%d columns)", c->name, d->draft.ncols);
    return true;
}
/* ---- mutations (TBL-011..049): always the draft, never the server ---- */

static void sel(TdDoc *d, int kind, int id) { d->sel_kind = kind; d->sel_id = id; d->f_loaded_rev = 0; }
static TdCol *add_column(TdDoc *d, int after_id) {
    push_undo(d, "add-col");
    char nm[128]; td_unique_col_name(&d->draft, d->dialect == TD_ORACLE ? "NEW_COLUMN" : "new_column", nm, sizeof nm);
    TdCol *c = td_add_col(&d->draft, nm, td_default_type(d->dialect));
    int id = c->id;
    if (after_id) { int at = td_col_index(&d->draft, after_id); while (at >= 0 && td_col_index(&d->draft, id) > at + 1) td_move_col(&d->draft, id, -1); }
    changed(d); sel(d, SEL_COL, id);
    return td_col(&d->draft, id);
}
static TdKey *add_key(TdDoc *d, int kind) {
    if (kind == TK_PK && td_pk(&d->draft)) { sel(d, SEL_KEY, td_pk(&d->draft)->id); return NULL; }
    push_undo(d, "add-key");
    TdKey *k = td_add_key(&d->draft, kind, "");
    if (d->sel_kind == SEL_COL && kind != TK_CHECK && td_col(&d->draft, d->sel_id)) k->cols[k->ncols++] = d->sel_id;
    if (kind == TK_CHECK) snprintf(k->check, sizeof k->check, "%s", "");
    char nm[128]; td_key_name(&d->draft, k, NULL, nm, sizeof nm);
    if (d->dialect == TD_ORACLE) for (char *p = nm; *p; p++) *p = (char)toupper((unsigned char)*p);
    snprintf(k->name, sizeof k->name, "%s", nm);
    int id = k->id;
    changed(d); sel(d, SEL_KEY, id);
    return td_key(&d->draft, id);
}
static void remove_selected(TdDoc *d) {
    if (d->sel_kind == SEL_COL) { push_undo(d, "rm"); td_del_col(&d->draft, d->sel_id); changed(d); sel(d, SEL_TABLE, 0); }
    else if (d->sel_kind == SEL_KEY) { push_undo(d, "rm"); td_del_key(&d->draft, d->sel_id); changed(d); sel(d, SEL_TABLE, 0); }
}
static void duplicate_col(TdDoc *d) {
    TdCol *c = td_col(&d->draft, d->sel_id); if (!c) return;
    push_undo(d, "dup");
    TdCol copy = *c;
    char nm[128]; char base[140]; snprintf(base, sizeof base, "%s_copy", c->name); td_unique_col_name(&d->draft, base, nm, sizeof nm);
    TdCol *n = td_add_col(&d->draft, nm, copy.type);
    int id = n->id; *n = copy; n->id = id; snprintf(n->name, sizeof n->name, "%s", nm);
    n->identity = 0; n->def_name[0] = 0;   /* never an identity / PK copy (TBL-024.3) */
    changed(d); sel(d, SEL_COL, id);
}
static void restore_removed(TdDoc *d) {
    if (d->sel_kind == SEL_GONE_COL) { const TdCol *o = td_col_c(&d->orig, d->sel_id); if (!o) return; push_undo(d, "restore"); TdCol *n = td_add_col(&d->draft, o->name, o->type); int nid = n->id; *n = *o; (void)nid; d->draft.next_id--; changed(d); sel(d, SEL_COL, o->id); }
    else if (d->sel_kind == SEL_GONE_KEY) { for (int i = 0; i < d->orig.nkeys; i++) if (d->orig.keys[i].id == d->sel_id) { push_undo(d, "restore"); TdKey *k = td_add_key(&d->draft, d->orig.keys[i].kind, ""); *k = d->orig.keys[i]; d->draft.next_id--; changed(d); sel(d, SEL_KEY, k->id); break; } }
}
/* "a, b DESC" -> column ids; unknown names are reported, not invented */
static int parse_cols(TdDoc *d, const char *s, int *ids, bool *desc, int max, char *bad, int bn) {
    int n = 0; bad[0] = 0;
    char buf[1024]; snprintf(buf, sizeof buf, "%s", s);
    for (char *t = strtok(buf, ","); t && n < max; t = strtok(NULL, ",")) {
        while (*t == ' ') t++;
        char *e = t + strlen(t); while (e > t && e[-1] == ' ') *--e = 0;
        bool ds = false; if (e - t > 5 && !strcasecmp(e - 5, " DESC")) { e[-5] = 0; ds = true; }
        if (!*t) continue;
        int id = 0; for (int i = 0; i < d->draft.ncols; i++) if (!strcmp(d->draft.cols[i].name, t)) id = d->draft.cols[i].id;
        if (!id) { snprintf(bad, (size_t)bn, "%s", t); continue; }
        ids[n] = id; if (desc) desc[n] = ds; n++;
    }
    return n;
}
static void cols_text(TdDoc *d, const int *ids, const bool *desc, int n, char *o, int on) {
    int w = 0; o[0] = 0;
    for (int i = 0; i < n && w < on - 1; i++) { const TdCol *c = td_col_c(&d->draft, ids[i]); w += snprintf(o + w, (size_t)(on - w), "%s%s%s", i ? ", " : "", c ? c->name : "?", desc && desc[i] ? " DESC" : ""); }
}

/* ---- tree (TBL-004 / 094): rows of the draft plus what was removed ---- */

typedef struct Row { int kind, id, grp; char label[300]; char mark; } Row;   /* kind: 0 table, 1 group, 2 col, 3 key, 4 gone col, 5 gone key */
static int build_rows(TdDoc *d, Row *rows, int max) {
    int n = 0;
    const char *flt = d->filter.buf;
#define ROW(k, i, g, m, ...) do { if (n < max) { rows[n].kind = k; rows[n].id = i; rows[n].grp = g; rows[n].mark = m; snprintf(rows[n].label, sizeof rows[n].label, __VA_ARGS__); n++; } } while (0)
    ROW(0, 0, 0, d->is_new ? '+' : strcmp(d->orig.name, d->draft.name) || strcmp(d->orig.comment, d->draft.comment) ? '~' : ' ', "%s", d->draft.name[0] ? d->draft.name : "(no name)");
    int ncol = d->draft.ncols;
    ROW(1, 0, TK_NKINDS, ' ', "Columns (%d)", ncol);
    if (d->is_new && ncol == 0 && !read_only(d)) ROW(6, 0, TK_NKINDS, ' ', "Add the first column");   /* the empty state's action, a row of the tree */
    if (!d->closed_grp[TK_NKINDS]) {
        for (int i = 0; i < d->draft.ncols; i++) {
            const TdCol *c = &d->draft.cols[i], *o = d->is_new ? NULL : td_col_c(&d->orig, c->id);
            if (flt[0] && !strcasestr(c->name, flt)) continue;   /* filters, keeps the real order (TBL-094.2) */
            char m = !o ? '+' : !td_col_same(o, c) ? '~' : ' ';
            bool pk = td_col_in_kind(&d->draft, c->id, TK_PK), fk = td_col_in_kind(&d->draft, c->id, TK_FK);
            ROW(2, c->id, TK_NKINDS, m, "%s%s  %s%s%s", c->name, pk ? "  [PK]" : "", c->gen[0] ? "AS (...)" : c->type, fk ? "  [FK]" : "", c->notnull ? "  not null" : "");
        }
        for (int i = 0; !d->is_new && i < d->orig.ncols; i++) if (!td_col_c(&d->draft, d->orig.cols[i].id)) ROW(4, d->orig.cols[i].id, TK_NKINDS, '-', "%s  (removed)", d->orig.cols[i].name);
    }
    for (int g = 0; g < TK_NKINDS; g++) {
        int cnt = 0; for (int i = 0; i < d->draft.nkeys; i++) cnt += d->draft.keys[i].kind == g && !d->draft.keys[i].backs_constraint;
        static const char *gn[] = { "Primary Key", "Unique Keys", "Foreign Keys", "Checks", "Indexes" };
        ROW(1, 0, g, ' ', "%s (%d)", gn[g], cnt);
        if (d->closed_grp[g]) continue;
        for (int i = 0; i < d->draft.nkeys; i++) {
            const TdKey *k = &d->draft.keys[i];
            if (k->kind != g || k->backs_constraint) continue;
            const TdKey *o = NULL; for (int j = 0; !d->is_new && j < d->orig.nkeys; j++) if (d->orig.keys[j].id == k->id) o = &d->orig.keys[j];
            char m = !o ? '+' : !td_key_same(o, k, true) ? '~' : ' ';
            char cols[300]; cols_text(d, k->cols, k->desc, k->ncols, cols, sizeof cols);
            if (g == TK_CHECK) ROW(3, k->id, g, m, "%s  %s", k->name[0] ? k->name : "(unnamed)", k->raw[0] ? k->raw : k->check);
            else if (g == TK_FK) ROW(3, k->id, g, m, "%s  (%s) -> %s.%s", k->name[0] ? k->name : "(unnamed)", cols, k->ref_schema, k->ref_table);
            else ROW(3, k->id, g, m, "%s  (%s)%s%s", k->name[0] ? k->name : "(unnamed)", k->raw[0] ? "not modelled" : cols, k->unique && g == TK_INDEX ? " unique" : "", k->where[0] ? " partial" : "");
        }
        for (int i = 0; !d->is_new && i < d->orig.nkeys; i++) {
            const TdKey *o = &d->orig.keys[i];
            if (o->kind != g || o->backs_constraint) continue;
            bool kept = false; for (int j = 0; j < d->draft.nkeys; j++) kept |= d->draft.keys[j].id == o->id;
            if (!kept) ROW(5, o->id, g, '-', "%s  (removed)", o->name);
        }
    }
#undef ROW
    return n;
}

/* ---- the tab ---- */

static Rect hdr_rect, footer_rect;
static int count_lines(const char *s) { int n = 0; for (; *s; s++) n += *s == '\n'; return n; }
static const char *td_risk_name(int r) { return r == RISK_HIGH ? "HIGH" : r == RISK_MEDIUM ? "MEDIUM" : "LOW"; }

/* loads the property fields from the model when the selection or the model changed by itself */
static void load_fields(TdDoc *d) {
    if (d->f_kind == d->sel_kind && d->f_id == d->sel_id && d->f_loaded_rev) return;
    for (int i = 0; i < F_N; i++) textfield_set(&d->f[i], "");
    d->f_kind = d->sel_kind; d->f_id = d->sel_id; d->f_loaded_rev = d->rev;
    if (d->sel_kind == SEL_TABLE) {
        textfield_set(&d->f[F_NAME], d->draft.name); textfield_set(&d->f[F_COMMENT], d->draft.comment); textfield_set(&d->f[F_SCHEMA], d->draft.schema);
        textfield_set(&d->f[F_ENGINE], d->draft.engine); textfield_set(&d->f[F_CHARSET], d->draft.charset); textfield_set(&d->f[F_COLL], d->draft.collation);
    } else if (d->sel_kind == SEL_COL) {
        TdCol *c = td_col(&d->draft, d->sel_id); if (!c) return;
        textfield_set(&d->f[F_NAME], c->name); textfield_set(&d->f[F_TYPE], c->type); textfield_set(&d->f[F_DEFAULT], c->def);
        textfield_set(&d->f[F_COMMENT], c->comment); textfield_set(&d->f[F_GEN], c->gen); textfield_set(&d->f[F_COLL], c->collation);
        for (int i = 0; i < d->nusing; i++) if (d->using_col[i] == c->id) textfield_set(&d->f[F_USING], d->using_expr[i]);
    } else if (d->sel_kind == SEL_KEY) {
        TdKey *k = td_key(&d->draft, d->sel_id); if (!k) return;
        char cols[1000];
        textfield_set(&d->f[F_NAME], k->name);
        cols_text(d, k->cols, k->desc, k->ncols, cols, sizeof cols); textfield_set(&d->f[F_KCOLS], cols);
        bool f0[TD_MAXKC] = { false }; cols_text(d, k->include, f0, k->ninclude, cols, sizeof cols); textfield_set(&d->f[F_INCLUDE], cols);
        char rt[300]; snprintf(rt, sizeof rt, "%s%s%s", k->ref_schema, k->ref_schema[0] ? "." : "", k->ref_table); textfield_set(&d->f[F_REFTAB], rt);
        int w = 0; cols[0] = 0; for (int i = 0; i < k->nref; i++) w += snprintf(cols + w, sizeof cols - (size_t)w, "%s%s", i ? ", " : "", k->ref_cols[i]);
        textfield_set(&d->f[F_REFCOLS], cols);
        textfield_set(&d->f[F_WHERE], k->where); textfield_set(&d->f[F_METHOD], k->method); textfield_set(&d->f[F_CHECK], k->check);
    }
}
/* a labelled text field bound to a model string: an edit is a draft change with one undo step per field */
/* The designer's text field with the keyboard (one at a time across docs): a property field F_*,
 * or TF_FILTER. While one has it, the read-only SQL preview (the tab's editor) must not see the
 * typing: it would swallow it as "read-only". */
enum { TF_FILTER = F_N };
static struct { TdDoc *doc; int f; } TF = { NULL, -1 };
static bool tf_focused(App *a, TdDoc *d, int f) { return TF.doc == d && TF.f == f && a->focus == FOCUS_EDITOR; }
static bool tf_any(App *a, TdDoc *d) { return TF.doc == d && TF.f >= 0 && a->focus == FOCUS_EDITOR; }
static void tf_focus(App *a, TdDoc *d, int f) { TF.doc = d; TF.f = f; a->focus = FOCUS_EDITOR; }
static bool field(App *a, TdDoc *d, int f, Rect row, const char *label, const char *placeholder, char *model, int msize, bool enabled, const char *hint) {
    float lw = px(a, 130);
    ui_label(a, rect_make(row.x, row.y, lw, row.h), label, ALIGN_LEFT, T_TEXT_MUTED);
    Rect fr = rect_make(row.x + lw, row.y + px(a, 2), row.w - lw, row.h - px(a, 4));
    char tag[80]; snprintf(tag, sizeof tag, "td:%s", label); ui_record(tag, fr);
    if (!enabled) { draw_rrect(a->renderer, fr, px(a, 4), color_hex(T_BG)); ui_label(a, rect_inset(fr, px(a, 8), 0), model[0] ? model : placeholder, ALIGN_LEFT, T_TEXT_DIM); return false; }
    if (ui_textfield(a, &d->f[f], fr, placeholder, tf_focused(a, d, f))) tf_focus(a, d, f);
    (void)hint;
    if (strcmp(d->f[f].buf, model)) {
        char key[64]; snprintf(key, sizeof key, "f%d:%d:%d", f, d->sel_kind, d->sel_id);
        push_undo(d, key);
        snprintf(model, (size_t)msize, "%s", d->f[f].buf);
        changed(d); d->f_loaded_rev = d->rev;
        return true;
    }
    return false;
}
/* The referenced table of a foreign key in the connection's catalog (columns loaded), NULL when unknown. */
static DbTable *ref_table_of(App *a, TdDoc *d, const TdKey *k) {
    Connection *c = conn_of(a, d->conn_id);
    if (!c || !k->ref_table[0]) return NULL;
    char full[300]; snprintf(full, sizeof full, "%s.%s", k->ref_schema, k->ref_table);
    for (int i = 0; i < c->ntables; i++) {
        const char *nm = c->tables[i].name, *dot = strrchr(nm, '.');
        bool hit = !strcasecmp(nm, full) || (!dot && !strcasecmp(nm, k->ref_table)) || (dot && !strcasecmp(dot + 1, k->ref_table) && !strncasecmp(nm, k->ref_schema, (size_t)(dot - nm)) && (size_t)(dot - nm) == strlen(k->ref_schema));
        if (!hit) continue;
        if (!c->tables[i].cols_loaded && c->db && !c->db->busy) conn_load_columns(c, &c->tables[i]);
        return &c->tables[i];
    }
    return NULL;
}
/* A new referenced table: the referenced columns become its primary key (in order), as far as they go. */
static void ref_cols_from_pk(App *a, TdDoc *d, TdKey *k) {
    DbTable *t = ref_table_of(a, d, k); if (!t) return;
    int n = 0;
    for (int i = 0; i < t->ncols && n < TD_MAXKC; i++) if (t->cols[i].pk) snprintf(k->ref_cols[n++], 128, "%s", t->cols[i].name);
    if (!n) return;
    k->nref = k->ncols > n ? k->ncols : n;
    for (int i = n; i < k->nref; i++) k->ref_cols[i][0] = 0;
}
static bool check(App *a, TdDoc *d, Rect row, const char *label, bool *v, bool enabled) {
    char tag[80]; snprintf(tag, sizeof tag, "td:%s", label);
    Rect cr = rect_make(row.x + px(a, 130), row.y, row.w - px(a, 130), row.h); ui_record(tag, cr);
    if (ui_checkbox(a, cr, label, *v, enabled)) { push_undo(d, ""); *v = !*v; changed(d); d->f_loaded_rev = d->rev; return true; }
    return false;
}
static void info(App *a, Rect row, uint32_t color, const char *fmt, ...) {
    char b[600]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    draw_text_fit(a->font_small, a->renderer, b, -1, rect_make(row.x + px(a, 130), row.y, row.w - px(a, 130), row.h), color_hex(color), ALIGN_LEFT);
}

static void draw_props(App *a, TdDoc *d, Rect r) {
    SDL_Renderer *R = a->renderer;
    draw_rect(R, r, color_hex(T_PANEL));
    float rh = px(a, 30);
    Rect row = rect_make(r.x + px(a, 14), r.y + px(a, 8), r.w - px(a, 28), rh);
    bool en = !read_only(d);
    TdCaps caps = td_caps(d->dialect, d->version);
    load_fields(d);
#define NEXT() (row.y += rh)
    if (d->sel_kind == SEL_TABLE) {
        ui_label(a, row, "TABLE", ALIGN_LEFT, T_TEXT_MUTED); NEXT();
        field(a, d, F_NAME, row, "Name", "table name", d->draft.name, sizeof d->draft.name, en, NULL); NEXT();
        field(a, d, F_SCHEMA, row, "Schema", d->dialect == TD_SQLITE ? "main" : "schema", d->draft.schema, sizeof d->draft.schema, en && d->is_new, NULL); NEXT();
        field(a, d, F_COMMENT, row, "Comment", caps.comments == TCAP_NO ? "not supported here" : "what the table holds", d->draft.comment, sizeof d->draft.comment, en && caps.comments != TCAP_NO, NULL); NEXT();
        if (d->dialect == TD_MYSQL || d->dialect == TD_MARIADB) {
            field(a, d, F_ENGINE, row, "Engine", "InnoDB", d->draft.engine, sizeof d->draft.engine, en, NULL); NEXT();
            field(a, d, F_CHARSET, row, "Charset", "server default", d->draft.charset, sizeof d->draft.charset, en, NULL); NEXT();
            field(a, d, F_COLL, row, "Collation", "server default", d->draft.collation, sizeof d->draft.collation, en, NULL); NEXT();
        }
        if (d->is_new && caps.temp == TCAP_YES && d->dialect != TD_MSSQL) { check(a, d, row, d->dialect == TD_ORACLE ? "Global temporary (rows per session)" : "Temporary (dropped at the end of the session)", &d->draft.temporary, en); NEXT(); }
        if (caps.unlogged == TCAP_YES) { check(a, d, row, "Unlogged (lost after a crash, not replicated)", &d->draft.unlogged, en); NEXT(); }
        if (!d->is_new) {
            info(a, row, T_TEXT_DIM, "Rows (catalog estimate): %s", d->orig.approx_rows >= 0 ? "" : "unknown"); if (d->orig.approx_rows >= 0) { char n[32]; snprintf(n, sizeof n, "%lld", d->orig.approx_rows); draw_text_fit(a->font_small, R, n, -1, rect_make(row.x + px(a, 300), row.y, px(a, 200), row.h), color_hex(T_TEXT_DIM), ALIGN_LEFT); } NEXT();
            if (d->orig.tablespace[0]) { info(a, row, T_TEXT_DIM, "Tablespace: %s (kept)", d->orig.tablespace); NEXT(); }
            if (d->orig.extra[0]) { info(a, row, T_YELLOW, "Options not modelled (kept as they are): %s", d->orig.extra); NEXT(); }
            if (d->orig.special[0]) { info(a, row, T_YELLOW, "Read-only: %s", d->orig.special); NEXT(); }
            ui_label(a, row, "DEPENDENTS", ALIGN_LEFT, T_TEXT_MUTED); NEXT();
            const char *p = d->orig.deps; int shown = 0;
            while (*p && row.y < r.y + r.h - rh) { const char *e = strchr(p, '\n'); int l = e ? (int)(e - p) : (int)strlen(p); char ln[300]; snprintf(ln, sizeof ln, "%.*s", l, p); info(a, row, T_TEXT, "%s", ln); NEXT(); shown++; p += l + (e ? 1 : 0); }
            if (!shown) { info(a, row, T_TEXT_DIM, "%s", "none known"); NEXT(); }
            if (d->orig.deps_partial) { info(a, row, T_YELLOW, "%s", "Incomplete: this catalog does not list views that read the table"); NEXT(); }
        }
    } else if (d->sel_kind == SEL_COL) {
        TdCol *c = td_col(&d->draft, d->sel_id); if (!c) { sel(d, SEL_TABLE, 0); return; }
        const TdCol *o = d->is_new ? NULL : td_col_c(&d->orig, c->id);
        ui_label(a, row, o ? "COLUMN" : "COLUMN (new)", ALIGN_LEFT, T_TEXT_MUTED); NEXT();
        field(a, d, F_NAME, row, "Name", "column name", c->name, sizeof c->name, en, NULL); NEXT();
        c = td_col(&d->draft, d->sel_id);
        Rect tr = row; tr.w -= px(a, 34);
        field(a, d, F_TYPE, tr, "Data type", c->gen[0] && d->dialect == TD_MSSQL ? "computed" : "type", c->type, sizeof c->type, en, NULL);
        c = td_col(&d->draft, d->sel_id);
        Rect tb = rect_make(row.x + row.w - px(a, 30), row.y + px(a, 3), px(a, 30), row.h - px(a, 6));
        if (ui_button(a, tb, "...", BTN_QUIET, en)) {   /* native types of the engine, filtered by what is typed (TBL-017) */
            popup_open(&G.types, tb.x, tb.y + tb.h); G.menu_doc = d;
            const char *const *ty = td_types(d->dialect); int k = 0;
            for (int i = 0; ty[i] && k < 60; i++) if (!c->type[0] || strncasecmp(ty[i], c->type, strcspn(c->type, "(")) == 0 || !strchr(c->type, '(')) { snprintf(G.type_labels[k], sizeof G.type_labels[0], "%s", ty[i]); popup_add(&G.types, MI_TYPE0 + i, G.type_labels[k], false, true); k++; }
            if (!k) for (int i = 0; ty[i] && k < 60; i++) { snprintf(G.type_labels[k], sizeof G.type_labels[0], "%s", ty[i]); popup_add(&G.types, MI_TYPE0 + i, G.type_labels[k], false, true); k++; }
        }
        NEXT();
        check(a, d, row, "Not null", &c->notnull, en && !td_col_in_kind(&d->draft, c->id, TK_PK)); NEXT();
        field(a, d, F_DEFAULT, row, "Default", "SQL expression: 0, now(), 'text in quotes'", c->def, sizeof c->def, en && !c->identity && !c->gen[0], NULL); NEXT();
        c = td_col(&d->draft, d->sel_id);
        if (caps.identity != TCAP_NO) {
            ui_label(a, rect_make(row.x, row.y, px(a, 130), row.h), "Identity", ALIGN_LEFT, T_TEXT_MUTED);
            if (d->dialect == TD_PG || d->dialect == TD_ORACLE) {
                static const char *lab[] = { "None", "Always", "By default" };
                Rect sr = rect_make(row.x + px(a, 130), row.y + px(a, 3), fminf(px(a, 300), row.w - px(a, 130)), row.h - px(a, 6)); ui_record("td:identity", sr);
                int s = en ? ui_segmented(a, sr, lab, 3, c->identity) : -1;
                if (s >= 0 && s != c->identity) { push_undo(d, ""); c->identity = s; if (s) c->def[0] = 0; changed(d); d->f_loaded_rev = 0; }
            } else { bool v = c->identity != 0; if (check(a, d, row, d->dialect == TD_MSSQL ? "IDENTITY(1,1)" : d->dialect == TD_SQLITE ? "AUTOINCREMENT (INTEGER primary key)" : "AUTO_INCREMENT", &v, en)) { c = td_col(&d->draft, d->sel_id); c->identity = v ? 2 : 0; if (v) c->def[0] = 0; } }
            NEXT();
        }
        c = td_col(&d->draft, d->sel_id);
        if (caps.generated != TCAP_NO) {
            field(a, d, F_GEN, row, "Generated as", "expression (empty = a normal column)", c->gen, sizeof c->gen, en, NULL); NEXT();
            c = td_col(&d->draft, d->sel_id);
            if (c->gen[0] && caps.generated_virtual != TCAP_NO && d->dialect != TD_ORACLE) { check(a, d, row, "Stored (computed on write)", &c->gen_stored, en); NEXT(); }
        }
        c = td_col(&d->draft, d->sel_id);
        field(a, d, F_COMMENT, row, "Comment", caps.comments == TCAP_NO ? "not supported here" : "what it means", c->comment, sizeof c->comment, en && caps.comments != TCAP_NO, NULL); NEXT();
        c = td_col(&d->draft, d->sel_id);
        if (caps.collation != TCAP_NO) { field(a, d, F_COLL, row, "Collation", "default", c->collation, sizeof c->collation, en, NULL); NEXT(); }
        c = td_col(&d->draft, d->sel_id);
        if (o && d->dialect == TD_PG && strcasecmp(o->type, c->type)) {   /* USING for a conversion PostgreSQL cannot do implicitly (TBL-069 / 075) */
            int slot = -1; for (int i = 0; i < d->nusing; i++) if (d->using_col[i] == c->id) slot = i;
            if (slot < 0 && d->nusing < TD_MAXKC) { slot = d->nusing++; d->using_col[slot] = c->id; d->using_expr[slot][0] = 0; }
            char ph[300]; char qc[200]; td_quote(TD_PG, o->name, qc, sizeof qc); snprintf(ph, sizeof ph, "for example %s::%s", qc, c->type);
            if (slot >= 0 && field(a, d, F_USING, row, "USING", ph, d->using_expr[slot], 256, en, NULL)) {}
            NEXT();
        }
        if (o) {
            char risk[300]; td_type_risk(o->type, c->type, risk, sizeof risk);
            if (risk[0]) { info(a, row, T_YELLOW, "Type change: %s", risk); NEXT(); }
            if (c->notnull && !o->notnull) { info(a, row, T_YELLOW, "%s", "NOT NULL on existing rows: the step fails if a row is NULL (Review > Check existing data)"); NEXT(); }
            if (o->extra[0]) { info(a, row, T_YELLOW, "Not modelled, kept: %s", o->extra); NEXT(); }
        }
        int used = 0; for (int i = 0; i < d->draft.nkeys; i++) for (int j = 0; j < d->draft.keys[i].ncols; j++) used += d->draft.keys[i].cols[j] == c->id;
        if (used) { info(a, row, T_TEXT_DIM, "Used by %d key(s) / index(es): removing it removes them too", used); NEXT(); }
    } else if (d->sel_kind == SEL_KEY) {
        TdKey *k = td_key(&d->draft, d->sel_id); if (!k) { sel(d, SEL_TABLE, 0); return; }
        bool kenabled = en && !k->raw[0];
        char hdr[64]; snprintf(hdr, sizeof hdr, "%s", td_key_kind_name(k->kind)); for (char *p = hdr; *p; p++) *p = (char)toupper((unsigned char)*p);
        ui_label(a, row, hdr, ALIGN_LEFT, T_TEXT_MUTED); NEXT();
        Rect nr = row; nr.w -= px(a, 74);
        field(a, d, F_NAME, nr, "Name", "named by the engine", k->name, sizeof k->name, en, NULL);
        k = td_key(&d->draft, d->sel_id);
        if (ui_button(a, rect_make(row.x + row.w - px(a, 70), row.y + px(a, 3), px(a, 70), row.h - px(a, 6)), "Suggest", BTN_QUIET, en)) {   /* naming convention, only when asked (TBL-040) */
            push_undo(d, ""); char nm[128]; td_key_name(&d->draft, k, NULL, nm, sizeof nm); if (d->dialect == TD_ORACLE) for (char *p = nm; *p; p++) *p = (char)toupper((unsigned char)*p); snprintf(k->name, sizeof k->name, "%s", nm); changed(d); d->f_loaded_rev = 0;
        }
        NEXT();
        if (k->raw[0]) { info(a, row, T_YELLOW, "Not modelled by Lume (kept, can be removed, not edited): %s", k->raw); NEXT(); }
        if (k->kind != TK_CHECK) {
            Input *in = &a->in;
            float lw = px(a, 130);
            ui_label(a, rect_make(row.x, row.y, lw, row.h), k->kind == TK_FK ? "Columns \xE2\x86\x92 referenced" : "Columns", ALIGN_LEFT, T_TEXT_MUTED);
            DbTable *rt = k->kind == TK_FK ? ref_table_of(a, d, k) : NULL;
            for (int i = 0; i < k->ncols; i++) {
                const TdCol *col = td_col(&d->draft, k->cols[i]);
                float x = row.x + lw, bh = row.h - px(a, 6), by = row.y + px(a, 3);
                char lab[200]; snprintf(lab, sizeof lab, "%d  %s", i + 1, col ? col->name : "?");
                float nw = k->kind == TK_FK ? (row.w - lw) * 0.38f : (row.w - lw) - px(a, 150);
                ui_label(a, rect_make(x + px(a, 4), row.y, nw, row.h), lab, ALIGN_LEFT, col ? T_TEXT : T_RED);
                x += nw;
                char tag[200];
                if (k->kind == TK_FK) {   /* the referenced column of this one */
                    ui_label(a, rect_make(x, row.y, px(a, 20), row.h), "\xE2\x86\x92", ALIGN_LEFT, T_TEXT_DIM);
                    Rect rb = rect_make(x + px(a, 22), by, (row.w - lw) - nw - px(a, 22) - px(a, 104), bh);
                    snprintf(tag, sizeof tag, "td:refcol:%d", i + 1); ui_record(tag, rb);
                    const char *rc = i < k->nref && k->ref_cols[i][0] ? k->ref_cols[i] : NULL;
                    char rl[160]; snprintf(rl, sizeof rl, "%s  \xE2\x96\xBE", rc ? rc : (rt ? "choose" : "choose a table first"));
                    if (ui_button(a, rb, rl, rc ? BTN_NORMAL : BTN_QUIET, kenabled && rt)) {
                        popup_open(&G.menu, rb.x, rb.y + rb.h); G.menu_doc = d;
                        for (int c2 = 0; c2 < rt->ncols && c2 < 256 && G.menu.n < POPUP_MAX; c2++) {
                            static char labs[POPUP_MAX][160]; snprintf(labs[G.menu.n], 160, "%s%s", rt->cols[c2].name, rt->cols[c2].pk ? "  (PK)" : "");
                            popup_add(&G.menu, 3000 + i * 256 + c2, labs[G.menu.n], false, true);
                        }
                    }
                    x = rb.x + rb.w + px(a, 4);
                } else if (k->kind == TK_INDEX) {   /* ascending / descending */
                    Rect db = rect_make(x, by, px(a, 52), bh); snprintf(tag, sizeof tag, "td:kcol:%d:order", i + 1); ui_record(tag, db);
                    if (ui_button(a, db, k->desc[i] ? "DESC" : "ASC", BTN_QUIET, kenabled)) { push_undo(d, ""); k->desc[i] = !k->desc[i]; changed(d); d->f_loaded_rev = 0; }
                    x += px(a, 56);
                }
                /* move up / down / remove */
                Rect ub = rect_make(x, by, px(a, 30), bh), db2 = rect_make(x + px(a, 32), by, px(a, 30), bh), xb = rect_make(x + px(a, 64), by, px(a, 30), bh);
                snprintf(tag, sizeof tag, "td:kcol:%d:up", i + 1); ui_record(tag, ub);
                if (ui_button(a, ub, "\xE2\x86\x91", BTN_QUIET, kenabled && i > 0)) {
                    push_undo(d, ""); int t = k->cols[i]; k->cols[i] = k->cols[i - 1]; k->cols[i - 1] = t; bool tb = k->desc[i]; k->desc[i] = k->desc[i - 1]; k->desc[i - 1] = tb;
                    if (k->kind == TK_FK && i < k->nref) { char tr[128]; memcpy(tr, k->ref_cols[i], 128); memcpy(k->ref_cols[i], k->ref_cols[i - 1], 128); memcpy(k->ref_cols[i - 1], tr, 128); }
                    changed(d); d->f_loaded_rev = 0;
                }
                snprintf(tag, sizeof tag, "td:kcol:%d:down", i + 1); ui_record(tag, db2);
                if (ui_button(a, db2, "\xE2\x86\x93", BTN_QUIET, kenabled && i + 1 < k->ncols)) {
                    push_undo(d, ""); int t = k->cols[i]; k->cols[i] = k->cols[i + 1]; k->cols[i + 1] = t; bool tb = k->desc[i]; k->desc[i] = k->desc[i + 1]; k->desc[i + 1] = tb;
                    if (k->kind == TK_FK && i + 1 < k->nref) { char tr[128]; memcpy(tr, k->ref_cols[i], 128); memcpy(k->ref_cols[i], k->ref_cols[i + 1], 128); memcpy(k->ref_cols[i + 1], tr, 128); }
                    changed(d); d->f_loaded_rev = 0;
                }
                snprintf(tag, sizeof tag, "td:kcol:%d:remove", i + 1); ui_record(tag, xb);
                if (ui_button(a, xb, "", BTN_QUIET, kenabled)) {
                    push_undo(d, "");
                    memmove(&k->cols[i], &k->cols[i + 1], sizeof k->cols[0] * (size_t)(k->ncols - i - 1)); memmove(&k->desc[i], &k->desc[i + 1], sizeof k->desc[0] * (size_t)(k->ncols - i - 1)); k->ncols--;
                    if (k->kind == TK_FK && i < k->nref) { memmove(&k->ref_cols[i], &k->ref_cols[i + 1], 128 * (size_t)(k->nref - i - 1)); k->nref--; }
                    changed(d); d->f_loaded_rev = 0;
                    break;
                }
                draw_cross(a->renderer, xb.x + xb.w / 2, xb.y + xb.h / 2, px(a, 7), fmaxf(1, px(a, 1.3f)), color_hex(kenabled ? T_TEXT_MUTED : T_TEXT_DIM));
                NEXT();
            }
            /* add one of the table's columns not in the key yet */
            {
                Rect ab = rect_make(row.x + lw, row.y + px(a, 3), px(a, 150), row.h - px(a, 6)); ui_record("td:addcol", ab);
                int left = 0; for (int c2 = 0; c2 < d->draft.ncols; c2++) { bool in_key = false; for (int j = 0; j < k->ncols; j++) if (k->cols[j] == d->draft.cols[c2].id) in_key = true; left += !in_key; }
                if (ui_button(a, ab, k->ncols ? "Add column  \xE2\x96\xBE" : "Choose a column  \xE2\x96\xBE", k->ncols ? BTN_QUIET : BTN_NORMAL, kenabled && left > 0 && k->ncols < TD_MAXKC)) {
                    popup_open(&G.menu, ab.x, ab.y + ab.h); G.menu_doc = d;
                    for (int c2 = 0; c2 < d->draft.ncols && G.menu.n < POPUP_MAX; c2++) {
                        bool in_key = false; for (int j = 0; j < k->ncols; j++) if (k->cols[j] == d->draft.cols[c2].id) in_key = true;
                        if (in_key) continue;
                        static char labs[POPUP_MAX][300]; snprintf(labs[G.menu.n], 300, "%s  %s", d->draft.cols[c2].name, d->draft.cols[c2].type);
                        popup_add(&G.menu, 2000 + c2, labs[G.menu.n], false, true);
                    }
                }
                if (!k->ncols) ui_label(a, rect_make(ab.x + ab.w + px(a, 10), row.y, row.w - lw - ab.w - px(a, 10), row.h), "a key needs at least one column", ALIGN_LEFT, T_TEXT_DIM);
                (void)in;
            }
            NEXT();
        }
        if (k->kind == TK_CHECK) { field(a, d, F_CHECK, row, "Expression", "a predicate, e.g. total >= 0", k->check, sizeof k->check, kenabled, NULL); NEXT(); info(a, row, T_TEXT_DIM, "%s", "Every existing row is checked when the constraint is added"); NEXT(); }
        if (k->kind == TK_FK) {
            char rt[300]; snprintf(rt, sizeof rt, "%s%s%s", k->ref_schema, k->ref_schema[0] ? "." : "", k->ref_table);
            char buf[300]; snprintf(buf, sizeof buf, "%s", rt);
            if (field(a, d, F_REFTAB, row, "References", "type to find a table", buf, sizeof buf, kenabled, NULL)) {
                k = td_key(&d->draft, d->sel_id);
                char *dot = strchr(buf, '.');
                if (dot) { snprintf(k->ref_schema, sizeof k->ref_schema, "%.*s", (int)(dot - buf), buf); snprintf(k->ref_table, sizeof k->ref_table, "%s", dot + 1); }
                else { snprintf(k->ref_schema, sizeof k->ref_schema, "%s", d->draft.schema); snprintf(k->ref_table, sizeof k->ref_table, "%s", buf); }
                if (ref_table_of(a, d, k)) ref_cols_from_pk(a, d, k);
            }
            NEXT();
            k = td_key(&d->draft, d->sel_id);
            {   /* suggestions: tables of the connection containing what was typed (until one is chosen) */
                Connection *cc = conn_of(a, d->conn_id);
                const char *q = d->f[F_REFTAB].buf;
                if (kenabled && cc && q[0] && !ref_table_of(a, d, k)) {
                    int shown = 0;
                    for (int i = 0; i < cc->ntables && shown < 6; i++) {
                        if (cc->tables[i].is_view || !strcasestr(cc->tables[i].name, q)) continue;
                        Rect sr = rect_make(row.x + px(a, 130), row.y + px(a, 2), row.w - px(a, 130), row.h - px(a, 4));
                        char tag[300]; snprintf(tag, sizeof tag, "td:reftable:%s", cc->tables[i].name); ui_record(tag, sr);
                        if (ui_button(a, sr, cc->tables[i].name, BTN_QUIET, true)) {
                            push_undo(d, "");
                            const char *nm = cc->tables[i].name, *dt = strrchr(nm, '.');
                            if (dt) { snprintf(k->ref_schema, sizeof k->ref_schema, "%.*s", (int)(dt - nm), nm); snprintf(k->ref_table, sizeof k->ref_table, "%s", dt + 1); }
                            else { snprintf(k->ref_schema, sizeof k->ref_schema, "%s", d->draft.schema); snprintf(k->ref_table, sizeof k->ref_table, "%s", nm); }
                            ref_cols_from_pk(a, d, k);
                            changed(d); d->f_loaded_rev = 0;
                        }
                        NEXT(); shown++;
                    }
                    if (!shown) { info(a, row, T_YELLOW, "No table of this connection matches \"%s\"", q); NEXT(); }
                }
            }
            k = td_key(&d->draft, d->sel_id);
            k = td_key(&d->draft, d->sel_id);
            static const char *acts[] = { "No action", "Restrict", "Cascade", "Set null", "Set default" };
            static const char *codes[] = { "", "RESTRICT", "CASCADE", "SET NULL", "SET DEFAULT" };
            int nact = d->dialect == TD_ORACLE ? 4 : d->dialect == TD_MSSQL ? 5 : 5;
            for (int pass = 0; pass < 2; pass++) {
                if (pass == 1 && caps.on_update_actions == TCAP_NO) { info(a, row, T_TEXT_DIM, "%s", "ON UPDATE: not supported on Oracle"); NEXT(); break; }
                char *v = pass ? k->on_update : k->on_delete;
                int cur2 = 0; for (int i = 0; i < 5; i++) if (!strcmp(v, codes[i])) cur2 = i;
                const char *lab2[5]; int map[5], m = 0;
                for (int i = 0; i < nact; i++) { if (d->dialect == TD_ORACLE && (i == 1 || i == 4 || (pass && i))) continue; if (d->dialect == TD_MSSQL && i == 1) continue; lab2[m] = acts[i]; map[m++] = i; }
                ui_label(a, rect_make(row.x, row.y, px(a, 130), row.h), pass ? "On update" : "On delete", ALIGN_LEFT, T_TEXT_MUTED);
                int sc = 0; for (int i = 0; i < m; i++) if (map[i] == cur2) sc = i;
                Rect sr = rect_make(row.x + px(a, 130), row.y + px(a, 3), row.w - px(a, 130), row.h - px(a, 6)); ui_record(pass ? "td:on update" : "td:on delete", sr);
                int s = kenabled ? ui_segmented(a, sr, lab2, m, sc) : -1;
                if (s >= 0 && s != sc) { push_undo(d, ""); snprintf(v, 16, "%s", codes[map[s]]); changed(d); }
                NEXT();
            }
            if (!strcmp(k->on_delete, "CASCADE")) { info(a, row, T_YELLOW, "%s", "ON DELETE CASCADE: deleting a referenced row deletes these rows too"); NEXT(); }
            if (caps.deferrable == TCAP_YES) { check(a, d, row, "Deferrable (checked at COMMIT when deferred)", &k->deferrable, kenabled); NEXT(); if (k->deferrable) { check(a, d, row, "Initially deferred", &k->deferred, kenabled); NEXT(); } }
            info(a, row, T_TEXT_DIM, "%s -> %s.%s", d->draft.name, k->ref_schema, k->ref_table[0] ? k->ref_table : "?"); NEXT();
        }
        if (k->kind == TK_INDEX) {
            check(a, d, row, "Unique", &k->unique, kenabled); NEXT();
            if (caps.index_method == TCAP_YES) { field(a, d, F_METHOD, row, "Method", "btree (default), gin, brin, hash...", k->method, sizeof k->method, kenabled, NULL); NEXT(); }
            if (caps.partial_index == TCAP_YES) { field(a, d, F_WHERE, row, d->dialect == TD_MSSQL ? "Filter" : "Where", "partial index condition (optional)", k->where, sizeof k->where, kenabled, NULL); NEXT(); }
            if (caps.include_index == TCAP_YES) {
                char cur[1000]; bool f0[TD_MAXKC] = { false }; cols_text(d, k->include, f0, k->ninclude, cur, sizeof cur);
                char b2[1000]; snprintf(b2, sizeof b2, "%s", cur);
                if (field(a, d, F_INCLUDE, row, "Include", "non-key columns (optional)", b2, sizeof b2, kenabled, NULL)) { char bad[128]; k = td_key(&d->draft, d->sel_id); k->ninclude = parse_cols(d, b2, k->include, NULL, TD_MAXKC, bad, sizeof bad); }
                NEXT();
            }
            info(a, row, T_TEXT_DIM, "%s", "Indexes are never created by themselves: only those in the draft"); NEXT();
        }
    } else if (d->sel_kind == SEL_GONE_COL || d->sel_kind == SEL_GONE_KEY) {
        ui_label(a, row, "REMOVED IN THE DRAFT", ALIGN_LEFT, T_RED); NEXT();
        info(a, row, T_TEXT, "%s", d->sel_kind == SEL_GONE_COL ? "The column and its data are dropped when the change is applied." : "The constraint / index is dropped when the change is applied."); NEXT();
        if (ui_button(a, rect_make(row.x + px(a, 130), row.y + px(a, 2), px(a, 120), row.h - px(a, 4)), "Restore", BTN_NORMAL, en)) restore_removed(d);
        NEXT();
    }
#undef NEXT
}

static void draw_tree(App *a, TdDoc *d, Rect r) {
    SDL_Renderer *R = a->renderer; Input *in = &a->in;
    draw_rect(R, r, color_hex(T_PANEL_ALT));
    bool en = !read_only(d);
    /* toolbar: + - up down, filter */
    float bh = px(a, 26), bw = px(a, 28);
    Rect tb = rect_make(r.x + px(a, 6), r.y + px(a, 6), r.w - px(a, 12), bh);
    if (ui_button(a, rect_make(tb.x, tb.y, bw, bh), "+", BTN_QUIET, en)) add_column(d, d->sel_kind == SEL_COL ? d->sel_id : 0);
    ui_record("td:add column", rect_make(tb.x, tb.y, bw, bh));
    if (ui_button(a, rect_make(tb.x + bw, tb.y, bw, bh), "-", BTN_QUIET, en && (d->sel_kind == SEL_COL || d->sel_kind == SEL_KEY))) remove_selected(d);
    ui_record("td:remove", rect_make(tb.x + bw, tb.y, bw, bh));
    bool can_move = en && d->sel_kind == SEL_COL && (d->is_new || !td_col_c(&d->orig, d->sel_id) || td_caps(d->dialect, d->version).column_order == TCAP_YES);
    if (ui_button(a, rect_make(tb.x + 2 * bw, tb.y, bw, bh), "^", BTN_QUIET, can_move)) { push_undo(d, ""); if (td_move_col(&d->draft, d->sel_id, -1)) changed(d); }
    if (ui_button(a, rect_make(tb.x + 3 * bw, tb.y, bw, bh), "v", BTN_QUIET, can_move)) { push_undo(d, ""); if (td_move_col(&d->draft, d->sel_id, 1)) changed(d); }
    if (ui_textfield(a, &d->filter, rect_make(tb.x + 4 * bw + px(a, 6), tb.y, tb.w - 4 * bw - px(a, 6), bh), "Filter columns", tf_focused(a, d, TF_FILTER))) tf_focus(a, d, TF_FILTER);
    static Row rows[2048];
    int n = build_rows(d, rows, 2048);
    float rh = px(a, M_ROW);
    Rect list = rect_make(r.x, tb.y + bh + px(a, 6), r.w, r.y + r.h - (tb.y + bh + px(a, 6)));
    bool hov = rect_contains(list, in->mouse_x, in->mouse_y);
    if (hov && in->wheel_y != 0) d->tree_scroll -= in->wheel_y;
    d->tree_scroll = clampf(d->tree_scroll, 0, fmaxf(0, n * rh - list.h));
    if (ui_scrollbar_input(a, &d->tree_sb, list, n * rh, &d->tree_scroll)) hov = false;   /* the bar, not a row */
    int hover = hov ? (int)floorf((in->mouse_y - list.y + d->tree_scroll) / rh) : -1;
    if (hover >= n) hover = -1;
    if (hover >= 0 && in->mouse_pressed && !in->mouse_consumed) {
        in->mouse_consumed = true;
        Row *w = &rows[hover];
        if (w->kind == 6) add_column(d, 0);
        else if (w->kind == 1) { d->closed_grp[w->grp] = !d->closed_grp[w->grp]; if (in->clicks >= 2 && en) { if (w->grp == TK_NKINDS) add_column(d, 0); else add_key(d, w->grp); } }
        else sel(d, w->kind == 0 ? SEL_TABLE : w->kind == 2 ? SEL_COL : w->kind == 3 ? SEL_KEY : w->kind == 4 ? SEL_GONE_COL : SEL_GONE_KEY, w->id);
    }
    if (hover >= 0 && in->mouse_rpressed && !in->mouse_consumed) {   /* the component's menu (spec 4.3) */
        in->mouse_consumed = true;
        Row *w = &rows[hover];
        if (w->kind == 2 || w->kind == 3) sel(d, w->kind == 2 ? SEL_COL : SEL_KEY, w->id);
        popup_open(&G.menu, in->mouse_x, in->mouse_y); G.menu_doc = d;
        if (w->kind == 1 && w->grp == TK_NKINDS) popup_add(&G.menu, 1001, "Add Column", false, en);
        else if (w->kind == 1) { static char l[64]; snprintf(l, sizeof l, "Add %s", td_key_kind_name(w->grp)); popup_add(&G.menu, 1100 + w->grp, l, false, en && !(w->grp == TK_PK && td_pk(&d->draft))); }
        else if (w->kind == 2) {
            popup_add(&G.menu, 1001, "Add Column After", false, en);
            popup_add(&G.menu, 1002, "Duplicate", false, en);
            popup_add(&G.menu, 1003, "Move Up", false, en); popup_add(&G.menu, 1004, "Move Down", false, en);
            popup_add(&G.menu, 1110, "Add to Primary Key", true, en); popup_add(&G.menu, 1111, "Add Unique", false, en); popup_add(&G.menu, 1112, "Add Foreign Key", false, en); popup_add(&G.menu, 1114, "Add Index", false, en);
            popup_add(&G.menu, 1005, "Remove", true, en);
            popup_add(&G.menu, 1006, "Copy Definition", false, true);
        } else if (w->kind == 3) {
            const TdKey *k = td_key(&d->draft, w->id);
            if (k && k->kind == TK_FK) popup_add(&G.menu, 1007, "Go to Referenced Table", false, true);
            popup_add(&G.menu, 1005, "Remove", false, en);
            popup_add(&G.menu, 1006, k && k->kind == TK_CHECK ? "Copy Predicate" : "Copy DDL", false, true);
        } else if (w->kind == 0) { popup_add(&G.menu, MI_CREATE_DDL, "Copy CREATE DDL", false, true); popup_add(&G.menu, 1008, "Revert All Changes", false, en && tdes_doc_dirty(d)); }
        else if (w->kind >= 4) popup_add(&G.menu, 1009, "Restore", false, en);
    }
    clip_push(R, list);
    int first = (int)(d->tree_scroll / rh), last = (int)ceilf((d->tree_scroll + list.h) / rh);
    for (int i = first < 0 ? 0 : first; i < n && i <= last; i++) {   /* visible rows only (TBL-094) */
        Row *w = &rows[i];
        Rect rr = rect_make(list.x, list.y + i * rh - d->tree_scroll, list.w, rh);
        bool selected = (w->kind == 0 && d->sel_kind == SEL_TABLE) || (w->kind == 2 && d->sel_kind == SEL_COL && d->sel_id == w->id) || (w->kind == 3 && d->sel_kind == SEL_KEY && d->sel_id == w->id) ||
                        (w->kind == 4 && d->sel_kind == SEL_GONE_COL && d->sel_id == w->id) || (w->kind == 5 && d->sel_kind == SEL_GONE_KEY && d->sel_id == w->id);
        if (selected) draw_rect(R, rr, color_hex(T_SELECTED)); else if (i == hover) draw_rect(R, rr, color_hex(T_HOVER));
        float x = rr.x + px(a, w->kind == 0 ? 10 : w->kind == 1 ? 22 : 40);
        if (w->kind == 1) draw_chevron(R, x - px(a, 8), rr.y + rr.h / 2, px(a, 7), px(a, 1.5f), !d->closed_grp[w->grp], color_hex(T_TEXT_MUTED));
        /* state in words and colour, not colour only (TBL-097.2) */
        uint32_t mc = w->mark == '+' ? T_GREEN : w->mark == '~' ? T_YELLOW : w->mark == '-' ? T_RED : 0;
        if (mc) { char m[2] = { w->mark, 0 }; draw_text_fit(a->font_ui, R, m, -1, rect_make(x - px(a, 16), rr.y, px(a, 14), rr.h), color_hex(mc), ALIGN_LEFT); }
        char tag[340]; snprintf(tag, sizeof tag, "tdrow:%s", w->label); ui_record(tag, rr);
        if (w->kind == 6) {   /* a button inside the tree's flow: the groups below move down for it */
            Rect b = rect_make(rr.x + px(a, 30), rr.y + px(a, 2), fminf(px(a, 190), rr.w - px(a, 40)), rr.h - px(a, 4));
            draw_rrect(R, b, px(a, M_RADIUS), color_hex(i == hover ? T_ACCENT_HOVER : T_ACCENT));
            draw_text_fit(a->font_ui, R, "+  Add the first column", -1, b, color_hex(T_ON_ACCENT), ALIGN_CENTER);
            if (i == hover) a->want_cursor = CURSOR_HAND;
            continue;
        }
        draw_text_fit(w->kind == 1 ? a->font_small : a->font_ui, R, w->label, -1, rect_make(x + px(a, 2), rr.y, rr.x + rr.w - x - px(a, 8), rr.h), color_hex(w->kind == 1 ? T_TEXT_MUTED : w->kind >= 4 ? T_TEXT_DIM : T_TEXT), ALIGN_LEFT);
    }
    clip_pop(R);
    ui_scrollbar_draw(a, &d->tree_sb, list, n * rh, d->tree_scroll);
}

void tdes_tab_body(App *a, TdDoc *d, struct Editor *ed, Rect body) {
    SDL_Renderer *R = a->renderer; Input *in = &a->in;
    G.a = a;
    if (in->mouse_pressed && !in->mouse_consumed && TF.doc == d) TF.f = -1;   /* a click anywhere drops the field focus; a field clicked below takes it again */
    replan(d);
    { char ttl[160]; title_of(d, ttl, sizeof ttl); panels_set_tab_title(a->panels, d->tab_id, ttl); }
    draw_rect(R, body, color_hex(T_EDITOR_BG));
    Connection *c = conn_of(a, d->conn_id);
    /* header: target, state, actions */
    float hh = px(a, 36);
    Rect hdr = rect_make(body.x, body.y, body.w, hh); hdr_rect = hdr;
    draw_rect(R, hdr, color_hex(T_PANEL));
    char q[400]; qualified(d, &d->draft, q, sizeof q);
    bool dirty = tdes_doc_dirty(d);
    char t[700]; snprintf(t, sizeof t, "%s %s%s", d->is_new ? "New Table:" : "Edit Table:", q, d->state == DS_APPLYING ? "   (applying)" : dirty ? "   * Draft" : d->apply_ok ? "   Applied" : "");
    draw_text_fit(a->font_ui, R, t, -1, rect_make(hdr.x + px(a, 14), hdr.y, hdr.w * 0.5f, hdr.h), color_hex(T_TEXT), ALIGN_LEFT);
    char tgt[200]; snprintf(tgt, sizeof tgt, "%s%s%s \xC2\xB7 %s", c && c->env ? conn_env_name(c->env) : "", c && c->env ? " \xC2\xB7 " : "", td_dialect_name(d->dialect), d->conn_name);
    Rect more = rect_make(hdr.x + hdr.w - px(a, 40), hdr.y + px(a, 5), px(a, 32), hh - px(a, 10));
    Rect revb = rect_make(more.x - px(a, 140), hdr.y + px(a, 5), px(a, 134), hh - px(a, 10));
    draw_text_fit(a->font_small, R, tgt, -1, rect_make(hdr.x + hdr.w * 0.5f, hdr.y, revb.x - hdr.x - hdr.w * 0.5f - px(a, 8), hh), color_hex(c && c->env == CONN_ENV_PROD ? T_RED : T_TEXT_MUTED), ALIGN_RIGHT);
    bool can_review = d->state == DS_READY && d->plan.n > 0 && !d->plan.blocked && !read_only(d);
    bool fresh = d->is_new && d->draft.ncols == 0;   /* nothing done yet: guidance, not errors */
    if (ui_button(a, more, "...", BTN_QUIET, true)) {
        popup_open(&G.menu, more.x - px(a, 160), more.y + more.h); G.menu_doc = d;
        popup_add(&G.menu, MI_COPY, "Copy SQL", false, d->plan.n > 0 && !d->plan.blocked);
        popup_add(&G.menu, MI_CONSOLE, "Open SQL in Editor", false, d->plan.n > 0 && !d->plan.blocked);
        popup_add(&G.menu, MI_MIGRATION, "Save as Migration (.sql)", false, d->plan.n > 0 && !d->plan.blocked);
        popup_add(&G.menu, MI_CREATE_DDL, "Copy CREATE DDL", false, d->draft.ncols > 0);
        { const Command *rc = commands_find("intelligence.review_table"); char why[80]; popup_add(&G.menu, MI_AI_REVIEW, "Review with Lume Intelligence", false, d->draft.ncols > 0 && rc && (!rc->enabled || rc->enabled(commands_ctx(), why, sizeof why))); }
        popup_add(&G.menu, MI_UNDO, "Undo", true, d->nundo > 0 && !read_only(d));
        popup_add(&G.menu, MI_REDO, "Redo", false, d->nredo > 0 && !read_only(d));
        popup_add(&G.menu, MI_SAVE_DRAFT, "Save Draft", true, d->state == DS_READY);
        popup_add(&G.menu, MI_DISCARD_DRAFT, "Discard Draft", false, d->state == DS_READY && (dirty || d->draft_on_disk));
        popup_add(&G.menu, MI_RELOAD, "Reload from Server", false, !d->is_new && d->state != DS_APPLYING);
    }
    ui_record("td:more", more);
    float y = body.y + hh;
    /* banners: drift, restored draft, read-only, messages (TBL-010 / 057 / 096) */
    char ban[700] = ""; uint32_t bc = T_YELLOW;
    if (d->state == DS_LOADING) { snprintf(ban, sizeof ban, "%s", d->msg); bc = T_TEXT_MUTED; }
    else if (d->state == DS_LOAD_FAILED) { snprintf(ban, sizeof ban, "Could not read the table: %s", d->msg); bc = T_RED; }
    else if (d->drift) snprintf(ban, sizeof ban, "%s", d->banner);
    else if (!d->is_new && d->orig.special[0]) snprintf(ban, sizeof ban, "Read-only: %s. Its definition can still be copied.", d->orig.special);
    else if (!d->is_new && !d->orig.can_alter) snprintf(ban, sizeof ban, "Read-only: %s. Its definition can still be copied.", d->orig.alter_why);
    else if (d->banner[0]) snprintf(ban, sizeof ban, "%s", d->banner);
    else if (d->msg[0]) { snprintf(ban, sizeof ban, "%s", d->msg); bc = d->apply_ok ? T_GREEN : d->state == DS_APPLYING ? T_TEXT_MUTED : T_RED; }
    if (ban[0]) {
        Rect br = rect_make(body.x, y, body.w, px(a, 28));
        draw_rect(R, br, color_alpha(color_hex(bc), 40));
        draw_text_fit(a->font_small, R, ban, -1, rect_make(br.x + px(a, 14), br.y, br.w - px(a, 200), br.h), color_hex(T_TEXT), ALIGN_LEFT);
        ui_record("td:banner", br);
        if (d->drift && ui_button(a, rect_make(br.x + br.w - px(a, 96), br.y + px(a, 3), px(a, 88), br.h - px(a, 6)), "Reload", BTN_NORMAL, true)) { d->drift = false; d->banner[0] = 0; d->state = DS_LOADING; start_snapshot(a, d, PH_LOAD, d->schema, d->name); }
        else if (!d->drift && d->state == DS_LOAD_FAILED && ui_button(a, rect_make(br.x + br.w - px(a, 96), br.y + px(a, 3), px(a, 88), br.h - px(a, 6)), "Retry", BTN_NORMAL, true)) { d->state = DS_LOADING; start_snapshot(a, d, PH_LOAD, d->schema, d->name); }
        else if (!d->drift && d->banner[0] && d->state == DS_READY && ui_button(a, rect_make(br.x + br.w - px(a, 96), br.y + px(a, 3), px(a, 88), br.h - px(a, 6)), "Dismiss", BTN_QUIET, true)) d->banner[0] = 0;
        y += br.h;
    }
    if (d->state == DS_LOADING || d->state == DS_LOAD_FAILED) return;   /* never a falsely empty editor (TBL-002.3) */
    /* footer */
    float fh = px(a, 36);
    Rect foot = rect_make(body.x, body.y + body.h - fh, body.w, fh); footer_rect = foot;
    draw_rect(R, foot, color_hex(T_PANEL));
    int ne = count_level(d, TV_ERROR), nw = count_level(d, TV_WARNING);
    char ft[400]; snprintf(ft, sizeof ft, "%d change%s \xC2\xB7 %d error%s \xC2\xB7 %d warning%s \xC2\xB7 risk %s \xC2\xB7 %s", d->nchanges, d->nchanges == 1 ? "" : "s", ne, ne == 1 ? "" : "s", nw, nw == 1 ? "" : "s",
                       td_risk_name(d->plan.risk), d->state == DS_APPLYING ? "applying" : "not applied");
    if (fresh) snprintf(ft, sizeof ft, "New table: add its first column to start");
    draw_text_fit(a->font_small, R, ft, -1, rect_make(foot.x + px(a, 14), foot.y, foot.w - px(a, 330), foot.h), color_hex(ne && !fresh ? T_RED : T_TEXT_MUTED), ALIGN_LEFT);
    Rect apb = rect_make(foot.x + foot.w - px(a, 150), foot.y + px(a, 5), px(a, 142), fh - px(a, 10));
    Rect dib = rect_make(apb.x - px(a, 100), apb.y, px(a, 94), apb.h);
    if (ui_button(a, dib, "Discard", BTN_QUIET, dirty && !read_only(d))) { push_undo(d, "discard"); td_free(&d->draft); if (d->is_new) { td_init(&d->draft); snprintf(d->draft.schema, sizeof d->draft.schema, "%s", d->schema); snprintf(d->draft.name, sizeof d->draft.name, "new_table"); d->draft.can_alter = true; } else td_copy(&d->draft, &d->orig); changed(d); sel(d, SEL_TABLE, 0); }
    ui_record("td:discard", dib);
    if (ui_button(a, apb, "Review & Apply", BTN_PRIMARY, can_review)) { G.modal = M_REVIEW; G.mdoc = d; d->consent = false; textfield_set(&d->prod_name, ""); d->datacheck[0] = 0; d->approved = td_plan_hash(&d->plan, d->rev); }
    ui_record("td:review & apply", apb);
    /* issues line: the first error / warning, pointing at its field (TBL-066.2) */
    float iy = foot.y;
    for (int i = 0, shown = 0; !fresh && i < d->nissues && shown < 2; i++) {
        if (d->issues[i].level == TV_INFO) continue;
        iy -= px(a, 22);
        Rect ir = rect_make(body.x, iy, body.w, px(a, 22));
        draw_rect(R, ir, color_alpha(color_hex(d->issues[i].level == TV_ERROR ? T_RED : T_YELLOW), 30));
        char it[400]; snprintf(it, sizeof it, "%s: %s", d->issues[i].level == TV_ERROR ? "Error" : "Warning", d->issues[i].text);
        draw_text_fit(a->font_small, R, it, -1, rect_inset(ir, px(a, 14), 0), color_hex(T_TEXT), ALIGN_LEFT);
        if (rect_contains(ir, in->mouse_x, in->mouse_y) && in->mouse_pressed && !in->mouse_consumed) {   /* Go to issue */
            in->mouse_consumed = true;
            if (d->issues[i].col_id) sel(d, SEL_COL, d->issues[i].col_id); else if (d->issues[i].key_id) sel(d, SEL_KEY, d->issues[i].key_id); else sel(d, SEL_TABLE, 0);
        }
        shown++;
    }
    /* preview (TBL-052): the tab's editor, read-only, always the current draft */
    float ph = d->preview_open ? fmaxf(px(a, 110), (iy - y) * 0.32f) : px(a, 26);
    Rect pv = rect_make(body.x, iy - ph, body.w, ph);
    Rect pvh = rect_make(pv.x, pv.y, pv.w, px(a, 26));
    draw_rect(R, pvh, color_hex(T_PANEL));
    draw_text_fit(a->font_small, R, d->plan.blocked && !fresh ? "SQL PREVIEW  (no SQL: fix the draft)" : d->plan.transactional ? "SQL PREVIEW  (one transaction)" : "SQL PREVIEW", -1, rect_make(pvh.x + px(a, 14), pvh.y, pvh.w * 0.5f, pvh.h), color_hex(d->plan.blocked && !fresh ? T_RED : T_TEXT_MUTED), ALIGN_LEFT);
    Rect tg = rect_make(pvh.x + pvh.w - px(a, 90), pvh.y + px(a, 2), px(a, 84), pvh.h - px(a, 4));
    if (ui_button(a, tg, d->preview_open ? "Collapse" : "Expand", BTN_QUIET, true)) d->preview_open = !d->preview_open;
    if (ed && d->preview_rev != d->plan_rev + 1) { editor_set_read_only(ed, false); editor_set_text(ed, d->script ? d->script : ""); editor_set_read_only(ed, true); d->preview_rev = d->plan_rev + 1; }
    if (d->preview_open && ed) {
        Rect pe = rect_make(pv.x, pv.y + pvh.h, pv.w, pv.h - pvh.h);
        if (a->in.mouse_pressed && !a->in.mouse_consumed && rect_contains(pe, a->in.mouse_x, a->in.mouse_y) && TF.doc == d) TF.f = -1;   /* a click in the preview takes the keyboard back */
        if (tf_any(a, d)) { int sf = a->focus; a->focus = FOCUS_NONE; editor_update(ed, a, pe); a->focus = sf; }   /* a field is typing: the preview only draws */
        else editor_update(ed, a, pe);
    }
    /* structure | properties, resizable by window width (TBL-004) */
    Rect mid = rect_make(body.x, y, body.w, pv.y - y);
    float lw = body.w < px(a, 700) ? body.w * 0.45f : fmaxf(px(a, 260), body.w * 0.34f);
    draw_tree(a, d, rect_make(mid.x, mid.y, lw, mid.h));

    draw_rect(R, rect_make(mid.x + lw, mid.y, px(a, 1), mid.h), color_hex(T_BORDER));
    draw_props(a, d, rect_make(mid.x + lw + px(a, 1), mid.y, mid.w - lw - px(a, 1), mid.h));
    /* keys: Undo / Redo when no text field is focused (TBL-007 / 086) */
    unsigned mods;
    if (!ui_text_editing() && a->focus == FOCUS_EDITOR) {
        if (input_key_any(in, SDLK_Z, &mods) && (mods & (SDL_KMOD_GUI | SDL_KMOD_CTRL))) { if (mods & SDL_KMOD_SHIFT) do_redo(d); else do_undo(d); }
        if (input_key_any(in, SDLK_DELETE, &mods)) remove_selected(d);
    }
    /* drafts are kept on disk while editing (TBL-009), at most every 2 s */
    if (dirty && d->state == DS_READY && d->draft_rev_saved != d->rev && now_s() - d->draft_saved_at > 2.0) save_draft(a, d);
    else if (dirty && d->draft_rev_saved != d->rev) app_wake_in(a, 2100);
}

/* ---- dialogs ---- */

static Rect modal_rect(App *a, float wf, float hf) {
    float w = fminf(a->width * wf, px(a, 1100)), h = fminf(a->height * hf, px(a, 780));
    return rect_make((a->width - w) / 2, (a->height - h) / 2, w, h);
}
static void modal_frame(App *a, Rect rc, const char *title) {
    SDL_Renderer *r = a->renderer;
    draw_rect(r, rect_make(0, 0, (float)a->width, (float)a->height), color_alpha(color_hex(0x000000), 110));
    draw_rrect(r, rc, px(a, 10), color_hex(T_BORDER));
    draw_rrect(r, rect_inset(rc, px(a, 1), px(a, 1)), px(a, 9), color_hex(T_PANEL_ALT));
    draw_text_fit(a->font_ui, r, title, -1, rect_make(rc.x + px(a, 18), rc.y + px(a, 8), rc.w - px(a, 36), px(a, 28)), color_hex(T_TEXT), ALIGN_LEFT);
}
static float mline(App *a, Rect area, float y, uint32_t color, bool mono, const char *fmt, ...) {
    char b[1200]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    float h = px(a, mono ? 20 : 22);
    float yy = y - G.mscroll;
    if (yy >= area.y - h && yy < area.y + area.h) draw_text_fit(mono ? a->font_mono : a->font_small, a->renderer, b, -1, rect_make(area.x, yy, area.w, h), color_hex(color), ALIGN_LEFT);
    return y + h;
}

/* the review (spec 2.3, TBL-053 / 054 / 059 / 060 / 068 / 070 / 071) */
static void draw_review(App *a, TdDoc *d) {
    Rect rc = modal_rect(a, 0.88f, 0.9f);
    Connection *c = conn_of(a, d->conn_id);
    char q[400]; qualified(d, d->drop_mode ? &d->orig : &d->draft, q, sizeof q);
    char title[500]; snprintf(title, sizeof title, d->drop_mode ? "Drop Table: %s" : "Review Table Changes: %s", q);
    modal_frame(a, rc, title);
    SDL_Renderer *R = a->renderer; Input *in = &a->in;
    bool prod = c && c->env == CONN_ENV_PROD;
    char tgt[300]; snprintf(tgt, sizeof tgt, "%s%s%s / %s / %s", prod ? "[PROD] " : c && c->env ? conn_env_name(c->env) : "", c && c->env && !prod ? " " : "", d->conn_name, td_dialect_name(d->dialect), q);
    draw_text_fit(a->font_small, R, tgt, -1, rect_make(rc.x + px(a, 18), rc.y + px(a, 36), rc.w - px(a, 36), px(a, 20)), color_hex(prod ? T_RED : T_TEXT_MUTED), ALIGN_LEFT);
    if (!d->drop_mode) replan(d);
    TdPlan *p = &d->plan;
    bool destructive = d->drop_mode || (p->flags & TS_DESTRUCTIVE) || p->risk == RISK_HIGH;
    float bottom_h = px(a, 50) + (destructive ? px(a, 30) : 0) + (prod ? px(a, 34) : 0) + (d->dialect == TD_PG && !d->drop_mode ? px(a, 32) : 0);
    Rect area = rect_make(rc.x + px(a, 18), rc.y + px(a, 62), rc.w - px(a, 36), rc.h - px(a, 62) - bottom_h);
    if (rect_contains(area, in->mouse_x, in->mouse_y) && in->wheel_y != 0) G.mscroll -= in->wheel_y * px(a, 20);
    if (G.mscroll < 0) G.mscroll = 0;
    if (G.mtotal > 0) ui_scrollbar_input(a, &G.msb, area, G.mtotal, &G.mscroll);
    draw_rect(R, area, color_hex(T_EDITOR_BG));
    clip_push(R, area);
    float y = area.y + px(a, 6);
    Rect ar = rect_inset(area, px(a, 10), 0);
    if (d->drop_mode) {
        y = mline(a, ar, y, T_RED, false, "Risk: HIGH \xC2\xB7 destructive \xC2\xB7 every row of %s is lost \xC2\xB7 rows (estimate): %lld", q, d->orig.approx_rows);
        y = mline(a, ar, y, T_TEXT, false, "Dependents that break or go with it:");
        const char *s = d->orig.deps; int k = 0;
        while (*s) { const char *e = strchr(s, '\n'); int l = e ? (int)(e - s) : (int)strlen(s); y = mline(a, ar, y, T_TEXT, false, "    %.*s", l, s); s += l + (e ? 1 : 0); k++; }
        if (!k) y = mline(a, ar, y, T_TEXT_DIM, false, "    none known%s", d->orig.deps_partial ? " (this catalog does not list every dependent)" : "");
        char qt[600]; td_qname(d->dialect, d->orig.schema, d->orig.name, qt, sizeof qt);
        y = mline(a, ar, y, T_TEXT_MUTED, false, "Statement (never CASCADE):");
        y = mline(a, ar, y, T_TEXT, true, "  1. DROP TABLE %s;", qt);
    } else {
        y = mline(a, ar, y, p->risk == RISK_HIGH ? T_RED : p->risk == RISK_MEDIUM ? T_YELLOW : T_GREEN, false, "Summary: %d change(s) \xC2\xB7 Risk: %s \xC2\xB7 Existing data: %s \xC2\xB7 Dependents known: %d%s \xC2\xB7 Rollback: %s",
                  d->nchanges, td_risk_name(p->risk), d->is_new ? "none (new table)" : d->orig.approx_rows > 0 ? "YES" : d->orig.approx_rows == 0 ? "no rows (estimate)" : "unknown",
                  count_lines(d->orig.deps), d->orig.deps_partial ? "+" : "",
                  p->rollback == TR_TRANSACTIONAL ? "transactional (all or nothing)" : p->rollback == TR_COMPENSATE ? "not transactional; a compensating script is proposed" : "IRREVERSIBLE (data is lost)");
        for (int i = 0; i < d->nchanges; i++) y = mline(a, ar, y, d->changes[i].text[1] == '-' ? T_RED : d->changes[i].text[1] == '+' ? T_GREEN : T_TEXT, false, "%s", d->changes[i].text);
        for (int i = 0; i < p->nwarn; i++) y = mline(a, ar, y, T_YELLOW, false, "[!] %s", p->warnings[i]);
        y += px(a, 6);
        y = mline(a, ar, y, T_TEXT_MUTED, false, "DDL (exact statements, in this order%s):", p->transactional ? ", inside BEGIN ... COMMIT" : ", each one committed by the server");
        for (int i = 0; i < p->n; i++) {
            const TdStep *s = &p->steps[i];
            char badges[200] = ""; int w = 0;
            if (s->flags & TS_DESTRUCTIVE) w += snprintf(badges + w, sizeof badges - (size_t)w, " [data loss]");
            if (s->flags & TS_REBUILD) w += snprintf(badges + w, sizeof badges - (size_t)w, " [rebuild]");
            else if (s->flags & TS_REWRITE) w += snprintf(badges + w, sizeof badges - (size_t)w, " [rewrite]");
            if (s->flags & TS_BLOCKING) w += snprintf(badges + w, sizeof badges - (size_t)w, " [lock]");
            if (s->flags & TS_IMPLICIT_COMMIT) w += snprintf(badges + w, sizeof badges - (size_t)w, " [commits]");
            if (s->flags & TS_DATA_CHECK) w += snprintf(badges + w, sizeof badges - (size_t)w, " [checks rows]");
            if (s->flags & TS_UNCERTAIN) w += snprintf(badges + w, sizeof badges - (size_t)w, " [uncertain]");
            y = mline(a, ar, y, (s->flags & TS_DESTRUCTIVE) ? T_RED : T_TEXT_MUTED, false, "%2d. %s%s", i + 1, s->why, badges);
            const char *t = s->sql;
            while (*t) { const char *e = strchr(t, '\n'); int l = e ? (int)(e - t) : (int)strlen(t); y = mline(a, ar, y, T_TEXT, true, "    %.*s", l, t); t += l + (e ? 1 : 0); }
        }
        if (p->compensate[0]) {
            y += px(a, 6);
            y = mline(a, ar, y, T_TEXT_MUTED, false, "Compensating script (a proposal: never run by Lume; may need a backup of the data):");
            const char *t = p->compensate;
            while (*t) { const char *e = strchr(t, '\n'); int l = e ? (int)(e - t) : (int)strlen(t); y = mline(a, ar, y, T_TEXT_DIM, true, "    %.*s", l, t); t += l + (e ? 1 : 0); }
        }
        if (d->datacheck[0]) {
            y += px(a, 6);
            const char *t = d->datacheck;
            while (*t) { const char *e = strchr(t, '\n'); int l = e ? (int)(e - t) : (int)strlen(t); y = mline(a, ar, y, strstr(t, "will fail") && strstr(t, "will fail") < t + l ? T_RED : T_TEXT, false, "%.*s", l, t); t += l + (e ? 1 : 0); }
        }
    }
    clip_pop(R);
    float maxs = fmaxf(0, y - area.y - area.h + px(a, 10) + G.mscroll);
    if (G.mscroll > maxs) G.mscroll = maxs;
    G.mtotal = maxs + area.h;
    ui_scrollbar_draw(a, &G.msb, area, G.mtotal, G.mscroll);
    /* options, consent and buttons */
    float by = area.y + area.h + px(a, 8);
    if (d->dialect == TD_PG && !d->drop_mode) {
        ui_label(a, rect_make(rc.x + px(a, 18), by, px(a, 120), px(a, 26)), "Lock timeout", ALIGN_LEFT, T_TEXT_MUTED);
        static bool lf;
        Rect lr = rect_make(rc.x + px(a, 140), by, px(a, 120), px(a, 26)); ui_record("td:lock timeout", lr);
        char before[16]; snprintf(before, sizeof before, "%s", d->lock_timeout.buf);
        if (ui_textfield(a, &d->lock_timeout, lr, "e.g. 5s (none)", lf)) lf = true;
        if (strcmp(before, d->lock_timeout.buf)) { changed(d); d->approved = 0; replan(d); d->approved = td_plan_hash(&d->plan, d->rev); }
        Rect cb = rect_make(rc.x + px(a, 280), by, px(a, 300), px(a, 26)); ui_record("td:check existing data", cb);
        if (!d->is_new && ui_button(a, cb, "Check existing data (reads the table)", BTN_QUIET, d->state == DS_READY && !d->job)) start_datacheck(a, d);
        by += px(a, 32);
    } else if (!d->is_new && !d->drop_mode) {
        Rect cb = rect_make(rc.x + px(a, 18), by - px(a, 2), px(a, 300), px(a, 26)); ui_record("td:check existing data", cb);
        if (ui_button(a, cb, "Check existing data (reads the table)", BTN_QUIET, d->state == DS_READY && !d->job)) start_datacheck(a, d);
    }
    if (destructive) {
        char ct[400];
        if (d->drop_mode) snprintf(ct, sizeof ct, "I understand that %s and all its rows are deleted", q);
        else { int k = 0; for (int i = 0; i < p->n; i++) if (p->steps[i].flags & (TS_DESTRUCTIVE | TS_REBUILD)) { k = i; break; } snprintf(ct, sizeof ct, "I understand: %s", p->n ? p->steps[k].why : "high risk"); }
        Rect cr = rect_make(rc.x + px(a, 18), by, rc.w - px(a, 36), px(a, 26)); ui_record("td:consent", cr);
        if (ui_checkbox(a, cr, ct, d->consent, true)) d->consent = !d->consent;
        by += px(a, 30);
    }
    if (prod) {   /* PROD policy: the table's name typed (TBL-071) */
        ui_label(a, rect_make(rc.x + px(a, 18), by, px(a, 260), px(a, 26)), "PROD: type the table name to confirm", ALIGN_LEFT, T_RED);
        Rect pr = rect_make(rc.x + px(a, 280), by, px(a, 240), px(a, 26)); ui_record("td:prod name", pr);
        static bool pf; if (ui_textfield(a, &d->prod_name, pr, d->drop_mode ? d->orig.name : d->draft.name, pf)) pf = true;
        by += px(a, 34);
    }
    Rect bc = rect_make(rc.x + rc.w - px(a, 18) - px(a, 100), rc.y + rc.h - px(a, 44), px(a, 100), px(a, 32));
    Rect bk = rect_make(bc.x - px(a, 110), bc.y, px(a, 100), bc.h);
    Rect bp = rect_make(bk.x - px(a, 160), bc.y, px(a, 150), bc.h);
    bool approval_ok = d->drop_mode || td_plan_hash(&d->plan, d->rev) == d->approved;
    bool ok = (!destructive || d->consent) && (!prod || !strcmp(d->prod_name.buf, d->drop_mode ? d->orig.name : d->draft.name)) && approval_ok && !d->job && (d->drop_mode || (!p->blocked && p->n));
    if (!approval_ok) draw_text_fit(a->font_small, R, "The draft changed: this review is stale", -1, rect_make(rc.x + px(a, 18), bc.y, px(a, 300), bc.h), color_hex(T_RED), ALIGN_LEFT);
    ui_record("td:cancel", bk); ui_record("td:apply", bc); ui_record("td:copy sql", bp);
    if (ui_button(a, bp, "Copy SQL", BTN_QUIET, true)) { if (d->drop_mode) { char qt[600], s[700]; td_qname(d->dialect, d->orig.schema, d->orig.name, qt, sizeof qt); snprintf(s, sizeof s, "DROP TABLE %s;\n", qt); SDL_SetClipboardText(s); } else SDL_SetClipboardText(d->script ? d->script : ""); app_status(a, "SQL copied (not run)"); }
    if (ui_button(a, bk, "Cancel", BTN_NORMAL, true) || input_key(in, SDLK_ESCAPE, 0)) { G.modal = M_NONE; d->drop_mode = false; }
    if (ui_button(a, bc, d->drop_mode ? "Drop Table" : "Apply Changes", destructive ? BTN_DANGER : BTN_PRIMARY, ok)) { if (d->drop_mode) apply_drop(a, d); else apply(a, d); }
}

static void draw_close(App *a, TdDoc *d) {
    Rect rc = modal_rect(a, 0.5f, 0.3f); rc.h = px(a, 170); rc.y = (a->height - rc.h) / 2;
    char t[300]; snprintf(t, sizeof t, "Close the designer of %s?", d->draft.name);
    modal_frame(a, rc, t);
    ui_label(a, rect_make(rc.x + px(a, 18), rc.y + px(a, 44), rc.w - px(a, 36), px(a, 40)), "The draft is not applied. Save it to continue later, or discard it. Nothing is sent to the server.", ALIGN_LEFT, T_TEXT_MUTED);
    float bw = px(a, 120), by = rc.y + rc.h - px(a, 48);
    Rect b3 = rect_make(rc.x + rc.w - px(a, 18) - bw, by, bw, px(a, 32)), b2 = rect_make(b3.x - bw - px(a, 10), by, bw, px(a, 32)), b1 = rect_make(b2.x - bw - px(a, 10), by, bw, px(a, 32));
    ui_record("td:save draft", b1); ui_record("td:discard draft", b2); ui_record("td:cancel close", b3);
    int tab = G.close_tab;
    if (ui_button(a, b1, "Save Draft", BTN_PRIMARY, true)) { save_draft(a, d); G.modal = M_NONE; panels_close_tab(a->panels, a, tab); return; }
    if (ui_button(a, b2, "Discard", BTN_DANGER, true)) { delete_draft(a, d); td_free(&d->draft); td_copy(&d->draft, &d->orig); G.modal = M_NONE; panels_close_tab(a->panels, a, tab); return; }
    if (ui_button(a, b3, "Cancel", BTN_NORMAL, true) || input_key(&a->in, SDLK_ESCAPE, 0)) G.modal = M_NONE;
}

static void run_menu(App *a, TdDoc *d, int id) {
    if (!alive(d)) return;
    Connection *c = conn_of(a, d->conn_id);
    replan(d);
    if (id >= MI_TYPE0 && id < 1000) {   /* a type from the picker (the component menu's ids start at 1001) */
        TdCol *col = td_col(&d->draft, d->sel_id);
        const char *const *ty = td_types(d->dialect); int n = 0; while (ty[n]) n++;
        if (col && id - MI_TYPE0 < n) { push_undo(d, ""); snprintf(col->type, sizeof col->type, "%s", ty[id - MI_TYPE0]); changed(d); d->f_loaded_rev = 0; }
        return;
    }
    switch (id) {
    case MI_COPY: SDL_SetClipboardText(d->script ? d->script : ""); app_status(a, "SQL copied (identical to the preview; not run)"); break;
    case MI_CONSOLE: { char t[160]; snprintf(t, sizeof t, "%s-ddl.sql", d->draft.name); panels_open_tab(a->panels, a, t, d->conn_id, d->script ? d->script : ""); app_status(a, "A copy of the SQL is in a new tab: running it there is your call; the designer keeps its draft"); break; }
    case MI_MIGRATION: {   /* TBL-063: a deterministic file name, metadata without secrets; never applied */
        const char *md = getenv("LUME_MIGRATIONS_DIR"); char dir[1100];
        if (md && *md) snprintf(dir, sizeof dir, "%s", md); else { pref_dir(a, dir, sizeof dir); strncat(dir, "/migrations", sizeof dir - strlen(dir) - 1); }
        mkdir_p(dir);
        time_t t = time(NULL); struct tm tm; gmtime_r(&t, &tm); char ts[32]; strftime(ts, sizeof ts, "%Y%m%d%H%M%S", &tm);
        char nm[300]; snprintf(nm, sizeof nm, "%s_%s_%s", ts, d->is_new ? "create" : "alter", d->draft.name); sanitize(nm);
        char path[1500]; snprintf(path, sizeof path, "%s/%s.sql", dir, nm);
        FILE *f = fopen(path, "wb");
        if (!f) { app_status(a, "Could not write %s: %s", path, strerror(errno)); break; }
        char q[400]; qualified(d, &d->draft, q, sizeof q);
        fprintf(f, "-- Lume Data table designer migration\n-- connection: %s (%s)\n-- table: %s\n-- base fingerprint: %016llx\n-- generated: %s (not applied)\n\n",
                d->conn_name, td_dialect_name(d->dialect), q, (unsigned long long)d->orig.fingerprint, ts);
        fputs(d->script ? d->script : "", f); fclose(f);
        app_status(a, "Saved %s (not applied)", path);
        break;
    }
    case MI_CREATE_DDL: { TdPlan p; TdPlanOpts o; memset(&o, 0, sizeof o); o.version = d->version; TdTable t; td_copy(&t, &d->draft); t.exists = false; td_plan(d->dialect, NULL, &t, &o, &p); char *s = td_plan_script(d->dialect, &p); SDL_SetClipboardText(s); free(s); td_plan_free(&p); td_free(&t); app_status(a, "CREATE DDL copied"); break; }
    case MI_AI_REVIEW: {   /* roadmap item 17: the table's DDL in a query tab, reviewed there by the assistant (nothing changes here) */
        TdPlan p; TdPlanOpts o; memset(&o, 0, sizeof o); o.version = d->version; TdTable t; td_copy(&t, &d->draft); t.exists = false;
        td_plan(d->dialect, NULL, &t, &o, &p); char *ddl = td_plan_script(d->dialect, &p);
        size_t L = strlen(ddl) + 200; char *txt = malloc(L); snprintf(txt, L, "-- %s, from the Table Designer (as drafted; nothing here runs by itself)\n%s", d->draft.name, ddl);
        char tt[160]; snprintf(tt, sizeof tt, "%s review.sql", d->draft.name);
        int id = panels_open_tab(a->panels, a, tt, d->conn_id, txt);
        if (id > 0) { panels_activate_tab(a->panels, a, id); commands_invoke(a, "intelligence.review_table"); }
        free(txt); free(ddl); td_plan_free(&p); td_free(&t); break;
    }
    case MI_UNDO: do_undo(d); break;
    case MI_REDO: do_redo(d); break;
    case MI_SAVE_DRAFT: save_draft(a, d); app_status(a, "Draft saved (no credentials in it)"); break;
    case MI_DISCARD_DRAFT: delete_draft(a, d); push_undo(d, "discard"); td_free(&d->draft); if (d->is_new) { td_init(&d->draft); snprintf(d->draft.schema, sizeof d->draft.schema, "%s", d->schema); snprintf(d->draft.name, sizeof d->draft.name, "new_table"); d->draft.can_alter = true; } else td_copy(&d->draft, &d->orig); changed(d); d->banner[0] = 0; sel(d, SEL_TABLE, 0); break;
    case MI_RELOAD: d->drift = false; d->banner[0] = 0; d->state = DS_LOADING; start_snapshot(a, d, PH_LOAD, d->schema, d->name); break;
    case 1001: add_column(d, d->sel_kind == SEL_COL ? d->sel_id : 0); break;
    case 1002: duplicate_col(d); break;
    case 1003: case 1004: if (d->is_new || !td_col_c(&d->orig, d->sel_id) || td_caps(d->dialect, d->version).column_order == TCAP_YES) { push_undo(d, ""); if (td_move_col(&d->draft, d->sel_id, id == 1003 ? -1 : 1)) changed(d); } else app_status(a, "%s cannot move existing columns (it would need a rebuild)", td_dialect_name(d->dialect)); break;
    case 1005: remove_selected(d); break;
    case 1006: {
        char b[2000] = "";
        if (d->sel_kind == SEL_COL) { const TdCol *cc = td_col(&d->draft, d->sel_id); if (cc) { char qc[300]; td_quote(d->dialect, cc->name, qc, sizeof qc); snprintf(b, sizeof b, "%s %s%s%s%s", qc, cc->type, cc->def[0] ? " DEFAULT " : "", cc->def, cc->notnull ? " NOT NULL" : ""); } }
        else if (d->sel_kind == SEL_KEY) { const TdKey *k = td_key(&d->draft, d->sel_id); if (k) snprintf(b, sizeof b, "%s", k->kind == TK_CHECK ? k->check : k->raw[0] ? k->raw : k->name); }
        SDL_SetClipboardText(b); app_status(a, "Copied"); break;
    }
    case 1007: { const TdKey *k = td_key(&d->draft, d->sel_id); if (k && c) tdes_open_table(a, c, k->ref_schema[0] ? k->ref_schema : d->draft.schema, k->ref_table, 0); break; }   /* this draft stays as it is (TBL-085.2) */
    case 1008: push_undo(d, "revert"); td_free(&d->draft); td_copy(&d->draft, &d->orig); changed(d); break;
    case 1009: restore_removed(d); break;
    default:
        if (id >= 2000 && id < 3000 && d->sel_kind == SEL_KEY) {   /* Add column (the key's column picker) */
            TdKey *k = td_key(&d->draft, d->sel_id); int ci = id - 2000;
            if (k && ci < d->draft.ncols && k->ncols < TD_MAXKC) {
                push_undo(d, ""); k->cols[k->ncols] = d->draft.cols[ci].id; k->desc[k->ncols] = false; k->ncols++;
                if (k->kind == TK_FK) { while (k->nref < k->ncols) k->ref_cols[k->nref++][0] = 0; if (!k->ref_cols[k->ncols - 1][0]) ref_cols_from_pk(a, d, k); }
                changed(d); d->f_loaded_rev = 0;
            }
            break;
        }
        if (id >= 3000 && d->sel_kind == SEL_KEY) {   /* a referenced column */
            TdKey *k = td_key(&d->draft, d->sel_id); int pos = (id - 3000) / 256, rc = (id - 3000) % 256;
            DbTable *rt = k ? ref_table_of(a, d, k) : NULL;
            if (k && rt && pos < TD_MAXKC && rc < rt->ncols) { push_undo(d, ""); while (k->nref <= pos) k->ref_cols[k->nref++][0] = 0; snprintf(k->ref_cols[pos], 128, "%s", rt->cols[rc].name); changed(d); d->f_loaded_rev = 0; }
            break;
        }
        if (id >= 1100 && id < 1100 + TK_NKINDS) add_key(d, id - 1100);
        else if (id >= 1110 && id <= 1114) { int kind = id - 1110; if (kind == TK_PK && td_pk(&d->draft)) { TdKey *pk = td_pk(&d->draft); if (pk->ncols < TD_MAXKC && !td_col_in_kind(&d->draft, d->sel_id, TK_PK)) { push_undo(d, ""); pk->cols[pk->ncols++] = d->sel_id; changed(d); } } else add_key(d, kind); }
    }
}

/* ---- per frame ---- */

void tdes_pre_update(App *a) {
    G.a = a;
    for (int i = 0; i < MAX_TDOCS; i++) if (G.docs[i]) poll_doc(a, G.docs[i]);
    if (G.menu.open) { int id = popup_update(a, &G.menu); if (id >= 0) { popup_close(&G.menu); run_menu(a, G.menu_doc, id); } }
    if (G.types.open) { int id = popup_update(a, &G.types); if (id >= 0) { popup_close(&G.types); run_menu(a, G.menu_doc, id); } }
    if (G.modal != M_NONE && !alive(G.mdoc)) G.modal = M_NONE;
    if (G.modal != M_NONE) {   /* the dialog owns this frame's input */
        G.stash = a->in; G.stashed = true;
        a->in.mouse_pressed = a->in.mouse_released = a->in.mouse_rpressed = false; a->in.mouse_consumed = true;
        a->in.nkeys = 0; a->in.ntext = 0; a->in.wheel_x = a->in.wheel_y = 0;
    }
}
void tdes_draw_overlay(App *a) {
    if (G.stashed) {
        bool down = a->in.mouse_down; float mx = a->in.mouse_x, my = a->in.mouse_y;
        a->in = G.stash; a->in.mouse_down = down; a->in.mouse_x = mx; a->in.mouse_y = my;
        G.stashed = false;
    }
    if (G.modal == M_REVIEW && alive(G.mdoc)) draw_review(a, G.mdoc);
    else if (G.modal == M_CLOSE && alive(G.mdoc)) draw_close(a, G.mdoc);
    if (G.menu.open) popup_draw(a, &G.menu);
    if (G.types.open) popup_draw(a, &G.types);
}
bool tdes_modal_open(void) { return G.modal != M_NONE; }

bool tdes_request_close(App *a, TdDoc *d, int tab_id) {
    if (!tdes_doc_dirty(d) || d->state != DS_READY) return false;
    G.modal = M_CLOSE; G.mdoc = d; G.close_tab = tab_id; (void)a;
    return true;
}
void tdes_tab_closed(App *a, TdDoc *d) {
    for (int i = 0; i < MAX_TDOCS; i++) if (G.docs[i] == d) G.docs[i] = NULL;
    if (G.mdoc == d) { G.modal = M_NONE; G.mdoc = NULL; }
    if (G.menu_doc == d) { popup_close(&G.menu); popup_close(&G.types); G.menu_doc = NULL; }
    if (d->job) { exec_cancel(d->job); exec_free(d->job); }   /* joins the worker before the document goes */
    td_free(&d->orig); td_free(&d->draft); clear_history(d); td_plan_free(&d->plan); td_plan_free(&d->run); free(d->script);
    (void)a;
    free(d);
}
static void on_close_conn(Connection *c);
void tdes_init(App *a) { memset(&G, 0, sizeof G); G.a = a; conn_add_close_hook(on_close_conn); }
void tdes_shutdown(App *a) {
    conn_remove_close_hook(on_close_conn);
    for (int i = 0; i < MAX_TDOCS; i++) { TdDoc *d = G.docs[i]; if (!d) continue; if (tdes_doc_dirty(d) && d->state == DS_READY) save_draft(a, d); }
}
static void on_close_conn(Connection *c) {
    for (int i = 0; i < MAX_TDOCS; i++) { TdDoc *d = G.docs[i]; if (d && !strcmp(d->conn_id, c->id) && d->job) { exec_cancel(d->job); exec_free(d->job); d->job = NULL; if (d->state == DS_APPLYING) { d->state = DS_READY; set_msg(d, "The connection closed during Apply: the outcome is unknown; Reload after reconnecting (nothing is retried)"); } else if (d->state == DS_LOADING) d->state = DS_LOAD_FAILED; } }
}

/* ---- harness ---- */

static TdDoc *active_doc(App *a) { return a->panels ? panels_active_designer(a->panels, a) : NULL; }
void tdes_dump(App *a) {
    TdDoc *d = active_doc(a);
    if (!d) { printf("tdstate none\n"); fflush(stdout); return; }
    replan(d);
    char q[400]; qualified(d, &d->draft, q, sizeof q);
    static const char *st[] = { "loading", "ready", "load_failed", "applying" };
    printf("tdstate table=%s new=%d state=%s dirty=%d rev=%u changes=%d errors=%d warnings=%d steps=%d blocked=%d risk=%s rollback=%d transactional=%d drift=%d undo=%d redo=%d cols=%d keys=%d can_alter=%d draft_file=%d modal=%d msg=\"%s\" banner=\"%s\"\n",
           q, d->is_new, st[d->state], tdes_doc_dirty(d), d->rev, d->nchanges, count_level(d, TV_ERROR), count_level(d, TV_WARNING), d->plan.n, d->plan.blocked, td_risk_name(d->plan.risk), d->plan.rollback,
           d->plan.transactional, d->drift, d->nundo, d->nredo, d->draft.ncols, d->draft.nkeys, d->orig.can_alter, d->draft_on_disk, G.modal, d->msg, d->banner);
    for (int i = 0; i < d->plan.n; i++) printf("tdsql %d flags=%d: %s\n", i + 1, d->plan.steps[i].flags, d->plan.steps[i].sql);
    if (d->plan.blocked) printf("tdblocked %s\n", d->plan.block_reason);
    for (int i = 0; i < d->nchanges; i++) printf("tdchange %s\n", d->changes[i].text);
    for (int i = 0; i < d->nissues; i++) printf("tdissue %s %s\n", d->issues[i].level == TV_ERROR ? "error" : d->issues[i].level == TV_WARNING ? "warning" : "info", d->issues[i].text);
    for (const char *p = d->datacheck; *p; ) { const char *e = strchr(p, '\n'); int l = e ? (int)(e - p) : (int)strlen(p); printf("tddatacheck %.*s\n", l, p); p += l + (e ? 1 : 0); }
    for (int i = 0; i < d->orig.ncols; i++) printf("tdcol %s %s%s\n", d->orig.cols[i].name, d->orig.cols[i].type, d->orig.cols[i].notnull ? " NOT NULL" : "");
    fflush(stdout);
}
/* td open "conn" schema table | td new "conn" schema | td col NAME | td key NAME | td table | td set FIELD VALUE |
 * td addcol | td addkey pk|unique|fk|check|index | td rm | td undo | td redo | td review | td consent | td apply | td cancel | td datacheck */
void tdes_cmd(App *a, const char *arg) {
    char op[32] = "", rest[600] = ""; sscanf(arg, "%31s %599[^\n]", op, rest);
    if (!strcmp(op, "open") || !strcmp(op, "new") || !strcmp(op, "ddl") || !strcmp(op, "dup") || !strcmp(op, "drop")) {
        char cn[64] = "", sc[128] = "", tb[128] = ""; sscanf(rest, "\"%63[^\"]\" %127s %127s", cn, sc, tb);
        Connection *c = NULL; for (int i = 0; a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->name, cn)) c = a->conns->items[i];
        if (!strcmp(sc, "-")) sc[0] = 0;
        if (c && !c->db && c->state != CONN_CONNECTING) app_connect(a, c);   /* the designer then waits for the session */
        bool ok = c && (op[0] == 'n' ? tdes_new_table(a, c, sc) : tdes_open_table(a, c, sc, tb, op[0] == 'd' && op[1] == 'd' ? 1 : op[1] == 'u' ? 2 : op[1] == 'r' ? 3 : 0));
        if (!ok) { printf("script: td %s failed: %s\n", op, rest); fflush(stdout); }
        return;
    }
    TdDoc *d = active_doc(a);
    if (!d) { printf("script: td: no designer tab\n"); fflush(stdout); return; }
    replan(d);
    if (!strcmp(op, "table")) sel(d, SEL_TABLE, 0);
    else if (!strcmp(op, "col")) { for (int i = 0; i < d->draft.ncols; i++) if (!strcmp(d->draft.cols[i].name, rest)) sel(d, SEL_COL, d->draft.cols[i].id); }
    else if (!strcmp(op, "gone")) { for (int i = 0; i < d->orig.ncols; i++) if (!strcmp(d->orig.cols[i].name, rest)) sel(d, SEL_GONE_COL, d->orig.cols[i].id); }
    else if (!strcmp(op, "key")) { for (int i = 0; i < d->draft.nkeys; i++) if (!strcmp(d->draft.keys[i].name, rest)) sel(d, SEL_KEY, d->draft.keys[i].id); }
    else if (!strcmp(op, "addcol")) add_column(d, d->sel_kind == SEL_COL ? d->sel_id : 0);
    else if (!strcmp(op, "addkey")) { static const char *k[] = { "pk", "unique", "fk", "check", "index" }; for (int i = 0; i < 5; i++) if (!strcmp(rest, k[i])) add_key(d, i); }
    else if (!strcmp(op, "rm")) remove_selected(d);
    else if (!strcmp(op, "dupcol")) duplicate_col(d);
    else if (!strcmp(op, "restore")) restore_removed(d);
    else if (!strcmp(op, "undo")) do_undo(d);
    else if (!strcmp(op, "redo")) do_redo(d);
    else if (!strcmp(op, "up") || !strcmp(op, "down")) run_menu(a, d, op[0] == 'u' ? 1003 : 1004);
    else if (!strcmp(op, "set")) {   /* as the property fields would: one undo step per field */
        char f[32] = "", v[512] = ""; sscanf(rest, "%31s %511[^\n]", f, v);
        if (!strcmp(v, "''")) v[0] = 0;
        push_undo(d, f);
        TdCol *c = d->sel_kind == SEL_COL ? td_col(&d->draft, d->sel_id) : NULL;
        TdKey *k = d->sel_kind == SEL_KEY ? td_key(&d->draft, d->sel_id) : NULL;
        if (d->sel_kind == SEL_TABLE) {
            if (!strcmp(f, "name")) snprintf(d->draft.name, sizeof d->draft.name, "%s", v);
            else if (!strcmp(f, "comment")) snprintf(d->draft.comment, sizeof d->draft.comment, "%s", v);
            else if (!strcmp(f, "schema")) snprintf(d->draft.schema, sizeof d->draft.schema, "%s", v);
        } else if (c) {
            if (!strcmp(f, "name")) snprintf(c->name, sizeof c->name, "%s", v);
            else if (!strcmp(f, "type")) snprintf(c->type, sizeof c->type, "%s", v);
            else if (!strcmp(f, "default")) snprintf(c->def, sizeof c->def, "%s", v);
            else if (!strcmp(f, "comment")) snprintf(c->comment, sizeof c->comment, "%s", v);
            else if (!strcmp(f, "notnull")) c->notnull = atoi(v) != 0;
            else if (!strcmp(f, "identity")) c->identity = atoi(v);
            else if (!strcmp(f, "gen")) snprintf(c->gen, sizeof c->gen, "%s", v);
            else if (!strcmp(f, "using")) { int s = -1; for (int i = 0; i < d->nusing; i++) if (d->using_col[i] == c->id) s = i; if (s < 0 && d->nusing < TD_MAXKC) { s = d->nusing++; d->using_col[s] = c->id; } if (s >= 0) snprintf(d->using_expr[s], 256, "%s", v); }
        } else if (k) {
            if (!strcmp(f, "name")) snprintf(k->name, sizeof k->name, "%s", v);
            else if (!strcmp(f, "cols")) { char bad[128]; k->ncols = parse_cols(d, v, k->cols, k->desc, TD_MAXKC, bad, sizeof bad); }
            else if (!strcmp(f, "ref")) { char *dot = strchr(v, '.'); if (dot) { snprintf(k->ref_schema, sizeof k->ref_schema, "%.*s", (int)(dot - v), v); snprintf(k->ref_table, sizeof k->ref_table, "%s", dot + 1); } else { snprintf(k->ref_schema, sizeof k->ref_schema, "%s", d->draft.schema); snprintf(k->ref_table, sizeof k->ref_table, "%s", v); } }
            else if (!strcmp(f, "refcols")) { k->nref = 0; for (char *t = strtok(v, ","); t && k->nref < TD_MAXKC; t = strtok(NULL, ",")) { while (*t == ' ') t++; snprintf(k->ref_cols[k->nref++], 128, "%s", t); } }
            else if (!strcmp(f, "check")) snprintf(k->check, sizeof k->check, "%s", v);
            else if (!strcmp(f, "where")) snprintf(k->where, sizeof k->where, "%s", v);
            else if (!strcmp(f, "unique")) k->unique = atoi(v) != 0;
            else if (!strcmp(f, "on_delete")) snprintf(k->on_delete, sizeof k->on_delete, "%s", v);
        }
        changed(d); d->f_loaded_rev = 0; d->undo_key[0] = 0;
    }
    else if (!strcmp(op, "review")) { replan(d); if (d->plan.n && !d->plan.blocked && !read_only(d)) { G.modal = M_REVIEW; G.mdoc = d; d->consent = false; d->datacheck[0] = 0; d->approved = td_plan_hash(&d->plan, d->rev); } else { printf("script: td review refused\n"); fflush(stdout); } }
    else if (!strcmp(op, "consent")) d->consent = true;
    else if (!strcmp(op, "prodname")) textfield_set(&d->prod_name, rest);
    else if (!strcmp(op, "apply")) { if (G.modal == M_REVIEW && G.mdoc == d) { bool prod = conn_of(a, d->conn_id) && conn_of(a, d->conn_id)->env == CONN_ENV_PROD; bool destr = d->drop_mode || (d->plan.flags & TS_DESTRUCTIVE) || d->plan.risk == RISK_HIGH;
            if ((destr && !d->consent) || (prod && strcmp(d->prod_name.buf, d->drop_mode ? d->orig.name : d->draft.name))) { printf("script: td apply refused (consent / PROD name)\n"); fflush(stdout); } else if (d->drop_mode) apply_drop(a, d); else apply(a, d); } else { printf("script: td apply: no review open\n"); fflush(stdout); } }
    else if (!strcmp(op, "cancel")) { G.modal = M_NONE; d->drop_mode = false; }
    else if (!strcmp(op, "datacheck")) start_datacheck(a, d);
    else if (!strcmp(op, "menu")) { int id = atoi(rest); run_menu(a, d, id); }
    else if (!strcmp(op, "savedraft")) save_draft(a, d);
    else if (!strcmp(op, "close")) { if (!tdes_request_close(a, d, d->tab_id)) panels_close_tab(a->panels, a, d->tab_id); }
    else if (!strcmp(op, "closeanswer")) { if (G.modal == M_CLOSE) { int tab = G.close_tab; if (!strcmp(rest, "save")) { save_draft(a, d); G.modal = M_NONE; panels_close_tab(a->panels, a, tab); } else if (!strcmp(rest, "discard")) { delete_draft(a, d); G.modal = M_NONE; panels_close_tab(a->panels, a, tab); } else G.modal = M_NONE; } }
    else { printf("script: td: unknown op %s\n", op); fflush(stdout); }
}
bool tdes_menu_pos(App *a, const char *label, float *x, float *y) {
    Rect rc;
    PopupMenu *m = G.types.open ? &G.types : G.menu.open ? &G.menu : NULL;
    if (!m || !popup_item_rect(a, m, label, &rc)) return false;
    *x = (rc.x + rc.w / 2) / a->density; *y = (rc.y + rc.h / 2) / a->density;
    return true;
}
