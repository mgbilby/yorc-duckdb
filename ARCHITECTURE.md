# yorc-duckdb: Architecture

Reads and writes ORCID v3.0 XML and JSON, inside DuckDB: documents become
relational tables, and rows become ORCID documents again. Clients
that harvest ORCID data (command-line loaders, notebooks,
pipelines) can do all their document ↔ table work through it. It is a pure
crosswalk, for any user and any purpose; it is published as a DuckDB
Community Extension.

## Layout

```
src/core/               DuckDB-free C++17 (never includes DuckDB headers)
  xml.cpp               pull parser: elements, attributes, entities, CDATA; no namespaces; XML escaping
  json.cpp              RFC 8259 parser (UTF-8 checked, depth bounded), JSON string escaping
  json_map.cpp          JSON -> the XML element tree (ORCID's Jackson conventions), document element inference
  sections.cpp          section tables (from the XSDs) + one generic extractor over either tree
  writer.cpp            writer templates (from the XSDs): records -> XML / JSON documents, bulk
  crosswalk.cpp         crosswalk mode: the writers' tree in yorc-rs's document bytes (orcid_crosswalk)
  id.cpp                ISO 7064 MOD 11-2 check digit, canonical form
  sha256.cpp            FIPS 180-4 SHA-256 (no OpenSSL/mbedTLS dependency)
  tar.cpp               streaming ustar/GNU/pax reader + member glob
  changes.cpp           snapshot diff (orcid_changes) on top of the section extractor
  reservoir.cpp         SplitMix64 + Algorithm R and the eligibility rule (orcid_dump_sample)
src/include/orcid/      headers for the above, plus compat.hpp (v1.5.x ↔ v2.0 shims)
src/yorc_extension.cpp DuckDB bindings only (table functions, writer scalars)
test/cpp/core_test.cpp  standalone core checks (`make core_test`)
test/sql/*.test         sqllogictests (`make test`)
test/data/              fixtures: record 0000-0003-0100-6634, dumps, releases, errors, synthetic/ (audit/PROVENANCE.md)
docs/                   REFERENCE.md (functions, columns), RECIPES.md, TASKS.md, COVERAGE.md,
                        ECOSYSTEM.md, LINKING.md, MIGRATING.md, LOCKED_DEPRECATED.md, DUCKDB2.md
examples/tasks/         task bundles: plain CREATE OR REPLACE MACRO files (docs/TASKS.md)
audit/                  provenance, coverage tooling (xsd_coverage.py), similarity method
reference/orcid-model/  ORCID message schema v3.0 XSDs + samples (record, summary,
                        notification, group-id; MIT, unmodified)
reference/orcid-dumps/  index of every ORCID Public Data File (CC0), scripts/orcid_dumps_index.sh
scripts/                maintenance scripts: orcid_dumps_index.sh (dumps index), make_release_fixtures.py,
                        sync_tasks_test.py (task bundles -> orcid_tasks.test), check_version.sh
test/fuzz/              libFuzzer targets over the core (`make fuzz`): sections, changes, tar, JSON + writers
duckdb/, extension-ci-tools/   submodules pinned to v1.5.6
```

## Decisions

- **Pure C++17, no vcpkg dependencies.** ORCID XML is regular and
  machine-generated, so a small pull parser beats pulling libxml2/expat into
  every platform build.
- **Core/bindings split.** Everything testable lives in `src/core/` and is checked
  in seconds without building DuckDB; `yorc_extension.cpp` only adapts it.
- **JSON is mapped, not extracted twice.** ORCID's JSON is Jackson's view of the
  same model as its XML, so `json_map.cpp` turns a JSON document into the XML
  element tree (value wrappers, attributes as members, wrapper arrays, epoch
  dates) and the one table-driven extractor reads it through an
  `ElementReader` with the `XmlReader` interface. Every section, column and
  computed value is then the same for both formats. The format is told per
  document by its first byte.
- **Writers are templates of the XSDs.** One node tree per writable item
  (`writer.cpp`): element names, namespace prefixes, `xs:sequence` order,
  required elements, and the JSON form of each leaf (plain string or ORCID's
  `{"value": ...}` wrapper). Leaves name the reader column they write, so a row
  read by a section writes back to the same document; the binding only turns
  STRUCT / LIST values into records. Read-only fields (source,
  dates, path) are never written.
- **One compat header.** All DuckDB-version conditionals are in
  `orcid/compat.hpp`, so moving to v2.0 is a submodule bump (docs/DUCKDB2.md).
- **Streaming per file and per archive member.** Each thread parses one batch
  of documents at a time; memory is bounded by the largest batch. `.tar`/`.tar.gz` dumps are
  streamed member by member through DuckDB's file system (gzip and httpfs
  included) by a small tar reader in the core (`src/core/tar.cpp`), so a remote
  dump is read only as far as the query needs. On v1.5.x the reader hands out
  batches of files or members to DuckDB's threads itself and keeps document order
  through batch indexes; on v2.0 the multi-file framework schedules files.
- **No network code and no policy in the extension.** It is a crosswalk:
  fetching, posting to ORCID, tokens, rate limits, consent, access and privacy
  rules belong to the client application, whatever it is. Remote paths work
  through DuckDB's file system; written documents are strings or files.
  The extension does not provide access to the ORCID Sandbox or the ORCID
  Member API.

## Roadmap

1. ✅ `orcid_valid`, `orcid_normalize`, `orcid_works`, `orcid_works_xml`.
2. ✅ All of the ORCID v3.0 message schema through one table-driven extractor
   (`src/core/sections.cpp`): fundings, affiliations (7 types), peer reviews,
   research resources and items, contributors, external ids, person, person items,
   history; summary, full, collection, single-item and bulk documents.
2a. ✅ Dump archives read in place (`.tar`, `.tar.gz`, over https via httpfs).
2b. ✅ Record history: `orcid_timeline` (one snapshot) and `orcid_changes` (two).
2c. ✅ API messages outside the record, same extractor: `summary` / `summary_items`
   (summary_3.0), `search_results` (search, expanded-search), `errors` (error, bulk
   per-entry errors), `notifications` (permission, custom), `group_ids` (group-id-3.0).
   The column tables gained list (`*`), count (`#`) and document-level (`^`) paths
   and the `$position` special for them.
3. ✅ Parallel scan on v1.5.x (batches of documents, order kept through batch indexes;
   docs/REFERENCE.md). ✅ libFuzzer targets (test/fuzz/: every section extractor,
   `DiffDocuments`, the tar reader; `make fuzz`).
3a. ✅ ORCID v3.0 JSON: every reader auto-detects it (files, globs, archive
   members), `_json` twins, `orcid_changes_json`; writers for every write
   sample item type (XML and JSON), bulk works. (The validators and
   `COPY ... TO (FORMAT orcid_xml | orcid_json)` were removed on 2026-10-18:
   the extension does not provide access to the ORCID Member API.)
4. Corpus tests on a 5,000-record Public Data File sample (local, not CI).
5. Publish at github.com/mgbilby/yorc-duckdb and submit to duckdb/community-extensions.
