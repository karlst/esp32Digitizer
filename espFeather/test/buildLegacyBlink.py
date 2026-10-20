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

"""Link the legacy LED test used by the normal web application's blink routes.

PlatformIO calls this during a web build. Diagnostic environments replace this
script with buildHardwareTests.py and do not link the web LED service.
"""
Import("env")
from pathlib import Path

env.BuildSources('$BUILD_DIR/legacyBlink',
                 str(Path(env.subst('$PROJECT_DIR')) / 'test' / 'hardware'),
                 src_filter=['-<*>', '+<ledBlinker.cpp>'])
