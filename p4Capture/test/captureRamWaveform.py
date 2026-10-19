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

"""Own P4 USB serial, select RAM mode, and save a self-contained waveform HTML.

Close the P4 monitor first. This sends only 'w' to P4; Karl starts Feather with 6.
Uses pyserial (already used by miniterm). No SD access or firmware upload.
"""
import argparse
from pathlib import Path
import re
import time
import serial


def savePlot(rows, metadata, destination):
    """Plot all bin extrema and means; no fitted sine or voltage calibration."""
    low = min(row[1] for row in rows)
    high = max(row[2] for row in rows)
    margin = max(1, (high - low) * 0.1)
    low, high = low - margin, high + margin
    elements = []
    for tick in range(7):
        x = 100 + tick * 1000 / 6
        elements.append(f'<text x="{x}" y="480">{tick*50}</text>')
    for tick in range(6):
        y = 440 - tick * 360 / 5
        value = low + tick * (high-low)/5
        elements.append(f'<path d="M100 {y}H1100" stroke="#ddd"/><text x="5" y="{y}">{value:.0f}</text>')
    points = []
    for index, minimum, maximum, mean in rows:
        x = 100 + (index + 0.5) / 600 * 1000
        yMin = 440 - (minimum-low)/(high-low)*360
        yMax = 440 - (maximum-low)/(high-low)*360
        yMean = 440 - (mean-low)/(high-low)*360
        elements.append(f'<path d="M{x} {yMin}V{yMax}" stroke="#79b9d2"/>')
        points.append(f'{x},{yMean}')
    elements.append('<polyline points="' + ' '.join(points) + '" fill="none" stroke="#006b91" stroke-width="2"/>')
    destination.write_text('<!doctype html><meta charset="utf-8"><title>P4 RAM waveform</title>'
        '<style>body{font:18px Arial;margin:30px}svg{width:100%;max-width:1200px}text{font-size:14px}</style>'
        '<h1>P4 RAM waveform — no SD recording</h1>'
        f'<p>Run {metadata[0]}, rate {metadata[1]} samples/s, capture clean={metadata[4]}.</p>'
        '<p>300 ms after a one-second settling skip. Mean line with minimum/maximum envelope for each 0.5 ms bin.</p>'
        '<svg viewBox="0 0 1200 530">' + ''.join(elements) +
        '<text x="450" y="520">Milliseconds after settling skip</text></svg>'
        '<p>Vertical axis: signed ADC counts, not calibrated volts. Capture clean does not establish sine fidelity.</p>', encoding='utf-8')


def main():
    """Wait up to 120 seconds for one complete, ordered P4 waveform report."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM3')
    parser.add_argument('--output', type=Path, default=Path('work/ram-waveform'))
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime('%Y%m%d-%H%M%S')
    prefix = args.output / stamp
    rows, metadata = [], None
    # Set modem control before opening; opening a monitor must not intentionally
    # reset the board. USB adapters may still reset: boot text is retained.
    port = serial.Serial(port=None, baudrate=115200, timeout=1)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        port.open()
        time.sleep(2)
        port.write(b'w')
        print('Waiting for RAM READY. Then press 6 on Feather COM4. Do not type p.')
        deadline = time.monotonic() + 120
        with prefix.with_suffix('.txt').open('w', encoding='utf-8') as log:
            while time.monotonic() < deadline:
                line = port.readline().decode('utf-8', errors='replace').strip()
                if line:
                    log.write(line + '\n')
                    log.flush()
                    if not line.startswith('WAVE,'):
                        print(line)
                    if 'Mode already selected' in line:
                        raise ValueError('Reset P4, close this reader, and rerun before any SD commands.')
                    match = re.fullmatch(r'WAVE_BEGIN run=(\d+) rate=(\d+) bins=(\d+) samplesPerBin=(\d+) clean=(\d+)', line)
                    if match:
                        metadata = tuple(map(int, match.groups()))
                        rows = []
                    elif line.startswith('WAVE,') and metadata:
                        _, index, minimum, maximum, mean = line.split(',')
                        if int(index) != len(rows):
                            raise ValueError('Missing or reordered waveform rows; raw log retained')
                        rows.append((int(index), int(minimum), int(maximum), float(mean)))
                    elif line.startswith('WAVE_END') and metadata:
                        if line != f'WAVE_END run={metadata[0]}' or len(rows) != 600 or metadata[2] != 600:
                            raise ValueError('Incomplete waveform; raw log retained')
                        target = prefix.with_suffix('.html')
                        savePlot(rows, metadata, target)
                        print(f'Plot saved: {target.resolve()}')
                        if metadata[4] != 1:
                            print('WARNING: capture reported errors; inspect the saved console log.')
                        return
        raise ValueError('Timed out; console log retained. Check Feather stopped; do not assume success.')
    finally:
        port.close()


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, serial.SerialException) as error:
        raise SystemExit(str(error))
