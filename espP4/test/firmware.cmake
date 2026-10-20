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

# Add bench implementations to the existing receiver without copying its decoder
# or recorder. CMAKE_CURRENT_LIST_DIR is this test directory, not main/.
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/hardware/RecordingHarness.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/hardware/SdWriteTest.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/hardware/RamWaveform.cpp")
target_include_directories(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/hardware"
    "${CMAKE_CURRENT_LIST_DIR}/profile")

# Only the profiling build collects CPU use and selects the shorter duration.
if(CONFIG_P4_HARDWARE_SMOKE)
    target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/hardware/SdSmokeSummary.cpp")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ADS127_HARDWARE_SMOKE=1)
elseif(CONFIG_P4_CAPTURE_PROFILE)
    target_sources(${COMPONENT_LIB} PRIVATE "${CMAKE_CURRENT_LIST_DIR}/profile/CaptureProfile.cpp")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ADS127_P4_LONG_RUN=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ADS127_P4_ENDURANCE=1)
endif()
