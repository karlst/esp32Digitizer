# Tests and hardware diagnostics

`runNativeChecks.ps1` runs eleven desktop suites. They check command handling,
legacy acquisition, status parsing, buffer wraparound and ownership, concurrent
byte ordering, overflow, partial writes, flush failure and synthetic rate/deadline
arithmetic. No board, serial connection or SD card is used. `fakes/` substitutes
platform types; it does not emulate actual card reliability.

Run from the repository root:

```powershell
& ./s3Recorder/test/runNativeChecks.ps1
```

## Hardware sources

All opt-in firmware diagnostics are in `hardware/`. `buildHardwareTests.py`
adds only the selected test sources to a PlatformIO build; normal firmware does
not compile the synthetic generator or diagnostic entry points.

| Environment | Purpose / trigger |
|---|---|
| `choke-test*` | Synthetic rate ramp; existing Start/Stop command. |
| `sd-reliability-26mbps` | Fixed 26 Mbps for 30 minutes; growing files. |
| `sd-reliability-preallocated` | Same load; entire 5.85 GB reserved first. Successful backend reference. |
| `sd-width-fourbit`, `sd-width-onebit` | Fixed 250 kbps comparison; manual Stop. |
| `sd-write-timing*` | Repeated fixed-block file writes; USB `R` starts. |
| `sd-read-diagnostic` | Raw-sector reads, no file writing; boot or USB `R`. |
| `sd-led-diagnostic` | GPIO LED sequence; card removed. |
| `sd-input-diagnostic` | GPIO input checks; card removed. |
| `hardware/idfSdRead` project | Independent newer-SDK read-only comparison. |

`storage-check*` uses the adapter's commissioning helper: creates a new 64-KiB
file and checks every byte at boot. It preserves existing files. No diagnostic
formats a card. The LED/input tests require their documented test wiring;
do not upload them onto a connected card as though they were recorder firmware.

Example build only:

```powershell
platformio run -d s3Recorder -e sd-reliability-preallocated
```

Upload is a separate, explicitly authorized action (`-t upload`). The reference
environment uses PSRAM from a task; do not enable it for the legacy cache-disabled
ADS1256 interrupt producer.

## Host launchers and logs

`host/checkSdio.py LOG` reboots the S3 and collects its boot report.
`host/runReliability.py LOG` sends Start, logs the timed test and waits for drain.
`host/runPsramChokeUntilFailure.py LOG` runs the ramp until failure or Stop.
These scripts **operate COM4 and can start writing**; they are not unit tests.
They require Python with pyserial, close the port afterward, and preserve logs
at the explicit path given. Use an unused log filename to retain previous runs.
The `.pio` copies are historical local helpers, not the maintained source.

`inspectRecording.py` inspects a recording supplied from the card. Generated
build products and serial logs belong in ignored `.pio/`, not beside test sources.
