#!/usr/bin/env bash
# Shows how crosswalk mode's output differs from test/data/crosswalk/ for the
# documents orcid_crosswalk.test reports (default: every one that differs).
# Runs the test's own setup statements in the built shell, so it needs a build.
#
#   scripts/crosswalk_diff.sh [STEM.FMT ...]     e.g. bulk-work-3.0.json
#
# Run from yorc-duckdb/. Writes into a temporary directory; prints unified diffs
# (the shell adds one final newline, so a last-line-only difference is noise).
set -euo pipefail
# the shell built with the extension (same DuckDB); not $DUCKDB, which may be another version
duckdb=${YORC_DUCKDB_SHELL:-./build/release/duckdb}
[ -x "$duckdb" ] || { echo "no shell at $duckdb (build first: make release)" >&2; exit 1; }
ext=${YORC_EXTENSION:-./build/release/extension/yorc/yorc.duckdb_extension}
[ -f "$ext" ] || { echo "no extension at $ext (build first: make release)" >&2; exit 1; }
test=test/sql/orcid_crosswalk.test
out=$(mktemp -d)
python3 -I - "$test" "$ext" > "$out/setup.sql" <<'PY'
import sys
lines = open(sys.argv[1]).read().split("\n")
sql, i, cur = [], 0, None
while i < len(lines):
    l = lines[i]
    if l == "statement ok":
        i += 1
        block = []
        while i < len(lines) and lines[i].strip() != "":
            block.append(lines[i]); i += 1
        stmt = "\n".join(block).replace("__TEST_DIR__", "/tmp")
        sql.append(stmt.rstrip(";") + ";")
        if stmt.startswith("INSERT INTO written"):
            break
    elif l.startswith("require "):
        # the built extension, not one from ~/.duckdb/extensions
        sql.append("LOAD '" + sys.argv[2] + "';")
    i += 1
print("\n".join(sql))
PY
cat >> "$out/setup.sql" <<'SQL'
.headers off
.mode list
SELECT e.stem || '.' || e.fmt FROM expected e LEFT JOIN written o USING (stem, fmt)
WHERE o.text IS DISTINCT FROM e.content ORDER BY 1;
SQL
stems=("$@")
if [ ${#stems[@]} -eq 0 ]; then
  "$duckdb" -unsigned -bail < "$out/setup.sql" > "$out/stems" \
    || { echo "the test's setup failed in $duckdb (see above)" >&2; exit 1; }
  mapfile -t stems < "$out/stems"
fi
[ ${#stems[@]} -gt 0 ] || { echo "no differences"; exit 0; }
for s in "${stems[@]}"; do
  stem=${s%.*}; fmt=${s##*.}
  { head -n -2 "$out/setup.sql"
    printf ".once %s\nSELECT coalesce(text, '<NULL>') FROM written WHERE stem = '%s' AND fmt = '%s';\n" \
      "$out/$s" "$stem" "$fmt"; } | "$duckdb" -unsigned -bail > /dev/null
  echo "=== $s (expected vs written)"
  diff -u "test/data/crosswalk/write_samples/$s" "$out/$s" || true
done
echo "files in $out"
