#!/usr/bin/env bash
# Обновить копии файлов BreezeCAD: sdk/bz_module.h и vendor/platform/Paths.*
#   scripts/update-sdk.sh            — из ветки main
#   scripts/update-sdk.sh v0.2.6     — из выпуска (тега)
# После обновления проверьте BZ_MODULE_ABI_VERSION и соберите с тестами.
set -euo pipefail
ref="${1:-main}"
cd "$(dirname "$0")/.."
raw="https://raw.githubusercontent.com/BreezeCAD/breezecad/${ref}"
curl -fsSL "$raw/src/modules/bz_module.h" -o sdk/bz_module.h
curl -fsSL "$raw/src/platform/Paths.h"    -o vendor/platform/Paths.h
curl -fsSL "$raw/src/platform/Paths.cpp"  -o vendor/platform/Paths.cpp
printf 'bz_module.h из BreezeCAD/breezecad, %s, %s\n' "$ref" "$(date +%Y-%m-%d)" > sdk/SOURCE
sed -i.bak "1s/.*/platform\/Paths.{h,cpp} из BreezeCAD\/breezecad (src\/platform), ${ref}, $(date +%Y-%m-%d) — без изменений./" vendor/SOURCE && rm -f vendor/SOURCE.bak
grep -m1 'BZ_MODULE_ABI_VERSION ' sdk/bz_module.h
