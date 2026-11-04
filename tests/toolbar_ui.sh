#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Main toolbar on screen (toolbar spec, TBR-001..052): drives the real app through the harness
# and checks the outcome in the toolbar state line (tbstate), the log and the files written.
# The PostgreSQL part runs only when the dbv-postgres container is up (reported as skip otherwise).
cd "$(dirname "$0")/.."
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
mkdir -p build/ui
fails=0
ok()   { echo "ok    $1"; }
bad()  { echo "FAIL  $1"; fails=$((fails + 1)); }
expect() { if eval "$2"; then ok "$1"; else bad "$1"; fi; }

# SQLite fixture: the demo, a second file, a file that is read-only in Lume
sq_setup() {
  P=$1; rm -rf "$P"; mkdir -p "$P"
  sqlite3 "$P/second.db" "CREATE TABLE t2(id INTEGER PRIMARY KEY, v TEXT); INSERT INTO t2 VALUES (1,'x');"
  sqlite3 "$P/ro.db" "CREATE TABLE keep(a int);"
  printf '[connection]\nid=demo1\nname=Demo (in-memory)\ndriver=sqlite\ndemo=1\npath=:memory:\n\n[connection]\nid=second1\nname=Second\ndriver=sqlite\npath=%s/%s/second.db\n\n[connection]\nid=ro1\nname=RO file\ndriver=sqlite\nread_only=1\npath=%s/%s/ro.db\n' "$PWD" "$P" "$PWD" "$P" > "$P/connections.txt"
}
run() { LUME_PREF_DIR=$1 LUME_SCRIPT=tests/scripts/$2.txt ./build/lume 2>/dev/null; }
missing() { printf '%s\n' "$1" | grep -c '^script: target not found'; }
nth() { printf '%s\n' "$1" | grep '^toolbar ' | sed -n "$2p"; }

# ---- part A: place, groups, hide / compact, Run semantics, datasource per tab, Explorer, Panels, keyboard, context menu, tooltip
P=build/tb-a; sq_setup $P
out=$(run $P tb_a); L=$P/logs/lume.log
expect "every script target found (part A)" '[ "$(missing "$out")" = 0 ]'
s1=$(nth "$out" 1)
expect "TBR-001: a 40-point row under the menu bar; the panels start below it" 'echo "$s1" | grep -q "h=40" && printf "%s\n" "$out" | grep -q "^where \"tab:Explorer\" [0-9]* 73 "'
expect "TBR-002: left to right: workspace, new SQL | datasource, schema | Run | search, panels, more" 'echo "$s1" | grep -q "slots=workspace,new_sql,datasource,schema,run,search(icon),panels(icon),more(icon)"'
expect "TBR-006: hidden, the content takes the height back (no empty band)" 'nth "$out" 2 | grep -q "toolbar hidden h=0" && printf "%s\n" "$out" | grep "^where \"tab:Explorer\"" | sed -n 2p | grep -q " 33 "'
expect "TBR-007: compact mode is 32 points" 'nth "$out" 3 | grep -q "h=32 .*compact=1"'
expect "TBR-017: Run = the statement under the caret (line 2), on the tab's connection, logged with the target" 'grep -q "execution [0-9]*: Run: lines 2-2 on Demo (in-memory) (SQLite)" $L'
expect "TBR-018: Run Selection without a selection is refused with the reason" 'grep -q "sql.run_selection refused: Select the SQL to run first" $L'
expect "TBR-018: a selection that cuts a statement asks first; Cancel runs nothing" 'nth "$out" 4 | grep -q "modal=1" && grep -q "Cancelled: nothing was run" $L'
expect "TBR-019: Run All in File shows the statement count and runs the whole file" 'grep -q "Run All in File: lines 1-2 on Demo" $L'
expect "TBR-025: a repeated Run within 400 ms is the same gesture (no double execution)" 'grep -q "command sql.run ignored: repeated within 400 ms" $L'
expect "TBR-009: the datasource changes for the active tab only (tab 2 on Second, tab 1 still on Demo)" 'nth "$out" 5 | grep -q "ds=\"Second\"" && nth "$out" 6 | grep -q "ds=\"Demo (in-memory)\""'
expect "spec 9 / Gherkin 1: an Explorer click on Second does not redirect Run (runs on Demo)" '[ "$(grep -c "command sql.run (tab \"query.sql\", connection Demo" $L)" -ge 3 ] && ! grep -q "connection Second" $L'
expect "TBR-036: the Panels menu shows Query History; choosing it docks the panel" 'printf "%s\n" "$out" | grep -q "^where \"tab:Query History\" [0-9]"'
expect "TBR-041: Customize > Advanced adds Tx / Commit / Rollback / Explain / History" 'nth "$out" 8 | grep -q "preset=Advanced"'
expect "TBR-047: Ctrl+F5 gives the toolbar the focus; Down opens the focused control; Escape twice returns" 'nth "$out" 9 | grep -q "kb=1" && nth "$out" 10 | grep -q "popover=1 menu=0 kb=1" && nth "$out" 11 | grep -q "kb=0"'
expect "TBR-038: the toolbar's context menu toggles Compact Mode" 'nth "$out" 12 | grep -q "compact=1"'
expect "TBR-044: the tooltip names the action and its shortcut" 'nth "$out" 13 | grep -q "tip=\"Run "'

# ---- keyboard: every control reachable, Escape never runs SQL (Gherkin 6)
P=build/tb-kb; sq_setup $P
out=$(run $P tb_kb); L=$P/logs/lume.log
focus=$(printf '%s\n' "$out" | grep -oE ' focus=[a-z_]+' | sed 's/ focus=//' | sort -u | tr '\n' ' ')
expect "TBR-047: Tab reaches every control ($focus)" 'for c in new_sql datasource schema run search panels more; do echo "$focus" | grep -qw $c || exit 1; done'
expect "TBR-047 / Gherkin 6: Down opens Run's menu, Escape closes it, nothing runs" '! grep -q "command sql" $L'
names=$(printf '%s\n' "$out" | grep -c '^tbname [a-z_]*: .')
expect "TBR-044 / 047: every control has an accessible name ($names named)" '[ "$names" -ge 8 ] && ! printf "%s\n" "$out" | grep -qE "^tbname [a-z_]*: *$"'

# ---- overflow at UI scale 1.6 (TBR-040)
P=build/tb-of; sq_setup $P
out=$(LUME_UI_SCALE=1.6 run $P tb_overflow)
s=$(nth "$out" 1)
expect "TBR-040: narrow bar: lowest priorities in \"...\" ($(echo "$s" | grep -oE 'overflow=[^ ]*'))" 'echo "$s" | grep -qE "overflow=[a-z]" && echo "$s" | grep -q "datasource" && echo "$s" | grep -qE "slots=[^ ]*run"'
expect "TBR-040: no control overlaps another or leaves the bar" 'echo "$s" | grep -q "overlap=0 outside=0"'
expect "TBR-040: the \"...\" menu opens with the overflowed actions" 'nth "$out" 2 | grep -q "menu=1"'

# ---- captures at three window sizes and two UI scales (TBR-051)
geo_ok=1
for sc in 1 1.25 1.6; do
  P=build/tb-sh; sq_setup $P
  out=$(LUME_UI_SCALE=$sc run $P tb_shots)
  printf '%s\n' "$out" | grep '^toolbar' | grep -vq 'overlap=0 outside=0' && geo_ok=0
  for f in 1280 1000 720; do [ -f build/ui/tb_size_$f.bmp ] && mv build/ui/tb_size_$f.bmp build/ui/tb_size_${f}_x$sc.bmp; done
done
expect "TBR-051: captures at 1280 / 1000 / 720 points and UI scales 1, 1.25, 1.6, no overlap anywhere" '[ $geo_ok = 1 ] && [ -f build/ui/tb_size_720_x1.6.bmp ]'

# ---- preferences: customize, restart, reset, corrupt file, export (TBR-039 / 041 / 042 / 043 / 052)
P=build/tb-ws; sq_setup $P
out=$(run $P tb_ws1)
expect "TBR-039: Customize hides Schema and saves" 'nth "$out" 1 | grep -q "slots=workspace,new_sql,datasource,run," && grep "\"hidden\"" $P/toolbar.json | grep -q "\"schema\""'
expect "TBR-052: the exported settings hold ids and flags only (no SQL, connection strings or secrets)" '[ -f $P/toolbar-export.json ] && ! grep -qiE "select|password|host|secret|path" $P/toolbar-export.json'
out=$(run $P tb_ws2)
expect "TBR-042: after a restart the customization is back" 'nth "$out" 1 | grep -q "slots=workspace,new_sql,datasource,run,"'
expect "TBR-043: Reset asks with its scope, then restores the defaults without a restart" 'nth "$out" 2 | grep -q "modal=1" && nth "$out" 3 | grep -q "datasource,schema,run" && grep -q "connections, queries and panel layouts were not touched" $P/logs/lume.log'
printf '{"schemaVersion": 1, "visible": "yes"' > $P/toolbar.json
out=$(run $P tb_ws3)
expect "TBR-052: a corrupt toolbar.json is set aside, the defaults apply, the reason is logged" 'ls $P/toolbar.json.bad-* >/dev/null 2>&1 && grep -q "toolbar preferences rejected" $P/logs/lume.log && nth "$out" 1 | grep -q "toolbar visible"'
expect "TBR-052: queries still run after the recovery" 'grep -q "execution [0-9]*: Run: lines" $P/logs/lume.log'
out=$(run $P tb_ws4)
expect "TBR-004: New Workspace switches to it" 'nth "$out" 1 | grep -q "ws=\"Proj X\""'
expect "TBR-004: closing with unsaved tabs asks Save / Discard / Cancel" 'nth "$out" 2 | grep -q "modal=1"'
expect "TBR-004 / 042: Save keeps the tab's SQL in the workspace; reopening restores it" 'grep -q "SELECT 42 AS answer;" "$P/workspaces/Proj X/tab-1.sql" && nth "$out" 4 | grep -q "ws=\"Proj X\""'
expect "TBR-004: recent workspaces by name only" 'grep -qx "Proj X" $P/recent_workspaces.txt && ! grep -q "/" $P/recent_workspaces.txt'

# ---- part B (SQLite): palette, collisions, menu contents, settings, Switch Tab, Quick Open,
#      history, a manual transaction (new tab, disconnect, Commit), session details, Customize Cancel
P=build/tb-b; sq_setup $P
out=$(run $P tb_more); L=$P/logs/lume.log
expect "every script target found (part B)" '[ "$(missing "$out")" = 0 ]'
expect "TBR-005: the palette finds Run by a synonym (\"execute\") and by its shortcut spelled out (\"cmd enter\")" 'printf "%s\n" "$out" | grep -A0 "^palette Query: Run Statement" | head -1 | grep -q . && [ "$(printf "%s\n" "$out" | grep -c "^palette Query: Run Statement")" -ge 2 ]'
expect "TBR-005 / 044: unavailable commands stay listed with the reason" 'printf "%s\n" "$out" | grep -q "^palette Query › Transaction: Commit .*disabled: No open transaction"'
expect "TBR-048: no two commands share a shortcut" 'printf "%s\n" "$out" | grep -q "^collisions 0"'
expect "TBR-019 / 030: the Run menu: count of statements, Run Selection disabled without one, no EXPLAIN ANALYZE on SQLite" 'printf "%s\n" "$out" | grep -q "^menuitem Run All in File (2 statements)" && printf "%s\n" "$out" | grep -q "^menuitem Run Selection  \[disabled\]" && ! printf "%s\n" "$out" | grep -q "^menuitem Explain Analyze"'
expect "TBR-010 / Gherkin 3: SQLite shows its database file, no fictional schema" 'printf "%s\n" "$out" | grep -q "^menuitem SQLITE DATABASES  ·  NO SCHEMAS" && ! printf "%s\n" "$out" | grep -qi "^menuitem public"'
expect "TBR-024: Query Execution Settings opens" 'nth "$out" 1 | grep -q "modal=3"'
expect "TBR-035: Switch Tab lists the tabs with their connection; Enter goes to the other one" 'nth "$out" 2 | grep -q "modal=6" && nth "$out" 3 | grep -q "ds=\"Demo (in-memory)\""'
expect "TBR-037: Quick Open finds a loaded table and opens a query tab on its connection, not run" 'grep -q "orders opened in a query tab on Demo (in-memory) (not run)" $L'
expect "TBR-034: the history panel opens an entry in a new tab (not run)" 'grep -q "Opened in a new tab on Demo (in-memory) (not run)" $L'
expect "TBR-016: Tx Manual on SQLite: BEGIN confirmed by the session" 'nth "$out" 4 | grep -q "tx=1 manual=1"'
expect "TBR-003: a new SQL tab does not join an open transaction: it asks for its connection" 'nth "$out" 5 | grep -q "popover=1 .*ds=\"\""'
expect "TBR-028: Disconnect with an open transaction asks; Cancel keeps both" 'printf "%s\n" "$out" | grep -q "^tbmodal kind=1 title=\"Open transaction\"" && nth "$out" 6 | grep -q "state=connected .*tx=1"'
expect "TBR-026: Commit shows the target and the session, then ends the transaction" 'printf "%s\n" "$out" | grep -q "^tbmodal kind=1 title=\"Commit?\" body=\"Commit the open transaction of this session?|Target: Demo (in-memory)" && nth "$out" 7 | grep -q "tx=0" && grep -q "Commit of the session" $L'
expect "TBR-015: session details: datasource, safe session id, no connection string" 'printf "%s\n" "$out" | grep "^tbmodal kind=5" | grep -q "Lume session: demo1#" && ! printf "%s\n" "$out" | grep "^tbmodal kind=5" | grep -qiE "password|path="'
expect "TBR-039: Customize previews a preset; Cancel restores the saved toolbar" 'nth "$out" 8 | grep -q "preset=Minimal" && nth "$out" 9 | grep -q "preset=Default"'

# ---- hidden toolbar: shortcuts still run, the choice survives a restart (TBR-006)
P=build/tb-h; sq_setup $P
out=$(run $P tb_hidden1)
expect "TBR-006: hidden, the Run shortcut still runs" 'nth "$out" 1 | grep -q "toolbar hidden h=0" && grep -q "execution [0-9]*: Run: lines" $P/logs/lume.log'
out=$(run $P tb_hidden2)
expect "TBR-006: the hidden toolbar stays hidden after a restart; View brings it back" 'nth "$out" 1 | grep -q "toolbar hidden" && nth "$out" 2 | grep -q "toolbar visible h=40"'

# ---- extension actions (TBR-050)
P=build/tb-ext; sq_setup $P
out=$(run $P tb_ext); L=$P/logs/lume.log
expect "TBR-050: a module's action can be put on the toolbar and runs (snapshot only, no database)" 'nth "$out" 1 | grep -q "cmd:md.reload" && grep -q "markdown: reloaded from its command" $L'
expect "TBR-050: unloading the module removes its action and panels; no dangling button" 'printf "%s\n" "$out" | grep -q "unloadmodule lume.markdown commands=1 panels=2" && ! nth "$out" 2 | grep -q "md.reload"'

# ---- read-only in Lume, engine level (TBR-013)
P=build/tb-ro; sq_setup $P
out=$(run $P tb_ro); L=$P/logs/lume.log
expect "TBR-013: SQLite read-only is enforced by the engine (the write fails)" 'grep -q "attempt to write a readonly database" $L && grep -q "RO file is read-only in Lume (engine)" $L'
expect "TBR-013: the tooltip states the real level of protection" 'nth "$out" 1 | grep -q "Read-only in Lume, enfor"'

# ---- read-only in Lume, Lume-check level (Oracle, TBR-029): strings are not SQL; a refusal reaches the Output
OENV="${LUME_ORA_ENV:-$HOME/workspace/tessarotto/test-servers/.env}"
if [ -z "$LUME_SKIP_ORACLE" ] && docker ps --format '{{.Names}}' | grep -qx dbv-oracle && [ -f "$OENV" ] && [ -e "$HOME/oracle/instantclient/libclntsh.dylib" ]; then
  P=build/tb-ro-ora; rm -rf $P; mkdir -p $P
  OPW=$(grep '^ORACLE_PASSWORD=' "$OENV" | cut -d= -f2- | tr -d '"')
  printf '[connection]\nid=orar\nname=ora RO\ndriver=oracle\nhost=localhost\nport=15210\nservice=FREEPDB1\nuser=system\nenv=DEV\nread_only=1\n' > $P/connections.txt
  ( umask 077; printf 'orar\tpassword\t%s\n' "$OPW" > $P/secrets.txt )
  out=$(run $P tb_ro_ora); L=$P/logs/lume.log; O=$P/output.txt
  expect "every script target found (Oracle read-only)" '[ "$(missing "$out")" = 0 ]'
  expect "TBR-029: a read whose strings hold DML words is not refused (only the write is)" '[ "$(grep -c "Not run: ora RO is read-only in Lume" $O)" = 1 ] && [ "$(grep -c "run refused by Lume.s read-only check" $L)" = 1 ]'
  expect "TBR-029: that read ran and completed on Oracle" 'grep -q "type: statement_completed  execution: #1" $O && grep -q "success  1 of 1 statement ok" $O'
  expect "TBR-029: the write is refused before anything is sent" 'grep -q "run refused by Lume.s read-only check on ora RO" $L && ! grep -qi "ORA-00942" $O'
  expect "TBR-029: the refusal stays in the Output as a skipped execution with the reason" 'grep -q "sql: UPDATE lume_ro_probe SET a = 1" $O && grep -q "warning  Not run: ora RO is read-only in Lume and this SQL may write (Read-only in Lume as a check" $O && grep -q "warning  0 of 1 statement ok · 0 failed · 1 not run" $O'
else echo "skip  Oracle read-only scenario: dbv-oracle / Instant Client not available"; fi

# ---- rendering cost and idle (TBR-046)
P=build/tb-perf; sq_setup $P
frames=$(run $P tb_perf | awk '/^stat op/ { print $4 + 0 }' | sort -n)
n=$(printf '%s\n' "$frames" | grep -c .); p95=$(printf '%s\n' "$frames" | awk -v n="$n" 'NR == int(n * 0.95 + 0.5)'); mx=$(printf '%s\n' "$frames" | tail -1)
awk -v p="$p95" -v m="$mx" -v n="$n" 'BEGIN { ok = n >= 40 && p <= 16; printf "%s  TBR-046: frames right after toolbar changes: %d, p95 %.2f ms, max %.2f ms (bar 16 ms)\n", ok ? "ok  " : "FAIL", n, p, m; exit !ok }' || fails=$((fails + 1))
printf 'wait 4000\nquit\n' > build/ui/tb_idle.txt
LUME_PREF_DIR=$P LUME_SCRIPT=build/ui/tb_idle.txt ./build/lume >/dev/null 2>&1 & pid=$!
sleep 3; cpu=$(ps -o %cpu= -p $pid | tr -d ' '); rss=$(ps -o rss= -p $pid | tr -d ' '); wait $pid
awk -v c="$cpu" -v r="$rss" 'BEGIN { ok = c + 0 <= 2.0; printf "%s  TBR-046: idle with the toolbar shown: CPU %s %% (bar 2 %%), RSS %.0f MB\n", ok ? "ok  " : "FAIL", c, r / 1024; exit !ok }' || fails=$((fails + 1))

# ---- PostgreSQL: schema, Explain, parameters, two sessions, transactions, PROD, failed connect
ENV_FILE="${LUME_PG_ENV:-$HOME/workspace/tessarotto/test-servers/pg/.env}"
if docker exec dbv-postgres true 2>/dev/null && [ -f "$ENV_FILE" ]; then
  P=build/tb-pg; rm -rf $P; mkdir -p $P
  PW=$(grep '^PG_PASSWORD=' "$ENV_FILE" | cut -d= -f2- | tr -d '"')
  {
    printf '[connection]\nid=demo1\nname=Demo (in-memory)\ndriver=sqlite\ndemo=1\npath=:memory:\n'
    for c in "pga:syn A:localhost:15432:" "pgb:syn B:localhost:15432:" "pgprod:syn PROD:localhost:15432:PROD" "pgbad:bad port:localhost:1:" "pgslow:slow host:192.0.2.1:5432:"; do
      id=${c%%:*}; rest=${c#*:}; name=${rest%%:*}; rest=${rest#*:}; host=${rest%%:*}; rest=${rest#*:}; port=${rest%%:*}; env=${rest#*:}
      printf '\n[connection]\nid=%s\nname=%s\ndriver=postgres\nhost=%s\nport=%s\ndatabase=syn\nuser=postgres\n' "$id" "$name" "$host" "$port"
      [ -n "$env" ] && printf 'env=%s\n' "$env"
    done
  } > $P/connections.txt
  ( umask 077; for id in pga pgb pgprod pgbad pgslow; do printf '%s\tpassword\t%s\n' "$id" "$PW"; done > $P/secrets.txt )
  out=$(run $P tb_pg); L=$P/logs/lume.log
  expect "every script target found (PostgreSQL)" '[ "$(missing "$out")" = 0 ]'
  expect "a failed connect (bad port) opens the dialog with the driver message (closed with Close)" 'printf "%s\n" "$out" | grep "^tbmodal" | grep -q "title=\"Could not connect to bad port\""'
  expect "TBR-011 / 045: connected in the background, the toolbar shows it" 'nth "$out" 1 | grep -q "ds=\"syn A\" state=connected"'
  expect "TBR-010: PostgreSQL schemas from the loaded catalog; choosing one sets the session's search_path" 'nth "$out" 2 | grep -q "schema=\"syn_fat\"" && grep -q "Default schema of this session: syn_fat" $L'
  expect "TBR-022: Explain opens a plan of the statement on the same session" 'grep -q "Execution plan of a statement on syn A" $L || grep -q "plan: estimated capture on syn A" $L'
  expect "TBR-024: parameters filled in a dialog; values never stored (history has the template only)" 'nth "$out" 3 | grep -q "modal=2" && grep -q "SELECT :v::int + 1" $P/history.tsv && ! cut -f6- $P/history.tsv | grep -q "41"'
  expect "TBR-021: two sessions run in parallel, the toolbar counts them" 'nth "$out" 4 | grep -q "running=1/2"'
  expect "TBR-020 / Gherkin 2: Stop on tab A cancels session A only; session B completes" 'grep -q "cancel requested: execution [0-9]* on syn A" $L && grep -q "on syn A cancelled after" $L && grep -qE "Loaded 1 row in 4(\\.[0-9]+)?s" $L && ! grep -q "cancel requested: execution [0-9]* on syn B" $L'
  expect "TBR-016: manual mode opens a transaction confirmed by the session (tx=1)" 'nth "$out" 6 | grep -q "tx=1 manual=1"'
  expect "TBR-009 / 028 / Gherkin 5: switching with a pending transaction asks; Cancel keeps session and transaction" 'nth "$out" 7 | grep -q "modal=1" && nth "$out" 8 | grep -q "ds=\"syn A\" .*tx=1"'
  expect "TBR-027: Rollback is confirmed and ends the transaction" 'nth "$out" 9 | grep -q "tx=0" && grep -q "Rollback of the session" $L'
  expect "TBR-012: Reconnect with an open transaction reports it lost (never committed)" 'grep -q "an open transaction was dropped with the old session" $L'
  expect "TBR-014 / 029: on PROD a write asks first with the target; Cancel runs nothing; a read runs" 'nth "$out" 12 | grep -q "ds=\"syn PROD\".*" && grep -q "Cancelled: nothing was run" $L && grep -q "Run: lines 1-1 on syn PROD" $L'
  sum_bad=$(printf '%s\n' "$out" | grep '^outnotes .* in bad port ')
  sum_a=$(printf '%s\n' "$out" | grep '^outnotes .* in syn A ')
  expect "OUT: the failed connect is kept in that connection's Output as an error, after the Connecting note" 'printf "%s\n" "$out" | grep -q "^outnote info Connecting to bad port" && printf "%s\n" "$out" | grep -q "^outnote error bad port: " && [ -n "$sum_bad" ]'
  expect "OUT: the error badges that session's Output; it does not land in another session" 'printf "%s\n" "$sum_bad" | grep -qE "unseen warnings/errors: [1-9]" && [ -n "$sum_a" ] && ! printf "%s\n" "$out" | sed -n "/^outnotes .* in bad port /,/^outnotes .* in syn A /p" | grep -q "^outnote error bad port"'
  expect "OUT: notes never hold the password" '! printf "%s\n" "$out" | grep "^outnote" | grep -qF "$PW"'
  expect "TBR-011: a failed connect shows failed and the reason (no password)" 'nth "$out" 13 | grep -q "state=failed" && grep -q "connect failed: bad port" $L && ! grep -qF "$PW" $L $P/history.tsv $P/toolbar.json'
  expect "TBR-011 / 045: connecting shows the state while the UI keeps drawing frames; Cancel Connect stops it" 'nth "$out" 14 | grep -q "state=connecting" && printf "%s\n" "$out" | grep "^stat connecting" | awk "{ exit !(\$4 + 0 < 16) }" && nth "$out" 15 | grep -q "state=disconnected"'
  expect "TBR-022: EXPLAIN ANALYZE warns that it executes the statement" 'printf "%s\n" "$out" | grep -q "^tbmodal kind=1 title=\"Run EXPLAIN ANALYZE?\" body=\"EXPLAIN ANALYZE executes the statement"'
  expect "TBR-051: disconnect while a statement runs is refused (Stop first)" 'grep -q "command ds.disconnect refused: A statement is running" $L'
  expect "TBR-023: Re-run on PROD shows the target and the SQL as it ran, then runs it as a new execution" 'printf "%s\n" "$out" | grep -q "^tbmodal kind=1 title=\"Re-run on PROD?\" body=\"Target: syn PROD.*not the editor" && grep -q "Re-run of the SQL that ran at" $L'
else
  echo "skip  PostgreSQL toolbar scenarios (dbv-postgres not running or no test credentials)"
fi

for f in build/ui/tb_*.bmp; do [ -f "$f" ] && sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null 2>&1 && rm "$f"; done
echo "TOOLBAR UI: $([ $fails -eq 0 ] && echo OK || echo FAIL) ($fails failures)"
exit $fails
