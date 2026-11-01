/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Sky icons as string art, one letter per pixel, so a 16x16 drawing is
 * readable in the source and costs 256 bytes of flash instead of a bitmap.
 * muse_clock.c scales each one up by an integer factor into an ARGB8888
 * image, which keeps the hard pixel edges the character art already has.
 *
 *   '.' nothing     'y' sun      'w' cloud, lit face
 *   'g' cloud, shaded side       'b' rain     's' snow
 *   'f' lightning
 */

#pragma once

#define MUSE_SKY_ART_W 16
#define MUSE_SKY_ART_H 16

#define MUSE_SKY_COLOR_SUN   0xffd24f
#define MUSE_SKY_COLOR_CLOUD 0xd8d2ff
#define MUSE_SKY_COLOR_SHADE 0x6f6a90
#define MUSE_SKY_COLOR_RAIN  0x7bb8ff
#define MUSE_SKY_COLOR_SNOW  0xf2efff
#define MUSE_SKY_COLOR_BOLT  0xffd24f

static const char *const MUSE_SKY_ART_CLEAR[MUSE_SKY_ART_H] = {
    "................",
    ".......yy.......",
    "...y...yy...y...",
    "....y..yy..y....",
    ".....yyyyyy.....",
    "....yyyyyyyy....",
    "...yyyyyyyyyy...",
    "yy.yyyyyyyyyy.yy",
    "yy.yyyyyyyyyy.yy",
    "...yyyyyyyyyy...",
    "....yyyyyyyy....",
    ".....yyyyyy.....",
    "....y..yy..y....",
    "...y...yy...y...",
    ".......yy.......",
    "................",
};

static const char *const MUSE_SKY_ART_PARTLY[MUSE_SKY_ART_H] = {
    "................",
    "..........yy....",
    "...y....yyyyyy..",
    "....y..yyyyyyyy.",
    ".....y.yyyyyyyy.",
    "yyy....yyyyyyyy.",
    ".....y.yyyyyyy..",
    "....y...yyyyy...",
    "...y............",
    "....wwww........",
    "..wwwwwwww......",
    ".wwwwwwwwwwww...",
    "wwwwwwwwwwwwww..",
    "gggggggggggggg..",
    "................",
    "................",
};

static const char *const MUSE_SKY_ART_CLOUD[MUSE_SKY_ART_H] = {
    "................",
    "................",
    ".....wwww.......",
    "...wwwwwwww.....",
    "..wwwwwwwwww....",
    ".wwwwwwwwwwww...",
    "..gggggggggg....",
    "....wwww....ww..",
    "..wwwwwwwwwwwww.",
    ".wwwwwwwwwwwwwww",
    "wwwwwwwwwwwwwwww",
    "gggggggggggggggg",
    "..gggggggggggg..",
    "................",
    "................",
    "................",
};

static const char *const MUSE_SKY_ART_FOG[MUSE_SKY_ART_H] = {
    "................",
    "................",
    "....wwwwww......",
    "..wwwwwwwwww....",
    ".wwwwwwwwwwww...",
    "wwwwwwwwwwwwww..",
    "gggggggggggggg..",
    "................",
    "..wwwwwwwwwww...",
    "................",
    ".wwwwwwwwwwwww..",
    "................",
    "..wwwwwwwwwww...",
    "................",
    ".wwwwwwwwwwwww..",
    "................",
};

static const char *const MUSE_SKY_ART_RAIN[MUSE_SKY_ART_H] = {
    "................",
    ".....wwww.......",
    "...wwwwwwww.....",
    "..wwwwwwwwww....",
    ".wwwwwwwwwwww...",
    "wwwwwwwwwwwwww..",
    "gggggggggggggg..",
    "..gggggggggg....",
    "................",
    "..b...b...b.....",
    "..b...b...b.....",
    ".b...b...b......",
    "................",
    "...b...b...b....",
    "...b...b...b....",
    "..b...b...b.....",
};

static const char *const MUSE_SKY_ART_SNOW[MUSE_SKY_ART_H] = {
    "................",
    ".....wwww.......",
    "...wwwwwwww.....",
    "..wwwwwwwwww....",
    ".wwwwwwwwwwww...",
    "wwwwwwwwwwwwww..",
    "gggggggggggggg..",
    "..gggggggggg....",
    "................",
    "..s.s...s.s.....",
    "...s.....s......",
    "..s.s...s.s.....",
    "................",
    "....s.s...s.s...",
    ".....s.....s....",
    "....s.s...s.s...",
};

static const char *const MUSE_SKY_ART_STORM[MUSE_SKY_ART_H] = {
    "................",
    ".....wwww.......",
    "...wwwwwwww.....",
    "..wwwwwwwwww....",
    ".wwwwwwwwwwww...",
    "wwwwwwwwwwwwww..",
    "gggggggggggggg..",
    "..gggggggggg....",
    "................",
    "......ffff......",
    ".....fff........",
    "....ffffff......",
    ".......fff......",
    "......ff........",
    ".....ff.........",
    "................",
};
