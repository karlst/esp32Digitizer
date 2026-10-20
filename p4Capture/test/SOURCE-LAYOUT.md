# Recording and test source layout

Production implementations stay in `p4Capture/main/`, `featherUi/src/`,
`s3Recorder/src/`, and `shared/bufferedWriter/src/`. Tests use those implementations;
there is no copied decoder, disk writer, or ADC driver.

| Location | Contents |
| --- | --- |
| `p4Capture/test/hardware/` | SD speed generator, generated-file format/checker, RAM waveform summary and report |
| `p4Capture/test/profile/` | Task/CPU profiling, decoder timing, and profiling arithmetic |
| `p4Capture/test/firmware.cmake` | Adds bench sources to the existing P4 firmware; enables CPU profiling only when configured |
| `featherUi/test/hardware/` | ADC bench setup/controller, DAC/loopback tests, retained LED test |
| `s3Recorder/test/hardware/` | Historical ADS127 receiver, generated-data recording tests, startup storage check and other hardware diagnostics |
| `shared/test/support/` | Bench serial-line assembly and run-duration choices shared by board tests |
| Each project's `test/` | PC checks, fake hardware, capture/analysis scripts and test build configuration |

The main programs retain calls that connect tests to real capture and storage.
P4 `main.cpp` selects SD or RAM output and starts/stops the existing SD test.
The recorder calls the separate generated-data verifier only for synthetic files.
Its ordinary checksum, error reporting, and ADC recording format remain in mainline
code because they protect real recordings too.

The Feather web application still offers its legacy LED routes, so its normal
build links the LED test from the test folder. The reusable ADS127 SPI driver
remains in `src/adc/`; only the bench configuration sequence moved out.

S3 storage-check builds link `storageCheck.cpp`; ordinary builds do not. S3's
synthetic build uses the real recorder and adds only test generation and metadata.

Build environment names, console commands, recording formats and capture
limits are unchanged. The root PlatformIO files import `test/platformio.ini`.
P4's main CMake file imports `test/firmware.cmake`. Generated build directories
and saved SDK configuration files are not source files and were not relocated.

After source changes, run the PC checks and rebuild the affected firmware before
uploading. Source/path inspection alone does not establish a successful build or
unchanged timing on hardware.
