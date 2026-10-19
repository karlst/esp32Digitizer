/*
 * Copyright (C) 2026 Karl Stock
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of ESP32 Digitizer.
 * You may redistribute and/or modify it under the GNU General Public License
 * as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.
 */

/**
 * @file version.hpp
 * @brief Shared build-identification string printed by Feather at startup.
 * This human-facing version is separate from the serial message format version
 * and the web asset cache suffix. Changing one does not automatically change others.
 */
#pragma once

#define ESP32_DIGITIZER_VERSION "0.1.0"