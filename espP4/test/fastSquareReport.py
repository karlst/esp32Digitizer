# Copyright (C) 2026 Karl Stock
# SPDX-License-Identifier: GPL-3.0-or-later
"""Check the 1 kHz square wave using 5 ms of samples read back from SD.

hardwareSmokeRetainedReport calls analyzeFastSquare after checking the recording
and its readback. readWaveform reads the sample rows printed by the P4;
analyzeFastSquare finds voltage transitions and measures the flat sections;
writePlot draws the individual samples and enlarged transitions.

A sample is one ADC reading. An edge is a change between the low and high levels.
These results include the generator, wiring and ADC response. The scope supplies
only two manually entered reference voltages, not a simultaneous waveform.
Frequency uses the configured sample rate; it is not an independent clock check.
"""
import html
import math
import re
import statistics
from pathlib import Path
from externalGeneratorReport import validateLevels

expectedSamples = 5120
chunkSamples = 64
targetFrequencyHz = 1000
frequencyTolerance = 0.05
plateauErrorLimit = 0.10
settlingFraction = 0.02


def readWaveform(text, rate, firstRetainedSample):
    """Read the P4 sample rows and return their starting position and voltages.

    Require 5,120 samples, sent in ordered rows of 64, starting one second into
    the data still on SD. rate is samples per second; firstRetainedSample is the
    position of the oldest remaining sample. Raise ValueError for missing or
    inconsistent rows. Convert signed 24-bit readings using a 2.5 V reference."""
    from hardwareSmokeReport import single
    first, count, reportedRate = map(int, single(
        r'^SMOKE_RAW_BEGIN version=1 firstSample=(\d+) count=(\d+) rate=(\d+)$', text, 'Raw waveform header'))
    if count != expectedSamples or reportedRate != rate or first != firstRetainedSample + rate:
        raise ValueError('Raw waveform size, rate or position is incorrect')
    codes = []
    for line in text.splitlines():
        if line.startswith('SMOKE_RAW,'):
            fields = line.split(',')
            if len(fields) != chunkSamples + 2 or int(fields[1]) != len(codes):
                raise ValueError('Raw waveform chunks are missing, duplicated or out of order')
            values = list(map(int, fields[2:]))
            if any(value <= -8388608 or value >= 8388607 for value in values):
                raise ValueError('Raw waveform contains clipped or invalid codes')
            codes.extend(values)
    if len(codes) != count or int(single(r'^SMOKE_RAW_END count=(\d+)$', text, 'Raw waveform end')) != count:
        raise ValueError('Raw waveform is incomplete')
    return first, [code * 2.5 / 8388608 for code in codes]


def edgeDetails(values, edges, highStates, low, high, rate):
    """Measure excursions and settling after each detected voltage transition.

    low and high are the ADC's measured flat-section means, in volts. For each
    complete interval between transitions, find the last sample more than 2% of
    the measured voltage swing away from the destination level. The following
    sample marks settling; return None if there is no such sample in the search.
    Exclude the final 10% of the interval to avoid including the next transition.

    Times start at the detected transition, not at the generator's actual edge,
    and have one-sample resolution. Excursions include noise; they do not by
    themselves establish ringing or the ADC's response time.
    """
    details = []
    band = (high-low) * settlingFraction
    for iEdge in range(len(edges)-1):
        iStart, iEnd = edges[iEdge], edges[iEdge+1]
        isHigh = highStates[iEdge]
        target = high if isHigh else low
        iStop = iEnd - max(1, (iEnd-iStart)//10)
        outside = [iSample for iSample in range(iStart,iStop) if abs(values[iSample]-target) > band]
        settledAt = (outside[-1]+1 if outside else iStart)
        settlingUs = (settledAt-iStart)*1e6/rate if settledAt < iStop else None
        section = values[iStart:iStop]
        excursion = max(0, max(section)-high) if isHigh else max(0, low-min(section))
        details.append(dict(sample=iStart, rising=isHigh, settlingUs=settlingUs,
                            excursionMv=excursion*1000, bandMv=band*1000))
    return details


def analyzeFastSquare(text, levels, result, rate, firstRetainedSample, plotPath=None):
    """Add the short-window waveform results to result and optionally draw a graph.

    levels contains the entered scope low/high voltages. Detect rising edges at
    75% of that swing and falling edges at 25%; separate thresholds keep small
    fluctuations from being counted as extra transitions. Check frequency,
    high/low durations and flat-section means. Return failed waveform results
    too, so the operator can inspect the graph. Malformed input raises an error."""
    validateLevels(levels)
    first, values = readWaveform(text, rate, firstRetainedSample)
    scopeLow, scopeHigh = levels
    swing = scopeHigh-scopeLow
    lowerGate, upperGate = scopeLow+.25*swing, scopeLow+.75*swing
    highState = values[0] > (scopeLow+scopeHigh)/2
    edges, highStates = [], []
    for iSample, voltage in enumerate(values[1:],1):
        if (not highState and voltage >= upperGate) or (highState and voltage <= lowerGate):
            highState = not highState
            edges.append(iSample)
            highStates.append(highState)

    # Use the middle half of each complete high or low section for voltage and
    # noise statistics. The generator may already be high when recording starts;
    # only sections bounded by two observed transitions are included.
    settled = {False: [], True: []}
    plateauErrors = []
    widths = []
    for iEdge in range(len(edges)-1):
        iStart, iEnd = edges[iEdge], edges[iEdge+1]
        width = iEnd-iStart
        widths.append(width/rate)
        margin = width//4
        plateau = values[iStart+margin:iEnd-margin]
        if plateau:
            state = highStates[iEdge]
            settled[state].extend(plateau)
            plateauErrors.append(abs(statistics.mean(plateau)-(scopeHigh if state else scopeLow)))
    rising = [iSample for iSample,state in zip(edges,highStates) if state]
    periods = [(right-left)/rate for left,right in zip(rising,rising[1:])]
    frequency = 1/statistics.mean(periods) if periods else None
    complete = bool(settled[False] and settled[True])
    frequencyGood = frequency is not None and abs(frequency/targetFrequencyHz-1) <= frequencyTolerance
    widthsGood = bool(widths) and all(abs(width*2*targetFrequencyHz-1) <= .10 for width in widths)
    failures = []
    if not complete or not 8 <= len(edges) <= 11 or not frequencyGood or not widthsGood:
        failures.append('Recorded window does not contain the expected 1 kHz, approximately 50% square wave')
    worst = max(plateauErrors, default=None)
    if worst is not None and worst > swing*plateauErrorLimit:
        failures.append('Settled-level error exceeds 10% of the scope-measured swing')
    measurements = {}
    for state,name,reference in ((False,'low',scopeLow),(True,'high',scopeHigh)):
        samples = settled[state]
        measurements[name] = (dict(meanV=statistics.mean(samples), errorMv=(statistics.mean(samples)-reference)*1000,
                                   stddevUv=statistics.pstdev(samples)*1e6, count=len(samples)) if samples else None)
    details = edgeDetails(values,edges,highStates,measurements['low']['meanV'],measurements['high']['meanV'],rate) if complete else []
    result.update(fastSquare=True, external=True, passed=not failures, failures=failures,
                  calibration='Manually entered scope levels; ADC conversion assumes 2.5 V reference',
                  frequencyHz=frequency, edges=details, transitions=len(edges), measurements=measurements,
                  scopeLow=scopeLow, scopeHigh=scopeHigh, waveformVolts=values, sampleRate=rate,
                  firstSample=first, windowSeconds=len(values)/rate, worstMv=worst*1000 if worst is not None else None)
    if plotPath is not None:
        writePlot(result,plotPath)
    return result


def summarize(result, includeChecks=True):
    """Return printable results, distinguishing the 5 ms waveform from the full recording."""
    lines = [f"1 kHz external-generator test: {result['seconds']:.3f} seconds recorded; 5 ms waveform analyzed"]
    if includeChecks:
        lines.extend(result['checks'])
    if result['frequencyHz'] is not None:
        lines.append(f"Recorded-window frequency: {result['frequencyHz']:.2f} Hz (uses configured ADC sample rate)")
    for name,measurement in result['measurements'].items():
        if measurement:
            lines.append(f"{name.capitalize()} settled: {measurement['meanV']:.6f} V; ADC minus scope {measurement['errorMv']:+.3f} mV; sample standard deviation {measurement['stddevUv']:.2f} uV")
    for rising,label in ((True,'Rising'),(False,'Falling')):
        edges = [edge for edge in result['edges'] if edge['rising']==rising]
        if edges:
            settling = [edge['settlingUs'] for edge in edges if edge['settlingUs'] is not None]
            lines.append(f"{label} edges: largest {'overshoot' if rising else 'undershoot'} {max(edge['excursionMv'] for edge in edges):.3f} mV")
            if settling:
                lines.append(f"  Latest settling after detection: {max(settling):.2f} us into +/-2% of ADC swing band")
            if len(settling)!=len(edges):
                lines.append('  Some intervals did not remain within that band before the next-edge exclusion')
    lines.extend(result['failures'])
    lines.append('PASS' if result['passed'] else 'FAIL')
    return '\n'.join(lines)


def writePlot(result,target):
    """Draw the 5 ms sample sequence and the first recorded rising and falling edges."""
    values,rate = result['waveformVolts'],result['sampleRate']
    minimum = min(values+[result['scopeLow']])-.03
    maximum = max(values+[result['scopeHigh']])+.03

    def chart(iStart,iEnd,origin,scale,label):
        """Draw samples iStart through iEnd-1; scale converts seconds to the axis units."""
        parts=[]
        for iTick in range(6):
            voltage=minimum+(maximum-minimum)*iTick/5
            y=320-(voltage-minimum)*280/(maximum-minimum)
            parts.append(f'<line x1="85" y1="{y}" x2="1085" y2="{y}" stroke="#ddd"/><text x="8" y="{y}">{voltage:.3f} V</text>')
            iSample=iStart+(iEnd-iStart-1)*iTick/5
            x=85+1000*iTick/5
            parts.append(f'<text x="{x}" y="345" text-anchor="middle">{(iSample-origin)/rate*scale:.2f}</text>')
        for voltage in (result['scopeLow'],result['scopeHigh']):
            y=320-(voltage-minimum)*280/(maximum-minimum)
            parts.append(f'<line x1="85" y1="{y}" x2="1085" y2="{y}" stroke="#176aab" stroke-dasharray="6 4"/>')
        coords=[(85+(iSample-iStart)*1000/(iEnd-iStart-1),320-(values[iSample]-minimum)*280/(maximum-minimum)) for iSample in range(iStart,iEnd)]
        line=' '.join(f'{x:.2f},{y:.2f}' for x,y in coords)
        parts.append(f'<polyline points="{line}" stroke="#c26716" fill="none" stroke-width="1.2"/>')
        if iEnd-iStart<300:
            for x,y in coords:
                parts.append(f'<circle cx="{x}" cy="{y}" r="1.8" fill="#c26716"/>')
        return f'<svg viewBox="0 0 1140 390" role="img" aria-label="{label}">{"".join(parts)}<text x="585" y="380" text-anchor="middle">{label}</text></svg>'

    panels=[chart(0,len(values),0,1000,'Milliseconds from start of the retained waveform window')]
    for rising,label in ((True,'Rising edge'),(False,'Falling edge')):
        edges=[edge for edge in result['edges'] if edge['rising']==rising and edge['sample']>=rate*30e-6]
        if edges:
            iEdge=edges[0]['sample']
            panels.append(f'<h2>{label}</h2>'+chart(max(0,iEdge-round(rate*30e-6)),min(len(values),iEdge+round(rate*150e-6)),iEdge,1e6,'Microseconds relative to threshold detection'))
    document=f'''<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>1 kHz ADC square-wave response</title>
<style>body{{font:17px system-ui;color:#172536;max-width:1200px;margin:30px auto;padding:24px}}svg{{width:100%}}text{{font:14px system-ui}}pre{{white-space:pre-wrap}}</style>
<h1>1 kHz square-wave response</h1><p>Orange: individual ADC samples read from SD. Blue dashed lines: manually entered scope levels.</p>
<p>Window starts {result['firstSample']/rate:.6f} seconds into the recording. Only these five milliseconds are analyzed for waveform shape.</p>
{''.join(panels)}<pre>{html.escape(summarize(result))}</pre>
<p>Settled measurements use the central half of each complete level. Detection uses 25%/75% of scope swing to avoid counting edge chatter twice.
Settling is measured after that detection, relative to the ADC's measured settled levels, within +/-2% of its swing. The last 10% of each interval is excluded.
Noise and isolated spikes affect settling and extrema. Overshoot/settling are descriptive; no ADC performance limit is asserted.</p>
<p>PASS requires recording integrity, frequency within 5% of 1 kHz, level widths within 10% of 500 us, and plateau errors within 10% of scope swing.
These are functional checks, not precision specifications. Generator edges are not captured simultaneously by the scope here.
Observed response includes generator, wiring, input circuit and ADC filtering. No claim isolates the source of distortion.</p></html>'''
    Path(target).write_text(document,encoding='utf-8')
