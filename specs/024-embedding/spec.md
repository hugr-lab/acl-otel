# Spec 024: acl_otel built into another extension

- **Status**: implemented
- **Date**: 2026-10-14

## Problem

hugr-node's `hugr_node` is duckdb-acl and tresor built into one extension (hugr-node specs 001/002). A platform node
also needs acl_otel. Shipped as a loadable beside `hugr_node`, it would be a second 57 MB artifact with its own gRPC,
and the node's bootstrap would have to load it. The owner's decision (2026-10-14): build acl_otel into `hugr_node`,
inert until an endpoint is configured, and the endpoint is one for the whole cluster. That last part is hugr-node's
rule (its spec 005); this spec is acl_otel's side, the same hooks tresor has (tresor spec 024).

## Design

- **`acl_otel_sources.cmake`**: what acl_otel compiles and links, with this repository's paths (`ACL_OTEL_ROOT`):
  the include directories (`src/include`, duckdb-ext-common's `contracts/` and `hooks/`, the build's own yyjson), the
  SDK (`find_package(opentelemetry-cpp)`, `ACL_OTEL_SDK_LIBS`), `ACL_OTEL_LIB_SOURCES` and `ACL_OTEL_SOURCES`, and
  `acl_otel_configure_targets(<targets>)`, which links the SDK libraries. `CMakeLists.txt` includes it; its own build is
  unchanged (the same sources, includes, libraries and SDK tests).
- **`ACL_OTEL_EMBEDDED`** (set by the embedder before the include): a property of acl_otel's sources, so another
  extension compiled into the same target is not affected. Under it, `acl_otel_extension.cpp` defines no
  `DUCKDB_EXTENSION_MAIN`, no `AclOtelExtension` and no entry point: the embedder's are the ones.
- **`LoadAclOtelCore(loader)`**: the whole registration (settings, functions, the sink on the base's hooks), what the
  entry point calls and what an embedder calls after loading acl's core.
- **The version**: duckdb defines `EXT_VERSION_<NAME>` for the extension it builds, which inside an embedder is the
  embedder's. `acl_otel_sources.cmake` then defines `ACL_OTEL_VERSION` (acl-otel's commit or tag, from
  `duckdb_extension_generate_version` over `ACL_OTEL_ROOT`) on acl_otel's sources, and `acl_otel_version.hpp` gives
  the three places that name the version (`acl_otel_version()`, the OTLP resource's `acl_otel.version`, the lineage
  producer URL) one `ACL_OTEL_VERSION_TEXT`.

## Enforcement / security

Nothing changes in what acl_otel does: it observes, never decides. Built in, it attaches to the same hook registries
(`AuditHooks::Reach`, tresor's) as the loadable, and with `acl_otel_endpoint` empty it exports nothing.

## Testing

acl_otel's own gate, unchanged: the SQL suite (with `ACL_EXT`), test-cpp (with `ACL_EXT`), the smoke load, clang-format,
clang-tidy. The embedded build and its tests are hugr-node's spec 005.

## Alternatives

- **A loadable beside `hugr_node`**: a second artifact with a second gRPC, and a bootstrap that must load it - refused
  by the owner.
- **hugr-node lists acl_otel's sources itself**: the list would drift from this repository's; the file is the contract,
  as tresor's `tresor_sources.cmake` is.
