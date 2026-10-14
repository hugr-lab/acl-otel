#pragma once

// acl_otel's own version, wherever it is built (spec 024): duckdb defines EXT_VERSION_<NAME> for the extension it
// builds - acl_otel's in its own build, the embedder's when another extension builds acl_otel in; there
// acl_otel_sources.cmake defines ACL_OTEL_VERSION (acl-otel's commit or tag) on acl_otel's sources instead.
// Neither (the SDK tests) leaves ACL_OTEL_VERSION_TEXT undefined.
#if defined(ACL_OTEL_VERSION)
#define ACL_OTEL_VERSION_TEXT ACL_OTEL_VERSION
#elif defined(EXT_VERSION_ACL_OTEL)
#define ACL_OTEL_VERSION_TEXT EXT_VERSION_ACL_OTEL
#endif
