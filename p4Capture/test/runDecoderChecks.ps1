# Compile/run the real decoder on this PC. No board connection or upload.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$recorderRoot = Join-Path (Split-Path $projectRoot -Parent) 's3Recorder'
$vsWhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$installation = & $vsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation) { throw 'Microsoft C++ build tools are required.' }
$buildDir = Join-Path $projectRoot '.pio/decoderChecks'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$environmentScript = Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat'
# A generated batch keeps compiler setup and compilation in the same process.
$batch = @"
@echo off
call "$environmentScript" >nul
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /DADS127_P4_ENDURANCE=1 /I"$projectRoot/main" /I"$projectRoot/../shared" "$projectRoot/test/decoderChecks.cpp" "$projectRoot/main/frameDecoder.cpp" /Fe:decoderChecks.exe
if %errorlevel% neq 0 exit /b 1
decoderChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /DBUFFERED_WRITER_EXTERNAL=1 /DBUFFERED_WRITER_CAPACITY=4194304 /DBUFFERED_WRITER_BLOCK_BYTES=131072 /I"$recorderRoot/src" /I"$recorderRoot/src/backend" /I"$recorderRoot/lib/bufferedWriter/src" "$recorderRoot/test/writerChecks.cpp" "$recorderRoot/src/sampleFormatter.cpp" "$recorderRoot/lib/bufferedWriter/src/bufferedWriter.cpp" /Fe:externalWriterChecks.exe
if %errorlevel% neq 0 exit /b 1
externalWriterChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /DBUFFERED_WRITER_EXTERNAL=1 /DBUFFERED_WRITER_CAPACITY=4194304 /DBUFFERED_WRITER_BLOCK_BYTES=131072 /I"$recorderRoot/lib/bufferedWriter/src" "$projectRoot/test/writerRolloverChecks.cpp" "$recorderRoot/lib/bufferedWriter/src/bufferedWriter.cpp" /Fe:writerRolloverChecks.exe
if %errorlevel% neq 0 exit /b 1
writerRolloverChecks.exe
exit /b %errorlevel%
"@
$batchPath = Join-Path $buildDir 'run.cmd'
Set-Content -LiteralPath $batchPath -Value $batch -Encoding ascii
Push-Location $buildDir
try {
    & $env:ComSpec /c $batchPath
    if ($LASTEXITCODE -ne 0) { throw "Decoder checks failed: $LASTEXITCODE" }
}
finally { Pop-Location }
