# Copyright (C) 2026 Karl Stock
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare 100 ms averages read from SD with the scope low/high voltage entries.

The generator runs independently of recording. analyzeWaveform locates the
changes between low and high levels, then excludes nearby sections from voltage
comparisons. A bin is a 100 ms group of samples represented by its average.
Variation between those averages does not describe individual-sample noise.
No gain or offset correction is applied to make the ADC agree with the scope.
"""
import html
import math
import statistics
from pathlib import Path

binSeconds = 0.1
edgeMarginBins = 2
minimumPlateauBins = 3
levelSeconds = 1.0
timingToleranceSeconds = 0.2
errorLimitPercent = 10.0


def validateLevels(levels):
    """Reject invalid references before the runner opens ports or starts a run."""
    low, high = levels
    if not all(math.isfinite(value) for value in levels) or not 0 <= low < high < 2.5:
        raise ValueError('Scope readings must be finite, ordered, and between 0 and 2.5 V')
    if high - low < 0.15:
        raise ValueError('This square-wave test needs at least 0.15 V between levels')


def analyzeWaveform(outputs, levels, result, plotPath=None):
    """Add low/high voltage and timing checks to the recording results.

    outputs maps consecutive 100 ms section indices to mean millivolts; levels
    contains the scope readings in volts. A plateau is a flat high or low section.
    Exclude two bins at each end, including the start and end of the recording.
    The allowed voltage difference is 10% of the entered high-to-low swing. This
    is a functional check, not a precision specification. Return failed results
    too, so the operator can inspect their graph.
    """
    validateLevels(levels)
    low, high = (value * 1000 for value in levels)
    indices = list(outputs)
    if len(indices) < 20 or indices != list(range(indices[0], indices[-1] + 1)):
        raise ValueError('External waveform needs consecutive retained disk bins')
    values = list(outputs.values())
    midpoint = (low + high) / 2
    states = [value > midpoint for value in values]
    transitions = [iBin for iBin in range(1, len(values)) if states[iBin] != states[iBin - 1]]
    boundaries = [0] + transitions + [len(values)]
    plateaus = []
    settled = {False: [], True: []}
    settledBins = {'low': [], 'high': []}

    # Capture may begin at any phase. Full intervals between observed edges
    # should last one second; do not impose the old start-low convention.
    intervals = [(right - left) * binSeconds for left, right in zip(transitions, transitions[1:])]
    expectedChanges = (len(values) - 1) * binSeconds / levelSeconds
    timingGood = (len(transitions) >= 2 and abs(len(transitions) - expectedChanges) <= 1.1 and
                  all(abs(duration - levelSeconds) <= timingToleranceSeconds + 1e-9 for duration in intervals))
    for iBinStart, iBinEnd in zip(boundaries, boundaries[1:]):
        # A capture can begin just before an edge, leaving only one bin in its
        # first plateau. Reject short sections before slicing: a negative slice
        # endpoint means "from the end of the recording" in Python, not empty.
        if iBinEnd - iBinStart >= 2 * edgeMarginBins + minimumPlateauBins:
            plateau = values[iBinStart + edgeMarginBins:iBinEnd - edgeMarginBins]
            state = states[iBinStart]
            settled[state].extend(plateau)
            settledBins['high' if state else 'low'].extend(indices[iBinStart + edgeMarginBins:iBinEnd - edgeMarginBins])
            measured = statistics.mean(plateau)
            reference = high if state else low
            plateaus.append(dict(second=(indices[0] + (iBinStart + iBinEnd) / 2) * binSeconds,
                                 high=state, meanMv=measured, errorMv=measured-reference))

    measurements = {}
    for state, name, reference in ((False, 'low', low), (True, 'high', high)):
        data = settled[state]
        measurements[name] = (dict(meanMv=statistics.mean(data),
                                   errorMv=statistics.mean(data)-reference,
                                   stddevMv=statistics.pstdev(data),
                                   spanMv=max(data)-min(data), bins=len(data)) if data else None)
    complete = all(measurements.values())
    worstMv = max((abs(plateau['errorMv']) for plateau in plateaus), default=None)
    errorPercent = worstMv * 100 / (high-low) if worstMv is not None else None
    passed = complete and timingGood and errorPercent <= errorLimitPercent
    failures = []
    if not complete:
        failures.append('Both settled voltage levels were not found')
    if not timingGood:
        failures.append('Expected one-second levels were not observed consistently')
    if errorPercent is not None and errorPercent > errorLimitPercent:
        failures.append('Plateau error exceeds 10% of the scope-measured swing')
    result.update(external=True, low=low, high=high, measurements=measurements,
                  transitions=len(transitions), timingGood=timingGood, plateaus=plateaus, settledBins=settledBins,
                  worstMv=worstMv, errorPercent=errorPercent, errorLimitPercent=errorLimitPercent,
                  calibration='Manually entered scope levels; not a precision calibration',
                  passed=bool(passed), failures=failures,
                  waveform=[dict(second=(iBin+.5)*binSeconds, volts=outputs[iBin]/1000) for iBin in indices])
    if plotPath is not None:
        writePlot(result, plotPath)
    return result


def summarize(result, includeChecks=True):
    """Describe signed level error and averaged-bin variation without claiming raw noise."""
    lines = ['External-generator recording: {:.3f} seconds'.format(result['seconds'])]
    if includeChecks:
        lines.extend(result['checks'])
    for name in ('low', 'high'):
        measured = result['measurements'][name]
        if measured is not None:
            lines.append(f"{name.capitalize()}: scope {result[name]/1000:.4f} V; ADC {measured['meanMv']/1000:.4f} V; "
                         f"ADC minus scope {measured['errorMv']:+.2f} mV; "
                         f"100-ms-bin standard deviation {measured['stddevMv']:.3f} mV; "
                         f"range {measured['spanMv']:.3f} mV")
    low = result['measurements']['low']
    high = result['measurements']['high']
    if low and high:
        swing = high['meanMv'] - low['meanMv']
        lines.append(f"ADC swing {swing:.3f} mV; scope swing {result['high']-result['low']:.3f} mV")
    if result['worstMv'] is not None:
        lines.append(f"Worst plateau error {result['worstMv']:.3f} mV ({result['errorPercent']:.2f}% of scope swing; limit 10%)")
    lines.append(f"Observed level changes: {result['transitions']}; one-second timing check: {'PASS' if result['timingGood'] else 'FAIL'}")
    if 'sampleStatistics' in result:
        from sampleNoiseReport import summarizeSamples
        lines.extend(summarizeSamples(result))
    lines.extend(result['failures'])
    lines.append('PASS' if result['passed'] else 'FAIL')
    return '\n'.join(lines)


def writePlot(result, target):
    """Save all retained bin averages and two constant scope-reference lines as HTML."""
    points = result['waveform']
    start = points[0]['second'] - binSeconds/2
    end = points[-1]['second'] + binSeconds/2
    volts = [point['volts'] for point in points] + [result['low']/1000, result['high']/1000]
    minimum, maximum = min(volts)-.05, max(volts)+.05

    def position(second, voltage):
        """Use the same time and voltage axes for recorded data and reference lines."""
        return 90 + 1000*(second-start)/(end-start), 30 + 350*(maximum-voltage)/(maximum-minimum)

    parts = []
    for iTick in range(6):
        voltage = minimum + (maximum-minimum)*iTick/5
        _, y = position(start, voltage)
        parts.append(f'<line x1="90" y1="{y}" x2="1090" y2="{y}" stroke="#ddd"/><text x="10" y="{y}">{voltage:.3f} V</text>')
        second = start + (end-start)*iTick/5
        x, _ = position(second, minimum)
        parts.append(f'<text x="{x}" y="410" text-anchor="middle">{second:.1f} s</text>')
    for name in ('low', 'high'):
        _, y = position(start, result[name]/1000)
        parts.append(f'<line x1="90" y1="{y}" x2="1090" y2="{y}" stroke="#166ac1" stroke-dasharray="7 5"><title>Scope {name}: {result[name]/1000:.4f} V</title></line>')
    coordinates = ' '.join('{:.2f},{:.2f}'.format(*position(point['second'], point['volts'])) for point in points)
    parts.append(f'<polyline points="{coordinates}" stroke="#d76b0b" stroke-width="2" fill="none"/>')
    for point in points:
        x, y = position(point['second'], point['volts'])
        parts.append(f'<circle cx="{x}" cy="{y}" r="2" fill="#d76b0b"><title>{point["second"]:.2f} s: {point["volts"]:.6f} V</title></circle>')
    from sampleNoiseReport import plotSamples
    samplePlots = plotSamples(result) if 'sampleStatistics' in result else ''
    document = f'''<!doctype html><html lang="en"><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>External generator: ADC recording</title>
<style>body{{font:17px system-ui;max-width:1200px;margin:32px auto;padding:20px;color:#172536}}svg{{width:100%}}pre{{white-space:pre-wrap}}text{{font:14px system-ui}}</style>
<h1>External generator: ADC recording</h1>
<p>Orange: 100 ms averages read back from SD. Dashed blue: scope readings entered by the operator.</p>
<svg viewBox="0 0 1140 440" role="img" aria-label="Recorded ADC waveform and scope voltage references">{''.join(parts)}</svg>
<pre>{html.escape(summarize(result))}</pre>
{samplePlots}
<p>Voltage comparison excludes two bins near each detected transition and capture boundary.
Lines between points do not represent measured edge shape. Reported variation is variation of
100 ms averages, not individual-sample ADC noise. No fitted gain or voltage offset.</p>
<p>Scope references are manually entered, not a simultaneous scope trace. ADS127 conversion assumes
a 2.5 V reference and unity gain. Differences include scope, probe, ADC reference and input-path errors;
this is not a precision calibration. Only the retained window shown is readback-verified.</p></html>'''
    Path(target).write_text(document, encoding='utf-8')
