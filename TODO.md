# Project TODO

Current build/commands: [P4 setup](p4Capture/CURRENT-SETUP.md).
Completed experiments: [October 19 checkpoint](docs/STATUS-2026-10-19-P4.md).
Items below are proposals or unfinished work, not automatic implementation authorization.
Karl handles Git and runs builds, checks, uploads and hardware operations.

## Before this checkpoint commit

- [x] Karl reran comparisonChecks.py and constantChecks.py after readability cleanup; both reported PASS on October 19.
- [ ] Karl reviews source and Git status, including any previously tracked generated files.
- [ ] Review the checkpoint as bench firmware; do not label it production-qualified.

## Analog verification

- [ ] Resolve DAC waveform flattening when connected to the TI input. Loading is
  suspected, not confirmed. A signal generator is being considered; no purchase confirmed.
- [ ] With a suitable source, define and test amplitude, frequency and timing fidelity.
- [ ] Agree production bandwidth, shortest event to preserve and allowable error
  before considering any averaging. Production SD samples currently remain unaveraged.

The jagged-blue-line investigation is closed for now: constant-command captures
showed much greater Feather measurement fluctuation. See the checkpoint for
numbers, factory-calibration details and measurement limitations.

## Physical digital path and storage

- [ ] Decide whether to add ADS127 frame-sync CRC. This needs a framing/rate review
  and cannot by itself detect missing/repeated complete frames.
- [ ] Decide whether a separate known-pattern source is worth implementing. The ADC
  has no documented arbitrary digital pattern generator. PC exact-value tests passed,
  but bypass the physical capture peripheral, DMA and RTOS queues.
- [ ] Qualify SD cards for sustained writes and stalls. CardP passed endurance;
  Card F overflowed. Determine cause before drawing conclusions about either model.
- [ ] Review reservation cleanup after interrupted preparation/reset; do not delete
  existing files automatically. CardP's old files were deleted explicitly by Karl.

## Firmware boundaries and headroom

- [ ] Assess per-core capacity before moving Feather duties. Profile baseline:
  Core 0 busy 40.3%; Core 1 busy 92.6%, including measurement overhead.
- [ ] Recheck a profiling-disabled build before production use.
- [ ] Decide whether RAM waveform support should have a compile-time exclusion.
  It currently remains compiled in and is selected by USB w; profiling has its own switch.
- [ ] Review stack/memory headroom with the eventual production configuration.
  The profile control stack was enlarged after the earlier low-headroom reading;
  do not apply that old minimum-free-stack number to every later build.

## Production error handling and data-quality reporting

Added October 18, 2026, at Karl's request.

- [ ] Decide how production recording responds to ADC status/header problems
  and framing errors. Distinguish an isolated event from persistent failures.
- [ ] Define a response for each condition: ADC error, repeated data, inactive
  channel, unexpected channel ID, power event, modulator saturation, and broken
  frame timing. Decide when to continue with a warning, attempt recovery, or stop.
- [ ] Decide how to preserve affected sample positions and timing, for example
  through a separate event record. Avoid silently deleting questionable samples
  and thereby shifting the apparent timing of subsequent samples. Define how
  uncertain sample counts after framing or data-loss events are represented.
- [ ] Decide what the operator sees during recording and what quality information
  is retained with the saved recording. Distinguish storage failures from ADC or
  capture-quality failures.
- [ ] Define and test the chosen policy before treating it as production behavior.

Current test behavior: checked header failures increment `badHeaders`, preserve
the first eight bad words for console reporting, and do not immediately stop
capture or discard the sample. Any bad header makes final capture validation
fail, resulting in an invalid file and `SD FAILED` even if storage succeeded.
Power-event and saturation bits are counted in the header histogram but do not
alone increment `badHeaders`. Per-sample status bytes are omitted from the file.
Production policy remains undecided; the alternatives above are not approved
implementation decisions.
