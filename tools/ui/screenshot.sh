#!/usr/bin/env bash

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
mocks_dir="$repo_root/docs/design/ui/mocks"
screenshots_dir="$mocks_dir/screenshots"
docker_image="ghcr.io/puppeteer/puppeteer:24.16.0"

if (($# > 0)); then
    printf 'usage: %s (renders the static mocks; it takes no arguments)\n' "$0" >&2
    exit 2
fi

mkdir -p "$screenshots_dir" "$repo_root/.cache/ui-render"

temporary_files=()

cleanup() {
    if ((${#temporary_files[@]} > 0)); then
        rm -f "${temporary_files[@]}"
    fi
}

trap cleanup EXIT

render_with_docker() {
    local source_file=$1
    local output_file=$2
    local network_log=$3
    local source_relative=${source_file#"$repo_root/"}
    local output_relative=${output_file#"$repo_root/"}
    local network_relative=${network_log#"$repo_root/"}

    docker run --rm \
        --platform linux/amd64 \
        --volume "$repo_root:/work" \
        --workdir /work \
        "$docker_image" \
        node /work/tools/ui/screenshot_driver.mjs "$source_relative" "$output_relative" "$network_relative"
}

render() {
    local direction=$1
    local screen=$2
    local theme=$3
    local source_file="$mocks_dir/$direction/$screen.html"
    local render_file=$source_file
    local output_file="$screenshots_dir/$direction-$screen-$theme.png"
    local network_log="$repo_root/.cache/ui-render/static-$direction-$screen-$theme-network.json"

    if [[ $theme == dark ]]; then
        render_file="$mocks_dir/$direction/.screenshot-dark-$screen.html"
        temporary_files+=("$render_file")
        sed 's/data-theme="light"/data-theme="dark"/' "$source_file" >"$render_file"
    fi

    render_with_docker "$render_file" "$output_file" "$network_log"

    printf 'rendered %s\n' "${output_file#"$repo_root/"}"
}

for screen in repository change organization; do
    render quiet "$screen" light
    render quiet "$screen" dark
    render orama "$screen" light
    render editorial "$screen" light
    render editorial "$screen" dark
done
