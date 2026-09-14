#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
idf_dir="${AI_PASSPORT_RECORDER_IDF_PATH:-${IDF_PATH:-}}"

if [[ -z "${idf_dir}" || ! -f "${idf_dir}/export.sh" ]]; then
    echo "Set AI_PASSPORT_RECORDER_IDF_PATH or source ESP-IDF before building." >&2
    exit 1
fi

source "${idf_dir}/export.sh"
build_dir="${project_dir}/build"
idf.py -C "${project_dir}" -B "${build_dir}" set-target esp32c3
idf.py -C "${project_dir}" -B "${build_dir}" build
idf.py -C "${project_dir}" -B "${build_dir}" merge-bin
cp "${project_dir}/build/merged-binary.bin" \
   "${project_dir}/build/ai-passport-recorder-complete.bin"
