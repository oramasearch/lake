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

mkdir -p "$screenshots_dir" "$repo_root/.cache/ui-render"

temporary_files=()
failures_file=""

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

run_with_timeout() {
    local timeout_seconds=$1
    shift

    "$@" &
    local command_pid=$!
    local elapsed_seconds=0

    while kill -0 "$command_pid" 2>/dev/null; do
        if ((elapsed_seconds >= timeout_seconds)); then
            kill -TERM "$command_pid" 2>/dev/null || true
            sleep 1
            if kill -0 "$command_pid" 2>/dev/null; then
                kill -KILL "$command_pid" 2>/dev/null || true
            fi
            wait "$command_pid" 2>/dev/null || true
            return 124
        fi
        sleep 1
        ((elapsed_seconds += 1))
    done

    wait "$command_pid"
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

prepare_live() {
    local slug=$1
    local route=$2
    local token=$3
    local theme=$4
    local response_file="$repo_root/.cache/ui-render/$slug-$theme.html"
    local render_file="$repo_root/.cache/ui-render/$slug-$theme-render.html"
    local output_file="$live_output_dir/$slug-$theme.png"
    local network_log="$repo_root/.cache/ui-render/$slug-$theme-network.json"
    local curl_args=(--fail --silent --show-error --output "$response_file")
    local asset_base=$base_url

    if [[ $token != - ]]; then
        curl_args+=(--cookie "lake_session=$token")
    fi
    asset_base=${asset_base/127.0.0.1/host.docker.internal}
    asset_base=${asset_base/localhost/host.docker.internal}
    curl "${curl_args[@]}" "$base_url$route"
    sed \
        -e "s|href=\"/style.css\"|href=\"$asset_base/style.css\"|" \
        -e "s|<html|<html data-theme=\"$theme\"|" \
        "$response_file" >"$render_file"
    temporary_files+=("$response_file" "$render_file")

    printf '%s\t%s\t%s\t%s\n' "$slug-$theme" "$render_file" "$output_file" "$network_log"
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
    failures_file="$live_output_dir/failures.tsv"
    jobs_file="$repo_root/.cache/ui-render/jobs.tsv"
    driver_failures_file="$repo_root/.cache/ui-render/failures.tsv"
    : >"$failures_file"
    while IFS=$'\t' read -r slug token route; do
        if [[ -z $slug || $slug == \#* ]]; then
            continue
        fi
        prepare_live "$slug" "$route" "$token" light
        prepare_live "$slug" "$route" "$token" dark
    done <"$live_manifest" >"$jobs_file"
    capture_count=$(wc -l <"$jobs_file")
    capture_timeout=$((capture_count * 35 + 30))
    if ! run_with_timeout "$capture_timeout" docker run --rm \
        --platform linux/amd64 \
        --volume "$repo_root:/work" \
        --workdir /work \
        "$docker_image" \
        node /work/tools/ui/screenshot_driver.mjs \
        --jobs ".cache/ui-render/jobs.tsv" ".cache/ui-render/failures.tsv"; then
        if [[ -s $driver_failures_file ]]; then
            cat "$driver_failures_file" >>"$failures_file"
        else
            printf '%s\n' 'screenshot driver exited before reporting page failures' >>"$failures_file"
        fi
    fi
    while IFS=$'\t' read -r page_name render_file output_file network_log; do
        printf 'rendered %s\n' "${output_file#"$repo_root/"}"
    done <"$jobs_file"
    if [[ -s $failures_file ]]; then
        printf 'screenshot failures:\n' >&2
        cat "$failures_file" >&2
        exit 1
    fi
    rm -f "$failures_file"
    exit 0
fi

for screen in repository change organization; do
    render quiet "$screen" light
    render quiet "$screen" dark
    render orama "$screen" light
    render editorial "$screen" light
    render editorial "$screen" dark
done
