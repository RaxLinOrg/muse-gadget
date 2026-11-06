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

# As with muse_clock: nothing in the firmware calls in here, so a plain
# archive would lose the MUSE_PAGE constructor to the linker.
if(CONFIG_MUSE_RADIO_ENABLED)
    set(srcs "muse_radio.c" "muse_radio_page.c")
else()
    set(srcs "")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    WHOLE_ARCHIVE
    REQUIRES muse
    PRIV_REQUIRES esp_http_client esp-tls mbedtls esp_netif lwip esp_timer esp_psram minimp3
)
