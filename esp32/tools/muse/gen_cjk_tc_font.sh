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

# Regenerates the board's CJK face from Noto Sans TC Medium at 16 px, covering
# every hanzi in Big5 (both levels, 13063 of them), kana, the fullwidth forms,
# and the CJK punctuation that appears in prose: fonts/muse_font_cjk_tc_16.c
# (level 1 and the rest, 4 bpp) and fonts/muse_font_cjk_tc2_16.c (level 2,
# 2 bpp).
#
# TC, not the SC that gen_cjk_noto_font.sh builds: Noto Sans SC over GB2312 is
# simplified, so 廣 聲 樂 灣 電 -- all of them in a station name on this board --
# are simply not in it.
#
# It is TWO files. LVGL keeps a glyph's bitmap offset in a 20-bit bitfield, so
# one font's glyph data cannot pass 1 MB, and all of Big5 at 16 px 4 bpp is
# 1.05 MB -- which the compiler catches as an overflow, but only after a full
# build. 3 bpp would fit in one file; LVGL only accepts it compressed, which
# this firmware does not build. So level 1 and level 2 are separate fonts and
# muse_ui.c chains the second behind the first.
#
# Two more things here are deliberate and easy to undo by accident:
#
#  - The U+3000..303F block is NOT taken whole. Most of it is vertical-form
#    brackets whose glyph boxes are far taller than the type, and lv_font_conv
#    sets line_height from the tallest box in the subset: taking the block
#    raises it from 20 to 31 px, which pushes every CJK caption in the
#    firmware around. Only the punctuation Chinese prose actually uses is
#    listed below.
#  - Big5 level 2 is included even though nothing measured so far needs it
#    (56 real podcast titles: not one character outside level 1). It costs
#    0.54 MB of the 1.26 MB, and it is what keeps this a drop-in replacement
#    for Unifont rather than a coverage regression -- Muse's replies are not
#    limited to common characters the way a podcast title is.
#
# Needs curl, python3 and npx.
set -eu

HERE="$(cd "$(dirname "$0")/../.." && pwd)"
OUT1="$HERE/components/muse/fonts/muse_font_cjk_tc_16.c"
OUT2="$HERE/components/muse/fonts/muse_font_cjk_tc2_16.c"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

curl -fsSL -o "$TMP/noto.otf" \
    "https://github.com/notofonts/noto-cjk/raw/main/Sans/SubsetOTF/TC/NotoSansTC-Medium.otf"

# Big5's two levels, read out of the codec rather than written down by hand,
# plus the punctuation and kana that go in the first file beside level 1.
python3 - "$TMP" <<'PYEOF'
import sys
lo_set, hi_set = set(), set()
for hi in range(0xA1, 0xFA):
    for lo in list(range(0x40, 0x7F)) + list(range(0xA1, 0xFF)):
        try:
            c = ord(bytes([hi, lo]).decode("big5"))
        except UnicodeDecodeError:
            continue
        if 0x3400 <= c <= 0x9FFF or 0xF900 <= c <= 0xFAFF:
            (lo_set if hi <= 0xC6 else hi_set).add(c)
# The CJK punctuation prose uses: ideographic space and full stop, the
# enumeration comma, the repeat mark, the bracket and quotation pairs. NOT the
# rest of U+3000..303F -- see the note at the top of this script.
lo_set |= {0x3000, 0x3001, 0x3002, 0x3003, 0x3005, 0x3008, 0x3009, 0x300A, 0x300B,
           0x300C, 0x300D, 0x300E, 0x300F, 0x3010, 0x3011, 0x301C, 0x301D, 0x301E}
lo_set |= set(range(0x3040, 0x3100))    # hiragana and katakana
lo_set |= set(range(0xFF01, 0xFF66))    # fullwidth forms
lo_set |= {0x00B0, 0x00B7, 0x00D7, 0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D,
           0x2026, 0x2027, 0x2030, 0x2032, 0x2033}

def ranges(cps):
    out, lo = [], None
    prev = None
    for c in sorted(cps):
        if prev is not None and c == prev + 1:
            prev = c
            continue
        if lo is not None:
            out.append(hex(lo) if lo == prev else "%s-%s" % (hex(lo), hex(prev)))
        lo = prev = c
    out.append(hex(lo) if lo == prev else "%s-%s" % (hex(lo), hex(prev)))
    return ",".join(out)

open(sys.argv[1] + "/r1.txt", "w").write(ranges(lo_set))
open(sys.argv[1] + "/r2.txt", "w").write(ranges(hi_set))
PYEOF

# ASCII rides in the first file so a line mixing Latin and Chinese can be set
# in one face; muse_page_text_font() is what asks for that.
npx -y lv_font_conv@1.5.3 --font "$TMP/noto.otf" --size 16 --bpp 4 \
    --format lvgl --lv-font-name muse_font_cjk_tc_16 --no-compress \
    -r 0x20-0x7F -r "$(cat "$TMP/r1.txt")" -o "$TMP/f1.c"
# Level 2 at 2 bpp, not 4. Four grey levels instead of sixteen on characters
# that are rare by definition -- 56 real podcast titles contained not one of
# them -- and the difference is 0.45 MB, which is what decides whether all of
# Big5 fits in the app slot beside everything else. 3 bpp would be the better
# compromise; LVGL only accepts it compressed, which this firmware does not
# build.
npx -y lv_font_conv@1.5.3 --font "$TMP/noto.otf" --size 16 --bpp 2 \
    --format lvgl --lv-font-name muse_font_cjk_tc2_16 --no-compress \
    -r "$(cat "$TMP/r2.txt")" -o "$TMP/f2.c"
sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' -e "s|$TMP/||g" "$TMP/f1.c" > "$OUT1"
sed -e 's|#include "lvgl/lvgl.h"|#include "lvgl.h"|' -e "s|$TMP/||g" "$TMP/f2.c" > "$OUT2"

# line_height is what the rest of the UI is laid out against, and 20 is what
# the first file's subset gives; a jump means the U+3000 note above was
# ignored. The second file's own line_height does not matter -- LVGL lays the
# line out with the primary font and places a fallback glyph by its ofs_y
# against the shared baseline -- so it is not checked.
grep -q '\.line_height = 20' "$OUT1" || echo "WARNING: $OUT1 line_height moved; check the subset"
echo "wrote $OUT1"
echo "wrote $OUT2"
