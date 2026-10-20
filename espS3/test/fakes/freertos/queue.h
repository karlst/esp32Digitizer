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
 * @file queue.h
 * @brief Supply queue creation/send/receive names to desktop tests.
 * Real firmware uses the installed FreeRTOS header; this wrapper selects our
 * single-thread declarations in FreeRTOS.h without changing production includes.
 */
#pragma once
#include "FreeRTOS.h"
