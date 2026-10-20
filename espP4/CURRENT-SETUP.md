# Start here: P4 and Feather bench diagnostics

Build tools and current commands: [Building the firmware](../docs/BUILDING.md).

Checkpoint: October 19, 2026. Karl runs commands from the repository root in
PowerShell and handles all Git operations. Nothing starts automatically at boot.
The latest reported uploads are **Feather ads127-p4-constant on COM4** and
**P4 profiling/RAM firmware on COM3**. Ports can change after USB reconnection.

## Choose the experiment before building

| Feather PlatformIO environment | Purpose / DAC behavior | P4 partner and storage | PC/operator entry point |
|---|---|---|---|
| `ads127-spi-check` | ADC register commissioning; no DAC | No P4 capture | [Feather test guide](../espFeather/test/README.md) |
| `ads127-capture-control` | Legacy S3, five seconds; DAC off | Legacy S3 receiver | Feather USB commands; not the current P4 pairing |
| `ads127-p4-capture-60s` | Internal-short ADC baseline; DAC off | P4 profile build, 60/65 seconds | [PROFILE.md](PROFILE.md); prepare SD, then Feather `6` |
| `ads127-p4-capture-30min` | Endurance baseline; DAC off | P4 normal SD build, 1800/1805 seconds | [README.md](README.md); prepare SD, then Feather `6` |
| `ads127-p4-dac-60s` | External CH0, nominal 10 Hz sine | P4 profile build; SD or RAM selected before run | [ANALOG-TEST.md](ANALOG-TEST.md) or [RAM-WAVEFORM.md](RAM-WAVEFORM.md) |
| `bench-dac-loopback` | Sine measured locally at A2 | No P4 or SD | `python .\espFeather\test\captureDacLoopback.py --port COM4` |
| `ads127-p4-compare` | Sine plus simultaneous A2 measurement | P4 profile/RAM build; no SD | `python .\espP4\test\captureComparison.py --feather COM4 --p4 COM3` |
| `ads127-p4-constant` | Fixed DAC code 135 plus A2 measurement | Same P4 profile/RAM build; no SD | Same comparison command with `--constant` |

`featheresp32` is the separate normal web application. It does not implement
these diagnostic report protocols. The table's capture commands operate hardware;
do not run them merely to check source syntax.

## Current wiring and capture sequence

Feather A1/GPIO25 drives A2/GPIO34 through the existing jumper and TI CH0P
through the coax. CH0N is grounded; boards share ground. Existing digital
translator/SPI/UART connections stay as documented in the wiring plan.
Make wiring changes with power off. For a connected capture, both USB supplies
and TI 6 V power must be present. No SD card is required for RAM capture.

Close both monitors, reset P4, then run the chosen comparison command. The PC
requests Feather help (`h`) and P4 RAM mode (`w`), validates their identities,
then sends stage `6` to Feather. READY is permission to start, not proof of
sample receipt. RAM mode locks until P4 reset; reset before every repeated run.
The reader saves complete logs even when a run fails. A saved HTML report means
its parsing/clean-capture checks succeeded, not that analog accuracy is proven.
Report field meanings and compatibility requirements are in
[REPORT-PROTOCOL.md](test/REPORT-PROTOCOL.md).

## What is isolated, and what is not

- Feather diagnostics are opt-in build environments; they exclude the web app.
- P4 profiling is controlled by `CONFIG_P4_CAPTURE_PROFILE`, default off.
  Enabled selects 60/65-second timing; disabled selects 1800/1805 seconds.
- P4 RAM waveform support is currently compiled in, with runtime `w` selection.
  SD tasks and their buffers still exist but do not access the card in RAM mode.
  A separate compile-time exclusion is an open proposal, not implemented behavior.
- Full-rate SD stores each accepted decoded sample as three bytes, without the
  diagnostic averaging. RAM plots summarize samples into half-millisecond bins.
- No build here is a claim of production readiness. See [open decisions](../TODO.md).

## Verification before the cleanup commit

Recent hardware evidence is in [the status record](../docs/STATUS-2026-10-19-P4.md).
This cleanup does not replace that evidence with a new claim of hardware testing.
For the Python readability edits, Karl runs these PC-only checks:

```powershell
python .\espP4\test\comparisonChecks.py
python .\espP4\test\constantChecks.py
```

Portable decoder, exact-sample, profiler and RAM-bin checks retain their separate
`run*Checks.ps1` entry points under `espP4/test`. Builds/uploads are separate
steps; there is no automatic hardware test runner. Review Git status yourself:
ignore rules do not remove already tracked generated files.
