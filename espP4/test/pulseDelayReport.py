# Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
"""Analyze direct-pulse timestamps and individual ADC readings after capture.

Read analyze -> measureEdge -> writeReport. No serial access or extra numerical
packages. Report response-center delay and interpolated half-height delay, not
an absolute time calibration. Raw logs remain the source evidence.

The generator goes directly to GPIO6 and through a divider to the ADC. Firmware
exports forty ADC readings near each direct edge (a voltage transition). This
module uses saved FSYNC frame-start timestamps to assign times to those readings.
The horizontal axis is nanoseconds after the direct GPIO6 edge, not time since
the PC received the data. No function here starts hardware or changes calibration.
"""
import html
import re
import statistics
from fsyncAssociationReport import analyze as analyzeAssociation


def measureEdge(samples, times, rising):
    """Measure one settled step in volts versus ns after the direct GPIO edge.

    The first/last eight readings define the two plateaus. Normalize the step
    from zero to one for either polarity. The response center is the weighted
    mean time of successive voltage changes; signed changes retain ringing.
    This estimates the first moment of the step derivative (group delay at low
    frequency for a linear system). Discrete sampling and finite windows limit
    it. Half-height interpolation is reported separately for comparison.

    analyze supplies forty voltage readings, forty corresponding times in ns,
    and the direct edge's polarity. Return both delay estimates and the arrays
    needed to draw that transition. Raise ValueError for an unsuitable waveform;
    a numerical result does not by itself establish measurement accuracy.
    """
    # Estimate the settled levels independently for this edge. Dividing by a
    # negative swing reverses falling edges, so both polarities plot 0% -> 100%.
    before = statistics.mean(samples[:8])
    after = statistics.mean(samples[-8:])
    swing = after - before
    if abs(swing) < 0.1 or (swing > 0) != rising:
        raise ValueError('ADC transition is absent, too small, or opposite to the direct pulse')
    if max(statistics.pstdev(samples[:8]), statistics.pstdev(samples[-8:])) > abs(swing) * 0.01:
        raise ValueError('ADC plateaus are not settled within the exported window')
    normalized = [(sample - before) / swing for sample in samples]
    if min(normalized) < -0.15 or max(normalized) > 1.15:
        raise ValueError('Large overshoot or another transition contaminates the ADC window')
    crossings = []
    # Locate adjacent readings on either side of halfway. Interpolate along
    # their connecting line; the ADC did not measure an extra point there.
    for iSample in range(1, len(samples)):
        if normalized[iSample - 1] < 0.5 <= normalized[iSample]:
            fraction = (0.5 - normalized[iSample - 1]) / (normalized[iSample] - normalized[iSample - 1])
            crossings.append(times[iSample - 1] + fraction * (times[iSample] - times[iSample - 1]))
    if len(crossings) != 1:
        raise ValueError('ADC response has no unique half-height crossing')

    # Only plateau-to-plateau changes enter the moment. Keep signed weights:
    # taking absolute values would bias the result when a response rings.
    # Treat each change as occurring halfway between its two sample times.
    # Sum(time * change) / sum(change) is the response's weighted time center.
    # Indexes 8..32 connect the two plateau regions without including the
    # outermost samples, where settled noise would add unnecessary weighting.
    changes = [normalized[i] - normalized[i - 1] for i in range(8, 33)]
    total = sum(changes)
    if not 0.95 <= total <= 1.05:
        raise ValueError('ADC step is incomplete inside the response-center window')
    center = sum(change * (times[i] + times[i - 1]) / 2
                 for i, change in zip(range(8, 33), changes)) / total
    if not 0 <= center <= 20000 or not 0 <= crossings[0] <= 20000:
        raise ValueError('Apparent delay is outside the 0–20 us measurement window')
    return {'centerNs': center, 'halfHeightNs': crossings[0], 'beforeV': before,
            'afterV': after, 'timesNs': times, 'normalized': normalized}


def summarize(values):
    """Return count and descriptive statistics for a nonempty list of delays in ns.

    Population standard deviation describes the spread of these observed edges,
    not uncertainty in the mean or absolute accuracy against an external clock.
    """
    return {'count': len(values), 'meanNs': statistics.mean(values),
            'medianNs': statistics.median(values), 'stdevNs': statistics.pstdev(values),
            'minNs': min(values), 'maxNs': max(values)}


def analyze(lines, frequencyHz=997):
    """Require clean FSYNC association and alternating edges at frequencyHz.

    Both paths share 6.25-ns timer ticks. Interpolate the sample-time relationship
    between the first and last FSYNC anchors, retaining its measured clock ratio.
    Only edges within that bracket are accepted. GPIO threshold, input circuits,
    translator propagation, wiring and sample interpolation remain uncalibrated.

    The runner passes console lines from one run and the expected generator
    frequency in Hz. Return a dictionary saved directly as JSON and consumed by
    writeReport. ValueError rejects incomplete evidence rather than estimating
    from a partial or inconsistent stream. The 2553-ns operational correction
    and the separate input-skew calibration are not subtracted in this report.
    """
    # Only these rates leave settled plateaus inside each forty-sample window.
    # Ten percent spacing tolerance detects wrong settings and missed edges.
    if frequencyHz not in (997, 1000, 10000):
        raise ValueError('Supported pulse frequencies are 997, 1000 and 10000 Hz')
    expectedIntervalNs = 500000000 / frequencyHz
    association = analyzeAssociation(lines)
    # Association is validated first: a plausible waveform cannot repair an
    # incorrect mapping of FSYNC pulses to stored ADC words. Require exactly
    # one DELAY RESULT row and a complete set of fixed-width DELAY EDGE rows.
    text = '\n'.join(lines)
    results = re.findall(r'^DELAY RESULT run=(\d+) status=(READY|FAIL) captured=(\d+) exported=(\d+) overflow=([01])$', text, re.M)
    records = re.findall(r'^DELAY EDGE run=(\d+) index=(\d+) ticks=(\d+) rising=([01]) start=(\d+) words=([0-9a-fA-F]{320})$', text, re.M)
    edgeLines = [line for line in lines if line.startswith('DELAY EDGE ')]
    if len(results) != 1:
        raise ValueError('Missing or duplicated pulse-delay summary')
    run, status, captured, exported, overflow = results[0]
    if int(run) != association['run'] or status != 'READY' or overflow != '0':
        raise ValueError('Direct-pulse capture failed or overflowed')
    if len(records) != len(edgeLines) or len(records) != int(exported) or not 20 <= len(records) <= 128 or not len(records) <= int(captured) <= 4096:
        raise ValueError('Missing direct edges or ADC windows')
    first, last = association['anchors'][0], association['anchors'][-1]
    # Firmware's 32-bit counter wraps at 2^32. Masking the subtraction gives
    # elapsed ticks through one wrap. The short capture is far below 2^32 ticks.
    span = (last['ticks'] - first['ticks']) & 0xffffffff
    slope = association['ticksPerSample']
    edges = []
    previous = None
    intervals = []
    for record in records:
        edgeRun, index, ticks, rising, start = map(int, record[:5])
        elapsed = (ticks - first['ticks']) & 0xffffffff
        if edgeRun != int(run) or ticks > 0xffffffff or elapsed > span or index >= int(captured):
            raise ValueError('Direct edge is outside this run or the FSYNC timing bracket')
        if previous is not None:
            # Each opposite-polarity edge should arrive half a generator period
            # later. Consecutive indexes also reject a missing console row even
            # if its omission could otherwise look like a valid waveform.
            intervalNs = ((ticks - previous[1]) & 0xffffffff) * 6.25
            if index != previous[0] + 1 or rising == previous[2] or not 0.9 * expectedIntervalNs <= intervalNs <= 1.1 * expectedIntervalNs:
                raise ValueError(f'Missing/repeated edges or source is not approximately {frequencyHz} Hz, 50% duty')
            intervals.append(intervalNs)
        previous = (index, ticks, rising)
        if start + 40 > min(association['decodedWords'], 65536):
            raise ValueError('ADC window exceeds retained data')
        raw = [int(record[5][i:i + 8], 16) for i in range(0, 320, 8)]
        # Each eight hex characters encode a four-byte word: 0x08 status followed
        # by a signed, two's-complement 24-bit ADC reading. Subtract 2^24 when
        # its sign bit is set, then scale by the configured 2.5-V reference.
        if any((word >> 24) != 0x08 for word in raw):
            raise ValueError('ADC window contains an abnormal status byte')
        samples = []
        for word in raw:
            value = word & 0xffffff
            if value & 0x800000:
                value -= 0x1000000
            samples.append(value * 2.5 / 8388608)
        # start+i is this reading's retained-word index. Relative to the first
        # FSYNC anchor, multiply its sample distance by measured ticks/sample,
        # subtract the direct edge's elapsed ticks, then convert ticks to ns.
        # Thus zero is the direct edge, and positive time means later FSYNC.
        times = [((start + i - first['sample']) * slope - elapsed) * 6.25 for i in range(40)]
        if not times[7] < -1000 or not times[32] > 18000:
            raise ValueError('ADC window is misplaced relative to the direct edge')
        edge = measureEdge(samples, times, bool(rising))
        edge.update({'index': index, 'rising': bool(rising), 'ticks': ticks, 'firstSample': start})
        edges.append(edge)
    rising = [edge['centerNs'] for edge in edges if edge['rising']]
    # Keep the polarities separate: threshold or analog asymmetry can affect
    # rising and falling measurements differently. The measured source rate
    # uses the average half-period of the exported consecutive edges only.
    falling = [edge['centerNs'] for edge in edges if not edge['rising']]
    if min(len(rising), len(falling)) < 8:
        raise ValueError('Too few transitions of each polarity')
    return {'run': int(run), 'association': association, 'edges': edges,
            'requestedFrequencyHz': frequencyHz,
            'measuredFrequencyHz': 500000000 / statistics.mean(intervals),
            'rising': summarize(rising), 'falling': summarize(falling),
            'halfHeight': summarize([edge['halfHeightNs'] for edge in edges]),
            'limitation': 'Measured path delay relative to GPIO6, including analog and FSYNC translator paths. '
                          'Not chip-only filter delay or nanosecond absolute accuracy. '
                          'Direct GPIO threshold, wiring, divider/translator and interpolation are uncalibrated.'}


def writeReport(path, results):
    """Write a standalone graph of individual normalized ADC transitions.

    Zero on the horizontal axis is the direct hardware edge. Each thin line
    contains individual readings, not averaged waveform bins. Rising and falling
    steps are normalized to the same zero-to-one direction for comparison.

    path is an HTML output file; results is a nonempty list returned by analyze.
    This overwrites only that report, retaining the separate raw serial logs.
    All drawing uses embedded SVG, so opening the file needs no external library.
    """
    edges = [edge for result in results for edge in result['edges']]
    rows = []
    for label, rising in (('Rising', True), ('Falling', False)):
        summary = summarize([edge['centerNs'] for edge in edges if edge['rising'] == rising])
        rows.append(f'<tr><td>{label}</td><td>{summary["count"]}</td>'
                    f'<td>{summary["medianNs"] / 1000:.4f}</td><td>{summary["stdevNs"]:.1f}</td>'
                    f'<td>{summary["minNs"] / 1000:.4f} – {summary["maxNs"] / 1000:.4f}</td></tr>')
    traces = []
    # Map -5..15 us horizontally to x=80..880, and normalized 0..1 vertically
    # to y=340..100. SVG's y axis grows downward. Low opacity keeps overlapping
    # individual traces visible without replacing them with an averaged curve.
    for edge in edges:
        points = ' '.join(f'{80 + (t / 1000 + 5) * 40:.2f},{340 - y * 240:.2f}'
                          for t, y in zip(edge['timesNs'], edge['normalized']) if -5 <= t / 1000 <= 15)
        color = '#136fa6' if edge['rising'] else '#d26b18'
        traces.append(f'<polyline points="{points}" fill="none" stroke="{color}" opacity="0.18"/>')
    ticks = ''.join(f'<text x="{80 + (t + 5) * 40}" y="390" text-anchor="middle">{t}</text>'
                    for t in (-5, 0, 5, 10, 15))
    path.write_text('<!doctype html><meta charset="utf-8"><title>ADC path delay</title>'
        '<style>body{font:17px system-ui;max-width:1000px;margin:40px auto;padding:20px} '
        'table{border-collapse:collapse}td,th{padding:12px;border-bottom:1px solid #ccc}</style>'
        '<h1>ADC path delay</h1><p>Response-center delay relative to the direct pulse on GPIO6.</p>'
        '<table><tr><th>Edge</th><th>Count</th><th>Median (µs)</th><th>Std dev (ns)</th><th>Range (µs)</th></tr>'
        + ''.join(rows) + '</table><p>Requested source: '
        + str(results[0].get('requestedFrequencyHz', 997)) + ' Hz. Measured per trial: '
        + ', '.join(f'{result["measuredFrequencyHz"]:.2f} Hz' for result in results if 'measuredFrequencyHz' in result)
        + '.</p><p>' + html.escape(results[0]['limitation']) + '</p>'
        '<svg viewBox="0 0 1000 440" role="img" aria-label="Individual ADC step responses after direct pulse">'
        '<path d="M80 70V365H880 M280 70V365" fill="none" stroke="#777"/>'
        '<text x="20" y="100">100%</text><text x="30" y="340">0%</text>'
        + ''.join(traces) + ticks + '<text x="480" y="425" text-anchor="middle">Microseconds after direct edge</text></svg>'
        '<p>Blue: rising edges. Orange: falling edges, inverted for comparison. Each trace uses individual '
        'ADC samples. The vertical line at zero is the direct pulse, not an analog scope measurement.</p>',
        encoding='utf-8')
