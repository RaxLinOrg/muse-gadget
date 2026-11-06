#!/bin/sh
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

# Regenerates components/muse/fonts/muse_font_page_cjk_{16,24}.c: Noto Sans TC
# Medium, 4 bpp, holding ONLY the characters the compiled-in page names use.
# That subset is what keeps it to about 25 KB together -- every common
# traditional character at 24 px is 0.86 MB, which the 4 MB app slot has no
# room for beside Unifont.
#
# So: when you add a station to muse_radio.c, or a show, add its characters to
# NAMES below and run this again. A character that is missing does not break
# the build -- it falls back to the 16 px caption font and is drawn small,
# next to 24 px neighbours, which is easy to miss in review and obvious on the
# panel.
#
# Needs curl, python3 and npx.
set -eu

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# Every name and caption the pages draw from their own tables. Traditional
# Chinese: Noto Sans *TC*, not the SC the caption fallback offers, which covers
# GB2312 and so has none of 廣 聲 樂 灣 電.
NAMES='ICRT 英語・多元音樂 中廣流行網 流行音樂・談話 中廣音樂網 音樂・古典
飛碟聯播網 流行・談話節目 寶島新聲 FM98.5 台語・華語流行 好事聯播網 港都983 高雄・在地音樂
All-In 跳脫 Do 式圈 博音 游庭皓的財經皓角 Gooaye 股癌 台灣通勤第一品牌 法客電台
分 時 集 新 最 播放 暫停 停止 連線中 緩衝 失敗 無 節目 電台 第 回 上 下'

curl -fsSL -o "$TMP/noto.otf" \
    "https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/TC/NotoSansTC-Medium.otf"

RANGE=$(NAMES="$NAMES" python3 -c '
import os
cps = sorted({ord(c) for c in os.environ["NAMES"] if ord(c) > 0x2000})
print(",".join(hex(c) for c in cps))')

for size in 16 24; do
    out="$HERE/components/muse/fonts/muse_font_page_cjk_$size.c"
    npx -y lv_font_conv@1.5.3 --font "$TMP/noto.otf" --size "$size" --bpp 4 \
        --format lvgl --lv-font-name "muse_font_page_cjk_$size" --no-compress \
        -r 0x20-0x7F -r "$RANGE" -o "$TMP/font.c"
    sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' -e "s|$TMP/||g" "$TMP/font.c" > "$out"
    echo "wrote $out"
done
