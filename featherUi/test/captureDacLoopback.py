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

"""Capture Feather A1->A2 measurements over COM4 and draw actual ADC readings.

Run only after the unpowered wiring step: disconnect TI coax, jumper A1 to A2.
Close COM4 monitors first. This starts one bounded test with g; no SD/P4 access.
"""
import argparse
from pathlib import Path
import re
import time
import serial


def savePlot(rows, clean, target):
    """Write rows of (microseconds, raw A2 count) to an HTML plot at target.

    main calls this after matching the board's declared row count. The vertical
    scale follows observed minimum/maximum, not the DAC's commanded range. clean
    is the board's collection flag; no sine fit or waveform acceptance test runs.
    """
    # Leave space above and below the measured range, including a constant input.
    low, high = min(value for _, value in rows), max(value for _, value in rows)
    margin = max(5, (high-low)*0.1)
    floor, ceiling = low-margin, high+margin
    lines = []
    for tick in range(7):
        x = 100 + tick*1000/6
        lines.append(f'<text x="{x}" y="480">{tick*50}</text>')
    for tick in range(6):
        y = 440 - tick*360/5
        value = floor + tick*(ceiling-floor)/5
        lines.append(f'<path d="M100 {y}H1100" stroke="#ddd"/><text x="5" y="{y}">{value:.0f}</text>')
    # Map 300,000 microseconds to the full plot width. Joining readings draws a
    # display line; it does not create measurements between the recorded times.
    points = ' '.join(f'{100+t/300000*1000},{440-(v-floor)/(ceiling-floor)*360}' for t,v in rows)
    lines.append(f'<polyline points="{points}" fill="none" stroke="#006b91" stroke-width="2"/>')
    target.write_text('<!doctype html><meta charset="utf-8"><title>Feather DAC measurement</title>'
        '<style>body{font:18px Arial;margin:30px}svg{width:100%;max-width:1200px}text{font-size:14px}</style>'
        '<h1>Feather DAC measured through A1 → A2</h1>'
        f'<p>Measured raw counts: {low} to {high}; samples={len(rows)}; collection clean={clean}.</p>'
        '<p>Actual measurements, not requested DAC codes. Confirm TI coax disconnected for the unloaded comparison.</p>'
        '<svg viewBox="0 0 1200 530">' + ''.join(lines) +
        '<text x="450" y="520">Milliseconds after one-second settling</text></svg>'
        '<p>Vertical axis: Feather 12-bit ADC counts. No calibrated voltage or precision distortion claim.</p>', encoding='utf-8')


def main():
    """Open COM4 without deliberately resetting; start one test, retain full log."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', default='COM4')
    parser.add_argument('--output', type=Path, default=Path('work/dac-loopback'))
    args = parser.parse_args()

    # Save each run beside its complete serial log. These files are on the PC;
    # this diagnostic does not write the P4's SD card.
    args.output.mkdir(parents=True, exist_ok=True)
    prefix = args.output / time.strftime('%Y%m%d-%H%M%S')
    port = serial.Serial(port=None, baudrate=115200, timeout=1)

    # Set reset-control signals before opening; some USB drivers may still pulse
    # them during connection. The two-second pause allows startup text to finish.
    port.dtr = port.rts = False
    port.port = args.port
    rows, expected, clean = [], None, 0
    try:
        port.open()
        time.sleep(2)
        port.write(b'g')
        deadline = time.monotonic() + 20
        with prefix.with_suffix('.txt').open('w', encoding='utf-8') as log:

            # Preserve all text, but collect readings only after LOOP_BEGIN.
            # A complete report can still say clean=0 and is plotted with a warning.
            while time.monotonic() < deadline:
                line = port.readline().decode('utf-8', errors='replace').strip()
                if line:
                    log.write(line + '\n')
                    log.flush()
                    if not line.startswith('LOOP,'):
                        print(line)
                    match = re.fullmatch(r'LOOP_BEGIN count=(\d+) clean=(\d+) missedReadSlots=(\d+)', line)
                    if match:
                        expected, clean, _ = map(int, match.groups())
                        rows = []
                    elif line.startswith('LOOP,') and expected is not None:
                        _, timestamp, value = line.split(',')
                        timestamp, value = int(timestamp), int(value)
                        if not 0 <= value <= 4095 or (rows and timestamp <= rows[-1][0]):
                            raise ValueError('Invalid value or time ordering; log retained')
                        rows.append((timestamp, value))
                    elif line == 'LOOP_END' and expected is not None:
                        if len(rows) != expected or len(rows) < 2:
                            raise ValueError('Incomplete measurement; log retained')
                        target = prefix.with_suffix('.html')
                        savePlot(rows, clean, target)
                        print(f'Plot saved: {target.resolve()}')
                        if not clean:
                            print('WARNING: collection incomplete or driver fault; review log.')
                        return
        raise ValueError('Timed out. Confirm loopback firmware and COM4. Log retained.')
    finally:
        # A bounded board-side timer stops normally even if the PC disconnects;
        # send abort as well when leaving while a serial connection remains open.
        if port.is_open:
            try:
                port.write(b's')
            except serial.SerialException:
                pass
        port.close()


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, serial.SerialException) as error:
        raise SystemExit(str(error))
