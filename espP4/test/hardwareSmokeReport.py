# Copyright (C) 2026 Karl Stock
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of ESP32 Digitizer.
# You may redistribute and/or modify it under the GNU General Public License
# as published by the Free Software Foundation, either version 3 of the License,
# or (at your option) any later version.
#
# Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
# Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.

"""Judge one paired 30-second hardware smoke run using actual disk readback.

No ports or files are opened here. The runner supplies complete board logs.
Use factory/SDK Feather calibration and the existing 2.5-V, unity-gain ADS127
configuration; never fit gain, offset, phase, or acceptance limits to a run.
"""
import math
import re
import statistics
from constantReport import calibrationMapping
from hardwareSmokePlot import writePlot

errorLimitPercent = 10.0


def single(pattern, text, description):
    """Require exactly one complete metadata line; reject absent or mixed runs."""
    matches = re.findall(pattern, text, re.M)
    if len(matches) != 1:
        raise ValueError(description + ': missing or repeated report')
    return matches[0]


def analyze(feather, p4, progress=None, plotPath=None):
    """Return summary numbers or raise a plain-language failure; no hardware I/O.

    Inspect all readback bins for the expected alternating states, then compare
    30 plateau averages, each using six settled bins (0.2..0.8 s of that second). A2 observes a
    500-us burst at the center of each 100-ms disk bin, so this is a steady-level
    accuracy check, not a sample-by-sample comparison of two identical filters.
    The largest plateau-average difference must be <=10% of measured input swing.
    Optional progress receives confirmed check messages, even if a later check
    fails. plotPath optionally saves one HTML voltage plot before that verdict.
    Each is evidence for that step only, never an early overall PASS.
    """
    checks = []

    def confirm(message):
        """Retain one completed check and optionally show it to the operator."""
        checks.append(message)
        if progress is not None:
            progress(message)

    if 'SMOKE_FEATHER version=1 ready=1 seconds=30' not in feather or 'DAC MODE: SQUARE codes=110,135 levelSeconds=1' not in feather:
        raise ValueError('Feather is not running the square-wave smoke firmware')
    if 'SMOKE_P4 version=1 ready=1 seconds=30' not in p4:
        raise ValueError('P4 is not running fresh SD smoke firmware')
    run, count, clean, missed, gap = map(int, single(
        r'^INPUT_BEGIN run=(\d+) count=(\d+) clean=(\d+) missed=(\d+) maxGapUs=(\d+) mode=burst$', feather, 'Input measurements'))
    if count != 300 or clean != 1 or missed != 0:
        raise ValueError('Feather did not collect all 300 clean input measurements')
    if single(r'^INPUT_END run=(\d+)$', feather, 'Input completion') != str(run):
        raise ValueError('Input report belongs to a different run')
    diskRun, rate, samples, bins, clean = map(int, single(
        r'^SMOKE_SD_BEGIN run=(\d+) rate=(\d+) samples=(\d+) bins=(\d+) clean=(\d+)$', p4, 'SD readback'))
    if diskRun != run or rate != 1024000 or bins != 300 or clean != 1:
        raise ValueError('SD readback or capture checks failed, or reports do not match')
    if single(r'^SMOKE_SD_END run=(\d+)$', p4, 'SD completion') != str(run):
        raise ValueError('SD report belongs to a different run')
    if not 29.98 <= samples / rate <= 30.02:
        raise ValueError('Recording length is not 30 seconds within 20 milliseconds')

    # SAVED is emitted only after full payload CRC, header sync and file close.
    # Check size/counters too; a waveform summary alone must never imply success.
    saved = single(r'^SD SAVED file=(\S+) bytes=(\d+) crc=([0-9a-fA-F]+) ringPeak=\d+/\d+ maxWriteOrFlushUs=\d+ overflow=(\d+) errors=(\d+) unsaved=(\d+)$', p4, 'SD save')
    if int(saved[1]) != samples * 3 or any(int(v) != 0 for v in saved[3:]):
        raise ValueError('SD byte count, overflow, or write-error check failed')
    done = single(r'^ADC127 DONE (\d+) words=(\d+) bad=(\d+) periodErrors=(\d+) short=(\d+) drops=(\d+) errors=(\d+) rate=[0-9.]+/s$', p4, 'Receiver completion')
    if int(done[0]) != run or int(done[1]) != samples or any(int(v) != 0 for v in done[2:]):
        raise ValueError('Receiver reported lost or invalid data')
    confirm('Digitizer -> P4 DMA: OK - no reported receive errors')
    confirm('Recording buffer: no overflow')
    confirm(f'SD written: {int(saved[1]):,} sample bytes')
    confirm('SD readback: checksum matches')
    if 'ADC STOP command echo confirmed' not in feather or 'ADC START echo confirmed.' not in feather:
        raise ValueError('ADC start/stop was not confirmed')
    single(r'^DAC OFF updates=\d+ missedIntervals=\d+ maxGapUs=\d+ driverFault=0$', feather, 'DAC completion')
    converter, calibration = calibrationMapping(feather, run)
    if converter is None:
        raise ValueError('Input voltage calibration is missing')

    inputRows = [line.split(',') for line in feather.splitlines() if line.startswith('INPUT,')]
    outputRows = [line.split(',') for line in p4.splitlines() if line.startswith('SMOKE_SD,')]
    if len(inputRows) != 300 or len(outputRows) != 300:
        raise ValueError('Input or disk measurements are incomplete')
    inputs, outputs = [], []
    for index, row in enumerate(inputRows):
        if len(row) != 8:
            raise ValueError('Malformed input measurement')
        _, position, timestamp, mean, minimum, maximum, reads, duration = row
        mean = float(mean)
        if (int(position) != index or not math.isfinite(mean) or
                not 0 <= int(minimum) <= mean <= int(maximum) <= 4095 or
                not 2 <= int(reads) <= 4096 or not 1 <= int(duration) <= 20000 or
                not 0 <= int(timestamp) - (50000 + index * 100000) <= 20000):
            raise ValueError('Input measurement timing or voltage is invalid')
        inputs.append(converter(mean))
    for index, row in enumerate(outputRows):
        if len(row) != 4:
            raise ValueError('Malformed disk measurement')
        _, position, count, mean = row
        mean = float(mean)
        expected = rate // 10 if index < 299 else min(rate // 10, samples - 299 * (rate // 10))
        if int(position) != index or int(count) != expected or not math.isfinite(mean) or not -8388608 < mean < 8388607:
            raise ValueError('Disk measurement order, size, or range is invalid')
        outputs.append(mean * 2500.0 / 8388608)

    settled = [i for i in range(300) if 2 <= i % 10 <= 7]
    low = statistics.mean(inputs[i] for i in settled if (i // 10) % 2 == 0)
    high = statistics.mean(inputs[i] for i in settled if (i // 10) % 2 == 1)
    swing = high - low
    if swing < 150:
        raise ValueError('Input did not alternate between two distinct voltages (need 150 mV swing)')
    # Verify all plateau observations follow the requested order. No gain or
    # offset adjustment is made; actual input sets only the signal-size scale.
    for i in settled:
        target = low if (i // 10) % 2 == 0 else high
        if abs(inputs[i] - target) > swing * 0.25:
            raise ValueError('Input square-wave levels are unstable or out of sequence')
    midpoint = (low + high) / 2
    for values, label in ((inputs, 'Input'), (outputs, 'Stored signal')):
        states = [value > midpoint for value in values]
        transitions = [i for i in range(1, 300) if states[i] != states[i-1]]
        if states[0] or len(transitions) != 29 or any(abs(at - expected) > 1 for at, expected in zip(transitions, range(10, 300, 10))):
            raise ValueError(label + ' does not contain the expected 29 level changes')
    confirm('DAC signal measured at A2: OK - two voltage levels and 29 changes detected')
    # Average each plateau independently; never let errors in different seconds
    # cancel. Six short A2 bursts estimate the same settled interval represented
    # by six disk bins. No gain/offset fit is introduced by this averaging.
    plateauErrorsMv = []
    inputPlateausMv, outputPlateausMv = [], []
    for second in range(30):
        indices = range(second * 10 + 2, second * 10 + 8)
        inputMean = statistics.mean(inputs[i] for i in indices)
        outputMean = statistics.mean(outputs[i] for i in indices)
        inputPlateausMv.append(inputMean)
        outputPlateausMv.append(outputMean)
        plateauErrorsMv.append(abs(outputMean - inputMean))
    worstMv = max(plateauErrorsMv)
    errorPercent = 100 * worstMv / swing
    result = dict(checks=checks, bytesWritten=int(saved[1]), seconds=samples/rate,
                  samples=samples, low=low, high=high, errorPercent=errorPercent,
                  errorLimitPercent=errorLimitPercent, worstMv=worstMv,
                  plateauErrorsMv=plateauErrorsMv, inputPlateausMv=inputPlateausMv,
                  outputPlateausMv=outputPlateausMv, file=saved[0], calibration=calibration,
                  passed=errorPercent <= errorLimitPercent)
    # Complete, structurally valid measurements get a plot even when voltage
    # agreement fails. Never draw missing or invalid records as a passing run.
    if plotPath is not None:
        writePlot(result, plotPath)
    if not result['passed']:
        raise ValueError(f'Plateau-average input/output error {errorPercent:.1f}% of signal swing exceeds {errorLimitPercent:g}% ({worstMv:.1f} mV)')
    return result


def summary(result, includeChecks=True):
    """Format the report; omit checks on screen if already printed as progress.

    Saved reports include all confirmations so they stand alone. This function
    is called only after analyze succeeds; failures retain partial checks instead.
    """
    checks = ''.join(message + '\n' for message in result['checks']) if includeChecks else ''
    return (f"Hardware smoke test: 30 seconds\n"
            f"Recorded: {result['seconds']:.3f} seconds\n"
            f"{checks}"
            f"Signal: {result['low']/1000:.3f} / {result['high']/1000:.3f} V; 29 level changes verified\n"
            f"Largest plateau-average error: {result['errorPercent']:.1f}% of signal swing (limit: {result['errorLimitPercent']:g}%)\n"
            f"PASS")
