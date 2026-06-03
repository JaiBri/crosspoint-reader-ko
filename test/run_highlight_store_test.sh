#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/highlight_store"
BINARY="$BUILD_DIR/HighlightStoreTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/highlight_store/HighlightStoreTest.cpp"
  "$ROOT_DIR/src/util/HighlightStore.cpp"
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
