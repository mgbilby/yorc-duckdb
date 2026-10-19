#!/usr/bin/env bash
# Progress-bar check for the file readers (not part of `make test`: it needs a
# terminal for the CLI's progress bar, emulated with script(1), and inputs big
# enough to take a few seconds).
#
#   test/progress_check.sh DUCKDB_CLI PATH_OR_GLOB [SECTION]
#
# Runs `SELECT count(*) FROM orcid_<SECTION>(PATH)` on one thread with the
# progress bar on, collects every percentage printed, and checks that they are
# within [0, 100], never decrease, and reach at least 90.
set -euo pipefail
cli=${1:?DuckDB CLI}
path=${2:?path or glob}
section=${3:-external_ids}
out=$(mktemp)
trap 'rm -f "$out"' EXIT
script -qec "$cli -init /dev/null -c \"SET enable_progress_bar=true; SET progress_bar_time=10; SET threads=1; SELECT count(*) FROM orcid_$section('$path');\"" /dev/null > "$out" 2>&1
values=$(tr '\r' '\n' < "$out" | grep -oE '[0-9]+%' | tr -d % | uniq | tr '\n' ' ')
echo "progress: $values"
prev=-1
for v in $values; do
	if [ "$v" -lt 0 ] || [ "$v" -gt 100 ]; then echo "FAIL: $v outside [0, 100]"; exit 1; fi
	if [ "$v" -lt "$prev" ]; then echo "FAIL: went back from $prev to $v"; exit 1; fi
	prev=$v
done
[ "$prev" -ge 90 ] || { echo "FAIL: last value $prev < 90 (bar stuck?)"; exit 1; }
echo "ok: monotonic, in [0, 100], reached $prev"
