#!/usr/bin/env bash
# SPDX-License-Identifier: MIT

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
temporary_dir=$(mktemp -d)

cleanup() {
    rm -rf "$temporary_dir"
}

trap cleanup EXIT

node "$script_dir/sync-design-system.mjs" --check
node "$script_dir/build-tokens.mjs" --check

cp "$repo_root/src/api/web_ui_tokens.inc" "$temporary_dir/web_ui_tokens.inc"
printf '\n/* changed */\n' >>"$temporary_dir/web_ui_tokens.inc"
if node "$script_dir/build-tokens.mjs" --check-file "$temporary_dir/web_ui_tokens.inc" >/dev/null 2>&1; then
    printf 'token gate accepted a changed generated file\n' >&2
    exit 1
fi

cp -R "$repo_root/vendor/orama-design" "$temporary_dir/orama-design"
printf '\n' >>"$temporary_dir/orama-design/tokens/dtcg/primitive/color.tokens.json"
if node "$script_dir/sync-design-system.mjs" --check --vendor-dir "$temporary_dir/orama-design" >/dev/null 2>&1; then
    printf 'snapshot gate accepted a changed vendored token\n' >&2
    exit 1
fi

printf 'token-gates: ok — generated output and snapshot changes were rejected\n'
