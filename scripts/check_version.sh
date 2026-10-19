#!/usr/bin/env bash
# Checks that the release version agrees everywhere it is written:
# description.yml (extension.version), CMakeLists.txt (project VERSION),
# the newest CHANGELOG.md entry and README.md; and, if HEAD is tagged v*,
# that the tag matches. Run from the repository root. Exit 0 if all agree.
set -euo pipefail
cd "$(dirname "$0")/.."
desc=$(sed -n 's/^  version: *\([0-9][0-9.]*\).*/\1/p' description.yml | head -1)
cmake=$(sed -n 's/^project(.*VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt | head -1)
log=$(sed -n 's/^## \[\([0-9][0-9.]*\)\].*/\1/p' CHANGELOG.md | head -1)
readme=$(sed -n 's/.*Current release: \*\*\([0-9][0-9.]*\)\*\*.*/\1/p' README.md | head -1)
echo "description.yml $desc | CMakeLists.txt $cmake | CHANGELOG.md $log | README.md $readme"
status=0
for v in "$cmake" "$log" "$readme"; do
	[ "$v" = "$desc" ] || { echo "MISMATCH: expected $desc everywhere" >&2; status=1; break; }
done
if tag=$(git describe --tags --exact-match HEAD 2>/dev/null); then
	[ "$tag" = "v$desc" ] || { echo "MISMATCH: HEAD is tagged $tag, not v$desc" >&2; status=1; }
	echo "tag $tag"
fi
exit "$status"
