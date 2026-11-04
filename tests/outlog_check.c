/* SPDX-License-Identifier: AGPL-3.0-only */
/* Headless checks of the execution-output model (OUT-006, 010, 011, 012, 015, 019, 020,
 * 021, 022, 024, 027, 028): ordering, dedup, state machine, retention, search, filters,
 * redaction, export, and 100k events timing. */
#include "outlog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int failures = 0;
static void check(bool ok, const char *what, const char *detail) {
    printf("%s  %s%s%.160s\n", ok ? "ok  " : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    if (!ok) failures++;
}
static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }
static char *slurp(const char *path) { FILE *f = fopen(path, "rb"); if (!f) return NULL; char *s = calloc(1, 1 << 20); size_t n = fread(s, 1, (1 << 20) - 1, f); s[n] = 0; fclose(f); return s; }

int main(void) {
    OutLog l; outlog_init(&l, 7, "Demo");
    /* a script: two statements, the second fails */
    const char *sql = "UPDATE t SET a = 1 WHERE id = 3;\nSELECT * FROM nope;";
    int x = outlog_exec_begin(&l, sql, -1, 12, 100);
    OutExec *ex = outlog_exec(&l, x);
    check(x == 1 && ex && ex->status == ES_RUNNING && l.n == 1 && l.ev[0].type == OT_EXEC_STARTED && strstr(l.ev[0].message, "Execution #1 started") && strstr(ex->summary, "UPDATE t SET a"), "execution_started recorded; the execution keeps a one-line summary", l.ev[0].message);
    check(l.ev[0].sql && l.ev[0].sql_start == 100 && l.ev[0].sql_end == 100 + (int)strlen(sql) && l.ev[0].editor_rev == 12, "sql keeps editor revision and offsets of the executed text", NULL);
    outlog_push_sql(&l, x, 1, OT_STMT_STARTED, OL_INFO, "Started: UPDATE t SET a = 1 WHERE id = 3", "UPDATE t SET a = 1 WHERE id = 3", -1, 100, 12);
    OutEvent *e = outlog_push(&l, x, 1, OT_ROWS_AFFECTED, OL_SUCCESS, "Completed: 1 row affected \xC2\xB7 autocommit");
    e->rows_affected = 1; e->exec_ms = 3.8; e->autocommit = 1; ex->nstmts++; ex->ok++;
    e = outlog_push(&l, x, 2, OT_STMT_FAILED, OL_ERROR, "relation \"nope\" does not exist");
    e->err_state = strdup("42P01"); e->err_line = 2; e->err_col = 15; ex->nstmts++; ex->failed++;
    outlog_exec_finish(&l, x, ES_FAILED, 12.5);
    check(ex->status == ES_FAILED && l.n == 5 && l.ev[4].type == OT_EXEC_FINISHED && strstr(l.ev[4].message, "1 of 2 statements ok \xC2\xB7 1 failed \xC2\xB7 0 not run"), "execution_finished summarises statements by state", l.ev[4].message);
    outlog_exec_status(&l, x, ES_RUNNING);
    check(ex->status == ES_FAILED, "a late event never flips a terminal state back to running", NULL);
    check(ex->errors == 1 && ex->warnings == 0, "per-execution error / warning counters", NULL);
    char *txt = outlog_event_text(&l, &l.ev[3], true);
    check(strstr(txt, "sqlstate: 42P01") && strstr(txt, "position: line 2, column 15") && strstr(txt, "error") , "details text carries sqlstate and position", txt);
    free(txt);
    txt = outlog_event_text(&l, &l.ev[2], true);
    check(strstr(txt, "rows affected: 1") && strstr(txt, "execution: 3.8ms (client-measured)") && !strstr(txt, "rows returned"), "metrics listed only when measured; scope labelled", txt);
    free(txt);

    /* ordering and dedup (OUT-027) */
    OutLog t; outlog_init(&t, 1, "transport");
    OutEvent a1 = { .id = 10, .exec_id = 5, .seq = 1, .ts = 1.0, .level = OL_INFO, .type = OT_EXEC_STARTED, .message = "start", .exec_ms = -1, .fetch_ms = -1, .rows_affected = -1, .rows_returned = -1, .rows_loaded = -1, .err_line = -1, .err_col = -1, .err_code = -1, .autocommit = -1 };
    OutEvent a3 = a1; a3.id = 12; a3.seq = 3; a3.message = "third"; a3.type = OT_EXEC_FINISHED;
    OutEvent a2 = a1; a2.id = 11; a2.seq = 2; a2.message = "second"; a2.type = OT_STMT_COMPLETED;
    outlog_ingest(&t, &a1); outlog_ingest(&t, &a3); outlog_ingest(&t, &a2);
    bool dup = outlog_ingest(&t, &a2);
    check(t.n == 3 && strcmp(t.ev[1].message, "second") == 0 && strcmp(t.ev[2].message, "third") == 0 && !dup && t.dedup_dropped == 1 && t.out_of_order == 1, "late event inserted by sequence; duplicate id dropped", NULL);
    outlog_free(&t);

    /* search and filters (OUT-019 / OUT-020) */
    int hits[16];
    int nh = outlog_search(&l, "nope", hits, 16);
    check(nh == 2 && hits[0] == 0 && hits[1] == 3, "search over message and sql, case-insensitive", NULL);
    OutFilter f = { 0, 0, NULL, true };
    int hidden = 0, vis = outlog_count(&l, &f, &hidden);
    check(vis == 2 && hidden == 3, "errors/warnings-only filter reports how many are hidden", NULL);
    OutFilter f2 = { 1u << OL_SUCCESS, x, NULL, false };
    check(outlog_count(&l, &f2, NULL) == 1, "level mask combined with execution filter", NULL);

    /* redaction (OUT-028) */
    char red[400];
    outlog_redact("CREATE USER bob IDENTIFIED BY \"s3cret!\"; ALTER USER x PASSWORD 'pw'; conn: host=a password=abc user=u", red, sizeof red);
    check(!strstr(red, "s3cret") && !strstr(red, "'pw'") && !strstr(red, "password=abc") && strstr(red, "IDENTIFIED BY \"***\"") && strstr(red, "password=***") && strstr(red, "user=u"), "password-like literals and key=value secrets are masked, the rest kept", red);
    /* quoted keyword: the literal after the operator is the secret (was left in clear) */
    outlog_redact("SELECT 'x' WHERE 'password' = 'hunter2' AND \"secret\"='s2' AND password_hint = 'kept'", red, sizeof red);
    check(!strstr(red, "hunter2") && !strstr(red, "s2'") && strstr(red, "'password' = '***'") && strstr(red, "password_hint = 'kept'"), "quoted keyword: the value after the operator is masked; password_hint is not a secret", red);
    outlog_redact("SELECT token_count FROM t WHERE apikey = 'k1'", red, sizeof red);
    check(strstr(red, "token_count") && strstr(red, "apikey = '***'"), "identifiers containing a keyword stay; a bare keyword's value is masked", red);
    e = outlog_push(&l, x, 2, OT_DB_MESSAGE, OL_WARNING, "NOTICE: password 'leak' would be printed");
    txt = outlog_event_text(&l, e, false);
    check(!strstr(txt, "leak"), "copied text is redacted too", txt);
    free(txt);

    /* export (OUT-022) */
    char err[200];
    bool ok = outlog_export(&l, NULL, "/tmp/lume_out.json", true, err, sizeof err);
    char *js = slurp("/tmp/lume_out.json");
    check(ok && js && strstr(js, "\"schemaVersion\": 1") && strstr(js, "\"sqlState\": \"42P01\"") && strstr(js, "\"rowsAffected\": 1") && strstr(js, "\"historyComplete\": true") && !strstr(js, "leak"), "JSON export: schema version, error fields, metrics, redaction", ok ? "" : err);
    free(js); remove("/tmp/lume_out.json");
    ok = outlog_export(&l, &f, "/tmp/lume_out.txt", false, err, sizeof err);
    js = slurp("/tmp/lume_out.txt");
    check(ok && js && strstr(js, "filter active, some hidden") && strstr(js, "42P01"), "TXT export states that a filter hides events", ok ? "" : err);
    free(js); remove("/tmp/lume_out.txt");
    outlog_free(&l);

    /* retention and volume (OUT-024): 100k events, whole executions dropped oldest first */
    {   /* retention compacts the array while a push is stored: the pointer returned must be that event */
        OutLog rl; outlog_init(&rl, 3, "retain"); rl.max_events = 6;
        int x1 = outlog_exec_begin(&rl, "SELECT 1", -1, 1, 0);
        for (int i = 0; i < 3; i++) outlog_push(&rl, x1, 1, OT_DB_MESSAGE, OL_INFO, "old");
        outlog_exec_finish(&rl, x1, ES_COMPLETED, 1.0);
        int x2 = outlog_exec_begin(&rl, "SELECT 2", -1, 1, 0);
        OutEvent *e = outlog_push(&rl, x2, 1, OT_DB_MESSAGE, OL_INFO, "fresh");
        bool dropped = outlog_exec(&rl, x1) == NULL;
        check(dropped && e && e->exec_id == x2 && strcmp(e->message, "fresh") == 0,
              "the event returned by a push that triggered retention is the pushed one", e ? e->message : "NULL");
        outlog_free(&rl);
    }
    {   /* "Clear output" while a statement runs: the running execution keeps its record */
        OutLog cl; outlog_init(&cl, 4, "clear");
        int xd = outlog_exec_begin(&cl, "SELECT 1", -1, 1, 0); outlog_exec_finish(&cl, xd, ES_COMPLETED, 1.0);
        int xr = outlog_exec_begin(&cl, "SELECT pg_sleep(10)", -1, 1, 0);
        outlog_clear(&cl);
        OutEvent *e = outlog_push(&cl, xr, 1, OT_STMT_COMPLETED, OL_SUCCESS, "1 row loaded");
        check(cl.n == 1 && e != NULL && outlog_exec(&cl, xr) != NULL && outlog_exec(&cl, xd) == NULL,
              "clearing keeps running executions (their next event is stored and returned) and drops finished ones", NULL);
        outlog_free(&cl);
    }
    OutLog big; outlog_init(&big, 2, "big"); big.max_events = 50000;
    double t0 = now_ms();
    for (int i = 0; i < 10000; i++) {
        int xi = outlog_exec_begin(&big, "SELECT 1", -1, 1, 0);
        for (int k = 0; k < 9; k++) outlog_push(&big, xi, 1, OT_DB_MESSAGE, k == 8 ? OL_WARNING : OL_INFO, "message with some text in it");
        outlog_exec_finish(&big, xi, ES_COMPLETED, 1.0);
    }
    double dt = now_ms() - t0;
    char d[120]; snprintf(d, sizeof d, "%.0f ms for 110k events, %d kept, %ld dropped, %ld executions dropped", dt, big.n, big.dropped_events, big.dropped_execs);
    check(big.n <= 50000 + 11 && big.dropped_events > 50000 && big.dropped_execs > 0 && dt < (getenv("CI") ? 6000 : 2000),   /* shared CI machines are slower; the 2 s bar holds on a real Mac */ "100k+ events ingested quickly; retention keeps the newest whole executions", d);
    t0 = now_ms(); nh = outlog_search(&big, "text in", hits, 16); dt = now_ms() - t0;
    snprintf(d, sizeof d, "%.1f ms", dt);
    check(nh == 16 && dt < 200, "search over 50k events is fast", d);
    int xs, er, wa; outlog_counts(&big, &xs, &er, &wa);
    check(xs == big.nex && wa == big.nex, "counters reflect retained executions only", NULL);
    outlog_free(&big);

    /* notes: Lume's status messages kept as a record (outside executions) */
    OutLog nl; outlog_init(&nl, 9, "Notes");
    int n1 = outlog_note(&nl, OL_ERROR, "db1: connection refused");
    int xr = outlog_exec_begin(&nl, "SELECT 1", -1, 0, 0);
    int n2 = outlog_note(&nl, OL_INFO, "Copied 3 rows");
    int n2b = outlog_note(&nl, OL_INFO, "Copied 3 rows");
    outlog_exec_finish(&nl, xr, ES_COMPLETED, 1.0);
    int n3 = outlog_note(&nl, OL_INFO, "Copied 3 rows");
    int n4 = outlog_note(&nl, OL_INFO, "Copied 3 rows");
    int n4b = outlog_note(&nl, OL_INFO, "Copied 3 rows");
    OutExec *nx1 = outlog_exec(&nl, n1), *nx2 = outlog_exec(&nl, n2), *nx3 = outlog_exec(&nl, n3);
    check(n1 == -1 && n2 == -2 && xr == 1 && nx1 && nx1->note && nx1->status == ES_COMPLETED, "notes take negative ids: execution numbers have no gaps", NULL);
    check(n2b == n2 && nx2 && nx2->repeat == 2, "the same message right after a note bumps it (a note inside a running execution too)", NULL);
    check(n3 == -3 && n4 == n3 && n4b == n3 && nx3 && nx3->repeat == 3, "after other events the same message is a new note (the list stays in time order)", NULL);
    const OutEvent *ne = NULL; for (int i = 0; i < nl.n; i++) if (nl.ev[i].exec_id == n3) ne = &nl.ev[i];
    check(ne && ne->type == OT_NOTE && strstr(ne->message, "Copied 3 rows (\xC3\x97" "3)") && !strcmp(outlog_type_name(OT_NOTE), "lume_message"), "a repeated note shows its count", ne ? ne->message : NULL);
    int n5 = outlog_note(&nl, OL_WARNING, "Copied 3 rows");
    check(n5 == -4, "the same text at another level is a new note", NULL);
    check(nx1->errors == 1 && nx1->warnings == 0 && outlog_exec(&nl, n5)->warnings == 1 && nx3->warnings == 0 && nx3->errors == 0, "only warning / error notes count for the badge", NULL);
    char *nt = outlog_event_text(&nl, &nl.ev[0], true);
    check(nt && strstr(nt, "error    db1: connection refused") && strstr(nt, "type: lume_message") && !strstr(nt, "execution: #"), "a note exports as a plain message, without an execution number", nt);
    free(nt);
    outlog_clear(&nl);
    check(nl.n == 0 && outlog_note(&nl, OL_INFO, "after clear") == -5, "Clear drops notes; ids keep counting down", NULL);
    outlog_free(&nl);

    printf("%s (%d failures)\n", failures ? "OUTLOG: FAIL" : "OUTLOG: OK", failures);
    return failures ? 1 : 0;
}
