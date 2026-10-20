# Feather tests

Run `test/runSmokeChecks.ps1` from the repository root for the combined PC checks.
`espFeather/test/runAdcDriverChecks.ps1` tests the real ADC SPI driver and
configuration sequence using simulated registers, with no connected board.
`runMonitorChecks.ps1` tests the retained web application's status parser;
`runBenchDacChecks.ps1` checks DAC arithmetic. `uiSmoke.cjs` checks browser controls.
The Arduino and SPI substitutes are local to `test/fakes` and never enter firmware.

Hardware environments live in `test/platformio.ini`. The capture entry point
constructs `AcquisitionApplication` with `BenchAcquisitionServices`; test code
owns the DAC and optional A2 measurements. The real ADC driver/configuration
live under `src/adc` and `include/adc` and are shared with ordinary recording.

See [build instructions](../../docs/BUILDING.md),
[hardware smoke steps](../../docs/HARDWARE-SMOKE.md),
[experiment selection](../../espP4/CURRENT-SETUP.md), and
[retained test history](../../docs/P4-EARLY-TEST-HISTORY.md).
The S3 firmware was removed; these tests need no S3 directory.
