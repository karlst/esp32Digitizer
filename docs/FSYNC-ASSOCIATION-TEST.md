# FSYNC timestamp association diagnostic

This test checks whether hardware-captured FSYNC times can be associated with the
numbered ADC readings received by the P4. The default short test uses the existing
FSYNC connection on P4 GPIO1, after the translator; leave the generator off for
that mode. Optional PPS modes capture an external 1-Hz input on GPIO6. The
combined PPS/SD mode also records samples through the ordinary SD writer.
None of these modes establishes GPS UTC or measures ADC delay; the separate
[pulse-delay test](PULSE-DELAY-TEST.md) measures the input-to-output delay.

## PC checks, build, and upload

Karl runs these commands. From the repository root, run the usual PC checks first:

```powershell
.\test\runSmokeChecks.ps1
```

The decoder checks cover the new explicit stopped-ADC startup, including a first
captured HIGH, split blocks and restoration of ordinary startup on a later reset.
They also test timing arithmetic, timer wrap, wrong sample numbers and missing
observations. Python report checks reject inconsistent printed PASS results.
These tests do not establish hardware timing.

In the ESP-IDF PowerShell environment, from `espP4`, build an isolated configuration:

```powershell
idf.py -B build/fsync -D SDKCONFIG=sdkconfig.fsync -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.sd.defaults;sdkconfig.fsync.defaults" build
```

The runner requires `FSYNC_P4 version=3` (explicit startup-frame discard and
pulse-to-word numbering). Rebuild and reflash the P4 when updating older versions.

The SD defaults supply the board's PSRAM settings. Startup constructs an SD
adapter but allocates its task/ring only when combined mode is explicitly selected;
ordinary RAM modes do not access the card. It uses the same receiver, decoder, application
and UART controller as ordinary recording. ESP-IDF 5.5.6 is required because one
MCPWM/PCNT register access is tied to that SDK's allocation and register definitions.

After the build passes, upload the P4 only:

```powershell
idf.py -B build/fsync -D SDKCONFIG=sdkconfig.fsync -p COM3 flash
```

Keep the existing external-source Feather bench firmware, identified as
`SMOKE_FEATHER version=6`. Its DAC remains disabled. No Feather changes are needed.
Close serial monitors before running the test. With the ADC powered and both boards
ready, run from the repository root:

```powershell
python .\espP4\test\runFsyncAssociation.py
```

Defaults are Feather COM4, P4 COM3, five repeated starts. Override with `--feather`,
`--p4` or `--runs` if needed. The script refuses the ordinary P4 smoke firmware.
It saves both board logs, matched timestamps/words and results under
`captures/fsync-association/<date-time>/`. It requests a stop on failure or Ctrl+C.
No build, upload, formatting, file deletion or generator control is performed.

## What the default short association test does

1. While Feather holds the ADC stopped, verify FSYNC is LOW and that a hardware
   pulse counter sees no pulses. Arm the receiver, then check again before READY.
2. Count the first captured HIGH as the first frame, but deliberately discard its
   voltage. The observed startup contains 31 clocks; a full 32-clock first frame
   is also accepted and discarded. The 31-clock case is allowed only when that
   frame starts at captured clock zero. Other lengths, and all later framing
   errors, still fail. Stored word zero begins at hardware pulse two. Ordinary
   builds retain their existing decoding behavior.
3. MCPWM capture saves the 160-MHz timer on **every** FSYNC rising edge. PCNT counts
   those same edges. No software interrupt is requested for each FSYNC. PCNT's
   driver handles its infrequent 32767-count wraps.
4. Occasionally read an accumulated pulse-count baseline through the SDK. Then
   read GPIO, the small PCNT register and MCPWM timestamp directly inside a short
   critical section. Read both registers twice and discard the observation if
   either changes or FSYNC is HIGH at either end. No driver calls or copying occur
   inside this section. Attach the small count to the baseline, accounting for
   one 32767-pulse wrap. Reject a baseline older than 100 microseconds. A saved
   count of 101 identifies retained word index 99, because pulse one was discarded.
5. Retain up to 65536 words in RAM. After 32768 words, request ADC stop using the
   existing RAMFULL/STOP handshake. Allow queued receiver blocks to finish.
6. Print matched timestamps with their actual retained words, count differences,
   and timestamp-spacing residuals. Readiness is checked before each new run.

The timestamp comes from the hardware capture register, not the software polling
time. Polling deliberately skips many FSYNCs; it does not renumber them. The capture
timer's 32-bit wrap is handled by unsigned differences during these short trials.
MCPWM group 0 and its first capture channel, and PCNT unit 0, belong exclusively
to this test. SDK calls were too slow to obtain stable snapshots in the initial
hardware trials, so version 2 uses direct register reads for the short window.

## What the default association PASS means and what it does not

PASS requires checked startup, clean ordinary receive/decoder results, at least
four retained timestamp/word pairs spanning 1024 samples, increasing pulse numbers,
and timing consistent with the expected sample rate. The test measures the average
ticks per sample instead of assuming the two clocks have exactly their nominal
frequencies. Each interior timestamp must agree with that spacing within four
timer ticks (25 ns). This is a consistency threshold, not an absolute accuracy claim.

The normal receiver can discard its final incomplete DMA chunk. The report shows
that difference and permits at most 512 unmatched final frames, the maximum raw
chunk capacity. Those final readings are not presented as retained words. This
means the total-count check does not independently exclude a whole-frame loss
hidden inside that tail allowance. Likewise, a constant numbering error affecting
every anchor cannot be discovered from spacing alone: association relies on the
stopped-input startup and continuous receiver operation. Passing repeated trials
provides evidence for those conditions; it is not a universal proof against every
hardware failure. The test must not be represented as a calibrated PPS timestamp
or a demonstrated 6.25-ns absolute accuracy.

FAIL with too few anchors can mean the stable-read window was not reached often
enough; it does not establish a faulty ADC. Startup failure means the test did not
observe the required stopped LOW state. Preserve the logs rather than increasing
tolerances or silently guessing the missing first sample.

`FSYNC READS` reports saved snapshots and rejection reasons. `FSYNC PREFIX` saves
the first sixteen packed receiver bytes; `FSYNC FIRST` gives the first four frame
positions in received DCLK clocks. `FSYNC BAD_PERIOD` locates the first spacing
error, its previous frame position and the number of bits accumulated before it.
Version 3 adds `FSYNC ALIGN`, reporting the discarded frame count, observed
startup length and first retained pulse (2). Every `FSYNC ANCHOR` includes both
the retained sample index and the original hardware pulse count. The PC requires
`pulse = sample + 2`; a constant one-word shift cannot pass merely because the
timestamp spacing agrees. On a failed run, tentative pairs are labeled
`CANDIDATE`, not `ANCHOR`.

The initial 31-clock observation does not establish why that first clock was
absent, nor independently prove no entire startup frame was lost. This change
does not reconstruct that first reading. It deliberately omits the first frame
in both the 31- and 32-clock cases and makes that numbering assumption explicit.
Software regression checks use distinct successive voltages to verify which
word is retained first, exercise split blocks and timer wrap, and require later
missing clocks and other initial lengths to remain errors. Repeated hardware
trials subsequently passed; PC tests alone cannot establish hardware behavior.

The separate [pulse-delay build](PULSE-DELAY-TEST.md) now adds a direct test pulse
on a second capture channel and compares it with individual ADC readings. The
association-only configuration still requires no new signal wire and does not
measure ADC delay.

## Extended modes and combined SD test

The same FSYNC firmware accepts these mutually exclusive runner options:

| Option | Duration | Purpose |
| --- | --- | --- |
| `--read-test` | 10 seconds | Measure register-read cost and consistency/retry failures. |
| `--index-test` | 10 seconds | Calculate pulse numbers from timestamp differences; compare with the hardware pulse counter. |
| `--pps-test` | 60 seconds | Attach captured PPS edges to numbered sample references, across timer wraps, without SD. |
| `--pps-sd-test --runs 1` | 60 seconds | Perform the PPS checks while decoding and recording through the real SD writer. |

Index/PPS modes use frequent observations, nominally about once per millisecond,
not just one read per PPS. Timestamp differences advance pulse numbering; the
separate counter supplies the starting number and independent comparisons.
After ADC stop, the settled final counter/timestamp checks the accumulated number.
Startup still uses pulse 2 as sample zero. A PPS reference is a later directly
observed FSYNC, not necessarily the nearest sample. See `FsyncIndexTest.cpp` and
`PpsSampleTest.cpp` for rejection limits and wrap handling.

For the combined test, retain FSYNC on GPIO1 and connect the verified external
0–3.3 V, 1-Hz, 10%-duty generator signal to GPIO6 with common ground. CH0P may
receive the same pulse through the previously verified resistor divider, keeping
its input within the configured range; CH0N remains grounded. Real GPS is not
required. Power the ADC, insert the existing exFAT card, close serial monitors,
and reset both boards. From `espP4`:

```powershell
python .\test\runFsyncAssociation.py --pps-sd-test --runs 1 --p4 COM3 --feather COM4
```

The runner sets the P4 calendar clock from PC time and requires acknowledgement
before preparation. This supplies the existing file header's required date; it
does not synchronize timestamps to UTC. It configures Feather's safety timeout
to 300 seconds; reset restores the Feather default. The P4 requests stop after
60 seconds' worth of samples. File allocation and readback take additional time.

Combined mode creates one new file with 65 seconds of capacity, preserves old
files and never formats the card. Storage prepares first, then timing capture
arms while the ADC is stopped. Core 1 decodes and submits batches; the existing
Core 0 disk task writes the ring. This is still the existing three-byte sample
file format. Timing records go to PC logs/result.json, **not to the SD payload**.
The proposed tBlock/sBlock format is not implemented here.

The PC requires valid timing, confirmed source shutdown and `SD SAVED`, including
matching full-payload CRC readback, byte count, and zero loss/overflow/errors.
`ADC127 DONE` alone does not mean SD writing has finished. On an acquisition
fault the runner requests stop and collects ten more seconds of diagnostics;
that bounded wait does not certify disk completion. Keep the card inserted until
the firmware reports `SD SAVED` or `SD FAILED`.

## Decoder regression, measurements and verified result

The November startup-frame checks enlarged `beginFrame`. Inspection of retained
October object files showed frame-start processing inlined into `feed`; the
November object had a separate call for every sample. The repair explicitly
inlines the normal path and keeps startup/error processing in `checkFrame`.
The startup-discard flag is consumed at the second frame, so subsequent words
no longer compare the 64-bit frame count against one. All period, short-word
and status checks remain. Desktop checks include 31/32-clock startup, malformed
later frames, split input buffers, and a 1,100-frame startup-to-normal transition.

Diagnostic measurements are retained for future regressions:

- `FSYNC CPU`: raw bytes, total elapsed time inside decoding and its synchronous
  output callbacks, and time spent retaining diagnostic samples.
- `SD CPU`: batch/byte totals and elapsed time packing samples, copying into the
  ring, and calculating the submitted-payload checksum. These probes execute
  only in the FSYNC diagnostic build.
- `FSYNC WORK`: maximum raw-buffer waiting/processing times, pause count and
  maximum actual pause; `waitAfterPauseUs` is the maximum wait of the first
  buffer processed after a deliberate decoder pause.

Despite the historical `CPU` row names, these are elapsed microseconds including
interruptions, not exclusive CPU time. SD subtotals are already inside the
decoder total; do not add them again. Timer calls themselves add measurement
overhead. Reporting happens after decoder drain, never inside the sample loop.

Karl's run `captures/fsync-association/20261107-135357`, run ID 3740832635,
passed after the repair:

| Measurement | Result |
| --- | --- |
| Saved samples / bytes | 61,446,111 / 184,338,333 |
| PPS captured / mapped | 60 / 60 |
| Timer wraps | 2 |
| Final pulse-number error | 0 samples |
| Independent count comparisons | 29,813 matched; zero mismatches |
| Saved file / CRC32 | `0:/RECORD/session-18` / `4a00c092` |
| SD ring peak / capacity | 1,969,541 / 16,777,216 bytes |
| Maximum write/flush duration | 124,037 microseconds |
| Storage overflow / errors / unsaved bytes | 0 / 0 / 0 |

The stop handshake permits a few samples beyond the 60-second target. Full-file
readback verified the retained payload. This establishes a successful combined
run with the tested card and configuration, not a worst-case throughput bound,
GPS UTC association, or calibrated 6.25-ns absolute timing accuracy.
