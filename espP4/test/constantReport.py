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

"""Report constant-command RAM captures without fitting gain, offset or time.

The existing firmware averages different numbers of readings on each board.
Use a shared millivolt scale when the Feather exports SDK calibration metadata.
Keep original count plots too; raw extrema reveal excursions hidden by averaging.
No serial access occurs here; captureComparison calls this after capture ends.
"""
import html
import statistics
import re
from bisect import bisect_right
from compareWaveforms import parseInput, parseOutput, svgPanel


def calibrationMapping(text, run):
    """Validate a matching SDK table; return an approximate count-to-mV converter.

    The firmware supplies count/voltage pairs spaced 32 counts apart (the last
    interval is 31). Between pairs, use a straight-line estimate in millivolts.
    Conversion of an averaged
    count is approximate if the mapping is nonlinear within that averaging group.
    Old captures remain valid raw reports but cannot acquire invented calibration.
    """
    headers = re.findall(r'^INPUT_CAL run=(\d+) source=(\d+) defaultVrefMv=1100$', text, re.M)
    if not headers:
        return None, 'No calibration recorded; repeat capture with updated Feather firmware.'
    if len(headers) != 1 or int(headers[0][0]) != run or int(headers[0][1]) not in (0,1,2,3):
        raise ValueError('Invalid calibration identity')
    rows = [tuple(map(int, line.split(',')[1:])) for line in text.splitlines() if line.startswith('CAL,')]
    expected = list(range(0, 4096, 32))+[4095]
    if any(len(r) != 2 for r in rows):
        raise ValueError('Malformed calibration row')
    if [r[0] for r in rows] != expected or any(not 0 <= r[1] <= 5000 for r in rows):
        raise ValueError('Incomplete or invalid calibration table')
    if any(b[1] < a[1] for a, b in zip(rows,rows[1:])) or rows[-1][1] <= rows[0][1]:
        raise ValueError('Calibration table is not increasing')

    def toMillivolts(raw):
        """Estimate millivolts between neighboring table entries; reject counts outside 0..4095."""
        if not 0 <= raw <= 4095:
            raise ValueError('ADC count outside calibration range')
        index = min(len(rows) - 2, max(0, bisect_right(expected, raw) - 1))
        a, b = rows[index:index+2]
        return a[1]+(raw-a[0])*(b[1]-a[1])/(b[0]-a[0])

    labels = {0:'factory reference calibration',1:'factory two-point calibration',
              2:'ESTIMATE: nominal 1100 mV reference; no factory calibration used',
              3:'factory two-point fitted calibration'}
    return toMillivolts, labels[int(headers[0][1])]


def voltagePanels(panels, converter, label):
    """Center each instrument independently and give both plots identical mV bounds.

    Shared limits cover averaged curves; raw extrema remain below in count plots
    so isolated spikes do not flatten the averaged-line comparison. No gain fit,
    timing fit or smoothing is applied here.
    """
    centered = []
    for index, (name, units, means, lows, highs) in enumerate(panels):
        # Feather uses the recorded calibration table. ADS127 uses the assumed
        # 2500-mV reference: 2^23 signed counts correspond to positive full scale.
        # Removing each trace's average compares fluctuations, not absolute voltage.
        convert = converter if index == 0 else lambda raw: raw * 2500.0 / 8388608
        voltages = [(t,convert(v)) for t, v in means]
        center = statistics.mean(v for _,v in voltages)
        centered.append((name,[(t, v-center) for t, v in voltages]))
    span = max(0.1,max(abs(v) for _,points in centered for _,v in points)*1.1)
    body = ('<h2>Same voltage scale: averaged readings above/below their own average</h2>'
            '<p>Zero is each instrument’s own average. Both graphs use exactly the same '
            'vertical limits in millivolts (mV). No fitted gain or time shift. '
            'Only the existing approximately 0.5 ms averages are shown here.</p><p>Feather: '
            +html.escape(label)+'. SDK table interpolation gives approximate voltages. '
            'ADS127: assumed 2.5 V reference and unity input gain. Different hardware '
            'filters and numbers of averaged readings remain.</p>')
    for name, points in centered:
        _, spread, deviation = fluctuationSummary([v for _,v in points])
        body += f'<p>{html.escape(name)}: peak-to-peak {spread:.3f} mV; RMS fluctuation {deviation:.3f} mV.</p>'
        body += svgPanel([('#006b91',points)],name,'mV relative to own average; identical scale',(-span,span))
    return body


def fluctuationSummary(values):
    """Return mean, peak-to-peak spread and population deviation of plotted means.

Deviation is RMS distance from this trace's own mean, not input/output error.
These statistics describe averaged points, not all individual ADC conversions.
"""
    if len(values) < 2:
        raise ValueError('At least two points required')
    return statistics.mean(values), max(values)-min(values), statistics.pstdev(values)


def analyzeConstant(inputText, outputText, target):
    """Save a report from two clean text logs; return the HTML target path.

    Calibrated voltage plots share bounds; original count plots zoom separately.
    Missing calibration preserves the raw-only view. Malformed calibration or
    incompatible capture metadata raises ValueError rather than inventing volts.
    Input text is evidence, never commands; this function does not open devices.
    """
    if 'DAC MODE: CONSTANT code=135 ' not in inputText:
        raise ValueError('Missing constant-code firmware identity')
    inputMeta, inputRows = parseInput(inputText)
    outputMeta, outputRows = parseOutput(outputText)
    if inputMeta[0] != outputMeta[0] or inputMeta[5] != 'burst':
        raise ValueError('Expected matching run IDs and burst input')

    # Input parser already validated each burst's extrema and numeric fields.
    inputLow, inputHigh = [], []
    for line in inputText.splitlines():
        if line.startswith('INPUT,'):
            fields = line.split(',')
            at = (int(fields[2])-1000000)/1000
            inputLow.append((at, int(fields[4])))
            inputHigh.append((at, int(fields[5])))
    outputMeans = [(r[0], r[1]) for r in outputRows]
    panels = [
        ('Feather A2', 'Feather ADC counts (0..4095)', inputRows, inputLow, inputHigh),
        ('ADS127 through P4', 'Signed ADS127 counts', outputMeans,
         [(r[0], r[2]) for r in outputRows], [(r[0], r[3]) for r in outputRows])]
    body = '<h1>Constant DAC command: code 135 (nominal 1.747 V)</h1>'
    converter, label = calibrationMapping(inputText, inputMeta[0])
    if converter is not None:
        body += voltagePanels(panels,converter, label)
    else:
        body += '<p>'+html.escape(label)+'</p>'
    body += '<h2>Original count plots and individual-reading ranges</h2>'
    body += ('<p>Same wiring; no SD. A constant command does not guarantee a constant voltage. '
             'No waveform alignment or fitted gain/offset is used.</p>'
             '<p>Dark lines: approximately 0.5 ms averages (Feather: variable reading count; '
             'P4: 512 samples at 1,024,000 samples/s). Pale lines: lowest/highest individual readings '
             'within each group. Axes zoom independently and use different count units; '
             'do not compare line heights or count magnitudes as voltage noise.</p>')
    for name, units, means, lows, highs in panels:
        mean, spread, deviation = fluctuationSummary([v for _,v in means])
        summary = (f'{name}: average={mean:.3f}; averaged-point peak-to-peak={spread:.3f}; '
                   f'RMS fluctuation about own average={deviation:.3f}; '
                   f'individual reading range={min(v for _,v in lows)}..{max(v for _,v in highs)} counts.')
        print(summary)
        body += '<p>'+html.escape(summary)+'</p>'
        body += svgPanel([('#b8cbdc',lows),('#b8cbdc',highs),('#006b91',means)],name,units)
    body += ('<p>This short measurement helps distinguish instrument fluctuations from '
             'waveform timing effects. It cannot uniquely separate real source noise, '
             'ADC noise and different hardware filtering. It is not a production noise qualification.</p>')
    target.write_text('<!doctype html><meta charset="utf-8"><title>Constant voltage comparison</title>'
                      '<style>body{font:18px sans-serif;max-width:1200px;margin:24px auto}svg{width:100%}</style>'+body,
                      encoding='utf-8')
    print('Plot saved:',target.resolve())
    return target
