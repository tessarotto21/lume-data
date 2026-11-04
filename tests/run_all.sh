#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Cross-driver validation suite (US-060): runs every headless and UI check, records the
# environment (hardware, OS, compiler, library and server versions) and writes
# build/test-report.md with one line per suite plus the per-scenario log. Suites whose
# backend is not reachable are reported as "not run", never as passed. Exit 1 on any failure.
cd "$(dirname "$0")/.."
mkdir -p build/test-logs
REPORT=build/test-report.md
: > "$REPORT"
fails=0
section() { printf '\n## %s\n\n' "$1" >> "$REPORT"; }
line() { printf '%s\n' "$1" >> "$REPORT"; }

line "# Lume result grid: test report"
line ""
line "Generated $(date '+%Y-%m-%d %H:%M:%S') on $(hostname -s)."
section "Environment"
line "| Item | Value |"
line "|---|---|"
line "| Hardware | $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m), $(( $(sysctl -n hw.memsize 2>/dev/null || echo 0) / 1073741824 )) GB |"
line "| OS | $(sw_vers -productName 2>/dev/null) $(sw_vers -productVersion 2>/dev/null || uname -sr) |"
line "| Compiler | $(cc --version | head -1) |"
line "| SDL | $(otool -L build/lume 2>/dev/null | grep -o 'libSDL3[^ ]* (compatibility[^)]*)' | head -1) |"
line "| SQLite (linked) | $(printf '#include <sqlite3.h>\n#include <stdio.h>\nint main(void){printf("%%s", sqlite3_libversion());return 0;}' > build/test-logs/sqv.c && cc -o build/test-logs/sqv build/test-logs/sqv.c -lsqlite3 2>/dev/null && build/test-logs/sqv) |"
line "| PostgreSQL server | $(docker exec dbv-postgres postgres --version 2>/dev/null || echo 'not reachable') |"
line "| libpq (dlopen) | $(ls /opt/homebrew/lib/libpq.*.dylib 2>/dev/null | head -1 | xargs -n1 basename 2>/dev/null || echo '?'), $(otool -L /opt/homebrew/lib/libpq.5.dylib 2>/dev/null | grep -o 'current version [0-9.]*' | head -1) |"
line "| Oracle client (in the container) | $(docker exec dbv-oracle sh -c 'ls $ORACLE_HOME/lib/libclntsh.so.*' 2>/dev/null | head -1 | xargs -n1 basename 2>/dev/null || echo 'not reachable') |"
line "| Oracle server | $(docker exec dbv-oracle sh -c '$ORACLE_HOME/bin/sqlplus -v' 2>/dev/null | grep -o 'Release [0-9.]*' | head -1 || echo 'not reachable') |"
line "| Binary | $(wc -c < build/lume 2>/dev/null) bytes, $(cat src/*.c src/*.h | wc -l | tr -d ' ') source lines |"

section "Suites"
line "| Suite | Scope | Result | ok | FAIL | skip | Log |"
line "|---|---|---|---|---|---|---|"
run_suite() {   # name, scope, command
    name=$1; scope=$2; shift 2
    log="build/test-logs/$name.log"
    if "$@" > "$log" 2>&1; then res="pass"; else res="FAIL"; fails=$((fails + 1)); fi
    ok=$(grep -c '^ok ' "$log"); bad=$(grep -c '^FAIL' "$log"); sk=$(grep -c '^skip' "$log")
    line "| $name | $scope | $res | $ok | $bad | $sk | $log |"
}
not_run() { line "| $1 | $2 | not run | | | | $3 |"; }

cmake --build build > build/test-logs/build.log 2>&1 || { line "| build | app | FAIL | | | | build/test-logs/build.log |"; fails=$((fails + 1)); }
run_suite grid_check   "model: sort, filter, search, copy, export formats, guard, XLSX, transpose, compare, geo, write failures" sh tests/grid_check.sh
run_suite extract_check "extractors: approval, sandbox, no shell, timeout, cancel, audit" sh tests/extract_check.sh
run_suite extract_linux "extractors on Linux: bubblewrap sandbox in a Debian container, as an ordinary user" sh tests/extract_check_linux.sh
run_suite exec_check   "splitter (3 dialects) and async executor on SQLite: cancel, stop / continue on error, error offsets" sh tests/exec_check.sh
run_suite units_check  "duration formatting: unit by magnitude (ns, us, ms, s, min, h), rounding across units" sh tests/units_check.sh
run_suite objects_check "object editor core: formatter golden cases, diff / 3-way, CREATE headers, PG signatures, planner per engine, diagnostics, extension contracts" sh tests/objects_check.sh
run_suite objects_db   "object editor end to end on SQLite, PostgreSQL and Oracle: load, validate, plan, apply, rollback, conflicts, overloads, INVALID status, lost connection" sh tests/objects_db_check.sh
run_suite objects_ui   "UI harness: object editor (Explorer objects, tab, drafts, review, apply, conflicts, read-only, diagnostics, PostgreSQL contract / overloads / PROD, large definitions)" sh tests/objects_ui.sh
run_suite nsx_check    "Schema Explorer v2 model, headless (170k simulated schemas): adapters' SQL, Schemas / Others, keyset pages, server search, exact counts only, pins, persistence, stale answers, timeouts" sh tests/nsx_check.sh
run_suite nsx_ui       "UI harness: Schema Explorer v2 on PostgreSQL with 170,009 schemas, Oracle ALL_USERS, SQL Server, MySQL Databases, SQLite tree: menus, search, pin / Move to Others, persistence, idle CPU" sh tests/nsx_ui.sh
run_suite objcat_ui "completion with every visible object read in a second session on Oracle, PostgreSQL, MySQL and MariaDB (schemas, SCHEMA., synonyms, other schemas' columns in the background, INSERT column list, disk cache, setting off); the Explorer's package members and catalog Sequences" sh tests/objcat_ui.sh
run_suite deadnet_ui "every server driver: quitting with sessions whose network died (container paused) never freezes: idle logoff, a running statement, an open cursor" sh tests/deadnet_ui.sh
run_suite tbl_check    "Visual Table Designer core, headless: golden DDL for 5 dialects, rename = RENAME, risk / rollback classes, validation, drafts, SQLite rebuild end to end" sh tests/tbl_check.sh
run_suite tbl_db_check "Visual Table Designer on real servers: PostgreSQL, MySQL, MariaDB, SQL Server, Oracle (create, alter, read-back, drift, partial failure, drop column)" sh tests/tbl_db_check.sh
run_suite tdes_ui      "UI harness: Visual Table Designer (new / modify / undo / rebuild with consent / drift / drafts / drop / Explorer menus; PostgreSQL USING, data checks, PROD, read-only)" sh tests/tdes_ui.sh
run_suite plan_check   "Execution plans + Lume Intelligence, headless: real fixtures of 6 engines, capture SQL / write safety, diagnostics with evidence, index candidates, AI request / response, loopback mock (key file, hostile answer, cancel)" sh tests/plan_check.sh
run_suite plan_ui      "UI harness: Execution Plan panel (SQLite, PostgreSQL estimated / actual, PROD policy, imports of every engine, comparison, AI off / mock with preview and audit, index -> Table Designer, idle CPU)" sh tests/plan_ui.sh
run_suite rpt_check    "Report Studio + AI for reports, headless: read-only policy, semantic catalog and SQL compiler, golden dataset (SQLite), ReportSpec / periods / formats, AI proposals (validation, conflicts, scope, injection), tools, PNG" sh tests/rpt_check.sh
run_suite schidx_check "headless (ASan): schema index for the assistant on large databases: 15k-table catalog, picking per question, search, byte budget, untrusted text, cache, bulk catalog SQL per engine" sh tests/schidx_check.sh
run_suite rpt_db_check "Report Studio golden dataset on PostgreSQL / MySQL / MariaDB / SQL Server / Oracle through Lume's drivers, inside the read-only transaction; engine-level refusal of writes" sh tests/rpt_db_check.sh
run_suite rpt_ui       "UI harness: Report Studio (cards, cache / undo without re-query, layout, locks, themes, filters, policy, timeout, save / autosave / read mode, PNG) and the assistant via mock (ambiguity, plan, approval, copilot, injection, consent, audit)" sh tests/rpt_ui.sh
run_suite gitcore_check "Git Core, headless, against real Git: argv runner (timeout, cancel, no prompts), redaction, discovery (worktrees, Unicode), porcelain v2 status, diff / hunk staging, log / refs / remotes, conflicts, 1,000 files" sh tests/gitcore_check.sh
run_suite rpt_aisql_ui  "UI harness: the assistant from the database structure via mock (consent first, no rows sent, AI SQL checked locally with WHERE 1=0, one correction round, read-only policy, not-certified marks, KPI equals SQLite) and the e-commerce demo database" sh tests/rpt_aisql_ui.sh
run_suite rpt_aisql_pg  "PostgreSQL: the assistant's SQL through a SELECT-only login (BL-051): grant check, structure limited to that login's grants, cards on the read-only connection, the database refuses writes, require_readonly_ai_sql" sh tests/rpt_aisql_pg.sh
run_suite rpt_bigdb_pg "UI harness: the assistant on a large PostgreSQL database (thousands of tables): structure indexed in the background and cached, only the tables of the question sent, search_schema answered locally, the SQL then checked and run" sh tests/rpt_bigdb_pg.sh
run_suite rpt_export_ui "UI harness: report files where the user wants them (Save As on first save, later saves to the same file, autosave / versions in prefs, recent list) and exports as PDF, PNG, card CSV and page XLSX" sh tests/rpt_export_ui.sh
run_suite open_ui      "UI harness: File > Open / drag and drop by the kind of file (.sql query tab, .lrpt report, .db connection, content sniffing without extension)" sh tests/open_ui.sh
run_suite rpt_theme_ui  "UI harness: shared report themes (.lrtheme with logo, referenced by several reports and the workspace Brand Kit, outside edits restyle them, local overrides, locked themes, copy in the report when the file is missing, header and logo on screen and in PNG)" sh tests/rpt_theme_ui.sh
run_suite rpt_cardsql_ui "UI harness: the selected card's query in the properties panel (embedded SQL editor, Apply through the read-only policy, rows / database error of the last run, compiled SQL of catalog cards read-only)" sh tests/rpt_cardsql_ui.sh
run_suite rpt_filter_ui "UI harness: report filters as SQL parameters (options from a read-only query, all = NULL, literals, :period_start / :period_end, saved with the report) and the assistant adding a filter and cards that use it" sh tests/rpt_filter_ui.sh
run_suite rpt_design_ui "UI harness: the assistant as a designer (set_theme checked for contrast, card styles, shapes, one undo step, a locked team theme refused, the model told the current theme only)" sh tests/rpt_design_ui.sh
run_suite ai_sql_ui "UI harness: Lume Intelligence in a query tab: the tab's SQL and last error as context, Fix the error / Explain, edits applied only by the user (undoable, refused when the tab changed), a new tab, data changes flagged and never run, Cmd+K, prose refused" sh tests/ai_sql_ui.sh
run_suite settings_ui "UI harness: the Settings screen (Cmd+,) and its Lume Intelligence section: fast model saved to ai.txt (no key), Forget per connection, Esc closes; timeouts per section saved" sh tests/settings_ui.sh
run_suite query_timeout_ui "UI harness: the query timeout from Settings cancels a statement that runs longer; the session runs the next one" sh tests/query_timeout_ui.sh
run_suite output_select_ui "UI harness: the Output text view: select with the mouse (drag / double / triple click), Cmd+C copies the selection (system clipboard)" sh tests/output_select_ui.sh
run_suite sql_eval "Lume Intelligence evaluation (mock): questions on the demo database classified first-try / corrected / failed from the audit; the same script runs a real provider only with --real and LUME_ALLOW_REAL_AI=1 typed by the user" sh tests/evals/sql_eval.sh
run_suite openai_ui    "UI harness: OpenAI for Lume Intelligence via mock (API key in the secret store, model list, Responses API; Continue with ChatGPT: OAuth PKCE loopback, session in the secret store, SSE on the plan, refresh rotation, usage limit, revoked session)" sh tests/openai_ui.sh
run_suite git_ui       "UI harness: Source Control (file tabs, commit only staged, hunk staging, unsaved buffers vs pull, rejected push, fetch / pull strategies, branches, reload on checkout, conflicts, discard + audit, stash, masked credentials, init, AI commit message with consent, database untouched)" sh tests/git_ui.sh
run_suite about_check  "About metadata, headless: real version / build / channel / platform, honest Not available, drivers from the registry, Copy Info allowlist, URL validation" sh tests/about_check.sh
run_suite about_ui     "UI harness: About Lume Data (brand entry, single instance, keyboard, Esc / focus, licenses, Copy Info without secrets, asset fallback, idle CPU)" sh tests/about_ui.sh
run_suite outlog_check "execution output model: events, ordering, dedup, retention, search, redaction, export" sh tests/outlog_check.sh
run_suite image_check  "PNG / JPEG decoder: valid, truncated, bomb headers, corrupt, unsupported" sh tests/image_check.sh
run_suite edit_check   "SQLite editing: conflicts, atomicity, precision, generated columns, commit failure" sh tests/edit_check.sh
if docker exec dbv-postgres true 2>/dev/null; then
    run_suite pg_check "PostgreSQL: streaming export, cancel, backend kill, server select, editing with a second session, commit-time kill" sh tests/pg_check.sh
else not_run pg_check "PostgreSQL" "container dbv-postgres not running"; fi
if [ -z "$LUME_SKIP_ORACLE" ] && docker exec dbv-oracle true 2>/dev/null; then
    run_suite oracle_check "Oracle: driver, cursor, capabilities (Not supported marked), editing with a second session" sh tests/oracle_check.sh
else not_run oracle_check "Oracle" "container dbv-oracle not running or LUME_SKIP_ORACLE set"; fi
run_suite perf_check   "benchmark: timings and memory" sh tests/perf_check.sh
run_suite output_perf  "Output panel on screen: 100k events, scroll, search (frame ms, RSS)" sh tests/output_perf.sh
run_suite smoke        "UI: launch, run, preview, screenshots" sh tests/smoke.sh
run_suite grid_ui      "UI harness: 40 screenshots across server ops, layout, viewer, budget, edit, V3" sh tests/grid_ui.sh
run_suite results_tabs_ui "UI harness: results belong to the query tab that ran them; a re-run replaces them; switching tabs shows the last run of each tab" sh tests/results_tabs_ui.sh
run_suite new_query_ui "UI harness: a connection context menu offers New Query (a query tab on that connection, connecting first)" sh tests/new_query_ui.sh
run_suite grid_scrollbar_ui "UI harness: the result grid scroll bar can be dragged with the mouse" sh tests/grid_scrollbar_ui.sh
run_suite editor_hscroll_ui "UI harness: the SQL editor never scrolls sideways past its text" sh tests/editor_hscroll_ui.sh
run_suite chat_scroll_ui "UI harness: an approval taller than the chat opens at its title once and stays at the end once scrolled there" sh tests/chat_scroll_ui.sh
run_suite conn_dialog_ui "UI harness: New Connection picks the database first (search), PROD suggests read-only, Test reports beside its button, editing changes the type through the picker" sh tests/conn_dialog_ui.sh
run_suite themes_ui "UI harness: colour themes from Settings > Appearance and the Command Palette, saved and restored, Match the system, unknown theme keeps the default" sh tests/themes_ui.sh
run_suite conn_secrets_ui "UI harness: connection passwords live in the OS secret store (one-time move out of secrets.txt, back on the next launch, removed with the connection); fake password, test service" sh tests/conn_secrets_ui.sh
run_suite telemetry_ui "UI harness: anonymous usage statistics are off by default; with consent only counts and ranges go out (loopback mock), never SQL, names, hosts, users or paths" sh tests/telemetry_ui.sh
run_suite complete_check "headless (ASan): autocompletion of JOIN ... ON conditions, catalog objects (schemas, SCHEMA., synonyms, kind filter), columns arriving in the background, qualified columns, INSERT column list, 200k-object speed" sh tests/complete_check.sh
run_suite dgm_check "ER diagram model, headless (ASan / UBSan too): catalog of the 14k fixture in 4 statements, cardinality rules, layout determinism and time, culling grid, find p95, file round trip, exports escaping" sh tests/dgm_check.sh
run_suite dgm_ui "UI harness: ER diagrams (open focused, find / Next across the scope, expand budget, positions kept, changes flagged, relations / notes / entities, every export, 14k frames, Lume Intelligence mock, idle CPU, read-only)" sh tests/dgm_ui.sh
run_suite update_ui "UI harness: update check against GitHub releases (launch + daily, Download opens this platform's installer, Skip This Version, Help > Check for Updates, Settings switch; loopback mock)" sh tests/update_ui.sh
run_suite crash_report_ui "UI harness: crash reports with consent (ask by default: view / send / do not send; always; never), Sentry-format event with no path, user or host (loopback mock)" sh tests/crash_report_ui.sh
run_suite connect_fail_ui "UI harness: a failed connect opens a dialog with the driver message and Retry / Edit connection / Close" sh tests/connect_fail_ui.sh
run_suite ssh_tunnel_ui "UI harness: SSH tunnel through the system ssh (offline: ssh's refusal named in the dialog, option look-alike hosts refused, settings kept when switched off, no ssh left running)" sh tests/ssh_tunnel_ui.sh
run_suite ssh_tunnel_e2e "UI harness: SSH tunnel end to end through a throwaway local sshd (unknown host key asked with its fingerprint from the connect failure and from Test connection, changed key refused, exactly the approved key remembered, a query through the tunnel when the test PostgreSQL runs, Duplicate keeps the tunnel, orphaned ssh of a killed run stopped at launch)" sh tests/ssh_tunnel_e2e.sh
run_suite explorer_folders_ui "UI harness: Explorer folders (nested, closed until opened and remembered; rename / remove / new connection here from the folder menu)" sh tests/explorer_folders_ui.sh
run_suite dock_check   "docking engine: transactions, undo, presets, persistence, corrupt layouts, drop targets, latency" sh tests/dock_check.sh
run_suite panels_ui    "UI harness: panels shell, drag & drop, editor groups, layouts, native windows, perf, idle CPU" sh tests/panels_ui.sh
run_suite textfield_ui "UI harness: text fields (word / line moves, selection, undo, clipboard, scroll, double / triple click)" sh tests/textfield_ui.sh
run_suite toolbar_check "main toolbar, headless: driver capability table (5 engines), SQL write classification" sh tests/toolbar_check.sh
run_suite toolbar_ui    "UI harness: main toolbar (TBR-001..052): layout, overflow, Run semantics, sessions in parallel, transactions, PROD, keyboard, prefs, workspaces" sh tests/toolbar_ui.sh
run_suite crash_check  "crash regressions under ASan: two connections + preview of the inactive one" sh tests/crash_check.sh

section "Benchmark output"
line '```'
cat build/test-logs/perf_check.log build/test-logs/output_perf.log >> "$REPORT"
line '```'
section "Scenarios"
for f in build/test-logs/grid_check.log build/test-logs/exec_check.log build/test-logs/outlog_check.log build/test-logs/extract_check.log build/test-logs/edit_check.log build/test-logs/pg_check.log build/test-logs/oracle_check.log; do
    [ -f "$f" ] || continue
    line ""; line "### $(basename "$f" .log)"; line ""
    grep -E '^(ok|FAIL|skip)' "$f" | sed 's/^/- /' >> "$REPORT"
done
line ""
line "Result: $([ $fails -eq 0 ] && echo 'all suites passed' || echo "$fails suite(s) failed")."
echo "report: $REPORT ($fails failed)"
exit $fails
