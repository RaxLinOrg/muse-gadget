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

# Nothing in the firmware calls into this component, so with a plain archive
# the linker would drop it and its MUSE_PAGE constructor with it. WHOLE_ARCHIVE
# is what makes the page register itself.
if(CONFIG_MUSE_CLOCK_ENABLED)
    set(srcs "muse_clock.c" "muse_weather.c" "fonts/muse_font_clock_88.c")
else()
    set(srcs "")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    WHOLE_ARCHIVE
    # muse re-exports lvgl as a public dependency, so lvgl.h comes with it.
    REQUIRES muse
    PRIV_REQUIRES esp_http_client esp-tls mbedtls esp_netif lwip esp_timer esp_psram nvs_flash
)
