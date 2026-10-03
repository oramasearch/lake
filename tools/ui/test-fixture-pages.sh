#!/usr/bin/env bash
# Starts lake-ui-fixture and requests every page it seeds and every GET route the server
# registered (lake#497). A seeded page must answer 200 with HTML; every registered route must
# answer below 500, signed in and signed out; and the fixture must still be running afterwards.
#
# usage: test-fixture-pages.sh PATH_TO_LAKE_UI_FIXTURE

set -euo pipefail

fixture=${1:?usage: test-fixture-pages.sh PATH_TO_LAKE_UI_FIXTURE}
work_dir=$(mktemp -d)
fixture_pid=""

cleanup() {
    if [[ -n $fixture_pid ]] && kill -0 "$fixture_pid" 2>/dev/null; then
        kill "$fixture_pid" 2>/dev/null || true
        wait "$fixture_pid" 2>/dev/null || true
    fi
    rm -rf "$work_dir"
}
trap cleanup EXIT

manifest="$work_dir/pages.tsv"
"$fixture" "$work_dir/store" "$manifest" 0 >"$work_dir/stdout.log" 2>"$work_dir/stderr.log" &
fixture_pid=$!

port=""
for _ in $(seq 1 600); do
    if ! kill -0 "$fixture_pid" 2>/dev/null; then
        printf 'lake-ui-fixture exited during startup:\n' >&2
        cat "$work_dir/stderr.log" "$work_dir/stdout.log" >&2
        exit 1
    fi
    port=$(sed -n 's/^{"event":"startup".*"port":\([0-9][0-9]*\)}$/\1/p' "$work_dir/stdout.log")
    [[ -n $port ]] && break
    sleep 0.1
done
if [[ -z $port ]]; then
    printf 'lake-ui-fixture did not report startup\n' >&2
    cat "$work_dir/stderr.log" >&2
    exit 1
fi

base="http://127.0.0.1:$port"
failures=0

request() {
    local token=$1 route=$2
    local args=(-s -o "$work_dir/body" -w '%{http_code} %{content_type}' --max-time 30)
    if [[ $token != - ]]; then
        args+=(--cookie "lake_session=$token")
    fi
    curl "${args[@]}" "$base$route" || printf '000 -'
}

fail() {
    printf 'FAIL %s\n' "$1" >&2
    failures=$((failures + 1))
}

pages=0
session=""
while IFS=$'\t' read -r slug token route; do
    pages=$((pages + 1))
    [[ $slug == sessions ]] && session=$token
    read -r status content_type <<<"$(request "$token" "$route")"
    if [[ $status != 200 || $content_type != text/html* ]]; then
        fail "page $slug $route -> $status $content_type"
    fi
done <"$manifest"
if ((pages == 0)); then
    fail "the manifest lists no pages"
fi

routes=0
while IFS= read -r route; do
    routes=$((routes + 1))
    for token in "$session" ui-fixture-admin -; do
        read -r status _ <<<"$(request "$token" "$route")"
        if [[ $status == 000 || $status -ge 500 ]]; then
            fail "route $route (session ${token:0:12}) -> $status"
        fi
    done
done <"$manifest.routes"
if ((routes == 0)); then
    fail "the fixture listed no registered GET routes"
fi

if ! kill -0 "$fixture_pid" 2>/dev/null; then
    fail "lake-ui-fixture stopped while serving"
    cat "$work_dir/stderr.log" >&2
fi

if ((failures > 0)); then
    printf 'fixture-pages: %d failure(s) over %d pages and %d routes\n' "$failures" "$pages" "$routes" >&2
    exit 1
fi
printf 'fixture-pages: ok — %d seeded pages rendered, %d registered GET routes below 500\n' \
    "$pages" "$routes"
