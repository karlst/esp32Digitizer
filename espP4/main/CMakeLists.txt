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

# Compile P4 capture and storage plus the P4-local reusable byte writer. The reusable writer
# has no receiver dependency; these include paths expose its interface to sdRecorder.
# Select one small startup file. Application and storage implementations are shared.
if(CONFIG_P4_INPUT_SKEW)
    idf_component_register(SRCS "../test/inputSkew/main.cpp" "../test/inputSkew/InputSkewTest.cpp"
        INCLUDE_DIRS "../test/inputSkew"
        REQUIRES esp_driver_mcpwm esp_driver_gpio esp_timer)
    return()
endif()

if(CONFIG_P4_SD_CHOKE)
    # Standalone storage workload: no receiver, Feather link or recording startup.
    idf_component_register(SRCS "../test/choke/main.cpp" "../test/choke/SdChokeTest.cpp"
        "../test/choke/PreservedFiles.cpp" "src/P4RecordingDisk.cpp"
        "../lib/fileRecorder/src/FileRecordingBackend.cpp" "../lib/fileRecorder/src/SessionFileHeader.cpp"
        "../lib/fileRecorder/src/RetainedPayloadChecksums.cpp"
        "../lib/bufferedWriter/src/BufferedWriter.cpp"
        INCLUDE_DIRS "include" "../test/choke" "../lib/bufferedWriter/src" "../lib/fileRecorder/src"
        REQUIRES esp_timer fatfs sdmmc esp_driver_sdmmc esp_psram esp_hw_support esp_rom)
    math(EXPR chokeRingBytes "${CONFIG_P4_CHOKE_RING_MIB} * 1048576")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        BUFFERED_WRITER_EXTERNAL=1 BUFFERED_WRITER_CAPACITY=${chokeRingBytes} BUFFERED_WRITER_BLOCK_BYTES=131072)
    return()
endif()

set(startup "src/main.cpp")
if(CONFIG_P4_FSYNC_ASSOCIATION)
    set(startup "../test/fsync/main.cpp")
endif()
if(CONFIG_P4_HARDWARE_SMOKE OR CONFIG_P4_CAPTURE_PROFILE OR CONFIG_P4_DIAGNOSTICS)
    set(startup "../test/hardware/main.cpp")
endif()
# Driver dependencies must be available during IDF's early dependency scan,
# before configuration-dependent test sources are selected below. Ordinary
# startup does not construct or call the capture/counter diagnostic services.
idf_component_register(SRCS ${startup} "src/RecordingApplication.cpp"
    "src/RecordingController.cpp" "src/ConsoleRecordingReporter.cpp"
    "src/AdcReceiver.cpp" "src/SampleDecoder.cpp" "src/FrameDecoder.cpp" "src/SdRecorder.cpp"
    "../lib/bufferedWriter/src/BufferedWriter.cpp" "src/P4RecordingDisk.cpp"
    "../lib/fileRecorder/src/FileRecordingBackend.cpp" "../lib/fileRecorder/src/SessionFileHeader.cpp"
        "../lib/fileRecorder/src/RetainedPayloadChecksums.cpp"
    INCLUDE_DIRS "include" "../../common" "../lib/bufferedWriter/src" "../lib/fileRecorder/src"
    REQUIRES esp_driver_parlio esp_driver_uart esp_driver_gpio esp_timer fatfs sdmmc esp_driver_sdmmc esp_psram esp_rom esp_hw_support
    PRIV_REQUIRES esp_driver_mcpwm esp_driver_pcnt)
# Allocate the ring separately at runtime: 16 MiB total, writes of up to 128 KiB.
# Apply identical definitions to all sources so class layouts agree.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    BUFFERED_WRITER_EXTERNAL=1 BUFFERED_WRITER_CAPACITY=16777216 BUFFERED_WRITER_BLOCK_BYTES=131072)

# Test implementations are absent from ordinary recording builds.
if(CONFIG_P4_HARDWARE_SMOKE OR CONFIG_P4_CAPTURE_PROFILE OR CONFIG_P4_DIAGNOSTICS)
    include("${CMAKE_CURRENT_LIST_DIR}/../test/firmware.cmake")
endif()

if(CONFIG_P4_FSYNC_ASSOCIATION)
    target_sources(${COMPONENT_LIB} PRIVATE "../test/fsync/FsyncAssociationTest.cpp"
        "../test/fsync/FsyncAssociationAnalysis.cpp" "../test/fsync/PulseDelayCapture.cpp"
        "../test/fsync/FsyncReadTest.cpp" "../test/fsync/FsyncIndexTest.cpp" "../test/fsync/PpsSampleTest.cpp" "../test/fsync/FsyncSdTest.cpp"
        "../test/fsync/FsyncBoundaryMath.cpp" "../test/fsync/FsyncBoundaryCapture.cpp"
        "../test/fsync/FsyncPollTrace.cpp")
    target_sources(${COMPONENT_LIB} PRIVATE "../test/fsync/FsyncTimedTest.cpp" "src/TimedRecorder.cpp"
        "../lib/timedRecording/src/TimedFileFormat.cpp" "../lib/timedRecording/src/TimedRecordingStream.cpp"
        "../lib/timedRecording/src/TimedIntervalBuffer.cpp" "../lib/timedRecording/src/TimedBufferedSink.cpp"
        "../lib/timedRecording/src/TimedFatStorage.cpp")
    target_include_directories(${COMPONENT_LIB} PRIVATE "../test/fsync" "../lib/timedRecording/src")
endif()
