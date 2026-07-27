#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/reading_speed_store"
BINARY="$BUILD_DIR/ReadingSpeedStoreTest"

mkdir -p "$BUILD_DIR"

SOURCES=(
  "$ROOT_DIR/test/reading_speed_store/ReadingSpeedStoreTest.cpp"
  "$ROOT_DIR/src/util/ReadingSpeedStore.cpp"
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
