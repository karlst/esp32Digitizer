# Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
"""Validate one completed FSYNC diagnostic transcript without touching hardware.

Read analyze in three stages: check run/startup counts, check each pulse-to-word
pair, then recompute timing from the endpoints and check interior observations.
FSYNC is the ADC frame-start signal. An anchor pairs its hardware pulse number
and timer count with a retained ADC word. This module is also the prerequisite
validator for pulseDelayReport: waveform timing is meaningless if those pairs
are inconsistent. The runner supplies console lines; this file never reads a port.
"""
import math
import re


def analyze(lines):
    """Check one run's counts and independently recompute timestamp spacing.

    ADC word indexes are zero-based; hardware pulse counts are one-based.
    Reported PASS is necessary but not sufficient. This detects malformed,
    mixed-run and inconsistent reports. It cannot establish an absolute first
    sample number independently of the firmware's stopped-input startup checks.

    Return a JSON-ready dictionary with accepted anchors and counts; ValueError
    rejects the run. word is eight hex characters (status plus signed sample),
    ticks is a 32-bit counter, and sample is an index into retained RAM.
    """
    text = '\n'.join(lines)
    results = re.findall(
        r'^FSYNC RESULT run=(\d+) status=(PASS|FAIL) anchors=(\d+) '
        r'ticksPerSample=([\d.]+) residualTicks=([\d.]+) startup=([01]) clean=([01])$', text, re.M)
    counts = re.findall(
        r'^FSYNC COUNTS run=(\d+) hardware=(\d+) frames=(\d+) words=(\d+) tail=(-?\d+) rejected=(\d+)$',
        text, re.M)
    alignment = re.findall(
        r'^FSYNC ALIGN run=(\d+) discarded=(\d+) clocks=(\d+) firstPulse=(\d+)$', text, re.M)
    anchors = re.findall(
        r'^FSYNC ANCHOR run=(\d+) sample=(\d+) pulse=(\d+) ticks=(\d+) word=([0-9a-fA-F]{8})$', text, re.M)
    if len(results) != 1 or len(counts) != 1 or len(alignment) != 1:
        raise ValueError('Missing, duplicated or damaged FSYNC result/count report')
    run, status, declared, slopeText, residualText, startup, clean = results[0]
    # One startup frame is intentionally omitted, whether it has 31 or 32
    # captured clocks. Therefore the first retained word belongs to pulse 2.
    # A clean receiver report and this alignment must both be present.
    countRun, hardware, frames, words, tail, rejected = map(int, counts[0])
    run = int(run)
    alignmentRun, discarded, startupClocks, firstPulse = map(int, alignment[0])
    if alignmentRun != run or discarded != 1 or startupClocks not in (31, 32) or firstPulse != discarded + 1:
        raise ValueError('Startup discard and first retained pulse disagree')
    if status != 'PASS' or startup != '1' or clean != '1':
        raise ValueError('Board rejected FSYNC association; see p4.txt')
    if countRun != run or any(int(anchor[0]) != run for anchor in anchors):
        raise ValueError('FSYNC records belong to different runs')
    # Hardware can count final frames still in an undelivered DMA memory batch
    # at shutdown. Keep that tail explicit and bounded; it does not become
    # retained data. A frame counter can also include one unfinished last frame.
    if words < 32768 or frames not in (words + discarded, words + discarded + 1) or hardware - frames != tail or not 0 <= tail <= 512:
        raise ValueError('Hardware and decoded sample counts disagree')
    if len(anchors) != int(declared) or len(anchors) < 4:
        raise ValueError('Too few or missing matched timestamps')
    samples = [int(anchor[1]) for anchor in anchors]
    # Verify absolute pulse-to-word mapping as well as spacing. Spacing alone
    # cannot detect every word being shifted by the same one-sample offset.
    if any(int(anchor[2]) != int(anchor[1]) + firstPulse for anchor in anchors):
        raise ValueError('Hardware pulse does not identify the reported retained word')
    ticks = [int(anchor[3]) for anchor in anchors]
    if any(sample >= min(words, 65536) for sample in samples) or any(tick > 0xffffffff for tick in ticks):
        raise ValueError('Timestamp points outside the retained words or timer range')
    for iAnchor in range(1, len(anchors)):
        # Unsigned subtraction handles the timer wrapping through zero. The
        # sub-second bound rejects reversed timestamps disguised by that wrap.
        elapsed = (ticks[iAnchor] - ticks[iAnchor - 1]) & 0xffffffff
        if samples[iAnchor] <= samples[iAnchor - 1] or not 0 < elapsed < 160000000:
            raise ValueError('Repeated or reversed timestamp/sample number')
    span = samples[-1] - samples[0]
    # For example, 1024 sample intervals spanning 160000 ticks give 156.25
    # ticks/sample. Residual is the largest difference between an observed time
    # and the straight line joining the endpoints. Four ticks is a consistency
    # threshold (25 ns), not a guarantee of absolute timing accuracy.
    slope = ((ticks[-1] - ticks[0]) & 0xffffffff) / span
    residual = max(abs(((tick - ticks[0]) & 0xffffffff) - (sample - samples[0]) * slope)
                   for tick, sample in zip(ticks, samples))
    if span < 1024 or not 156.0 < slope < 156.5 or residual > 4.0:
        raise ValueError('Timestamp spacing does not agree with sample numbering')
    if not math.isclose(slope, float(slopeText), abs_tol=0.000001) or abs(residual - float(residualText)) > 0.001:
        raise ValueError('Printed timing summary disagrees with the saved timestamps')
    return {'run': run, 'passed': True, 'hardwarePulses': hardware, 'decodedWords': words,
            'discardedStartupFrames': discarded, 'startupFrameClocks': startupClocks,
            'firstRetainedPulse': firstPulse,
            'undeliveredTailFrames': tail, 'rejectedSnapshots': rejected,
            'ticksPerSample': slope, 'maxResidualTicks': residual,
            'anchors': [{'sample': int(anchor[1]), 'pulse': int(anchor[2]),
                         'ticks': int(anchor[3]), 'word': anchor[4]}
                        for anchor in anchors],
            'limitation': 'Consistency under checked startup; no absolute time or ADC-delay calibration.'}
