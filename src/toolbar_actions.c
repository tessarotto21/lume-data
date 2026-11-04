/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Main toolbar: commands and dialogs (see toolbar.h / toolbar_int.h). UI thread only.
 *
 * Runs from the toolbar, the Query menu and the shortcuts all come here (toolbar_action is
 * called by main.c's do_action first), so the safety rules apply whatever the entry point:
 *
 *   - the target (tab, connection, session generation) is captured when the action starts and
 *     checked again before anything is sent: a target that changed meanwhile is refused, never
 *     silently redirected (spec 8.2, TBR-025);
 *   - Run Selection never runs text outside the selection; a selection that cuts a statement
 *     asks first (TBR-018);
 *   - on a PROD connection, SQL that may write and multi-statement scripts are confirmed with
 *     the target in full (connection, schema, environment); the keyword test is conservative and
 *     the server's permissions remain the rule (TBR-019 / 029);
 *   - "read-only in Lume" at the Lume-check level (Oracle) refuses such SQL with the reason;
 *   - parameter values are substituted as quoted literals and never shown in the Output nor kept
 *     in the history (the template is shown instead) (TBR-024);
 *   - Commit / Rollback are confirmed with the target and logged as what the server answered;
 *     a lost session is reported as lost, never as committed (TBR-012 / 026 / 027).
 *
 * Dialogs are drawn in two passes like the connection dialog: input in toolbar_frame (before
 * the panels), drawing in toolbar_draw_overlay (on top).
 */
#include "toolbar_int.h"
#include "crashrep.h"
#include "update.h"
#include "planview.h"
#include "plan.h"
#include <ctype.h>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "conn.h"
#include "db.h"
#include "draw.h"
#include "editor.h"
#include "exec.h"
#include "font.h"
#include "log.h"
#include "menu.h"
#include "output.h"
#include "panels.h"
#include "results.h"
#include "theme.h"
#include "sshtun.h"

/* ---- helpers ---------------------------------------------------------------------- */

void tba_run_free(TbRun *r) { free(r->sql); free(r->display); free(r->export_path); free(r->plan_stmt); memset(r, 0, sizeof *r); }

static Connection *ctx_conn(App *a) { const LumeCtx *c = commands_ctx(); return c->has_conn ? tb_conn_by_id(a, c->conn_id) : NULL; }

/* "Demo (in-memory) · SQLite · public · PROD" for confirmations (TBR-026 / 029). */
static void target_text(App *a, const LumeCtx *c, char *out, size_t n) {
    (void)a;
    snprintf(out, n, "%s  \xC2\xB7  %s%s%s%s%s", c->conn_name, c->caps ? c->caps->name : c->driver,
             c->schema[0] ? "  \xC2\xB7  schema " : "", c->schema, c->env ? "  \xC2\xB7  " : "", c->env ? conn_env_name(c->env) : "");
}

/* Line numbers (1-based) of byte range [base, base + len) in the active editor. */
static void line_range(App *a, int base, int len, int *l0, int *l1) {
    int tl = 0; char *t = editor_text(a->editor, &tl);
    *l0 = *l1 = 1;
    for (int i = 0; t && i < base + len && i < tl; i++) if (t[i] == '\n') { if (i < base) (*l0)++; (*l1)++; }
    free(t);
}

void tba_confirm4(Toolbar *t, int kind, const char *title, const char *body, const char *b0, const char *b1, const char *b2, const char *b3, const char *arg) {
    tba_confirm(t, kind, title, body, b0, b1, b2, arg);
    if (b3) snprintf(t->cf_btn[t->cf_nbtn++], 40, "%s", b3);
}

void tba_confirm(Toolbar *t, int kind, const char *title, const char *body, const char *b0, const char *b1, const char *b2, const char *arg) {
    t->modal = TM_CONFIRM; t->cf_kind = kind; t->cf_choice = -1;
    snprintf(t->cf_title, sizeof t->cf_title, "%s", title ? title : "");
    snprintf(t->cf_body, sizeof t->cf_body, "%s", body ? body : "");
    t->cf_nbtn = 0;
    if (b0) snprintf(t->cf_btn[t->cf_nbtn++], 40, "%s", b0);
    if (b1) snprintf(t->cf_btn[t->cf_nbtn++], 40, "%s", b1);
    if (b2) snprintf(t->cf_btn[t->cf_nbtn++], 40, "%s", b2);
    snprintf(t->cf_arg, sizeof t->cf_arg, "%s", arg ? arg : "");
}

/* Remembers what ran in a session (Re-run Last, TBR-023): the exact text, not the editor's. */
static void remember_run(Toolbar *t, const char *conn_id, const char *sql, int len, const char *title) {
    int k = -1; for (int i = 0; i < t->nlast; i++) if (!strcmp(t->last[i].conn_id, conn_id)) k = i;
    if (k < 0) { if (t->nlast == 8) { free(t->last[0].sql); memmove(&t->last[0], &t->last[1], sizeof t->last[0] * 7); t->nlast--; } k = t->nlast++; t->last[k].sql = NULL; }
    free(t->last[k].sql);
    t->last[k].sql = malloc((size_t)len + 1); memcpy(t->last[k].sql, sql, (size_t)len); t->last[k].sql[len] = 0; t->last[k].len = len;
    snprintf(t->last[k].conn_id, 16, "%s", conn_id);
    snprintf(t->last[k].title, sizeof t->last[k].title, "%s", title ? title : "");
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); strftime(t->last[k].when, sizeof t->last[k].when, "%H:%M:%S", &tm);
}

/* ---- execution ------------------------------------------------------------------------ */

/* Sends a prepared run, after checking the target is still the one captured at the click. */
static void execute(Toolbar *t, App *a, TbRun *r) {
    commands_frame(a);
    const LumeCtx *c = commands_ctx();
    if (strcmp(c->conn_id, r->conn_id) || c->conn_generation != r->conn_generation || !c->connected) {
        app_status(a, "Not run: the tab's connection changed since you asked (it was %s); nothing was sent", r->conn_id[0] ? "another one" : "none");
        LOG_W("run refused: target changed between the request and the confirmation");
        tba_run_free(r); return;
    }
    Connection *cc = tb_conn_by_id(a, r->conn_id);
    /* Tx: Manual on an engine with BEGIN: a transaction is opened first if none is (TBR-016) */
    char *sql = r->sql; int len = r->len; char *joined = NULL;
    if (cc && cc->manual_tx && c->tx_state == 0 && cc->db && cc->db->drv->tx_begin) {
        size_t bl = strlen(cc->db->drv->tx_begin);
        joined = malloc(bl + 3 + (size_t)len);
        memcpy(joined, cc->db->drv->tx_begin, bl); memcpy(joined + bl, ";\n", 2); memcpy(joined + bl + 2, sql, (size_t)len);
        len += (int)bl + 2; sql = joined;
    }
    OutRunOpts op = { .mode = r->mode, .title = r->title[0] ? r->title : NULL, .note = r->note[0] ? r->note : NULL, .display = r->display, .export_path = r->export_path };
    int id = app_run_sql_opts(a, sql, len, joined ? -1 : r->base, &op);
    if (id > 0) remember_run(t, r->conn_id, r->sql, r->len, r->title);
    free(joined);
    tba_run_free(r);
}

/* Lume's read-only check refused a run: the reason goes to the status bar and, so it does not
 * vanish with the next status, to the Output as a skipped execution. With parameters the
 * Output shows the template (r->display), never the substituted values. */
static void refuse_read_only(App *a, const LumeCtx *c, const TbRun *r) {
    char why[400];
    snprintf(why, sizeof why, "Not run: %s is read-only in Lume and this SQL may write (%s)", c->conn_name, caps_ro_text(RO_LUME_CHECK));
    app_status_bar(a, "%s", why);   /* output_refused records it: no second note */
    output_refused(a->output, a, ctx_conn(a), r->display ? r->display : r->sql, r->display ? -1 : r->len, why);
    LOG_I("run refused by Lume's read-only check on %s", c->conn_name);
}

/* Prepares a run from the editor: source, safety checks, then executes or asks. */
static void start_run(Toolbar *t, App *a, int src, int mode, bool allow_partial) {
    commands_frame(a);
    const LumeCtx *c = commands_ctx();
    char why[200] = "";
    if (!c->connected) { app_status(a, "Not run: %s", c->has_conn ? "the tab's connection is not open" : "this tab has no connection"); return; }
    if (c->running) { app_status(a, "A statement is running on %s: Stop it first", c->conn_name); return; }
    TbRun r; memset(&r, 0, sizeof r);
    r.src = src; r.mode = mode;
    r.sql = app_editor_sql(a, src, allow_partial, &r.len, &r.base, why, sizeof why);
    if (!r.sql) {
        if (src == RUNSRC_SELECTION && strstr(why, "part of a statement")) {
            t->run = r; t->run.src = src; t->run.mode = mode;
            tba_confirm(t, CF_RUN, "Run a partial selection?", "The selection covers only part of a statement. Lume never extends it to a whole statement: it can run exactly the selected text, which may fail or do something different.", "Run exactly the selection", "Cancel", NULL, "partial");
            return;
        }
        app_status(a, "Not run: %s", why); return;
    }
    snprintf(r.conn_id, sizeof r.conn_id, "%s", c->conn_id); r.conn_generation = c->conn_generation;
    int l0, l1; line_range(a, r.base, r.len, &l0, &l1);
    const char *what = src == RUNSRC_SELECTION ? "Run Selection" : src == RUNSRC_FILE ? "Run All in File" : src == RUNSRC_STATEMENT ? "Run Statement" : "Run";
    snprintf(r.note, sizeof r.note, "%s: lines %d-%d on %s (%s%s%s)", what, l0, l1, c->conn_name, c->caps ? c->caps->name : c->driver, c->schema[0] ? ", schema " : "", c->schema);
    /* safety (TBR-013 / 019 / 029) */
    SqlStmt parts[256]; int np = sql_split(r.sql, r.len, SQL_DIALECT_GENERIC, parts, 256);
    bool writes = false;
    for (int i = 0; i < np && !writes; i++) writes = caps_sql_may_write(r.sql + parts[i].start, parts[i].end - parts[i].start);
    if (c->read_only && writes && c->ro_level == RO_LUME_CHECK) { refuse_read_only(a, c, &r); tba_run_free(&r); return; }
    if (c->env == CONN_ENV_PROD && (writes || np > 1)) {
        char tg[200]; target_text(a, c, tg, sizeof tg);
        char body[700], snip[160]; snprintf(snip, sizeof snip, "%.*s", r.len > 140 ? 140 : r.len, r.sql);
        for (char *p = snip; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
        snprintf(body, sizeof body, "Target: %s\n%d statement%s%s.\n%s\n\nLume checks the SQL text conservatively; the server's permissions and rules decide what is allowed.",
                 tg, np, np == 1 ? "" : "s", writes ? ", and at least one may change data or schema" : " in one script", snip);
        t->run = r;
        tba_confirm(t, CF_RUN, "Run on PROD?", body, "Run on PROD", "Cancel", NULL, "");
        return;
    }
    execute(t, a, &r);
}

/* Strips one trailing ';' (and spaces) so a prefix like EXPLAIN applies to a single statement. */
static int trim_statement(const char *s, int len) {
    while (len > 0 && (isspace((unsigned char)s[len - 1]) || s[len - 1] == ';')) len--;
    return len;
}

static void start_explain(Toolbar *t, App *a, bool analyze) {
    commands_frame(a);
    const LumeCtx *c = commands_ctx();
    char why[200] = "";
    int len = 0, base = 0;
    char *st = app_editor_sql(a, RUNSRC_AUTO, false, &len, &base, why, sizeof why);
    if (!st) { app_status(a, "Explain: %s", why); return; }
    len = trim_statement(st, len);
    if (!planview_supported(c->driver, analyze)) { free(st); app_status(a, "Explain: %s for this driver", analyze ? "no actual plan" : "not supported"); return; }
    Connection *cc = ctx_conn(a);
    if (!analyze) {   /* the graphical plan (EPL-001): estimated, not run */
        char note[200]; snprintf(note, sizeof note, "Execution plan of a statement on %s", c->conn_name);
        planview_explain(a, cc, st, len, 0, note); free(st); return;
    }
    const DriverCaps *k = c->caps;
    if (cc && plan_stmt_writes(st) && (cc->env == CONN_ENV_PROD || cc->read_only)) { planview_explain(a, cc, st, len, 1, NULL); free(st); return; }   /* refused before any question (EPL-072) */
    TbRun r; memset(&r, 0, sizeof r);
    snprintf(r.conn_id, sizeof r.conn_id, "%s", c->conn_id); r.conn_generation = c->conn_generation;
    r.mode = RUN_NEW_TAB; r.base = -1;
    r.plan_actual = true;   /* after the confirmation the plan goes to the Execution Plan panel */
    r.plan_stmt = malloc((size_t)len + 1); memcpy(r.plan_stmt, st, (size_t)len); r.plan_stmt[len] = 0;
    (void)k;
    char snip[48]; snprintf(snip, sizeof snip, "%.*s", len > 40 ? 40 : len, st);
    for (char *p = snip; *p; p++) if (*p == '\n') *p = ' ';
    snprintf(r.title, sizeof r.title, "%s \xC2\xB7 %s", analyze ? "Plan (analyze)" : "Plan", snip);
    bool writes = caps_sql_may_write(st, len);
    size_t cap = (size_t)len + 256;
    r.sql = malloc(cap);
    if (!strcmp(c->driver, "oracle"))
        r.len = snprintf(r.sql, cap, "%s%.*s;\nSELECT plan_table_output FROM TABLE(DBMS_XPLAN.DISPLAY())", k->explain_prefix, len, st);
    else if (analyze && writes && !strcmp(c->driver, "postgres"))
        r.len = snprintf(r.sql, cap, "BEGIN;\n%s%.*s;\nROLLBACK", k->explain_analyze_prefix, len, st);   /* side effects undone */
    else r.len = snprintf(r.sql, cap, "%s%.*s", analyze ? k->explain_analyze_prefix : k->explain_prefix, len, st);
    snprintf(r.note, sizeof r.note, "%s of a statement on %s", analyze ? "EXPLAIN ANALYZE" : "Execution plan", c->conn_name);
    free(st);
    if (analyze) {
        /* TBR-022: EXPLAIN ANALYZE runs the statement for real */
        char body[600];
        snprintf(body, sizeof body, "EXPLAIN ANALYZE executes the statement to measure it.%s\n\nTarget: %s",
                 writes ? (strcmp(c->driver, "postgres") == 0 ? "\nThis statement may change data: Lume runs it inside a transaction that is rolled back afterwards." : "\nThis statement may change data.") : "\nIt is a read, but it still runs completely on the server.",
                 c->conn_name);
        t->run = r;
        tba_confirm(t, CF_RUN, "Run EXPLAIN ANALYZE?", body, writes ? "Analyze (rolled back)" : "Analyze", "Cancel", NULL, "");
        return;
    }
    execute(t, a, &r);
}

/* Parameters (TBR-024): ":name", "$1" and "?" outside strings and comments; ":: " casts skipped. */
static int find_params(const char *s, int len, TbParam *out, int max) {
    int n = 0, q = 0;
    for (int i = 0; i < len; i++) {
        char ch = s[i];
        if (ch == '\'' || ch == '"') { char qq = ch; i++; while (i < len && s[i] != qq) i++; continue; }
        if (ch == '-' && i + 1 < len && s[i + 1] == '-') { while (i < len && s[i] != '\n') i++; continue; }
        if (ch == '/' && i + 1 < len && s[i + 1] == '*') { i += 2; while (i + 1 < len && !(s[i] == '*' && s[i + 1] == '/')) i++; i++; continue; }
        char nm[40] = "";
        if (ch == ':' && i + 1 < len && (isalpha((unsigned char)s[i + 1]) || s[i + 1] == '_') && (i == 0 || s[i - 1] != ':')) {
            int j = i + 1; while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '_') && j - i < 38) j++;
            snprintf(nm, sizeof nm, ":%.*s", j - i - 1, s + i + 1); i = j - 1;
        } else if (ch == '$' && i + 1 < len && isdigit((unsigned char)s[i + 1])) {
            int j = i + 1; while (j < len && isdigit((unsigned char)s[j])) j++;
            snprintf(nm, sizeof nm, "$%.*s", j - i - 1, s + i + 1); i = j - 1;
        } else if (ch == '?') snprintf(nm, sizeof nm, "?%d", ++q);
        if (!nm[0]) continue;
        bool dup = false; for (int k = 0; k < n; k++) if (!strcmp(out[k].name, nm)) dup = true;
        if (!dup && n < max) { memset(&out[n], 0, sizeof out[n]); snprintf(out[n].name, sizeof out[n].name, "%s", nm); n++; }
    }
    return n;
}

/* Text with every placeholder replaced by its value as a literal: numbers bare, NULL as NULL,
 * anything else quoted with '' doubled. Returns malloc'd text. */
static char *substitute(const char *s, int len, TbParam *p, int np, int *out_len) {
    size_t cap = (size_t)len * 2 + 64; for (int k = 0; k < np; k++) cap += (size_t)p[k].f.len * 2 + 4;
    char *o = malloc(cap); size_t w = 0; int q = 0;
    for (int i = 0; i < len; i++) {
        char ch = s[i];
        if (ch == '\'' || ch == '"') { char qq = ch; o[w++] = s[i++]; while (i < len && s[i] != qq) o[w++] = s[i++]; if (i < len) o[w++] = s[i]; continue; }
        if (ch == '-' && i + 1 < len && s[i + 1] == '-') { while (i < len && s[i] != '\n') o[w++] = s[i++]; if (i < len) o[w++] = s[i]; continue; }
        char nm[40] = ""; int skip = 0;
        if (ch == ':' && i + 1 < len && (isalpha((unsigned char)s[i + 1]) || s[i + 1] == '_') && (i == 0 || s[i - 1] != ':')) { int j = i + 1; while (j < len && (isalnum((unsigned char)s[j]) || s[j] == '_') && j - i < 38) j++; snprintf(nm, sizeof nm, ":%.*s", j - i - 1, s + i + 1); skip = j - i; }
        else if (ch == '$' && i + 1 < len && isdigit((unsigned char)s[i + 1])) { int j = i + 1; while (j < len && isdigit((unsigned char)s[j])) j++; snprintf(nm, sizeof nm, "$%.*s", j - i - 1, s + i + 1); skip = j - i; }
        else if (ch == '?') { snprintf(nm, sizeof nm, "?%d", ++q); skip = 1; }
        int k = -1; for (int m = 0; nm[0] && m < np; m++) if (!strcmp(p[m].name, nm)) k = m;
        if (k < 0) { o[w++] = ch; continue; }
        const char *v = p[k].f.buf;
        bool num = *v != 0; for (const char *x = v; *x; x++) if (!(isdigit((unsigned char)*x) || *x == '.' || (*x == '-' && x == v))) num = false;
        if (!strcasecmp(v, "NULL")) { memcpy(o + w, "NULL", 4); w += 4; }
        else if (num) { size_t vl = strlen(v); memcpy(o + w, v, vl); w += vl; }
        else { o[w++] = '\''; for (const char *x = v; *x; x++) { if (*x == '\'') o[w++] = '\''; o[w++] = *x; } o[w++] = '\''; }
        i += skip - 1;
    }
    o[w] = 0; *out_len = (int)w;
    return o;
}

static void start_params(Toolbar *t, App *a) {
    commands_frame(a);
    const LumeCtx *c = commands_ctx();
    char why[200] = ""; TbRun r; memset(&r, 0, sizeof r);
    r.sql = app_editor_sql(a, RUNSRC_AUTO, false, &r.len, &r.base, why, sizeof why);
    if (!r.sql) { app_status(a, "Run with Parameters: %s", why); return; }
    t->nparams = find_params(r.sql, r.len, t->params, TB_MAX_PARAMS);
    if (!t->nparams) { app_status(a, "No parameters in this SQL (:name, $1 or ?): use Run"); free(r.sql); return; }
    snprintf(r.conn_id, sizeof r.conn_id, "%s", c->conn_id); r.conn_generation = c->conn_generation; r.mode = RUN_EDITOR;
    t->run = r; t->param_focus = 0; t->modal = TM_PARAMS;
}

static void params_run(Toolbar *t, App *a) {
    TbRun *r = &t->run;
    int nl = 0; char *sub = substitute(r->sql, r->len, t->params, t->nparams, &nl);
    r->display = r->sql; r->sql = sub; r->len = nl;   /* the Output and the history show the template */
    snprintf(r->note, sizeof r->note, "Run with Parameters: %d value%s, not recorded", t->nparams, t->nparams == 1 ? "" : "s");
    for (int k = 0; k < t->nparams; k++) textfield_set(&t->params[k].f, "");   /* no value outlives the dialog */
    t->modal = TM_NONE;
    TbRun run = *r; memset(r, 0, sizeof *r);
    /* same safety rules as Run */
    const LumeCtx *c = commands_ctx();
    bool writes = caps_sql_may_write(run.sql, run.len);
    if (c->read_only && writes && c->ro_level == RO_LUME_CHECK) { refuse_read_only(a, c, &run); tba_run_free(&run); return; }
    if (c->env == CONN_ENV_PROD && writes) { t->run = run; char tg[200]; target_text(a, c, tg, sizeof tg); char body[400]; snprintf(body, sizeof body, "Target: %s\nThis SQL may change data or schema.", tg); tba_confirm(t, CF_RUN, "Run on PROD?", body, "Run on PROD", "Cancel", NULL, ""); return; }
    execute(t, a, &run);
}

/* Execute to File: the save dialog answers on another thread; the path is picked up next frame. */
static SDL_AtomicInt file_ready; static char file_path[1100];
static void SDLCALL on_file_chosen(void *ud, const char *const *files, int filter) {
    (void)ud; (void)filter;
    if (!files || !files[0]) return;
    snprintf(file_path, sizeof file_path, "%s", files[0]);
    SDL_SetAtomicInt(&file_ready, 1);
}

void toolbar_run_to_file(Toolbar *t, App *a, const char *path) {
    commands_frame(a);
    const LumeCtx *c = commands_ctx();
    char why[200] = ""; TbRun r; memset(&r, 0, sizeof r);
    r.sql = app_editor_sql(a, RUNSRC_AUTO, false, &r.len, &r.base, why, sizeof why);
    if (!r.sql) { app_status(a, "Execute to File: %s", why); return; }
    snprintf(r.conn_id, sizeof r.conn_id, "%s", c->conn_id); r.conn_generation = c->conn_generation; r.mode = RUN_NEW_TAB;
    r.export_path = strdup(path);
    const char *bn = strrchr(path, '/');
    snprintf(r.note, sizeof r.note, "Execute to File: the first result goes to %s", bn ? bn + 1 : path);
    if (c->env == CONN_ENV_PROD && caps_sql_may_write(r.sql, r.len)) { t->run = r; char tg[200]; target_text(a, c, tg, sizeof tg); char body[400]; snprintf(body, sizeof body, "Target: %s\nThis SQL may change data or schema.", tg); tba_confirm(t, CF_RUN, "Run on PROD?", body, "Run on PROD", "Cancel", NULL, ""); return; }
    execute(t, a, &r);
}

static void start_rerun(Toolbar *t, App *a) {
    commands_frame(a);
    const LumeCtx *c = commands_ctx();
    int k = -1; for (int i = 0; i < t->nlast; i++) if (!strcmp(t->last[i].conn_id, c->conn_id)) k = i;
    if (k < 0) { app_status(a, "Nothing has run on %s yet", c->conn_name); return; }
    TbRun r; memset(&r, 0, sizeof r);
    r.len = t->last[k].len; r.sql = malloc((size_t)r.len + 1); memcpy(r.sql, t->last[k].sql, (size_t)r.len + 1);
    r.base = -1; r.mode = RUN_NEW_TAB;
    snprintf(r.title, sizeof r.title, "%s", t->last[k].title);
    snprintf(r.conn_id, sizeof r.conn_id, "%s", c->conn_id); r.conn_generation = c->conn_generation;
    /* TBR-023: the SQL as it ran, not the editor's current text; a new execution id */
    snprintf(r.note, sizeof r.note, "Re-run of the SQL that ran at %s on %s (the text as it ran, not the editor's current text)", t->last[k].when, c->conn_name);
    if (c->env == CONN_ENV_PROD) {
        char tg[200]; target_text(a, c, tg, sizeof tg);
        char body[700], snip[200]; snprintf(snip, sizeof snip, "%.*s", r.len > 180 ? 180 : r.len, r.sql);
        snprintf(body, sizeof body, "Target: %s\nThe SQL that ran at %s (not the editor's current text):\n%s", tg, t->last[k].when, snip);
        t->run = r;
        tba_confirm(t, CF_RUN, "Re-run on PROD?", body, "Re-run", "Cancel", NULL, "");
        return;
    }
    execute(t, a, &r);
}

/* A statement Lume sends for the toolbar itself (Commit, Rollback, SET ...), logged like any
 * run; follow = what to do when it ends well. */
static int session_statement(Toolbar *t, App *a, const char *sql, const char *note, int follow_kind, const char *follow_arg) {
    commands_frame(a);
    const LumeCtx *c = commands_ctx();
    OutRunOpts op = { .mode = RUN_NEW_TAB, .note = note };
    int id = app_run_sql_opts(a, sql, -1, -1, &op);
    if (id > 0 && follow_kind >= 0 && t->nfollow < 4) {
        t->follow[t->nfollow].exec_id = id; snprintf(t->follow[t->nfollow].conn_id, 16, "%s", c->conn_id);
        t->follow[t->nfollow].kind = follow_kind; snprintf(t->follow[t->nfollow].arg, 64, "%s", follow_arg ? follow_arg : "");
        t->nfollow++;
    }
    return id;
}

/* follow-up kinds */
enum { FU_NONE, FU_SCHEMA, FU_ISOLATION, FU_TX_MANUAL_OFF, FU_SWITCH_DS, FU_DISCONNECT, FU_TX_BEGIN };

void tba_set_schema(Toolbar *t, App *a, const char *schema) {
    commands_frame(a);
    const LumeCtx *c = commands_ctx();
    Connection *cc = ctx_conn(a);
    if (!cc || !cc->db || !c->caps || !c->caps->set_schema_fmt) { app_status(a, "This driver has no default schema to set"); return; }
    if (c->running) { app_status(a, "A statement is running on %s: wait before changing the schema", c->conn_name); return; }
    char q[200], sql[300];
    if (schema) cc->db->drv->quote_ident(schema, q, sizeof q);
    else if (!strcmp(c->driver, "postgres")) snprintf(q, sizeof q, "DEFAULT");
    else { const char *user = conn_value(cc, "user"); if (!user || !*user) { app_status(a, "Cannot tell the server's default schema for %s", c->conn_name); return; } cc->db->drv->quote_ident(user, q, sizeof q); }
    snprintf(sql, sizeof sql, c->caps->set_schema_fmt, q);
    char note[200]; snprintf(note, sizeof note, "Default schema of this session: %s", schema ? schema : "server default");
    session_statement(t, a, sql, note, FU_SCHEMA, schema ? schema : "");
}

void tba_set_isolation(Toolbar *t, App *a, const char *level) {
    commands_frame(a);
    const LumeCtx *c = commands_ctx();
    char sql[200];
    if (!strcmp(c->driver, "postgres")) snprintf(sql, sizeof sql, "SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL %s", level);
    else if (!strcmp(c->driver, "oracle")) snprintf(sql, sizeof sql, "ALTER SESSION SET ISOLATION_LEVEL = %s", level);
    else { app_status(a, "Isolation levels are not supported for this driver"); return; }
    char note[160]; snprintf(note, sizeof note, "Isolation level for the next transactions: %s", level);
    session_statement(t, a, sql, note, FU_ISOLATION, level);
}

/* ---- datasource switch (TBR-009) ------------------------------------------------------------- */

static void do_switch(Toolbar *t, App *a, Connection *c) {
    panels_set_tab_connection(a->panels, a, c);
    tb_note_recent(t, c->id);
    app_status(a, c->db ? "This tab now runs on %s (nothing was run)" : "This tab now runs on %s: connecting\xE2\x80\xA6", c->name);
}

void tba_choose_datasource(Toolbar *t, App *a, Connection *c) {
    commands_frame(a);
    const LumeCtx *x = commands_ctx();
    if (!c) return;
    if (x->has_conn && !strcmp(x->conn_id, c->id)) { if (!c->db && c->state != CONN_CONNECTING) commands_invoke(a, "ds.connect"); return; }
    /* only this tab's own execution blocks the switch: a run another tab started on the shared
     * session is not affected by this tab pointing elsewhere */
    if (x->running_here) { app_status(a, "This tab's statement is running on %s: Stop it or wait before switching it", x->conn_name); return; }
    if (x->tx_state == 1) {
        char body[400];
        snprintf(body, sizeof body, "%s has an open transaction. Switching this tab to %s leaves it open in its session unless you end it now.", x->conn_name, c->name);
        tba_confirm(t, CF_SWITCH_DS, "Open transaction", body, "Commit and switch", "Rollback and switch", "Cancel", c->id);
        return;
    }
    do_switch(t, a, c);
}

static void ws_switch(Toolbar *t, App *a, const char *name, bool save);

/* ---- the confirmation's answer --------------------------------------------------------------- */

static void confirm_answer(Toolbar *t, App *a, int b) {
    int kind = t->cf_kind; char arg[64]; snprintf(arg, sizeof arg, "%s", t->cf_arg);
    const char *btn = b >= 0 && b < t->cf_nbtn ? t->cf_btn[b] : "Cancel";
    t->modal = TM_NONE;
    bool cancel = !strcmp(btn, "Cancel");
    if (cancel) {   /* nothing runs, nothing changes (Gherkin: Cancel preserves the session and transaction) */
        if (kind == CF_RUN) { tba_run_free(&t->run); app_status(a, "Cancelled: nothing was run"); }
        return;
    }
    switch (kind) {
    case CF_RUN:
        if (!strcmp(arg, "partial")) { int src = t->run.src, mode = t->run.mode; tba_run_free(&t->run); start_run(t, a, src, mode, true); }
        else if (t->run.plan_actual) {   /* EXPLAIN ANALYZE confirmed: the graphical actual plan (EPL-002 / 072) */
            TbRun r = t->run; memset(&t->run, 0, sizeof t->run);
            Connection *cc = tb_conn_by_id(a, r.conn_id);
            char note[200]; snprintf(note, sizeof note, "EXPLAIN ANALYZE of a statement on %s", cc ? cc->name : "?");
            if (cc) planview_explain(a, cc, r.plan_stmt, (int)strlen(r.plan_stmt), 1, note);
            tba_run_free(&r);
        }
        else { TbRun r = t->run; memset(&t->run, 0, sizeof t->run); execute(t, a, &r); }
        break;
    case CF_SWITCH_DS:
        session_statement(t, a, b == 0 ? "COMMIT" : "ROLLBACK", b == 0 ? "Commit before switching the tab's connection" : "Rollback before switching the tab's connection", FU_SWITCH_DS, arg);
        break;
    case CF_TX_MODE_AUTO:
        session_statement(t, a, b == 0 ? "COMMIT" : "ROLLBACK", b == 0 ? "Commit, then auto-commit mode" : "Rollback, then auto-commit mode", FU_TX_MANUAL_OFF, "");
        break;
    case CF_COMMIT: session_statement(t, a, "COMMIT", "Commit of the session's transaction (requested from the toolbar)", FU_NONE, ""); break;
    case CF_ROLLBACK: session_statement(t, a, "ROLLBACK", "Rollback of the session's transaction (requested from the toolbar)", FU_NONE, ""); break;
    case CF_DISCONNECT:
        if (b == 2) { Connection *c = ctx_conn(a); if (c) app_disconnect(a, c); }   /* disconnect anyway: the server decides */
        else session_statement(t, a, b == 0 ? "COMMIT" : "ROLLBACK", b == 0 ? "Commit before disconnecting" : "Rollback before disconnecting", FU_DISCONNECT, "");
        break;
    case CF_RECONNECT: { Connection *c = ctx_conn(a); if (c) app_reconnect(a, c); break; }
    case CF_RESET_TOOLBAR: {
        bool vis = t->prefs.visible; int nr = t->prefs.nrecent; char rec[TB_RECENT][16]; memcpy(rec, t->prefs.recent, sizeof rec);
        memset(&t->prefs, 0, sizeof t->prefs); t->prefs.visible = vis || true; tb_apply_preset(t, PRESET_DEFAULT);
        memcpy(t->prefs.recent, rec, sizeof rec); t->prefs.nrecent = nr;   /* recent datasources are not "toolbar layout" */
        tb_mark_dirty(t);
        LOG_I("toolbar reset to its defaults (only the toolbar: connections, queries and panel layouts untouched)");
        app_status(a, "Toolbar reset to its defaults: connections, queries and panel layouts were not touched");
        break;
    }
    case CF_WS_SWITCH: ws_switch(t, a, arg, b == 0); break;   /* Save / Discard */
    case CF_CRASH: crashrep_answer(a, b < 0 ? 2 : b); break;   /* Send report / View report / Don't send (Esc: don't send) */
    case CF_UPDATE: update_answer(a, b < 0 ? 2 : b); break;   /* Download / Release Notes / Later / Skip This Version (Esc: later) */
    case CF_CONN_FAILED: {   /* Retry / Edit connection... / Close */
        Connection *c = tb_conn_by_id(a, arg);
        if (c && b == 0) { a->pending_activate = c; app_connect_async(a, c, false); }
        else if (c && b == 1) app_edit_connection(a, c);
        break;
    }
    case CF_SSH_TRUST: {   /* Trust and Connect / Edit connection... / Cancel */
        Connection *c = tb_conn_by_id(a, arg);
        char fp[96];
        if (c && b == 0 && sshtun_unknown_key(c->error, fp, sizeof fp, NULL, 0)) {
            snprintf(c->ssh_trust, sizeof c->ssh_trust, "%s", fp);   /* this exact key, on this connect */
            a->pending_activate = c; app_connect_async(a, c, false);
        } else if (c && b == 1) app_edit_connection(a, c);
        break;
    }
    default: break;
    }
}

/* ---- workspaces (TBR-004 / 042) ----------------------------------------------------------------- */

static void ws_add_recent(App *a, const char *name) {
    char path[1100], lines[8][64]; int n = 0;
    snprintf(path, sizeof path, "%srecent_workspaces.txt", a->conns ? a->conns->dir : "");
    FILE *f = fopen(path, "r");
    if (f) { char l[64]; while (n < 8 && fgets(l, sizeof l, f)) { l[strcspn(l, "\r\n")] = 0; if (l[0] && strcmp(l, name)) snprintf(lines[n++], 64, "%s", l); } fclose(f); }
    f = fopen(path, "w");
    if (f) { fprintf(f, "%s\n", name); for (int i = 0; i < n && i < 7; i++) fprintf(f, "%s\n", lines[i]); fclose(f); }
    snprintf(path, sizeof path, "%sworkspace.txt", a->conns ? a->conns->dir : "");
    f = fopen(path, "w"); if (f) { fprintf(f, "%s\n", name); fclose(f); }
}

/* The open query tabs of the workspace: one .sql file per tab plus session.txt (title, connection
 * id, file). Connection ids only, never connection strings or passwords. */
static void ws_save_session(Toolbar *t, App *a) {
    char path[1200]; snprintf(path, sizeof path, "%ssession.txt", t->ws_dir);
    FILE *f = fopen(path, "w"); if (!f) return;
    for (int k = 0; k < panels_tab_count(a->panels); k++) {
        TabInfo ti; if (!panels_tab_info(a->panels, a, k, &ti)) continue;
        char fn[64]; snprintf(fn, sizeof fn, "tab-%d.sql", k + 1);
        char p2[1200]; snprintf(p2, sizeof p2, "%s%s", t->ws_dir, fn);
        int len = 0; char *txt = editor_text(ti.ed, &len);
        FILE *g = fopen(p2, "w"); if (g) { if (txt) fwrite(txt, 1, (size_t)len, g); fclose(g); }
        free(txt);
        fprintf(f, "%s\t%s\t%s\n", ti.title, ti.conn_id, fn);
    }
    fclose(f);
}

/* True when a tab differs from what the workspace saved (unsaved changes, TBR-004). */
static bool ws_dirty(Toolbar *t, App *a) {
    char path[1200]; snprintf(path, sizeof path, "%ssession.txt", t->ws_dir);
    FILE *f = fopen(path, "r");
    int saved = 0; bool dirty = false;
    char line[400];
    while (f && fgets(line, sizeof line, f)) {
        char *fn = strrchr(line, '\t'); if (!fn) continue; fn++; fn[strcspn(fn, "\r\n")] = 0;
        TabInfo ti; if (!panels_tab_info(a->panels, a, saved++, &ti)) { dirty = true; continue; }
        char p2[1200]; snprintf(p2, sizeof p2, "%s%s", t->ws_dir, fn);
        size_t fl = 0; char *disk = SDL_LoadFile(p2, &fl);
        int len = 0; char *txt = editor_text(ti.ed, &len);
        if (!disk || (size_t)len != fl || memcmp(disk, txt, fl)) dirty = true;
        SDL_free(disk); free(txt);
    }
    if (f) fclose(f);
    if (panels_tab_count(a->panels) != saved) {
        /* extra tabs only count when they hold text */
        for (int k = saved; k < panels_tab_count(a->panels); k++) { TabInfo ti; if (panels_tab_info(a->panels, a, k, &ti)) { int len = 0; char *txt = editor_text(ti.ed, &len); for (int i = 0; i < len; i++) if (!isspace((unsigned char)txt[i])) { dirty = true; break; } free(txt); } }
        if (!f && saved == 0 && !dirty) dirty = false;
    }
    return dirty;
}

static void ws_restore_session(Toolbar *t, App *a) {
    char path[1200]; snprintf(path, sizeof path, "%ssession.txt", t->ws_dir);
    FILE *f = fopen(path, "r");
    panels_close_all_tabs(a->panels, a);
    if (!f) return;
    char line[400]; int opened = 0, first = -1;
    while (fgets(line, sizeof line, f)) {
        char *t1 = strchr(line, '\t'); if (!t1) continue; *t1++ = 0;
        char *t2 = strchr(t1, '\t'); if (!t2) continue; *t2++ = 0; t2[strcspn(t2, "\r\n")] = 0;
        if (strchr(t2, '/') || strstr(t2, "..")) continue;   /* only files of the workspace folder */
        char p2[1200]; snprintf(p2, sizeof p2, "%s%s", t->ws_dir, t2);
        size_t fl = 0; char *txt = SDL_LoadFile(p2, &fl);
        int id = panels_open_tab(a->panels, a, line, t1, txt ? txt : "");
        if (first < 0) first = id;
        SDL_free(txt); opened++;
    }
    fclose(f);
    if (opened && first >= 0) {
        /* the empty tab panels_close_all_tabs left goes away when the workspace had tabs */
        TabInfo ti;
        for (int k = 0; k < panels_tab_count(a->panels); k++) if (panels_tab_info(a->panels, a, k, &ti) && ti.id < first) { panels_activate_tab(a->panels, a, first); panels_close_tab(a->panels, a, ti.id); break; }
        panels_activate_tab(a->panels, a, first);
    }
}

static void ws_switch(Toolbar *t, App *a, const char *name, bool save) {
    if (!strcmp(name, t->ws_name)) { app_status(a, "%s is already the current workspace", name); return; }
    if (t->dirty) tb_save_prefs(t, a);
    if (save) ws_save_session(t, a);   /* Save: the workspace being left keeps its tabs; Discard: its last saved tabs stay */
    tb_load_workspace(t, a, name);
    ws_restore_session(t, a);
    ws_add_recent(a, t->ws_name);
    LOG_I("workspace switched to \"%s\"", t->ws_name);
    app_status(a, "Workspace: %s", t->ws_name);
}

/* Switch to workspace `name`, asking first when the tabs changed since the last save (TBR-004). */
void tba_ws_request(Toolbar *t, App *a, const char *name) {
    if (ws_dirty(t, a)) { tba_confirm(t, CF_WS_SWITCH, "Unsaved changes", "Query tabs changed since the workspace was saved.", "Save and switch", "Discard and switch", "Cancel", name); return; }
    ws_switch(t, a, name, true);
}

/* ---- commands owned by the toolbar ------------------------------------------------------------- */

static void open_list(Toolbar *t, App *a, int kind);

bool tba_action(Toolbar *t, App *a, int act) {
    commands_frame(a);
    const LumeCtx *c = commands_ctx();
    switch (act) {
    /* runs: the safety rules apply to every entry point (menu, shortcut, toolbar) */
    case ACT_RUN: start_run(t, a, RUNSRC_AUTO, RUN_EDITOR, false); return true;
    case ACT_RUN_SELECTION: start_run(t, a, RUNSRC_SELECTION, RUN_EDITOR, false); return true;
    case ACT_RUN_ALL: start_run(t, a, RUNSRC_FILE, RUN_EDITOR, false); return true;
    case ACT_RUN_NEW_TAB: start_run(t, a, RUNSRC_AUTO, RUN_NEW_TAB, false); return true;
    case ACT_EXPLAIN: start_explain(t, a, false); return true;
    case ACT_EXPLAIN_ANALYZE: start_explain(t, a, true); return true;
    case ACT_RUN_PARAMS: start_params(t, a); return true;
    case ACT_RUN_TO_FILE: {
        const char *forced = SDL_getenv("LUME_RUN_TO_FILE");   /* test harness: no native dialog */
        if (forced && *forced) { toolbar_run_to_file(t, a, forced); return true; }
        SDL_DialogFileFilter flt[] = { { "CSV", "csv" }, { "JSON", "json" }, { "SQL", "sql" } };
        SDL_ShowSaveFileDialog(on_file_chosen, NULL, a->window, flt, 3, "result.csv");
        return true;
    }
    case ACT_RERUN_LAST: start_rerun(t, a); return true;
    case ACT_EXEC_SETTINGS: t->modal = TM_SETTINGS; return true;
    /* datasource / session */
    case ACT_CONNECT: { Connection *cc = ctx_conn(a); if (cc) { if (!app_connect_async(a, cc, false)) app_status(a, "%s: %s", cc->name, cc->error); } return true; }
    case ACT_DISCONNECT: {
        Connection *cc = ctx_conn(a); if (!cc) return true;
        if (c->tx_state == 1) { char body[300]; snprintf(body, sizeof body, "%s has an open transaction. Disconnecting closes the session: the server decides what happens to it (usually a rollback).", c->conn_name); tba_confirm4(t, CF_DISCONNECT, "Open transaction", body, "Commit and disconnect", "Rollback and disconnect", "Disconnect anyway", "Cancel", ""); return true; }
        app_disconnect(a, cc); return true;
    }
    case ACT_RECONNECT: {
        Connection *cc = ctx_conn(a); if (!cc) return true;
        if (c->tx_state == 1) { char body[300]; snprintf(body, sizeof body, "%s has an open transaction. Reconnecting opens a new session: the open transaction is lost (Lume does not commit it).", c->conn_name); tba_confirm(t, CF_RECONNECT, "Reconnect and lose the transaction?", body, "Reconnect", "Cancel", NULL, ""); return true; }
        app_reconnect(a, cc); return true;
    }
    case ACT_CANCEL_CONNECT: { Connection *cc = ctx_conn(a); if (cc) app_disconnect(a, cc); return true; }
    case ACT_SESSION_DETAILS: {
        Connection *cc = ctx_conn(a); if (!cc) return true;
        /* safe identifiers only: no connection string, no password (TBR-015) */
        snprintf(t->session_text, sizeof t->session_text,
                 "Datasource: %s\nDriver: %s\nState: %s\nLume session: %s#%u\nEnvironment: %s\nRead-only: %s\nDefault schema: %s\nTransaction mode: %s\nTransaction: %s\nIsolation: %s\n\n"
                 "Lume keeps one session per connection: tabs on the same connection share it (and its transaction); tabs on different connections never do. Reconnect opens a new session (an open transaction is lost, never committed).",
                 cc->name, c->caps ? c->caps->name : cc->driver, conn_state_name(c->conn_state), cc->id, cc->generation,
                 cc->env ? conn_env_name(cc->env) : "none", cc->read_only ? caps_ro_text(cc->ro_level ? cc->ro_level : RO_LUME_CHECK) : "no",
                 cc->schema[0] ? cc->schema : "server default (unknown, not assumed)", cc->manual_tx ? "manual" : "auto-commit",
                 c->tx_state == 1 ? "open (reported by the driver)" : c->tx_state == 0 ? "none" : "not reported by this driver", cc->isolation[0] ? cc->isolation : "server default");
        t->modal = TM_SESSION;
        return true;
    }
    /* transactions (TBR-016 / 026 / 027 / 028) */
    case ACT_TX_MANUAL: {
        Connection *cc = ctx_conn(a); if (!cc || !cc->db) return true;
        if (cc->manual_tx) { app_status(a, "%s is already in manual transaction mode", cc->name); return true; }
        cc->manual_tx = true;
        if (cc->db->drv->tx_begin) session_statement(t, a, cc->db->drv->tx_begin, "Manual transaction mode: BEGIN (Commit or Rollback ends it)", FU_TX_BEGIN, "");
        else if (cc->db->drv->set_autocommit) { cc->db->drv->set_autocommit(cc->db, false); app_status(a, "%s: manual transactions (the driver no longer commits each statement)", cc->name); }
        return true;
    }
    case ACT_TX_AUTO: {
        Connection *cc = ctx_conn(a); if (!cc) return true;
        if (!cc->manual_tx) { app_status(a, "%s is already in auto-commit mode", cc->name); return true; }
        if (c->tx_state == 1) { char body[300]; snprintf(body, sizeof body, "%s has an open transaction. End it before returning to auto-commit.", c->conn_name); tba_confirm(t, CF_TX_MODE_AUTO, "Open transaction", body, "Commit", "Rollback", "Cancel", ""); return true; }
        cc->manual_tx = false;
        if (cc->db && cc->db->drv->set_autocommit && !cc->db->drv->tx_begin) cc->db->drv->set_autocommit(cc->db, true);
        app_status(a, "%s: auto-commit", cc->name);
        return true;
    }
    case ACT_COMMIT: case ACT_ROLLBACK: {
        char tg[200]; target_text(a, c, tg, sizeof tg);
        char body[400]; snprintf(body, sizeof body, "%s the open transaction of this session?\nTarget: %s\nSession: %s#%u", act == ACT_COMMIT ? "Commit" : "Roll back", tg, c->conn_id, c->conn_generation);
        tba_confirm(t, act == ACT_COMMIT ? CF_COMMIT : CF_ROLLBACK, act == ACT_COMMIT ? "Commit?" : "Rollback?", body, act == ACT_COMMIT ? "Commit" : "Rollback", "Cancel", NULL, "");
        return true;
    }
    /* + SQL (TBR-003): bound to the connection in use, unless it has an open transaction (a new
     * tab never joins it implicitly) or there is none: then the datasource chooser opens */
    case ACT_NEW_EDITOR_TAB: {
        bool tx = c->tx_state == 1;
        panels_new_editor_tab(a->panels, a, NULL);
        if (tx) panels_set_tab_connection(a->panels, a, NULL);
        if (tx || !a->active) {
            tb_open_datasource(t, a);
            app_status(a, tx ? "New query tab: choose its connection (%s has an open transaction, which a new tab does not join)" : "New query tab: choose its connection", c->conn_name);
        }
        return true;
    }
    /* navigation */
    case ACT_SWITCH_TAB: open_list(t, a, LIST_SWITCH_TAB); return true;
    case ACT_QUICK_OPEN: open_list(t, a, LIST_QUICK_OPEN); return true;
    case ACT_PANEL_HISTORY: panels_show(a->panels, a, "lume.data.history"); return true;
    /* the toolbar itself */
    case ACT_TOOLBAR_TOGGLE: t->prefs.visible = !t->prefs.visible; tb_mark_dirty(t); t->kb_mode = false; app_status(a, t->prefs.visible ? "Main toolbar shown" : "Main toolbar hidden: View > Toolbar > Main Toolbar brings it back; menus and shortcuts keep working"); return true;
    case ACT_TOOLBAR_COMPACT: t->prefs.compact = !t->prefs.compact; tb_mark_dirty(t); return true;
    case ACT_TOOLBAR_LABELS: t->prefs.labels = !t->prefs.labels; tb_mark_dirty(t); return true;
    case ACT_TOOLBAR_CUSTOMIZE: t->before_customize = t->prefs; t->cust_sel = 0; t->modal = TM_CUSTOMIZE; return true;
    case ACT_TOOLBAR_RESET:
        tba_confirm(t, CF_RESET_TOOLBAR, "Reset the toolbar?", "Restores the toolbar's visibility, order, added actions, compact mode and labels to the defaults.\nNot affected: connections, queries, result tabs, panel layouts.", "Reset Toolbar", "Cancel", NULL, "");
        return true;
    case ACT_TOOLBAR_FOCUS:
        if (!t->prefs.visible) { app_status(a, "The main toolbar is hidden (View > Toolbar > Main Toolbar)"); return true; }
        t->kb_mode = true; t->focus = 0; t->focus_return = a->focus; app_status(a, "Toolbar focus: arrows / Tab move, Enter activates, Down opens, Esc returns");
        return true;
    case ACT_TOOLBAR_EXPORT: {
        char path[1200]; snprintf(path, sizeof path, "%stoolbar-export.json", t->ws_dir);
        app_status(a, toolbar_export_prefs(t, a, path) ? "Toolbar settings exported to toolbar-export.json in the workspace folder (no connection data inside)" : "Could not export the toolbar settings");
        return true;
    }
    /* workspaces */
    case ACT_WORKSPACE_NEW: textfield_set(&t->name_field, ""); t->modal = TM_NAME; return true;
    case ACT_WORKSPACE_OPEN: open_list(t, a, LIST_WORKSPACES); return true;
    case ACT_WORKSPACE_CLOSE:
        if (!strcmp(t->ws_name, "Lume Data")) { app_status(a, "The default workspace stays open"); return true; }
        if (ws_dirty(t, a)) { tba_confirm(t, CF_WS_SWITCH, "Unsaved changes", "Query tabs changed since the workspace was saved.", "Save and close", "Discard and close", "Cancel", "Lume Data"); return true; }
        ws_switch(t, a, "Lume Data", true); return true;
    case ACT_WORKSPACE_SETTINGS: {
        snprintf(t->session_text, sizeof t->session_text, "Workspace: %s\nFolder: %s\nKeeps: toolbar settings (toolbar.json), the open query tabs and their connection (session.txt + .sql files).\nNever kept: passwords, connection strings, parameter values.\nPanel layouts are global (Window > Manage Layouts).",
                 t->ws_name, strcmp(t->ws_name, "Lume Data") ? "workspaces/<name> in the preferences folder" : "the preferences folder");
        t->modal = TM_SESSION;
        return true;
    }
    default: return false;
    }
}

void tba_execution_finished(Toolbar *t, App *a, Db *db, int exec_id, int status) {
    (void)db;
    for (int i = 0; i < t->nfollow; i++) {
        if (t->follow[i].exec_id != exec_id) continue;
        int kind = t->follow[i].kind; char conn_id[16], arg[64];
        snprintf(conn_id, sizeof conn_id, "%s", t->follow[i].conn_id); snprintf(arg, sizeof arg, "%s", t->follow[i].arg);
        memmove(&t->follow[i], &t->follow[i + 1], sizeof t->follow[0] * (size_t)(t->nfollow - i - 1)); t->nfollow--;
        Connection *cc = tb_conn_by_id(a, conn_id);
        bool ok = status == ES_COMPLETED;
        if (!cc) return;
        switch (kind) {
        case FU_SCHEMA: if (ok) { snprintf(cc->schema, sizeof cc->schema, "%s", arg); app_refresh_schema(a); app_status(a, "%s: default schema %s", cc->name, arg[0] ? arg : "reset to the server's"); } else app_status(a, "Schema not changed: the server refused it (see Output)"); break;
        case FU_ISOLATION: if (ok) snprintf(cc->isolation, sizeof cc->isolation, "%s", arg); break;
        case FU_TX_MANUAL_OFF: if (ok) { cc->manual_tx = false; if (cc->db && cc->db->drv->set_autocommit && !cc->db->drv->tx_begin) cc->db->drv->set_autocommit(cc->db, true); app_status(a, "%s: auto-commit", cc->name); } break;
        case FU_TX_BEGIN: if (!ok) { cc->manual_tx = false; app_status(a, "Manual mode not set: BEGIN failed (see Output)"); } else app_status(a, "%s: manual transaction open (Commit or Rollback ends it)", cc->name); break;
        case FU_SWITCH_DS: if (ok) { Connection *to = tb_conn_by_id(a, arg); if (to) do_switch(t, a, to); } else app_status(a, "Not switched: the transaction could not be ended (see Output)"); break;
        case FU_DISCONNECT: if (ok) app_disconnect(a, cc); else app_status(a, "Not disconnected: the transaction could not be ended (see Output)"); break;
        default: break;
        }
        return;
    }
}

/* ---- dialogs ----------------------------------------------------------------------------- */

/* Rows of a list dialog (Switch Tab, Quick Open, workspaces), filtered by the search text. */
static void list_build(Toolbar *t, App *a) {
    t->nlist = 0;
    const char *q = t->list_search.buf;
    if (t->list_kind == LIST_SWITCH_TAB) {
        for (int k = 0; k < panels_tab_count(a->panels) && t->nlist < 256; k++) {
            TabInfo ti; if (!panels_tab_info(a->panels, a, k, &ti)) continue;
            Connection *cc = ti.conn_id[0] ? tb_conn_by_id(a, ti.conn_id) : NULL;
            if (q[0] && !strcasestr(ti.title, q) && !(cc && strcasestr(cc->name, q))) continue;
            snprintf(t->list_text[t->nlist], 160, "%s%s", ti.title, ti.active ? "  (current)" : "");
            snprintf(t->list_sub[t->nlist], 120, "query  \xC2\xB7  %s%s%s", cc ? cc->name : "no connection", cc && cc->env ? "  \xC2\xB7  " : "", cc && cc->env ? conn_env_name(cc->env) : "");
            t->list_arg[t->nlist++] = ti.id;
        }
        for (int i = 0; i < results_tab_count(a->results) && t->nlist < 256; i++) {
            int ex; char title[96]; bool pin, cur;
            if (!results_tab_info(a->results, i, &ex, title, sizeof title, &pin, &cur)) continue;
            if (q[0] && !strcasestr(title, q)) continue;
            snprintf(t->list_text[t->nlist], 160, "#%d %s%s", ex, title, cur ? "  (current)" : "");
            snprintf(t->list_sub[t->nlist], 120, "result%s", pin ? "  \xC2\xB7  pinned" : "");
            t->list_arg[t->nlist++] = -ex;   /* negative: a result tab */
        }
    } else if (t->list_kind == LIST_QUICK_OPEN) {
        /* objects already discovered (the loaded catalogs): never a query per key (TBR-037) */
        for (int i = 0; i < a->conns->n && t->nlist < 256; i++) {
            Connection *cc = a->conns->items[i];
            for (int k = 0; k < cc->ntables && t->nlist < 256; k++) {
                const char *nm = cc->tables[k].name;
                if (q[0] && !strcasestr(nm, q)) continue;
                const char *dot = strchr(nm, '.');
                snprintf(t->list_text[t->nlist], 160, "%s", dot ? dot + 1 : nm);
                snprintf(t->list_sub[t->nlist], 120, "%s  \xC2\xB7  %s%s%.*s", cc->tables[k].is_view ? "view" : "table", cc->name, dot ? "  \xC2\xB7  schema " : "", dot ? (int)(dot - nm) : 0, nm);
                t->list_arg[t->nlist++] = i * 100000 + k;
            }
        }
    } else {
        if (!q[0] || strcasestr("Lume Data", q)) {   /* the default one obeys the filter like the others */
            snprintf(t->list_text[t->nlist], 160, "Lume Data%s", !strcmp(t->ws_name, "Lume Data") ? "  (current)" : "");
            snprintf(t->list_sub[t->nlist], 120, "default workspace"); t->list_arg[t->nlist++] = -1;
        }
        char d[1200]; snprintf(d, sizeof d, "%sworkspaces", a->conns ? a->conns->dir : "");
        DIR *dir = opendir(d);
        struct dirent *e;
        while (dir && (e = readdir(dir)) && t->nlist < 256) {
            if (e->d_name[0] == '.') continue;
            if (q[0] && !strcasestr(e->d_name, q)) continue;
            snprintf(t->list_text[t->nlist], 160, "%s%s", e->d_name, !strcmp(t->ws_name, e->d_name) ? "  (current)" : "");
            snprintf(t->list_sub[t->nlist], 120, "workspace"); t->list_arg[t->nlist++] = t->nlist;
        }
        if (dir) closedir(dir);
    }
    if (t->list_sel >= t->nlist) t->list_sel = t->nlist ? t->nlist - 1 : 0;
}

static void open_list(Toolbar *t, App *a, int kind) {
    t->list_kind = kind; t->list_sel = 0; t->list_scroll = 0; textfield_set(&t->list_search, "");
    t->modal = TM_LIST;
    list_build(t, a);
    /* preselect the first tab that is not the current one, like Ctrl+Tab elsewhere */
    if (kind == LIST_SWITCH_TAB) for (int i = 0; i < t->nlist; i++) if (!strstr(t->list_text[i], "(current)")) { t->list_sel = i; break; }
}

static void list_choose(Toolbar *t, App *a, int i) {
    if (i < 0 || i >= t->nlist) return;
    int arg = t->list_arg[i];
    t->modal = TM_NONE;
    if (t->list_kind == LIST_SWITCH_TAB) {
        /* the editor keeps its caret, selection and connection (TBR-035) */
        if (arg >= 0) panels_activate_tab(a->panels, a, arg);
        else { results_select_exec(a->results, -arg); panels_show(a->panels, a, "lume.data.results"); }
    } else if (t->list_kind == LIST_QUICK_OPEN) {
        Connection *cc = a->conns->items[arg / 100000]; const DbTable *tb = &cc->tables[arg % 100000];
        char q[300], sql[400];
        if (cc->db) cc->db->drv->quote_ident(tb->name, q, sizeof q); else snprintf(q, sizeof q, "%s", tb->name);
        snprintf(sql, sizeof sql, "SELECT * FROM %s;\n", q);
        panels_open_tab(a->panels, a, tb->name, cc->id, sql);
        TabInfo ti; for (int k = 0; k < panels_tab_count(a->panels); k++) if (panels_tab_info(a->panels, a, k, &ti) && !strcmp(ti.title, tb->name)) panels_activate_tab(a->panels, a, ti.id);
        app_status(a, "%s opened in a query tab on %s (not run)", tb->name, cc->name);
    } else {
        char name[64]; snprintf(name, sizeof name, "%s", t->list_text[i]);
        char *p = strstr(name, "  (current)"); if (p) *p = 0;
        tba_ws_request(t, a, name);
    }
}

/* Centred dialog box of w x h points; returns it in render pixels. */
static Rect dialog_box(App *a, float w, float h) {
    float W = px(a, w), H = px(a, h);
    return rect_make((a->width - W) / 2, fmaxf(px(a, 60), (a->height - H) / 3), W, H);
}

/* Input and drawing share this function: draw = false in toolbar_frame (input only, drawn
 * again on top later), true in toolbar_draw_overlay. Widgets apply input once per frame. */
static void modal_pass(Toolbar *t, App *a, bool draw) {
    SDL_Renderer *r = a->renderer;
    Input *in = &a->in;
    if (t->modal == TM_NONE) return;
    Input saved = *in;
    if (draw) { in->mouse_pressed = false; in->mouse_rpressed = false; }
    float notes_h = 0;   /* an update offer with its notes: as tall as its text */
    if (t->modal == TM_CONFIRM && t->cf_kind == CF_UPDATE && strstr(t->cf_body, "WHAT'S NEW")) {
        float ch = 0;   /* a text line ~17 points, wrapping at ~95 characters; an empty line 10 (the drawing loop's paragraph gap) */
        for (const char *q = t->cf_body, *ls = q; ; q++) if (!*q || *q == '\n') { ch += q == ls ? 10 : 17 * (float)(1 + (q - ls) / 95); ls = q + 1; if (!*q) break; }
        notes_h = fminf(fminf(32 + ch + 80, 560), a->height / a->scale - 120);   /* title above, buttons below */
    }
    Rect box = t->modal == TM_CONFIRM ? (notes_h > 0 ? dialog_box(a, 600, fmaxf(notes_h, 230)) : dialog_box(a, 520, 230)) : t->modal == TM_PARAMS ? dialog_box(a, 480, 120 + 40 * (float)t->nparams) :
               t->modal == TM_SETTINGS ? dialog_box(a, 520, 250) : t->modal == TM_CUSTOMIZE ? dialog_box(a, 620, fminf(180 + 28 * (float)t->nitems, a->height / a->scale - 80)) :
               t->modal == TM_SESSION ? dialog_box(a, 560, 330) : t->modal == TM_NAME ? dialog_box(a, 420, 150) : dialog_box(a, 560, 440);
    t->modal_rect = box;
    if (draw) {
        draw_rect(r, rect_make(0, 0, (float)a->width, (float)a->height), color_alpha(color_hex(0x000000), 110));
        draw_rrect(r, rect_inset(box, -px(a, 1), -px(a, 1)), px(a, 9), color_hex(T_BORDER));
        draw_rrect(r, box, px(a, 8), color_hex(T_PANEL_ALT));
    }
    float x = box.x + px(a, 20), y = box.y + px(a, 18), w = box.w - px(a, 40);
    bool esc = !draw && input_key(in, SDLK_ESCAPE, 0);
    bool enter = !draw && (input_key(in, SDLK_RETURN, 0) || input_key(in, SDLK_KP_ENTER, 0));
    switch (t->modal) {
    case TM_CONFIRM: {
        if (draw) {
            draw_text_fit(a->font_ui, r, t->cf_title, -1, rect_make(x, y, w, px(a, 22)), color_hex(T_TEXT), ALIGN_LEFT);
            float ly = y + px(a, 32); const char *p = t->cf_body;
            /* each paragraph wraps (a driver message is often longer than the box is wide) */
            while (*p && ly < box.y + box.h - px(a, 60)) {
                const char *e = strchr(p, '\n'); int l = e ? (int)(e - p) : (int)strlen(p);
                char seg[800]; snprintf(seg, sizeof seg, "%.*s", l < 790 ? l : 790, p);
                ly += (l ? results_draw_wrapped(a->font_small, r, seg, rect_make(x, ly, w, box.y + box.h - px(a, 60) - ly), color_hex(T_TEXT_MUTED), 0, true) : px(a, 8)) + px(a, 2);
                p = e ? e + 1 : p + l;
            }
        }
        float bw = px(a, t->cf_nbtn > 3 ? 120 : 160), bx = box.x + box.w - px(a, 20);
        int choice = -1;
        for (int b = t->cf_nbtn - 1; b >= 0; b--) {
            bx -= bw;
            bool primary = b == 0 && strcmp(t->cf_btn[b], "Cancel");
            if (ui_button(a, rect_make(bx, box.y + box.h - px(a, 46), bw - px(a, 8), px(a, 30)), t->cf_btn[b], primary ? (strstr(t->cf_btn[b], "PROD") || strstr(t->cf_btn[b], "Rollback") ? BTN_DANGER : BTN_PRIMARY) : BTN_NORMAL, true) && !draw) choice = b;
        }
        if (!draw) {
            if (esc) { int cancel = -1; for (int b = 0; b < t->cf_nbtn; b++) if (!strcmp(t->cf_btn[b], "Cancel")) cancel = b; confirm_answer(t, a, cancel >= 0 ? cancel : -1); }
            else if (choice >= 0) confirm_answer(t, a, choice);
            /* Enter never confirms a PROD write by accident: only the explicit button */
        }
        break;
    }
    case TM_PARAMS: {
        if (draw) draw_text_fit(a->font_ui, r, "Run with Parameters", -1, rect_make(x, y, w, px(a, 22)), color_hex(T_TEXT), ALIGN_LEFT);
        if (draw) draw_text_fit(a->font_small, r, "Values are sent as literals; they are not shown in the Output nor kept in the history. NULL = SQL NULL.", -1, rect_make(x, y + px(a, 22), w, px(a, 18)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        float fy = y + px(a, 48);
        for (int k = 0; k < t->nparams; k++, fy += px(a, 40)) {
            if (draw) draw_text_fit(a->font_ui, r, t->params[k].name, -1, rect_make(x, fy, px(a, 120), px(a, 30)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
            if (ui_textfield(a, &t->params[k].f, rect_make(x + px(a, 130), fy, w - px(a, 130), px(a, 30)), "value", t->param_focus == k) && !draw) t->param_focus = k;
        }
        if (!draw) {
            for (int k = 0; k < in->nkeys; k++) if (!in->keys[k].used && in->keys[k].key == SDLK_TAB) { in->keys[k].used = true; t->param_focus = (t->param_focus + ((in->keys[k].mods & KM_SHIFT) ? t->nparams - 1 : 1)) % t->nparams; }
        }
        bool run = ui_button(a, rect_make(box.x + box.w - px(a, 140), box.y + box.h - px(a, 46), px(a, 120), px(a, 30)), "Run", BTN_PRIMARY, true);
        bool cancel = ui_button(a, rect_make(box.x + box.w - px(a, 270), box.y + box.h - px(a, 46), px(a, 120), px(a, 30)), "Cancel", BTN_NORMAL, true);
        if (!draw) {
            if (esc || cancel) { for (int k = 0; k < t->nparams; k++) textfield_set(&t->params[k].f, ""); tba_run_free(&t->run); t->modal = TM_NONE; }
            else if (run || enter) params_run(t, a);
        }
        break;
    }
    case TM_SETTINGS: {
        if (draw) draw_text_fit(a->font_ui, r, "Query Execution Settings", -1, rect_make(x, y, w, px(a, 22)), color_hex(T_TEXT), ALIGN_LEFT);
        static const char *const fetch[] = { "100", "500", "1,000", "5,000" };
        static const int fetch_v[] = { 100, 500, 1000, 5000 }, fetch_a[] = { ACT_FETCH_100, ACT_FETCH_500, ACT_FETCH_1000, ACT_FETCH_5000 };
        int fs = 1; for (int i = 0; i < 4; i++) if (a->fetch_size == fetch_v[i]) fs = i;
        if (draw) draw_text_fit(a->font_small, r, "Rows fetched per batch", -1, rect_make(x, y + px(a, 34), w, px(a, 18)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        int pick = ui_segmented(a, rect_make(x, y + px(a, 54), px(a, 320), px(a, 28)), fetch, 4, fs);
        if (!draw && pick >= 0) commands_dispatch(a, fetch_a[pick]);
        if (ui_checkbox(a, rect_make(x, y + px(a, 100), w, px(a, 24)), "Continue on error in scripts (otherwise stop at the first failure)", a->continue_on_error, true) && !draw) commands_dispatch(a, ACT_CONTINUE_ON_ERROR);
        if (draw) draw_text_fit(a->font_small, r, "Applied now, to the next executions of every session.", -1, rect_make(x, y + px(a, 136), w, px(a, 18)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        bool close = ui_button(a, rect_make(box.x + box.w - px(a, 140), box.y + box.h - px(a, 46), px(a, 120), px(a, 30)), "Done", BTN_PRIMARY, true);
        if (!draw && (close || esc || enter)) t->modal = TM_NONE;
        break;
    }
    case TM_SESSION: {
        if (draw) {
            float ly = y; const char *p = t->session_text;
            while (*p) { const char *e = strchr(p, '\n'); int l = e ? (int)(e - p) : (int)strlen(p); draw_text_fit(ly == y ? a->font_ui : a->font_small, r, p, l, rect_make(x, ly, w, px(a, 20)), color_hex(ly == y ? T_TEXT : T_TEXT_MUTED), ALIGN_LEFT); ly += px(a, 20); p = e ? e + 1 : p + l; }
        }
        bool close = ui_button(a, rect_make(box.x + box.w - px(a, 140), box.y + box.h - px(a, 46), px(a, 120), px(a, 30)), "Close", BTN_PRIMARY, true);
        if (!draw && (close || esc || enter)) t->modal = TM_NONE;
        break;
    }
    case TM_NAME: {
        if (draw) draw_text_fit(a->font_ui, r, "New Workspace", -1, rect_make(x, y, w, px(a, 22)), color_hex(T_TEXT), ALIGN_LEFT);
        ui_textfield(a, &t->name_field, rect_make(x, y + px(a, 36), w, px(a, 30)), "Name (letters, digits, spaces, - _)", true);
        bool ok = ui_button(a, rect_make(box.x + box.w - px(a, 140), box.y + box.h - px(a, 46), px(a, 120), px(a, 30)), "Create", BTN_PRIMARY, t->name_field.len > 0);
        bool cancel = ui_button(a, rect_make(box.x + box.w - px(a, 270), box.y + box.h - px(a, 46), px(a, 120), px(a, 30)), "Cancel", BTN_NORMAL, true);
        if (!draw) {
            if (esc || cancel) t->modal = TM_NONE;
            else if ((ok || enter) && t->name_field.len) {
                char nm[48]; int w2 = 0;
                for (const char *p = t->name_field.buf; *p && w2 < 46; p++) if (isalnum((unsigned char)*p) || *p == ' ' || *p == '-' || *p == '_') nm[w2++] = *p;
                nm[w2] = 0;
                t->modal = TM_NONE;
                if (!w2 || !strcmp(nm, "Lume Data")) app_status(a, "Choose another name");
                else ws_switch(t, a, nm, true);
            }
        }
        break;
    }
    case TM_LIST: {
        const char *titles[] = { "Switch Tab", "Quick Open", "Open Workspace" };
        if (draw) draw_text_fit(a->font_ui, r, titles[t->list_kind], -1, rect_make(x, y, w, px(a, 22)), color_hex(T_TEXT), ALIGN_LEFT);
        char before[256]; snprintf(before, sizeof before, "%s", t->list_search.buf);
        ui_textfield(a, &t->list_search, rect_make(x, y + px(a, 30), w, px(a, 30)), t->list_kind == LIST_QUICK_OPEN ? "Table or view name (loaded catalogs only)" : "Filter", true);
        if (!draw && strcmp(before, t->list_search.buf)) { t->list_sel = 0; t->list_scroll = 0; list_build(t, a); }
        float ly = y + px(a, 70), rh = px(a, 34);
        int visible = (int)((box.y + box.h - px(a, 16) - ly) / rh);
        if (t->list_sel < t->list_scroll) t->list_scroll = t->list_sel;
        if (t->list_sel >= t->list_scroll + visible) t->list_scroll = t->list_sel - visible + 1;
        for (int i = t->list_scroll; i < t->nlist && i < t->list_scroll + visible; i++, ly += rh) {
            Rect rr = rect_make(x - px(a, 6), ly, w + px(a, 12), rh - px(a, 2));
            char tg[200]; snprintf(tg, sizeof tg, "list:%s", t->list_text[i]); ui_record(tg, rr);
            if (draw) {
                if (i == t->list_sel) draw_rrect(r, rr, px(a, 4), color_hex(T_SELECTED));
                draw_text_fit(a->font_ui, r, t->list_text[i], -1, rect_make(x, ly, w, px(a, 18)), color_hex(T_TEXT), ALIGN_LEFT);
                draw_text_fit(a->font_small, r, t->list_sub[i], -1, rect_make(x, ly + px(a, 16), w, px(a, 16)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
            } else if (rect_contains(rr, in->mouse_x, in->mouse_y) && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; list_choose(t, a, i); return; }
        }
        if (draw && !t->nlist) draw_text_fit(a->font_small, r, "Nothing matches", -1, rect_make(x, ly, w, px(a, 18)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        if (!draw) {
            for (int k = 0; k < in->nkeys; k++) {
                KeyPress *kp = &in->keys[k]; if (kp->used) continue;
                if (kp->key == SDLK_DOWN || (kp->key == SDLK_TAB && !(kp->mods & KM_SHIFT))) { t->list_sel = t->nlist ? (t->list_sel + 1) % t->nlist : 0; kp->used = true; }
                else if (kp->key == SDLK_UP || (kp->key == SDLK_TAB && (kp->mods & KM_SHIFT))) { t->list_sel = t->nlist ? (t->list_sel - 1 + t->nlist) % t->nlist : 0; kp->used = true; }
            }
            if (esc) t->modal = TM_NONE;
            else if (enter) list_choose(t, a, t->list_sel);
        }
        break;
    }
    case TM_CUSTOMIZE: {
        if (draw) draw_text_fit(a->font_ui, r, "Customize Toolbar", -1, rect_make(x, y, w, px(a, 22)), color_hex(T_TEXT), ALIGN_LEFT);
        /* preset (TBR-041); the bar behind updates as a preview; Cancel restores (TBR-039) */
        static const char *const pn[] = { "Minimal", "Default", "Advanced" };
        int pick = ui_segmented(a, rect_make(x, y + px(a, 30), px(a, 330), px(a, 28)), pn, 3, t->prefs.preset < PRESET_CUSTOM ? t->prefs.preset : -1);
        if (!draw && pick >= 0) tb_apply_preset(t, pick);
        if (draw && t->prefs.preset == PRESET_CUSTOM) draw_text_fit(a->font_small, r, "Custom", -1, rect_make(x + px(a, 340), y + px(a, 30), px(a, 80), px(a, 28)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        float ly = y + px(a, 70), rh = px(a, 28);
        for (int i = 0; i < t->nitems; i++, ly += rh) {
            TbItem *it = &t->items[i];
            if (it->kind == IK_FLEX) { ly -= rh; continue; }
            bool vis = !tb_item_hidden(t, it->id);
            char lbl[120]; snprintf(lbl, sizeof lbl, "%s%s", it->custom ? it->label : it->label, it->required ? "  (required)" : it->custom ? "  (added)" : "");
            char tg[80]; snprintf(tg, sizeof tg, "cust:%s", it->id);
            Rect cb = rect_make(x, ly, w - px(a, 200), rh - px(a, 2));
            ui_record(tg, cb);
            if (ui_checkbox(a, cb, lbl, vis || it->required, !it->required) && !draw) { tb_set_hidden(t, it->id, vis); t->prefs.preset = PRESET_CUSTOM; tb_rebuild_items(t); }
            /* move within its group; removing an added action */
            bool up_ok = i > 0 && t->items[i - 1].group == it->group, down_ok = i + 1 < t->nitems && t->items[i + 1].group == it->group;
            char up_tg[60], dn_tg[60]; snprintf(up_tg, sizeof up_tg, "Up %s", it->id); snprintf(dn_tg, sizeof dn_tg, "Down %s", it->id);
            Rect ub = rect_make(x + w - px(a, 190), ly, px(a, 56), rh - px(a, 4)), db2 = rect_make(x + w - px(a, 128), ly, px(a, 64), rh - px(a, 4));
            ui_record(up_tg, ub); ui_record(dn_tg, db2);
            bool up = ui_button(a, ub, "Up", BTN_QUIET, up_ok), down = ui_button(a, db2, "Down", BTN_QUIET, down_ok);
            bool rm = it->custom && ui_button(a, rect_make(x + w - px(a, 60), ly, px(a, 60), rh - px(a, 4)), "Remove", BTN_QUIET, true);
            if (!draw && (up || down)) {
                int j = up ? i - 1 : i + 1;
                char tmp[40]; int oi = -1, oj = -1;
                for (int k = 0; k < t->prefs.norder; k++) { if (!strcmp(t->prefs.order[k], it->id)) oi = k; if (!strcmp(t->prefs.order[k], t->items[j].id)) oj = k; }
                if (oi >= 0 && oj >= 0) { snprintf(tmp, sizeof tmp, "%s", t->prefs.order[oi]); snprintf(t->prefs.order[oi], 40, "%s", t->prefs.order[oj]); snprintf(t->prefs.order[oj], 40, "%s", tmp); }
                t->prefs.preset = PRESET_CUSTOM; tb_rebuild_items(t); break;
            }
            if (!draw && rm) {
                for (int k = 0; k < t->prefs.ncustom; k++) if (!strcmp(t->prefs.custom[k], it->id + 4)) { memmove(&t->prefs.custom[k], &t->prefs.custom[k + 1], 40 * (size_t)(t->prefs.ncustom - k - 1)); t->prefs.ncustom--; }
                for (int k = 0; k < t->prefs.norder; k++) if (!strcmp(t->prefs.order[k], it->id)) { memmove(&t->prefs.order[k], &t->prefs.order[k + 1], 40 * (size_t)(t->prefs.norder - k - 1)); t->prefs.norder--; }
                t->prefs.preset = PRESET_CUSTOM; tb_rebuild_items(t); break;
            }
        }
        /* Add Action: the next registry command not on the bar yet (cycled with the button) */
        static int add_idx;
        const Command *cand = NULL;
        for (int k = 0, tries = 0; tries < commands_count(); tries++, k++) {
            const Command *cm = commands_at((add_idx + k) % commands_count());
            if (!cm || !strncmp(cm->id, "toolbar.", 8)) continue;
            bool on = false; for (int i = 0; i < t->nitems; i++) if (t->items[i].cmd && !strcmp(t->items[i].cmd, cm->id)) on = true;
            if (!on) { cand = cm; add_idx = (add_idx + k) % commands_count(); break; }
        }
        float by = box.y + box.h - px(a, 86);
        char addl[120]; snprintf(addl, sizeof addl, "Add action: %s", cand ? cand->label : "(all added)");
        if (draw) draw_text_fit(a->font_small, r, addl, -1, rect_make(x, by, w - px(a, 220), px(a, 28)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        bool next = ui_button(a, rect_make(x + w - px(a, 210), by, px(a, 100), px(a, 28)), "Next", BTN_NORMAL, cand != NULL);
        bool add = ui_button(a, rect_make(x + w - px(a, 100), by, px(a, 100), px(a, 28)), "Add", BTN_NORMAL, cand != NULL && t->prefs.ncustom < TB_MAX_CUSTOM);
        if (!draw && next) add_idx = (add_idx + 1) % commands_count();
        if (!draw && add && cand) {
            snprintf(t->prefs.custom[t->prefs.ncustom++], 40, "%s", cand->id);
            int at = t->prefs.norder; for (int k = 0; k < t->prefs.norder; k++) if (!strcmp(t->prefs.order[k], "history")) at = k + 1;
            memmove(&t->prefs.order[at + 1], &t->prefs.order[at], 40 * (size_t)(t->prefs.norder - at)); snprintf(t->prefs.order[at], 40, "cmd:%s", cand->id); t->prefs.norder++;
            t->prefs.preset = PRESET_CUSTOM; tb_rebuild_items(t);
        }
        bool ok = ui_button(a, rect_make(box.x + box.w - px(a, 140), box.y + box.h - px(a, 46), px(a, 120), px(a, 30)), "Save", BTN_PRIMARY, true);
        bool cancel = ui_button(a, rect_make(box.x + box.w - px(a, 270), box.y + box.h - px(a, 46), px(a, 120), px(a, 30)), "Cancel", BTN_NORMAL, true);
        if (!draw) {
            if (esc || cancel) { t->prefs = t->before_customize; tb_rebuild_items(t); t->modal = TM_NONE; app_status(a, "Toolbar not changed"); }
            else if (ok) { t->modal = TM_NONE; tb_mark_dirty(t); tb_save_prefs(t, a); app_status(a, "Toolbar saved (%s)", tb_preset_name(t->prefs.preset)); }
        }
        break;
    }
    default: break;
    }
    if (!draw) {
        /* the dialog owns the input */
        in->nkeys = 0; in->ntext = 0; in->mouse_consumed = true; in->wheel_y = 0; in->mouse_rpressed = false;
    } else *in = saved;
}

void tba_modal_input(Toolbar *t, App *a) {
    if (SDL_GetAtomicInt(&file_ready)) { SDL_SetAtomicInt(&file_ready, 0); toolbar_run_to_file(t, a, file_path); }
    modal_pass(t, a, false);
}
void tba_modal_draw(Toolbar *t, App *a) { modal_pass(t, a, true); }

bool toolbar_flag(Toolbar *t, int which) { return t && (which == 0 ? t->prefs.visible : which == 1 ? t->prefs.compact : t->prefs.labels); }

const char *toolbar_workspace_dir(Toolbar *t) { return t ? t->ws_dir : ""; }

/* A connect the user asked for failed: said in the middle of the screen, not only in the status bar */
bool toolbar_update_prompt(Toolbar *t, App *a, const char *title, const char *body, int notice) {
    (void)a;
    if (!t || t->modal) return false;   /* never over another question */
    if (notice) tba_confirm(t, CF_UPDATE, title, body, "OK", NULL, NULL, "");
    else tba_confirm4(t, CF_UPDATE, title, body, "Download", "Release Notes", "Later", "Skip This Version", "");
    return true;
}
void toolbar_crash_prompt(Toolbar *t, App *a, const char *body) {
    (void)a;
    if (!t) return;
    tba_confirm(t, CF_CRASH, "Lume Data closed unexpectedly", body, "Send report", "View report", "Don't send", "");
}
void toolbar_connect_failed(Toolbar *t, App *a, Connection *c) {
    (void)a;
    if (!t || !c || t->modal) return;   /* never over another question */
    char title[160], body[700];
    char fp[96], type[64];
    if (c->ssh && sshtun_unknown_key(c->error, fp, sizeof fp, type, sizeof type)) {   /* never accepted silently: the user compares */
        snprintf(title, sizeof title, "Trust the SSH host %s?", c->ssh_host);
        snprintf(body, sizeof body, "Lume has not connected to this SSH host before. Its host key fingerprint is:\n\n%s %s\n\n"
                 "Compare it with the fingerprint your server administrator gives you, and trust it only if they match. "
                 "Lume then adds it to your known_hosts; if the server shows a different key later, the connection is refused.", type, fp);
        tba_confirm(t, CF_SSH_TRUST, title, body, "Trust and Connect", "Edit connection...", "Cancel", c->id);
        return;
    }
    snprintf(title, sizeof title, "Could not connect to %s", c->name);
    /* the driver's message, tidied: its line breaks and indentation become single spaces */
    char err[500]; size_t k = 0; bool sp = false;
    for (const char *p = c->error[0] ? c->error : "The connection failed."; *p && k + 1 < sizeof err; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t' || *p == ' ') { sp = k > 0; continue; }
        if (sp) { err[k++] = ' '; sp = false; if (k + 1 >= sizeof err) break; }
        err[k++] = *p;
    }
    err[k] = 0;
    /* the hint fits the kind of database: a file, or a server */
    const DriverInfo *di = conn_driver(c); bool file = false;
    for (int i = 0; di && i < di->nfields; i++) if (di->fields[i].type == FIELD_FILE) file = true;
    snprintf(body, sizeof body, "%s\n\n%s", err, file ? "Check that the file exists, that Lume can read it, and the path in the connection."
                                                       : "Check that the server is running and reachable, and the connection's host, port and credentials.");
    tba_confirm(t, CF_CONN_FAILED, title, body, "Retry", "Edit connection...", "Close", c->id);
}
