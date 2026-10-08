#!/usr/bin/env bash
# spec 018 e2e: Marquez in docker; a node (acl + acl_otel) defines virtual objects, grants a role,
# attaches a source and writes as the role under an external parent; Marquez's API then shows the
# datasets, their column lineage and tags, the source, and the run under its parent.
#   ACL_EXT=<duckdb-acl build>/extension/acl/acl.duckdb_extension test/e2e/marquez/run.sh
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
: "${ACL_EXT:?ACL_EXT names the built acl extension}"
otel="$root/build/release/extension/acl_otel/acl_otel.duckdb_extension"
cli="$root/build/release/duckdb"
api=http://127.0.0.1:5050
project=acl-otel-marquez

docker compose -p "$project" -f "$here/docker-compose.yml" up -d >/dev/null
trap '[ -n "${KEEP:-}" ] || docker compose -p "$project" -f "$here/docker-compose.yml" down -v >/dev/null 2>&1' EXIT
for _ in $(seq 1 60); do curl -sf "$api/api/v1/namespaces" >/dev/null && break; sleep 2; done

sql=$(sed -e "s#__ACL__#$ACL_EXT#" -e "s#__OTEL__#$otel#" -e "s#__URL__#$api#" "$here/node.sql")
out=$(printf "%s\n" "$sql" | "$cli" -unsigned -csv)
echo "$out"
status=$(echo "$out" | tail -1 | sed 's/""/"/g')

fail() { echo "FAIL: $*"; exit 1; }
ok() { echo "  ok:   $*"; }
enc() { python3 -c 'import sys,urllib.parse;print(urllib.parse.quote(sys.argv[1],safe=""))' "$1"; }
get() { curl -sf "$api$1"; }

echo "$status" | grep -q '"lost_retries":0' || fail "lost events: $status"
echo "$status" | grep -q '"rejected":0' || fail "rejected events: $status"
ok "delivered, nothing rejected or lost"

ns=$(enc acl://e2e/sales)
orders=$(get "/api/v1/namespaces/$ns/datasets/orders") || fail "no dataset sales.orders in Marquez"
# the facet as sent (Marquez's own `fields` keeps one copy per DatasetEvent version)
echo "$orders" | python3 -c 'import json,sys;d=json.load(sys.stdin);f=[x["name"] for x in d["facets"]["schema"]["fields"]];assert f==["id","amount","ssn","address"],f' \
	|| fail "orders' schema: $orders"
ok "the virtual table with its schema"
echo "$orders" | python3 -c '
import json,sys
d=json.load(sys.stdin)
tags=d["facets"]["tags"]["tags"]
have={(t["key"],t.get("field",""),t["value"]) for t in tags}
for want in [("acl.role.clerk","","insert,select"),("acl.role.clerk","id","visible"),("acl.role.clerk","address.city","visible")]:
    assert want in have,(want,have)
assert not any(t.get("field")=="ssn" for t in tags),tags
' || fail "clerk's tags: $orders"
ok "the role's tags: caps, visible fields, a narrowed path, the hidden field untagged"

# a definition's columnLineage travels as its DatasetEvent's facet (Marquez indexes column lineage
# from runs only, so its graph API shows the runs' edges below, the facet this one)
big=$(get "/api/v1/namespaces/$ns/datasets/big") || fail "no dataset sales.big"
echo "$big" | python3 -c '
import json,sys
f=json.load(sys.stdin)["facets"]["columnLineage"]["fields"]
inputs=f["doubled"]["inputFields"]
assert inputs[0]["namespace"]=="acl://e2e/source/phys" and inputs[0]["name"]=="main.orders" and inputs[0]["field"]=="amount",inputs
assert inputs[0]["transformations"][0]["subtype"]=="TRANSFORMATION",inputs
' || fail "the view's column lineage: $big"
ok "the view's column lineage reaches the physical source"

src=$(get "/api/v1/namespaces/$(enc acl://e2e/source/warehouse)/datasets/warehouse") || fail "no source warehouse"
ok "the attached source's namespace"

# Marquez names a job with a parent `<parent job>.<job>`
runs=$(get "/api/v1/namespaces/$(enc acl://e2e/client/gateway)/jobs/daily.load.dbt.sink_model/runs") ||
	fail "no job daily.load.dbt.sink_model"
echo "$runs" | python3 -c '
import json,sys
r=json.load(sys.stdin)["runs"]
assert r and r[0]["state"]=="COMPLETED",r
f=r[0]["facets"]
assert f["parent"]["run"]["runId"]=="01929e3a-0000-7000-8000-000000000001",f["parent"]
assert f["duckdb_acl"]["roles"]==["clerk"],f["duckdb_acl"]
' || fail "the run: $runs"
ok "the write's run, under its parent, with the duckdb_acl facet"

sink=$(get "/api/v1/column-lineage?nodeId=$(enc "dataset:acl://e2e/sales:sink")&depth=1") || fail "no column lineage for sales.sink"
echo "$sink" | python3 -c '
import json,sys
ids={n["id"] for n in json.load(sys.stdin)["graph"]}
assert "datasetField:acl://e2e/sales:orders:amount" in ids,ids
assert not any("source/phys" in i for i in ids),ids
' || fail "the run's column lineage: $sink"
ok "the run's edges are virtual: sink.total <- orders.amount, no physical name"
echo "PASS e2e marquez"
