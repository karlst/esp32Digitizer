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

# Standalone ESP-IDF receiver with an Arduino-independent reusable SD writer.
cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET esp32p4)
include("${CMAKE_CURRENT_LIST_DIR}/cmake/exfat.cmake")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espP4)

# Catch component-precedence mistakes before compiling incompatible file layouts.
idf_component_get_property(p4SelectedFatfs fatfs COMPONENT_DIR)
if(NOT "${p4SelectedFatfs}" STREQUAL "${p4FatfsComponent}")
    message(FATAL_ERROR "P4 must use its generated exFAT-enabled FatFs component.")
endif()
