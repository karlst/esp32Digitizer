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

# Compile/run the real decoder, buffered writer tests and >4-GiB counter-wrap test
# on this PC. All data stays in RAM; this does not run a board or write an SD card.
# The compiler and generated executables do write ordinary build files below
# .pio/decoderChecks. "RAM" above refers to simulated recording data, not builds.
# Reading path: locate Visual C++, create a batch with its environment, compile
# and run each executable, then propagate any failure to runSmokeChecks.ps1.
# The first executable links the real FrameDecoder and FsyncAssociationAnalysis;
# its synthetic startup and counter-wrap cases cannot establish hardware timing.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$writerRoot = Join-Path $projectRoot 'lib/bufferedWriter'
$recorderRoot = Join-Path $projectRoot 'lib/fileRecorder'
$timedRoot = Join-Path $projectRoot 'lib/timedRecording'
$vsWhere = "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe"
$installation = & $vsWhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$installation)
{
    throw 'Microsoft C++ build tools are required.'
}
$buildDir = Join-Path $projectRoot '.pio/decoderChecks'
New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
$environmentScript = Join-Path $installation 'VC/Auxiliary/Build/vcvars64.bat'
# A generated batch keeps compiler setup and compilation in the same process.
# cl is Microsoft's C++ compiler. /std:c++17 selects the language version;
# /O2 enables optimization, /EHsc exception handling, /W4 warnings, /I header
# paths and /Fe the executable name. The /D settings choose test buffer sizes
# and an externally supplied memory buffer; no ESP32 firmware is produced.
# Each errorlevel check stops before a later success can hide an earlier failure.
$batch = @"
@echo off
call "$environmentScript" >nul
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /DADS127_P4_ENDURANCE=1 /I"$projectRoot/main/include" /I"$projectRoot/test/hardware" /I"$projectRoot/test/profile" /I"$projectRoot/../common" "$projectRoot/test/decoderChecks.cpp" "$projectRoot/main/src/FrameDecoder.cpp" "$projectRoot/test/fsync/FsyncAssociationAnalysis.cpp" /Fe:decoderChecks.exe
if %errorlevel% neq 0 exit /b 1
decoderChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /DBUFFERED_WRITER_EXTERNAL=1 /DBUFFERED_WRITER_CAPACITY=4194304 /DBUFFERED_WRITER_BLOCK_BYTES=131072 /I"$writerRoot/src" "$writerRoot/test/main.cpp" "$writerRoot/test/bufferedWriterChecks.cpp" "$writerRoot/src/BufferedWriter.cpp" /Fe:externalWriterChecks.exe
if %errorlevel% neq 0 exit /b 1
externalWriterChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /DBUFFERED_WRITER_EXTERNAL=1 /DBUFFERED_WRITER_CAPACITY=4194304 /DBUFFERED_WRITER_BLOCK_BYTES=131072 /I"$writerRoot/src" "$projectRoot/test/writerRolloverChecks.cpp" "$writerRoot/src/BufferedWriter.cpp" /Fe:writerRolloverChecks.exe
if %errorlevel% neq 0 exit /b 1
writerRolloverChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /DBUFFERED_WRITER_EXTERNAL=1 /DBUFFERED_WRITER_CAPACITY=1024 /DBUFFERED_WRITER_BLOCK_BYTES=256 /I"$recorderRoot/test/fakes" /I"$recorderRoot/src" /I"$writerRoot/src" "$recorderRoot/test/sessionChecks.cpp" "$recorderRoot/test/fakes/MemoryRecordingDisk.cpp" "$recorderRoot/src/SessionFileHeader.cpp" "$recorderRoot/src/FileRecordingBackend.cpp" "$recorderRoot/src/RetainedPayloadChecksums.cpp" "$writerRoot/src/BufferedWriter.cpp" /Fe:sessionChecks.exe
if %errorlevel% neq 0 exit /b 1
sessionChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /DBUFFERED_WRITER_EXTERNAL=1 /DBUFFERED_WRITER_CAPACITY=262144 /DBUFFERED_WRITER_BLOCK_BYTES=131072 /I"$recorderRoot/test/fakes" /I"$recorderRoot/src" /I"$writerRoot/src" "$recorderRoot/test/writeBatchChecks.cpp" "$recorderRoot/test/fakes/MemoryRecordingDisk.cpp" "$recorderRoot/src/SessionFileHeader.cpp" "$recorderRoot/src/FileRecordingBackend.cpp" "$recorderRoot/src/RetainedPayloadChecksums.cpp" "$writerRoot/src/BufferedWriter.cpp" /Fe:writeBatchChecks.exe
if %errorlevel% neq 0 exit /b 1
writeBatchChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /I"$timedRoot/src" /I"$projectRoot/main/include" /I"$projectRoot/test" "$projectRoot/test/timedFormatChecks.cpp" "$timedRoot/src/TimedFileFormat.cpp" "$timedRoot/src/TimedRecordingStream.cpp" /Fe:timedFormatChecks.exe
if %errorlevel% neq 0 exit /b 1
timedFormatChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /DBUFFERED_WRITER_EXTERNAL=1 /DBUFFERED_WRITER_CAPACITY=1024 /DBUFFERED_WRITER_BLOCK_BYTES=256 /I"$timedRoot/src" /I"$writerRoot/src" /I"$projectRoot/test" "$projectRoot/test/timedBufferChecks.cpp" "$timedRoot/src/TimedBufferedSink.cpp" "$writerRoot/src/BufferedWriter.cpp" /Fe:timedBufferChecks.exe
if %errorlevel% neq 0 exit /b 1
timedBufferChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /DBUFFERED_WRITER_EXTERNAL=1 /DBUFFERED_WRITER_CAPACITY=1024 /DBUFFERED_WRITER_BLOCK_BYTES=256 /I"$timedRoot/src" /I"$writerRoot/src" /I"$recorderRoot/src" /I"$recorderRoot/test/fakes" /I"$projectRoot/test" "$projectRoot/test/timedStorageChecks.cpp" "$timedRoot/src/TimedFileFormat.cpp" "$timedRoot/src/TimedRecordingStream.cpp" "$timedRoot/src/TimedBufferedSink.cpp" "$timedRoot/src/TimedFatStorage.cpp" "$writerRoot/src/BufferedWriter.cpp" "$recorderRoot/test/fakes/MemoryRecordingDisk.cpp" /Fe:timedStorageChecks.exe
if %errorlevel% neq 0 exit /b 1
timedStorageChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /I"$projectRoot/test/fsync" "$projectRoot/test/fsyncBoundaryChecks.cpp" "$projectRoot/test/fsync/FsyncBoundaryMath.cpp" /Fe:fsyncBoundaryChecks.exe
if %errorlevel% neq 0 exit /b 1
fsyncBoundaryChecks.exe
if %errorlevel% neq 0 exit /b 1
cl /nologo /std:c++17 /O2 /EHsc /W4 /I"$timedRoot/src" /I"$projectRoot/test" "$projectRoot/test/timedIntervalChecks.cpp" "$timedRoot/src/TimedIntervalBuffer.cpp" "$timedRoot/src/TimedRecordingStream.cpp" "$timedRoot/src/TimedFileFormat.cpp" /Fe:timedIntervalChecks.exe
if %errorlevel% neq 0 exit /b 1
timedIntervalChecks.exe
exit /b %errorlevel%
"@
$batchPath = Join-Path $buildDir 'run.cmd'
Set-Content -LiteralPath $batchPath -Value $batch -Encoding ascii
Push-Location $buildDir
try {
    & $env:ComSpec /c $batchPath
    if ($LASTEXITCODE -ne 0)
    {
        throw "Decoder checks failed: $LASTEXITCODE"
    }
}
finally { Pop-Location }
