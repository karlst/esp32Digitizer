# Compile and run actual control logic with fake hardware. No PlatformIO upload/serial actions.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$vsWhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$installation = & $vsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Microsoft C++ build tools are required for these desktop tests.' }
$buildDir = Join-Path $projectRoot '.pio/nativeChecks'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$environmentScript = Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat'
$batch = @"
@echo off
call "$environmentScript" >nul
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /I"$projectRoot/test/fakes" /I"$projectRoot/src" "$projectRoot/test/nativeChecks.cpp" "$projectRoot/src/commandProtocol.cpp" "$projectRoot/src/commandHistory.cpp" "$projectRoot/src/acquisition.cpp" "$projectRoot/src/featherLink.cpp" /Fe:nativeChecks.exe
if errorlevel 1 exit /b 1
nativeChecks.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /I"$projectRoot/test/fakes" /I"$projectRoot/src" "$projectRoot/test/driverChecks.cpp" "$projectRoot/src/ads1256.cpp" "$projectRoot/src/commandProtocol.cpp" /Fe:driverChecks.exe
if errorlevel 1 exit /b 1
driverChecks.exe
"@
$batchPath = Join-Path $buildDir 'run.cmd'
Set-Content -LiteralPath $batchPath -Value $batch -Encoding ascii
Push-Location $buildDir
try {
    & $env:ComSpec /c $batchPath
    if ($LASTEXITCODE -ne 0) { throw "Native checks failed with exit code $LASTEXITCODE" }
}
finally { Pop-Location }
