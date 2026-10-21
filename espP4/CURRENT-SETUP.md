# Start here: P4 and Feather bench diagnostics

Build tools and current commands: [Building the firmware](../docs/BUILDING.md).

For current results and the last reported uploads, see [project status](../docs/STATUS.md).
This page selects compatible experiments; it does not track a second current
status. Karl runs hardware commands and handles Git. Nothing starts automatically
at boot, and ports can change after USB reconnection.

## Choose the experiment before building

| Feather PlatformIO environment | Purpose / DAC behavior | P4 partner and storage | PC/operator entry point |
|---|---|---|---|
| `ads127-hardware-smoke` | 30-second two-level DAC signal plus A2 measurements | P4 smoke build; SD recording/readback, 128 MiB reserve | [Hardware smoke instructions](../docs/HARDWARE-SMOKE.md) |
| `ads127-spi-check` | ADC register communication check; no DAC | No P4 capture | [Feather test guide](../espFeather/test/README.md) |
| `ads127-capture-control` | Five-second diagnostic; DAC off | No current P4 pairing | Feather USB commands |
| `ads127-p4-capture-60s` | Internal-short ADC baseline; DAC off | P4 profile build, 60/65 seconds | [PROFILE.md](PROFILE.md); Feather `g`; SD prepares automatically |
| `ads127-p4-capture-30min` | Endurance baseline; DAC off | P4 normal SD build, 1800/1805 seconds | [README.md](README.md); Feather `g`; SD prepares automatically |
| `ads127-p4-dac-60s` | External CH0, nominal 10 Hz sine | P4 profile build; SD or RAM selected before run | [ANALOG-TEST.md](ANALOG-TEST.md) or [RAM-WAVEFORM.md](RAM-WAVEFORM.md) |
| `bench-dac-loopback` | Sine measured locally at A2 | No P4 or SD | `python .\espFeather\test\captureDacLoopback.py --port COM4` |
| `ads127-p4-compare` | Sine plus simultaneous A2 measurement | P4 profile/RAM build; no SD | `python .\espP4\test\captureComparison.py --feather COM4 --p4 COM3` |
| `ads127-p4-constant` | Fixed DAC code 135 plus A2 measurement | Same P4 profile/RAM build; no SD | Same comparison command with `--constant` |

`featheresp32` in `espFeather/webApp` is the separate web application. It does not implement
these diagnostic report protocols. The table's capture commands operate hardware;
do not run them merely to check source syntax.

## Current wiring and capture sequence

Feather A1/GPIO25 drives A2/GPIO34 through the existing jumper and TI CH0P
through the coax. CH0N is grounded; boards share ground. Existing digital
translator/SPI/UART connections stay as documented in the wiring plan.
Make wiring changes with power off. For a connected capture, both USB supplies
and TI 6 V power must be present. No SD card is required for RAM capture.

For RAM comparison, close both monitors, reset P4, then run the chosen comparison command. The PC
requests Feather help (`h`) and P4 RAM mode (`w`), validates their identities,
then requests the default configuration with `g` on Feather. READY is permission to start, not proof of
sample receipt. RAM mode locks until P4 reset; reset before every repeated run.
The reader saves complete logs even when a run fails. A saved HTML report means
its parsing/clean-capture checks succeeded, not that analog accuracy is proven.
Report field meanings and compatibility requirements are in
[REPORT-PROTOCOL.md](test/REPORT-PROTOCOL.md).

## What is isolated, and what is not

- Feather diagnostics are opt-in build environments; they exclude the web app.
- P4 profiling is controlled by `CONFIG_P4_CAPTURE_PROFILE`, default off.
  Enabled selects 60/65-second timing. The separate `CONFIG_P4_HARDWARE_SMOKE`
  selects 30/35 seconds with profiling off. With both disabled, timing is 1800/1805 seconds.
- P4 RAM waveform support is currently compiled in, with runtime `w` selection.
  SD tasks and their buffers still exist but do not access the card in RAM mode.
  A separate compile-time exclusion is an open proposal, not implemented behavior.
- Full-rate SD stores each accepted decoded sample as three bytes, without the
  diagnostic averaging. RAM plots summarize samples into half-millisecond bins.
- No build here is a claim of production readiness. See [open decisions](../TODO.md).

## Verification

See [PC smoke checks](../test/README.md) for the combined eleven-suite runner.
Firmware builds/uploads are separate steps described in [BUILDING.md](../docs/BUILDING.md).
The [hardware smoke runner](../docs/HARDWARE-SMOKE.md) operates both boards and SD;
it requires its dedicated firmware. A plot can also be saved when the final
voltage comparison fails, so read its PASS/FAIL result.
