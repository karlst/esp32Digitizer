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

"""One offline HTML/SVG plot of validated, unadjusted smoke-test voltages."""
import html
import math
from pathlib import Path


def writePlot(result, target):
    """Save one common-scale plot; each point averages one settled plateau.

    The caller supplies validated measurements. Horizontal steps show plateau
    averages; vertical edges mark commanded timing, not measured edge shapes. Native SVG tooltips show actual voltages.
    """
    inputs = [v / 1000 for v in result['inputPlateausMv']]
    outputs = [v / 1000 for v in result['outputPlateausMv']]
    firstSecond = result.get('firstSecond', 0)
    span = len(inputs)
    minimum = math.floor((min(inputs + outputs) - .03) * 20) / 20
    maximum = math.ceil((max(inputs + outputs) + .03) * 20) / 20

    def point(second, voltage):
        """Map seconds and volts to the same axes for both instruments."""
        return 85 + (second - firstSecond) * 1040 / span, 35 + (maximum - voltage) * 370 / (maximum - minimum)

    parts = []
    for tick in range(6):
        voltage = minimum + tick * (maximum - minimum) / 5
        _, y = point(firstSecond, voltage)
        parts.append(f'<line x1="85" y1="{y}" x2="1125" y2="{y}" stroke="#dce3eb"/><text x="73" y="{y+5}" text-anchor="end">{voltage:.2f}</text>')
    for second in range(firstSecond, firstSecond + span + 1, 5):
        x, _ = point(second, minimum)
        parts.append(f'<text x="{x}" y="433" text-anchor="middle">{second}</text>')
    for values, color, label in ((inputs, '#166ac1', 'A2 input'), (outputs, '#d76b0b', 'SD output')):
        coordinates = [point(firstSecond+i+.5, value) for i, value in enumerate(values)]
        # Hold each measured plateau average across its commanded one-second
        # interval. Adjacent endpoint pairs share x, creating vertical edges.
        steps = [point(second, value)
                 for iPoint, value in enumerate(values)
                 for second in (firstSecond + iPoint, firstSecond + iPoint + 1)]
        line = ' '.join(f'{x:.2f},{y:.2f}' for x,y in steps)
        parts.append(f'<polyline points="{line}" stroke="{color}" fill="none" stroke-width="2.2"/>')
        for iPoint, (x,y) in enumerate(coordinates):
            parts.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="3.5" fill="{color}"><title>{label}, second {firstSecond+iPoint+1}: {values[iPoint]:.4f} V</title></circle>')
    verdict = 'PASS' if result['passed'] else 'FAIL'
    color = '#176c3b' if result['passed'] else '#af2727'
    chart = ''.join(parts)
    document = f'''<!doctype html>
<html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Hardware smoke test: input and SD output</title>
<style>body{{font:17px system-ui,sans-serif;color:#172536;background:#f5f7fa;margin:32px auto;padding:0 24px;max-width:1200px}}main{{background:white;padding:24px;border-radius:12px}}h1{{font-size:27px;margin-top:0}}svg{{width:100%;height:auto}}svg text{{font:15px system-ui;fill:#354658}}.blue{{color:#166ac1}}.orange{{color:#d76b0b}}.note{{color:#4c5d70;font-size:15px}}</style>
<main><h1>Hardware smoke test: input versus SD output</h1>
<p><strong style="color:{color}">{verdict}</strong> &middot; Largest plateau-average error: <b>{result['errorPercent']:.1f}%</b> of signal swing &middot; Limit: {result['errorLimitPercent']:g}%</p>
<p><b class="blue">Blue: measured input at Feather A2</b> &nbsp; <b class="orange">Orange: ADS127 samples read back from SD</b></p>
<svg viewBox="0 0 1170 480" role="img" aria-label="Input and stored output voltages over the retained recording window"><text x="15" y="20">Volts</text>{chart}<text x="605" y="465" text-anchor="middle">Seconds since capture started</text></svg>
<p class="note">Each horizontal step is the measured average of the settled portion (0.2-0.8 seconds) of that one-second level. Vertical edges mark commanded one-second boundaries, not measured transition timing or shape. Both traces use the same voltage scale. No fitted gain or offset.</p>
<p class="note">Plot: seconds {firstSecond} to {firstSecond + span} of the recording. Earlier overwritten data are not verified by this readback.</p>
<p class="note">SD payload written: {result['bytesWritten']:,} bytes &middot; {result['seconds']:.3f} seconds &middot; {html.escape(result['file'])}<br>Input: {html.escape(result['calibration'])}. ADS127: assumed 2.5 V reference, unity gain. This is a functional smoke test.</p></main></html>'''
    Path(target).write_text(document, encoding='utf-8')
