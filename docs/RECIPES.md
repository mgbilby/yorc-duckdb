# orcid — recipes

Worked SQL for the jobs people bring ORCID data to DuckDB for: reading saved
records and Public Data File dumps (XML or JSON), profiling a record, finding duplicate
DOIs, building co-author pairs and institution lists, following a record
from one release to the next, and writing ORCID documents. The functions stay per document. Everything that
crosses documents (joining sections, pairing two releases, counting across
records) is ordinary SQL, shown here.

Run the examples from the repository root: most of them read the fixtures in
`test/data/` and the ORCID schema samples in `reference/orcid-model/`, and those
are checked by `test/sql/orcid_recipes.test` (the recipe number is in each
test's comment). Examples that need a network or a full dump say so.

The saved record of the examples is `test/data/demo/0000-0002-2771-9344.xml`, a
public record that ORCID staff use as an example (Sofia Maria Garcia Hernandez,
a fictitious person). It has 12 works and no fundings or affiliations, so the
recipes about those (5.2, 5.3, 8) read ORCID's sample record
`reference/orcid-model/record_3.0/samples/read_samples/record-3.0.xml` instead,
whose values are ORCID's placeholders.

New to the extension? Start with the walkthrough,
[WALKTHROUGH.md](WALKTHROUGH.md): a setup cell and nine numbered cells with
their expected output, runnable in one go with `duckdb < docs/walkthrough.sql`.

Column names and their XML sources are in [REFERENCE.md](REFERENCE.md). What
the schema carries and which column holds it is in [COVERAGE.md](COVERAGE.md).
Reusable versions of several recipes, as macro files, are in
[TASKS.md](TASKS.md).

## 1. Install and load

```sql
INSTALL yorc FROM community;
LOAD yorc;
```

A build from this repository has the extension linked into its shell
(`./build/release/duckdb`), so `LOAD` is not needed there. To load a locally
built loadable extension into a stock DuckDB v1.5.6:

```sql
-- the shell must be started with -unsigned (or SET allow_unsigned_extensions = true at startup)
LOAD 'build/release/extension/yorc/yorc.duckdb_extension';
```

The extension needs no other extension. `httpfs` (recipe 3.5) and `parquet`
(recipe 13) are optional companions; see [ECOSYSTEM.md](ECOSYSTEM.md).

## 2. Validate and normalise iDs

`orcid_valid` checks the ISO 7064 MOD 11-2 check character. `orcid_normalize`
returns the canonical `XXXX-XXXX-XXXX-XXXX` form, or NULL. Both accept an
`https://orcid.org/`, `http://orcid.org/` or `orcid.org/` prefix after leading
whitespace, and a lowercase `x`. After the prefix, hyphens and whitespace are
ignored wherever they are, so `0000 0002 1825 0097` and `000000021825-0097`
are accepted too. What remains must be 15 digits and the check character.

```sql
SELECT orcid_normalize('https://orcid.org/0000-0002-1825-0097');  -- 0000-0002-1825-0097
SELECT orcid_normalize(' 000000021694233x ');                      -- 0000-0002-1694-233X
SELECT orcid_valid('0000-0002-1825-0098');                         -- false (bad check digit)
```

Clean a column of user-entered iDs, keeping the rows that do not parse for
review:

```sql
SELECT raw, orcid_normalize(raw) AS orcid, orcid_valid(raw) AS valid
FROM (VALUES ('https://orcid.org/0000-0002-1825-0097'),
             ('0000 0002 1825 0097'),
             ('0000-0002-1825-0098'),
             ('https://www.orcid.org/0000-0002-1825-0097')) t(raw);
```

The first two normalise to `0000-0002-1825-0097`. The third has a wrong check
digit and the fourth an unrecognised prefix (`www.`), so both give NULL and
`false`. Strip such prefixes with `regexp_replace` first if your source
writes them.

## 3. Read records

Every section comes as `orcid_<section>(path)`, which reads files, and
`orcid_<section>_xml(xml)`, which parses strings. Both accept a full `/record`,
a section collection (`/works`, `/employments`, ...), a single item (`/work/{put-code}`)
or a `bulk` response.

### 3.1 One saved record

```sql
SELECT put_code, title, type, pub_year, doi
FROM orcid_works('test/data/demo/0000-0002-2771-9344.xml')
ORDER BY pub_year DESC, put_code
LIMIT 5;
```

### 3.2 A directory or a glob

Paths go through DuckDB's file system: globs, `~`, and with `httpfs`
`https://` and `s3://`. A glob reads its files in sorted path order and in
parallel. A missing file, or a glob that matches nothing, is an error.

```sql
SELECT count(*), count(DISTINCT orcid) FROM orcid_works('test/data/*.xml');
-- every fixture document one level down: activities/, changes/, demo/, errors/, synthetic/
SELECT count(*) FROM orcid_works('test/data/*/*.xml');
```

### 3.3 XML already in a table

The `_xml` twin takes a column through a lateral join. `read_text` (core
DuckDB) loads files as strings; the iD can come from the file name:

```sql
CREATE TABLE records AS
SELECT regexp_extract(filename, '([0-9]{4}-[0-9]{4}-[0-9]{4}-[0-9]{3}[0-9X])', 1) AS orcid,
       content AS xml
FROM read_text('test/data/*.xml');

SELECT r.orcid, a.affiliation_type, a.org_name, a.start_year
FROM records r, orcid_affiliations_xml(r.xml) a;
```

For a table that holds **one** document, pass it as a constant argument
instead (recipe 14.3).

`orcid_<section>_xml(NULL)` yields no rows with the full schema, which is how
to create an empty table of the right shape:

```sql
CREATE TABLE works AS SELECT * FROM orcid_works_xml(NULL);
```

### 3.4 A Public Data File archive, read in place

`.tar`, `.tar.gz` and `.tgz` archives are streamed member by member and never
unpacked to disk.

```sql
SELECT orcid, given_names, family_name
FROM orcid_person('test/data/dump/orcid_test_summaries.tar.gz')
ORDER BY orcid;

-- the raw documents of an archive: file, member, name, orcid (from the path), xml
SELECT member, orcid, length(xml) AS bytes
FROM orcid_documents('test/data/dump/orcid_test_summaries.tar.gz');
```

### 3.5 A dump on figshare, over https

Needs the network and the `httpfs` extension. Only the bytes up to the last
member read are downloaded, so `max_records` keeps exploration cheap. The
download URLs, sizes and MD5s of every release are in
`reference/orcid-dumps/index.csv` (regenerate it with
`scripts/orcid_dumps_index.sh`).

```sql
INSTALL httpfs; LOAD httpfs;
SELECT orcid, title, pub_year
FROM orcid_works('https://ndownloader.figshare.com/files/<file-id>', max_records := 1000);
```

### 3.6 `members`, `max_records` and `max_member_bytes`

The file readers and `orcid_documents` take three named parameters:

| parameter | default | effect |
|---|---|---|
| `members` | `'*.xml'` | glob over archive member paths (`*` also matches `/`) |
| `max_records` | unlimited | stop after this many matching members per file |
| `max_member_bytes` | 64 MiB | refuse a matching member larger than this, before reading it |

```sql
-- dumps group records by the last three characters of the iD
SELECT count(*), min(orcid)
FROM orcid_works('test/data/dump/orcid_test_summaries.tar.gz', members := '*/634/*');

-- the first matching member only
SELECT count(*) FROM orcid_works('test/data/dump/orcid_test_summaries.tar.gz', max_records := 1);

-- a member over the limit fails the scan and names the member
SELECT count(*) FROM orcid_works('test/data/dump/orcid_test_summaries.tar.gz', max_member_bytes := 1000);
```

gzip has no index, so `members` filters what is parsed, not what is read:
reaching the last member still decompresses everything before it.

### 3.7 A reproducible sample of a dump

`orcid_dump_sample(archive, n, seed)` draws `n` records in one pass, with the
sampler of yorc-rs and yorc-zig: the same archive, `n` and seed pick the same
records in all three packages ([REFERENCE.md](REFERENCE.md#orcid_dump_sample-a-seeded-sample-of-a-dump)).
Records that are not kept are never read.

```sql
-- seed 42 keeps 0000-0003-0100-6634 of the fixture's two records (as the walkthroughs)
SELECT slot, orcid, eligible FROM orcid_dump_sample('test/data/dump/orcid_test_summaries.tar.gz', 1, 42);

-- the works of a sample: read the kept documents with a section's _xml twin
SELECT s.orcid, count(*) AS works
FROM orcid_dump_sample('test/data/dump/orcid_test_summaries.tar.gz', 5, 42) s, orcid_works_xml(s.xml) w
GROUP BY s.orcid ORDER BY s.orcid;
```

## 4. One query per section

Each query reads an ORCID schema sample, so the values can be checked against
the file.

```sql
-- 4.1 works: one row per work-summary / work
SELECT put_code, title, type, pub_year, doi, item
FROM orcid_works('reference/orcid-model/record_3.0/samples/read_samples/work-full-3.0.xml');

-- 4.2 fundings: funder and amount
SELECT title, type, amount, currency, org_name, org_disambiguation_source
FROM orcid_fundings('reference/orcid-model/record_3.0/samples/read_samples/funding-full-3.0.xml');

-- 4.3 affiliations: all seven types in one table
SELECT affiliation_type, org_name, role_title, start_year, end_year
FROM orcid_affiliations('reference/orcid-model/record_3.0/samples/read_samples/record-3.0.xml')
ORDER BY affiliation_type;

-- 4.4 peer reviews: role, subject and convening organisation
SELECT reviewer_role, review_type, completion_year, subject_type, subject_name, convening_org_name
FROM orcid_peer_reviews('reference/orcid-model/record_3.0/samples/read_samples/peer-review-full-3.0.xml');

-- 4.5 research resources and their items
SELECT title, host_name, start_year, end_year
FROM orcid_research_resources('reference/orcid-model/record_3.0/samples/read_samples/research-resource-3.0.xml');
SELECT owner_put_code, ordinal, resource_name, resource_type, host_name
FROM orcid_research_resource_items('reference/orcid-model/record_3.0/samples/read_samples/research-resource-3.0.xml')
ORDER BY ordinal;

-- 4.6 contributors of full works and fundings
SELECT owner, owner_put_code, ordinal, contributor_orcid, credit_name, sequence, role
FROM orcid_contributors('reference/orcid-model/record_3.0/samples/read_samples/work-full-3.0.xml');

-- 4.7 every external identifier, with the item that owns it
SELECT owner, owner_put_code, container, type, value, relationship
FROM orcid_external_ids('reference/orcid-model/record_3.0/samples/read_samples/work-full-3.0.xml')
ORDER BY type;

-- 4.8 names and biography
SELECT given_names, family_name, credit_name, biography
FROM orcid_person('reference/orcid-model/record_3.0/samples/read_samples/record-3.0.xml');

-- 4.9 other names, URLs, emails, addresses, keywords, person ids
SELECT kind, value, url, type, visibility
FROM orcid_person_items('reference/orcid-model/record_3.0/samples/read_samples/record-3.0.xml')
ORDER BY kind;

-- 4.10 record history
SELECT creation_method, claimed, verified_email, visibility
FROM orcid_history('reference/orcid-model/record_3.0/samples/read_samples/history-3.0.xml');
```

The timeline, the API messages and the change log have recipes of their own
(9, 10, 12).

## 5. Joins across sections

Every row of a record section carries `source_hash`, the SHA-256 of the
document it came from. Within one document, `put_code` identifies an item. So
`(source_hash, put_code)` joins an item to its contributors and identifiers,
whichever document type the item came from. (Use `orcid` as well, or instead,
when every document carries a `path`. A bulk response does not, and a record
always does.)

### 5.1 Works with their contributors and identifiers

```sql
WITH w AS (SELECT * FROM orcid_works('reference/orcid-model/record_3.0/samples/read_samples/work-full-3.0.xml')),
     c AS (SELECT * FROM orcid_contributors('reference/orcid-model/record_3.0/samples/read_samples/work-full-3.0.xml')),
     e AS (SELECT * FROM orcid_external_ids('reference/orcid-model/record_3.0/samples/read_samples/work-full-3.0.xml'))
SELECT w.put_code, w.title, c.credit_name, c.role, e.type AS id_type, e.value AS id_value
FROM w
LEFT JOIN c ON c.source_hash = w.source_hash AND c.owner_put_code = w.put_code
LEFT JOIN e ON e.source_hash = w.source_hash AND e.owner_put_code = w.put_code
ORDER BY e.type;
```

In a record, external ids also sit at group level (the ids ORCID grouped the
works by). Those rows have `owner = 'group'` and no `owner_put_code`. Filter on
`owner IN ('work', 'work-summary')` for the item-level ones.

### 5.2 An affiliation timeline

Employments in start order; an open end date means current. Distinctions are
point events and carry no end date, so treat them separately. The demonstration
record has no affiliations, so this reads ORCID's sample record (one employment,
1948 to 2025):

```sql
SELECT affiliation_type, org_name, role_title, start_year, end_year, end_year IS NULL AS is_current
FROM orcid_affiliations('reference/orcid-model/record_3.0/samples/read_samples/record-3.0.xml')
WHERE affiliation_type = 'employment'
ORDER BY start_year, end_year NULLS LAST;
```

### 5.3 Funding by funder

On ORCID's sample record (one funding; the demonstration record has none):

```sql
SELECT org_name AS funder, org_disambiguation_source, org_disambiguated_id,
       count(*) AS fundings, min(start_year) AS first_year, max(end_year) AS last_year
FROM orcid_fundings('reference/orcid-model/record_3.0/samples/read_samples/record-3.0.xml')
GROUP BY ALL
ORDER BY fundings DESC, funder;
```

## 6. DOIs and duplicates

`orcid_works.doi` is the first item-level external id of type `doi` with
relationship `self`, ASCII-lowercased. All of a work's identifiers are in
`orcid_external_ids`.

```sql
-- DOIs listed by more than one work of a record
SELECT doi, count(*) AS works, count(DISTINCT group_index) AS groups, list(put_code ORDER BY put_code) AS put_codes
FROM orcid_works('test/data/demo/0000-0002-2771-9344.xml')
WHERE doi IS NOT NULL
GROUP BY doi
HAVING count(*) > 1
ORDER BY works DESC, doi;
```

ORCID groups works that share an identifier, so repeats of a DOI inside one
group are the same work from several sources (a publisher, a repository, the
researcher). `groups > 1` is the case worth a look: the same DOI in two groups.
In the record 0000-0002-2771-9344 there is none: its one repeated DOI,
`10.25592/uhhfdm.19102`, is on two works of work group 1.

Across records, a DOI claimed by several people is co-authorship evidence:

```sql
SELECT doi, count(DISTINCT orcid) AS records
FROM orcid_works('ORCID_2025_10_summaries.tar.gz', max_records := 100000)
WHERE doi IS NOT NULL
GROUP BY doi
HAVING count(DISTINCT orcid) > 1
ORDER BY records DESC;
```

## 7. Co-author pairs from contributors

Full works list their contributors, and some contributors carry an iD. Two
iDs on the same work make a pair. The Public Data File's activities archives
hold full works, one per member; summaries have none.

```sql
WITH c AS (
  SELECT DISTINCT source_hash, owner, owner_put_code, contributor_orcid
  FROM orcid_contributors('test/data/synthetic/coauthors-bulk.xml')
  WHERE contributor_orcid IS NOT NULL)
SELECT a.contributor_orcid AS orcid_a, b.contributor_orcid AS orcid_b, count(*) AS works
FROM c a
JOIN c b ON a.source_hash = b.source_hash AND a.owner = b.owner AND a.owner_put_code = b.owner_put_code
        AND a.contributor_orcid < b.contributor_orcid
GROUP BY ALL
ORDER BY works DESC, orcid_a, orcid_b;
```

`examples/tasks/coauthors.sql` packages this as `task_coauthor_pairs(path)`
([TASKS.md](TASKS.md)).

## 8. Institutions via disambiguated ids

Organisations in affiliations, fundings, peer reviews and research resources
carry `*_disambiguated_id` and `*_disambiguation_source` (`RINGGOLD`, `GRID`,
`ROR`, `FUNDREF`, `LEI`). Group on both to count people per institution
(here over ORCID's sample record, which has one affiliation of each type; over
many records, `people` counts the iDs):

```sql
SELECT org_disambiguation_source AS source, org_disambiguated_id AS id,
       any_value(org_name) AS name, count(DISTINCT orcid) AS people, count(*) AS affiliations
FROM orcid_affiliations('reference/orcid-model/record_3.0/samples/read_samples/record-3.0.xml')
WHERE org_disambiguated_id IS NOT NULL
GROUP BY ALL
ORDER BY affiliations DESC, source, id;
```

ORCID writes the same institution under different sources over time, and the
identifiers have different spellings (`https://ror.org/…`, a bare GRID id, a
FundRef DOI URL). Mapping them onto one registry is a join against that
registry's own dump; [LINKING.md](LINKING.md) shows it for ROR.

## 9. A record's timeline

Every item carries a `created-date` and a `last-modified-date`.
`orcid_timeline` turns them into dated events: one `created` row per item,
plus a `modified` row when the last modification differs.

```sql
SELECT event_at::TIMESTAMPTZ AS event_at, event, kind, put_code, label
FROM orcid_timeline('test/data/demo/0000-0002-2771-9344.xml')
ORDER BY event_at
LIMIT 10;

-- activity per year and kind
SELECT year(event_at::TIMESTAMPTZ) AS year, kind, count(*) FILTER (WHERE event = 'created') AS added
FROM orcid_timeline('test/data/demo/0000-0002-2771-9344.xml')
GROUP BY ALL ORDER BY year, kind;
```

A snapshot records only the creation and the latest modification of each
item; intermediate edits need two snapshots (recipe 10).

## 10. Changes

### 10.1 Between two snapshots of a record

```sql
SELECT section, kind, put_code, change, field, old_value, new_value
FROM orcid_changes('test/data/changes/old.xml', 'test/data/changes/new.xml');
```

Items are matched on section, kind and put-code; a modified item yields one
row per changed column. `orcid_changes_xml(old_xml, new_xml)` does the same
for strings, and a NULL side means "no snapshot", so every item of the other
side is `added` or `removed`.

### 10.2 Between two Public Data File releases

`orcid_documents` gives each member's iD from its path, which pairs the same
records across two releases without unpacking either:

```sql
WITH pairs AS (
  SELECT orcid, a.xml AS old_xml, b.xml AS new_xml
  FROM orcid_documents('test/data/releases/ORCID_2024_10_summaries.tar.gz') a
  FULL JOIN orcid_documents('test/data/releases/ORCID_2025_10_summaries.tar.gz') b USING (orcid))
SELECT c.section, c.change, count(*) AS n
FROM pairs p, orcid_changes_xml(p.old_xml, p.new_xml) c
GROUP BY ALL ORDER BY c.section, c.change;
```

On full releases, compare one iD prefix at a time (`members := '*/097/*'` on
both sides) so each pass reads about 1/1000 of each archive's members.
`examples/tasks/release-changes.sql` wraps this
(`task_release_changes`, `task_release_change_summary`).

## 11. Locked and deprecated records

The Public Data File serves a locked record (error code 9018) or a deprecated
one (9007) as an error document in place of the record. `orcid_errors` reads
the iD from the message, and for a deprecated record the primary account it
points to:

```sql
SELECT orcid, response_code, error_code, primary_orcid
FROM orcid_errors('test/data/errors/*.xml')
ORDER BY orcid;
```

Neither document yields `person`, `history` or `works` rows. To list them
across a dump, read `orcid_errors` over the archive; see
[LOCKED_DEPRECATED.md](LOCKED_DEPRECATED.md).

## 12. API messages outside the record

The same readers parse the ORCID API's other v3.0 messages.

```sql
-- 12.1 the record summary (/v3.0/{iD}/summary): ORCID's per-type counts
SELECT orcid, credit_name, employment_count, work_validated_count, peer_review_total, email_domains
FROM orcid_summary('reference/orcid-model/summary_3.0/samples/summary-3.0.xml');
SELECT kind, count(*) FROM orcid_summary_items('reference/orcid-model/summary_3.0/samples/summary-3.0.xml')
GROUP BY kind ORDER BY kind;

-- 12.2 search and expanded-search results
SELECT num_found, ordinal, orcid FROM orcid_search_results('reference/orcid-model/record_3.0/samples/read_samples/search-3.0.xml');

-- 12.3 errors, standalone or per entry of a bulk response (position = entry index)
SELECT response_code, error_code, user_message, container, position
FROM orcid_errors('reference/orcid-model/record_3.0/samples/read_samples/bulk-work-err.xml');

-- 12.4 inbox notifications (permission and custom)
SELECT notification_type, subject, item_count, item_types
FROM orcid_notifications('reference/orcid-model/notification_3.0/samples/notification-permission-3.0.xml');

-- 12.5 peer-review group ids
SELECT name, group_id, type FROM orcid_group_ids('reference/orcid-model/group-id-3.0/samples/group-id-3.0.xml');
```

Reading these messages needs no API access: the extension parses
documents you already have. It never calls an API itself.

## 13. Export to Parquet and CSV

`COPY` writes any query. Parquet needs the `parquet` extension, which
DuckDB's release builds include; CSV is core.

```sql
COPY (SELECT * FROM orcid_works('test/data/*.xml')) TO 'works.parquet' (FORMAT parquet);
COPY (SELECT orcid, put_code, title, pub_year, doi FROM orcid_works('test/data/*.xml'))
  TO 'works.csv' (HEADER, DELIMITER ',');

-- one Parquet file per section of a dump, for downstream tools
COPY (SELECT * FROM orcid_affiliations('ORCID_2025_10_summaries.tar.gz'))
  TO 'affiliations.parquet' (FORMAT parquet, COMPRESSION zstd);
```

Reading a dump once into DuckDB tables, then exporting, is cheaper than
reading the archive once per section:

```sql
CREATE TABLE docs AS SELECT * FROM orcid_documents('ORCID_2025_10_summaries.tar.gz', max_records := 100000);
CREATE TABLE works AS SELECT w.* FROM docs d, orcid_works_xml(d.xml) w;
```

## 14. Performance

### 14.1 Threads and row order

The file readers use DuckDB's `threads`. Plain files are parsed in parallel;
archive members are decompressed one after another and parsed in parallel.
With `preserve_insertion_order` on (the default), rows come in document order
however many threads run. Turning it off lets inserts run in parallel too
(faster `CREATE TABLE AS`); add `ORDER BY` wherever order matters:

```sql
SET threads = 4;
SET preserve_insertion_order = false;
CREATE TABLE works AS SELECT * FROM orcid_works('test/data/*.xml');
```

### 14.2 The progress bar

The readers report progress: by file for plain files, and by compressed bytes
consumed for an archive.

```sql
SET enable_progress_bar = true;
```

### 14.3 One document in a table: use the constant-argument form

DuckDB plans `FROM t, orcid_works_xml(t.xml)` as a join keyed on the whole
document. When `t` has **exactly one row**, every output row then keeps its
own copy of the document (500 MB for the 1.3 MB fixture record, 7.5 GB for a
5.4 MB record). Pass a one-row table's document as a constant argument
instead:

```sql
CREATE TEMP TABLE stage AS SELECT content AS xml FROM read_text('test/data/0000-0003-0100-6634.xml');
SELECT count(*) FROM orcid_works_xml((SELECT xml FROM stage));
```

With two or more rows, the lateral form is fine (REFERENCE.md, "Memory of
lateral joins").

### 14.4 Dumps

- Use `max_records` while exploring; `members` to select an iD prefix.
- Read an archive once (`orcid_documents` into a table, or one `CREATE TABLE
  AS` per section you need) rather than once per query.
- Keep `max_member_bytes` at its default unless a member is legitimately
  larger. The largest record seen in the summaries corpus is 5.4 MB.

## 15. DuckDB v2.0

On the v2.0 pre-release the file readers become multi-file readers. The
queries above work unchanged, and v2.0 adds:

```sql
-- a list of paths, and the filename virtual column
SELECT filename, count(*) FROM orcid_works(['test/data/demo/0000-0002-2771-9344.xml',
                                            'test/data/activities/0000-0002-8635-8390_works_30089981.xml'])
GROUP BY filename;
```

plus `union_by_name` and `hive_partitioning`. The one-row lateral plan of
recipe 14.3 is the same on v2.0. See [DUCKDB2.md](DUCKDB2.md).

## 16. Read ORCID JSON

The readers take ORCID's JSON as they take its XML: the format is told per
file and per archive member, and the rows are the same
([REFERENCE.md, JSON documents](REFERENCE.md#json-documents)).

```sql
-- 16.1 a record saved as JSON (GET /v3.0/{iD}/record with Accept: application/vnd.orcid+json)
SELECT orcid, put_code, title, type, created
FROM orcid_works('reference/orcid-model/record_3.0/samples/read_samples/record-full-3.0.json');

-- 16.2 a glob or a directory mixing both formats
SELECT title, item FROM orcid_works('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.*');

-- 16.3 JSON held in a column: the _json twins
SELECT a.affiliation_type, a.org_name, a.start_year
FROM read_text('reference/orcid-model/record_3.0/samples/read_samples/record-full-3.0.json') t,
     orcid_affiliations_json(t.content) a
ORDER BY a.put_code;

-- 16.4 a single affiliation, keyword or other name: ORCID's JSON does not name
-- the item type (the URL does), so say which with json_root
SELECT department, role_title, org_name
FROM orcid_affiliations('reference/orcid-model/record_3.0/samples/write_samples/employment-3.0.json',
                        json_root := 'employment');
```

## 17. Write a work as an ORCID document

The writers return an ORCID v3.0 document as a string; nothing is sent to
ORCID. The STRUCT's fields are the columns of `orcid_works`, plus lists
of external ids and contributors ([REFERENCE.md, Writing ORCID
documents](REFERENCE.md#writing-orcid-documents)).

```sql
-- 17.1 from values
SELECT orcid_work_to_json({
    'title': 'Hydrothermal vent communities', 'type': 'journal-article',
    'journal': 'Deep-Sea Research', 'pub_year': 2024, 'pub_month': 3,
    'external_ids': [{'type': 'doi', 'value': '10.1234/vents.2024', 'relationship': 'self'}],
    'contributors': [{'credit_name': 'A. Researcher', 'sequence': 'first', 'role': 'author'}]});

-- 17.2 a work read from a record, with its identifiers and contributors, back to XML
WITH w AS (SELECT * FROM orcid_works('test/data/activities/0000-0002-8635-8390_works_30089981.xml'))
SELECT orcid_work_to_xml(struct_insert(w,
         external_ids := (SELECT list(e) FROM orcid_external_ids('test/data/activities/0000-0002-8635-8390_works_30089981.xml') e
                          WHERE e.owner_put_code = w.put_code),
         contributors := (SELECT list(c ORDER BY c.ordinal)
                          FROM orcid_contributors('test/data/activities/0000-0002-8635-8390_works_30089981.xml') c
                          WHERE c.owner_put_code = w.put_code)))
FROM w;
```

A missing required field is an error naming it (`'title' is required`), and
so is a field the writer does not know (`unknown field 'titel'`). Read-only
columns of a row (source, dates, path) are accepted and not written.

## 18. Bulk documents

A bulk document holds up to 100 works.

```sql
-- 18.1 every work of a record, as bulk documents of at most 100 works
WITH w AS (SELECT * FROM orcid_works('test/data/demo/0000-0002-2771-9344.xml')),
     b AS (SELECT put_code, (row_number() OVER (ORDER BY put_code) - 1) // 100 AS batch FROM w)
SELECT b.batch, count(*) AS works, orcid_bulk_works_to_json(list(w ORDER BY w.put_code)) AS body
FROM w JOIN b USING (put_code)
GROUP BY b.batch ORDER BY b.batch;
```

`orcid_bulk_works_to_*` refuses more than 100 works.

## 19. Convert between XML and JSON

Read one format, write the other: the rows are the same either way.

```sql
-- 19.1 a work document from XML to JSON
SELECT orcid_work_to_json(struct_insert(w,
         external_ids := (SELECT list(e) FROM orcid_external_ids('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml') e),
         contributors := (SELECT list(c ORDER BY c.ordinal) FROM orcid_contributors('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml') c)))
FROM orcid_works('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml') w;

-- 19.2 and an employment from JSON to XML (the type comes from json_root, then from affiliation_type)
SELECT orcid_affiliation_to_xml(struct_insert(a,
         external_ids := (SELECT list(e) FROM orcid_external_ids('reference/orcid-model/record_3.0/samples/write_samples/employment-3.0.json',
                                                                 json_root := 'employment') e)))
FROM orcid_affiliations('reference/orcid-model/record_3.0/samples/write_samples/employment-3.0.json', json_root := 'employment') a;
```
