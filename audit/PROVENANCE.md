# Provenance

What every non-trivial artifact in this repository was built from, what each
hand-written subsystem was written against, and how the tests' expected
values were derived. Licence notices for third-party material live in
[`NOTICE`](../NOTICE); the tooling and the commands that run it in
[`README.md`](README.md).

Ground rules followed throughout the project:

- **Specifications and openly licensed data were the only sources.** The
  ORCID message schema (MIT-style licence), ORCID's public record data (CC0),
  W3C, NIST, ISO and POSIX specifications.
- **No code from other ORCID or XML software was copied, ported or
  translated.** The XML reader, the SHA-256, the ISO 7064 check and the tar
  reader are written from their specifications.
- **Expected test values come from the fixtures**, by grep, awk or reading
  the file, not from this extension's own output.

## Vendored material

### `reference/orcid-model/`

- **Source:** https://github.com/ORCID/orcid-model at commit
  `9592e2d3bde21a1edf703f26f8bc304448f886fb`. Copied:
  - `src/main/resources/record_3.0/` (27 XSDs, README, samples);
  - `common_3.0/`, `summary_3.0/`, `notification_3.0/`, `group-id-3.0/`
    (XSDs and samples).
- **Licence:** ORCID's MIT-style licence, `reference/orcid-model/LICENSE`.
- **Unmodified.** `reference/orcid-model/PROVENANCE.md` repeats the commit and
  paths. To refresh, copy the same paths at a newer commit and re-run
  `audit/tools/xsd_coverage.py` (README.md).
- **Use:** the specification the extractors in `src/core/sections.cpp` are
  written against; the samples are fixtures for `test/sql/orcid_sections.test`,
  `orcid_api_messages.test`, `orcid_history_of_record.test`,
  `orcid_recipes.test`, `orcid_walkthrough.test` and `orcid_docs_examples.test`
  (`record-3.0.xml` stands in for the demonstration record where an example
  needs affiliations or fundings), and fuzzing seeds.

### `reference/orcid-dumps/index.csv`

- **Source:** the figshare API (api.figshare.com), listing every file of
  every "ORCID Public Data File" release: names, sizes, MD5s and download
  URLs. The data files themselves are released by ORCID under CC0 1.0.
- **How:** `scripts/orcid_dumps_index.sh` (network; curl and jq). Generated,
  never hand-edited.

## Generated artifacts

| artifact | generator | inputs |
|---|---|---|
| the coverage table and totals in `docs/COVERAGE.md` | `audit/tools/xsd_coverage.py` | the vendored XSDs, `src/core/sections.cpp` |
| the bundle blocks of `test/sql/orcid_tasks.test` | `scripts/sync_tasks_test.py` | `examples/tasks/*.sql` |
| `test/data/releases/*.tar.gz` | `scripts/make_release_fixtures.py` (byte-reproducible: sorted members, fixed times and owners, gzip without name or time) | six summaries of the ORCID Public Data File 2025 (CC0) |
| `test/data/dump/*` | built by hand with GNU tar (gzip'd GNU format, and uncompressed pax) | three ORCID documents and one non-XML member (below) |
| `test/data/json/orcid_test_json.tar` | Python's `tarfile` (ustar, mtime 0, uid/gid 0, mode 0644), members in the order listed below | two schema samples and one text member (below) |

## Test fixtures (`test/data/`)

| fixture | origin | licence |
|---|---|---|
| `0000-0003-0100-6634.xml` | the public record of 0000-0003-0100-6634 as returned by `https://pub.orcid.org/v3.0/`, unmodified; the fixture of the tests, the dump fixtures and the benchmarks | CC0 1.0 (ORCID public data) |
| `demo/0000-0002-2771-9344.xml` | the public record of 0000-0002-2771-9344 (a fictitious person's record that ORCID uses as an example) as returned by `https://pub.orcid.org/v3.0/0000-0002-2771-9344/record`, unmodified; byte-identical to yorc-rs's `tests/data/0000-0002-2771-9344.xml` (45,543 bytes, SHA-256 `59018b6d315be538bb84a6105ff3ac1c4c274f3b467a455eb2576e03bbc2f86e`). The record of the demonstrations: `docs/WALKTHROUGH.md`, `docs/RECIPES.md`, `docs/TASKS.md` and the README examples (`orcid_docs_examples.test`). In a directory of its own so that the `test/data/*.xml` globs of the tests still match `0000-0003-0100-6634.xml` alone | CC0 1.0 (ORCID public data) |
| `activities/0000-0002-8635-8390_works_30089981.xml` | one full work, unmodified, from the Public Data File 2025 activities archive | CC0 1.0 |
| `errors/0000-0001-5272-5001_locked.xml`, `errors/0000-0001-5433-3001_deprecated.xml` | the summaries of a locked and a deprecated record (error documents), unmodified, from the Public Data File 2025 summaries archive | CC0 1.0 |
| `crosswalk/write_samples/<stem>.xml`, `.json` | `scripts/crosswalk_expected.py` (python3 -I, standard library) over the 21 XML write samples of `reference/orcid-model/record_3.0/samples/write_samples`: the bytes yorc-rs's `to_string` writes for each, derived from its rules (`yorc-rs/src/xml/write.rs`, `src/json.rs`, `tests/write_roundtrip.rs`), not produced by yorc-rs; `--check` re-derives and compares | ORCID MIT-style (derived) |
| `changes/old.xml` | `reference/orcid-model/record_3.0/samples/read_samples/record-3.0.xml` (fictitious iD 8888-8888-8888-8880), byte for byte | ORCID MIT-style |
| `changes/new.xml` | an edited copy of `old.xml`, made for the `orcid_changes` tests: a given name, a work title and an employment role title revised, a keyword added, the funding removed | ORCID MIT-style (derived) |
| `dump/orcid_test_summaries.tar.gz` | members (checked with `cmp` against the files named): `634/0000-0003-0100-6634.xml` = the fixture record; `880/8888-8888-8888-8880.xml` = the `record-3.0.xml` sample; `a-directory-name-long-enough-to-need-gnu-long-names-or-pax-headers/and-a-file-name-too-0000-0000-0000-0000.xml` = the `works-3.0.xml` sample, under a path longer than ustar's 100 characters; `README.txt` ("not xml") | as the members |
| `dump/orcid_test_summaries_pax.tar` | the same members as an uncompressed pax archive | as the members |
| `dump/orcid_test_summaries_2members.tar.gz` | the same tar as `orcid_test_summaries.tar.gz`, gzipped as two concatenated members (split at byte 700000 of the tar, `gzip -9n` each part) | as the members |
| `releases/ORCID_2024_10_summaries.tar.gz`, `releases/ORCID_2025_10_summaries.tar.gz` | `scripts/make_release_fixtures.py`: release "2024" holds five 2025 summaries unchanged; "2025" applies the edits listed in the script (a title revised, a work group removed, a family name and a role title changed, a record removed, one added) | CC0 1.0 (derived) |
| `json/orcid_test_json.tar` | members: `orcid_test_json/0000-0002-7319-2192.json` = `reference/orcid-model/record_3.0/samples/read_samples/record-full-3.0.json`; `orcid_test_json/work-full-3.0.xml` = `read_samples/work-full-3.0.xml`; `orcid_test_json/README.txt` (one line), to check per-member format detection and the default `*.xml` / `*.json` member filter | ORCID MIT-style (the samples) |
| `encoding/work-latin1.xml.bad` | written by hand with `printf`: a minimal work whose title holds the Latin-1 byte 0xE9 ("Caf\xE9 culture", byte 190), to check that XML input that is not UTF-8 is refused; the `.bad` extension keeps it out of the `*.xml` globs | this repository (MIT) |
| `synthetic/coauthors-bulk.xml` | written by hand to `work-3.0.xsd` / `bulk-3.0.xsd` for the co-author recipes and task bundle: two full works with invented titles and DOIs (prefix 10.5555, reserved for examples), contributors with ORCID's documentation example iDs | this repository (MIT) |

`test/fuzz/xml.dict` is a hand-written libFuzzer dictionary of XML syntax and
ORCID element names taken from the vendored XSDs; `test/fuzz/json.dict` the same
for JSON syntax and ORCID's JSON member names (the JSON samples). The fuzzers'
seeds are the fixtures above and the schema samples (Makefile `FUZZ_XML_SEEDS`,
`FUZZ_JSON_SEEDS`), copied into `build/fuzz/` at build time and never committed.

## Hand-written subsystems: what was consulted

| subsystem | files | written from |
|---|---|---|
| XML reader (elements, attributes, the five predefined entities, character references, CDATA, comments, processing instructions; DTDs are not expanded) | `src/core/xml.cpp` | XML 1.0 (Fifth Edition), W3C Recommendation |
| SHA-256 | `src/core/sha256.cpp` | FIPS 180-4, Secure Hash Standard |
| iD check and canonical form | `src/core/id.cpp` | ISO/IEC 7064:2003 MOD 11-2 as ORCID documents it (support.orcid.org, "Structure of the ORCID Identifier") |
| tar reader (ustar, pax `path` records, GNU long names) | `src/core/tar.cpp` | POSIX.1-2001 ustar and pax interchange formats; the GNU tar manual for the `L` long-name entry |
| sections and columns | `src/core/sections.cpp` | the vendored ORCID message schema v3.0; each column's source path is documented in `docs/REFERENCE.md` |
| JSON parser and string escaping, UTF-8 validation | `src/core/json.cpp` | RFC 8259 (The JavaScript Object Notation Data Interchange Format); RFC 3629 (UTF-8) |
| JSON to XML element tree, document element inference | `src/core/json_map.cpp` | the vendored samples: the JSON / XML pairs of `record_3.0/samples/write_samples`, and `read_samples/record-full-3.0.json` (the rules are listed in `docs/REFERENCE.md`, "JSON documents"); epoch-day to civil-date arithmetic after H. Hinnant, "chrono-Compatible Low-Level Date Algorithms" (public domain description) |
| writers (XML and JSON) | `src/core/writer.cpp`, XML escaping in `src/core/xml.cpp` | the vendored XSDs (element names, namespaces, `xs:sequence` order, `minOccurs`, value ranges); ORCID's JSON conventions from the JSON samples; XML 1.0 (Fifth Edition) for escaping and the characters XML cannot carry |
| change log | `src/core/changes.cpp` | the section model above (items matched on section, kind and put-code) |
| DuckDB binding | `src/yorc_extension.cpp`, `src/include/orcid/compat.hpp` | DuckDB's public extension API (headers of the `duckdb/` submodule) |

## How the expected values were derived

| test | expected values from |
|---|---|
| `orcid_id.test` | ORCID's documentation example iDs and the fixture record's iD, with check digits worked by hand |
| `orcid_sections.test`, `orcid_api_messages.test` | reading the schema samples |
| `orcid_works.test` | the file header says "an independent reference parser", but which parser is not recorded. **TODO (maintainer):** name it, or re-derive the values by grep. The counts 382 works, 155 groups, 311 DOIs and 226 distinct DOIs were re-derived with grep/awk for `orcid_recipes.test` and agree |
| `orcid_works_xml.test` | inline literals, by inspection |
| `orcid_dumps.test`, `orcid_max_member_bytes.test` | `tar -tvzf` listings (member names and sizes) and grep counts of the members |
| `orcid_activity_items.test`, `orcid_locked_deprecated.test` | grep counts and reading the fixtures |
| `orcid_history_of_record.test` | reading `changes/old.xml` and `new.xml`, and the sample's created/modified dates |
| `orcid_docs_examples.test` | grep and inspection of the fixtures; the saved-record examples over `demo/0000-0002-2771-9344.xml` (and `record-3.0.xml` for affiliations) from python3 -I (`xml.etree`), the timeline counted as REFERENCE.md describes it (the same count reproduces the 940 events of `0000-0003-0100-6634.xml`); the release pairs from the edits listed in `scripts/make_release_fixtures.py`, with put-codes checked by grep |
| `orcid_walkthrough.test` | the expected outputs of `docs/WALKTHROUGH.md`: python3 -I (`xml.etree`, `tarfile`, `hashlib`) and `stat` over `demo/0000-0002-2771-9344.xml`, the schema samples and the dump fixture, as each block's HTML comment there states |
| `orcid_lateral_memory.test` | the fixture's 382 works and its put-code sum (shared with `orcid_works.test`) |
| `orcid_parallel.test` | the single-thread result of the same query (an ordering and determinism check, not a value check) |
| `orcid_recipes.test`, `orcid_tasks.test` | grep, awk, python3 -I (`xml.etree`) and `tar -tzf` over the fixtures; each query's comment states the command or the count |
| `test/cpp/core_test.cpp` | the same fixtures and samples, by inspection; SHA-256 vectors from FIPS 180-4; the JSON and writer checks from the JSON samples (python3 -I, `json` module) and the rules of `json_map.cpp` / `writer.cpp` |
| `orcid_json.test` | the JSON samples read with python3 -I (`json` module) and the XML samples by grep; JSON/XML pairs compared row for row (the two identical pairs) or by NULL pattern (the others; the columns only one file fills found by comparing the element paths of each pair) |
| `orcid_crosswalk.test` | the write samples as in `orcid_write.test`; the expected bytes are `test/data/crosswalk/write_samples/` (below) |
| `orcid_write.test` | the samples themselves (read, write, read back, compared with EXCEPT ALL); counts by `ls`, grep and python3 -I; output formats from the writer rules |
