#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Headless toolbar checks: capability table and SQL write classification.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Isrc -Itests/shim -o build/toolbar_check tests/toolbar_check.c src/caps.c src/sqllex.c
build/toolbar_check
