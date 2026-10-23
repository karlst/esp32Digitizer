# Copyright (C) 2026 Karl Stock
# SPDX-License-Identifier: GPL-3.0-or-later
"""Validate selectable-duration runs and compare only the data retained on SD."""
import math
import re
import statistics
from constantReport import calibrationMapping
from hardwareSmokePlot import writePlot


def analyzeRetained(feather, p4, progress=None, plotPath=None):
    """Check whole-run counters, exact retained coverage and unadjusted voltage levels.

    Earlier overwritten samples have no readback proof. Firmware verifies each
    retained generation against its accepted-source CRC. This reader requires
    that verdict plus consistent session/range metadata and a matching A2 window.
    """
    from hardwareSmokeReport import single, errorLimitPercent
    checks = []

    def confirm(message):
        """Publish only an established check, never an early overall PASS."""
        checks.append(message)
        if progress is not None:
            progress(message)

    if 'SMOKE_FEATHER version=4 ready=1 ' not in feather or 'SMOKE_P4 version=5 ready=1 ' not in p4:
        raise ValueError('Selectable-duration firmware is required on both boards')
    seconds = int(single(r'^TEST OK seconds=(\d+)$', p4, 'P4 duration'))
    if seconds not in (30, 300, 600) or single(r'^TEST OK seconds=(\d+)$', feather, 'Feather duration') != str(seconds):
        raise ValueError('Board durations do not match')
    if 'DAC MODE: SQUARE codes=110,135 levelSeconds=1' not in feather:
        raise ValueError('Feather is not using the slow square wave')
    run, count, clean, missed, gap = map(int, single(
        r'^INPUT_BEGIN run=(\d+) count=(\d+) clean=(\d+) missed=(\d+) maxGapUs=(\d+) mode=burst$', feather, 'Input report'))
    inputFirst, inputTotal, inputSeconds = map(int, single(
        r'^INPUT_RANGE first=(\d+) total=(\d+) seconds=(\d+)$', feather, 'Input coverage'))
    if (inputSeconds != seconds or inputTotal != seconds * 10 or count != min(800, inputTotal) or
            inputFirst != inputTotal - count or clean != 1 or missed != 0 or
            single(r'^INPUT_END run=(\d+)$', feather, 'Input completion') != str(run)):
        raise ValueError('Feather observations are incomplete or belong to another run')
    diskRun, rate, samples, bins, clean = map(int, single(
        r'^SMOKE_SD_BEGIN run=(\d+) rate=(\d+) samples=(\d+) bins=(\d+) clean=(\d+)$', p4, 'SD report'))
    if re.search(rf'^ADC127 RAMFULL {run}$', p4, re.M):
        raise ValueError('SD file capacity exhausted; this is not ring-buffer overflow')
    if diskRun != run or rate != 1024000 or clean != 1 or single(r'^SMOKE_SD_END run=(\d+)$', p4, 'SD completion') != str(run):
        raise ValueError('P4 capture or retained-file verification failed; see ADC127 and SD FAILED lines')
    # 20 ms covers the DMA tail; 100 ppm allows independent board clocks to
    # diverge during longer runs. This is not a precision frequency qualification.
    if abs(samples / rate - seconds) > max(.02, seconds * .0001):
        raise ValueError('Recorded duration differs from the selected duration')
    first, retained, total = map(int, single(
        r'^SMOKE_SD_RANGE firstSample=(\d+) retainedSamples=(\d+) totalSamples=(\d+)$', p4, 'SD sample coverage'))
    firstByte, retainedBytes, totalBytes = map(int, single(
        r'^SD RETAINED firstByte=(\d+) bytes=(\d+) totalBytes=(\d+)$', p4, 'SD byte coverage'))
    session, files, changes, reuses, lost = map(int, single(
        r'^SD SESSION id=(\d+) files=(\d+) changes=(\d+) reuses=(\d+) lost=(\d+)$', p4, 'SD pool'))
    expectedFiles = 2 if seconds == 30 else 5
    lastGeneration = (samples - 1) // (16 * rate)
    expectedFirst = max(0, lastGeneration - expectedFiles + 1) * 16 * rate
    if (samples <= 0 or total != samples or first != expectedFirst or retained != total - first or
            firstByte != first * 3 or retainedBytes != retained * 3 or totalBytes != total * 3 or
            files != expectedFiles or session == 0 or changes != lastGeneration or
            reuses != max(0, lastGeneration + 1 - files) or lost != 0):
        raise ValueError('File rotation, retained range or lost-input counters do not match')
    saved = single(r'^SD SAVED file=(\S+) bytes=(\d+) crc=([0-9a-fA-F]+) ringPeak=\d+/\d+ maxWriteOrFlushUs=\d+ overflow=(\d+) errors=(\d+) unsaved=(\d+)$', p4, 'SD save')
    if int(saved[1]) != totalBytes or any(int(v) != 0 for v in saved[3:]):
        raise ValueError('SD write, overflow or byte-count check failed')
    done = list(map(int, single(r'^ADC127 DONE (\d+) words=(\d+) bad=(\d+) periodErrors=(\d+) short=(\d+) drops=(\d+) errors=(\d+) rate=[0-9.]+/s$', p4, 'ADC completion')))
    if done[0] != run or done[1] != samples or any(done[2:]):
        raise ValueError('Receiver reported missing or invalid samples')
    if 'ADC START echo confirmed.' not in feather or 'ADC STOP command echo confirmed' not in feather:
        raise ValueError('ADC start/stop was not confirmed')
    single(r'^DAC OFF updates=\d+ missedIntervals=0 maxGapUs=\d+ driverFault=0$', feather, 'DAC completion')
    confirm('Digitizer -> P4 DMA: OK - no reported receive errors')
    confirm('Recording buffer: no overflow')
    confirm(f'SD written: {totalBytes:,} sample bytes; {changes} file changes, {reuses} reuses')
    confirm(f'SD readback: retained-file checksums match ({retainedBytes:,} bytes)')

    converter, calibration = calibrationMapping(feather, run)
    if converter is None:
        raise ValueError('Input calibration is missing')
    inputs, outputs = {}, {}
    rows = [line.split(',') for line in feather.splitlines() if line.startswith('INPUT,')]
    if len(rows) != count:
        raise ValueError('Input rows are missing or repeated')
    for iBin, row in enumerate(rows, inputFirst):
        if len(row) != 8:
            raise ValueError('Malformed input row')
        _, position, timestamp, mean, minimum, maximum, reads, duration = row
        mean = float(mean)
        if (int(position) != iBin or not math.isfinite(mean) or
                not 0 <= int(minimum) <= mean <= int(maximum) <= 4095 or
                not 2 <= int(reads) <= 4096 or not 1 <= int(duration) <= 20000 or
                not 0 <= int(timestamp) - (iBin * 100000 + 50000) <= 20000):
            raise ValueError('Input timing, order or range is invalid')
        inputs[iBin] = converter(mean)
    binSamples = rate // 10
    firstBin, endBin = first // binSamples, (samples + binSamples - 1) // binSamples
    rows = [line.split(',') for line in p4.splitlines() if line.startswith('SMOKE_SD,')]
    if bins != endBin - firstBin or len(rows) != bins or bins > 801:
        raise ValueError('Retained disk bins are missing or repeated')
    for iBin, row in enumerate(rows, firstBin):
        if len(row) != 4:
            raise ValueError('Malformed disk row')
        _, position, amount, mean = row
        mean = float(mean)
        expected = min(samples, (iBin + 1) * binSamples) - max(first, iBin * binSamples)
        if int(position) != iBin or int(amount) != expected or not math.isfinite(mean) or not -8388608 < mean < 8388607:
            raise ValueError('Disk bin order, sample count or range is invalid')
        outputs[iBin] = mean * 2500.0 / 8388608
    # Compare only overlapping retained bins. The first offset is a whole
    # 16-second file boundary; any tiny excess tail has no corresponding A2 bin.
    indices = list(range(firstBin, min(endBin, inputTotal)))
    if not indices or any(i not in inputs for i in indices):
        raise ValueError('Feather does not cover the retained disk waveform')
    settled = [i for i in indices if 2 <= i % 10 <= 7]
    low = statistics.mean(inputs[i] for i in settled if i // 10 % 2 == 0)
    high = statistics.mean(inputs[i] for i in settled if i // 10 % 2 == 1)
    swing = high - low
    if swing < 150:
        raise ValueError('Input needs two distinct levels with at least 150 mV swing')
    if any(abs(inputs[i] - (high if i // 10 % 2 else low)) > swing * .25 for i in settled):
        raise ValueError('Input levels are unstable or out of sequence')
    expectedTransitions = list(range((indices[0] // 10 + 1) * 10, indices[-1] + 1, 10))
    for values, label in ((inputs, 'Input'), (outputs, 'Stored signal')):
        states = [values[i] > (low + high) / 2 for i in indices]
        transitions = [indices[j] for j in range(1, len(indices)) if states[j] != states[j - 1]]
        if (states[0] != bool(indices[0] // 10 % 2) or len(transitions) != len(expectedTransitions) or
                any(abs(a - b) > 1 for a, b in zip(transitions, expectedTransitions))):
            raise ValueError(label + ' has missing or misplaced retained square-wave transitions')
    confirm(f'DAC signal: {len(expectedTransitions)} retained level changes verified')
    plateauSeconds = [second for second in range(indices[0] // 10, (indices[-1] // 10) + 1)
                      if all(second * 10 + j in inputs and second * 10 + j in outputs for j in range(2, 8))]
    inputPlateaus = [statistics.mean(inputs[s * 10 + j] for j in range(2, 8)) for s in plateauSeconds]
    outputPlateaus = [statistics.mean(outputs[s * 10 + j] for j in range(2, 8)) for s in plateauSeconds]
    errors = [abs(a - b) for a, b in zip(inputPlateaus, outputPlateaus)]
    worstMv = max(errors)
    errorPercent = worstMv * 100 / swing
    result = dict(checks=checks, bytesWritten=totalBytes, bytesRetained=retainedBytes, seconds=samples/rate,
                  requestedSeconds=seconds, samples=samples, low=low, high=high, errorPercent=errorPercent,
                  errorLimitPercent=errorLimitPercent, worstMv=worstMv, plateauErrorsMv=errors,
                  inputPlateausMv=inputPlateaus, outputPlateausMv=outputPlateaus,
                  firstSecond=plateauSeconds[0], transitions=len(expectedTransitions), file=saved[0],
                  calibration=calibration, passed=errorPercent <= errorLimitPercent)
    if plotPath is not None:
        writePlot(result, plotPath)
    if not result['passed']:
        raise ValueError(f'Plateau-average input/output error {errorPercent:.1f}% exceeds {errorLimitPercent:g}% ({worstMv:.1f} mV)')
    return result
