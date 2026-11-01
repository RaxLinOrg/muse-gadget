#!/usr/bin/env bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Regenerates the big clock face. The checked-in fonts/muse_font_clock_88.c is
# what this produces, so you only need this to change the size or the weight.
#
# Eleven glyphs, 0-9 and the colon: a full Montserrat at this size would be
# most of a megabyte, and the page never shows a letter in it.

set -eu

SIZE=${1:-88}
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HERE/fonts/muse_font_clock_$SIZE.c"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Montserrat, the face the rest of the UI uses, from Google's own repository.
curl -fsSL -o "$TMP/Montserrat[wght].ttf" \
    "https://github.com/google/fonts/raw/main/ofl/montserrat/Montserrat%5Bwght%5D.ttf"

npx -y lv_font_conv@1.5.3 --font "$TMP/Montserrat[wght].ttf" --size "$SIZE" --bpp 4 \
    --format lvgl --lv-font-name "muse_font_clock_$SIZE" --no-compress \
    -r 0x30-0x3A -o "$TMP/font.c"

sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' -e "s|$TMP/||g" "$TMP/font.c" > "$OUT"
echo "wrote $OUT"
