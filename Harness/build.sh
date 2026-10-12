#!/bin/bash
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/build"
clang -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -I"$ROOT/Core/include" \
  "$ROOT"/Core/src/*.c "$ROOT/Harness/main.c" -framework CoreGraphics -framework ImageIO -framework CoreFoundation \
  -o "$ROOT/build/harness"
