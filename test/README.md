# PC smoke checks

From the repository root in ordinary PowerShell:

```powershell
.\test\runSmokeChecks.ps1
```

The runner also works from other directories when invoked by its full path.
It calls twelve suites in `espP4/test` and `espFeather/test`; test logic
stays in those projects. It stops on the first failure, names the failed suite,
and prints how many suites passed. A failed run returns a failing process exit
when called through PowerShell `-File`.

Requirements: Microsoft C++ build tools (as used by the existing runners) and
Python 3 on PATH. Use `-Python 'C:\path\to\python.exe'` to select an interpreter.
No Python packages beyond the standard library are needed by these twelve suites.
The writer checks include the existing counter wraparound test beyond 4 GiB;
that suite may take longer than the small arithmetic checks.

Coverage: decoder, recording header/packing, shared writer, exact sample values,
RAM summaries, profiling math, Feather ADC driver/configuration, Feather DAC math, Feather status parsing, and
Python recording/measurement analysis. Two suites check the hardware smoke
readback parser and pass/fail rules using synthetic data, without accessing boards. PC tests cannot establish ADC/DAC analog
accuracy, board timing, SD performance, or successful firmware compilation.

## Optional browser check

```powershell
.\test\runSmokeChecks.ps1 -IncludeBrowser
```

This adds the existing Feather UI test with simulated device replies. It needs
Node.js, Playwright resolvable by Node, and Microsoft Edge. `-Node` can select a
Node executable. The runner installs nothing and does not silently skip a
requested browser check if a dependency is missing. Screenshots go under
`espFeather/.pio/`.

The retired S3 project is no longer required. Firmware builds are separate; see [Building the firmware](../docs/BUILDING.md).

## Real hardware smoke test

`runHardwareSmoke.ps1` is separate from the PC-only checks. It runs the DAC, ADC
and P4 SD recorder for 30 seconds, then compares actual disk readback with A2
measurements. It requires the dedicated firmware on both boards. See
[hardware smoke setup and commands](../docs/HARDWARE-SMOKE.md).
