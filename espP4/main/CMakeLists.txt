# Copyright (C) 2026 Karl Stock
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of ESP32 Digitizer.
# You may redistribute and/or modify it under the GNU General Public License
# as published by the Free Software Foundation, either version 3 of the License,
# or (at your option) any later version.
#
# Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
# Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.

# Compile P4 capture and storage plus the shared byte writer. The reusable writer
# has no S3 dependency; these include paths expose its interface to sdRecorder.
# Select one small startup file. Application and storage implementations are shared.
set(startup "src/main.cpp")
if(CONFIG_P4_HARDWARE_SMOKE OR CONFIG_P4_CAPTURE_PROFILE OR CONFIG_P4_DIAGNOSTICS)
    set(startup "../test/hardware/main.cpp")
endif()
idf_component_register(SRCS ${startup} "src/RecordingApplication.cpp" "src/FrameDecoder.cpp" "src/SdRecorder.cpp"
    "../../shared/bufferedWriter/src/BufferedWriter.cpp"
    INCLUDE_DIRS "include" "../../shared" "../../shared/bufferedWriter/src"
    REQUIRES esp_driver_parlio esp_driver_uart esp_driver_gpio esp_timer fatfs sdmmc esp_driver_sdmmc esp_psram esp_rom esp_hw_support)
# Allocate the ring separately at runtime: 4 MiB total, writes of up to 128 KiB.
# Apply identical definitions to all sources so class layouts agree.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BUFFERED_WRITER_EXTERNAL=1 BUFFERED_WRITER_CAPACITY=4194304 BUFFERED_WRITER_BLOCK_BYTES=131072)

# Test implementations are absent from ordinary recording builds.
if(CONFIG_P4_HARDWARE_SMOKE OR CONFIG_P4_CAPTURE_PROFILE OR CONFIG_P4_DIAGNOSTICS)
    include("${CMAKE_CURRENT_LIST_DIR}/../test/firmware.cmake")
endif()
