# Building the firmware

## Which tools build which board?

| Folder | Build system | Application framework |
| --- | --- | --- |
| `espP4` | Espressif ESP-IDF, using `idf.py` | ESP-IDF directly |
| `espFeather` | PlatformIO | Arduino for ESP32 |
| `espS3` | PlatformIO | Arduino for ESP32; legacy recorder |
| `shared` | Compiled by the board projects | No separate firmware build |

ESP-IDF means **Espressif IoT Development Framework**. Espressif makes the ESP32
chips. Its toolkit supplies hardware drivers, task scheduling, and tools for
configuring, compiling, uploading, and monitoring firmware. Arduino for ESP32
uses ESP-IDF underneath; PlatformIO manages its build tools and dependencies.

A **build** compiles code on the PC. An **upload** replaces firmware on a board.
Neither the build commands below nor the PC checks require connected boards.
Karl runs commands and handles Git. Uploading and starting a hardware test are
separate actions; this guide does not do either automatically.

## P4: ESP-IDF terminal

Use the installed **ESP-IDF 5.5.6 PowerShell** terminal. In VS Code with the
ESP-IDF extension, the command palette offers **ESP-IDF: Open ESP-IDF Terminal**.
An ordinary PowerShell terminal does not necessarily have the required compiler
and Python environment. Run `idf.py --version` to check the active environment.

From the repository root, change to the P4 project:

```powershell
Set-Location .\espP4
```

For the 60-second profiling and RAM-comparison firmware:

```powershell
idf.py -B build/profile -D SDKCONFIG=sdkconfig.profile -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.sd.defaults;sdkconfig.profile.defaults" build
```

For the 30-minute SD capture firmware without task/CPU profiling:

```powershell
idf.py -B build/recording -D SDKCONFIG=sdkconfig.sd -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.sd.defaults" build
```

These build directories are new after the October 20 folder rename. Do not reuse
old `build-profile`, `build-sd`, or root `build` caches: they contain absolute paths
to the former project folder. Existing saved `sdkconfig.profile` and `sdkconfig.sd`
settings are retained; defaults only initialize settings not already saved.
Choose the matching Feather environment before running a hardware experiment;
see [current setup](../espP4/CURRENT-SETUP.md).

ESP-IDF still finds its application component in `main`. Implementations are in
`main/src`; headers are in `main/include`; `main/CMakeLists.txt` lists both.
Tests and profiling remain under `test` and use the same decoder and recorder.

## Feather: ordinary PowerShell

From the repository root, build the normal web application:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir .\espFeather -e featheresp32
```

For the currently documented constant-input diagnostic, replace the environment:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir .\espFeather -e ads127-p4-constant
```

`-e` selects an environment: a named set of source files and compiler settings.
Other diagnostics are listed in [current setup](../espP4/CURRENT-SETUP.md).
Test environments live in `espFeather/test/platformio.ini`, imported by the main
configuration. The web application's browser assets under `data` require a
separate filesystem upload when those assets change.

## Legacy S3: ordinary PowerShell

From the repository root:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir .\espS3 -e esp32-s3-devkitc-1
```

Test environments live in `espS3/test/platformio.ini`. This is the historical S3
recorder; building it does not establish that it meets the P4 capture rate.

## PC checks after source moves

Run these individually from the repository root in ordinary PowerShell:

```powershell
.\espP4\test\runDecoderChecks.ps1
.\espP4\test\runExactSampleChecks.ps1
.\espP4\test\runRamWaveformChecks.ps1
.\espP4\test\runProfileChecks.ps1
.\espFeather\test\runBenchDacChecks.ps1
.\espS3\test\runNativeChecks.ps1
```

The C++ runners locate Microsoft's installed C++ build tools. They exercise code
on the PC; they do not upload firmware, operate boards, or establish hardware
performance. Compilation and hardware verification remain outstanding after this
folder cleanup until Karl reports their results.

## Capture output and preserved evidence

New capture logs and plots default to `captures/` under the repository root.
Run capture scripts from that root, or pass `--output` to choose another location.
This generated folder is ignored by Git. Selected historical evidence is under
[docs/evidence](evidence/README.md); hardware references are under
[docs/reference](reference/README.md).
