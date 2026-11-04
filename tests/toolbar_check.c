/* SPDX-License-Identifier: AGPL-3.0-only */
/* Headless checks for the toolbar spec: the driver capability table (TBR-030) and Lume's
 * conservative "may write" classification used by PROD confirmations and the Lume-level
 * read-only check (TBR-013 / 029). No SDL, no database. */
#include "caps.h"
#include "rs.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* stubs: caps.c only logs and frees result sets on paths not exercised here */
void log_write(int level, const char *fmt, ...) { (void)level; (void)fmt; }
void rs_free(ResultSet *rs) { (void)rs; }

static int fails;
static void check(bool ok, const char *what) { printf("%s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

int main(void) {
    /* every engine named by the spec has declared, testable rules */
    const char *engines[] = { "sqlite", "postgres", "oracle", "mysql", "mariadb", "sqlserver" };
    for (int i = 0; i < 6; i++) { char w[96]; snprintf(w, sizeof w, "capabilities declared for %s", engines[i]); check(caps_for(engines[i]) != NULL, w); }
    const DriverCaps *sq = caps_for("sqlite"), *pg = caps_for("postgres"), *ora = caps_for("oracle"), *my = caps_for("mysql"), *ms = caps_for("sqlserver");
    check(sq->available && pg->available && ora->available, "SQLite, PostgreSQL and Oracle have a Lume driver");
    check(my->available && caps_for("mariadb")->available && ms->available, "MySQL / MariaDB (db_mysql) and SQL Server (db_mssql, FreeTDS) have a driver");
    check(my->read_only == RO_SESSION && !strcmp(my->read_only_sql, "SET SESSION TRANSACTION READ ONLY"), "MySQL read-only uses its own statement (not PostgreSQL's)");
    check(!sq->schemas && sq->attached_dbs && !sq->set_schema_fmt, "SQLite: no schema selector (attached databases instead), no SET statement invented");
    check(pg->schemas && pg->set_schema_fmt && strstr(pg->set_schema_fmt, "search_path"), "PostgreSQL: default schema through search_path");
    check(ora->schemas && strstr(ora->set_schema_fmt, "CURRENT_SCHEMA"), "Oracle: default schema through CURRENT_SCHEMA");
    check(pg->explain_analyze && !sq->explain_analyze && !ora->explain_analyze, "EXPLAIN ANALYZE only where the engine has it (PostgreSQL)");
    check(!sq->isolation && pg->isolation && pg->isolation_levels, "isolation levels: PostgreSQL yes, SQLite no");
    check(sq->read_only == RO_ENGINE && pg->read_only == RO_SESSION && ora->read_only == RO_LUME_CHECK, "read-only levels: engine (SQLite), session default (PostgreSQL), Lume check (Oracle)");
    check(strstr(caps_ro_text(RO_LUME_CHECK), "still allows writes") != NULL, "the Lume-check level says the server still allows writes (no false promise)");
    check(caps_for("nope") == NULL && caps_count() == 6, "unknown driver: no capabilities");
    /* may-write classification: conservative */
    struct { const char *sql; bool w; } cases[] = {
        { "SELECT * FROM t", false }, { "  -- c\n select 1", false }, { "/* x */ SELECT 1", false },
        { "EXPLAIN SELECT 1", false }, { "EXPLAIN ANALYZE DELETE FROM t", true }, { "PRAGMA table_info(t)", false },
        { "PRAGMA journal_mode = WAL", true }, { "WITH x AS (SELECT 1) SELECT * FROM x", false },
        { "WITH d AS (DELETE FROM t RETURNING *) SELECT * FROM d", true }, { "UPDATE t SET a = 1", true },
        { "delete from t", true }, { "DROP TABLE t", true }, { "CREATE TABLE t(a int)", true }, { "CALL p()", true },
        { "BEGIN", true }, { "selectx FROM t", true }, { "SHOW search_path", false }, { "VALUES (1)", false },
        /* WITH: DML words inside strings, quoted identifiers and comments are not SQL */
        { "WITH e AS (SELECT column_value AS k FROM TABLE(sys.odcivarchar2list('APP.T:SELECT','APP.T.C:UPDATE','APP.LOG:INSERT','APP.P:EXECUTE','SYS:CREATE SESSION'))) SELECT * FROM e", false },
        { "WITH e AS (SELECT 'x:DELETE' AS a, 'y:MERGE' AS b FROM dual) SELECT * FROM e", false },
        { "WITH \"UPDATE\" AS (SELECT 1 AS a) SELECT * FROM \"UPDATE\"", false },
        { "WITH x AS (SELECT 1 /* then UPDATE t */ AS a) SELECT * FROM x", false },
        { "WITH x AS (SELECT 1 AS a) -- an INSERT comes later\nSELECT * FROM x", false },
        { "WITH x AS (SELECT 'it''s an UPDATE' AS a) SELECT * FROM x", false },
        /* ... but a construct one engine reads as a string never hides SQL another engine runs */
        { "WITH x AS (SELECT 1) UPDATE t SET a = 1", true },
        { "WITH x AS (SELECT 1 AS [it's]) UPDATE t SET a = 1", true },              /* SQL Server bracket identifier */
        { "WITH x AS (SELECT 'a\\'' AS s) UPDATE t SET a = 1", true },              /* MySQL backslash escape */
        { "WITH x AS (SELECT 'a\\' AS s) UPDATE t SET a = 1 WHERE b = 'c'", true }, /* standard: the backslash is a character */
        { "WITH x AS (SELECT $$it's$$ AS s) DELETE FROM t", true },                 /* PostgreSQL dollar quoting */
        { "WITH x AS (SELECT q'[it's]' AS s FROM dual) SELECT 1 FROM x WHERE 'a' = 'b' UNION SELECT 1 FROM x; DELETE FROM t", true },
        { "WITH x AS (SELECT 1 # it's\n) UPDATE t SET a = 1", true },               /* MySQL hash comment */
        { "WITH x AS (SELECT 1 AS a) /*!UPDATE t SET a = 1*/ SELECT * FROM x", true },   /* MySQL runs this comment */
        { "WITH x AS (SELECT 5--1 AS v) UPDATE t SET a = 1", true },                /* MySQL: 5 - -1, not a comment */
        { "/*!UPDATE t SET a = 1*/ SELECT 1", true },
        { "WITHX AS (SELECT 1) SELECT 1", true },
    };
    int ok = 0, n = (int)(sizeof cases / sizeof cases[0]);
    for (int i = 0; i < n; i++) {
        bool w = caps_sql_may_write(cases[i].sql, -1);
        if (w == cases[i].w) ok++; else printf("      wrong for: %s (said %s)\n", cases[i].sql, w ? "write" : "read");
    }
    char d[96]; snprintf(d, sizeof d, "may-write classification: %d of %d cases (anything not clearly a read is a write)", ok, n);
    check(ok == n, d);
    printf("TOOLBAR CHECK: %s (%d failures)\n", fails ? "FAIL" : "OK", fails);
    return fails != 0;
}
