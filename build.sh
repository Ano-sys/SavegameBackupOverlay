#!/usr/bin/env bash
set -euo pipefail

export DEVKITPRO=/opt/devkitpro
export DEVKITA64=$DEVKITPRO/devkitA64

cmake -S . -B build-switch \
    -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/Switch.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DGOOGLE_OAUTH_REQUIRED="${GOOGLE_OAUTH_REQUIRED:-OFF}" "$@"
cmake --build build-switch --parallel
