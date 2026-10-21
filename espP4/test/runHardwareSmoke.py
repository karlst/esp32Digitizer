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

"""Run the opt-in Feather/P4 hardware smoke firmware; preserve logs and report PASS/FAIL.

Karl runs this after uploads, with both boards reset, ADC powered, A1->A2 jumper,
DAC-to-ADC coax and an SD card installed. This opens COM ports and starts one
recording; it never builds, uploads, formats, or deletes existing recordings.
"""
import argparse
import json
from pathlib import Path
import queue
import threading
import time
from captureComparison import readPort
from hardwareSmokeReport import analyze, summary


def capture(featherPort, p4Port, output):
    """Identify both idle firmwares, prepare SD, capture once and validate readback.

    Two reader threads drain USB concurrently; this thread alone sends commands
    and writes logs. Opening ports holds DTR/RTS low rather than deliberately
    resetting boards. Identity checks happen before SD preparation or ADC start.
    A failed PC run requests Feather stop; board-local 30/35-s limits remain the
    fallback if USB is lost. Timeouts are failures, never evidence of safe removal
    while the card may still be draining. Keep the card installed after an abort.
    """
    import serial
    if featherPort.upper() == p4Port.upper():
        raise ValueError('Feather and P4 must use different ports')
    session = output / time.strftime('%Y%m%d-%H%M%S')
    session.mkdir(parents=True, exist_ok=False)
    ports, logs, threads = {}, {}, []
    records = {'Feather': [], 'P4': []}
    events, stop = queue.Queue(), threading.Event()
    started, finished = False, False
    confirmed = []

    def showProgress(message):
        """Print a validated check once and retain it if the final comparison fails."""
        confirmed.append(message)
        print(message, flush=True)

    try:
        for name, address in (('Feather', featherPort), ('P4', p4Port)):
            port = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=1)
            port.dtr = port.rts = False
            port.port = address
            ports[name] = port
            port.open()
            logs[name] = (session / (name.lower() + '.txt')).open('w', encoding='utf-8')
        time.sleep(2)
        for name, port in ports.items():
            port.reset_input_buffer()
            thread = threading.Thread(target=readPort, args=(port, name, events, stop), daemon=True)
            thread.start()
            threads.append(thread)
        ports['Feather'].write(b'h')
        ports['P4'].write(b'h')
        phase, ready, ended = 'identity', set(), set()
        deadline = time.monotonic() + 12
        print('Checking boards...')
        while not finished:
            if time.monotonic() >= deadline:
                reasons = {'identity': 'Boards did not identify as idle smoke firmware; close monitors and reset both boards',
                           'prepare': 'Storage or receiver did not become ready; check both logs',
                           'capture': 'Recording or SD readback did not finish'}
                raise ValueError(reasons[phase])
            try:
                name, line = events.get(timeout=0.2)
            except queue.Empty:
                name, line = None, None
            if name is not None:
                records[name].append(line)
                logs[name].write(line + '\n')
                logs[name].flush()
                if line.startswith('READER_ERROR'):
                    raise ValueError(name + ' USB connection failed')
                if 'ready=0' in line and line.startswith('SMOKE_'):
                    raise ValueError(name + ' is not ready; reset both boards before retrying')
                if phase == 'identity':
                    expected = 'SMOKE_FEATHER' if name == 'Feather' else 'SMOKE_P4'
                    if line == expected + ' version=3 ready=1 seconds=30':
                        ready.add(name)
                    elif line.startswith(expected + ' version='):
                        raise ValueError('Upload current hardware-smoke firmware to both boards (version 3 required)')
                    if len(ready) == 2:
                        # One command requests preparation and recording. Mark
                        # started first so an interrupted write still requests STOP.
                        started = True
                        ports['Feather'].write(b'g')
                        phase = 'prepare'
                        deadline = time.monotonic() + 200
                        print('Preparing SD card and receiver...')
                elif phase == 'prepare':
                    if name == 'Feather' and line.startswith('ADC START echo confirmed.'):
                        phase = 'capture'
                        deadline = time.monotonic() + 240
                        print('Recording 30 seconds, then reading the SD card back...')
                    elif ('FAILED' in line or 'PREPARE FAILED' in line or
                          line.startswith(('No matching receiver READY', 'Test already pending/running',
                                           'Source/ADC START failed', 'DAC setup failed',
                                           'ADC127 FAULT', 'ADC127 TIMEOUT'))):
                        raise ValueError('Preparation or capture start failed; check the board logs')
                elif phase == 'capture':
                    refused = ('No matching receiver READY', 'ADC127 FAILED',
                               'Source/ADC START failed', 'DAC setup failed',
                               'Test already pending/running')
                    if name == 'Feather' and line.startswith(refused):
                        raise ValueError('ADC capture could not start; check the Feather log')
                    if name == 'Feather' and line.startswith('INPUT_END run='):
                        ended.add(name)
                    if name == 'P4' and line.startswith('SMOKE_SD_END run='):
                        ended.add(name)
                    finished = len(ended) == 2
        result = analyze('\n'.join(records['Feather']), '\n'.join(records['P4']),
                         showProgress, session / 'comparison.html')
        (session / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        report = summary(result)
        if result['calibration'].startswith('ESTIMATE'):
            report += '\nInput voltage uses nominal calibration; absolute accuracy is limited.'
        (session / 'result.txt').write_text(report + '\n', encoding='utf-8')
        print(summary(result, includeChecks=False))
        if result['calibration'].startswith('ESTIMATE'):
            print('Input voltage uses nominal calibration; absolute accuracy is limited.')
    except BaseException as error:
        (session / 'result.txt').write_text('\n'.join(confirmed + ['FAIL: ' + (str(error) or 'Interrupted')]) + '\n', encoding='utf-8')
        raise
    finally:
        feather = ports.get('Feather')
        if started and not finished and feather is not None and feather.is_open:
            try:
                feather.write(b's')
                time.sleep(0.3)
            except (OSError, serial.SerialException):
                pass
        stop.set()
        for thread in threads:
            thread.join(timeout=1)
        for port in ports.values():
            port.close()
        for log in logs.values():
            log.close()
        if (session / 'comparison.html').exists():
            print('Plot:', (session / 'comparison.html').resolve())
        print('Logs:', session.resolve())


def main():
    """Select ports/output from command line; return a nonzero exit code on failure."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--feather', default='COM4')
    parser.add_argument('--p4', default='COM3')
    parser.add_argument('--output', type=Path, default=Path('captures/hardware-smoke'))
    args = parser.parse_args()
    result = 0
    try:
        capture(args.feather, args.p4, args.output)
    except (OSError, ValueError, ImportError, KeyboardInterrupt) as error:
        print('FAIL:', str(error) or 'Interrupted')
        print('Keep the SD card installed if recording/readback was interrupted.')
        result = 1
    return result


if __name__ == '__main__':
    raise SystemExit(main())
