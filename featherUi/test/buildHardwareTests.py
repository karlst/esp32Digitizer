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

"""Build only the opt-in ADC diagnostic entry point; never upload or open USB.

The normal source filter selects the reusable ADC class; this adds the test's
setup/loop outside src, keeping the ordinary web/DAC application out of the build.
"""
Import("env")
from pathlib import Path

projectRoot = Path(env.subst('$PROJECT_DIR'))
frameworkRoot = Path(env.PioPlatform().get_package_dir('framework-arduinoespressif32'))
env.Append(CPPPATH=[str(projectRoot / 'src' / 'adc'),
                   str(projectRoot.parent / 'shared'),
                   str(frameworkRoot / 'libraries' / 'SPI' / 'src')])
selectedSource = env.GetProjectOption('custom_test_source', 'ads127SpiDiagnostic.cpp')
if selectedSource not in ['ads127SpiDiagnostic.cpp', 'ads127CaptureControl.cpp', 'benchDacLoopback.cpp']:
    raise ValueError('Unknown Feather hardware test: ' + selectedSource)
env.BuildSources('$BUILD_DIR/hardwareTests', str(projectRoot / 'test' / 'hardware'),
                 src_filter=['-<*>', '+<' + selectedSource + '>', '+<benchDac.cpp>', '+<inputSnapshot.cpp>'])
