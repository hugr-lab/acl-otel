# Development

The development guide is [CLAUDE.md](https://github.com/hugr-lab/acl-otel/blob/main/CLAUDE.md) in the repository. It covers the pins,
the contract, and the rules that hold here.

## Build

```sh
git submodule update --init --recursive
make vcpkg-setup          # once: the OpenTelemetry SDK (otlp-http, otlp-grpc) comes from vcpkg
GEN=ninja make            # release build of duckdb + the extension
```

acl-otel and duckdb-acl load into one DuckDB, so they pin the **same** DuckDB. A bump of the base's
pin is a bump here on the same day.

## Test

```sh
build/release/test/unittest 'test/sql/*'
GEN=ninja make test-cpp                                  # includes the OTLP tests against in-process receivers
ACL_EXT=/path/to/acl.duckdb_extension build/release/test/unittest test/sql/acl_otel_beside_acl.test
make tidy-ci                                             # clang-tidy; a finding fails
```

The beside-acl test is the proof that two loadables share one registry, and CI runs it. A change that
passes only the standalone suite is not done.

## Specs

Each feature has one short spec under [specs/](https://github.com/hugr-lab/acl-otel/tree/main/specs).

## These docs

```sh
cd website && npm ci && npx docusaurus start
```

Links between pages are relative. `scripts/ci/check_docs_links.py` and the build's broken-link check
enforce that.
