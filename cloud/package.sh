#!/bin/bash
set -euo pipefail

cloud_dir=$(cd -- "$(dirname -- "$0")" && pwd)
output=${1:-"$cloud_dir/function.zip"}
output_dir=$(cd -- "$(dirname -- "$output")" && pwd)
output="$output_dir/$(basename -- "$output")"
build_dir=$(mktemp -d "$cloud_dir/.lambda-build.XXXXXXXX")
trap 'rm -rf -- "$build_dir"' EXIT

export UV_CACHE_DIR=${UV_CACHE_DIR:-"$cloud_dir/.uv-cache"}
export UV_PYTHON_INSTALL_DIR=${UV_PYTHON_INSTALL_DIR:-"$cloud_dir/.python"}
uv pip install --target "$build_dir/stage" --python-version 3.12 \
    --python-platform aarch64-manylinux_2_28 --only-binary :all: \
    --requirement "$cloud_dir/requirements.txt"

cp -- "$cloud_dir/handler.py" "$cloud_dir/voice_support.py" "$build_dir/stage/"
(
    cd -- "$build_dir/stage"
    zip -q -r "$build_dir/function.zip" . -x '*/__pycache__/*' '*.pyc'
)
mv -- "$build_dir/function.zip" "$output"
printf 'Built %s\n' "$output"
