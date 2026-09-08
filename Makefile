PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=acl_otel
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# The OpenTelemetry C++ SDK (spec 002) comes from vcpkg - opentelemetry-cpp with the OTLP HTTP and
# gRPC exporters (§5 of the contract: no vendoring) - through the merged-manifest flow every duckdb
# extension uses. Bootstrap once with `make vcpkg-setup`, or point VCPKG_TOOLCHAIN_PATH at an
# existing checkout (duckdb-acl's works: the same baseline, and its binary cache already holds
# grpc, protobuf, abseil and curl).
USE_MERGED_VCPKG_MANIFEST := 1
VCPKG_TOOLCHAIN_PATH ?= $(PROJ_DIR)vcpkg/scripts/buildsystems/vcpkg.cmake
GOALS := $(if $(MAKECMDGOALS),$(MAKECMDGOALS),all)
# only the goals that configure cmake need the toolchain; the CI's checkout-phase goals run before
# any vcpkg exists (duckdb-acl's lesson, its Makefile says why the list names the goals that NEED it)
OTEL_VCPKG_GOALS := all release debug reldebug relassert wasm_mvp wasm_eh wasm_threads
ifeq ($(wildcard $(VCPKG_TOOLCHAIN_PATH)),)
ifneq ($(filter $(OTEL_VCPKG_GOALS),$(GOALS)),)
$(error this build needs vcpkg: run 'make vcpkg-setup' first, or set VCPKG_TOOLCHAIN_PATH)
endif
endif

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# --- Standalone C++ tests (the duckdb-acl / mssql-extension style) -----------------------------
# Each test/cpp/test_*.cpp is its own program, built from the already-compiled release tree and
# linked against the shared libduckdb. Same generator as the main build: GEN=ninja make test-cpp.
# the transport's test links the SDK and is a CMake target (below), not a Makefile-compiled one
TEST_CPP_SOURCES := $(filter-out test/cpp/test_acl_otel_otlp.cpp test/cpp/test_acl_otel_metrics_otlp.cpp,\
	$(wildcard test/cpp/test_*.cpp))
TEST_CPP_FLAGS := -std=c++17 -O2 -DNDEBUG -pthread
TEST_CPP_DIR := build/test
TEST_CPP_BINS := $(patsubst test/cpp/%.cpp,$(TEST_CPP_DIR)/%,$(TEST_CPP_SOURCES))
TEST_CPP_INCLUDES := -I duckdb/src/include -I duckdb/third_party/fmt/include -I src/include \
	-I duckdb-acl/src/include -I duckdb/third_party/yyjson/include
ifeq ($(shell uname -s),Darwin)
TEST_CPP_DUCKDB_LIB := build/release/src/libduckdb.dylib
else
TEST_CPP_DUCKDB_LIB := build/release/src/libduckdb.so
endif
TEST_CPP_LINK = -L build/release/src -lduckdb -Wl,-rpath,$(abspath build/release/src)

# the extension's own translation units a test compiles in (they are not exported from libduckdb)
$(TEST_CPP_DIR)/test_acl_otel_rules: TEST_CPP_EXTRA := src/acl_otel_rules.cpp
$(TEST_CPP_DIR)/test_acl_otel_rules: src/acl_otel_rules.cpp src/include/acl_otel.hpp
# the sink observes the metrics accumulators (spec 003), so the test links them too
$(TEST_CPP_DIR)/test_acl_otel_sink: TEST_CPP_EXTRA := src/acl_otel_sink.cpp src/acl_otel_metrics.cpp \
	src/acl_otel_sampling.cpp duckdb/third_party/yyjson/yyjson.cpp
$(TEST_CPP_DIR)/test_acl_otel_sampling: TEST_CPP_EXTRA := src/acl_otel_sampling.cpp \
	duckdb/third_party/yyjson/yyjson.cpp
$(TEST_CPP_DIR)/test_acl_otel_sampling: src/acl_otel_sampling.cpp src/include/acl_otel.hpp
$(TEST_CPP_DIR)/test_acl_otel_health: TEST_CPP_EXTRA := src/acl_otel_sampling.cpp \
	duckdb/third_party/yyjson/yyjson.cpp
$(TEST_CPP_DIR)/test_acl_otel_health: src/acl_otel_sampling.cpp src/include/acl_otel.hpp
$(TEST_CPP_DIR)/test_acl_otel_sink: src/acl_otel_sink.cpp src/acl_otel_metrics.cpp src/include/acl_otel.hpp
$(TEST_CPP_DIR)/test_acl_otel_metrics: TEST_CPP_EXTRA := src/acl_otel_metrics.cpp duckdb/third_party/yyjson/yyjson.cpp
$(TEST_CPP_DIR)/test_acl_otel_metrics: src/acl_otel_metrics.cpp src/include/acl_otel_metrics.hpp

$(TEST_CPP_DIR)/%: test/cpp/%.cpp $(TEST_CPP_DUCKDB_LIB)
	@mkdir -p $(TEST_CPP_DIR)
	$(CXX) $(TEST_CPP_FLAGS) $(TEST_CPP_INCLUDES) $< $(TEST_CPP_EXTRA) $(TEST_CPP_LINK) -o $@

.PHONY: test-cpp test-cpp-run
test-cpp:
	@test -f $(TEST_CPP_DUCKDB_LIB) || { \
		echo "test-cpp: $(TEST_CPP_DUCKDB_LIB) missing - run 'GEN=ninja make' first" >&2; exit 1; }
	@$(MAKE) --no-print-directory test-cpp-run

# the transport's test is a CMake target (it links the SDK): built here on demand, run with the rest
TEST_CPP_CMAKE_BINS := build/release/extension/acl_otel/acl_otel_test_otlp \
	build/release/extension/acl_otel/acl_otel_test_metrics_otlp

test-cpp-run: $(TEST_CPP_BINS)
	@test -n "$(TEST_CPP_BINS)" || { echo "test-cpp: no test/cpp/test_*.cpp sources found" >&2; exit 1; }
	@cmake --build build/release --target acl_otel_test_otlp acl_otel_test_metrics_otlp \
		> build/test/cmake-tests.log 2>&1 || \
		{ cat build/test/cmake-tests.log; exit 1; }
	@fail=0; for b in $(TEST_CPP_BINS) $(TEST_CPP_CMAKE_BINS); do \
		if "$$b" > "$$b.log" 2>&1; then echo "  PASS $$(basename $$b)"; \
		else echo "  FAIL $$(basename $$b)"; cat "$$b.log"; fail=1; fi; \
	done; [ $$fail = 0 ]

# --- clang-tidy ---------------------------------------------------------------------------------
# The community pipeline's Tidy Check cannot run here (it configures without vcpkg, and this
# extension needs the SDK to configure at all), so tidy is ours: over our own sources, against the
# compile database the release build already wrote. macOS needs homebrew's LLVM and an -isysroot;
# a Linux runner has run-clang-tidy on PATH.
LLVM_BIN ?= $(shell ls -d /opt/homebrew/opt/llvm/bin 2>/dev/null || echo /usr/bin)
TIDY_SOURCES := $(wildcard src/*.cpp)
TIDY_SYSROOT := $(shell xcrun --show-sdk-path 2>/dev/null)

.PHONY: tidy
tidy:
	@test -f build/release/compile_commands.json || { \
		echo "tidy: no compile database - run 'GEN=ninja make' first" >&2; exit 1; }
	@$(LLVM_BIN)/run-clang-tidy -clang-tidy-binary $(LLVM_BIN)/clang-tidy -p build/release -quiet \
		-header-filter='.*/acl-otel/src/include/.*' \
		$(if $(TIDY_SYSROOT),-extra-arg=-isysroot -extra-arg=$(TIDY_SYSROOT),) \
		$(abspath $(TIDY_SOURCES)) 2>/dev/null \
		| grep -E '^$(CURDIR)/src/[^ ]+:[0-9]+:[0-9]+: (warning|error)' | sort -u \
		| sed 's|^$(CURDIR)/||' ; true

# what CI runs: the same, but a finding fails the step
.PHONY: tidy-ci
tidy-ci:
	@out="$$($(MAKE) --no-print-directory tidy)"; \
		if [ -n "$$out" ]; then echo "$$out"; echo "tidy: findings above" >&2; exit 1; fi; \
		echo "tidy: clean"

# Bootstrap a local vcpkg checkout (the standard duckdb-extension dependency flow)
.PHONY: vcpkg-setup
vcpkg-setup:
	@test -d vcpkg || git clone https://github.com/microsoft/vcpkg.git vcpkg
	@test -x vcpkg/vcpkg || vcpkg/bootstrap-vcpkg.sh -disableMetrics
