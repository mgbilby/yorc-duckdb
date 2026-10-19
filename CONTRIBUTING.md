# Contributing to yorc-duckdb

Keep pull requests small and tested: every behavior change needs a test.

## First build

DuckDB's extension build system is vendored as submodules (`duckdb/`,
`extension-ci-tools/`, both pinned to v1.5.6):

```sh
git clone --recurse-submodules https://github.com/mgbilby/yorc-duckdb
# or, in an existing clone:
git submodule update --init --recursive

GEN=ninja make release        # optimized build
GEN=ninja make debug          # assertions + symbols
```

`make release` produces `build/release/duckdb` (the shell with the extension
linked in), `build/release/test/unittest` (the sqllogictest runner) and
`build/release/extension/yorc/yorc.duckdb_extension`. The first build
compiles all of DuckDB; later builds are incremental. A change to
`extension_config.cmake` needs `rm -rf build`.

## The fast loop: core tests

`src/core/` is dependency-free C++17 that never includes DuckDB headers.
`test/cpp/core_test.cpp` checks it directly:

```sh
make core_test          # -> core_test: all checks passed
make portability_check  # core with -Werror under every compiler found (gcc, clang, aarch64+qemu, MinGW)
```

Work on parsing or extraction should iterate here first, then wire the result
into SQL in `src/yorc_extension.cpp`.

## The SQL loop: sqllogictests

```sh
make test
build/release/test/unittest "test/sql/orcid_works.test"
```

Conventions:

* header block `# name:`, `# description:`, `# group: [orcid]`, then `require orcid`;
* fixtures in `test/data/`, referenced by relative path (the runner executes from
  the repository root); write outputs to `__TEST_DIR__`;
* `statement error` asserts a stable substring of the message, never the whole
  text, and nothing that differs between DuckDB v1.5.x and v2.0;
* expected values come from the fixture itself (grep or awk counts, `tar -tvzf`
  listings, reading the file), stated in a comment above the query, never from
  running this extension ([audit/PROVENANCE.md](audit/PROVENANCE.md) lists the
  source per test file);
* a task bundle in `examples/tasks/` is copied into `test/sql/orcid_tasks.test`
  by `python3 scripts/sync_tasks_test.py`; run it with `--check` before a commit
  ([docs/TASKS.md](docs/TASKS.md)).

## Cleanliness

`make format-check` / `make format` apply DuckDB's style (`.clang-format`) to
`src/` and `test/`, and `make tidy-check` runs clang-tidy. Keep formatting-only
changes in commits of their own.

## DuckDB versions

All version-specific code goes in `src/include/orcid/compat.hpp`; the rest of
the tree must compile unchanged against v1.5.6 and the v2.0 pre-release. See
[docs/DUCKDB2.md](docs/DUCKDB2.md).

## Specifications only

Parsers, extractors and writers are written from the ORCID v3.0 message schema
(its XSDs and samples), ISO 7064, XML 1.0, RFC 8259 / RFC 3629 and FIPS 180-4
(see NOTICE). Do not copy code, tables or test data from other ORCID, XML or
JSON software.

## Release cadence

The extension tracks DuckDB stable releases. The `duckdb` submodule pins the
latest stable; expect a bump PR shortly after each DuckDB release, paired with
the matching `extension-ci-tools` branch and workflow tag. Community Extensions
rebuild per DuckDB version, so users get new builds with their DuckDB upgrade.

## Releases

`extension.version` in `description.yml` is the release version
([semver](https://semver.org/)). A release is:

1. bump the version in `description.yml` (`extension.version`), in
   `CMakeLists.txt` (`project(... VERSION ...)`) and in README.md ("Current
   release"), and add a `CHANGELOG.md` entry. `scripts/check_version.sh` checks
   that they agree. Merge to `main` and wait for a green
   `MainDistributionPipeline` run on that commit;
2. tag it `vX.Y.Z` (annotated) and create the GitHub release from the tag,
   titled with the bare version (`X.Y.Z`);
3. open a PR to [duckdb/community-extensions](https://github.com/duckdb/community-extensions)
   that sets `extension.version` and `repo.ref` (the full SHA of the tagged
   commit) in `extensions/yorc/description.yml`. The registry builds and
   publishes that commit for every platform once the PR is merged.

## Housekeeping

* New SQL surface goes into `docs/REFERENCE.md` and `CHANGELOG.md` in the same
  PR. SQL examples in README.md or description.yml get a matching query in
  `test/sql/orcid_docs_examples.test`.
* Format with the repository `.clang-format` (tabs, 120 columns).
* Do not commit build outputs (`build/`) or editor state.
