# Function reference

Worked examples by task are in [RECIPES.md](RECIPES.md), reusable macro files in
[TASKS.md](TASKS.md), and which schema element each column carries (and what no
column carries) in [COVERAGE.md](COVERAGE.md).

The extension reads and writes ORCID v3.0 documents in both of ORCID's
formats, XML and JSON. Reading is below; writing (the scalar writers) is in
[Writing ORCID documents](#writing-orcid-documents).

yorc-duckdb is an independent open-source project, not affiliated with,
endorsed by or sponsored by ORCID, Inc. It does not provide access to the
ORCID Sandbox or the ORCID Member API, and makes no requests to ORCID: it
reads ORCID documents and Public Data File archives you already have.

All table functions come in three forms with identical columns:

- `orcid_<section>(path)`: reads ORCID v3.0 files, XML or JSON. The format is told per file and per archive member by its first byte that is not whitespace (after an optional UTF-8 byte order mark): `{` or `[` is JSON, anything else XML, so a glob or an archive may mix the two. Paths go through DuckDB's file system (local paths, `~`, globs, and with httpfs `https://`/`s3://`). A missing file or a glob matching nothing is an error. On DuckDB v2.0 this is a multi-file reader (`LIST(VARCHAR)` paths, `filename`/`file_index` columns, parallel across files; see [DUCKDB2.md](DUCKDB2.md)).
- `orcid_<section>_xml(xml)`: parses XML strings, including a column through a lateral join: `SELECT w.* FROM records r, orcid_works_xml(r.xml) w`. `NULL` yields no rows, so `CREATE TABLE t AS SELECT * FROM orcid_works_xml(NULL)` creates an empty table with the right schema.
- `orcid_<section>_json(json)`: the same for JSON strings (same in-out protocol, same constant-argument form, same `NULL` behaviour). Each twin reads its own format only: a column holds one format, and `orcid_works_json('<work/>')` is a JSON error, not a guess.

Every section accepts any ORCID v3.0 document that contains its items: a full `/record`, a section collection (`/works`, `/employments`, ...), a single item (`/work/{put-code}`), or a `bulk` response, in either format. JSON is mapped onto the same element tree as the XML ([JSON documents](#json-documents)), so the same document yields the same rows in both. Columns come from [the ORCID message schema](../reference/orcid-model/PROVENANCE.md); the extraction tables live in `src/core/sections.cpp`.

Text is whitespace-trimmed; empty or missing values (and JSON `null`) are NULL; numbers that are not plain digits are NULL; when an element repeats, the first one wins, except in list columns, which join every value with `; `. Malformed XML raises `orcid: malformed XML in <file or input row>: <reason>`, malformed JSON `orcid: malformed JSON in <file or input row>: <reason> at byte <n>`. Both formats must be UTF-8 (ORCID's only encoding): a document that is not, such as a Latin-1 file, is malformed (`orcid: malformed XML in 'x.xml': invalid UTF-8 at byte 190`), whatever its XML declaration says, so no invalid text reaches a `VARCHAR` column. A character reference that names no Unicode character (`&#xD800;`, `&#x110000;`, `&#0;`) is kept as literal text.

**Column positions are stable.** A column added to a section goes after the existing ones (as `peer_reviews.subject_translated_name_language` did), so `SELECT *` gains columns at the end and the earlier ones keep their positions. The columns appended for the context gaps of the schema (the assertion origin outside `works`, normalized ids outside `external_ids`, the normalization error code, a contributor's host, the record's `orcid-identifier` URI and host, a notification item's URL, relationship and normalized id) are listed per section in [COVERAGE.md](COVERAGE.md#columns-added-for-the-context-gaps). A query that inserts `SELECT *` into an existing table, or unions it with an older result, needs the new columns (or an explicit column list).

## Memory of lateral joins

`FROM stage s, orcid_works_xml(s.xml) w` is planned by DuckDB (v1.5.6) as a
duplicate-eliminated join whose inner hash join is keyed on the correlated
column: the whole document. When the outer side has **exactly one row**, the
estimate for that side rounds to 0, the function's output becomes the hash
join's build side, and every output row stores its own copy of the document.
Memory is then document size x output rows: 7.5 GB and 6-9 s for one 5.4 MB
record with 1,362 works, 500 MB for the 1.3 MB test record with 382 works.
With two or more outer rows the outer side is built instead and memory stays
O(documents + chunk) (5,000 summaries in batches of 1,000: 1.2-1.9 GB peak,
most of it the staged XML). Threads, `preserve_insertion_order`, the projected
columns, `w.*` with `REPLACE` or explicit columns, `ANALYZE`, a `MATERIALIZED`
CTE and `LATERAL (SELECT ...)` make no difference; it is not the extension's
state (one document's rows, freed per input row).

The `_json` twins are planned the same way. For a one-row input, pass the
document as a constant argument. It is not decorrelated, so it never meets
the join:

```sql
-- one staged document: 0.05 s, 46 MB peak for the 5.4 MB record
INSERT INTO works BY NAME
  SELECT w.* REPLACE (s.orcid AS orcid, s.source_hash AS source_hash)
  FROM stage s, orcid_works_xml((SELECT xml FROM stage)) w;
```

DuckDB v2.0 (since extension 0.2.0) makes it the
same way, with the in-out side estimated at 0 rows and built: 7.2 GB and 8.6 s
for the 5.4 MB record. A second staged row brings that to 92 MB and 0.17 s, and
the constant form below takes 0.07 s and 74 MB.

It gives the same rows as the lateral form for zero or one staged row (zero
rows when the stage is empty or `xml` is NULL) and fails with "More than one
row returned by a subquery" for more, so it cannot mix documents. Regression
test: `test/sql/orcid_lateral_memory.test` (64 MB `memory_limit`).

**Row order.** The same plan also changes the order: when the function's output
is the build side (one outer row), the rows come back in reverse document order
(seen for `research-resource-3.0.xml`'s five external ids and `bulk-work-3.0.xml`'s
two works). The file form, a string literal and the constant-argument form keep
document order. A query that needs one document's rows in document order (to
rebuild a list, for example) reads it with the constant-argument form;
`test/sql/orcid_crosswalk.test` does.

## Reading ORCID Public Data File archives

The file readers (`orcid_<section>(path)`) also read `.tar`, `.tar.gz` and `.tgz`
archives, streaming them member by member: nothing is unpacked to disk, and
reading stops as soon as the query has what it needs. With the `httpfs`
extension the path can be an `https://` URL, so a dump on figshare can be queried
in place; only the bytes up to the last member read are downloaded.

| named parameter | default | meaning |
|---|---|---|
| `members` | `*.xml` and `*.json` | glob over archive member paths (`*` also matches `/`, `?` one character); by default every `.xml` and `.json` member |
| `max_records` | unlimited | stop after this many matching members per file (a plain XML file counts as one) |
| `json_root` | inferred | the document element of JSON documents whose content does not name it: `'employment'` (or another affiliation type) for an affiliation, `'keyword'` or `'other-name'` for `{"content": ...}`. Ignored for XML. See [JSON documents](#json-documents) |
| `max_member_bytes` | 67108864 (64 MiB) | largest archive member read. A matching member over it fails the scan, before its content is read, with `orcid: member '<path>' in '<archive>' is <n> bytes, over max_member_bytes (<limit>)`. A member is held in memory whole while it is parsed, outside `memory_limit`, so this bounds what a corrupt or crafted archive (a gzip bomb) can allocate. The largest corpus record is 5.4 MB. Plain XML files are not limited |

```sql
INSTALL httpfs; LOAD httpfs;
-- the first 1,000 records of a summaries dump (URL from reference/orcid-dumps/index.csv)
SELECT orcid, title, pub_year
FROM orcid_works('https://ndownloader.figshare.com/files/<file-id>', max_records := 1000);

-- one iD directory of a dump (dumps group records by the iD's last three characters)
SELECT * FROM orcid_person('ORCID_2025_10_summaries.tar.gz', members := '*/097/*');
```

gzip has no index, so reaching a member means decompressing everything before
it: `members` filters what is parsed, not what is read. Put `max_records` on
exploratory queries over full dumps. A truncated or non-tar file raises
`orcid: truncated tar archive` / `bad tar header checksum`.

`reference/orcid-dumps/index.csv` lists every file of every ORCID Public Data File
release (CC0) with size, MD5 and download URL; regenerate it with
`scripts/orcid_dumps_index.sh`:

```sql
SELECT release, file_name, size_bytes, download_url
FROM read_csv('reference/orcid-dumps/index.csv') WHERE kind = 'summaries' ORDER BY published DESC;
```

## `orcid_documents`: raw documents

`orcid_documents(path)` returns one row per document, XML or JSON: per file, or per
archive member for `.tar` / `.tar.gz` / `.tgz`. Globs, `members`,
`max_records` and `max_member_bytes` work as for the section readers. Members are read in archive
order, sequentially.

| # | column | type | value |
|---|---|---|---|
| 1 | `file` | VARCHAR | the file (or archive) path |
| 2 | `member` | VARCHAR | the archive member path; NULL for a plain file |
| 3 | `name` | VARCHAR | the last path component of the member or file |
| 4 | `orcid` | VARCHAR | the first valid iD (`dddd-dddd-dddd-dddX`, check digit verified) in the member or file path, else NULL |
| 5 | `xml` | VARCHAR | the document's text as read, in either format (the name predates JSON); a document (or member name) that is not UTF-8 is an error, `orcid_documents: invalid UTF-8 at byte <n> of <member or file>` |
| 6 | `format` | VARCHAR | `xml` or `json`, by the first byte that is not whitespace (as the readers tell it) |

Use it to pair records across two dump releases and compare them:

```sql
WITH pairs AS (
  SELECT orcid, a.xml AS old_xml, b.xml AS new_xml
  FROM orcid_documents('ORCID_2024_10_summaries.tar.gz', members := '*/097/*') a
  FULL JOIN orcid_documents('ORCID_2025_10_summaries.tar.gz', members := '*/097/*') b USING (orcid))
SELECT c.* FROM pairs p, orcid_changes_xml(p.old_xml, p.new_xml) c;
```

A record that is only in one release yields only added or removed items.
Removing a work group shifts the `group_index` of the groups after it, which
shows up as a `modified` row (see `test/sql/orcid_docs_examples.test`).

## `orcid_dump_sample`: a seeded sample of a dump

`orcid_dump_sample(archive, n, seed)` draws a uniform sample of `n` records
from a Public Data File archive (`.tar`, `.tar.gz`, `.tgz`, or a glob of them,
read as one stream in path order), in one pass. It picks exactly the records
that yorc-rs's `dump::Reservoir` and yorc-zig's `reservoir.Reservoir` pick
from the same archive, `n` and seed, so a sample drawn in SQL can be
reproduced with either library and the other way round:

- **Eligible members:** regular files whose last path component is longer than
  four characters, ends in `.xml` (any case), and whose stem is an ORCID iD
  by `orcid_normalize` (check character verified). Everything else (directories,
  `README.txt`, `works` documents under other names) is not offered and does
  not consume a random draw. A member name that is not UTF-8 is an error only
  when that member is kept.
- **The draw:** SplitMix64 seeded with `seed`; the k-th eligible member (from 0)
  is kept while k < n; after that one draw `j = below(k + 1)`, Lemire's
  multiply-shift `(next() * (k + 1)) >> 64` with no rejection step, and the
  member replaces slot `j` if `j < n`. `n = 0` keeps nothing.
- **Reading:** a member is read only while it is in the sample, and only when
  the `xml` column is selected; skipped members are never read (gzip still
  decompresses past them). `max_member_bytes` (default 64 MiB) applies to the
  kept members read.

| # | column | type | value |
|---|---|---|---|
| 1 | `slot` | BIGINT | the reservoir slot, from 0; rows come in slot order, as yorc-rs's `into_items()` |
| 2 | `orcid` | VARCHAR | the canonical iD of the member's stem |
| 3 | `file` | VARCHAR | the archive path |
| 4 | `member` | VARCHAR | the member path in the archive |
| 5 | `name` | VARCHAR | its last path component |
| 6 | `xml` | VARCHAR | the member's document (read only if selected) |
| 7 | `eligible` | BIGINT | eligible members seen in the whole pass (the same on every row; yorc-rs's `seen()`) |

`n` is a BIGINT (at least 0), `seed` a UBIGINT (0 to 2^64 - 1). The pass is
sequential: one archive is one gzip stream.

```sql
-- the walkthroughs' sample (cell 8): seed 42 keeps 0000-0003-0100-6634
SELECT slot, orcid, eligible FROM orcid_dump_sample('test/data/dump/orcid_test_summaries.tar.gz', 1, 42);

-- 1,000 records of a summaries dump, and their works
SELECT w.* FROM orcid_dump_sample('ORCID_2025_10_summaries.tar.gz', 1000, 7) s, orcid_works_xml(s.xml) w;
```

To sample rows of any query instead, DuckDB's own `USING SAMPLE reservoir(n
ROWS) REPEATABLE (seed)` works on one thread, with different picks.

## JSON documents

ORCID's JSON is Jackson's serialization of the same Java model as its XML, so
its member names are the XML local names. The reader parses the JSON
(`src/core/json.cpp`: strict RFC 8259, UTF-8 checked, no dependency) and maps
it onto the element tree of the equivalent XML document
(`src/core/json_map.cpp`); the section extractor then reads both alike. The
rules were derived by pairing the JSON and XML samples of `reference/orcid-model`
(`record_3.0/samples/write_samples`, and `read_samples/record-full-3.0.json`
against `record-3.0.xml`) and are checked by `test/sql/orcid_json.test`:

| JSON | element tree (XML) |
|---|---|
| member `"k": {...}` | child element `k` |
| member `"k": "text"`, a number or a boolean | leaf element `k` with that text (numbers as written, `true` / `false`) |
| `"k": [a, b]` | `k` repeated, once per entry (arrays of strings too: expanded search's `other-name`, `email`, `institution-name`) |
| `"k": null`, absent | no element |
| `{"value": v}`, with at most `language-code`, `currency-code`, `transient`, `visibility` beside it | leaf element with text `v` and those attributes (`title`, `url`, `source-name`, `credit-name`, `country`, `amount`, `translated-title`, `external-id-normalized`, dates) |
| scalar `put-code`, `path`, `visibility`, `display-index`, `verified`, `primary`, `num-found`, `transient` | attribute of the element; `put-code` is also kept as a child element (a record summary's items carry it as one) |
| `path` in an identifier object (`{"uri", "path", "host"}`: `orcid-identifier`, `source-client-id`, `contributor-orcid`, ...) | child element `path` |
| `lang` | attribute `xml:lang` (notification-custom) |
| `"summaries": [{"employment-summary": {...}}, ...]` (in `affiliation-group`), `"bulk": [{"work": {...}}, {"error": {...}}]` | the wrapped elements, as children of the parent, in order |
| `"resource-item": [...]` directly in a research resource | inside a `resource-items` element, as in the XML |
| `{"value": 1540896558250}`, or the bare number `1540896558250`, in a member named `*-date` | ISO 8601 UTC with milliseconds, as ORCID's XML writes it: `2018-10-30T10:49:18.250Z` |
| `history`'s `"creation-method"`: ORCID's Java enum names `API`, `DIRECT`, `MEMBER_REFERRED`, `WEBSITE` | the XSD values `API`, `Direct`, `Member-referred`, `website`; any other text is kept as it is (yorc-rs's `creation_method_from_json`) |
| the members of `record`, `person`, `personal-details`, `record-summary` and `research-resource`, in any key order (yorc-rs and yorc-zig write sorted keys) | children in the XSD's sequence order, so that `person_items` (and `timeline`, `summary_items`) come out in the XML's order: other names, researcher URLs, emails, addresses, keywords, external identifiers. Items of one kind keep their array order (`kChildOrders` in `src/core/json_map.cpp`) |

**The document element.** A JSON document has no root name, while the item
patterns of the sections need one (`^record/person`, a single `work`, an
`error`). It is taken, in order, from: `json_root` when given; a `bulk` member
(`bulk`); the document's `path` (`"/<iD>"` is a record, `"/<iD>/work/123"` a
work, `"/<iD>/employments"` the employments collection, `"/<iD>/other-names/4923"`
an other name); and otherwise from the members it has (`orcid-identifier` with
`activities-summary` / `person` / `history` is a record, `reviewer-role` a peer
review, `proposal` a research resource, an `organization` with a `title` or a
`type` a funding, a `title` with a `type` a work, `group-id` a group-id
record, `response-code` an error, `num-found` a search, and so on:
`InferJsonRoot` in `src/core/json_map.cpp`). Two shapes cannot be told
apart, because ORCID's JSON leaves the item type to the URL it was posted to
or fetched from: an affiliation without a `path` (the same members for all
seven types), and `{"content": ...}` (an other name or a keyword). Those raise
`orcid: cannot read the JSON in ...: ... pass json_root := 'employment' (or
...)`; pass `json_root`. A JSON document that is no ORCID document gives no
rows. A top-level array is a sequence of documents.

**Where ORCID has no JSON.** The ORCID Public Data File (the yearly dumps) is
XML only. Every v3.0 API message has a JSON form, but `reference/orcid-model`
has JSON samples for the write samples' items, the full record and the group id only:
the record summary, search, error, bulk-error and notification readers follow
the same rules, and `test/sql/orcid_json.test` checks them on the XML samples
written out in ORCID's JSON conventions, not on ORCID's own JSON.

**Metadata columns.** `source_hash` is the SHA-256 of the bytes read (the
file, archive member or string, in whichever format), so it changes when the
document's bytes change; the XML and the JSON of one item hash differently,
and so do two JSON texts that differ only in whitespace. `orcid` is taken
from the root `path` (`"/0000-0002-7319-2192"`) or `orcid-identifier.path`, as
in XML. `item`, `kind` and `affiliation_type` are the element names above (for
a JSON root: the inferred or given document element). `orcid_documents` has a
`format` column.

## Parallel scans and row order

The file readers spread the work over DuckDB's threads (the `threads`
setting). On DuckDB v1.5.x the reader parallelizes the work itself: a shared
scan state hands out batches of consecutive documents, each thread parses its
batch, and every output chunk comes from one batch.
- **Plain files:** a glob's files are read and parsed in parallel. A batch is
  at most 32 files, and fewer for small globs, so that every thread still gets
  work.
- **Archive members:** the tar stream itself is sequential, so members are
  read and decompressed one after another under a lock. Parsing them runs on
  all threads. A batch is at most 32 members or 4 MiB.

**Row order.** Within a document, rows are in document order. Across
documents the order is files in sorted path order, then members in archive
order. That order is guaranteed when `preserve_insertion_order` is on (the
DuckDB default): each batch carries its sequence number as the batch index, so
an ordered result or an insert restores it however many threads ran. With
`preserve_insertion_order = false`, rows of different documents can arrive in
any order. Use `ORDER BY` when the order matters to the query itself.

**`max_records`** is counted when a member is handed out, per file, so it
selects exactly the same members on any number of threads.

Measured on the committed corpus (`corpus/`: 5,000 summaries plus 5,000
activity files, about 450 MB unpacked), 4-core container, best of 3, seconds.
"before" is the sequential reader of the previous release.

| query (DuckDB v1.5.6) | threads=1 before | threads=1 after | threads=4 before | threads=4 after |
|---|---|---|---|---|
| `CREATE TABLE AS SELECT * FROM orcid_works('orcid-corpus.tar.gz')` (75,192 rows) | 3.68 | 3.90 | 3.67 | 1.25 |
| same, over the extracted directory (`*/*.xml`, 10,000 files) | 3.47 | 3.53 | 3.68 | 1.85 |
| `orcid_external_ids` over the directory (253,717 rows) | 3.32 | 3.50 | 3.47 | 1.56 |
| `orcid_contributors(…tar.gz, members := 'activities/*')` (194,037 rows) | 1.22 | 1.24 | 1.22 | 0.81 |
| `SELECT count(*) FROM orcid_person('orcid-corpus.tar.gz')` | 3.07 | 3.11 | 3.18 | 1.20 |

On v2.0 the multi-file framework schedules files across threads; see
[DUCKDB2.md](DUCKDB2.md).

**Progress.** The file readers report progress, so DuckDB's progress bar moves
(`SET enable_progress_bar = true`; the CLI shows it after `progress_bar_time`,
2 s by default).
- **Every file weighs the same.** A plain file counts as done when a thread
  takes it.
- **An archive counts by the share of its compressed bytes consumed.** Its
  size is looked up once.
  - For `.tar.gz`, the position of the underlying file is read at most once per
    MiB of decompressed data.
  - For a plain `.tar`, the bytes read are counted directly.
  - An archive whose size or position is unknown (a stream) counts as a whole
    file when it is finished.
- **The value never decreases and stays within [0, 100].** On v2.0 the same
  callback reports progress within each file.

The readers also give DuckDB a row estimate: about one row per 2 KB of archive,
or 50 rows per plain file, capped by `max_records`. Without it the scan
counted as little as the tiny pipelines after it, and the bar stopped near 33%.
`test/progress_check.sh` checks that the bar's values are monotonic, stay in
[0, 100], and reach at least 90.
- On the committed corpus it printed 22 → 99 for `orcid-corpus.tar.gz`,
  16 → 99 for the extracted directory, and 11 → 99 for a glob of one
  `.tar.gz` and one `.tar`.
- The overhead is within measurement noise: 3.83 s before and 3.80 s after,
  on one thread, for all works of the corpus archive.

The `_xml` functions are in-out functions, so DuckDB already runs them in
parallel over their input chunks; their protocol is unchanged.

## Scalars

- `orcid_valid(id VARCHAR) -> BOOLEAN`: ISO 7064 MOD 11-2 check. Accepts the inputs `orcid_normalize` accepts; NULL in, NULL out.
- `orcid_normalize(id VARCHAR) -> VARCHAR`: canonical `XXXX-XXXX-XXXX-XXXX`, or NULL. Accepts leading whitespace, then an optional `https://orcid.org/`, `http://orcid.org/` or `orcid.org/` prefix; after it, hyphens and whitespace are ignored wherever they are (so missing, extra or space separators are fine), and a lowercase `x` is accepted. What remains must be 15 digits and the check character.

## `orcid_works`

Works: `work-summary` (record, `/works`) and full `work` (`/work/{put-code}`, bulk).

Items: `work-summary`, `work` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD: root `path` attribute, else `orcid-identifier/path`, else the item `path` attribute |
| 2 | `put_code` | BIGINT | `@put-code` |
| 3 | `group_index` | INTEGER | 1-based position of the enclosing `group` / `affiliation-group` |
| 4 | `display_index` | INTEGER | `@display-index` |
| 5 | `title` | VARCHAR | `title/title` |
| 6 | `subtitle` | VARCHAR | `title/subtitle` |
| 7 | `journal` | VARCHAR | `journal-title` |
| 8 | `type` | VARCHAR | `type` |
| 9 | `pub_year` | INTEGER | `publication-date/year` |
| 10 | `pub_month` | INTEGER | `publication-date/month` |
| 11 | `pub_day` | INTEGER | `publication-date/day` |
| 12 | `doi` | VARCHAR | first item-level external id with type `doi` and relationship `self`, ASCII-lowercased |
| 13 | `url` | VARCHAR | `url` |
| 14 | `source_name` | VARCHAR | `source/source-name` |
| 15 | `visibility` | VARCHAR | `@visibility` |
| 16 | `created` | VARCHAR | `created-date` |
| 17 | `last_modified` | VARCHAR | `last-modified-date` |
| 18 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |
| 19 | `translated_title` | VARCHAR | `title/translated-title` |
| 20 | `translated_title_language` | VARCHAR | `title/translated-title@language-code` |
| 21 | `short_description` | VARCHAR | `short-description` |
| 22 | `citation_type` | VARCHAR | `citation/citation-type` |
| 23 | `citation_value` | VARCHAR | `citation/citation-value` |
| 24 | `language_code` | VARCHAR | `language-code` |
| 25 | `country` | VARCHAR | `country` |
| 26 | `path` | VARCHAR | `@path` |
| 27 | `source_client_id` | VARCHAR | `source/source-client-id/path` |
| 28 | `source_orcid` | VARCHAR | `source/source-orcid/path` |
| 29 | `assertion_origin_name` | VARCHAR | `source/assertion-origin-name` |
| 30 | `assertion_origin_orcid` | VARCHAR | `source/assertion-origin-orcid/path` |
| 31 | `assertion_origin_client_id` | VARCHAR | `source/assertion-origin-client-id/path` |
| 32 | `item` | VARCHAR | item element local name |

## `orcid_fundings`

Fundings: `funding-summary` and full `funding`.

Items: `funding-summary`, `funding` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD: root `path` attribute, else `orcid-identifier/path`, else the item `path` attribute |
| 2 | `put_code` | BIGINT | `@put-code` |
| 3 | `group_index` | INTEGER | 1-based position of the enclosing `group` / `affiliation-group` |
| 4 | `display_index` | INTEGER | `@display-index` |
| 5 | `title` | VARCHAR | `title/title` |
| 6 | `translated_title` | VARCHAR | `title/translated-title` |
| 7 | `translated_title_language` | VARCHAR | `title/translated-title@language-code` |
| 8 | `type` | VARCHAR | `type` |
| 9 | `organization_defined_type` | VARCHAR | `organization-defined-type` |
| 10 | `short_description` | VARCHAR | `short-description` |
| 11 | `amount` | VARCHAR | `amount` |
| 12 | `currency` | VARCHAR | `amount@currency-code` |
| 13 | `start_year` | INTEGER | `start-date/year` |
| 14 | `start_month` | INTEGER | `start-date/month` |
| 15 | `start_day` | INTEGER | `start-date/day` |
| 16 | `end_year` | INTEGER | `end-date/year` |
| 17 | `end_month` | INTEGER | `end-date/month` |
| 18 | `end_day` | INTEGER | `end-date/day` |
| 19 | `org_name` | VARCHAR | `organization/name` |
| 20 | `org_city` | VARCHAR | `organization/address/city` |
| 21 | `org_region` | VARCHAR | `organization/address/region` |
| 22 | `org_country` | VARCHAR | `organization/address/country` |
| 23 | `org_disambiguated_id` | VARCHAR | `organization/disambiguated-organization/disambiguated-organization-identifier` |
| 24 | `org_disambiguation_source` | VARCHAR | `organization/disambiguated-organization/disambiguation-source` |
| 25 | `url` | VARCHAR | `url` |
| 26 | `visibility` | VARCHAR | `@visibility` |
| 27 | `path` | VARCHAR | `@path` |
| 28 | `source_name` | VARCHAR | `source/source-name` |
| 29 | `source_client_id` | VARCHAR | `source/source-client-id/path` |
| 30 | `source_orcid` | VARCHAR | `source/source-orcid/path` |
| 31 | `assertion_origin_name` | VARCHAR | `source/assertion-origin-name` |
| 32 | `created` | VARCHAR | `created-date` |
| 33 | `last_modified` | VARCHAR | `last-modified-date` |
| 34 | `item` | VARCHAR | item element local name |
| 35 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |
| 36 | `assertion_origin_orcid` | VARCHAR | `source/assertion-origin-orcid/path` (appended, CROSSCHECK #10) |
| 37 | `assertion_origin_client_id` | VARCHAR | `source/assertion-origin-client-id/path` (appended, CROSSCHECK #10) |

## `orcid_affiliations`

All seven affiliation types (distinction, education, employment, invited position, membership, qualification, service), summary and full.

Items: `distinction-summary`, `distinction`, `education-summary`, `education`, `employment-summary`, `employment`, `invited-position-summary`, `invited-position`, `membership-summary`, `membership`, `qualification-summary`, `qualification`, `service-summary`, `service` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD: root `path` attribute, else `orcid-identifier/path`, else the item `path` attribute |
| 2 | `affiliation_type` | VARCHAR | item element local name without `-summary` |
| 3 | `put_code` | BIGINT | `@put-code` |
| 4 | `group_index` | INTEGER | 1-based position of the enclosing `group` / `affiliation-group` |
| 5 | `display_index` | INTEGER | `@display-index` |
| 6 | `department` | VARCHAR | `department-name` |
| 7 | `role_title` | VARCHAR | `role-title` |
| 8 | `start_year` | INTEGER | `start-date/year` |
| 9 | `start_month` | INTEGER | `start-date/month` |
| 10 | `start_day` | INTEGER | `start-date/day` |
| 11 | `end_year` | INTEGER | `end-date/year` |
| 12 | `end_month` | INTEGER | `end-date/month` |
| 13 | `end_day` | INTEGER | `end-date/day` |
| 14 | `org_name` | VARCHAR | `organization/name` |
| 15 | `org_city` | VARCHAR | `organization/address/city` |
| 16 | `org_region` | VARCHAR | `organization/address/region` |
| 17 | `org_country` | VARCHAR | `organization/address/country` |
| 18 | `org_disambiguated_id` | VARCHAR | `organization/disambiguated-organization/disambiguated-organization-identifier` |
| 19 | `org_disambiguation_source` | VARCHAR | `organization/disambiguated-organization/disambiguation-source` |
| 20 | `url` | VARCHAR | `url` |
| 21 | `visibility` | VARCHAR | `@visibility` |
| 22 | `path` | VARCHAR | `@path` |
| 23 | `source_name` | VARCHAR | `source/source-name` |
| 24 | `source_client_id` | VARCHAR | `source/source-client-id/path` |
| 25 | `source_orcid` | VARCHAR | `source/source-orcid/path` |
| 26 | `assertion_origin_name` | VARCHAR | `source/assertion-origin-name` |
| 27 | `created` | VARCHAR | `created-date` |
| 28 | `last_modified` | VARCHAR | `last-modified-date` |
| 29 | `item` | VARCHAR | item element local name |
| 30 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |
| 31 | `assertion_origin_orcid` | VARCHAR | `source/assertion-origin-orcid/path` (appended, CROSSCHECK #10) |
| 32 | `assertion_origin_client_id` | VARCHAR | `source/assertion-origin-client-id/path` (appended, CROSSCHECK #10) |

## `orcid_peer_reviews`

Peer reviews: `peer-review-summary` and full `peer-review` (completion date from either `completion-date` or `review-completion-date`).

Items: `peer-review-summary`, `peer-review` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD: root `path` attribute, else `orcid-identifier/path`, else the item `path` attribute |
| 2 | `put_code` | BIGINT | `@put-code` |
| 3 | `group_index` | INTEGER | 1-based position of the enclosing `group` / `affiliation-group` |
| 4 | `display_index` | INTEGER | `@display-index` |
| 5 | `reviewer_role` | VARCHAR | `reviewer-role` |
| 6 | `review_type` | VARCHAR | `review-type` |
| 7 | `review_url` | VARCHAR | `review-url` |
| 8 | `completion_year` | INTEGER | `completion-date/year` or `review-completion-date/year` |
| 9 | `completion_month` | INTEGER | `completion-date/month` or `review-completion-date/month` |
| 10 | `completion_day` | INTEGER | `completion-date/day` or `review-completion-date/day` |
| 11 | `review_group_id` | VARCHAR | `review-group-id` |
| 12 | `subject_type` | VARCHAR | `subject-type` |
| 13 | `subject_name` | VARCHAR | `subject-name/title` |
| 14 | `subject_subtitle` | VARCHAR | `subject-name/subtitle` |
| 15 | `subject_translated_name` | VARCHAR | `subject-name/translated-title` |
| 16 | `subject_container_name` | VARCHAR | `subject-container-name` |
| 17 | `subject_url` | VARCHAR | `subject-url` |
| 18 | `subject_id_type` | VARCHAR | `subject-external-identifier/external-id-type` |
| 19 | `subject_id_value` | VARCHAR | `subject-external-identifier/external-id-value` |
| 20 | `convening_org_name` | VARCHAR | `convening-organization/name` |
| 21 | `convening_org_city` | VARCHAR | `convening-organization/address/city` |
| 22 | `convening_org_region` | VARCHAR | `convening-organization/address/region` |
| 23 | `convening_org_country` | VARCHAR | `convening-organization/address/country` |
| 24 | `convening_org_disambiguated_id` | VARCHAR | `convening-organization/disambiguated-organization/disambiguated-organization-identifier` |
| 25 | `convening_org_disambiguation_source` | VARCHAR | `convening-organization/disambiguated-organization/disambiguation-source` |
| 26 | `visibility` | VARCHAR | `@visibility` |
| 27 | `path` | VARCHAR | `@path` |
| 28 | `source_name` | VARCHAR | `source/source-name` |
| 29 | `source_client_id` | VARCHAR | `source/source-client-id/path` |
| 30 | `source_orcid` | VARCHAR | `source/source-orcid/path` |
| 31 | `assertion_origin_name` | VARCHAR | `source/assertion-origin-name` |
| 32 | `created` | VARCHAR | `created-date` |
| 33 | `last_modified` | VARCHAR | `last-modified-date` |
| 34 | `item` | VARCHAR | item element local name |
| 35 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |
| 36 | `subject_translated_name_language` | VARCHAR | `subject-name/translated-title@language-code` (added after `source_hash`, with the writers, which need it) |
| 37 | `assertion_origin_orcid` | VARCHAR | `source/assertion-origin-orcid/path` (appended, CROSSCHECK #10) |
| 38 | `assertion_origin_client_id` | VARCHAR | `source/assertion-origin-client-id/path` (appended, CROSSCHECK #10) |

## `orcid_research_resources`

Research resources: `research-resource-summary` and full `research-resource`. The `host_*` columns hold the first host (the first non-empty value of each column among the proposal's hosts), kept for compatibility; every host is a row of [`orcid_research_resource_hosts`](#orcid_research_resource_hosts).

Items: `research-resource-summary`, `research-resource` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD: root `path` attribute, else `orcid-identifier/path`, else the item `path` attribute |
| 2 | `put_code` | BIGINT | `@put-code` |
| 3 | `group_index` | INTEGER | 1-based position of the enclosing `group` / `affiliation-group` |
| 4 | `display_index` | INTEGER | `@display-index` |
| 5 | `title` | VARCHAR | `proposal/title/title` |
| 6 | `translated_title` | VARCHAR | `proposal/title/translated-title` |
| 7 | `translated_title_language` | VARCHAR | `proposal/title/translated-title@language-code` |
| 8 | `host_name` | VARCHAR | `proposal/hosts/organization/name` |
| 9 | `host_city` | VARCHAR | `proposal/hosts/organization/address/city` |
| 10 | `host_region` | VARCHAR | `proposal/hosts/organization/address/region` |
| 11 | `host_country` | VARCHAR | `proposal/hosts/organization/address/country` |
| 12 | `host_disambiguated_id` | VARCHAR | `proposal/hosts/organization/disambiguated-organization/disambiguated-organization-identifier` |
| 13 | `host_disambiguation_source` | VARCHAR | `proposal/hosts/organization/disambiguated-organization/disambiguation-source` |
| 14 | `start_year` | INTEGER | `proposal/start-date/year` |
| 15 | `start_month` | INTEGER | `proposal/start-date/month` |
| 16 | `start_day` | INTEGER | `proposal/start-date/day` |
| 17 | `end_year` | INTEGER | `proposal/end-date/year` |
| 18 | `end_month` | INTEGER | `proposal/end-date/month` |
| 19 | `end_day` | INTEGER | `proposal/end-date/day` |
| 20 | `url` | VARCHAR | `proposal/url` |
| 21 | `visibility` | VARCHAR | `@visibility` |
| 22 | `path` | VARCHAR | `@path` |
| 23 | `source_name` | VARCHAR | `source/source-name` |
| 24 | `source_client_id` | VARCHAR | `source/source-client-id/path` |
| 25 | `source_orcid` | VARCHAR | `source/source-orcid/path` |
| 26 | `assertion_origin_name` | VARCHAR | `source/assertion-origin-name` |
| 27 | `created` | VARCHAR | `created-date` |
| 28 | `last_modified` | VARCHAR | `last-modified-date` |
| 29 | `item` | VARCHAR | item element local name |
| 30 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |
| 31 | `assertion_origin_orcid` | VARCHAR | `source/assertion-origin-orcid/path` (appended, CROSSCHECK #10) |
| 32 | `assertion_origin_client_id` | VARCHAR | `source/assertion-origin-client-id/path` (appended, CROSSCHECK #10) |

## `orcid_research_resource_items`

One row per `resource-item` of a full research resource. The `host_*` columns hold the item's first host (first non-empty value per column), kept for compatibility; every host is in `orcid_research_resource_hosts`.

Items: `resource-items/resource-item` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD: root `path` attribute, else `orcid-identifier/path`, else the item `path` attribute |
| 2 | `owner_put_code` | BIGINT | that ancestor's `put-code` |
| 3 | `ordinal` | INTEGER | 1-based position among same-name siblings |
| 4 | `resource_name` | VARCHAR | `resource-name` |
| 5 | `resource_type` | VARCHAR | `resource-type` |
| 6 | `host_name` | VARCHAR | `hosts/organization/name` |
| 7 | `host_city` | VARCHAR | `hosts/organization/address/city` |
| 8 | `host_region` | VARCHAR | `hosts/organization/address/region` |
| 9 | `host_country` | VARCHAR | `hosts/organization/address/country` |
| 10 | `host_disambiguated_id` | VARCHAR | `hosts/organization/disambiguated-organization/disambiguated-organization-identifier` |
| 11 | `host_disambiguation_source` | VARCHAR | `hosts/organization/disambiguated-organization/disambiguation-source` |
| 12 | `url` | VARCHAR | `url` |
| 13 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |

## `orcid_research_resource_hosts`

One row per host organization of a research resource: of its proposal (summary or full) and of each resource item. research-resource-3.0.xsd allows 1 to 10 hosts in each `hosts`; the first-host columns of `orcid_research_resources` and `orcid_research_resource_items` keep only the first (CROSSCHECK #9). The hosts of an item join its `orcid_research_resource_items` row on `owner_put_code` and `item_ordinal` = `ordinal`.

Items: `hosts/organization` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD: root `path` attribute, else `orcid-identifier/path`, else the item `path` attribute |
| 2 | `owner_put_code` | BIGINT | the research resource's `put-code` (nearest ancestor with one) |
| 3 | `ordinal` | INTEGER | the host's 1-based position in its `hosts` |
| 4 | `host_name` | VARCHAR | `name` |
| 5 | `host_city` | VARCHAR | `address/city` |
| 6 | `host_region` | VARCHAR | `address/region` |
| 7 | `host_country` | VARCHAR | `address/country` |
| 8 | `host_disambiguated_id` | VARCHAR | `disambiguated-organization/disambiguated-organization-identifier` |
| 9 | `host_disambiguation_source` | VARCHAR | `disambiguated-organization/disambiguation-source` |
| 10 | `container` | VARCHAR | the element holding `hosts`: `proposal` or `resource-item` |
| 11 | `item_ordinal` | INTEGER | for `resource-item`: the item's 1-based position (`orcid_research_resource_items.ordinal`); NULL for the proposal |
| 12 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |

## `orcid_contributors`

One row per contributor of a full work or funding.

Items: `contributors/contributor` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD: root `path` attribute, else `orcid-identifier/path`, else the item `path` attribute |
| 2 | `owner` | VARCHAR | nearest ancestor with a `put-code` (else the enclosing group element) |
| 3 | `owner_put_code` | BIGINT | that ancestor's `put-code` |
| 4 | `ordinal` | INTEGER | 1-based position among same-name siblings |
| 5 | `contributor_orcid` | VARCHAR | `contributor-orcid/path` |
| 6 | `contributor_orcid_uri` | VARCHAR | `contributor-orcid/uri` |
| 7 | `credit_name` | VARCHAR | `credit-name` |
| 8 | `email` | VARCHAR | `contributor-email` |
| 9 | `sequence` | VARCHAR | `contributor-attributes/contributor-sequence` |
| 10 | `role` | VARCHAR | `contributor-attributes/contributor-role` |
| 11 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |
| 12 | `contributor_orcid_host` | VARCHAR | `contributor-orcid/host` (appended, CROSSCHECK #10) |

## `orcid_external_ids`

Every external identifier: item-level and group-level `external-ids`, peer-review `review-identifiers` and `subject-external-identifier`.

Items: `external-ids/external-id`, `review-identifiers/external-id`, `subject-external-identifier` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD: root `path` attribute, else `orcid-identifier/path`, else the item `path` attribute |
| 2 | `owner` | VARCHAR | nearest ancestor with a `put-code` (else the enclosing group element) |
| 3 | `owner_put_code` | BIGINT | that ancestor's `put-code` |
| 4 | `group_index` | INTEGER | 1-based position of the enclosing `group` / `affiliation-group` |
| 5 | `container` | VARCHAR | local name of the parent element |
| 6 | `type` | VARCHAR | `external-id-type` |
| 7 | `value` | VARCHAR | `external-id-value` |
| 8 | `normalized` | VARCHAR | `external-id-normalized` |
| 9 | `normalized_error` | VARCHAR | `external-id-normalized-error/error-message` |
| 10 | `url` | VARCHAR | `external-id-url` |
| 11 | `relationship` | VARCHAR | `external-id-relationship` |
| 12 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |
| 13 | `normalized_error_code` | VARCHAR | `external-id-normalized-error/error-code` (appended, CROSSCHECK #10) |

## `orcid_person`

One row per record `person` (or standalone `person` / `personal-details` document). Only the XSD positions match (record-3.0: `record/person`; person-3.0: the document element), so a full activity document never yields a row.

Items: `^person`, `^record/person`, `^personal-details` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD: root `path` attribute, else `orcid-identifier/path`, else the item `path` attribute |
| 2 | `given_names` | VARCHAR | `name/given-names` |
| 3 | `family_name` | VARCHAR | `name/family-name` |
| 4 | `credit_name` | VARCHAR | `name/credit-name` |
| 5 | `name_visibility` | VARCHAR | `name@visibility` |
| 6 | `biography` | VARCHAR | `biography/content` |
| 7 | `biography_visibility` | VARCHAR | `biography@visibility` |
| 8 | `last_modified` | VARCHAR | `last-modified-date` |
| 9 | `name_last_modified` | VARCHAR | `name/last-modified-date` |
| 10 | `biography_last_modified` | VARCHAR | `biography/last-modified-date` |
| 11 | `path` | VARCHAR | `@path` |
| 12 | `item` | VARCHAR | item element local name |
| 13 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |
| 14 | `orcid_uri` | VARCHAR | the record's `orcid-identifier/uri` (below the document element; NULL for a standalone `person` or `personal-details`; appended, CROSSCHECK #10) |
| 15 | `orcid_host` | VARCHAR | the record's `orcid-identifier/host` (as `orcid_uri`) |

## `orcid_person_items`

Other names, researcher URLs, emails, addresses, keywords and person external identifiers, in a record or standalone.

Items: `other-names/other-name`, `^other-name`, `researcher-urls/researcher-url`, `^researcher-url`, `emails/email`, `^email`, `addresses/address`, `^address`, `keywords/keyword`, `^keyword`, `external-identifiers/external-identifier`, `^external-identifier` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD: root `path` attribute, else `orcid-identifier/path`, else the item `path` attribute |
| 2 | `kind` | VARCHAR | item element local name |
| 3 | `put_code` | BIGINT | `@put-code` |
| 4 | `display_index` | INTEGER | `@display-index` |
| 5 | `value` | VARCHAR | `content` or `email` or `country` or `external-id-value` or `url-name` |
| 6 | `url` | VARCHAR | `url` or `external-id-url` |
| 7 | `type` | VARCHAR | `external-id-type` |
| 8 | `relationship` | VARCHAR | `external-id-relationship` |
| 9 | `verified` | BOOLEAN | `@verified` |
| 10 | `primary` | BOOLEAN | `@primary` |
| 11 | `visibility` | VARCHAR | `@visibility` |
| 12 | `path` | VARCHAR | `@path` |
| 13 | `source_name` | VARCHAR | `source/source-name` |
| 14 | `source_client_id` | VARCHAR | `source/source-client-id/path` |
| 15 | `source_orcid` | VARCHAR | `source/source-orcid/path` |
| 16 | `created` | VARCHAR | `created-date` |
| 17 | `last_modified` | VARCHAR | `last-modified-date` |
| 18 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |
| 19 | `assertion_origin_name` | VARCHAR | `source/assertion-origin-name` (appended, CROSSCHECK #10) |
| 20 | `assertion_origin_orcid` | VARCHAR | `source/assertion-origin-orcid/path` (appended, CROSSCHECK #10) |
| 21 | `assertion_origin_client_id` | VARCHAR | `source/assertion-origin-client-id/path` (appended, CROSSCHECK #10) |
| 22 | `normalized` | VARCHAR | `external-id-normalized` (external identifiers; appended, CROSSCHECK #10) |
| 23 | `normalized_error` | VARCHAR | `external-id-normalized-error/error-message` (as `normalized`) |
| 24 | `normalized_error_code` | VARCHAR | `external-id-normalized-error/error-code` (as `normalized`) |

## `orcid_history`

Record history (claimed, creation method, verification flags).

Items: `^history`, `^record/history` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD: root `path` attribute, else `orcid-identifier/path`, else the item `path` attribute |
| 2 | `creation_method` | VARCHAR | `creation-method` |
| 3 | `completion_date` | VARCHAR | `completion-date` |
| 4 | `submission_date` | VARCHAR | `submission-date` |
| 5 | `last_modified` | VARCHAR | `last-modified-date` |
| 6 | `claimed` | BOOLEAN | `claimed` |
| 7 | `deactivation_date` | VARCHAR | `deactivation-date` |
| 8 | `verified_email` | BOOLEAN | `verified-email` |
| 9 | `verified_primary_email` | BOOLEAN | `verified-primary-email` |
| 10 | `visibility` | VARCHAR | `@visibility` |
| 11 | `source_name` | VARCHAR | `source/source-name` |
| 12 | `source_client_id` | VARCHAR | `source/source-client-id/path` |
| 13 | `source_orcid` | VARCHAR | `source/source-orcid/path` |
| 14 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |
| 15 | `assertion_origin_name` | VARCHAR | `source/assertion-origin-name` (appended, CROSSCHECK #10) |
| 16 | `assertion_origin_orcid` | VARCHAR | `source/assertion-origin-orcid/path` (appended, CROSSCHECK #10) |
| 17 | `assertion_origin_client_id` | VARCHAR | `source/assertion-origin-client-id/path` (appended, CROSSCHECK #10) |

## API messages outside the record

The sections below read the other ORCID v3.0 API messages: the record summary, search results, errors, notifications (inbox messages) and group-id messages. They work exactly like the record sections (file reader plus `_xml` twin, archives, `source_hash`).

## `orcid_summary`

The record summary (`/v3.0/{iD}/summary`, summary-3.0.xsd): one row per `record-summary`. The per-type counts are ORCID's own totals, so they can exceed the number of items listed (the sample lists 3 employments under a count of 5); the listed items are in `orcid_summary_items`. The 3.0 summary carries counts only, no per-type latest dates.

Items: `^record-summary` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD: root `path` attribute, else `orcid-identifier/path`, else the item `path` attribute |
| 2 | `orcid_uri` | VARCHAR | `orcid-identifier/uri` |
| 3 | `orcid_host` | VARCHAR | `orcid-identifier/host` |
| 4 | `credit_name` | VARCHAR | `credit-name` |
| 5 | `created` | VARCHAR | `created-date` |
| 6 | `last_modified` | VARCHAR | `last-modified-date` |
| 7 | `external_identifier_count` | INTEGER | number of `external-identifiers/external-identifier` elements |
| 8 | `external_identifier_types` | VARCHAR | every `external-identifiers/external-identifier/external-id-type`, joined with `; ` |
| 9 | `employment_count` | INTEGER | `employments/count` |
| 10 | `professional_activity_count` | INTEGER | `professional-activities/count` |
| 11 | `education_qualification_count` | INTEGER | `education-qualifications/count` |
| 12 | `funding_self_asserted_count` | INTEGER | `fundings/self-asserted-count` |
| 13 | `funding_validated_count` | INTEGER | `fundings/validated-count` |
| 14 | `work_self_asserted_count` | INTEGER | `works/self-asserted-count` |
| 15 | `work_validated_count` | INTEGER | `works/validated-count` |
| 16 | `peer_review_publication_grants` | INTEGER | `peer-reviews/peer-review-publication-grants` |
| 17 | `peer_review_self_asserted_count` | INTEGER | `peer-reviews/self-asserted-count` |
| 18 | `peer_review_total` | INTEGER | `peer-reviews/total` |
| 19 | `research_resource_self_asserted_count` | INTEGER | `research-resources/self-asserted-count` |
| 20 | `research_resource_validated_count` | INTEGER | `research-resources/validated-count` |
| 21 | `email_domain_count` | INTEGER | `email-domains/count` |
| 22 | `email_domains` | VARCHAR | every `email-domains/email-domain/value`, joined with `; ` |
| 23 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |

## `orcid_summary_items`

Every item listed in a record summary: external identifiers, employments, professional activities, education/qualifications and email domains. `kind` is the item element (`external-identifier`, `employment`, `professional-activity`, `education-qualification`, `email-domain`); `put_code` is an element here, not an attribute.

Items: `^record-summary/external-identifiers/external-identifier`, `^record-summary/employments/employment`, `^record-summary/professional-activities/professional-activity`, `^record-summary/education-qualifications/education-qualification`, `^record-summary/email-domains/email-domain` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD: root `path` attribute, else `orcid-identifier/path`, else the item `path` attribute |
| 2 | `kind` | VARCHAR | item element local name |
| 3 | `ordinal` | INTEGER | 1-based position among same-name siblings |
| 4 | `put_code` | BIGINT | `put-code` |
| 5 | `type` | VARCHAR | `type` or `external-id-type` |
| 6 | `value` | VARCHAR | `external-id-value` or `value` |
| 7 | `url` | VARCHAR | `url` or `external-id-url` |
| 8 | `organization_name` | VARCHAR | `organization-name` |
| 9 | `role` | VARCHAR | `role` |
| 10 | `start_year` | INTEGER | `start-date/year` |
| 11 | `start_month` | INTEGER | `start-date/month` |
| 12 | `start_day` | INTEGER | `start-date/day` |
| 13 | `end_year` | INTEGER | `end-date/year` |
| 14 | `end_month` | INTEGER | `end-date/month` |
| 15 | `end_day` | INTEGER | `end-date/day` |
| 16 | `validated` | BOOLEAN | `validated` |
| 17 | `verification_date` | VARCHAR | `verification-date` |
| 18 | `created` | VARCHAR | `created-date` |
| 19 | `last_modified` | VARCHAR | `last-modified-date` |
| 20 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |

## `orcid_search_results`

Results of `/v3.0/search` (search-3.0.xsd: iD only) and `/v3.0/expanded-search` (expanded-search-3.0.xsd: names, emails, institutions). One row per result; a search without results yields no rows. `num_found` is the total match count of the query, repeated on every row.

Items: `^search/result`, `^expanded-search/expanded-result` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `num_found` | BIGINT | `@num-found` of the document element |
| 2 | `ordinal` | INTEGER | 1-based position among same-name siblings |
| 3 | `orcid` | VARCHAR | `orcid-identifier/path` or `orcid-id` |
| 4 | `orcid_uri` | VARCHAR | `orcid-identifier/uri` |
| 5 | `orcid_host` | VARCHAR | `orcid-identifier/host` |
| 6 | `given_names` | VARCHAR | `given-names` |
| 7 | `family_names` | VARCHAR | `family-names` |
| 8 | `credit_name` | VARCHAR | `credit-name` |
| 9 | `other_names` | VARCHAR | every `other-name`, joined with `; ` |
| 10 | `emails` | VARCHAR | every `email`, joined with `; ` |
| 11 | `institution_names` | VARCHAR | every `institution-name`, joined with `; ` |
| 12 | `item` | VARCHAR | item element local name |
| 13 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |

## `orcid_errors`

API errors (error-3.0.xsd): a standalone `error` response, or the per-entry errors of a `bulk` response, where `position` is the 1-based entry index, so it lines up with the submitted items (the works of the same response come from `orcid_works`).

Locked and deprecated records are error documents too: the Public Data File (and the API) serve `409 Conflict: The ORCID record is locked ... ORCID <iD>` (error code 9018) and `301 Moved Permanently: This account is deprecated. Please refer to account: <primary iD>. ORCID <iD>` (9007) in place of the record. They carry no `path`, so `orcid` falls back to the iD the message names, and `primary_orcid` gives a deprecated record's primary account. See [LOCKED_DEPRECATED.md](LOCKED_DEPRECATED.md).

Items: `^error`, `^bulk/error` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD: root `path` attribute, else `orcid-identifier/path`, else the item `path` attribute, else the valid iD after `ORCID ` in `developer-message` (then `user-message`) |
| 2 | `response_code` | INTEGER | `response-code` |
| 3 | `developer_message` | VARCHAR | `developer-message` |
| 4 | `user_message` | VARCHAR | `user-message` |
| 5 | `error_code` | INTEGER | `error-code` |
| 6 | `more_info` | VARCHAR | `more-info` |
| 7 | `container` | VARCHAR | local name of the parent element (NULL for the document element) |
| 8 | `position` | INTEGER | 1-based position among all sibling elements (in a `bulk` response: the entry's position) |
| 9 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |
| 10 | `primary_orcid` | VARCHAR | deprecated record: the valid iD after `refer to account:` in `developer-message` (then `user-message`); NULL otherwise |

## `orcid_notifications`

Inbox notifications: `permission` (notification-permission-3.0.xsd) and `custom` (notification-custom-3.0.xsd), standalone or inside `notifications`. The activity items of a permission notification are joined in document order; an item without an external id leaves no entry in the id columns, so match them by position only when every item has one.

Items: `^notification`, `^notifications/notification` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `put_code` | BIGINT | `@put-code` |
| 2 | `notification_type` | VARCHAR | `notification-type` |
| 3 | `subject` | VARCHAR | `notification-subject` or `subject` |
| 4 | `intro` | VARCHAR | `notification-intro` |
| 5 | `body_text` | VARCHAR | `body-text` |
| 6 | `body_html` | VARCHAR | `body-html` |
| 7 | `language` | VARCHAR | `@xml:lang` |
| 8 | `authorization_url` | VARCHAR | `authorization-url/uri` |
| 9 | `authorization_path` | VARCHAR | `authorization-url/path` |
| 10 | `authorization_host` | VARCHAR | `authorization-url/host` |
| 11 | `item_count` | INTEGER | number of `items/item` elements |
| 12 | `item_types` | VARCHAR | every `items/item/item-type`, joined with `; ` |
| 13 | `item_names` | VARCHAR | every `items/item/item-name`, joined with `; ` |
| 14 | `item_external_id_types` | VARCHAR | every `items/item/external-id/external-id-type`, joined with `; ` |
| 15 | `item_external_id_values` | VARCHAR | every `items/item/external-id/external-id-value`, joined with `; ` |
| 16 | `created` | VARCHAR | `created-date` |
| 17 | `sent_date` | VARCHAR | `sent-date` |
| 18 | `read_date` | VARCHAR | `read-date` |
| 19 | `actioned_date` | VARCHAR | `actioned-date` |
| 20 | `archived_date` | VARCHAR | `archived-date` |
| 21 | `source_name` | VARCHAR | `source/source-name` |
| 22 | `source_client_id` | VARCHAR | `source/source-client-id/path` |
| 23 | `source_orcid` | VARCHAR | `source/source-orcid/path` |
| 24 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |
| 25 | `assertion_origin_name` | VARCHAR | `source/assertion-origin-name` (appended, CROSSCHECK #10) |
| 26 | `assertion_origin_orcid` | VARCHAR | `source/assertion-origin-orcid/path` (appended, CROSSCHECK #10) |
| 27 | `assertion_origin_client_id` | VARCHAR | `source/assertion-origin-client-id/path` (appended, CROSSCHECK #10) |
| 28 | `item_external_id_urls` | VARCHAR | every `items/item/external-id/external-id-url`, joined with `; ` (appended, CROSSCHECK #10) |
| 29 | `item_external_id_relationships` | VARCHAR | every `items/item/external-id/external-id-relationship`, joined with `; ` |
| 30 | `item_external_id_normalized` | VARCHAR | every `items/item/external-id/external-id-normalized`, joined with `; ` |

## `orcid_group_ids`

Peer-review group ids (group-id-3.0.xsd): a single `group-id-record` or a `group-id-records` page. The source fields sit directly in the record (it extends `common:source-type`); `total`, `page` and `page_size` come from the page and repeat on every row.

Items: `^group-id-record`, `^group-id-records/group-id-record` (`^` = anchored at the document element).

| # | column | type | source |
|---|---|---|---|
| 1 | `put_code` | BIGINT | `@put-code` |
| 2 | `name` | VARCHAR | `name` |
| 3 | `group_id` | VARCHAR | `group-id` |
| 4 | `description` | VARCHAR | `description` |
| 5 | `type` | VARCHAR | `type` |
| 6 | `source_name` | VARCHAR | `source-name` or `source/source-name` |
| 7 | `source_client_id` | VARCHAR | `source-client-id/path` or `source/source-client-id/path` |
| 8 | `source_orcid` | VARCHAR | `source-orcid/path` or `source/source-orcid/path` |
| 9 | `total` | INTEGER | `total` below the document element |
| 10 | `page` | INTEGER | `page` below the document element |
| 11 | `page_size` | INTEGER | `page-size` below the document element |
| 12 | `source_hash` | VARCHAR | lowercase hex SHA-256 of the whole document |

Item patterns are local-name suffixes of the element path, so `contributors/contributor` matches at any depth; a leading `^` anchors the pattern at the document element (`^record/person` matches only the `person` child of a `record` document element, `^error` only an `error` document). Record-level and API message items are anchored, so a full activity document never yields `person`, `history`, `summary`, `errors`, ... rows. Nothing is read inside a `group/summary` element: that is the pre-3.0 peer-review layout (ORCID's read sample `peer-reviews-3.0.xml`), which the v3.0 XSDs replace with `peer-review-group/peer-review-summary`; yorc-rs, yorc-zig and ORCID's own classes skip it too.

Paths are relative to the item element; `a/b@x` is attribute `x` of element `b`. List columns ("every `x`, joined with `; `") hold each non-empty value in document order; count columns ("number of `x` elements") are 0 when there is none; "below the document element" values are document-level and repeat on every row.

## How a record changed over time

### `orcid_timeline(path)` / `orcid_timeline_xml(xml)`: one snapshot

Every activity, person item, name and biography carries a `created-date` and a
`last-modified-date`. The timeline turns them into dated events: one `created`
row per item, plus a `modified` row when the last modification differs (an item
with only a modification date gets just that row; an undated item one row with
NULL `event_at`/`event`). A snapshot records only creation and the *latest*
modification of each item; intermediate edits need several snapshots (below).

| # | column | type | source |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD |
| 2 | `event_at` | VARCHAR | ISO 8601 timestamp of the event (`event_at::TIMESTAMPTZ` to cast) |
| 3 | `event` | VARCHAR | `created` or `modified` |
| 4 | `kind` | VARCHAR | item kind: `work`, `funding`, `employment`, …, `keyword`, `email`, `name`, `biography` |
| 5 | `put_code` | BIGINT | item put-code |
| 6 | `label` | VARCHAR | title, role, subject or value of the item |
| 7 | `organization` | VARCHAR | affiliation / funder / convening / host organization |
| 8–13 | `visibility`, `source_name`, `created`, `last_modified`, `path`, `source_hash` | VARCHAR | as in the section tables |

```sql
SELECT event_at::TIMESTAMPTZ AS event_at, event, kind, label
FROM orcid_timeline('0000-0002-1825-0097.xml') ORDER BY event_at;
```

### `orcid_changes(old_path, new_path)` / `orcid_changes_xml(old_xml, new_xml)` / `orcid_changes_json(old_json, new_json)`: two snapshots

Compares two snapshots of a record, for example the same iD in two yearly Public
Data File releases (see `reference/orcid-dumps/index.csv`), or two `/record`
downloads. Items are matched on section, kind and put-code (single-item sections,
person and history, on the section alone), across `person`, `person_items`,
`works`, `fundings`, `affiliations`, `peer_reviews`, `research_resources` and
`history`. Every column except `source_hash` and `item` is compared, so a
modified item yields one row per changed field.

| # | column | type | meaning |
|---|---|---|---|
| 1 | `orcid` | VARCHAR | record iD |
| 2 | `section` | VARCHAR | section table the item belongs to |
| 3 | `kind` | VARCHAR | item kind where the section mixes kinds (affiliations, person items) |
| 4 | `put_code` | BIGINT | item put-code |
| 5 | `change` | VARCHAR | `added`, `removed` or `modified` |
| 6 | `field` | VARCHAR | changed column (for `modified`) |
| 7 | `old_value`, 8 `new_value` | VARCHAR | the column's values, as text |
| 9 | `label` | VARCHAR | title / role / value of the item (new side preferred) |

`orcid_changes` tells the format of each file, so an XML snapshot can be compared
with a JSON one (the same content gives no rows); `orcid_changes_json` reads two
JSON strings. Both take `json_root`.

**`json_root` on a lateral column.** Every `_json` twin also takes `json_root` as an
extra positional argument, read per row: `orcid_<section>_json(json, json_root)` and
`orcid_changes_json(old, new, json_root)`. On DuckDB v1.5.6, a call on a lateral column
(`FROM t, orcid_affiliations_json(t.doc, json_root := 'employment') a`) passes the named
form as that column, so both spellings work there; a NULL `json_root` means none.

Rows come section by section; within a section, modified and added items in the
new snapshot's order, then removed items. With `orcid_changes_xml` and
`orcid_changes_json`, a NULL side means "no snapshot", so every item on the
other side is added or removed:

```sql
-- what changed between consecutive releases, for snapshots kept in a table
SELECT a.release AS from_release, b.release AS to_release, c.*
FROM snapshots a JOIN snapshots b ON b.release = a.release + 1,
     orcid_changes_xml(a.xml, b.xml) c;
```


## Writing ORCID documents

The writers turn rows back into ORCID v3.0 documents: one item as XML or
JSON, or a bulk document of works. They are a crosswalk only; the extension
makes no network calls.

| function | returns |
|---|---|
| `orcid_work_to_xml(rec)`, `orcid_work_to_json(rec)` | a `work` (work-3.0.xsd) |
| `orcid_funding_to_xml(rec)`, `orcid_funding_to_json(rec)` | a `funding` (funding-3.0.xsd) |
| `orcid_affiliation_to_xml(rec)`, `orcid_affiliation_to_json(rec)` | a `distinction`, `education`, `employment`, `invited-position`, `membership`, `qualification` or `service`, by `rec.affiliation_type` |
| `orcid_peer_review_to_xml(rec)`, `orcid_peer_review_to_json(rec)` | a `peer-review` (peer-review-3.0.xsd) |
| `orcid_research_resource_to_xml(rec)`, `orcid_research_resource_to_json(rec)` | a `research-resource` (research-resource-3.0.xsd) |
| `orcid_person_item_to_xml(rec)`, `orcid_person_item_to_json(rec)` | an `address`, `keyword`, `other-name`, `researcher-url` or `external-identifier`, by `rec.kind` |
| `orcid_group_id_to_xml(rec)`, `orcid_group_id_to_json(rec)` | a `group-id-record` (group-id-3.0.xsd) |
| `orcid_bulk_works_to_xml(list)`, `orcid_bulk_works_to_json(list)` | a `bulk` of 1 to 100 works (bulk-3.0.xsd; `{"bulk": [{"work": ...}, ...]}`) |
| each writer above with a trailing `BOOLEAN`: `orcid_work_to_xml(rec, true)`, ..., `orcid_bulk_works_to_json(list, true)` | the same document in yorc-rs's bytes when `true`, in yorc-duckdb's own when `false` ([Crosswalk mode](#crosswalk-mode)) |
| `SET orcid_crosswalk = true` | every writer above emits yorc-rs's bytes ([Crosswalk mode](#crosswalk-mode)); off by default |

All return `VARCHAR`; a NULL argument gives NULL. Each is the set ORCID's
write samples cover (`reference/orcid-model/record_3.0/samples/write_samples`)
plus the group-id record. Emails, names, biographies and the other record
parts have no writer: their XSDs exist, but ORCID publishes no write sample
for them. Notifications are not written
either: the reader's `notifications` columns join their items into lists, so
no row shape would write them back faithfully.

### Input: a STRUCT whose fields are the reader's columns

`rec` is a `STRUCT`. Its scalar fields are the columns of the matching reader
section (`works` for `orcid_work_to_*`, `affiliations` for
`orcid_affiliation_to_*`, ...), of any type (written as text), so a row read
by that section can be passed whole: `SELECT orcid_work_to_json(w) FROM
orcid_works('x.xml') w`. Children that the readers expose as separate sections
are nested `LIST(STRUCT)` fields with the columns of those sections. A field
that is no column of the section and no list field is an error
(`unknown field 'titel'`), so a typo never drops a value silently; NULL
fields and blank text are left out; a NULL list is absent, an empty list
present.

| writer | scalar fields: columns of | list fields (item fields: columns of) |
|---|---|---|
| `work` | `orcid_works` | `external_ids` (`orcid_external_ids`), `contributors` (`orcid_contributors`) |
| `funding` | `orcid_fundings` | `external_ids`, `contributors` |
| `affiliation` | `orcid_affiliations` | `external_ids` |
| `peer_review` | `orcid_peer_reviews` | `external_ids` |
| `research_resource` | `orcid_research_resources` | `external_ids` (the proposal's), `hosts` (the proposal's: `orcid_research_resource_hosts`), `resource_items` (`orcid_research_resource_items`, each with its own `external_ids` and `hosts`) |
| `person_item` | `orcid_person_items` | |
| `group_id` | `orcid_group_ids` | |

Reassembling a record from the readers is a join on the owner: list items
keep their list order (contributors: order by `ordinal`):

```sql
SELECT orcid_work_to_json(struct_insert(w,
         external_ids := (SELECT list(e) FROM orcid_external_ids('work.xml') e WHERE e.owner_put_code = w.put_code),
         contributors := (SELECT list(c ORDER BY c.ordinal) FROM orcid_contributors('work.xml') c
                          WHERE c.owner_put_code = w.put_code)))
FROM orcid_works('work.xml') w;
```

A row assembled this way round-trips: `orcid_works_xml(orcid_work_to_xml(x))`
gives the row of `x` back (and the same for its external ids and
contributors, and for every writer and both formats), except `source_hash`,
which hashes the new document, and the read-only columns below, which are
NULL in what ORCID's write samples carry anyway. `test/sql/orcid_write.test`
checks this for every write sample, in XML, in JSON, and XML to JSON to XML.
One limit: the affiliation JSON carries no type, so reading it back needs
`json_root` (see [JSON documents](#json-documents)).

**Research-resource hosts.** A proposal or resource item writes every host of
its `hosts` list field (the rows of `orcid_research_resource_hosts` for it, in
`ordinal` order; at most 10, research-resource-3.0.xsd). Without the list, the
flat `host_*` fields write one host, as before. Both may be given (a row of
`orcid_research_resources` with `hosts` added, say) if they agree: each
non-blank flat field must equal the first non-empty value of that field among
the listed hosts, which is how the readers fill the flat columns. Otherwise
it is an error (`research_resource: 'host_name' is 'A', but the first
'hosts[].host_name' is 'B': ...`); the list is then what is written.

```sql
SELECT orcid_research_resource_to_xml(struct_insert(r,
         hosts := (SELECT list(h ORDER BY h.ordinal) FROM orcid_research_resource_hosts('rr.xml') h
                   WHERE h.container = 'proposal' AND h.owner_put_code = r.put_code),
         resource_items := (SELECT list(struct_insert(i,
                                   hosts := (SELECT list(h ORDER BY h.ordinal) FROM orcid_research_resource_hosts('rr.xml') h
                                             WHERE h.container = 'resource-item' AND h.owner_put_code = i.owner_put_code
                                               AND h.item_ordinal = i.ordinal)) ORDER BY i.ordinal)
                            FROM orcid_research_resource_items('rr.xml') i WHERE i.owner_put_code = r.put_code)))
FROM orcid_research_resources('rr.xml') r;
```

### What is written

Fields map to elements as the reader's columns read them (REFERENCE.md tables
above, the `source` column). The element order is that of each
`xs:sequence`; namespace prefixes are ORCID's (`work:`, `common:`, ...).

| writer | written (in schema order; **bold** = required: missing is an error) |
|---|---|
| `work` | **`title`**, `subtitle`, `translated_title` (+ **`translated_title_language`**), `journal`, `short_description`, `citation_type` + `citation_value` (both, when either), **`type`**, publication date (**`pub_year`**, `pub_month`, `pub_day`), `external_ids` (**`type`**, **`value`**, `url`, `relationship`), `url`, `contributors` (`contributor_orcid_uri`, `contributor_orcid`, `contributor_orcid_host`, `credit_name`, `email`, `sequence`, `role`), `language_code`, `country` |
| `funding` | **`type`**, `organization_defined_type`, `title` (+ `translated_title`, **`translated_title_language`**), `short_description`, `amount` (+ **`currency`**), `url`, start and end dates, `external_ids`, `contributors` (as works, without `sequence`: funding-3.0.xsd has no contributor sequence, so a non-blank `sequence` is an error rather than a dropped value), the organization (**`org_name`**, `org_city`, `org_region`, `org_country`, `org_disambiguated_id` + `org_disambiguation_source`) |
| `affiliation` | **`affiliation_type`** (the document element), `department`, `role_title`, start and end dates, the organization (as fundings, optional), `url`, `external_ids` |
| `peer_review` | **`reviewer_role`**, `external_ids` as `review-identifiers` (always written), `review_url`, **`review_type`**, **`completion_year`** (+ month, day) as `review-completion-date`, **`review_group_id`**, the subject identifier, `subject_container_name`, `subject_type`, `subject_name` (+ `subject_subtitle`, `subject_translated_name` + **`subject_translated_name_language`**), `subject_url`, the convening organization (**`convening_org_name`**, ...) |
| `research_resource` | the proposal: **`title`** (+ translated title), the hosts (**`hosts`**, 1 to 10, or the flat first host: **`host_name`**, `host_city`, `host_region`, `host_country`, `host_disambiguated_id` + `host_disambiguation_source`), `external_ids` (always written), start and end dates, `url`; then **`resource_items`** (at least one: **`resource_name`**, **`resource_type`**, the hosts as the proposal's, `external_ids`, `url`) |
| `person_item` | by **`kind`**: `address` (**`value`** as `address:country`), `keyword` / `other-name` (**`value`** as `content`), `researcher-url` (`value` as `url-name`, **`url`**), `external-identifier` (**`type`**, **`value`**, `url`, `relationship`) |
| `group_id` | **`name`**, **`group_id`**, **`description`**, **`type`** |

Every item also writes `put_code` (the `put-code` attribute: ORCID needs it to
update an item, work-3.0.xsd) and `visibility` (an attribute; ORCID's
`bulk-works-3-works-3.0.json` sample sends it), except `group_id`, which has
no visibility.

**Read-only and derived fields are not written**: `orcid`, `path`,
`display_index`, `group_index`, `created`, `last_modified`, the `source_*` and
`assertion_origin_*` columns, `item`, `source_hash`, the list items' `owner`,
`owner_put_code`, `ordinal`, `item_ordinal` and `container`, and `normalized` /
`normalized_error` / `normalized_error_code` (ORCID's transient
normalization). ORCID sets them itself,
and none of its write samples carries one. Two derived fields are used:

- a work's `doi`, when `external_ids` is not given (absent or NULL), becomes
  one external id (`doi`, the value, `self`), so `SELECT orcid_work_to_xml(w)
  FROM orcid_works(...) w` keeps the DOI; given `external_ids` (even empty)
  win;
- a peer review's subject identifier is the `external_ids` item whose
  `container` is `peer-review` (what `orcid_external_ids` yields for
  `subject-external-identifier`), with its URL and relationship; without one,
  `subject_id_type` and `subject_id_value`. The other items are the
  `review-identifiers`. Two items of container `peer-review` are an error.

A contributor's `contributor_orcid_host` (`orcid.org`) is written after the
URI and path (`common:orcid-id`; ORCID's `work-full-3.0.xml` write sample
carries it); it needs `contributor_orcid`, since the identifier is chosen by
its path.

**Values.** Years must be integers 1900..2100, months 1..12 and days 1..31
(common-3.0.xsd), written with two digits (`02`); a day needs its month.
`put_code` must be an integer. Other text is written as given.

**Errors** name the function and the field, with the list position for list
items: `orcid_work_to_xml: work: 'external_ids[2].type' is required
(common:external-id-type)`, `'translated_title_language' is required with
'translated_title'`, `'pub_month' is 13, outside the schema's range 1..12`,
`funding: cannot write 'contributors[2].sequence'
(funding/contributors/contributor[2]/contributor-attributes/contributor-sequence):
a funding contributor has no contributor-sequence (funding-3.0.xsd)`.

The writers check what the XSD structure needs to write a document. They do
not check vocabularies, lengths or patterns: a work of type `poem` or with a
1001-character title is written.

### Output and escaping

This is the default layout; [Crosswalk mode](#crosswalk-mode) writes
yorc-rs's bytes instead.

XML: an XML declaration, the namespaces used declared on the document element
(`xmlns:common="http://www.orcid.org/ns/common"`, ...; no
`xsi:schemaLocation`), one element per line, tab-indented, text inline. `&`,
`<` and `>` are escaped everywhere and `"` in attributes; CR is written as
`&#13;` (and TAB / LF as `&#9;` / `&#10;` in attributes) so that it survives
XML's line-end and attribute normalization. Characters XML 1.0 cannot carry
(U+0000-U+0008, U+000B, U+000C, U+000E-U+001F, U+FFFE, U+FFFF) are an error,
not a dropped or replaced character.

JSON: ORCID's own conventions (as `read_samples/record-full-3.0.json`
serializes): member names are the XML local names, simple values that ORCID
wraps are `{"value": ...}` objects (titles, `journal-title`, URLs,
`credit-name`, the dates' `year` / `month` / `day`, `amount` with
`currency-code`, `translated-title` with `language-code`, an address's
`country`, ...), repeated elements are arrays (`"external-id": [...]`,
`"contributor": [...]`, a research resource's `"resource-item": [...]` and
`"hosts": {"organization": [...]}`), `put-code` is a number, NULL fields are
left out (not written as `null`), two-space indentation. Strings escape `"`,
`\` and control characters (`\n`, `\t`, ..., `\u0001`, and DEL as `\u007f`);
everything else, U+2028 / U+2029 and non-BMP characters included, is UTF-8
(as serde_json and Jackson write it).

Text that is not valid UTF-8 is an error in both formats. (DuckDB `VARCHAR`
is UTF-8 already; the check covers the core's other callers.)

### Crosswalk mode

By default the writers use yorc-duckdb's own layout (above). Crosswalk mode
writes the same documents in exactly the bytes yorc-rs's document
writers write for them (`xml::to_string` and `json::to_string` in
`yorc-rs/src/xml/write.rs` and `src/json.rs`), for documents without
ORCID's read-only fields (see *Item documents only* below). yorc-zig matches
yorc-rs byte for byte, so the three implementations can be compared file
by file (the cross-implementation check, [BENCHMARKS.md](../BENCHMARKS.md#parity)). It is off by default, and nothing changes
until it is switched on.

| how | scope |
|---|---|
| `SET orcid_crosswalk = true` (`RESET orcid_crosswalk`, or `= false`, to switch back) | every writer scalar of the session, unless the call says otherwise |
| a trailing `BOOLEAN` argument: `orcid_work_to_xml(rec, true)`, `orcid_bulk_works_to_json(list, true)`, ... | that call. `true` is crosswalk mode, `false` is the default layout, whatever the setting. A NULL argument gives NULL, like a NULL `rec` |

The argument wins over the setting. DuckDB scalars take no
named parameters, so the argument is positional: `orcid_work_to_xml(rec,
crosswalk := true)` is not available.

Both modes write the same elements and values from the same input, and make
the same checks with the same errors (`src/core/crosswalk.cpp` lays out the
tree that `src/core/writer.cpp` renders). Only the bytes differ:

| | default | crosswalk (yorc-rs) |
|---|---|---|
| XML indentation | one tab per level | two spaces per level |
| JSON key order | schema (`xs:sequence`) order, attributes last | sorted by byte order (serde_json's `BTreeMap`), at every level |
| JSON for an empty container that is always written (a proposal's or resource item's `external-ids`, a peer review's `review-identifiers`) | `{}` | `{"external-id": []}` (yorc-rs's `req_ext_ids`) |
| DEL (U+007F) in a JSON string | `\u007f` | the character itself, as serde_json writes it |
| `put-code` | as given (digits, optional `-`) | as yorc-rs prints its `i64`: no leading zeros (`007` becomes `7`); a value outside the `i64` range is an error |

Everything else is already the same in both, and is yorc-rs's contract:

- XML: `<?xml version="1.0" encoding="UTF-8"?>` and LF, LF line ends, a
  final LF; ORCID's prefixes; the namespaces used, declared on the document
  element before its attributes, in `xml::Ns` order (alphabetical by
  prefix); elements in XSD order; attributes `put-code`, then `visibility`;
  `<x/>` for an element without children or text; text escapes `&`, `<`,
  `>` and CR (`&#13;`), attributes also `"`, TAB and LF (`&quot;`, `&#9;`,
  `&#10;`), double-quoted; fuzzy-date parts zero-padded (`2012`, `02`).
- JSON: two-space indentation, `"key": value` (not Jackson's `"key" :
  value`), `{}` and `[]` for empty values, a final LF; no `null`s; the
  `{"value": ...}` wrappers of ORCID's JSON; `put-code` a number; dates'
  parts strings; `"` `\` `\b` `\f` `\n` `\r` `\t` escaped, other C0
  controls as `\u00xx` (lower-case hex), everything else, U+2028 / U+2029
  included, as UTF-8.

```sql
SET orcid_crosswalk = true;
SELECT orcid_person_item_to_xml({'kind': 'keyword', 'value': 'vents', 'put_code': 7});
-- <?xml version="1.0" encoding="UTF-8"?>
-- <keyword:keyword xmlns:keyword="http://www.orcid.org/ns/keyword" put-code="7">
--   <keyword:content>vents</keyword:content>
-- </keyword:keyword>
SELECT orcid_person_item_to_json({'kind': 'keyword', 'value': 'vents', 'put_code': 7}, false);  -- the default layout
```

**Item documents only.** yorc-duckdb's writers write one item, or a bulk
of works, and never write the fields ORCID sets itself (source, created and
last-modified dates, `path`, `display-index`, normalized external ids, a
work's `country/@visibility`), in either mode. yorc-rs's `to_string`
writes every field its model holds, so the bytes agree for documents
without those fields, such as ORCID's write samples. Two more limits, both
outside ORCID's write samples:

- an organization's `common:address` is always written (`<common:address/>`
  / `"address": {}` when it has no city, region or country), as the XSD
  requires it; yorc-rs writes it only when the model holds one. Every
  ORCID write sample has an address, so the bytes agree there.
- `group_id` (a group-id record) has no yorc-rs counterpart. Crosswalk
  mode applies the same rules to it (two spaces, sorted keys).

`test/sql/orcid_crosswalk.test` reads each of ORCID's 21 XML write samples,
writes it back in crosswalk mode and compares it with
`test/data/crosswalk/write_samples/<stem>.{xml,json}`. Those files were
derived from yorc-rs's rules by `scripts/crosswalk_expected.py`, not
written by yorc-rs itself. The authoritative check is
the cross-implementation check ([BENCHMARKS.md](../BENCHMARKS.md#parity)),
whose DuckDB leg runs in crosswalk mode and compares the written documents
with the Rust and Zig output byte for byte.
