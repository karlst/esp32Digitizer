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

# Karl runs this PC-only exact-value test. No boards, serial ports or SD access.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$vsWhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$installation = & $vsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Microsoft C++ build tools are required.' }
$buildDir = Join-Path $projectRoot '.pio/exactSampleChecks'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$environmentScript = Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat'
# Compile the real decoder, not a copied implementation, in the same environment.
$batch = @"
@echo off
call "$environmentScript" >nul
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /I"$projectRoot/main/include" /I"$projectRoot/test/hardware" /I"$projectRoot/test/profile" "$projectRoot/test/exactSampleChecks.cpp" "$projectRoot/main/src/FrameDecoder.cpp" /Fe:exactSampleChecks.exe
if %errorlevel% neq 0 exit /b 1
exactSampleChecks.exe
exit /b %errorlevel%
"@
Set-Content -LiteralPath (Join-Path $buildDir 'run.cmd') -Value $batch -Encoding ascii
Push-Location $buildDir
try {
    & $env:ComSpec /c (Join-Path $buildDir 'run.cmd')
    if ($LASTEXITCODE -ne 0) { throw "Exact sample checks failed: $LASTEXITCODE" }
}
finally { Pop-Location }
