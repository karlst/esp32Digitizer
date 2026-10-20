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

# Build PC test executables, using MSVC instead of the ESP32 cross-compiler:
# nativeChecks: actual S3 command/acquisition/link logic with a fake ADC and RTOS.
# driverChecks: actual ADS1256 driver with a simulated SPI controller.
# monitorChecks: actual Feather status parser/JSON with a fake Arduino interface.
# fastCaptureChecks: actual interrupt reader with simulated hardware registers.
# writerChecks: reusable ring-buffer ordering, wraparound, overflow and I/O faults.
# chokeChecks/chokeCommandChecks: timed generator, real ring and temporary commands.
# Include-path order selects test substitutes; firmware source is not rewritten.
# No upload or physical serial access occurs. A failed assertion stops the runner.
param([switch]$LegacyStatus)
$ErrorActionPreference = 'Stop'
$statusFlag = if ($LegacyStatus) { '/DS3_LEGACY_STATUS=1' } else { '' }
$projectRoot = Split-Path $PSScriptRoot -Parent
# Locate installed Visual C++ build tools instead of assuming a versioned path.
$vsWhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$installation = & $vsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Microsoft C++ build tools are required for these desktop tests.' }
# Keep generated batch/object/executable files under ignored .pio, out of source.
$buildDir = Join-Path $projectRoot '.pio/nativeChecks'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$environmentScript = Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat'
# Run each suite only after its compilation and the previous suite succeeded.
# vcvars64 supplies the compiler environment to the generated command script.
$batch = @"
@echo off
call "$environmentScript" >nul
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 $statusFlag /I"$projectRoot/test/fakes" /I"$projectRoot/src" /I"$projectRoot/src/backend" /I"$projectRoot/test/hardware" /I"$projectRoot/../shared" /I"$projectRoot/../shared/bufferedWriter/src" "$projectRoot/test/nativeChecks.cpp" "$projectRoot/src/commandProtocol.cpp" "$projectRoot/src/commandHistory.cpp" "$projectRoot/src/acquisition.cpp" "$projectRoot/src/fastCapture.cpp" "$projectRoot/src/featherLink.cpp" "$projectRoot/test/fakeRecordingService.cpp" "$projectRoot/src/sampleFormatter.cpp" "$projectRoot/../shared/bufferedWriter/src/bufferedWriter.cpp" /Fe:nativeChecks.exe
if %errorlevel% neq 0 exit /b 1
nativeChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /I"$projectRoot/test/fakes" /I"$projectRoot/src" /I"$projectRoot/src/backend" /I"$projectRoot/test/hardware" /I"$projectRoot/../shared" /I"$projectRoot/../shared/bufferedWriter/src" "$projectRoot/test/driverChecks.cpp" "$projectRoot/src/ads1256.cpp" "$projectRoot/src/commandProtocol.cpp" /Fe:driverChecks.exe
if %errorlevel% neq 0 exit /b 1
driverChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /I"$projectRoot/test/fakes" /I"$projectRoot/src" /I"$projectRoot/src/backend" /I"$projectRoot/test/hardware" /I"$projectRoot/../shared" /I"$projectRoot/../shared/bufferedWriter/src" "$projectRoot/test/fastCaptureChecks.cpp" "$projectRoot/src/fastCapture.cpp" "$projectRoot/src/sampleFormatter.cpp" "$projectRoot/../shared/bufferedWriter/src/bufferedWriter.cpp" /Fe:fastCaptureChecks.exe
if %errorlevel% neq 0 exit /b 1
fastCaptureChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /I"$projectRoot/../featherUi/test/fakes" /I"$projectRoot/../featherUi/src" "$projectRoot/../featherUi/test/monitorChecks.cpp" "$projectRoot/../featherUi/src/s3Monitor.cpp" "$projectRoot/../featherUi/src/recordingMonitor.cpp" /Fe:monitorChecks.exe
if %errorlevel% neq 0 exit /b 1
monitorChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /I"$projectRoot/src" /I"$projectRoot/src/backend" /I"$projectRoot/test/hardware" /I"$projectRoot/../shared/bufferedWriter/src" /I"$projectRoot/../shared/bufferedWriter/test" "$projectRoot/test/writerChecks.cpp" "$projectRoot/../shared/bufferedWriter/test/bufferedWriterChecks.cpp" "$projectRoot/src/sampleFormatter.cpp" "$projectRoot/../shared/bufferedWriter/src/bufferedWriter.cpp" /Fe:writerChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /DBUFFERED_WRITER_EXTERNAL=1 /DBUFFERED_WRITER_CAPACITY=4194304 /DBUFFERED_WRITER_BLOCK_BYTES=131072 /I"$projectRoot/src" /I"$projectRoot/src/backend" /I"$projectRoot/test/hardware" /I"$projectRoot/../shared/bufferedWriter/src" /I"$projectRoot/../shared/bufferedWriter/test" "$projectRoot/test/writerChecks.cpp" "$projectRoot/../shared/bufferedWriter/test/bufferedWriterChecks.cpp" "$projectRoot/src/sampleFormatter.cpp" "$projectRoot/../shared/bufferedWriter/src/bufferedWriter.cpp" /Fe:externalWriterChecks.exe
if %errorlevel% neq 0 exit /b 1
externalWriterChecks.exe
if %errorlevel% neq 0 exit /b 1
writerChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /I"$projectRoot/src" /I"$projectRoot/src/backend" /I"$projectRoot/test/hardware" /I"$projectRoot/../shared/bufferedWriter/src" "$projectRoot/test/chokeChecks.cpp" "$projectRoot/test/hardware/chokeTest.cpp" "$projectRoot/../shared/bufferedWriter/src/bufferedWriter.cpp" /Fe:chokeChecks.exe
if %errorlevel% neq 0 exit /b 1
chokeChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /DS3_CHOKE_FIXED_RATE=1 /I"$projectRoot/src" /I"$projectRoot/src/backend" /I"$projectRoot/test/hardware" /I"$projectRoot/../shared/bufferedWriter/src" "$projectRoot/test/fixedChokeChecks.cpp" "$projectRoot/test/hardware/chokeTest.cpp" "$projectRoot/../shared/bufferedWriter/src/bufferedWriter.cpp" /Fe:fixedChokeChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /DS3_CHOKE_INITIAL_BPS=24000000 /I"$projectRoot/src" /I"$projectRoot/src/backend" /I"$projectRoot/test/hardware" /I"$projectRoot/../shared/bufferedWriter/src" "$projectRoot/test/highRateChokeChecks.cpp" "$projectRoot/test/hardware/chokeTest.cpp" "$projectRoot/../shared/bufferedWriter/src/bufferedWriter.cpp" /Fe:highRateChokeChecks.exe
if %errorlevel% neq 0 exit /b 1
highRateChokeChecks.exe
if %errorlevel% neq 0 exit /b 1
fixedChokeChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /DS3_CHOKE_FIXED_RATE=1 /DS3_CHOKE_INITIAL_BPS=26000000 /DS3_CHOKE_DURATION_SECONDS=2 /I"$projectRoot/src" /I"$projectRoot/src/backend" /I"$projectRoot/test/hardware" /I"$projectRoot/../shared/bufferedWriter/src" "$projectRoot/test/timedChokeChecks.cpp" "$projectRoot/test/hardware/chokeTest.cpp" "$projectRoot/../shared/bufferedWriter/src/bufferedWriter.cpp" /Fe:timedChokeChecks.exe
if %errorlevel% neq 0 exit /b 1
timedChokeChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /DS3_CHOKE_TEST=1 /I"$projectRoot/test/fakes" /I"$projectRoot/src" /I"$projectRoot/src/backend" /I"$projectRoot/test/hardware" /I"$projectRoot/../shared" /I"$projectRoot/../shared/bufferedWriter/src" "$projectRoot/test/nativeChecks.cpp" "$projectRoot/src/commandProtocol.cpp" "$projectRoot/src/commandHistory.cpp" "$projectRoot/src/acquisition.cpp" "$projectRoot/test/hardware/acquisitionChoke.cpp" "$projectRoot/test/hardware/chokeEvents.cpp" "$projectRoot/test/hardware/chokeTest.cpp" "$projectRoot/src/fastCapture.cpp" "$projectRoot/src/featherLink.cpp" "$projectRoot/test/fakeRecordingService.cpp" "$projectRoot/src/sampleFormatter.cpp" "$projectRoot/../shared/bufferedWriter/src/bufferedWriter.cpp" /Fe:chokeCommandChecks.exe
if %errorlevel% neq 0 exit /b 1
chokeCommandChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /I"$projectRoot/test/fakes" /I"$projectRoot/../shared" /I"$projectRoot/../featherUi/src/adc" "$projectRoot/test/ads127DriverChecks.cpp" "$projectRoot/../featherUi/src/adc/ads127l18.cpp" "$projectRoot/../featherUi/src/adc/ads127LowRate.cpp" /Fe:ads127DriverChecks.exe
if %errorlevel% neq 0 exit /b 1
ads127DriverChecks.exe
"@
$batchPath = Join-Path $buildDir 'run.cmd'
Set-Content -LiteralPath $batchPath -Value $batch -Encoding ascii
# Restore the caller's directory even if compilation or an assertion fails.
Push-Location $buildDir
try {
    & $env:ComSpec /c $batchPath
    if ($LASTEXITCODE -ne 0) { throw "Native checks failed with exit code $LASTEXITCODE" }
}
finally { Pop-Location }
