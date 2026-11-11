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

PlatformIO executes this as a pre-build SCons script, not a standalone Python
program. Import supplies its build environment. For the external-generator
timing tests, ads127CaptureControl.cpp is the selected entry point and the real
AcquisitionApplication is linked below; build flags disable DAC generation.
"""
Import("env")
from pathlib import Path

projectRoot = Path(env.subst('$PROJECT_DIR'))
frameworkRoot = Path(env.PioPlatform().get_package_dir('framework-arduinoespressif32'))
# Sources added by this pre-build script need explicit header paths, including
# the common application headers directly inside include.
env.Append(CPPPATH=[str(projectRoot / 'include'),
                   str(projectRoot / 'include' / 'adc'),
                   str(projectRoot / 'test' / 'hardware'),
                   str(projectRoot.parent / 'common'),
                   str(frameworkRoot / 'libraries' / 'SPI' / 'src')])
selectedSource = env.GetProjectOption('custom_test_source', 'ads127SpiDiagnostic.cpp')
if selectedSource not in ['ads127SpiDiagnostic.cpp', 'ads127CaptureControl.cpp',
                          'benchDacLoopback.cpp', 'ads127ScopeControl.cpp']:
    raise ValueError('Unknown Feather hardware test: ' + selectedSource)
# The scope-only test needs just its entry point and the reusable ADC driver.
# It neither generates analog input nor runs the P4 recording handshake.
sourceFilter = ['-<*>', '+<' + selectedSource + '>']
if selectedSource != 'ads127ScopeControl.cpp':
    sourceFilter += ['+<BenchDac.cpp>', '+<InputSnapshot.cpp>', '+<BenchAcquisitionServices.cpp>']
env.BuildSources('$BUILD_DIR/hardwareTests', str(projectRoot / 'test' / 'hardware'),
                 src_filter=sourceFilter)

# Compile the real controller only for capture environments. Other diagnostics
# keep their own entry points and do not need its handshake state machine.
if selectedSource == 'ads127CaptureControl.cpp':
    env.BuildSources('$BUILD_DIR/acquisition', str(projectRoot / 'src'),
                     src_filter=['-<*>', '+<AcquisitionApplication.cpp>'])
