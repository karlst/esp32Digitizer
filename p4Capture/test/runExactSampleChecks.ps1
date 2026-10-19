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
cl /nologo /std:c++17 /O2 /EHsc /W4 /I"$projectRoot/main" "$projectRoot/test/exactSampleChecks.cpp" "$projectRoot/main/frameDecoder.cpp" /Fe:exactSampleChecks.exe
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
