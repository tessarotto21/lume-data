#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# The suites CI runs (GitHub-hosted runners): everything that needs no database server, no Docker
# and no real AI (the AI suites talk to the loopback mocks only). Database suites (PostgreSQL,
# Oracle, SQL Server, MySQL containers, the large-catalog test bed) stay in tests/run_all.sh, run on
# a developer machine; so does extract_check (its sandbox runs the host's python, which a CI image may not
# have set up). Writes build/test-report-ci.md; exit 1 on any failure.
# Usage: sh tests/run_ci.sh            (the list for this OS)
#        LUME_CI_SUITES="a b" sh tests/run_ci.sh   (only those)
cd "$(dirname "$0")/.."
mkdir -p build/test-logs
REPORT=build/test-report-ci.md
OS=$(uname -s)
HEADLESS="grid_check exec_check units_check objects_check tbl_check rpt_check toolbar_check outlog_check image_check about_check dock_check edit_check"
case "$OS" in
  Darwin)
    LIST="$HEADLESS schidx_check plan_check gitcore_check crash_check smoke
          about_ui ai_sql_ui conn_dialog_ui conn_secrets_ui connect_fail_ui grid_scrollbar_ui editor_hscroll_ui chat_scroll_ui telemetry_ui crash_report_ui update_ui complete_check dgm_check dgm_ui explorer_folders_ui ssh_tunnel_ui ssh_tunnel_e2e new_query_ui results_tabs_ui output_select_ui
          settings_ui themes_ui query_timeout_ui textfield_ui open_ui rpt_design_ui rpt_export_ui rpt_filter_ui rpt_theme_ui rpt_cardsql_ui rpt_aisql_ui" ;;
  Linux)
    # validated under xvfb in ubuntu:22.04 (BL-058); still to validate there: about_ui, connect_fail_ui,
    # rpt_design_ui, rpt_export_ui
    LIST="$HEADLESS schidx_check plan_check gitcore_check crash_check smoke
          ai_sql_ui conn_dialog_ui grid_scrollbar_ui editor_hscroll_ui chat_scroll_ui telemetry_ui crash_report_ui update_ui complete_check dgm_check dgm_ui explorer_folders_ui ssh_tunnel_ui ssh_tunnel_e2e new_query_ui results_tabs_ui output_select_ui
          settings_ui themes_ui query_timeout_ui textfield_ui open_ui rpt_filter_ui rpt_theme_ui rpt_cardsql_ui rpt_aisql_ui" ;;
  *)
    LIST="$HEADLESS" ;;
esac
[ -n "${LUME_CI_SUITES:-}" ] && LIST=$LUME_CI_SUITES
{
  echo "# Lume CI test report"
  echo
  echo "Generated $(date -u '+%Y-%m-%d %H:%M UTC') on $OS $(uname -m)."
  echo
  echo "| Suite | Result | ok | FAIL | skip | Log |"
  echo "|---|---|---|---|---|---|"
} > "$REPORT"
fails=0
cmake --build build > build/test-logs/build.log 2>&1 || { echo "| build | FAIL | | | | build/test-logs/build.log |" >> "$REPORT"; cat "$REPORT"; exit 1; }
for s in $LIST; do
  log="build/test-logs/$s.log"
  if [ ! -f "tests/$s.sh" ]; then echo "| $s | missing | | | | |" >> "$REPORT"; fails=$((fails + 1)); continue; fi
  printf '%-20s ' "$s"
  if sh "tests/$s.sh" > "$log" 2>&1; then res=pass; else res=FAIL; fails=$((fails + 1)); fi
  ok=$(grep -c '^ok ' "$log"); bad=$(grep -c '^FAIL' "$log"); sk=$(grep -c '^skip' "$log")
  echo "$res ($ok ok, $bad FAIL)"
  echo "| $s | $res | $ok | $bad | $sk | $log |" >> "$REPORT"
done
echo
echo "report: $REPORT ($fails failed)"
[ $fails -eq 0 ]
