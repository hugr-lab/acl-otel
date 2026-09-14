#!/usr/bin/env bash
# The beside-acl proof needs the base's real artifact (spec 001: two loadables on one instance is
# the whole point, and a suite that runs only alone proves the wiring on nobody). Fetch the base's
# extension from a green `main` CI run of duckdb-acl - a public repository, so the workflow's own
# token reads it - and print the path for ACL_EXT.
#
# The run to take is NOT simply the newest green one: a duckdb extension loads only into the duckdb
# it was built against, so the artifact has to be built against the duckdb THIS repo pins. The base
# publishes that fact per commit (its `duckdb` submodule), so each candidate run is checked against
# our own pin before it is downloaded. Newest first, and the commit our `duckdb-acl` submodule pins
# is tried before all of them.
#
# A run built against another duckdb is skipped with the reason said out loud. If none matches, the
# failure names both pins instead of surfacing later as "The file was built specifically for DuckDB
# version ..." from LOAD - which is the same problem seen three steps further from its cause. The
# usual case is a base bump whose own CI has not gone green yet: re-run this job once it has.
#
#   scripts/ci/fetch_base.sh <platform: linux_amd64 | osx_arm64> [dest dir]
set -euo pipefail
platform="${1:?platform (linux_amd64 | osx_arm64)}"
dest="${2:-base}"
repo="hugr-lab/duckdb-acl"

want_duckdb="$(git rev-parse HEAD:duckdb 2>/dev/null || true)"
want_base="$(git rev-parse HEAD:duckdb-acl 2>/dev/null || true)"
test -n "$want_duckdb" || {
	echo "fetch_base: cannot read the pinned duckdb commit - is this a checkout with submodules?" >&2
	exit 1
}
echo "fetch_base: this repo pins duckdb $want_duckdb, base $want_base"

# Not every green run carries every platform (a job can be skipped and the run still succeed), so
# there are several candidates; the pinned base commit goes first, then the newest green runs.
# plain string + while-read rather than mapfile: a macOS runner's bash is 3.2
candidates="$(
	{
		if [ -n "$want_base" ]; then
			gh run list -R "$repo" -w CI -b main -s success --limit 20 --json databaseId,headSha \
				--jq ".[] | select(.headSha == \"$want_base\") | \"\(.databaseId) \(.headSha)\""
		fi
		gh run list -R "$repo" -w CI -b main -s success --limit 10 --json databaseId,headSha \
			--jq '.[] | "\(.databaseId) \(.headSha)"'
	} | awk '!seen[$1]++'
)"
test -n "$candidates" || {
	echo "fetch_base: no successful CI run of $repo main found" >&2
	exit 1
}

rm -rf "$dest"
while read -r run_id sha; do
	test -n "$run_id" || continue
	pin="$(gh api "repos/$repo/contents/duckdb?ref=$sha" --jq .sha 2>/dev/null || true)"
	if [ "$pin" != "$want_duckdb" ]; then
		echo "fetch_base: skipping run $run_id ($sha): built against duckdb ${pin:-unknown}"
		continue
	fi
	echo "fetch_base: trying $repo main run $run_id ($sha) for artifact acl-$platform"
	if gh run download -R "$repo" "$run_id" -n "acl-$platform" -D "$dest" 2>/dev/null; then
		break
	fi
	echo "fetch_base: ... that run has no acl-$platform artifact"
done <<CANDIDATES
$candidates
CANDIDATES

ext="$dest/acl.duckdb_extension"
test -f "$ext" || {
	echo "fetch_base: no acl-$platform artifact built against duckdb $want_duckdb in the recent green" >&2
	echo "  main runs of $repo. If the base has just bumped its pin, its own CI is probably still" >&2
	echo "  running - re-run this job when it is green. Otherwise the two pins have diverged and the" >&2
	echo "  fix is a submodule bump here, never a metadata-mismatch override." >&2
	exit 1
}
echo "fetch_base: $ext ($(wc -c < "$ext" | tr -d " ") bytes)"
echo "$ext"
