# yorc-duckdb on DuckDB v2.0

**Today:** built and released against **DuckDB v1.5.6** (submodules,
CI workflow and `description.yml` all agree). The source also compiles against
the v2.0 pre-release (`v2.0-cyanoptera`). Every API difference is in
[`src/include/orcid/compat.hpp`](../src/include/orcid/compat.hpp), and nothing
else in the source branches on the version (the build has one
`if(COMMAND duckdb_extension_statically_link)` in `extension_config.cmake`).

## What compat.hpp covers

| v1.5.6 | v2.0 | shim |
|---|---|---|
| bind callbacks take `vector<string> &names` | `vector<Identifier> &names` | `OrcidBindNames` (deduced from `table_function_bind_t`) |
| `FlatVector::GetData<T>` is writable | returns `const T *`; writes use `GetDataMutable` | `OrcidFlatData<T>` |
| `UnaryExecutor::ExecuteWithNulls` | removed; callback returns `optional<T>` | `OrcidUnaryWithNulls` |
| `DataChunk::SetCardinality` | deprecated; vectors carry their own size | `OrcidSetCardinality` → `SetChildCardinality` |
| scalars may throw freely | throwing scalars must call `SetFallible()` | `OrcidRegisterScalar(…, fallible)` |
| readers expand globs themselves | multi-file framework wraps a single-file reader | `OrcidRegisterReader`, `OrcidReaderGlobsItself` |
| `TableFunction::named_parameters` map | typed keyword arguments (`**kwargs`) on the signature, which the multi-file wrapper forwards | `OrcidAddNamedParameter` |
| extensions loaded by the config are linked | only extensions named by `duckdb_extension_statically_link()` are linked | `extension_config.cmake` calls it when the command exists |

**Not yet built against v2.0** (added after the last v2.0 check below): the
`_json` twins and `orcid_changes_json` (in-out functions with a `json_root`
keyword argument, registered through `OrcidAddNamedParameter`), the writer
scalars (`LogicalType::ANY` argument, `Vector::GetValue` and the `Value`
STRUCT / LIST accessors, flat `VARCHAR` results written through
`OrcidFlatData`; two overloads in a `ScalarFunctionSet`,
`OrcidRegisterScalarSet`). Each of these is a v1.5.6 API; if v2.0 renames
one, the shim goes in `compat.hpp`.
Crosswalk mode (2026-10-18) adds the `orcid_crosswalk` setting
(`DBConfig::AddExtensionOption` on `ExtensionLoader::GetDatabaseInstance()`,
read with `ClientContext::TryGetCurrentSetting`: the shims
`OrcidAddBooleanSetting` / `OrcidBooleanSetting` in `compat.hpp`), reads it in
the writer scalars through `ExpressionState::GetContext()`, and registers each
writer as a `ScalarFunctionSet` of `(ANY)` and `(ANY, BOOLEAN)`
(`OrcidRegisterScalarSet`).

The file readers also declare projection pushdown on both versions, because v2.0
adds the `filename` / `file_index` virtual columns only to scans that support it
("Virtual columns require projection pushdown").

Detection: `DUCKDB_MAJOR_VERSION >= 2`, or the presence of
`duckdb/common/multi_file/table_function_multi_file.hpp`, which only v2.0 has.
(`duckdb/common/identifier.hpp` is not a usable signal: v1.5.6 backported it.)

## What users get on v2.0

`orcid_works` becomes a multi-file reader: `orcid_works(['a.xml', 'b.xml'])`,
the `filename` and `file_index` virtual columns, `union_by_name`,
`hive_partitioning`, and files scheduled across threads. (On v1.5.6 the reader parallelizes itself;
see REFERENCE.md, "Parallel scans and row order".) Results are otherwise identical. The sqllogictests
are written to pass on both (the missing-file tests match only the file name,
because the error wording differs); confirm with the manual v2.0 CI job.

## Last local check against v2.0

2026-10-17, `duckdb` `v2.0-cyanoptera` at f8f3f4dcd954 and `extension-ci-tools`
`v2.0-cyanoptera`, in a separate checkout (the submodules stayed at v1.5.6):
- The extension builds once it uses the three shims added that day (named
  parameters, projection pushdown, static linking).
- All 10 sqllogictest files pass.
- The maintainer's 10,000-record corpus check passes: 10,000 files, 0 mismatches.

Same day, second check (a shell-only build at the same commits, with
`max_member_bytes` added): the one-row lateral derive still plans the in-out
side as the hash join's build side (`INOUT_FUNCTION (est=0)`). It peaks at
7.2 GB for the 5.4 MB record, as on v1.5.6, so the constant-argument form
(REFERENCE.md, "Memory of lateral joins") is needed on both. `max_member_bytes`
behaves as on v1.5.6: the error names the member, and 0 is a binder error.

Timings on the committed corpus, 4-core container, best of 3, seconds:

| query | threads=1 | threads=4 |
|---|---|---|
| `CREATE TABLE AS SELECT * FROM orcid_works('orcid-corpus.tar.gz')` | 3.92 | 2.69 |
| same, over the extracted directory (10,000 files) | 5.20 | 4.00 |
| `SELECT count(*) FROM orcid_person('orcid-corpus.tar.gz')` | 3.20 | 1.31 |

The directory scan is slower than on v1.5.6 (3.53 / 1.85 s), because the
framework binds and initializes every file on its own. The v2.0 batch API
(`claim_batch` / `prepare_batch`) is the place to tune this once v2.0 is
released.

## Checking the v2.0 build before release

1. Run the **Build against DuckDB v2.0 pre-release (manual)** job
   (`workflow_dispatch` on `.github/workflows/MainDistributionPipeline.yml`,
   e.g. `gh workflow run MainDistributionPipeline.yml -R mgbilby/yorc-duckdb`).
   It pairs the `v2.0-cyanoptera` branches of duckdb and extension-ci-tools;
   if the ci-tools branch lags behind, use `main` for both `@` and
   `ci_tools_version`, as community-extensions' `build_next.yml` has done.
2. When it is green, uncomment `repo.ref_next` in `description.yml`, set it to
   that commit, and send the same change to
   `extensions/yorc/description.yml` in duckdb/community-extensions. Without
   `ref_next`, community-extensions' `build_next.yml` skips this extension and
   a v2.0 regression would go unreported.

## Switching to v2.0 when it is released

Once DuckDB tags `v2.0.0` and `extension-ci-tools` has a matching `v2.0.0` branch:

```sh
git -C duckdb fetch --depth 1 origin tag v2.0.0 && git -C duckdb checkout v2.0.0
git -C extension-ci-tools fetch --depth 1 origin v2.0.0 && git -C extension-ci-tools checkout FETCH_HEAD
git add duckdb extension-ci-tools
sed -i 's/branch = v1.5.6/branch = v2.0.0/' .gitmodules
```

(The `branch` lines in `.gitmodules` are only hints for
`git submodule update --remote`; duckdb has a `v1.5.6` tag, not a branch, so
always move the duckdb submodule by tag as above.)

Then in `.github/workflows/MainDistributionPipeline.yml` change the stable job
to `@v2.0.0`, `duckdb_version: v2.0.0`, `ci_tools_version: v2.0.0`, and point
the manual job at whatever the next pre-release is. Run `make test` and
`make core_test`, update `ref` (and `ref_next`) in `description.yml`, and bump
`extension.version`. No source changes are needed. When v1.5.x support is
dropped, the `#else` branches in compat.hpp can be deleted.

## The version stamp of a local build

DuckDB's CMake takes the version it compiles into the CLI and into the
extension's metadata from `git describe --tags` in the `duckdb` submodule. A
shallow submodule (`git submodule update --init --depth 1`, or a fetch of the
pinned commit only) has no tags, so the build is stamped **`v0.0.1`**, and a
stock DuckDB v1.5.6 (the CLI, or libduckdb in a client) refuses it:
"built specifically for DuckDB version 'v0.0.1'". Check with
`build/release/duckdb --version`; it must print `v1.5.6`.

Either fetch the release tag before the first build:

```sh
git -C duckdb fetch --depth 1 origin tag v1.5.6
```

or set the version explicitly (the Makefile passes it to CMake):

```sh
OVERRIDE_GIT_DESCRIBE=v1.5.6 GEN=ninja make release
```

The version is fixed at configure time, so after either fix remove
`build/release` (or `build/debug`) and build again. CI is not affected: the
reusable workflow checks out with `fetch-depth: 0` and runs
`make set_duckdb_version`, which checks out the tag.
