# Changelog

All notable changes to yorc-duckdb. Versions follow [Semantic Versioning](https://semver.org/);
the version is set in `description.yml` (`extension.version`) and
`CMakeLists.txt` (`project(... VERSION ...)`), and the release tag is `v<version>`
(`scripts/check_version.sh` checks that they agree). A build from the tagged
commit reports that tag as `extension_version` in `duckdb_extensions()`; any
other build reports the commit's short hash.

## [0.2.0] - 2026-10-21

Built and released against DuckDB v2.0.0; DuckDB v1.5.6 keeps working
(docs/DUCKDB2.md). On DuckDB v2.0:

- the `orcid_<section>` file readers are multi-file readers: a list of
  paths (`orcid_works(['a.xml', 'b.xml'])`);
- the `filename` and `file_index` virtual columns on every scan;
- `union_by_name` and `hive_partitioning` options;
- files scheduled across threads.

Results are otherwise identical on both DuckDB versions. No source change
was needed: every API difference between v1.5.6 and v2.0 is shimmed in
`src/include/orcid/compat.hpp`.

## [0.1.0] - 2026-10-19

First release. Built and tested against DuckDB v1.5.6; the source also builds
against the DuckDB v2.0 pre-release (`docs/DUCKDB2.md`). Reads and writes
ORCID v3.0 XML and JSON. No access to the ORCID Sandbox or the ORCID Member
API, and no requests to ORCID. yorc-duckdb is an independent open-source
project, not affiliated with, endorsed by or sponsored by ORCID, Inc.

### Tables (each as `orcid_<section>(path)`, `orcid_<section>_xml(xml)` and `orcid_<section>_json(json)`)
- Record sections from the ORCID v3.0 message schema: `works`, `fundings`,
  `affiliations` (all seven types), `peer_reviews`, `research_resources`,
  `research_resource_items`, `contributors`, `external_ids`, `person`,
  `person_items`, `history`. Each reads a full `/record`, a section collection,
  a single item, or a `bulk` response.
- History: `orcid_timeline` (dated created / modified events of one snapshot)
  and `orcid_changes(old, new)` / `orcid_changes_xml` (field-level differences
  between two snapshots).
- Other API messages: `summary`, `summary_items`, `search_results`, `errors`
  (with the iD of locked and deprecated records, and `primary_orcid` for
  deprecated ones), `notifications`, `group_ids`.
- `orcid_documents(path)`: the raw documents of files, globs and archive members
  (file, member, name, iD, xml), to pair records across dump releases.
- `orcid_dump_sample(archive, n, seed)`: a seeded
  reservoir sample of the records of a dump, the same picks as yorc-rs's
  `dump::Reservoir` and yorc-zig's `reservoir` (SplitMix64, Algorithm R, the
  same eligibility rule); members not kept are never read
  (`src/core/reservoir.cpp`, `test/sql/orcid_dump_sample.test`). The
  walkthrough's cell 8 uses it in place of `USING SAMPLE`.
- The pre-3.0 peer-review layout `activities:group/peer-review:summary`
  (read sample `peer-reviews-3.0.xml`) is no longer read:
  nothing inside a `group/summary` element becomes a row, as in
  yorc-rs, yorc-zig and ORCID's classes; the sample gives 1 external id, not 3.
- `research_resource_hosts`: every host
  organization of a proposal or resource item (1 to 10 each), with `container`
  and `item_ordinal`; the first-host columns of `research_resources` and
  `research_resource_items` stay. The research_resource writer takes `hosts`
  lists (proposal and items) and writes every host; flat `host_*` fields still
  work alone, and must agree with a list given beside them.
- Context columns appended: the assertion
  origin outside `works`, normalized ids in `person_items` and
  `notifications`, `normalized_error_code`, `contributor_orcid_host` (now also
  written), the record's `orcid_uri` / `orcid_host` in `person`, and a
  notification item's external-id URL and relationship. Every column is
  appended after the existing ones; the full list is in `docs/COVERAGE.md`
  ("Columns added for the context gaps").

### JSON
- ORCID v3.0 JSON read by every section into the same rows as the XML: the
  JSON is mapped onto the XML element tree (value wrappers, attributes as
  members, `summaries` / `bulk` wrapper arrays, research resources'
  `resource-item`, epoch-millisecond dates as ORCID's ISO form); mapping in
  `docs/REFERENCE.md` ("JSON documents"), derived from the sample pairs of
  `reference/orcid-model`.
- `orcid_<section>(path)` tells the format per file and per archive member by
  its first byte that is not whitespace, so globs and archives may mix XML and
  JSON; the default member filter is now every `.xml` and `.json` member.
- `orcid_<section>_json(json)` twins (in-out, as the `_xml` ones) and
  `orcid_changes_json(old, new)`; `orcid_changes` compares an XML snapshot
  with a JSON one.
- `json_root` named parameter for the shapes ORCID's JSON leaves untyped (an
  affiliation, `{"content": ...}`); without it they are an error naming the
  values to pass.
- `orcid_documents` gains a `format` column (`xml` / `json`).
- A bare epoch-millisecond number in a `*-date` member (`"created-date":
  1540896558250`) is converted as the `{"value": ...}` form is, instead of
  staying digits.
- `history/creation-method` read from JSON gives the XSD value, as the XML
  does: ORCID's Java enum names `DIRECT`, `MEMBER_REFERRED`, `WEBSITE` become
  `Direct`, `Member-referred`, `website` (`API` stays); other text is kept.
- JSON members of `record`, `person`, `personal-details`, `record-summary`
  and `research-resource` are read in the XSD's child order whatever the key
  order, so `person_items` from JSON with sorted keys (as yorc-rs writes it)
  comes in the XML's order.
- A dependency-free RFC 8259 parser (`src/core/json.cpp`): strict, UTF-8
  checked, depth bounded; libFuzzer target `fuzz_json`.

### Writing
- `orcid_<writer>_to_xml(rec)` / `orcid_<writer>_to_json(rec)` for `work`,
  `funding`, `affiliation` (seven types), `peer_review`, `research_resource`,
  `person_item` (address, keyword, other name, researcher URL, external
  identifier) and `group_id`: a STRUCT of the reader's columns, with
  `LIST(STRUCT)` fields for external ids, contributors and resource items,
  becomes an ORCID v3.0 document, in the XSDs' element order and
  namespaces, or ORCID's JSON conventions. Required fields and value ranges
  are checked; read-only fields are not written; `put-code` and `visibility`
  are. There is no history writer, so the rule that `history/@visibility`
  is XML-only (left out of yorc-rs's and yorc-zig's JSON, as ORCID's JSON
  has no such member; cross-implementation review #16) needs no change here,
  in either layout; the JSON reader still accepts the member.
- `orcid_bulk_works_to_xml(list)` / `_json`: bulk documents of 1 to 100 works.
- A funding contributor's `sequence` is an error (funding-3.0.xsd has no
  contributor sequence; yorc-rs refuses it too) instead of being dropped
  silently.
- XML escaping (entities, `&#13;`, characters XML 1.0 cannot carry are an
  error) and JSON escaping (control characters, U+2028/U+2029, non-BMP as
  UTF-8); invalid UTF-8 is an error.
- `orcid_peer_reviews` gains `subject_translated_name_language` (column 36),
  which a subject's translated title needs on write.
- Every write sample of `reference/orcid-model` reads, writes and reads back
  to the same rows in XML, JSON and across the two
  (`test/sql/orcid_write.test`).
- Crosswalk mode , off by default:
  `SET orcid_crosswalk = true`, a trailing `BOOLEAN` argument on every writer
  scalar (`orcid_work_to_xml(rec, true)`, `orcid_bulk_works_to_json(list,
  true)`; it wins over the setting, and NULL gives NULL) make the writers
  emit exactly the bytes of yorc-rs's document writers (and yorc-zig's)
  for documents without ORCID's read-only fields: XML indented
  with two spaces; JSON keys sorted, empty always-present containers as
  `{"external-id": []}`, DEL unescaped (serde_json), put-code as an i64.
  A separate emitter (`src/core/crosswalk.cpp`) lays out the tree the
  writers already render, so elements, checks and errors are the same, and
  the default output is unchanged. `test/sql/orcid_crosswalk.test` compares
  all 21 XML write samples with yorc-rs's bytes, derived from its rules
  (`scripts/crosswalk_expected.py`, `test/data/crosswalk/`); the
  cross-implementation check (BENCHMARKS.md, "Parity") compares with the
  real Rust output.
- Scope: a pure crosswalk for any user and any purpose; no network calls and
  no policy (README, "Scope").

### Scalars
- `orcid_valid(id)` (ISO 7064 MOD 11-2) and `orcid_normalize(id)` (canonical
  `XXXX-XXXX-XXXX-XXXX`; URL prefixes, missing hyphens, lowercase `x`).

### Reading files and dumps
- Paths through DuckDB's file system: globs, `~`, and `https://` / `s3://` with httpfs.
- ORCID Public Data File archives (`.tar`, `.tar.gz`, `.tgz`) streamed member by
  member, never unpacked; `members` (glob over member paths), `max_records`,
  and `max_member_bytes` (64 MiB by default: a larger member is an error, so a
  crafted archive cannot inflate one member into memory without bound).
- Parallel scans on DuckDB v1.5.x over files and archive members, with document
  order preserved (`preserve_insertion_order`), progress reporting, a row
  estimate, and projection pushdown.
- Every record-table row carries the record iD and the SHA-256 of its source document.

### Robustness
- No dependencies: XML pull parser, SHA-256, tar reader and iD check are part
  of the extension.
- Item patterns anchored at their XSD positions, so full activity documents
  never yield person, history or API-message rows.
- Corrupt tar headers cannot trigger large allocations (memory follows the data
  actually read).
- XML input is checked for UTF-8 where it enters the extractor (one pass, an
  ASCII fast path), as JSON input already was: a document that is not UTF-8 is
  `orcid: malformed XML in ...: invalid UTF-8 at byte <n>`, and
  `orcid_documents` refuses it too, so no invalid text reaches a `VARCHAR`.
  Character references to no Unicode character (`&#xD800;`, `&#0;`) stay
  literal text.
- An `&` in XML text or an attribute value looks at most 12 bytes ahead for
  its `;`, so a run of `&` reads in linear time (it was quadratic: a few MB
  of `&` held a scan for hours).
- libFuzzer targets for every section extractor, the snapshot diff and the tar
  reader (`make fuzz`).
- Documentation: numbered recipes (`docs/RECIPES.md`), task bundles of reusable
  macros (`docs/TASKS.md`, `examples/tasks/`: record health, DOI dedupe,
  co-author pairs, affiliation timeline, release changes), a field-by-field
  schema coverage table (`docs/COVERAGE.md`, generated by
  `audit/tools/xsd_coverage.py`: 106 of 109 data-bearing names mapped), the
  extension's niche and companions (`docs/ECOSYSTEM.md`), linking to other
  registries (`docs/LINKING.md`), migrating from ad-hoc pipelines
  (`docs/MIGRATING.md`), and provenance of every vendored artifact and fixture
  (`audit/PROVENANCE.md`).
- Tests: sqllogictests over the ORCID schema samples and CC0 fixtures from the
  2025 Public Data File, every documentation example (`test/sql/orcid_docs_examples.test`),
  every fixture-runnable recipe (`orcid_recipes.test`) and task bundle (`orcid_tasks.test`),
  and standalone core checks (`make core_test`).

[0.1.0]: https://github.com/mgbilby/yorc-duckdb/releases/tag/v0.1.0
