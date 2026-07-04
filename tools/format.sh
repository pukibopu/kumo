#!/bin/sh
# Formats all tracked C++ sources in place.
cd "$(dirname "$0")/.." || exit 1
git ls-files '*.h' '*.cpp' '*.mm' | xargs clang-format -i
