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

# PC-only waveform math check. Karl runs this; no upload or hardware access.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$vsWhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$installation = & $vsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Microsoft C++ build tools are required.' }
$buildDir = Join-Path $projectRoot '.pio/benchDacChecks'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$environmentScript = Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat'
# Keep compiler setup and compilation within the same command environment.
$batch = @"
@echo off
call "$environmentScript" >nul
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /I"$projectRoot/test/hardware" "$projectRoot/test/benchDacChecks.cpp" /Fe:benchDacChecks.exe
if %errorlevel% neq 0 exit /b 1
benchDacChecks.exe
exit /b %errorlevel%
"@
Set-Content -LiteralPath (Join-Path $buildDir 'run.cmd') -Value $batch -Encoding ascii
Push-Location $buildDir
try {
    & $env:ComSpec /c (Join-Path $buildDir 'run.cmd')
    if ($LASTEXITCODE -ne 0) { throw "DAC math checks failed: $LASTEXITCODE" }
}
finally { Pop-Location }
