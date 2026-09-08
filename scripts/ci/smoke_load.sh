#!/usr/bin/env bash
# The artifact must LOAD, not only compile: symbol visibility, a runtime dependency that resolved on
# the build machine only, an init that throws - all pass a build and fail the first user. The built
# file is copied out of the tree and loaded by the CLI from a different directory, as an operator
# would. Scenarios:
#   1. alone: the extension loads, answers its version, reports itself attached to the registry it
#      created (the base adopts that registry when it loads later - spec 069 C2);
#   2. beside acl, when ACL_EXT names the base's built extension: both load, the status says so.
#
#   scripts/ci/smoke_load.sh [<extension file>] [<duckdb binary>]
set -euo pipefail
ext="${1:-build/release/extension/acl_otel/acl_otel.duckdb_extension}"
duckdb="${2:-build/release/duckdb}"
[ -f "$ext" ] || { echo "smoke_load: no extension at $ext" >&2; exit 1; }
[ -x "$duckdb" ] || { echo "smoke_load: no duckdb CLI at $duckdb" >&2; exit 1; }
ext_abs="$(cd "$(dirname "$ext")" && pwd)/$(basename "$ext")"
duckdb_abs="$(cd "$(dirname "$duckdb")" && pwd)/$(basename "$duckdb")"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
cp "$ext_abs" "$tmp/acl_otel.duckdb_extension"
cd "$tmp"

out="$("$duckdb_abs" -unsigned -csv -noheader -c "
LOAD '$tmp/acl_otel.duckdb_extension';
SELECT 'version=' || coalesce(acl_otel_version(), '<null>');
SELECT 'attached=' || (acl_otel_status() LIKE '{\"attached\":true,%');
SELECT 'stopped=' || acl_otel_stop();
" 2>&1)" || { echo "smoke_load: the artifact did not load:" >&2; echo "$out" >&2; exit 1; }
grep -q '^attached=true$' <<<"$out" || { echo "smoke_load: not attached after load:" >&2; echo "$out" >&2; exit 1; }
grep -q '^stopped=true$' <<<"$out" || { echo "smoke_load: stop did not detach:" >&2; echo "$out" >&2; exit 1; }

beside="beside acl: not checked (ACL_EXT not set)"
if [ -n "${ACL_EXT:-}" ]; then
	[ -f "$ACL_EXT" ] || { echo "smoke_load: ACL_EXT names no file: $ACL_EXT" >&2; exit 1; }
	both="$("$duckdb_abs" -unsigned -csv -noheader -c "
LOAD '$ACL_EXT';
LOAD '$tmp/acl_otel.duckdb_extension';
SELECT 'both=' || (acl_otel_status() LIKE '%\"acl_loaded\":true%');
" 2>&1)" || { echo "smoke_load: did not load beside acl:" >&2; echo "$both" >&2; exit 1; }
	grep -q '^both=true$' <<<"$both" || { echo "smoke_load: beside acl, the registry is not shared:" >&2; echo "$both" >&2; exit 1; }
	beside="beside acl: shared registry"
fi

echo "smoke_load: ok (loads alone, attaches, detaches; $beside; size $(wc -c <"$ext_abs" | tr -d ' ') bytes)"
