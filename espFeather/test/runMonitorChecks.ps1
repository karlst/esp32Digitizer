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

# Compile the existing Feather status-parser checks against fake Arduino serial
# interfaces. No board, serial port, firmware build or upload is involved.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$vsWhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$installation = & $vsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Microsoft C++ build tools are required.' }
$buildDir = Join-Path $projectRoot '.pio/monitorChecks'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$environmentScript = Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat'

# Use the real parser and JSON formatter with the existing fake serial input.
# The batch stops at a compiler failure rather than running an older executable.
$batch = @"
@echo off
call "$environmentScript" >nul
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /I"$projectRoot/test/fakes" /I"$projectRoot/webApp/include" "$projectRoot/test/monitorChecks.cpp" "$projectRoot/webApp/src/S3Monitor.cpp" "$projectRoot/webApp/src/RecordingMonitor.cpp" /Fe:monitorChecks.exe
if %errorlevel% neq 0 exit /b 1
monitorChecks.exe
exit /b %errorlevel%
"@
$batchPath = Join-Path $buildDir 'run.cmd'
Set-Content -LiteralPath $batchPath -Value $batch -Encoding ascii

# Restore the caller's working directory even when a check fails.
Push-Location $buildDir
try {
    & $env:ComSpec /c $batchPath
    if ($LASTEXITCODE -ne 0) { throw "Feather monitor checks failed: $LASTEXITCODE" }
}
finally { Pop-Location }
