#!/usr/bin/env bash

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
mocks_dir="$repo_root/docs/design/ui/mocks"
screenshots_dir="$mocks_dir/screenshots"
docker_image="ghcr.io/puppeteer/puppeteer:24.16.0"

mkdir -p "$screenshots_dir"

browser=""
for candidate in chromium google-chrome google-chrome-stable chromium-browser; do
    if command -v "$candidate" >/dev/null 2>&1; then
        browser=$(command -v "$candidate")
        break
    fi
done

temporary_files=()

cleanup() {
    if ((${#temporary_files[@]} > 0)); then
        rm -f "${temporary_files[@]}"
    fi
}

trap cleanup EXIT

render_with_local_browser() {
    local source_file=$1
    local output_file=$2

    "$browser" \
        --headless=new \
        --disable-gpu \
        --hide-scrollbars \
        --run-all-compositor-stages-before-draw \
        --force-device-scale-factor=1 \
        --window-size=1440,900 \
        --screenshot="$output_file" \
        "file://$source_file" >/dev/null 2>&1
}

render_with_docker() {
    local source_file=$1
    local output_file=$2
    local source_relative=${source_file#"$repo_root/"}
    local output_relative=${output_file#"$repo_root/"}

    docker run --rm \
        --entrypoint /bin/sh \
        --platform linux/amd64 \
        --volume "$repo_root:/work" \
        --workdir /work \
        "$docker_image" \
        -c 'browser_path=$(find /home/pptruser/.cache/puppeteer/chrome -type f -name chrome | head -1)
            "$browser_path" \
                --headless=new \
                --no-sandbox \
                --disable-gpu \
                --hide-scrollbars \
                --run-all-compositor-stages-before-draw \
                --force-device-scale-factor=1 \
                --window-size=1440,900 \
                --screenshot="/work/$2" \
                "file:///work/$1" >/dev/null 2>&1' \
        screenshot-render "$source_relative" "$output_relative"
}

render() {
    local direction=$1
    local screen=$2
    local theme=$3
    local source_file="$mocks_dir/$direction/$screen.html"
    local render_file=$source_file
    local output_file="$screenshots_dir/$direction-$screen-$theme.png"

    if [[ $theme == dark ]]; then
        render_file="$mocks_dir/$direction/.screenshot-dark-$screen.html"
        temporary_files+=("$render_file")
        sed 's/data-theme="light"/data-theme="dark"/' "$source_file" >"$render_file"
    fi

    if [[ -n $browser ]]; then
        render_with_local_browser "$render_file" "$output_file"
    else
        render_with_docker "$render_file" "$output_file"
    fi

    printf 'rendered %s\n' "${output_file#"$repo_root/"}"
}

for screen in repository change organization; do
    render quiet "$screen" light
    render quiet "$screen" dark
    render orama "$screen" light
    render editorial "$screen" light
    render editorial "$screen" dark
done
