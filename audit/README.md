# Verification tooling

Everything under `audit/` exists to *show* properties of the extension
rather than to ship in it: where every vendored, generated and fixture
artifact came from, how much of the ORCID schema the sections cover, and (to
be done) how independent the implementation is from other ORCID software.
Python lives here and under `scripts/`, nowhere else in the repository. None
of it is part of the build, and the DuckDB community build ignores this
directory.

This file is the index and the run instructions. Where an artifact came from
and why it can be trusted is `PROVENANCE.md`'s subject, and is not retold here.

## Documents

| file | what it holds |
|---|---|
| `PROVENANCE.md` | the source and method behind every vendored and generated artifact and every test fixture, the specifications each hand-written subsystem was written from, and how the tests' expected values were derived |
| `SIMILARITY.md` | the method for a quantitative independence check against other ORCID XML software; **not yet measured** (TODO) |

## Tools (`tools/`)

| script | purpose |
|---|---|
| `xsd_coverage.py` | tabulates every element and attribute name of the vendored XSDs against the column paths in `src/core/sections.cpp` and the writer templates in `src/core/writer.cpp` (adding each name's JSON form and whether it is written); prints the totals and the table of [`docs/COVERAGE.md`](../docs/COVERAGE.md) (`--json` for the raw tabulation) |

Related scripts outside `audit/`:

| script | purpose |
|---|---|
| `scripts/orcid_dumps_index.sh` | regenerates `reference/orcid-dumps/index.csv` from figshare (network) |
| `scripts/make_release_fixtures.py` | builds `test/data/releases/` from six CC0 summaries |
| `scripts/sync_tasks_test.py` | copies `examples/tasks/*.sql` into `test/sql/orcid_tasks.test`; `--check` verifies them |
| `scripts/crosswalk_expected.py` | derives `test/data/crosswalk/write_samples/` (yorc-rs's payload bytes for each XML write sample); `--check` compares |
| `scripts/check_version.sh` | the extension version agrees across `description.yml`, `CMakeLists.txt` and the tag |

## Running the checks

Run everything from the repository root. All of them use the Python 3
standard library only and need no build.

```sh
# the coverage table is current: regenerate and compare with docs/COVERAGE.md
python3 audit/tools/xsd_coverage.py . > /tmp/coverage.md
diff <(sed -n '/^| `/p' /tmp/coverage.md) <(sed -n '/^| `/p' docs/COVERAGE.md)

# the task-bundle copies in the sqllogictest match examples/tasks/
python3 scripts/sync_tasks_test.py --check
```

The first line of `xsd_coverage.py`'s output is the totals as JSON. The
"Totals" table of COVERAGE.md is written from it by hand, so compare the two
after a schema or section change.
