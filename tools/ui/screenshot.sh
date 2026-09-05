#!/usr/bin/env bash

set -euo pipefail

script_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repo_root=$(cd "$script_dir/../.." && pwd)
mocks_dir="$repo_root/docs/design/ui/mocks"
screenshots_dir="$mocks_dir/screenshots"
docker_image="ghcr.io/puppeteer/puppeteer:24.16.0"

live_manifest=""
live_output_dir=""
base_url=""

while (($# > 0)); do
    case "$1" in
    --live-manifest)
        live_manifest=$2
        shift 2
        ;;
    --base-url)
        base_url=${2%/}
        shift 2
        ;;
    --output-dir)
        live_output_dir=$2
        shift 2
        ;;
    *)
        printf 'unknown argument: %s\n' "$1" >&2
        exit 2
        ;;
    esac
done

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
        --add-host host.docker.internal:host-gateway \
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

render_live() {
    local slug=$1
    local route=$2
    local token=$3
    local theme=$4
    local response_file="$repo_root/.cache/ui-render/$slug-$theme.html"
    local render_file="$repo_root/.cache/ui-render/$slug-$theme-render.html"
    local output_file="$live_output_dir/$slug-$theme.png"
    local curl_args=(--fail --silent --show-error --output "$response_file")
    local asset_base=$base_url

    if [[ $token != - ]]; then
        curl_args+=(--cookie "lake_session=$token")
    fi
    if [[ -z $browser ]]; then
        asset_base=${asset_base/127.0.0.1/host.docker.internal}
        asset_base=${asset_base/localhost/host.docker.internal}
    fi
    curl "${curl_args[@]}" "$base_url$route"
    sed \
        -e "s|href=\"/style.css\"|href=\"$asset_base/style.css\"|" \
        -e "s|<html|<html data-theme=\"$theme\"|" \
        "$response_file" >"$render_file"
    temporary_files+=("$response_file" "$render_file")

    if [[ -n $browser ]]; then
        render_with_local_browser "$render_file" "$output_file"
    else
        render_with_docker "$render_file" "$output_file"
    fi
    printf 'rendered %s\n' "${output_file#"$repo_root/"}"
}

if [[ -n $live_manifest || -n $base_url || -n $live_output_dir ]]; then
    if [[ -z $live_manifest || -z $base_url || -z $live_output_dir ]]; then
        printf '%s\n' '--live-manifest, --base-url, and --output-dir must be used together' >&2
        exit 2
    fi
    if [[ $live_output_dir != /* ]]; then
        live_output_dir="$repo_root/$live_output_dir"
    fi
    mkdir -p "$repo_root/.cache/ui-render" "$live_output_dir"
    while IFS=$'\t' read -r slug token route; do
        if [[ -z $slug || $slug == \#* ]]; then
            continue
        fi
        render_live "$slug" "$route" "$token" light
        render_live "$slug" "$route" "$token" dark
    done <"$live_manifest"
    exit 0
fi

for screen in repository change organization; do
    render quiet "$screen" light
    render quiet "$screen" dark
    render orama "$screen" light
    render editorial "$screen" light
    render editorial "$screen" dark
done
