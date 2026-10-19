# Fuzzing

libFuzzer targets over the DuckDB-free core (`src/core/`), built with
`clang++ -fsanitize=fuzzer,address,undefined`:

| target | input |
|---|---|
| `fuzz_sections` | one XML document, through `ExtractSection` for every section (and the integer/boolean parsers the binding applies) |
| `fuzz_changes` | two documents split at the first NUL byte (`old\0new`; without one, both sides are the input), through `DiffDocuments`, each side XML or JSON by its first byte |
| `fuzz_tar` | a tar archive, through `TarReader` (fed in varying read sizes) and `GlobMatch` |
| `fuzz_json` | one JSON document, through `ParseJson` (every string re-escaped with `AppendJsonString` must parse back to itself), `ExtractDocument` for every section, and the writers: every works row written back as XML and JSON must read again |

```sh
make fuzz                    # build, then run each target for FUZZ_SECONDS (default 60)
make fuzz FUZZ_SECONDS=600   # longer
make fuzz_build              # build only: build/fuzz/fuzz_*
```

Seed corpora are generated in `build/fuzz/seed_<target>/` from `test/data/` and the
`reference/orcid-model` samples (XML for `fuzz_sections`, JSON for `fuzz_json`, both
for `fuzz_changes`; the tar seeds are the test archives, decompressed);
`fuzz_json` uses `test/fuzz/json.dict`, the others `xml.dict`;
new inputs libFuzzer finds go to `build/fuzz/corpus_<target>/`. Needs clang with
compiler-rt (Debian/Ubuntu: `clang` and `libclang-rt-<version>-dev`).

A crash leaves `crash-<sha1>` in `build/fuzz/`. Reproduce with
`build/fuzz/fuzz_<target> build/fuzz/crash-<sha1>`, fix it, and add the input to a
regression check in `test/cpp/core_test.cpp`.
