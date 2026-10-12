#!/bin/bash
# Assembles the SMWDisX disassembly into a ROM image + WLA symbol file.
# Output: Generated/smw.sfc, Generated/smw.sym
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ASAR="$ROOT/tools/asar-build/asar/bin/asar"
VERSION="${SMW_VERSION:-1}"   # 0=J 1=U 2=SS 3=E0 4=E1

if [ ! -x "$ASAR" ]; then
  echo "Building asar..."
  [ -d "$ROOT/tools/asar-src" ] || git clone --depth 1 --branch v1.91 https://github.com/RPGHacker/asar.git "$ROOT/tools/asar-src"
  cmake -S "$ROOT/tools/asar-src/src" -B "$ROOT/tools/asar-build" -DCMAKE_BUILD_TYPE=Release -DASAR_GEN_LIB=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 >/dev/null
  cmake --build "$ROOT/tools/asar-build" -j 8 --target asar-standalone >/dev/null
fi

mkdir -p "$ROOT/Generated"
OUT="$ROOT/Generated/smw.sfc"
SYM="$ROOT/Generated/smw.sym"
# Only rebuild when the disassembly changed.
if [ -f "$OUT" ] && [ -z "$(find "$ROOT/SMWDisX" -newer "$OUT" -type f -print -quit)" ]; then
  exit 0
fi
cd "$ROOT/SMWDisX"
"$ASAR" -D_VER="$VERSION" --symbols=wla --symbols-path="$SYM" smw.asm "$OUT"
python3 "$ROOT/scripts/gen_symbols.py" "$SYM" "$ROOT/Core/include/smw_symbols.h"
echo "ROM: $OUT ($(shasum "$OUT" | cut -d' ' -f1))"
