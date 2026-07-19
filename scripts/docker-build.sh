#!/usr/bin/env bash
# Builds and tests inside the Docker container. Usage:
#   scripts/docker-build.sh              # configure + build (VST3 + tests)
#   scripts/docker-build.sh test         # build + run unit tests
#   scripts/docker-build.sh <cmd...>     # run an arbitrary command in /work
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=autotrim-build
docker build -q -t "$IMAGE" docker >/dev/null

run() {
    docker run --rm \
        --user "$(id -u):$(id -g)" \
        -e HOME=/tmp \
        -v "$PWD":/work \
        -w /work \
        "$IMAGE" "$@"
}

case "${1:-build}" in
    build)
        run cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
        run cmake --build build
        ;;
    test)
        run cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
        run cmake --build build --target dsp_tests
        run ./build/dsp_tests
        ;;
    *)
        run "$@"
        ;;
esac
