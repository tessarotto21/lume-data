#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Colour themes (Settings > Appearance > Theme, "Theme: ..." in the Command Palette): the choice
# applies at once, is saved in settings.txt and comes back on the next launch; "Match the system"
# follows the OS; an unknown theme in settings.txt keeps the default. Screenshots per theme in
# build/themes-out for a look.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=visible
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
st() { printf '%s\n' "$1" | grep '^settingsstate' | sed -n "$2p"; }
P=build/themes-ui; rm -rf $P; sh tests/rpt_setup_sqlite.sh $P >/dev/null || { echo "FAIL  setup"; exit 1; }
OUT=$PWD/build/themes-out; rm -rf "$OUT"; mkdir -p "$OUT"
sed "s|__OUT__|$OUT|g" tests/scripts/themes.txt > build/themes.txt
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=build/themes.txt ./build/lume 2>&1)
printf '%s\n' "$out" > build/themes_ui.out
expect "every script target found" '! printf "%s\n" "$out" | grep -q "target not found"'
expect "the default is Lume Dark" 'st "$out" 1 | grep -q "section=Appearance .*theme=lume-dark applied=lume-dark"'
expect "Settings > Appearance > Dracula applies it at once" 'st "$out" 2 | grep -q "theme=dracula applied=dracula"'
expect "the Command Palette's \"Theme: Lume Light\" applies it" 'st "$out" 3 | grep -q "theme=lume-light applied=lume-light"'
expect "Match the system picks Lume Dark or Lume Light" 'st "$out" 4 | grep -Eq "theme=system applied=lume-(dark|light)"'
expect "every theme drew a different screen" '[ "$(cd $OUT && cksum main_*.bmp | cut -d" " -f1 | sort -u | wc -l | tr -d " ")" = 7 ]'
# pixels of a colour in a screenshot: the diagram's fixed dark card and header colours
count() { python3 -I tests/tools/bmp_count.py "$1" "$2"; }
expect "ER diagram: Lume Dark keeps its card colours" '[ "$(count "$OUT/dgm_lume-dark.bmp" 20232A)" -gt 0 ] && [ "$(count "$OUT/dgm_lume-dark.bmp" 272B33)" -gt 0 ]'
expect "ER diagram: Lume Light draws its cards with the theme's colours, none of the dark ones" '[ "$(count "$OUT/dgm_lume-light.bmp" 20232A)" = 0 ] && [ "$(count "$OUT/dgm_lume-light.bmp" 272B33)" = 0 ] && [ "$(count "$OUT/dgm_lume-light.bmp" E4E7EC)" -gt 0 ]'
expect "the choice is saved in settings.txt" 'grep -q "^theme=lume-light$" $P/settings.txt'
out2=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/themes2.txt ./build/lume 2>&1)
expect "next launch: the theme comes back" 'st "$out2" 1 | grep -q "theme=lume-light applied=lume-light"'
sed -i.bak 's/^theme=.*/theme=no-such-theme/' $P/settings.txt
out3=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/themes2.txt ./build/lume 2>&1)
expect "an unknown theme in settings.txt keeps the default (and says so in the log)" 'st "$out3" 1 | grep -q "theme=lume-dark applied=lume-dark" && grep -q "unknown theme" $P/logs/lume.log'
if [ $fails = 0 ]; then echo "THEMES UI: all passed"; else echo "THEMES UI: FAILED ($fails failures)"; fi
exit $fails
