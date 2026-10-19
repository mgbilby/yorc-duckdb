# yorc-duckdb: Cultivate your research identity and networks

The `yorc` DuckDB extension: ORCID records for DuckDB.

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![GitHub Release](https://img.shields.io/github/v/release/mgbilby/yorc-duckdb)
![GitHub Actions Workflow Status](https://img.shields.io/github/actions/workflow/status/mgbilby/yorc-duckdb/.github%2Fworkflows%2FMainDistributionPipeline.yml)
[![DuckDB](https://img.shields.io/static/v1?label=duckdb&message=v1.5.6&color=blue)](https://github.com/duckdb/duckdb/releases)
[![Community downloads per week](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fcommunity-extensions.duckdb.org%2Fdownloads-last-week.json&query=%24.yorc&label=downloads%2Fweek&color=brightgreen)](https://duckdb.org/community_extensions/download_metrics)

## Not affiliated with ORCID

yorc-duckdb is an independent open-source project. It is not affiliated with,
endorsed by or sponsored by ORCID, Inc. It does not provide access to the
ORCID Sandbox or the ORCID Member API, and makes no requests to ORCID: it
reads ORCID documents and Public Data File archives you already have.

## About

Reads and writes ORCID v3.0 XML and JSON.

**Walkthrough:** [docs/WALKTHROUGH.md](docs/WALKTHROUGH.md), numbered cells (setup, then nine
use cases) from checking an iD to getting a record from the Public API, run with the duckdb CLI alone
(`duckdb < docs/walkthrough.sql`).

Query [ORCID](https://orcid.org/) researcher records with SQL. The extension turns
ORCID v3.0 documents, XML or JSON (Public API responses, saved records, and the yearly
[ORCID Public Data File](https://info.orcid.org/documentation/integration-guide/orcid-public-data-file/)
dumps, read in place) into relational tables: works, fundings, affiliations,
peer reviews, people, identifiers, and how a record changed over time. And
back: rows become ORCID v3.0 work, funding, affiliation, peer review,
research resource and person item documents, in either format.

The documents it writes are strings or files; it sends nothing anywhere.

- Pure C++17 with no dependencies. The XML reader, the JSON parser, the writers,
  the ISO 7064 check, the tar reader and SHA-256 are written from the
  specifications, not from prior ORCID, XML or JSON software (see
  [`audit/`](audit) and [`NOTICE`](NOTICE)).
- 106 of the schema's 109 data-bearing element and attribute names are carried
  by a column ([`docs/COVERAGE.md`](docs/COVERAGE.md)).
- No network code: the extension parses documents you already have, or that
  DuckDB's `httpfs` reads for it (see [Scope](#scope)).
- Paths go through DuckDB's file system, so globs, `~`, and with `httpfs` both
  `https://` and `s3://` work. `.tar` and `.tar.gz` dumps are streamed member by
  member and never unpacked to disk.
- Every record-table row carries the record's iD and a SHA-256 of its source document, for change detection.
- The same rows as the [yorc-rs](https://github.com/mgbilby/yorc-rs) and
  [yorc-zig](https://github.com/mgbilby/yorc-zig) libraries, and in crosswalk
  mode the same written bytes, in a cross-check of ORCID's samples; timings
  in [BENCHMARKS.md](BENCHMARKS.md), reproducible with the public harness
  [yorc-bench](https://github.com/mgbilby/yorc-bench).

Current release: **0.1.0** (see [`CHANGELOG.md`](CHANGELOG.md)).

## Installation

yorc is a DuckDB Community Extension. To install and use it, run these SQL
commands in your DuckDB session:

```sql
INSTALL yorc FROM community;
LOAD yorc;
```

That's it.

## Quick start

```sql
-- iDs: check digit and canonical form
SELECT orcid_normalize('https://orcid.org/0000-0002-1825-0097');  -- 0000-0002-1825-0097
SELECT orcid_valid('0000-0002-1825-0098');                         -- false

-- one row per work, from saved records (any /record, /works, /work or bulk response)
SELECT orcid, title, type, pub_year, doi
FROM orcid_works('records/*.xml')
ORDER BY pub_year DESC;

-- XML already in a table: the _xml twin in a lateral join
SELECT r.orcid, a.affiliation_type, a.org_name, a.start_year
FROM records r, orcid_affiliations_xml(r.xml) a;

-- ORCID's JSON reads into the same tables (the format is told per file; _json twins for columns)
SELECT orcid, title, type, pub_year, doi FROM orcid_works('records/*.json');

-- and back: a work as an ORCID JSON document (or _to_xml), from a STRUCT of the reader's columns
SELECT orcid_work_to_json({'title': 'Deep-sea vents', 'type': 'journal-article', 'pub_year': 2024,
                           'external_ids': [{'type': 'doi', 'value': '10.1234/vents', 'relationship': 'self'}]});

-- a Public Data File dump, straight from figshare (only the bytes needed are read)
INSTALL httpfs; LOAD httpfs;
SELECT orcid, given_names, family_name
FROM orcid_person('https://ndownloader.figshare.com/files/<file-id>', max_records := 1000);

-- what changed in a record between two snapshots, field by field
SELECT section, kind, put_code, change, field, old_value, new_value
FROM orcid_changes('2024/0000-0002-1825-0097.xml', '2025/0000-0002-1825-0097.xml');
```

Every example on this page runs as a test on the fixtures in `test/data/`
(`test/sql/orcid_docs_examples.test`). More, grouped by job, are in
[docs/RECIPES.md](docs/RECIPES.md).

## Scope

The extension is a crosswalk between ORCID v3.0 documents (XML and JSON) and
tables, for any user and any purpose. It reads documents you already have and
writes ORCID documents back; it makes no network calls and holds no policy.
Consent, access control, privacy rules such as the GDPR, ORCID's API terms
and rate limits belong to the applications that fetch and store the data.

## Comparing two Public Data File releases

`orcid_documents` lists the raw documents of files and archive members, with
the iD taken from the path. That pairs the same records across two releases
without unpacking them, and `orcid_changes_xml` reports what changed. A NULL
side means the record is only in one release, so all its items are added or
removed:

```sql
WITH pairs AS (
  SELECT orcid, a.xml AS old_xml, b.xml AS new_xml
  FROM orcid_documents('ORCID_2024_10_summaries.tar.gz', members := '*/097/*') a
  FULL JOIN orcid_documents('ORCID_2025_10_summaries.tar.gz', members := '*/097/*') b USING (orcid))
SELECT c.orcid, c.section, c.kind, c.put_code, c.change, c.field, c.old_value, c.new_value
FROM pairs p, orcid_changes_xml(p.old_xml, p.new_xml) c
ORDER BY c.orcid, c.section, c.put_code, c.field;
```

Dumps group records by the last three characters of the iD, so `members :=
'*/097/*'` compares the same prefix of both releases, about 1/1000 of each.
On the two synthetic releases in `test/data/releases/` (five 2025 summaries,
with a year of edits applied by `scripts/make_release_fixtures.py`) this query
returns a revised title, a removed work, a changed family name and role title,
a removed and an added record, and nothing for the unchanged record.

## Functions

Every table below comes in three forms with the same columns:
`orcid_<section>(path)` reads XML or JSON files, globs, URLs and `.tar`/`.tar.gz`
dumps (the format is told per file and per member), and
`orcid_<section>_xml(xml)` and `orcid_<section>_json(json)` parse strings, for
example a column through a lateral join. The record sections accept a full
`/record`, a section collection, a single item or a `bulk` response.

| table function | rows | cols |
|---|---|---|
| **Record** | | |
| `orcid_works` / `orcid_works_xml` | works (summary or full): titles, type, dates, DOI and other ids, citation, source | 32 |
| `orcid_fundings` / `orcid_fundings_xml` | fundings: title, type, amount, dates, funder organization | 37 |
| `orcid_affiliations` / `orcid_affiliations_xml` | all 7 affiliation types (employment, education, qualification, invited position, distinction, membership, service): role, department, dates, organization | 32 |
| `orcid_peer_reviews` / `orcid_peer_reviews_xml` | peer reviews: role, type, subject, convening organization | 38 |
| `orcid_research_resources` / `orcid_research_resources_xml` | research-resource proposals and their first host | 32 |
| `orcid_research_resource_items` / `orcid_research_resource_items_xml` | the resource items of each proposal | 13 |
| `orcid_research_resource_hosts` / `orcid_research_resource_hosts_xml` | every host organization of a proposal or resource item | 12 |
| `orcid_contributors` / `orcid_contributors_xml` | contributors of full works and fundings, with role and sequence | 12 |
| `orcid_external_ids` / `orcid_external_ids_xml` | every external identifier, with its owner item and relationship | 13 |
| `orcid_person` / `orcid_person_xml` | names and biography | 15 |
| `orcid_person_items` / `orcid_person_items_xml` | other names, researcher URLs, emails, addresses, keywords, person external ids | 24 |
| `orcid_history` / `orcid_history_xml` | record history: creation method, submission and claim dates, verification flags | 17 |
| **Over time** | | |
| `orcid_timeline` / `orcid_timeline_xml` | dated `created` / `modified` events of every item in one snapshot | 13 |
| **Other API messages** | | |
| `orcid_summary` / `orcid_summary_items` (+ `_xml`) | record summary endpoint: per-type counts, email domains; its listed ids, employments and activities | 23 / 20 |
| `orcid_search_results` / `orcid_search_results_xml` | `search` and `expanded-search` results: iD, names, emails, institutions, `num_found` | 13 |
| `orcid_errors` / `orcid_errors_xml` | API errors, standalone or per entry of a `bulk` response; locked and deprecated records (iD, primary iD) | 10 |
| `orcid_notifications` / `orcid_notifications_xml` | inbox notifications (permission and custom): subject, items, dates, source | 30 |
| `orcid_group_ids` / `orcid_group_ids_xml` | peer-review group-id records | 12 |

| other function | returns |
|---|---|
| `orcid_valid(id)` | `BOOLEAN`: ISO 7064 MOD 11-2 check digit |
| `orcid_normalize(id)` | canonical `XXXX-XXXX-XXXX-XXXX`, or NULL; accepts URL prefixes, missing hyphens, a lowercase `x` |
| `orcid_changes(old_path, new_path)` / `orcid_changes_xml(old_xml, new_xml)` / `orcid_changes_json(old_json, new_json)` | added, removed and modified items (one row per changed field) between two snapshots of a record, in either format |
| `orcid_documents(path)` | the raw documents of files, globs and archive members: `file`, `member`, `name`, `orcid` (from the path), `xml` (the text, either format), `format` |
| `orcid_dump_sample(archive, n, seed)` | a seeded sample of `n` records of a dump, the same picks as yorc-rs's and yorc-zig's reservoir sampler: `slot`, `orcid`, `file`, `member`, `name`, `xml` (read only if selected), `eligible` ([REFERENCE.md](docs/REFERENCE.md#orcid_dump_sample-a-seeded-sample-of-a-dump)) |

| writer | writes |
|---|---|
| `orcid_<writer>_to_xml(rec)` / `orcid_<writer>_to_json(rec)` for `work`, `funding`, `affiliation`, `peer_review`, `research_resource`, `person_item`, `group_id` | one item as an ORCID v3.0 document; `rec` is a STRUCT of the reader's columns, plus `LIST(STRUCT)` fields for external ids, contributors and resource items |
| `orcid_bulk_works_to_xml(list)` / `orcid_bulk_works_to_json(list)` | a bulk document of 1 to 100 works |
| `SET orcid_crosswalk = true`, or a trailing `true` (`orcid_work_to_xml(rec, true)`) | crosswalk mode, off by default: the writers emit exactly yorc-rs's (and yorc-zig's) XML and JSON bytes instead of yorc-duckdb's own layout ([REFERENCE.md](docs/REFERENCE.md#crosswalk-mode)) |

The file readers and `orcid_documents` take these named parameters: `members` (glob
over archive member paths, default every `.xml` and `.json` member), `max_records` (stop after this
many documents per file) and `max_member_bytes` (largest archive member read,
default 64 MiB; a larger one is an error); the readers and `_json` twins also take
`json_root`, for the JSON shapes ORCID leaves untyped (an affiliation, a keyword).
They scan in parallel and report progress.
Every column, its type and its source path in the ORCID schema are listed in
[`docs/REFERENCE.md`](docs/REFERENCE.md).

## Documentation

| document | what it holds |
|---|---|
| [docs/REFERENCE.md](docs/REFERENCE.md) | every function and column, with its source path in the ORCID schema; the JSON mapping; the writers' input shapes |
| [docs/RECIPES.md](docs/RECIPES.md) | numbered, copy-paste recipes: reading records and dumps, every section, joins, DOIs and duplicates, co-author pairs, institutions, timelines, changes between releases, exports, performance, JSON, writing ORCID documents, XML/JSON conversion |
| [docs/TASKS.md](docs/TASKS.md) | task bundles: reusable macro files in [`examples/tasks/`](examples/tasks) (record health, DOI dedupe, co-authors, affiliation timeline, release changes) |
| [docs/COVERAGE.md](docs/COVERAGE.md) | every element and attribute of the vendored XSDs and the columns that carry it |
| [docs/ECOSYSTEM.md](docs/ECOSYSTEM.md) | what this extension owns, and the core and community extensions it composes with |
| [docs/LINKING.md](docs/LINKING.md) | joining to Crossref and DataCite DOIs, ROR organisations, ISNI and Wikidata |
| [docs/MIGRATING.md](docs/MIGRATING.md) | from ad-hoc ORCID XML pipelines to SQL, step by step |
| [docs/LOCKED_DEPRECATED.md](docs/LOCKED_DEPRECATED.md) | locked and deprecated records in the Public Data File |
| [docs/DUCKDB2.md](docs/DUCKDB2.md) | the DuckDB v2.0 pre-release |
| [BENCHMARKS.md](BENCHMARKS.md) | timings (provisional) next to yorc-rs, yorc-zig and ORCID's Java library, and the cross-check of their outputs; the harness is [yorc-bench](https://github.com/mgbilby/yorc-bench) |
| [ARCHITECTURE.md](ARCHITECTURE.md), [CONTRIBUTING.md](CONTRIBUTING.md), [CHANGELOG.md](CHANGELOG.md) | layout, conventions and release cadence, the changes of each release |
| [test/README.md](test/README.md), [audit/README.md](audit/README.md) | the test suite; provenance and verification |

## DuckDB versions

Built and tested against **DuckDB v1.5.6**. The source also compiles against
the v2.0 pre-release, where the file readers become multi-file readers (lists of
paths, a `filename` column, parallel across files). See
[`docs/DUCKDB2.md`](docs/DUCKDB2.md).

## Building

### Managing dependencies

The extension is pure C++17 with no external dependencies, so no vcpkg setup is
needed (`vcpkg.json` lists none).

### Build steps

DuckDB and its extension build system are git submodules. Clone with them:

```sh
git clone --recurse-submodules https://github.com/mgbilby/yorc-duckdb
cd yorc-duckdb
# or, in an existing clone:
git submodule update --init --recursive
```

Then build:

```sh
make                    # release build
GEN=ninja make release  # faster with ninja (and ccache)
```

This builds:

- `build/release/duckdb`: the DuckDB shell with the extension linked in;
- `build/release/test/unittest`: DuckDB's test runner, also with the extension linked in;
- `build/release/extension/yorc/yorc.duckdb_extension`: the loadable extension as it is distributed.

## Cleanliness

C++ follows DuckDB's style, configured in [`.clang-format`](.clang-format).
The targets come from the extension build system (`extension-ci-tools`):

```sh
make format-check   # report formatting differences (duckdb/scripts/format.py)
make format         # fix them in src/ and test/
make tidy-check     # clang-tidy over the build (needs a configured build)
```

Formatting has not yet been applied to the whole tree, so the first
`make format` will touch files; review and commit that change on its own.
Documentation copies are checked the same way:
`python3 scripts/sync_tasks_test.py --check` and
`python3 audit/tools/xsd_coverage.py .` ([audit/README.md](audit/README.md)).

## Running the extension

Start the shell with `./build/release/duckdb`. The extension is linked in, so
the functions are there without `LOAD`. Try the recipes in
[docs/RECIPES.md](docs/RECIPES.md) from the repository root: they read the
fixtures in `test/data/`.

To load the built extension into a stock DuckDB v1.5.6 instead, start it with
`-unsigned` and run
`LOAD 'build/release/extension/yorc/yorc.duckdb_extension';`.

## Running the tests

Tests are SQL tests in `test/sql/` over the fixtures in `test/data/` and the
ORCID schema samples in `reference/orcid-model/`. Everything runs locally:
a green local run is the gate before a release.

```sh
make test         # sqllogictests (needs the build above)
make core_test    # standalone checks of src/core: g++ only, seconds, no DuckDB
make fuzz         # libFuzzer targets over the core (clang)
```

[`test/README.md`](test/README.md) has the prerequisites, the layout and the
exact commands, and [`CONTRIBUTING.md`](CONTRIBUTING.md) describes the test
loops, the conventions and the release cadence.

## Data and license

ORCID releases public record data under CC0. The fixture records in `test/data/`
(`0000-0003-0100-6634.xml`, and `demo/0000-0002-2771-9344.xml` for the
documentation's examples) and the ORCID message schema in
`reference/orcid-model/` are third-party material, and [`NOTICE`](NOTICE) gives
their sources.

The extension is MIT licensed, see [`LICENSE`](LICENSE).

This repository is based on https://github.com/duckdb/extension-template.
