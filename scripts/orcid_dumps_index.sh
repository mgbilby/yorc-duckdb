#!/usr/bin/env bash
# Rebuild reference/orcid-dumps/index.csv: every file of every
# "ORCID Public Data File" release, as published by ORCID on figshare
# (https://orcid.figshare.com/), with size, MD5 and direct download URL.
# The data files are released by ORCID under CC0.
#
#   scripts/orcid_dumps_index.sh          # writes the index in place (any cwd)
#
# Needs: curl, jq, network access to api.figshare.com.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."   # the extension's root
OUT=reference/orcid-dumps/index.csv
API=https://api.figshare.com/v2
TMP=$(mktemp); trap 'rm -f "$TMP"' EXIT

echo 'release,published,doi,landing_url,file_name,kind,size_bytes,md5,download_url,license' > "$TMP"
# figshare answers 422 to field-qualified queries it cannot parse and to pages
# past the end of the result window, so search for the quoted phrase, filter
# titles locally, and stop at the first empty or rejected page.
search() {  # page -> JSON array on stdout; returns 1 on an HTTP error
  local body code
  body=$(curl -sS -w '\n%{http_code}' -X POST "$API/articles/search" \
         -H 'Content-Type: application/json' \
         -d "{\"search_for\": \"\\\"ORCID Public Data File\\\"\", \"page_size\": 100, \"page\": $1}")
  code=${body##*$'\n'}; body=${body%$'\n'*}
  if [ "$code" != 200 ]; then
    echo "figshare search page $1: HTTP $code: $body" >&2
    return 1
  fi
  printf '%s\n' "$body"
}
page=1
while :; do
  if ! hits=$(search "$page"); then
    [ "$page" -gt 1 ] && break   # past the last page
    exit 1
  fi
  n=$(jq 'length' <<<"$hits")
  [ "$n" -eq 0 ] && break
  for id in $(jq -r '.[] | select(.title | test("^ORCID Public Data File"; "i")) | .id' <<<"$hits"); do
    curl -fsS "$API/articles/$id" | jq -r '
      . as $a
      | .files[]
      | [ $a.title,
          ($a.published_date // "" | .[0:10]),
          ($a.doi // ""),
          ($a.url_public_html // ""),
          .name,
          (if (.name | test("summaries"; "i")) then "summaries"
           elif (.name | test("activities"; "i")) then "activities"
           elif (.name | test("lambda|changed|ids"; "i")) then "other"
           else "other" end),
          .size,
          (.computed_md5 // .supplied_md5 // ""),
          .download_url,
          ($a.license.name // "") ]
      | @csv' >> "$TMP"
    sleep 0.2   # stay well under figshare's rate limits
  done
  page=$((page + 1))
done
{ head -1 "$TMP"; tail -n +2 "$TMP" | sort -t, -k2,2r -k5,5; } > "$OUT"
echo "wrote $OUT ($(($(wc -l < "$OUT") - 1)) files)"
[ "$(wc -l < "$OUT")" -gt 1 ] || { echo "no 'ORCID Public Data File' articles found on figshare" >&2; exit 1; }
