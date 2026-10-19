# Uses the shared DuckDB extension build system (git submodules: duckdb,
# extension-ci-tools).  `make release` / `make debug` / `make test`.
PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

EXT_NAME=yorc
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

include extension-ci-tools/makefiles/duckdb_extension.Makefile

# Standalone checks for src/core/ (no DuckDB, builds in seconds).
.PHONY: core_test
core_test:
	mkdir -p build
	g++ -std=c++17 -Wall -Wextra -O2 -Isrc/include src/core/*.cpp test/cpp/core_test.cpp -o build/core_test
	./build/core_test

# libFuzzer targets over src/core/ (test/fuzz/README.md). Needs clang with compiler-rt.
FUZZ_CXX ?= clang++
FUZZ_SECONDS ?= 60
FUZZ_MAX_LEN ?= 65536
FUZZ_FLAGS = -std=c++17 -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined -Isrc/include
FUZZ_TARGETS = fuzz_sections fuzz_changes fuzz_tar fuzz_json
FUZZ_XML_SEEDS = test/data/*.xml test/data/*/*.xml reference/orcid-model/*/samples/*.xml \
	reference/orcid-model/*/samples/*/*.xml
FUZZ_JSON_SEEDS = reference/orcid-model/*/samples/*/*.json

.PHONY: fuzz fuzz_build
fuzz_build:
	mkdir -p build/fuzz
	for t in $(FUZZ_TARGETS); do \
		$(FUZZ_CXX) $(FUZZ_FLAGS) src/core/*.cpp test/fuzz/$${t}.cpp -o build/fuzz/$$t || exit 1; \
	done
	rm -rf build/fuzz/seed_* && mkdir -p build/fuzz/seed_fuzz_sections build/fuzz/seed_fuzz_changes build/fuzz/seed_fuzz_tar \
		build/fuzz/seed_fuzz_json
	@for f in $(wildcard $(FUZZ_XML_SEEDS)); do \
		n=$$(echo "$$f" | tr / _); cp "$$f" build/fuzz/seed_fuzz_sections/$$n; cp "$$f" build/fuzz/seed_fuzz_changes/$$n; \
	done
	@for f in $(wildcard $(FUZZ_JSON_SEEDS)); do \
		n=$$(echo "$$f" | tr / _); cp "$$f" build/fuzz/seed_fuzz_json/$$n; cp "$$f" build/fuzz/seed_fuzz_changes/$$n; \
	done
	rm -rf build/fuzz/tarsrc && mkdir -p build/fuzz/tarsrc/ORCID_2025_10_activities_0/634/0000-0003-0100-6634/works/a-path-long-enough-to-need-a-gnu-long-name-or-a-pax-header-record-xxxxxxxxxxxxxxxxxxxxxxxx
	cp test/data/errors/*.xml build/fuzz/tarsrc/
	cp test/data/activities/*.xml build/fuzz/tarsrc/ORCID_2025_10_activities_0/634/0000-0003-0100-6634/works/a-path-long-enough-to-need-a-gnu-long-name-or-a-pax-header-record-xxxxxxxxxxxxxxxxxxxxxxxx/
	for fmt in gnu pax ustar; do \
		tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner --format=$$fmt -C build/fuzz/tarsrc \
			-cf build/fuzz/seed_fuzz_tar/$$fmt.tar . 2>/dev/null || true; \
	done

fuzz: fuzz_build
	cd build/fuzz && for t in $(FUZZ_TARGETS); do \
		mkdir -p corpus_$$t; \
		dict=; [ $$t = fuzz_tar ] || dict=-dict=../../test/fuzz/xml.dict; [ $$t = fuzz_json ] && dict=-dict=../../test/fuzz/json.dict; \
		./$$t $$dict -max_total_time=$(FUZZ_SECONDS) -max_len=$(FUZZ_MAX_LEN) -rss_limit_mb=2048 -timeout=10 \
			-print_final_stats=1 corpus_$$t seed_$$t || exit 1; \
	done

# Portability checks without building DuckDB (part of the release
# checklist): src/core + core_test with -Werror under every compiler found
# (g++, clang++, g++ -m32, aarch64 cross g++ run under qemu-aarch64, MinGW
# compile only), and the binding TU against the DuckDB headers. Skips missing ones.
STRICT = -std=c++17 -Wall -Wextra -Wpedantic -Werror -O2 -Isrc/include
# `return std::move(result)` converting unique_ptr<Derived> to unique_ptr<Base> is
# DuckDB's idiom and needed by older compilers; GCC 13 calls it redundant.
EXT_STRICT = -std=c++17 -Wall -Wextra -Werror -Wno-unused-parameter -fsyntax-only \
	-isystem duckdb/src/include -isystem duckdb/third_party/fmt/include -isystem duckdb/third_party/re2 -Isrc/include
.PHONY: portability_check
portability_check:
	mkdir -p build/portability
	@set -e; run() { echo "== $$*"; "$$@"; }; \
	for cxx in g++ clang++; do \
		if command -v $$cxx >/dev/null; then \
			run $$cxx $(STRICT) src/core/*.cpp test/cpp/core_test.cpp -o build/portability/core_$$cxx; \
			./build/portability/core_$$cxx 2>/dev/null | tail -1; \
		fi; \
	done; \
	if printf '#include <cerrno>\nint main(){}\n' | g++ -m32 -x c++ - -o build/portability/m32 2>/dev/null; then \
		run g++ -m32 $(STRICT) src/core/*.cpp test/cpp/core_test.cpp -o build/portability/core_m32; \
		./build/portability/core_m32 2>/dev/null | tail -1; \
	fi; \
	if command -v aarch64-linux-gnu-g++ >/dev/null; then \
		run aarch64-linux-gnu-g++ -static $(STRICT) src/core/*.cpp test/cpp/core_test.cpp -o build/portability/core_arm64; \
		if command -v qemu-aarch64 >/dev/null; then qemu-aarch64 build/portability/core_arm64 2>/dev/null | tail -1; fi; \
	fi; \
	if command -v x86_64-w64-mingw32-g++-posix >/dev/null; then \
		run x86_64-w64-mingw32-g++-posix -static $(STRICT) src/core/*.cpp test/cpp/core_test.cpp -o build/portability/core.exe; \
	fi; \
	run g++ $(EXT_STRICT) -Wpedantic -Wno-redundant-move src/yorc_extension.cpp; \
	if command -v clang++ >/dev/null; then run clang++ $(EXT_STRICT) src/yorc_extension.cpp; fi; \
	if command -v x86_64-w64-mingw32-g++-posix >/dev/null; then run x86_64-w64-mingw32-g++-posix $(EXT_STRICT) -Wno-redundant-move src/yorc_extension.cpp; fi; \
	if command -v aarch64-linux-gnu-g++ >/dev/null; then run aarch64-linux-gnu-g++ $(EXT_STRICT) -Wno-redundant-move src/yorc_extension.cpp; fi; \
	echo "portability_check: ok"
