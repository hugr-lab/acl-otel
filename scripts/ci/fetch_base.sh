#!/usr/bin/env bash
# The beside-acl proof needs the base's real artifact (spec 001: two loadables on one instance is
# the whole point, and a suite that runs only alone proves the wiring on nobody). Fetch the base's
# extension from duckdb-acl's latest green `main` CI run - a public repository, so the workflow's
# own token reads it - and print the path for ACL_EXT. The two must share one duckdb pin (CLAUDE.md:
# bumped together, the same day); a version mismatch shows up as LOAD refusing the base's artifact,
# and the fix is the pin bump, never a metadata-mismatch override.
#
#   scripts/ci/fetch_base.sh <platform: linux_amd64 | osx_arm64> [dest dir]
set -euo pipefail
platform="${1:?platform (linux_amd64 | osx_arm64)}"
dest="${2:-base}"
repo="hugr-lab/duckdb-acl"
# The newest green run is the one to take, but not every green run carries every platform (a job
# can be skipped and the run still succeed), so walk back a few before giving up.
runs="$(gh run list -R "$repo" -w CI -b main -s success --limit 5 --json databaseId --jq '.[].databaseId')"
if [ -z "$runs" ]; then
	echo "fetch_base: no successful CI run of $repo main found" >&2
	exit 1
fi
rm -rf "$dest"
for run_id in $runs; do
	sha="$(gh run view -R "$repo" "$run_id" --json headSha --jq .headSha)"
	echo "fetch_base: trying $repo main run $run_id ($sha) for artifact acl-$platform"
	if gh run download -R "$repo" "$run_id" -n "acl-$platform" -D "$dest" 2>/dev/null; then
		break
	fi
	echo "fetch_base: ... that run has no acl-$platform artifact"
done
ext="$dest/acl.duckdb_extension"
test -f "$ext" || {
	echo "fetch_base: no acl-$platform artifact in the last 5 green main runs of $repo" >&2
	exit 1
}
echo "fetch_base: $ext ($(wc -c < "$ext") bytes)"
echo "$ext"
