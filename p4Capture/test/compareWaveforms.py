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

"""Compare simultaneous Feather INPUT and P4 WAVE RAM reports, without hardware.

Fit one time shift, scale and offset using only the first cycle's interior.
Apply that unchanged to later points. This is a shape comparison between two
different ADCs, not voltage calibration or proof that every ADS127 sample arrived.
"""
import argparse
from bisect import bisect_right
import csv
import html
import math
from pathlib import Path
import re


def parseInput(text):
    """Validate legacy single readings or versioned bursts, preserving real times."""
    metadata, rows, ended = None, [], False
    for line in text.splitlines():
        match = re.fullmatch(r'INPUT_BEGIN run=(\d+) count=(\d+) clean=(\d+) missed=(\d+) maxGapUs=(\d+)(?: mode=(burst))?', line)
        if match:
            if metadata is not None:
                raise ValueError('Multiple Feather reports; choose one capture')
            metadata = tuple(map(int, match.groups()[:5])) + (match.group(6) or 'single',)
        elif line.startswith('INPUT,'):
            if metadata is None or ended:
                raise ValueError('Input rows outside report')
            fields = line.split(',')
            if len(fields) != (8 if metadata[5] == 'burst' else 4):
                raise ValueError('Input row does not match declared sampling mode')
            _, index, timestamp, value = fields[:4]
            index, timestamp, value = int(index), int(timestamp), float(value)
            if index != len(rows) or not math.isfinite(value) or not 0 <= value <= 4095 or not 1000000 <= timestamp <= 1310000:
                raise ValueError('Invalid input row index, time or ADC count')
            if metadata[5] == 'burst':
                minimum,maximum,reads,duration = map(int, fields[4:])
                if not 0 <= minimum <= value <= maximum <= 4095 or not 2 <= reads <= 4096 or not 1 <= duration <= 100000:
                    raise ValueError('Invalid burst extrema, conversion count or duration')
            at = (timestamp - 1000000) / 1000.0
            if rows and at <= rows[-1][0]:
                raise ValueError('Input timestamps are not strictly increasing')
            rows.append((at, value))
        elif line.startswith('INPUT_END'):
            if metadata is None or line != f'INPUT_END run={metadata[0]}':
                raise ValueError('Input end run ID mismatch')
            ended = True
    if not ended or metadata is None or len(rows) != metadata[1] or len(rows) < 200 or metadata[2] != 1:
        raise ValueError('Input record incomplete or marked unclean; retain logs for diagnosis')
    return metadata, rows


def parseOutput(text):
    """Validate ordered 0.5 ms bins and signed-24-bit ranges from the P4 report."""
    metadata, rows, ended = None, [], False
    for line in text.splitlines():
        match = re.fullmatch(r'WAVE_BEGIN run=(\d+) rate=(\d+) bins=(\d+) samplesPerBin=(\d+) clean=(\d+)', line)
        if match:
            if metadata is not None:
                raise ValueError('Multiple P4 reports; choose one capture')
            metadata = tuple(map(int, match.groups()))
        elif line.startswith('WAVE,'):
            if metadata is None or ended:
                raise ValueError('Output rows outside report')
            _, index, minimum, maximum, mean = line.split(',')
            index, minimum, maximum, mean = int(index), int(minimum), int(maximum), float(mean)
            if index != len(rows) or not math.isfinite(mean) or not -8388608 <= minimum <= mean <= maximum <= 8388607:
                raise ValueError('Invalid output row index or sample range')
            rows.append(((index + 0.5) * 0.5, mean, minimum, maximum))
        elif line.startswith('WAVE_END'):
            if metadata is None or line != f'WAVE_END run={metadata[0]}':
                raise ValueError('Output end run ID mismatch')
            ended = True
    if not ended or metadata is None or len(rows) != 600 or metadata[2] != 600 or metadata[4] != 1:
        raise ValueError('Output record incomplete or marked unclean; retain logs for diagnosis')
    if metadata[1] != metadata[3] * 2000:
        raise ValueError('Output bin duration is not 0.5 ms')
    return metadata, rows


def interpolate(rows, times, at):
    """Linear interpolation between neighboring output-bin means; no extrapolation."""
    if at < times[0] or at > times[-1]:
        raise ValueError('Requested comparison outside the captured window')
    index = min(len(rows) - 2, max(0, bisect_right(times, at)-1))
    fraction = (at-times[index])/(times[index+1]-times[index])
    return rows[index][1] + fraction*(rows[index+1][1]-rows[index][1])


def fitScale(pairs):
    """Least-squares FeatherCount = gain * P4Count + offset, with centered sums."""
    xMean = sum(x for x, _ in pairs)/len(pairs)
    yMean = sum(y for _, y in pairs)/len(pairs)
    variance = sum((x-xMean)**2 for x, _ in pairs)
    if variance <= 0:
        raise ValueError('Output has no variation for scale estimation')
    gain = sum((x-xMean)*(y-yMean) for x,y in pairs)/variance
    offset = yMean-gain*xMean
    error = sum((gain*x+offset-y)**2 for x,y in pairs)/len(pairs)
    return gain, offset, error


def compare(inputRows, outputRows):
    """Fit only 20..100 ms; evaluate held-out 100..280 ms with no further fit.

    Use a fixed population at every shift to avoid favoring shifts that drop
    difficult points. The +/-20 ms bound is narrower than a 10 Hz cycle; an
    optimum at that bound is reported rather than silently expanding the search.
    No clock-rate warp, nonlinear amplitude adjustment or separate cycle fits.
    """
    selected = [(t, v) for t, v in inputRows if 20 <= t <= 280]
    training = [(t, v) for t, v in selected if t < 100]
    validation = [(t, v) for t, v in selected if t >= 100]
    if len(training) < 40 or len(validation) < 100:
        raise ValueError('Insufficient common data for independent fit and validation')
    amplitude = max(v for _,v in selected)-min(v for _,v in selected)
    if amplitude < 20:
        raise ValueError('Input too flat for useful waveform alignment')
    times = [row[0] for row in outputRows]
    best = None
    for step in range(-800, 801):
        shift = step * 0.025
        pairs = [(interpolate(outputRows,times,t+shift),v) for t, v in training]
        gain, offset, error = fitScale(pairs)
        if best is None or error < best[0]:
            best = (error, shift, gain, offset)
    fitError, shift, gain, offset = best
    comparisons = []
    for at, measured in selected:
        raw = interpolate(outputRows,times,at+shift)
        mapped = gain*raw+offset
        comparisons.append((at,measured,raw,mapped,mapped-measured,'fit' if at < 100 else 'validation'))
    errors = [row[4] for row in comparisons if row[5] == 'validation']
    rms = math.sqrt(sum(error*error for error in errors)/len(errors))
    maximum = max(abs(error) for error in errors)
    return dict(shift=shift,gain=gain,offset=offset,fitRms=math.sqrt(fitError),rms=rms,
                maximum=maximum,amplitude=amplitude,normalizedRms=100*rms/amplitude,
                boundary=abs(shift)>=19.999,rows=comparisons)


def svgPanel(traces, title, yLabel, limits=None):
    """Plot time/value traces with a shared vertical scale and explicit units."""
    allValues = [value for _,points in traces for _,value in points]
    low, high = min(allValues), max(allValues)
    margin = max(1,(high-low)*0.1)
    low, high = low-margin,high+margin
    # Optional explicit bounds let different instruments share one visual scale.
    # Existing waveform reports retain their original automatic scaling.
    if limits is not None:
        low, high = limits
    elements = [f'<h2>{html.escape(title)}</h2><p>{html.escape(yLabel)}</p><svg viewBox="0 0 1200 400">']
    for tick in range(6):
        y = 330-tick*260/5
        value = low+tick*(high-low)/5
        elements.append(f'<path d="M110 {y}H1140" stroke="#ddd"/><text x="5" y="{y}">{value:.1f}</text>')
    for tick in range(7):
        x=110+tick*1030/6
        elements.append(f'<text x="{x}" y="355">{tick*50}</text>')
    for color,points in traces:
        mapped=' '.join(f'{110+t/300*1030:.3f},{330-(v-low)/(high-low)*260:.3f}' for t, v in points)
        elements.append(f'<polyline points="{mapped}" fill="none" stroke="{color}" stroke-width="1.8"/>')
    elements.append('<text x="450" y="390">Milliseconds after settling interval</text></svg>')
    return ''.join(elements)


def analyze(inputText, outputText, target):
    """Write raw traces, adjusted overlay, residuals and every comparison to CSV."""
    inputMeta, inputRows=parseInput(inputText)
    outputMeta, outputRows=parseOutput(outputText)
    if inputMeta[0] != outputMeta[0]:
        raise ValueError('Different run IDs: these are not the same capture')
    result=compare(inputRows,outputRows)
    rows=result['rows']
    csvPath=target.with_suffix('.csv')
    with csvPath.open('w',newline='',encoding='utf-8') as output:
        writer=csv.writer(output)
        writer.writerow(['inputTimeMs','inputFeatherCounts','interpolatedP4Counts','mappedP4InFeatherCounts','mappedMinusInputCounts','use'])
        writer.writerows(rows)
    summary=(f"Run {inputMeta[0]}. Fitted time shift: {result['shift']:+.3f} ms; "
             f"gain: {result['gain']:.9g}; offset: {result['offset']:.3f} Feather counts. "
             f"Validation RMS difference: {result['rms']:.3f} counts "
             f"({result['normalizedRms']:.3f}% of measured input peak-to-peak); "
             f"maximum absolute difference: {result['maximum']:.3f} counts.")
    warnings=[]
    burstNote='Input mode: single readings (legacy capture).'
    burstTraces=[]
    if inputMeta[5] == 'burst':
        # parseInput already validated these rows. Retain all diagnostics in a
        # separate CSV; averaging must not silently erase evidence of excursions.
        bursts=[]
        for line in inputText.splitlines():
            if line.startswith('INPUT,'):
                _,index,at,mean,minimum,maximum,reads,duration=line.split(',')
                bursts.append(((int(at)-1000000)/1000,float(mean),int(minimum),int(maximum),int(reads),int(duration)))
        with target.with_name(target.stem+'-input-bursts.csv').open('w',newline='',encoding='utf-8') as output:
            writer=csv.writer(output)
            writer.writerow(['timeMs','meanCounts','minCounts','maxCounts','conversionCount','actualDurationUs'])
            writer.writerows(bursts)
        durations=[row[5] for row in bursts]
        counts=[row[4] for row in bursts]
        burstNote=(f'Input mode: burst averages. Actual durations {min(durations)}..{max(durations)} us; '
                   f'{min(counts)}..{max(counts)} conversions per burst. Pale lines retain minimum/maximum.')
        if max(durations)>750 or min(durations)<450:
            warnings.append('Some input burst durations differ materially from 500 us; averaging windows are not equivalent.')
        burstTraces=[('#a4d0df',[(r[0], r[2]) for r in bursts]),('#a4d0df',[(r[0], r[3]) for r in bursts])]
    if result['boundary']:
        warnings.append('Best timing shift hit the search boundary; alignment is unresolved.')
    if result['gain'] <= 0:
        warnings.append('Fitted gain is nonpositive: polarity/shape needs investigation.')
    if inputMeta[3] or inputMeta[4] > 3000:
        warnings.append(f'Input sampling gaps: missed slots={inputMeta[3]}, maximum gap={inputMeta[4]} us.')
    plots=svgPanel(burstTraces+[('#006b91',inputRows)],'Measured input: Feather A2','Feather ADC counts (0..4095)')
    plots+=svgPanel([('#d36a12',[(r[0], r[1]) for r in outputRows])],'Raw output: ADS127 through P4','Signed ADS127 counts; each point is a 0.5 ms average')
    plots+=svgPanel([('#006b91',[(r[0], r[1]) for r in rows]),('#d36a12',[(r[0], r[3]) for r in rows])],
                    'Aligned overlay: blue input, orange adjusted output','Feather-count units after fitted gain and offset')
    plots+=svgPanel([('#8355a8',[(r[0],r[4]) for r in rows if r[5]=='validation'])],
                    'Independent comparison: later points, not used to fit','Adjusted output minus measured input, in Feather counts')
    target.write_text('<!doctype html><meta charset="utf-8"><title>Simultaneous waveform comparison</title>'
        '<style>body{font:17px Arial;margin:30px;max-width:1200px}svg{width:100%}text{font-size:13px}h2{margin-top:35px}</style>'
        '<h1>Input versus digitizer output — simultaneous RAM capture</h1><p>'+html.escape(summary)+'</p>'
        '<p>'+html.escape(burstNote)+'</p>'
        '<p>'+html.escape(' '.join(warnings))+'</p>'
        '<p>Only 20..100 ms fits the time shift, gain and offset. Those three values are held fixed for validation at 100..280 ms. '
        'Positive shift means use P4 data later on its own timeline to match a Feather measurement. Search: ±20 ms in 0.025 ms steps.</p>'
        '<p>This is waveform-shape agreement, not absolute voltage/gain/offset calibration. Scaling can hide constant analog gain/offset error. '
        'Feather noise/nonlinearity, independent clocks, ADC filtering and unequal averaging windows contribute to the differences. '
        'Burst reads are not uniformly spaced. A lower residual after averaging does not qualify a production recording path. '
        'Interpolation does not create new measurements. No pass threshold or claim of full ADC precision.</p>'+plots,encoding='utf-8')
    print(summary)
    print(burstNote)
    for warning in warnings:
        print('WARNING:',warning)
    print('Plot saved:',target.resolve())
    print('Point comparison CSV:',csvPath.resolve())
    return result


def main():
    """Offline analysis only: preserve original logs and create new derived files."""
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('inputLog',type=Path)
    parser.add_argument('outputLog',type=Path)
    parser.add_argument('--output',type=Path,default=Path('comparison.html'))
    args=parser.parse_args()
    analyze(args.inputLog.read_text(),args.outputLog.read_text(),args.output)


if __name__ == '__main__':
    main()
