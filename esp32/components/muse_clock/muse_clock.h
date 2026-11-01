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
 * A clock and three-day forecast on a swipe page of its own.
 *
 * Nothing in the firmware calls into this component: it registers its page
 * with MUSE_PAGE from a constructor and starts its own tasks from there, so
 * dropping the folder into components/ is the whole installation. See
 * README.md.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Whether the page is in the tileview at all (CONFIG_MUSE_CLOCK_ENABLED). */
bool muse_clock_enabled(void);

#ifdef __cplusplus
}
#endif
