#!/usr/bin/env bash
# Builds Reverse Sampler on macOS or Linux.
#   ./build.sh             build Release
#   ./build.sh --install   build, then copy the plug-ins to your user plug-in folders
set -euo pipefail
cd "$(dirname "$0")"

if ! command -v cmake >/dev/null 2>&1; then
    echo "CMake was not found. macOS: brew install cmake. Linux: install cmake with your package manager."
    exit 1
fi

jobs="$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)"

echo "Configuring. The first run downloads JUCE and can take a few minutes..."
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel "$jobs"

out="build/ReverseSampler_artefacts/Release"
echo
echo "Built:"
ls -d "$out"/*/* 2>/dev/null | sed 's/^/  /' || true

if [[ "${1:-}" == "--install" ]]; then
    if [[ "$(uname)" == "Darwin" ]]; then
        vst3_dir="$HOME/Library/Audio/Plug-Ins/VST3"
        au_dir="$HOME/Library/Audio/Plug-Ins/Components"
        mkdir -p "$vst3_dir" "$au_dir"
        rm -rf "$vst3_dir/Reverse Sampler.vst3" "$au_dir/Reverse Sampler.component"
        cp -R "$out/VST3/Reverse Sampler.vst3" "$vst3_dir/"
        cp -R "$out/AU/Reverse Sampler.component" "$au_dir/"
        echo "Installed to $vst3_dir and $au_dir"
    else
        vst3_dir="$HOME/.vst3"
        mkdir -p "$vst3_dir"
        rm -rf "$vst3_dir/Reverse Sampler.vst3"
        cp -R "$out/VST3/Reverse Sampler.vst3" "$vst3_dir/"
        echo "Installed to $vst3_dir"
    fi
fi