# yorc-duckdb benchmarks and parity

Timings of the `yorc` extension next to the other implementations of the
ORCID v3.0 message schema in the yorc family, the libraries
[yorc-rs](https://github.com/mgbilby/yorc-rs) (Rust) and
[yorc-zig](https://github.com/mgbilby/yorc-zig) (Zig), with ORCID's own Java
library ([orcid-model](https://github.com/ORCID/orcid-model) 4.0.2, JAXB and
Jackson configured as ORCID's API configures them) as the baseline. Then how
the outputs of the four compare.

**The DuckDB timings on this page are provisional.** The DuckDB leg of the
benchmark is new, and three of its figures (marked †) are implausibly fast;
see [Caveats](#caveats). The parity results do not depend on them.

## Results

_2026-10-18, on the maintainer's machine, yorc 0.1.0 sources before
release: median of 5 runs, the order of the programs rotating between runs;
single-threaded. DuckDB: the v1.5.6 CLI with the extension linked in
(`make release`), `SET threads = 1`. Rust: `--release`, fat LTO,
`-C target-cpu=native`. Zig 0.18.0-dev.131+41f885830: ReleaseFast for the
native CPU. Java: JDK 21, steady state after JIT warm-up._

Raw lines and notes: not published (this run predates
[yorc-bench](https://github.com/mgbilby/yorc-bench) and the corrected DuckDB
leg, see [Caveats](#caveats)). The corrected numbers are in
[yorc-bench's RESULTS.md](https://github.com/mgbilby/yorc-bench/blob/main/RESULTS.md), with every later run.

ns/op (lower is faster); A/B is A's time over B's, so below 1 A is faster.

| workload | Java | Rust | Zig | DuckDB (provisional) | DuckDB/Java | DuckDB/Rust |
|---|---:|---:|---:|---:|---:|---:|
| id_normalize | - | 776,062 | 447,670 | 1,845,000 | - | 2.38 |
| xml_read_record | 13,462,202 | 5,830,386 | 4,346,944 | 3,966,667 | 0.29 | 0.68 |
| xml_write_record | 5,350,607 | 596,104 | 1,023,642 | - | - | - |
| json_write_record | 4,307,037 | 4,877,996 | 4,071,753 | - | - | - |
| json_read_record | 4,061,310 | 5,607,829 | 2,226,314 | 40,966,667 | 10.09 | 7.31 |
| rows_record | - | 1,163,736 | 460,412 | 48,000,000 | - | 41.25 |
| sha256 | 3,108,650 | 4,092,053 | 3,980,834 | 226,667 † | (0.07) | (0.06) |
| dump_stream | 15,568,395 | 6,765,126 | 5,913,709 | 600,000 † | (0.04) | (0.09) |
| group_ids_read | 556 | 792 | 787 | 1,000 | 1.80 | 1.26 |
| timeline_record | - | 427,414 | 353,849 | 11,000,000 | - | 25.74 |
| changes_record | - | 2,626,577 | 1,223,644 | 6,833,333 | - | 2.60 |
| dump_sample | - | 706,061 | 854,750 | 150,000 † | - | (0.21) |
| geometric mean, all workloads | | | | | (0.43) | (1.46) |
| geometric mean, without † | | | | | 1.75 | 4.56 |

`-`: no counterpart. yorc-duckdb writes item documents from rows but has no
record writer, so the two record-writing workloads do not apply; ORCID's
Java library has no iD utility, rows, timeline, diff or dump reader.
Figures in parentheses include the † workloads and are not claims. Without
them, DuckDB/Java is over 3 workloads and DuckDB/Rust over 7.

For the libraries, over the same run: Rust/Java 0.67, Zig/Java 0.58,
Zig/Rust 0.76 (geometric means; Zig on its own allocator). A later run
(2026-10-19, Rust and Zig both on malloc, no DuckDB leg) gives Rust/Java
0.67, Zig/Java 0.46 and Zig/Rust 0.52; see the libraries' BENCHMARKS.md.

What the run says about yorc-duckdb, with the caveats below:

- Reading the real 1.3 MB record from a file into its 14 section tables
  (`xml_read_record`) takes 0.29 of the time ORCID's Java library takes to
  unmarshal it, and 0.68 of yorc-rs's parse into its model.
- Row-producing workloads over a document already in a table (`rows_record`,
  `timeline_record`, `json_read_record`) are far slower than the libraries'
  in-memory equivalents: 7 to 41 times yorc-rs's time. They run 14 table
  functions (one per section) through lateral joins over a one-row table, and
  the time includes that SQL machinery; the libraries flatten an already
  parsed model. Both readings are under investigation along with the †
  figures.
- `group_ids_read` is about a microsecond on every side.

## Workloads

Each workload is one SQL statement that runs all its iterations inside
DuckDB; the time is the shell's `.timer` for that statement over all
iterations (millisecond resolution), after untimed warm-up runs. The inputs
ship in this repository.

| workload | DuckDB statement | the libraries' operation |
|---|---|---|
| `id_normalize` | `orcid_normalize` over 10,000 generated iDs | the same iDs |
| `xml_read_record` | the 14 sections, `orcid_<section>(file)` over `test/data/0000-0003-0100-6634.xml`: 14 parses of the file | parse the record, held in memory, into the model |
| `json_read_record` | the 14 sections, `orcid_<section>_json(doc, 'record')` over yorc-rs's JSON of the record | parse that JSON into the model |
| `rows_record` | the 14 sections, `orcid_<section>_xml(doc)`, the document from a table | flatten the parsed model into rows |
| `sha256` † | the `source_hash` of one `orcid_history_xml` row, less the same scan without the row | SHA-256 hex of the record |
| `dump_stream` † | `orcid_person(dump, members := '*.xml')` over `test/data/dump/orcid_test_summaries.tgz` | stream and parse every `.xml` member |
| `dump_sample` † | `orcid_dump_sample(dump, 5, 42)`, `orcid` column only | the seeded reservoir sample, n = 5, seed 42 |
| `group_ids_read` | `orcid_group_ids_json(doc)` over ORCID's `peer-review-group-id.json` write sample | read the same document |
| `timeline_record` | `orcid_timeline_xml(doc)` | the record's dated events |
| `changes_record` | `orcid_changes_xml(doc, edited)`, `edited` the record with its first work title changed | the same diff |

## Caveats

- **Provisional DuckDB figures.** `sha256`, `dump_stream` and
  `dump_sample` (†) come out 5 to 26 times faster than the libraries, which
  is not plausible for the same work: probably the query optimiser prunes
  columns or members (for instance hashing or decompressing less than the
  workload intends), or a result is cached across iterations. They are being
  investigated, are left out of every claim on this page, and will be
  replaced by corrected figures.
- **SQL execution is in the time.** Planning, the table-function and lateral
  join machinery, vectors and the aggregate over the result are timed;
  process start-up, extension loading and staging the inputs are not. The
  statements are the closest SQL counterparts of the libraries' operations,
  not the same operations: read the DuckDB column as what a SQL user pays,
  not as library against library.
- **One thread.** The extension parallelises across files and archive
  members by default; the benchmark sets `threads = 1` to match the
  single-threaded libraries.
- **Timer resolution.** The shell's timer reports milliseconds for the whole
  statement, so a figure is (total ms) / iterations.
- **Warm cache.** `dump_stream` reads the archive from disk on every
  iteration, from the page cache after the first.
- Absolute numbers depend on the machine; compare ratios within one run.

## Parity

In the same run, a cross-check fed the same inputs (the real record, ORCID's
53 read and 21 write samples, ORCID's JSON samples, 10,000 iDs and the dump
fixture) to every implementation and compared the outputs class by class:
section rows, JSON-read rows, written XML and JSON byte for byte, dump
members, SHA-256, group ids, timelines and changes.

- yorc-duckdb, yorc-rs and yorc-zig were identical in every compared class.
  The written documents are compared in crosswalk mode
  (`SET orcid_crosswalk = true`), in which the writers emit yorc-rs's layout;
  see [docs/REFERENCE.md](docs/REFERENCE.md), "Crosswalk mode".
- ORCID's Java library, as the reference reader, read 74 of the 75 JSON
  documents the implementations wrote. The remaining entry is the library's
  own XSD check: ORCID's read sample `work-3.0.xml` has an empty
  `journal-title`, which the XSD's non-blank pattern rejects and yorc reads as
  absent.

In this repository, `test/sql/orcid_crosswalk.test` and `make core_test`
check the crosswalk bytes of the 21 write samples against
`test/data/crosswalk/`, which `scripts/crosswalk_expected.py` derives from
yorc-rs's rules.

## Reproducing

The harness that produced these figures is public:
[yorc-bench](https://github.com/mgbilby/yorc-bench). It holds the driver
programs for Rust, Zig and Java, the SQL leg run in this extension's
`build/release/duckdb` (`duckdb/bench.py`), timing the workloads above on the
same inputs, the cross-check that diffs their outputs, the committed results
of the published runs, and yorc-rs, yorc-zig and yorc-duckdb as submodules
at the commits measured:

```sh
git clone --recursive https://github.com/mgbilby/yorc-bench
cd yorc-bench
(cd yorc-duckdb && GEN=ninja make release)
./run.sh --duckdb --java    # all four (Rust 1.87+, Zig 0.18.0-dev.131+41f885830, JDK 21, Maven)
./compare.sh --only rust,zig,duckdb,java
```

Its README lists the prerequisites. Its [RESULTS.md](https://github.com/mgbilby/yorc-bench/blob/main/RESULTS.md) has the latest
published run (all four implementations, with the machine, toolchains and
commits) and every earlier one, each backed by its raw lines in `results/`.

Every input is in this repository (`test/data/`,
`reference/orcid-model/`). To time a query yourself, build with
`make release`, start `build/release/duckdb`, run `.timer on` and
`SET threads = 1;`, and time a statement that does many iterations, checking
with `EXPLAIN ANALYZE` that the plan does all the work you mean to time.
