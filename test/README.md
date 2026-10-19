# Test suite

Everything here runs locally; nothing in the suite depends on hosted CI. The
platform matrix (Linux, macOS, Windows, WASM) is the distribution pipeline's
job, and the DuckDB community build runs on its own infrastructure. **A green
local run is the gate before a release.**

Ubuntu prerequisites:

```sh
sudo apt install build-essential cmake ninja-build ccache python3 clang   # clang only for `make fuzz`
```

## Layout

| path | what it holds |
|---|---|
| `test/cpp/core_test.cpp` | standalone checks of `src/core/`: iD, SHA-256, XML reader, every section, the change log, the tar reader, the JSON parser and escaping, the JSON crosswalk, the writers and their crosswalk mode; one translation unit, `g++` only, no DuckDB |
| `test/sql/` | 18 sqllogictest files: the SQL surface end to end, run by DuckDB's `unittest` |
| `test/fuzz/` | libFuzzer targets over the core (`fuzz_sections`, `fuzz_changes`, `fuzz_tar`, `fuzz_json`) and the XML and JSON dictionaries; see its README |
| `test/progress_check.sh` | the progress bar is monotonic, stays in [0, 100] and reaches 90, on a large input; run by hand, needs script(1) |
| `test/data/` | committed fixtures, small and readable: one public ORCID record; `demo/` holds the public record the walkthrough and recipes use (0000-0002-2771-9344, kept apart so `test/data/*.xml` globs match only the test record); `activities/`, `errors/`, `changes/`, `dump/`, `releases/`, `synthetic/` group the rest (origins in [`audit/PROVENANCE.md`](../audit/PROVENANCE.md)) |

The sqllogictests:

| file | covers |
|---|---|
| `orcid_id.test` | `orcid_valid`, `orcid_normalize` |
| `orcid_works.test`, `orcid_works_xml.test` | the works reader and its `_xml` twin; XML edge cases |
| `orcid_json.test` | JSON reads into the same rows as the XML (the sample pairs), the record JSON, format detection in globs and archives, the `_json` twins, `orcid_changes_json` |
| `orcid_write.test` | the writers: every write sample reads, writes and reads back to the same rows (XML, JSON, XML to JSON to XML); required fields, the bulk limit, escaping |
| `orcid_crosswalk.test` | crosswalk mode (`SET orcid_crosswalk`, the trailing `BOOLEAN` argument): the 21 XML write samples, read and written back, give yorc-rs's bytes (`test/data/crosswalk/`); the default is unchanged; the argument wins over the setting |
| `orcid_sections.test` | every record section over the schema samples |
| `orcid_api_messages.test` | summary, search, error, notification and group-id messages |
| `orcid_activity_items.test` | a full activity document yields only activity rows |
| `orcid_history_of_record.test` | `orcid_timeline`, `orcid_changes` |
| `orcid_locked_deprecated.test` | locked and deprecated records as error documents |
| `orcid_dumps.test`, `orcid_max_member_bytes.test` | archives read in place (also a two-member gzip); `members`, `max_records`, `max_member_bytes` |
| `orcid_dump_sample.test` | `orcid_dump_sample`: the picks of yorc-rs's and yorc-zig's reservoir sampler |
| `orcid_parallel.test` | multi-threaded scans keep document order and `max_records` |
| `orcid_lateral_memory.test` | the constant-argument form stays within 64 MB |
| `orcid_docs_examples.test` | every SQL example of README.md and description.yml |
| `orcid_recipes.test` | the fixture-runnable recipes of [docs/RECIPES.md](../docs/RECIPES.md) |
| `orcid_tasks.test` | the task bundles of `examples/tasks/` ([docs/TASKS.md](../docs/TASKS.md)) |
| `orcid_walkthrough.test` | the cells of [docs/WALKTHROUGH.md](../docs/WALKTHROUGH.md), and that `docs/walkthrough.sql` holds each cell verbatim |

`orcid_recipes.test`, `orcid_tasks.test` and `orcid_walkthrough.test` were written from the fixtures
without being run, and so were `orcid_json.test`, `orcid_write.test`, the JSON
and writer additions to `orcid_docs_examples.test` and `orcid_recipes.test`
(recipes 16-19), and the JSON / writer checks of `core_test.cpp`; so were
the UTF-8 additions to `orcid_works_xml.test`, the
funding-sequence additions to `orcid_write.test`, the bare-epoch addition to
`orcid_json.test`, and `TestXmlUtf8`
of `core_test.cpp`; so were `orcid_crosswalk.test` and `TestCrosswalk`,
`TestCrosswalkSamples` of `core_test.cpp`, whose expected bytes
(`test/data/crosswalk/`) come from yorc-rs's rules
(`scripts/crosswalk_expected.py`), not from yorc-rs itself:
the cross-implementation check ([BENCHMARKS.md](../BENCHMARKS.md#parity)) is the check against the real Rust output. Run them, and check any failure against the fixture by hand
before changing an expected value.

## Running

```sh
# full build, then the SQL suite
GEN=ninja make release
./build/release/test/unittest --test-dir . "test/sql/*"     # or: make test

# one file
./build/release/test/unittest --test-dir . "test/sql/orcid_recipes.test"

# standalone core checks, and the same under every compiler found with -Werror
make core_test
make portability_check

# fuzzing (clang with compiler-rt): each target for FUZZ_SECONDS (default 60)
make fuzz

# documentation and copies kept in step (Python 3 standard library, no build)
python3 scripts/sync_tasks_test.py --check
python3 audit/tools/xsd_coverage.py . | head -1        # compare with docs/COVERAGE.md "Totals"
bash scripts/check_version.sh
```

The sqllogictests read `test/data/` and `reference/orcid-model/` by relative
path, so run `unittest` from the repository root (`--test-dir .`). A change
to `extension_config.cmake` needs `rm -rf build`. `make clean` leaves
`CMakeCache.txt` in place, and the stale extension list is reused.

## Expected values

Expected values come from the fixtures, not from this extension: grep and awk
counts over the pretty-printed XML, `tar -tvzf` listings for archive members,
and reading the schema samples. Each test states its source in a comment, and
[`audit/PROVENANCE.md`](../audit/PROVENANCE.md) lists them per file. A test
that echoes the extension's own output proves nothing about the C++.

## Fixtures and real data

The committed fixtures pin exact semantics: a member path longer than ustar
allows, a non-XML member, a deprecated record's primary iD, a work group that
moves when another is removed, a bulk response without a root path. They say nothing about millions of
records; that is what the Public Data File is for.

Downloaded releases live outside the repository, for example in
`~/orciddata`, so there is nothing to ignore and no way to commit them by
accident. They are CC0, published yearly on figshare, and tens of gigabytes
each. `reference/orcid-dumps/index.csv` has every file's URL, size and MD5.
DuckDB expands `~` when it opens a file, so `'~/orciddata/...'` works inside
SQL.

| data | file | use |
|---|---|---|
| summaries | `ORCID_<year>_10_summaries.tar.gz` | record sections, person, history, errors (locked and deprecated) |
| activities | `ORCID_<year>_10_activities_<n>.tar.gz` | full works with contributors (`orcid_contributors`), full fundings, peer reviews, research resources |
| two consecutive releases | both summaries files | `orcid_changes_xml` across releases (RECIPES.md 10.2) |

Checks to run on a release:

```sql
-- one iD prefix of a release, read in place
SELECT count(*) FROM orcid_works('~/orciddata/ORCID_2025_10_summaries.tar.gz', members := '*/097/*');
```

```sh
# the same members, counted outside DuckDB (summaries are pretty-printed, one element per line)
tar -xzOf ~/orciddata/ORCID_2025_10_summaries.tar.gz --wildcards '*/097/*.xml' | grep -c '<work:work-summary'
```

The row counts per section must equal grep counts of the item elements
(`<work:work-summary`, `<person:person`, ...). Locked and deprecated records
appear in `orcid_errors`, never in `orcid_person`. On two releases, the
change log of a record that did not change is empty.
