# acl_otel

OpenTelemetry for [duckdb-acl](https://github.com/hugr-lab/duckdb-acl)'s audit: a duckdb extension
loaded beside `acl` on the same instance that turns the base's audit events and counters into OTel
**logs** and **metrics**, decides the audit level **per role, per user and per door**, samples,
and reports its own health. It contains no enforcement, changes no decision, and can never slow or
stop one.

It compiles against one header of the base, `acl_audit.hpp`, and reaches the base through duckdb's
object cache (spec 069 of duckdb-acl). The contract it meets is
[`specs/069-audit/extension-requirements.md`](https://github.com/hugr-lab/duckdb-acl/blob/main/specs/069-audit/extension-requirements.md)
in the base; what is built so far is in [`specs/`](specs/README.md).

## Status

Spec 001 (the wiring): the sink and the level policy attach to the base's registry in either load
order, `acl_otel_level_rules` decides a session's level (first match: role / subject / issuer /
door), `acl_otel_status()` reports the extension's own numbers, `acl_otel_stop()` /
`acl_otel_start()` detach and re-attach. **No transport yet**: `acl_otel_endpoint` is accepted and
nothing is exported until spec 002 (OTLP via the OpenTelemetry C++ SDK) lands - every event the
base hands over is counted as dropped for "no exporter", never silently.

```sql
LOAD acl;
LOAD acl_otel;
SET GLOBAL acl_otel_level_rules = '[{"role": "analyst", "door": "flight", "level": "all"}, {"level": "denied"}]';
SELECT acl_otel_status();
```

## Building

```sh
git submodule update --init --recursive
GEN=ninja make                       # release build of duckdb + the extension
build/release/test/unittest 'test/sql/*'
GEN=ninja make test-cpp
ACL_EXT=/path/to/acl.duckdb_extension build/release/test/unittest test/sql/acl_otel_beside_acl.test
```

The pins ride duckdb-acl's: duckdb's 2.0 release branch `v2.0-cyanoptera`, extension-ci-tools
`main`, the base's `main` for the contract header (`duckdb-acl/` submodule, headers only).

## License

Business Source License 1.1 - see [LICENSE](LICENSE). Production use is permitted except as a hosted
or managed service offered to third parties; each version becomes Apache-2.0 four years after its
release. The parameters (Additional Use Grant, Change Date, Change License) are the licensor's and
may be revised before the first release.
