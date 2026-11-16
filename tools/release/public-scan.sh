#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# public-scan.sh: look for internal material in the tracked tree before it is published.
#
# It greps every file Git tracks (or a given revision's tree) for generic patterns: personal
# company addresses, internal hosts, local home-directory paths, agent instruction files and
# private-repository names. It prints a count per category and, with --list, every matching line.
# It exits 1 when anything matches and 0 when the tree is clean. Agent instruction files
# (CLAUDE.md, AGENTS.md) are listed for information and do not fail the scan: they stay.
#
# The patterns are deliberately generic, so this script is safe to publish. A release review may
# run a stricter, private pattern list as well; see docs/open-source/README.md.
#
# Usage: tools/release/public-scan.sh [--list] [<revision>]

set -euo pipefail

list=0
revision=""
for arg in "$@"; do
    case "$arg" in
    --list) list=1 ;;
    -h | --help)
        sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'
        exit 0
        ;;
    -*)
        echo "public-scan: unknown option $arg" >&2
        exit 2
        ;;
    *) revision="$arg" ;;
    esac
done

cd "$(git rev-parse --show-toplevel)"
self="tools/release/public-scan.sh"

# category|extended regular expression
categories=(
    # Personal addresses at the company's domains; role addresses stay.
    'people|[A-Za-z0-9._%+-]+@orama(search)?\.(com|net|dev)'
    # Hosts of Orama's own development and staging deployments.
    'hosts|[A-Za-z0-9.-]+\.oramadev\.net'
    # Local home-directory paths.
    'paths|(/Users/[A-Za-z][A-Za-z0-9._-]+/|/home/[a-z][a-z0-9._-]+/)'
    # Private umbrella repositories. zetta-bridge and liquid-bridge are named on purpose: they are
    # the build's documented sources, submodules of the public lake-platform repository.
    'private-refs|(orama-bridge|orama-work)'
)
# Role addresses a public tree may name.
allowed_people='(support|security|opensource|conduct|noreply|no-reply|hello|info)@orama'
# Home directories of container users (the image's own user, the browser image's), not people.
allowed_paths='/home/(lake|pptruser)/'

agent_files=(CLAUDE.md AGENTS.md .claude .codex)
# The agent instruction files stay in the repository; their content is not scanned.
kept=(":(exclude)CLAUDE.md" ":(exclude)AGENTS.md")

grep_tree() {
    local pattern="$1"
    if [[ -n "$revision" ]]; then
        git grep -n -I -E -e "$pattern" "$revision" -- . ":(exclude)$self" "${kept[@]}" || true
    else
        git grep -n -I -E -e "$pattern" -- . ":(exclude)$self" "${kept[@]}" || true
    fi
}

total=0
for entry in "${categories[@]}"; do
    name="${entry%%|*}"
    pattern="${entry#*|}"
    matches="$(grep_tree "$pattern")"
    if [[ "$name" == people && -n "$matches" ]]; then
        matches="$(grep -v -E "$allowed_people" <<<"$matches" || true)"
    fi
    if [[ "$name" == paths && -n "$matches" ]]; then
        matches="$(grep -v -E "$allowed_paths" <<<"$matches" || true)"
    fi
    count=0
    [[ -n "$matches" ]] && count="$(wc -l <<<"$matches" | tr -d ' ')"
    printf '%-14s %s\n' "$name" "$count"
    total=$((total + count))
    if ((list && count > 0)); then
        sed 's/^/    /' <<<"$matches"
    fi
done

found_agents=()
for path in "${agent_files[@]}"; do
    if [[ -n "$revision" ]]; then
        git cat-file -e "$revision:$path" 2>/dev/null && found_agents+=("$path")
    else
        [[ -n "$(git ls-files -- "$path")" ]] && found_agents+=("$path")
    fi
done
printf '%-14s %s (kept; not counted)\n' agent-files "${#found_agents[@]}"
if ((list && ${#found_agents[@]} > 0)); then
    printf '    %s\n' "${found_agents[@]}"
fi

printf '%-14s %s\n' total "$total"
((total == 0))
