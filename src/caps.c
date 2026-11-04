/* SPDX-License-Identifier: AGPL-3.0-only */
/* Driver capabilities and read-only enforcement: see caps.h. UI thread only. */
#include "caps.h"
#include "sqllex.h"
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include "conn.h"
#include "db.h"
#include "rs.h"
#include "log.h"

static const DriverCaps table[] = {
    { "sqlite", "SQLite", true,
      .schemas = false, .attached_dbs = true, .transactions = true, .isolation = false,
      .explain = true, .explain_analyze = false, .cancel = true, .parameters = true, .read_only = RO_ENGINE,
      .explain_prefix = "EXPLAIN QUERY PLAN " },
    { "postgres", "PostgreSQL", true,
      .schemas = true, .transactions = true, .isolation = true,
      .explain = true, .explain_analyze = true, .cancel = true, .parameters = true, .read_only = RO_SESSION,
      .explain_prefix = "EXPLAIN ", .explain_analyze_prefix = "EXPLAIN (ANALYZE, BUFFERS) ",
      .set_schema_fmt = "SET search_path TO %s", .isolation_levels = "READ COMMITTED|REPEATABLE READ|SERIALIZABLE" },
    { "oracle", "Oracle", true,
      .schemas = true, .transactions = true, .isolation = true,
      .explain = true, .explain_analyze = false, .cancel = true, .parameters = true, .read_only = RO_LUME_CHECK,
      .explain_prefix = "EXPLAIN PLAN FOR ",
      .set_schema_fmt = "ALTER SESSION SET CURRENT_SCHEMA = %s", .isolation_levels = "READ COMMITTED|SERIALIZABLE" },
    /* db_mysql.c: one driver, two engines (BL-013) */
    { "mysql", "MySQL", true,
      .schemas = true, .transactions = true, .isolation = true,
      .explain = true, .explain_analyze = true, .cancel = true, .parameters = true, .read_only = RO_SESSION,
      .explain_prefix = "EXPLAIN ", .explain_analyze_prefix = "EXPLAIN ANALYZE ",
      .set_schema_fmt = "USE %s", .isolation_levels = "READ UNCOMMITTED|READ COMMITTED|REPEATABLE READ|SERIALIZABLE",
      .read_only_sql = "SET SESSION TRANSACTION READ ONLY" },
    { "mariadb", "MariaDB", true,
      .schemas = true, .transactions = true, .isolation = true,
      .explain = true, .explain_analyze = true, .cancel = true, .parameters = true, .read_only = RO_SESSION,
      .explain_prefix = "EXPLAIN ", .explain_analyze_prefix = "EXPLAIN ANALYZE ",
      .set_schema_fmt = "USE %s", .isolation_levels = "READ UNCOMMITTED|READ COMMITTED|REPEATABLE READ|SERIALIZABLE",
      .read_only_sql = "SET SESSION TRANSACTION READ ONLY" },
    /* db_mssql.c through FreeTDS (BL-014) */
    { "sqlserver", "SQL Server", true,
      .schemas = true, .transactions = true, .isolation = true,
      .explain = true, .explain_analyze = false, .cancel = true, .parameters = true, .read_only = RO_LUME_CHECK,
      .explain_prefix = "SET SHOWPLAN_TEXT ON; ",
      .set_schema_fmt = "USE %s", .isolation_levels = "READ UNCOMMITTED|READ COMMITTED|REPEATABLE READ|SNAPSHOT|SERIALIZABLE" },
};

const DriverCaps *caps_for(const char *id) {
    for (size_t i = 0; id && i < sizeof table / sizeof table[0]; i++) if (!strcmp(table[i].driver, id)) return &table[i];
    return NULL;
}
int caps_count(void) { return (int)(sizeof table / sizeof table[0]); }
const DriverCaps *caps_at(int i) { return i >= 0 && i < caps_count() ? &table[i] : NULL; }

const char *caps_ro_text(int level) {
    switch (level) {
    case RO_ENGINE: return "Read-only in Lume, enforced by the database engine for this connection";
    case RO_SESSION: return "Read-only in Lume as the session default: the server refuses writes, but SQL in the editor could switch it back; database privileges remain the real protection";
    case RO_LUME_CHECK: return "Read-only in Lume as a check before running only: the server still allows writes for this login";
    default: return "";
    }
}

/* Runs one statement on an idle connection and reports whether it succeeded. */
static bool run_quiet(Db *db, const char *sql) {
    ResultSet rs;
    db->drv->exec(db, sql, -1, 1, &rs);
    bool ok = rs.ok;
    if (!ok) LOG_W("read-only setup failed: %s", rs.error);
    rs_free(&rs);
    return ok;
}

int caps_apply_read_only(Connection *c) {
    if (!c->read_only || !c->db) return RO_NONE;
    const DriverCaps *k = caps_for(c->driver);
    if (!k) return RO_LUME_CHECK;
    int level = k->read_only;
    if (level == RO_ENGINE && !run_quiet(c->db, "PRAGMA query_only = ON")) level = RO_LUME_CHECK;
    if (level == RO_SESSION && !run_quiet(c->db, k->read_only_sql ? k->read_only_sql : "SET SESSION CHARACTERISTICS AS TRANSACTION READ ONLY")) level = RO_LUME_CHECK;
    LOG_I("%s is read-only in Lume (%s)", c->name, level == RO_ENGINE ? "engine" : level == RO_SESSION ? "session default" : "Lume check");
    return level;
}

/* Skips whitespace and SQL comments; returns the first keyword's start. */
static const char *first_word(const char *s, const char *end) {
    for (;;) {
        while (s < end && isspace((unsigned char)*s)) s++;
        if (s + 1 < end && s[0] == '-' && s[1] == '-') { while (s < end && *s != '\n') s++; continue; }
        if (s + 1 < end && s[0] == '/' && s[1] == '*' && !(s + 2 < end && s[2] == '!')) {   /* slash-star-bang is run by MySQL: not skipped */ const char *e = s + 2; while (e + 1 < end && !(e[0] == '*' && e[1] == '/')) e++; s = e + 2 < end ? e + 2 : end; continue; }
        return s;
    }
}

static const char *const dml_words[] = { "INSERT", "UPDATE", "DELETE", "MERGE", NULL };

/* A DML keyword anywhere in raw text (word boundaries only): the old, quote-blind scan. */
static bool raw_has_dml(const char *s, const char *end) {
    for (const char *p = s; p < end; p++)
        for (int i = 0; dml_words[i]; i++) {
            size_t n = strlen(dml_words[i]);
            if ((size_t)(end - p) >= n && !strncasecmp(p, dml_words[i], n) && (p == s || !isalnum((unsigned char)p[-1])) && (p + n == end || !isalnum((unsigned char)p[n]))) return true;
        }
    return false;
}

/* A WITH statement writes when a DML keyword appears as SQL, not inside a string, quoted
 * identifier or comment. Lume does not know here which engine runs it, so the text is lexed
 * with every built-in dialect and any dialect that sees a DML keyword makes it a write: a
 * construct one engine reads as a string can never hide a statement another engine would run.
 * Two MySQL forms the lexer reads as comments are still scanned: an executable comment
 * (slash-star-bang, run by the server) and a double dash not followed by a space (arithmetic in MySQL). */
static bool with_may_write(const char *w, const char *end) {
    int len = (int)(end - w);
    SqlTok *tk = malloc(((size_t)len + 1) * sizeof *tk);
    if (!tk) return true;
    bool writes = false;
    for (int d = 0; d < DIALECT_COUNT && !writes; d++) {
        int n = sql_lex(w, len, d, tk, len + 1);
        if (n > len) { writes = true; break; }   /* cut: cannot happen (a token is at least one byte), stay conservative */
        for (int i = 0; i < n && !writes; i++) {
            const char *s = w + tk[i].off, *e = s + tk[i].len;
            if (tk[i].kind == TK_WORD) { for (int k = 0; dml_words[k]; k++) if (tok_is(w, &tk[i], dml_words[k])) { writes = true; break; } }
            else if (tk[i].kind == TK_COMMENT && tk[i].len >= 3 && ((s[0] == '/' && s[1] == '*' && s[2] == '!') || (s[0] == '-' && s[1] == '-' && !isspace((unsigned char)s[2]))))
                writes = raw_has_dml(s, e);
        }
    }
    free(tk);
    return writes;
}

bool caps_sql_may_write(const char *sql, int len) {
    if (!sql) return false;
    const char *end = sql + (len < 0 ? (int)strlen(sql) : len);
    const char *w = first_word(sql, end);
    static const char *reads[] = { "SELECT", "SHOW", "DESCRIBE", "DESC", "EXPLAIN", "VALUES", "TABLE", "PRAGMA", NULL };
    for (int i = 0; reads[i]; i++) {
        size_t n = strlen(reads[i]);
        if ((size_t)(end - w) >= n && !strncasecmp(w, reads[i], n) && (w + n == end || !isalnum((unsigned char)w[n]))) {
            /* a read keyword, but a CTE or EXPLAIN ANALYZE may still modify data */
            if (i == 4) { const char *x = first_word(w + n, end); if ((size_t)(end - x) >= 7 && !strncasecmp(x, "ANALYZE", 7)) return true; }
            if (i == 7) { for (const char *p = w; p < end; p++) if (*p == '=') return true; }   /* PRAGMA x = y writes */
            return false;
        }
    }
    if ((size_t)(end - w) >= 4 && !strncasecmp(w, "WITH", 4) && (w + 4 == end || !isalnum((unsigned char)w[4])))
        return with_may_write(w, end);   /* WITH ... DELETE / UPDATE / INSERT / MERGE */
    return w < end;   /* anything else (DML, DDL, CALL, BEGIN ... ) may write */
}

