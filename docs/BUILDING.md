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

### Start directly from PowerShell

In an ordinary PowerShell window, load the installed ESP-IDF environment:

```powershell
. C:\Espressif\tools\Microsoft.v5.5.6.PowerShell_profile.ps1
```

The leading dot followed by a space loads the script into the current session,
so its compiler and Python settings remain available there. Then run:

```powershell
Set-Location C:\Users\karls\source\esp32Digitizer\espP4
idf.py --version
```

Expect ESP-IDF 5.5.6. Repeat the activation command in each new PowerShell session
that needs the IDF tools. This prepares the terminal; it does not build or upload.

Use this installer-generated profile on Karl's PC rather than calling
`esp-idf/export.ps1` directly. The installation stores its tools and Python
environment under `C:\Espressif\tools`. Without those settings, the generic export
script looks under `C:\Users\karls\.espressif` and reports a missing Python
environment. That error alone does not mean a reinstall is needed.

### Open the terminal, including after restarting Windows

1. Open the project in VS Code.
2. Press **Ctrl+Shift+P** to open the command palette.
3. Type **ESP-IDF: Open ESP-IDF Terminal** and select that command.
4. If VS Code asks which workspace folder to use, choose **espP4**.
5. In the newly opened terminal, select the P4 directory and check the tools:

```powershell
Set-Location C:\Users\karls\source\esp32Digitizer\espP4
idf.py --version
```

The installed version for this project is **ESP-IDF 5.5.6**. Opening the IDF
terminal prepares the compiler and Python environment for that terminal session;
an ordinary PowerShell terminal may not have them. If `idf.py` is not recognized,
check that you are typing in the newly opened ESP-IDF terminal.

If the command is missing from the palette, check that the **Espressif IDF**
extension is enabled in VS Code. Opening this terminal does not build, upload,
or communicate with a board.

### Build the selected firmware

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

From the repository root, build the current terminal firmware (the constant-input test):

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir .\espFeather
```

The default environment is `ads127-p4-constant`. Use `-e` to select another named
set of sources and compiler settings; see [current setup](../espP4/CURRENT-SETUP.md).
The environments live in `espFeather/test/platformio.ini`. Application sources
are in `src`, headers in `include`, and hardware test entry points in `test`.

### Separate web application

The preserved web interface is its own project under `espFeather/webApp`.
It is excluded from the parent terminal build. Build it explicitly with:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir .\espFeather\webApp -e featheresp32
```

Its sources, headers, and browser assets are under `webApp/src`, `webApp/include`,
and `webApp/data`. Assets require a separate filesystem upload when changed.
This interface controls the older S3 application; it does not control the P4.

## Legacy S3: ordinary PowerShell

From the repository root:

```powershell
& "$env:USERPROFILE\.platformio\penv\Scripts\platformio.exe" run --project-dir .\espS3 -e esp32-s3-devkitc-1
```

Test environments live in `espS3/test/platformio.ini`. This is the historical S3
recorder; building it does not establish that it meets the P4 capture rate.

## PC checks after source moves

For the active P4 and Feather projects, run the combined smoke check first:

```powershell
.\test\runSmokeChecks.ps1
```

See [smoke-check coverage and requirements](../test/README.md). S3 remains a
separate legacy check. The individual commands below are useful for isolation.

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

## Thirty-second hardware smoke firmware

Use the separate `ads127-hardware-smoke` Feather environment and P4 `build/smoke`
configuration. See [hardware smoke test](HARDWARE-SMOKE.md) for build, upload,
wiring, and the single run command. The normal environments remain unchanged.
