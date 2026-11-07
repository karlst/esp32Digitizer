# Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
"""Pair saved copies of the same pulse; positive skew means GPIO1 was later.

Read analyze for validation and matching, tickDifference for timer wrap, then
summarize/writeReport for statistics and HTML. The input is a completed console
transcript from InputSkewTest, not live voltage samples. Both pins receive the
same generator signal. "Skew" is their relative recorded time difference; this
does not measure the ADC or a latency shared by both input paths.
"""
from collections import Counter
import re
import statistics


def tickDifference(first, second):
    """Return first minus second in ticks, choosing the signed difference at wrap.

    For example, first=0 and second=0xffffffff are one tick apart, not billions.
    Valid when the actual separation is less than half the 32-bit timer range,
    as it is for this sub-second test. No conversion to nanoseconds occurs here.
    """
    return ((first - second + 0x80000000) & 0xffffffff) - 0x80000000


def summarize(ticks):
    """Report observed quantization and repeatability without an accuracy claim."""
    values = [tick * 6.25 for tick in ticks]
    return {'count': len(values), 'meanNs': statistics.mean(values),
            'medianNs': statistics.median(values), 'stdevNs': statistics.pstdev(values),
            'minNs': min(values), 'maxNs': max(values),
            'histogramTicks': dict(sorted(Counter(ticks).items()))}


def analyze(lines):
    """Validate both edge streams before pairing matching physical transitions.

    Enabling two channels takes successive software calls. Permit one unmatched
    event at the beginning and one at the end, reporting those trims. Missing
    internal events, repeats, nonalternating polarities and wrong frequency fail.
    A 10-us pairing guard prevents pairing different half-cycles; it is not an
    accuracy tolerance. All actual signed differences are retained in the result.

    lines contains one run's console rows. Return raw pairs, boundary-trim counts
    and separate rising/falling summaries for JSON/HTML; raise ValueError on
    inconsistent evidence. No calibration is applied to the ADC delay files.
    """
    text = '\n'.join(lines)
    summaries = re.findall(r'^SKEW RESULT run=(\d+) status=(READY|FAIL) count1=(\d+) count6=(\d+) overflow=([01])$', text, re.M)
    records = re.findall(r'^SKEW EDGE run=(\d+) gpio=(1|6) index=(\d+) ticks=(\d+) rising=([01])$', text, re.M)
    if len(summaries) != 1 or len(records) != sum(line.startswith('SKEW EDGE ') for line in lines):
        raise ValueError('Missing, duplicated or malformed calibration records')
    run, status, count1, count6, overflow = summaries[0]
    if status != 'READY' or overflow != '0':
        raise ValueError('P4 capture overflowed or failed')
    channels = {1: [], 6: []}
    # Validate each pin's stream before comparing pins. Indexes must start at
    # zero and be contiguous; at about 1 kHz, opposite edges are 72000..88000
    # timer ticks apart (450..550 us). A dropped edge cannot be silently paired.
    for record in records:
        edgeRun, gpio, index, ticks, rising = map(int, record)
        channel = channels[gpio]
        if edgeRun != int(run) or index != len(channel) or ticks > 0xffffffff:
            raise ValueError('Mixed runs, missing records or invalid timer reading')
        if channel:
            elapsed = (ticks - channel[-1]['ticks']) & 0xffffffff
            if rising == channel[-1]['rising'] or not 72000 <= elapsed <= 88000:
                raise ValueError(f'GPIO{gpio}: missed/repeated pulse or input is not approximately 1 kHz, 50% duty')
        channel.append({'ticks': ticks, 'rising': rising})
    if len(channels[1]) != int(count1) or len(channels[6]) != int(count6):
        raise ValueError('Declared event counts do not match received data')
    if not all(100 <= len(channel) <= 512 for channel in channels.values()):
        raise ValueError('Too few events on one or both pins; check the shared pulse connection')

    # Find the common start using time, never arbitrary matching array indexes.
    # A channel enabled just after an edge can begin with the opposite polarity.
    first, second = channels[1], channels[6]
    iFirst = iSecond = 0
    initialDifference = tickDifference(first[0]['ticks'], second[0]['ticks'])
    if initialDifference > 1600:
        iSecond = 1
    elif initialDifference < -1600:
        iFirst = 1
    leadingTrim = iFirst + iSecond
    pairs = []
    while iFirst < len(first) and iSecond < len(second):
        # Once the starts align, advance both arrays together. Never search
        # ahead to hide an internal missing edge. 1600 ticks is the 10-us guard.
        a, b = first[iFirst], second[iSecond]
        difference = tickDifference(a['ticks'], b['ticks'])
        if a['rising'] != b['rising'] or abs(difference) > 1600:
            raise ValueError('The two inputs do not identify the same physical edge')
        pairs.append({'rising': bool(a['rising']), 'ticks1': a['ticks'],
                      'ticks6': b['ticks'], 'differenceTicks': difference})
        iFirst += 1
        iSecond += 1
    trailingTrim = len(first) - iFirst + len(second) - iSecond
    # Sequential channel disabling can leave one final edge on only one pin.
    # Report this boundary omission explicitly; two unmatched edges are refused.
    if trailingTrim > 1 or len(pairs) < 100:
        raise ValueError('Unequal capture streams beyond the allowed single boundary event')
    return {'run': int(run), 'pairs': pairs, 'leadingTrim': leadingTrim, 'trailingTrim': trailingTrim,
            'rising': summarize([pair['differenceTicks'] for pair in pairs if pair['rising']]),
            'falling': summarize([pair['differenceTicks'] for pair in pairs if not pair['rising']]),
            'sign': 'GPIO1 minus GPIO6; positive means GPIO1 recorded the edge later.',
            'limitation': 'Includes relative jumper-path delay and input thresholds for this waveform. '
                          'Common delay cancels. This is not absolute input latency or a full ADC timing calibration.'}


def writeReport(path, results):
    """Save a standalone summary and the distribution of tick differences."""
    pairs = [pair for result in results for pair in result['pairs']]
    rows = []
    distributions = []
    for label, polarity in [('Rising', True), ('Falling', False)]:
        summary = summarize([pair['differenceTicks'] for pair in pairs if pair['rising'] == polarity])
        rows.append(f'<tr><td>{label}</td><td>{summary["count"]}</td><td>{summary["meanNs"]:+.3f}</td>'
                    f'<td>{summary["medianNs"]:+.2f}</td><td>{summary["stdevNs"]:.2f}</td>'
                    f'<td>{summary["minNs"]:+.2f} to {summary["maxNs"]:+.2f}</td></tr>')
        distributions.append(f'<h2>{label}: counts at each timer difference</h2><table>'
                             '<tr><th>Difference (ticks)</th><th>Difference (ns)</th><th>Edges</th></tr>')
        for ticks, count in summary['histogramTicks'].items():
            distributions.append(f'<tr><td>{ticks:+d}</td><td>{ticks*6.25:+.2f}</td><td>{count}</td></tr>')
        distributions.append('</table>')
    path.write_text('<!doctype html><meta charset="utf-8"><title>P4 input timing difference</title>'
        '<style>body{font:17px system-ui;max-width:1000px;margin:40px auto;padding:20px}'
        'table{border-collapse:collapse}td,th{padding:10px;border-bottom:1px solid #ccc}</style>'
        '<h1>GPIO1 versus GPIO6 timing</h1><p>Same generator pulse on both inputs. '
        '<b>Positive means GPIO1 recorded the edge later.</b> One timer tick = 6.25 ns.</p>'
        '<table><tr><th>Edge</th><th>Count</th><th>Mean (ns)</th><th>Median (ns)</th>'
        '<th>Std dev (ns)</th><th>Range (ns)</th></tr>' + ''.join(rows) + '</table>'
        '<p>The mean can fall between timer ticks because it averages many observations. '
        'That does not establish accuracy finer than a tick. Relative cable delay and '
        'waveform-dependent input thresholds are included; common input delay cancels.</p>'
        '<p>This test does not change the saved ADC delay estimate. Rising-edge skew is '
        'the relevant comparison for the earlier rising-input/FSYNC measurement. '
        'Falling-edge results are an additional diagnostic; FSYNC was timestamped on its rising edge.</p>'
        + ''.join(distributions), encoding='utf-8')
