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

#pragma once

#include "muse_state.h"

/*
 * The noise each pet button makes: a crunch, a few bars of music, a spoken
 * "thank you". Three MP3s embedded in the image, decoded and played through
 * the same speaker a reply uses.
 *
 * The clips are 16 kHz mono, which is MUSE_AUDIO_RATE, so nothing resamples.
 * Re-encode at any other rate and the clip is refused rather than played at
 * the wrong pitch.
 *
 * Credits and licence are in pet_sounds.md beside this file.
 */

/* Ask for the clip that goes with a reaction; returns at once. A reaction with
 * no clip, or a request while one is already playing, is ignored. */
void muse_pet_sound_play(muse_reaction_t r);

/* Plays whatever was asked for, blocking until it is done. Called from the
 * voice task, which is the one task allowed to write to the speaker. */
void muse_pet_sound_pump(void);

/* True while a clip is waiting or playing: the voice task must stay awake. */
bool muse_pet_sound_busy(void);
