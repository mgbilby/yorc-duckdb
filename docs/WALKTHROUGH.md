# orcid: a walkthrough

Nine small steps with the extension, from checking an iD to getting a record
from ORCID's Public API, after a setup cell 0. Every step is a numbered cell:
a goal, the SQL, and the output to expect. Everything runs in the DuckDB
command-line shell, on files that ship with this package. No Jupyter, Python or network is needed
(cell 9, the one network step, is shown but not run).

yorc-duckdb is an independent open-source project, not affiliated with, endorsed by or sponsored by ORCID, Inc.; it does not provide access to the ORCID Sandbox or the ORCID Member API, and makes no requests to ORCID.

Run all the cells at once from the package root:

```sh
duckdb < docs/walkthrough.sql
```

or, inside `duckdb` started in the package root, `.read docs/walkthrough.sql`.
Each cell prints its number and title (`Cell 3. ...`) before its output, so
you can compare what you see with the "Expected output" blocks below (the
file also prints a blank line before each cell). To run
a single cell instead, paste its SQL block into `duckdb` after cell 0.

This is the beginner's path. [RECIPES.md](RECIPES.md) has 19 recipes for the
jobs people bring ORCID data to DuckDB for, and
[REFERENCE.md](REFERENCE.md) every function and column.

**Inputs**, all relative to the package root:

| file | what it is |
|---|---|
| `test/data/demo/0000-0002-2771-9344.xml` | a real public ORCID record (CC0), saved from the Public API |
| `reference/orcid-model/record_3.0/samples/read_samples/` | ORCID's read samples: a full record (`record-3.0.xml`, for cell 7) and a full work (`work-full-3.0.xml`) |
| `reference/orcid-model/record_3.0/samples/write_samples/` | ORCID's write samples: a full work as XML and as JSON (`work-full-3.0.xml`, `work-full-3.0.json`) |
| `test/data/dump/orcid_test_summaries.tar.gz` | a small Public Data File dump: three documents |

Cell 5 writes `walkthrough_works.csv` to the current directory; delete it
afterwards.

The record is 0000-0002-2771-9344, a public record that ORCID staff use as an
example (its owner, Sofia Maria Garcia Hernandez, is a fictitious person): 12
works and four other names, but no fundings, affiliations or peer reviews.
Cell 7 therefore reads ORCID's sample record instead.

**Expected outputs** are those of DuckDB v1.5.6. An HTML comment beside each
block says how its values follow from the input files (grep, tar, python3 -I).
The same queries and results are pinned by `test/sql/orcid_walkthrough.test`.

| cell | |
|---|---|
| [0](#cell-0) | Setup: load the extension, the version check and the paths |
| [1](#cell-1) | Check and normalise ORCID iDs |
| [2](#cell-2) | Read a saved ORCID record and summarise it |
| [3](#cell-3) | List the record's works with year, type and DOI, newest first, top 10 |
| [4](#cell-4) | Read ORCID JSON, and convert a document XML→JSON→XML |
| [5](#cell-5) | Flatten the record into section tables and export one as CSV |
| [6](#cell-6) | Contributors and co-authors of the works |
| [7](#cell-7) | An affiliation timeline |
| [8](#cell-8) | Stream the Public Data File dump fixture |
| [9](#cell-9) | Getting one public record from ORCID |

## Cell 0

**Setup: load the extension, the version check and the paths.** Goal: Load the extension, see which DuckDB runs it, and check that the six input files are where the cells expect them.

All paths are relative to the package root, so start `duckdb` there. With a
DuckDB from the community repository, install the extension once first:
`duckdb -c "INSTALL yorc FROM community"`. A shell built from this repository
(`./build/release/duckdb`) has it linked in, and `LOAD yorc` is then a no-op.
`.mode box` draws the tables below; `.pager off` keeps long results on screen.

<!-- cell 0 -->
```sql
.print "Cell 0. Setup: load the extension, the version check and the paths"
.mode box
.pager off
LOAD yorc;
SELECT version() AS duckdb, extension_name, loaded
FROM duckdb_extensions()
WHERE extension_name = 'yorc';
SELECT filename, size
FROM read_blob(['test/data/demo/0000-0002-2771-9344.xml',
                'reference/orcid-model/record_3.0/samples/read_samples/record-3.0.xml',
                'reference/orcid-model/record_3.0/samples/read_samples/work-full-3.0.xml',
                'reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml',
                'reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.json',
                'test/data/dump/orcid_test_summaries.tar.gz'])
ORDER BY filename;
```

Expected output:

<!-- derived: duckdb: the version this package targets (README badge, duckdb submodule tag v1.5.6); another DuckDB prints its own. size: stat -c %s on each file. -->
```text
Cell 0. Setup: load the extension, the version check and the paths
┌────────┬────────────────┬────────┐
│ duckdb │ extension_name │ loaded │
├────────┼────────────────┼────────┤
│ v1.5.6 │ yorc           │ true   │
└────────┴────────────────┴────────┘
┌───────────────────────────────────────────────────────────────────────────┬───────┐
│ filename                                                                  │ size  │
├───────────────────────────────────────────────────────────────────────────┼───────┤
│ reference/orcid-model/record_3.0/samples/read_samples/record-3.0.xml      │ 36958 │
│ reference/orcid-model/record_3.0/samples/read_samples/work-full-3.0.xml   │ 2922  │
│ reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.json │ 2358  │
│ reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml  │ 2500  │
│ test/data/demo/0000-0002-2771-9344.xml                                    │ 45543 │
│ test/data/dump/orcid_test_summaries.tar.gz                                │ 64616 │
└───────────────────────────────────────────────────────────────────────────┴───────┘
```

`extension_version` in `duckdb_extensions()` gives the extension's build
(`v0.1.0` for the release, a commit hash for a local build), so it is left out.
If a file is missing, `read_blob` names it: you are not in the package root.

## Cell 1

**Check and normalise ORCID iDs.** Goal: Tell a valid iD from an invalid one, and turn URL and spaced forms into the canonical `XXXX-XXXX-XXXX-XXXX`.

`orcid_valid` checks the ISO 7064 MOD 11-2 check character; `orcid_normalize`
returns the canonical form, or NULL when the input is not an iD. Both accept an
`https://orcid.org/` prefix, spaces anywhere, missing hyphens and a lowercase
`x`. The fourth input has a space at each end, which the box shows.

<!-- cell 1 -->
```sql
.print "Cell 1. Check and normalise ORCID iDs"
SELECT input, orcid_valid(input) AS valid, orcid_normalize(input) AS normalized,
       'https://orcid.org/' || orcid_normalize(input) AS uri
FROM (VALUES (1, '0000-0002-1825-0097'),
             (2, '0000-0002-1825-0098'),
             (3, 'https://orcid.org/000000021694233x'),
             (4, ' 0000 0002 2771 9344 ')) AS t(n, input)
ORDER BY n;
```

Expected output:

<!-- derived: The same four inputs as the Rust walkthrough (yorc-rs/WALKTHROUGH.md, cell 1). ISO 7064 MOD 11-2 computed with python3 -I over the first 15 digits: 000000021825009 gives 7 (so ...0098 is invalid), 000000021694233 gives X, 000000022771934 gives 4. URL prefix, missing hyphens, lowercase x, leading, inner and trailing spaces: REFERENCE.md, Scalars (orcid_normalize). NULL || text is NULL. -->
```text
Cell 1. Check and normalise ORCID iDs
┌────────────────────────────────────┬───────┬─────────────────────┬───────────────────────────────────────┐
│ input                              │ valid │ normalized          │ uri                                   │
├────────────────────────────────────┼───────┼─────────────────────┼───────────────────────────────────────┤
│ 0000-0002-1825-0097                │ true  │ 0000-0002-1825-0097 │ https://orcid.org/0000-0002-1825-0097 │
│ 0000-0002-1825-0098                │ false │ NULL                │ NULL                                  │
│ https://orcid.org/000000021694233x │ true  │ 0000-0002-1694-233X │ https://orcid.org/0000-0002-1694-233X │
│  0000 0002 2771 9344               │ true  │ 0000-0002-2771-9344 │ https://orcid.org/0000-0002-2771-9344 │
└────────────────────────────────────┴───────┴─────────────────────┴───────────────────────────────────────┘
```

## Cell 2

**Read a saved ORCID record and summarise it.** Goal: Read the saved record (the real CC0 record of 0000-0002-2771-9344) and count its works, fundings, affiliations and peer reviews.

Every section has a reader, `orcid_<section>(path)`, that returns one row per
item. Affiliations are the seven types (employment, education, distinction,
membership, service, qualification, invited position) in one table. This
record has works only, so the other three counts are 0.

<!-- cell 2 -->
```sql
.print "Cell 2. Read a saved ORCID record and summarise it"
SELECT p.orcid, p.given_names, p.family_name, p.credit_name,
       (SELECT count(*) FROM orcid_works('test/data/demo/0000-0002-2771-9344.xml'))        AS works,
       (SELECT count(*) FROM orcid_fundings('test/data/demo/0000-0002-2771-9344.xml'))     AS fundings,
       (SELECT count(*) FROM orcid_affiliations('test/data/demo/0000-0002-2771-9344.xml')) AS affiliations,
       (SELECT count(*) FROM orcid_peer_reviews('test/data/demo/0000-0002-2771-9344.xml')) AS peer_reviews
FROM orcid_person('test/data/demo/0000-0002-2771-9344.xml') p;
```

Expected output:

<!-- derived: grep -c over test/data/demo/0000-0002-2771-9344.xml: '<work:work-summary' 12, '<funding:funding-summary' 0, '<peer-review:peer-review-summary' 0, and no *-summary of the seven affiliation types (their containers are empty elements). Name: lines 24-26 (personal-details:given-names, family-name, credit-name). -->
```text
Cell 2. Read a saved ORCID record and summarise it
┌─────────────────────┬─────────────┬─────────────┬──────────────────────────────┬───────┬──────────┬──────────────┬──────────────┐
│ orcid               │ given_names │ family_name │ credit_name                  │ works │ fundings │ affiliations │ peer_reviews │
├─────────────────────┼─────────────┼─────────────┼──────────────────────────────┼───────┼──────────┼──────────────┼──────────────┤
│ 0000-0002-2771-9344 │ Sofia       │ Hernandez   │ Sofia Maria Garcia Hernandez │ 12    │ 0        │ 0            │ 0            │
└─────────────────────┴─────────────┴─────────────┴──────────────────────────────┴───────┴──────────┴──────────────┴──────────────┘
```

## Cell 3

**List the record's works with year, type and DOI, newest first, top 10.** Goal: The ten newest works, by publication date, with their type and DOI.

`doi` is the work's own DOI (its first `self` DOI, lowercased). A record can
list a work several times, once per source or deposit: here the three newest
rows are one data set in work group 1, deposited three times under two DOIs
(each recorded as a version of the other), so the same title appears with
different put-codes.

<!-- cell 3 -->
```sql
.print "Cell 3. List the record's works with year, type and DOI, newest first, top 10"
SELECT put_code, pub_year, type, doi, title
FROM orcid_works('test/data/demo/0000-0002-2771-9344.xml')
ORDER BY pub_year DESC NULLS LAST, pub_month DESC NULLS LAST, pub_day DESC NULLS LAST, put_code
LIMIT 10;
```

Expected output:

<!-- derived: python3 -I (xml.etree) over the 12 work-summary elements: put-code, publication-date year/month/day, type, first external-id with type doi and relationship self (lowercased), title; sorted by year, month, day descending (missing last), then put-code ascending. -->
```text
Cell 3. List the record's works with year, type and DOI, newest first, top 10
┌───────────┬──────────┬─────────────────┬────────────────────────────────┬───────────────────────────────────────────────────────────────────────────────────────────────┐
│ put_code  │ pub_year │ type            │ doi                            │ title                                                                                         │
├───────────┼──────────┼─────────────────┼────────────────────────────────┼───────────────────────────────────────────────────────────────────────────────────────────────┤
│ 224052943 │ 2026     │ data-set        │ 10.25592/uhhfdm.19102          │ Point cloud calorimeter shower datasets for multi-geometry pre-training: SimpleBox and LEMURS │
│ 224052944 │ 2026     │ data-set        │ 10.25592/uhhfdm.19103          │ Point cloud calorimeter shower datasets for multi-geometry pre-training: SimpleBox and LEMURS │
│ 224052945 │ 2026     │ data-set        │ 10.25592/uhhfdm.19102          │ Point cloud calorimeter shower datasets for multi-geometry pre-training: SimpleBox and LEMURS │
│ 37069489  │ 2017     │ other           │ 10.5281/zenodo.495149          │ Introducing Orcid                                                                             │
│ 43318438  │ 2017     │ other           │ 10.6084/m9.figshare.5479792.v1 │ ORCID Public Data File 2017                                                                   │
│ 37069490  │ 2016     │ other           │ 10.5281/zenodo.168190          │ Introducing Orcid: What, Why And How                                                          │
│ 37069241  │ 2016     │ data-set        │ 10.6084/m9.figshare.4134027.v1 │ ORCID Public Data File 2016                                                                   │
│ 37069473  │ 2016     │ journal-article │ 10.1002/chemv.201600091        │ Your Lifelong Digital Name                                                                    │
│ 37069249  │ 2015     │ data-set        │ 10.6084/m9.figshare.1582705    │ ORCID Public Data File 2015                                                                   │
│ 37069240  │ 2014     │ data-set        │ 10.14454/07243.2014.001        │ ORCID Annual Public Data File, 2014                                                           │
└───────────┴──────────┴─────────────────┴────────────────────────────────┴───────────────────────────────────────────────────────────────────────────────────────────────┘
```

Ties on the date are broken by put-code so the order is the same on every run.

## Cell 4

**Read ORCID JSON, and convert a document XML→JSON→XML.** Goal: Read ORCID's JSON write sample of a full work with the same readers as XML, then take the XML sample through JSON and back, in yorc-rs's bytes.

The readers tell XML from JSON by the first character of each file, so
`orcid_works` and its sister sections read ORCID's JSON as they read XML. The
writers go the other way: `orcid_work_to_json` and `orcid_work_to_xml` take a
row of `orcid_works` with its identifiers and contributors added as lists
(recipe 19). The trailing `true` is crosswalk mode: the exact bytes
yorc-rs (the Rust package) writes. The `_json` and `_xml` twins read a
document held in a table.

<!-- cell 4 -->
```sql
.print "Cell 4. Read ORCID JSON, and convert a document XML→JSON→XML"
.print "4a. ORCID JSON: the write sample work-full-3.0.json"
SELECT title, type, make_date(pub_year, pub_month, pub_day) AS published,
       (SELECT count(*) FROM orcid_external_ids('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.json')) AS external_ids
FROM orcid_works('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.json');
SELECT credit_name, contributor_orcid
FROM orcid_contributors('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.json')
ORDER BY ordinal;
.print "4b. work-full-3.0.xml to JSON and back to XML, in crosswalk mode"
CREATE OR REPLACE TEMP TABLE rec_xml AS
SELECT struct_insert(w,
         external_ids := (SELECT list(e) FROM orcid_external_ids('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml') e),
         contributors := (SELECT list(c ORDER BY c.ordinal) FROM orcid_contributors('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml') c)) AS rec
FROM orcid_works('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml') w;
CREATE OR REPLACE TEMP TABLE as_json AS
SELECT orcid_work_to_json(rec, true) AS doc FROM rec_xml;
CREATE OR REPLACE TEMP TABLE rec_json AS
SELECT struct_insert(w,
         external_ids := (SELECT list(e) FROM orcid_external_ids_json((SELECT doc FROM as_json)) e),
         contributors := (SELECT list(c ORDER BY c.ordinal) FROM orcid_contributors_json((SELECT doc FROM as_json)) c)) AS rec
FROM orcid_works_json((SELECT doc FROM as_json)) w;
SELECT array_to_string(regexp_extract_all(doc, '(?m)^  "([a-z-]+)":', 1), ', ') AS json_keys
FROM as_json;
SELECT (SELECT count(*)
        FROM (SELECT * EXCLUDE (source_hash) FROM orcid_works('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml')
              EXCEPT
              SELECT * EXCLUDE (source_hash) FROM orcid_works_json((SELECT doc FROM as_json)))) = 0 AS same_rows,
       (SELECT orcid_work_to_xml(rec, true) FROM rec_json) = (SELECT orcid_work_to_xml(rec, true) FROM rec_xml) AS same_xml;
```

Expected output:

<!-- derived: 4a: python3 -I (json) over reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.json: title, type, publication-date 2022-05-24, 2 external-id, contributors Jordan (0000-0002-5391-5437) and Fran (0000-0002-4325-871X), as the Rust walkthrough prints. 4b: the 11 child elements of reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml under their JSON names, sorted (crosswalk JSON sorts keys; top-level keys are the lines with a two-space indent; test/data/crosswalk/write_samples/work-full-3.0.json has the same 11). same_rows and same_xml: the round trip keeps every column but source_hash (REFERENCE.md, Writing ORCID documents; test/sql/orcid_write.test, XML to JSON to XML), so the XML written from either side is the same. -->
```text
Cell 4. Read ORCID JSON, and convert a document XML→JSON→XML
4a. ORCID JSON: the write sample work-full-3.0.json
┌────────────┬─────────────────┬────────────┬──────────────┐
│ title      │ type            │ published  │ external_ids │
├────────────┼─────────────────┼────────────┼──────────────┤
│ Work Title │ journal-article │ 2022-05-24 │ 2            │
└────────────┴─────────────────┴────────────┴──────────────┘
┌─────────────┬─────────────────────┐
│ credit_name │ contributor_orcid   │
├─────────────┼─────────────────────┤
│ Jordan      │ 0000-0002-5391-5437 │
│ Fran        │ 0000-0002-4325-871X │
└─────────────┴─────────────────────┘
4b. work-full-3.0.xml to JSON and back to XML, in crosswalk mode
┌────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┐
│ json_keys                                                                                                                          │
├────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┤
│ citation, contributors, country, external-ids, journal-title, language-code, publication-date, short-description, title, type, url │
└────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────┘
┌───────────┬──────────┐
│ same_rows │ same_xml │
├───────────┼──────────┤
│ true      │ true     │
└───────────┴──────────┘
```

## Cell 5

**Flatten the record into section tables and export one as CSV.** Goal: Turn the record into one table per section, count their rows and columns, then write the works table to a CSV file.

Each `CREATE TABLE ... AS SELECT * FROM orcid_<section>(...)` is one flat
table; every row carries the record's iD (`orcid`) and `source_hash`, and items
join their identifiers on `(source_hash, put_code)` (recipe 5). Sections the
record has no items for (fundings, affiliations, peer reviews, research
resources, full-work contributors, search results, errors) are empty tables
with the right columns. `COPY` writes any
table; CSV is core DuckDB, Parquet works the same way (recipe 13).

<!-- cell 5 -->
```sql
.print "Cell 5. Flatten the record into section tables and export one as CSV"
CREATE OR REPLACE TABLE works                   AS SELECT * FROM orcid_works('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE fundings                AS SELECT * FROM orcid_fundings('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE affiliations            AS SELECT * FROM orcid_affiliations('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE peer_reviews            AS SELECT * FROM orcid_peer_reviews('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE research_resources      AS SELECT * FROM orcid_research_resources('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE research_resource_items AS SELECT * FROM orcid_research_resource_items('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE contributors            AS SELECT * FROM orcid_contributors('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE external_ids            AS SELECT * FROM orcid_external_ids('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE person                  AS SELECT * FROM orcid_person('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE person_items            AS SELECT * FROM orcid_person_items('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE history                 AS SELECT * FROM orcid_history('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE search_results          AS SELECT * FROM orcid_search_results('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE errors                  AS SELECT * FROM orcid_errors('test/data/demo/0000-0002-2771-9344.xml');
CREATE OR REPLACE TABLE research_resource_hosts AS SELECT * FROM orcid_research_resource_hosts('test/data/demo/0000-0002-2771-9344.xml');
WITH cols AS (SELECT table_name, count(*) AS column_count
              FROM duckdb_columns()
              WHERE schema_name = 'main' AND database_name = current_database()
              GROUP BY table_name)
SELECT r.section_table, r.row_count, cols.column_count
FROM (SELECT 1 AS n, 'works' AS section_table, count(*) AS row_count FROM works
      UNION ALL SELECT 2, 'fundings', count(*) FROM fundings
      UNION ALL SELECT 3, 'affiliations', count(*) FROM affiliations
      UNION ALL SELECT 4, 'peer_reviews', count(*) FROM peer_reviews
      UNION ALL SELECT 5, 'research_resources', count(*) FROM research_resources
      UNION ALL SELECT 6, 'research_resource_items', count(*) FROM research_resource_items
      UNION ALL SELECT 7, 'contributors', count(*) FROM contributors
      UNION ALL SELECT 8, 'external_ids', count(*) FROM external_ids
      UNION ALL SELECT 9, 'person', count(*) FROM person
      UNION ALL SELECT 10, 'person_items', count(*) FROM person_items
      UNION ALL SELECT 11, 'history', count(*) FROM history
      UNION ALL SELECT 12, 'search_results', count(*) FROM search_results
      UNION ALL SELECT 13, 'errors', count(*) FROM errors
      UNION ALL SELECT 14, 'research_resource_hosts', count(*) FROM research_resource_hosts) r
JOIN cols ON cols.table_name = r.section_table
ORDER BY r.n;
COPY works TO 'walkthrough_works.csv' (HEADER, DELIMITER ',');
.mode list
.headers off
SELECT array_to_string(string_split(content, chr(10))[1:3], chr(10)) FROM read_text('walkthrough_works.csv');
.headers on
.mode box
```

Expected output:

<!-- derived: python3 -I (xml.etree) over the record, with the item patterns of REFERENCE.md: works 12, fundings, affiliations and peer_reviews 0 (as cell 2); external_ids 26 = every common:external-id under an external-ids element (work groups 11, work-summary 15); person 1; person_items 4 = other-name 4 (the researcher-url, email, address, keyword and external-identifier containers are empty); history 1; no research resources, no contributors in summaries, no search or error document: 0. Columns: the tables of REFERENCE.md (the 14 sections and counts of the Rust walkthrough, cell 5). CSV: the header is the works columns in order; the first two rows are the first two work-summary elements of the record (both in group 1), NULL written empty, nothing needs quoting; source_hash = sha256sum of the record, 59018b6d315be538bb84a6105ff3ac1c4c274f3b467a455eb2576e03bbc2f86e. -->
```text
Cell 5. Flatten the record into section tables and export one as CSV
┌─────────────────────────┬───────────┬──────────────┐
│ section_table           │ row_count │ column_count │
├─────────────────────────┼───────────┼──────────────┤
│ works                   │ 12        │ 32           │
│ fundings                │ 0         │ 37           │
│ affiliations            │ 0         │ 32           │
│ peer_reviews            │ 0         │ 38           │
│ research_resources      │ 0         │ 32           │
│ research_resource_items │ 0         │ 13           │
│ contributors            │ 0         │ 12           │
│ external_ids            │ 26        │ 13           │
│ person                  │ 1         │ 15           │
│ person_items            │ 4         │ 24           │
│ history                 │ 1         │ 17           │
│ search_results          │ 0         │ 13           │
│ errors                  │ 0         │ 10           │
│ research_resource_hosts │ 0         │ 12           │
└─────────────────────────┴───────────┴──────────────┘
orcid,put_code,group_index,display_index,title,subtitle,journal,type,pub_year,pub_month,pub_day,doi,url,source_name,visibility,created,last_modified,source_hash,translated_title,translated_title_language,short_description,citation_type,citation_value,language_code,country,path,source_client_id,source_orcid,assertion_origin_name,assertion_origin_orcid,assertion_origin_client_id,item
0000-0002-2771-9344,224052945,1,1,Point cloud calorimeter shower datasets for multi-geometry pre-training: SimpleBox and LEMURS,,Universität Hamburg,data-set,2026,8,17,10.25592/uhhfdm.19102,,DataCite,public,2026-08-17T12:42:27.716Z,2026-08-17T12:42:27.716Z,59018b6d315be538bb84a6105ff3ac1c4c274f3b467a455eb2576e03bbc2f86e,,,,,,,,/0000-0002-2771-9344/work/224052945,0000-0001-8099-6984,,,,,work-summary
0000-0002-2771-9344,224052944,1,1,Point cloud calorimeter shower datasets for multi-geometry pre-training: SimpleBox and LEMURS,,Universität Hamburg,data-set,2026,8,17,10.25592/uhhfdm.19103,,DataCite,public,2026-08-17T12:42:27.716Z,2026-08-17T12:42:27.716Z,59018b6d315be538bb84a6105ff3ac1c4c274f3b467a455eb2576e03bbc2f86e,,,,,,,,/0000-0002-2771-9344/work/224052944,0000-0001-8099-6984,,,,,work-summary
```

The CSV file has the header and all 12 works; the cell prints its first three
lines. `walkthrough_works.csv` is written to the current directory (the
package root): delete it when you are done.

## Cell 6

**Contributors and co-authors of the works.** Goal: List the contributors of ORCID's two full work samples (XML), and count the distinct co-authors they name.

A record's work summaries carry no contributors; full works do (the
`/work/{put-code}` documents, and the activities archives of the Public Data
File). ORCID's samples have two full works in XML, one in its read samples and
one in its write samples, each with one contributor. To pair co-authors of the
same work across many works, join the contributors on
`(source_hash, owner_put_code)` (recipe 7).

<!-- cell 6 -->
```sql
.print "Cell 6. Contributors and co-authors of the works"
SELECT credit_name, contributor_orcid, sequence, role
FROM (SELECT 'read' AS sample, *
      FROM orcid_contributors('reference/orcid-model/record_3.0/samples/read_samples/work-full-3.0.xml')
      UNION ALL
      SELECT 'write', *
      FROM orcid_contributors('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml'))
ORDER BY sample, ordinal;
SELECT count(DISTINCT contributor_orcid) AS coauthors
FROM (SELECT * FROM orcid_contributors('reference/orcid-model/record_3.0/samples/read_samples/work-full-3.0.xml')
      UNION ALL
      SELECT * FROM orcid_contributors('reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml'));
```

Expected output:

<!-- derived: python3 -I (xml.etree) over reference/orcid-model/record_3.0/samples/read_samples/work-full-3.0.xml and reference/orcid-model/record_3.0/samples/write_samples/work-full-3.0.xml: one work:contributor each; contributor-orcid path, credit-name, contributor-sequence, contributor-role. Read sample first, then the write sample (the order the Rust and Zig walkthroughs print). Two distinct contributor iDs. -->
```text
Cell 6. Contributors and co-authors of the works
┌──────────────────┬─────────────────────┬──────────┬────────┐
│ credit_name      │ contributor_orcid   │ sequence │ role   │
├──────────────────┼─────────────────────┼──────────┼────────┤
│ work:credit-name │ 8888-8888-8888-8880 │ first    │ author │
│ Laure L. Haak    │ 0000-0001-5109-3700 │ first    │ author │
└──────────────────┴─────────────────────┴──────────┴────────┘
┌───────────┐
│ coauthors │
├───────────┤
│ 2         │
└───────────┘
```

## Cell 7

**An affiliation timeline.** Goal: Put employments and educations in start order, with the organisation identifiers (Ringgold, ROR, GRID) ORCID holds for them.

The record 0000-0002-2771-9344 has no affiliations, so this cell reads ORCID's
sample record (`record-3.0.xml` of the read samples, fictitious iD
8888-8888-8888-8880), which has one summary of each affiliation type; its
values are ORCID's placeholders (`education-org`, ...). An open end date means
a current position. An organisation without a
disambiguated id is one the researcher typed in; group on
`(org_disambiguation_source, org_disambiguated_id)` to count people per
institution (recipe 8).

<!-- cell 7 -->
```sql
.print "Cell 7. An affiliation timeline"
SELECT start_year, end_year, affiliation_type AS type, org_name,
       org_disambiguation_source AS id_source, org_disambiguated_id AS org_id
FROM orcid_affiliations('reference/orcid-model/record_3.0/samples/read_samples/record-3.0.xml')
WHERE affiliation_type IN ('employment', 'education')
ORDER BY start_year NULLS LAST, start_month NULLS LAST, start_day NULLS LAST, affiliation_type, put_code;
```

Expected output:

<!-- derived: python3 -I (xml.etree) over reference/orcid-model/record_3.0/samples/read_samples/record-3.0.xml, its one employment-summary and one education-summary: start-date year/month/day, end-date year, organization name, disambiguation-source, disambiguated-organization-identifier; both start on 1948-02-02, so the type decides the order (education first); the employment's end date has a year only. The same rows as test/sql/orcid_recipes.test, recipe 4.3. -->
```text
Cell 7. An affiliation timeline
┌────────────┬──────────┬────────────┬────────────────┬───────────┬─────────────────────────────────────────────────────────┐
│ start_year │ end_year │ type       │ org_name       │ id_source │ org_id                                                  │
├────────────┼──────────┼────────────┼────────────────┼───────────┼─────────────────────────────────────────────────────────┤
│ 1948       │ 2019     │ education  │ education-org  │ GRID      │ common:disambiguated-organization-identifier-education  │
│ 1948       │ 2025     │ employment │ employment-org │ GRID      │ common:disambiguated-organization-identifier-employment │
└────────────┴──────────┴────────────┴────────────────┴───────────┴─────────────────────────────────────────────────────────┘
```

Add `role_title` to the query to tell positions at the same organisation
apart. An undated item would sort last (`NULLS LAST`).

## Cell 8

**Stream the Public Data File dump fixture.** Goal: Read a Public Data File archive in place: count its records, list their iDs, and draw a sample that is the same on every run.

The ORCID Public Data File is a set of `.tar.gz` archives, one XML document per
record. The readers stream them member by member, never unpacking to disk;
`orcid_documents` lists the raw documents, with the iD taken from the member's
path. The fixture is a three-document dump in the Public Data File's layout
(records grouped by the iD's last three characters), plus a `/works` document
under a long path and a non-XML member, which is skipped.

`orcid_dump_sample(archive, n, seed)` draws a seeded sample of the records
in one pass, with the sampler of yorc-rs and yorc-zig (SplitMix64 and
Algorithm R, `dump::reservoir`): a member named by a valid iD is offered to
the sampler, and one that is not kept is never read. With capacity 1 the
first record is kept, and the second replaces it if `SplitMix64(seed).below(2)`
is 0.

<!-- cell 8 -->
```sql
.print "Cell 8. Stream the Public Data File dump fixture"
.print "8a. The documents of the dump, in archive order"
SELECT name, orcid, regexp_extract(xml, '<([A-Za-z-]+:[A-Za-z-]+)', 1) AS root, strlen(xml) AS bytes
FROM orcid_documents('test/data/dump/orcid_test_summaries.tar.gz');
.print "8b. The counts, and the iDs of the records"
SELECT count(*) AS xml_members,
       count(*) FILTER (WHERE regexp_extract(xml, '<([A-Za-z-]+:[A-Za-z-]+)', 1) = 'record:record') AS records,
       count(orcid) AS named_by_valid_id
FROM orcid_documents('test/data/dump/orcid_test_summaries.tar.gz');
SELECT string_agg(orcid, ', ' ORDER BY orcid) AS record_ids
FROM orcid_person('test/data/dump/orcid_test_summaries.tar.gz');
.print "8c. A seeded sample of the records: the same picks as yorc-rs and yorc-zig"
SELECT 42 AS seed, orcid, eligible FROM orcid_dump_sample('test/data/dump/orcid_test_summaries.tar.gz', 1, 42)
UNION ALL
SELECT 7, orcid, eligible FROM orcid_dump_sample('test/data/dump/orcid_test_summaries.tar.gz', 1, 7)
ORDER BY seed DESC;
```

Expected output:

<!-- derived: tar -tvzf test/data/dump/orcid_test_summaries.tar.gz and python3 -I (tarfile, xml.etree): three .xml members (README.txt is not .xml), sizes 1324198, 36958 and 6672 bytes; the third path holds 0000-0000-0000-0000, whose check character would be 1, so no iD; root elements record:record, record:record, activities:works. XML members 3; records (record:record documents) 2; named by a valid iD 2 (the counts the Rust and Zig walkthroughs print). 8c: the two members named by a valid iD, in archive order, and the SplitMix64 draws as specified in yorc-rs src/dump/reservoir.rs (seed 42: below(2) = 1, the second record is skipped; seed 7: below(2) = 0, it replaces the first), as the Rust and Zig walkthroughs print. -->
```text
Cell 8. Stream the Public Data File dump fixture
8a. The documents of the dump, in archive order
┌─────────────────────────────────────────────┬─────────────────────┬──────────────────┬─────────┐
│ name                                        │ orcid               │ root             │ bytes   │
├─────────────────────────────────────────────┼─────────────────────┼──────────────────┼─────────┤
│ 0000-0003-0100-6634.xml                     │ 0000-0003-0100-6634 │ record:record    │ 1324198 │
│ 8888-8888-8888-8880.xml                     │ 8888-8888-8888-8880 │ record:record    │ 36958   │
│ and-a-file-name-too-0000-0000-0000-0000.xml │ NULL                │ activities:works │ 6672    │
└─────────────────────────────────────────────┴─────────────────────┴──────────────────┴─────────┘
8b. The counts, and the iDs of the records
┌─────────────┬─────────┬───────────────────┐
│ xml_members │ records │ named_by_valid_id │
├─────────────┼─────────┼───────────────────┤
│ 3           │ 2       │ 2                 │
└─────────────┴─────────┴───────────────────┘
┌──────────────────────────────────────────┐
│ record_ids                               │
├──────────────────────────────────────────┤
│ 0000-0003-0100-6634, 8888-8888-8888-8880 │
└──────────────────────────────────────────┘
8c. A seeded sample of the records: the same picks as yorc-rs and yorc-zig
┌──────┬─────────────────────┬──────────┐
│ seed │ orcid               │ eligible │
├──────┼─────────────────────┼──────────┤
│ 42   │ 0000-0003-0100-6634 │ 2        │
│ 7    │ 8888-8888-8888-8880 │ 2        │
└──────┴─────────────────────┴──────────┘
```

**These are the picks of the Rust and Zig walkthroughs** (cell 8 there:
seed 42 keeps 0000-0003-0100-6634, seed 7 keeps 8888-8888-8888-8880): the three
packages share the sampler, the eligibility rule and the seed, so the same
archive, `n` and seed give the same records in all three
([REFERENCE.md](REFERENCE.md#orcid_dump_sample-a-seeded-sample-of-a-dump)).
DuckDB's own `USING SAMPLE reservoir(n ROWS) REPEATABLE (seed)` samples rows
of any query instead (for example works), with its own generator: reproducible
within DuckDB on one thread, but not the same picks as the libraries.

## Cell 9

**Getting one public record from ORCID.** Goal: Save one public record from ORCID's Public API with curl, then read the saved file like any other.

**Not run by default.** This is the only step that uses the network. It is
manual, and makes one request to orcid.org.

- **Public mode.** Use ORCID's Public API, `https://pub.orcid.org/v3.0/`, with
  no token: it serves the public part of any record.
- **One request, then wait.** The yorc rule caps requests to ORCID at one
  every 45 seconds, to every orcid.org host, one at a time.
- **Public API only.** The ORCID Sandbox and the ORCID Member API are out of
  scope for yorc.
- **Bulk data comes from the Public Data File**, never from crawling the API:
  the yearly dumps (cell 8; every release's files and download URLs are in
  `reference/orcid-dumps/index.csv`).

The extension makes no network calls by design, so the request is curl's.
Save the record (one request, `Accept: application/vnd.orcid+xml`):

```sh
# manual, makes one request to orcid.org
curl -sS --fail --create-dirs -o downloads/0000-0002-2771-9344.xml \
  -H 'Accept: application/vnd.orcid+xml' \
  https://pub.orcid.org/v3.0/0000-0002-2771-9344/record
```

Then read the saved file in `duckdb`, started in the same directory (after
`LOAD yorc;`):

```sql
SELECT orcid, given_names, family_name FROM orcid_person('downloads/0000-0002-2771-9344.xml');
SELECT count(*) AS works FROM orcid_works('downloads/0000-0002-2771-9344.xml');
```

These results are the live record's, so they are not fixed. When the
package's copy was saved they were `0000-0002-2771-9344`, `Sofia`, `Hernandez`
and 12 works, as in cell 2.

What `docs/walkthrough.sql` runs for this cell is only its heading and a
pointer here:

<!-- cell 9 -->
```sql
.print "Cell 9. Getting one public record from ORCID"
.print "Manual, makes one request to orcid.org: not run by this file."
.print "See docs/WALKTHROUGH.md, cell 9, for the curl command and the query."
-- After saving the record with curl (one request):
-- SELECT orcid, given_names, family_name FROM orcid_person('downloads/0000-0002-2771-9344.xml');
-- SELECT count(*) AS works FROM orcid_works('downloads/0000-0002-2771-9344.xml');
```

Expected output:

<!-- derived: The three .print lines of the cell; the rest is comments. -->
```text
Cell 9. Getting one public record from ORCID
Manual, makes one request to orcid.org: not run by this file.
See docs/WALKTHROUGH.md, cell 9, for the curl command and the query.
```

## Keeping this page and walkthrough.sql in step

`docs/walkthrough.sql` is the cells' SQL blocks above, in order, each marked
`<!-- cell N -->` here; the file adds only comments and an empty `.print`
(a blank line) before each cell. Edit a cell in both places.
`test/sql/orcid_walkthrough.test` checks that every marked block of
this page appears verbatim in `docs/walkthrough.sql` and that the file has
ten cells, and it runs the same queries against the expected results
(output files go to the test's directory instead of the current one). When a
result changes, change the "Expected output" block, its HTML comment and the
test together.
