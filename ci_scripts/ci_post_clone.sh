#!/bin/sh
# Xcode Cloud: prepares a fresh clone. The disassembly is a submodule; the ROM
# is assembled from it during the build (scripts/build_rom.sh, which also
# compiles the asar assembler with CMake).
set -eu
cd "$CI_PRIMARY_REPOSITORY_PATH"
git submodule update --init --recursive
brew install cmake
./scripts/build_rom.sh
