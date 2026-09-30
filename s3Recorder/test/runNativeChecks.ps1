# Build four PC executables, using MSVC instead of the ESP32 cross-compiler:
# nativeChecks: actual S3 command/acquisition/link logic with a fake ADC and RTOS.
# driverChecks: actual ADS1256 driver with a simulated SPI controller.
# monitorChecks: actual Feather status parser/JSON with a fake Arduino interface.
# fastCaptureChecks: actual interrupt reader with simulated hardware registers.
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
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 $statusFlag /I"$projectRoot/test/fakes" /I"$projectRoot/src" /I"$projectRoot/../shared" /I"$projectRoot/lib/bufferedWriter/src" "$projectRoot/test/nativeChecks.cpp" "$projectRoot/src/commandProtocol.cpp" "$projectRoot/src/commandHistory.cpp" "$projectRoot/src/acquisition.cpp" "$projectRoot/src/fastCapture.cpp" "$projectRoot/src/featherLink.cpp" "$projectRoot/test/fakeRecordingService.cpp" "$projectRoot/src/sampleFormatter.cpp" "$projectRoot/lib/bufferedWriter/src/bufferedWriter.cpp" /Fe:nativeChecks.exe
if errorlevel 1 exit /b 1
nativeChecks.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /I"$projectRoot/test/fakes" /I"$projectRoot/src" /I"$projectRoot/../shared" /I"$projectRoot/lib/bufferedWriter/src" "$projectRoot/test/driverChecks.cpp" "$projectRoot/src/ads1256.cpp" "$projectRoot/src/commandProtocol.cpp" /Fe:driverChecks.exe
if errorlevel 1 exit /b 1
driverChecks.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /I"$projectRoot/test/fakes" /I"$projectRoot/src" /I"$projectRoot/../shared" /I"$projectRoot/lib/bufferedWriter/src" "$projectRoot/test/fastCaptureChecks.cpp" "$projectRoot/src/fastCapture.cpp" "$projectRoot/src/sampleFormatter.cpp" "$projectRoot/lib/bufferedWriter/src/bufferedWriter.cpp" /Fe:fastCaptureChecks.exe
if errorlevel 1 exit /b 1
fastCaptureChecks.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /I"$projectRoot/../featherUi/test/fakes" /I"$projectRoot/../featherUi/src" "$projectRoot/../featherUi/test/monitorChecks.cpp" "$projectRoot/../featherUi/src/s3Monitor.cpp" "$projectRoot/../featherUi/src/recordingMonitor.cpp" /Fe:monitorChecks.exe
if errorlevel 1 exit /b 1
monitorChecks.exe
if errorlevel 1 exit /b 1
cl /nologo /std:c++17 /EHsc /W4 /I"$projectRoot/src" /I"$projectRoot/lib/bufferedWriter/src" "$projectRoot/test/writerChecks.cpp" "$projectRoot/src/sampleFormatter.cpp" "$projectRoot/lib/bufferedWriter/src/bufferedWriter.cpp" /Fe:writerChecks.exe
if errorlevel 1 exit /b 1
writerChecks.exe
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
