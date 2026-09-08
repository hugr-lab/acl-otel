PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=acl_otel
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# No vcpkg yet: the first slice (spec 001) is the contract wiring and needs nothing beyond duckdb.
# spec 002 brings the OpenTelemetry C++ SDK through vcpkg (opentelemetry-cpp[otlp-grpc,otlp-http]),
# with the merged-manifest block and the fail-fast guard duckdb-acl's Makefile carries.

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

# --- Standalone C++ tests (the duckdb-acl / mssql-extension style) -----------------------------
# Each test/cpp/test_*.cpp is its own program, built from the already-compiled release tree and
# linked against the shared libduckdb. Same generator as the main build: GEN=ninja make test-cpp.
TEST_CPP_SOURCES := $(wildcard test/cpp/test_*.cpp)
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
$(TEST_CPP_DIR)/test_acl_otel_sink: TEST_CPP_EXTRA := src/acl_otel_sink.cpp
$(TEST_CPP_DIR)/test_acl_otel_sink: src/acl_otel_sink.cpp src/include/acl_otel.hpp

$(TEST_CPP_DIR)/%: test/cpp/%.cpp $(TEST_CPP_DUCKDB_LIB)
	@mkdir -p $(TEST_CPP_DIR)
	$(CXX) $(TEST_CPP_FLAGS) $(TEST_CPP_INCLUDES) $< $(TEST_CPP_EXTRA) $(TEST_CPP_LINK) -o $@

.PHONY: test-cpp test-cpp-run
test-cpp:
	@test -f $(TEST_CPP_DUCKDB_LIB) || { \
		echo "test-cpp: $(TEST_CPP_DUCKDB_LIB) missing - run 'GEN=ninja make' first" >&2; exit 1; }
	@$(MAKE) --no-print-directory test-cpp-run

test-cpp-run: $(TEST_CPP_BINS)
	@test -n "$(TEST_CPP_BINS)" || { echo "test-cpp: no test/cpp/test_*.cpp sources found" >&2; exit 1; }
	@fail=0; for b in $(TEST_CPP_BINS); do \
		if "$$b" > "$$b.log" 2>&1; then echo "  PASS $$(basename $$b)"; \
		else echo "  FAIL $$(basename $$b)"; cat "$$b.log"; fail=1; fi; \
	done; [ $$fail = 0 ]
