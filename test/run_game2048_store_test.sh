#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/game2048_store"
BINARY="$BUILD_DIR/Game2048StoreTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/game2048_store/Game2048StoreTest.cpp"
  "$ROOT_DIR/src/util/Game2048Store.cpp"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR"
  -I"$ROOT_DIR/src"
)

c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"

"$BINARY" "$@"
