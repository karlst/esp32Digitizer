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

Karl runs this after uploads, with both boards reset, ADC powered and SD installed.
Ordinary mode uses the Feather DAC and A1->A2 jumper. --external requires the
dedicated Feather build and a powered external generator connected to CH0.
This opens COM ports and starts one recording; it never builds, uploads, formats,
or deletes existing recordings. Generator power/output remain manual.
"""
import argparse
import json
from pathlib import Path
import queue
import re
import threading
import time
from captureComparison import readPort
from hardwareSmokeReport import analyze, summary


def capture(featherPort, p4Port, output, seconds=30, linkOnly=False, scopeLevels=None, frequencyHz=0.5):
    """Identify both idle firmwares, prepare SD, capture once and validate readback.

    A reader thread for each board collects USB serial output while this thread
    sends commands and saves logs. DTR/RTS, serial control lines that can reset a
    board, are held low when opening the ports. Check firmware identity before
    preparing SD files or starting the ADC.

    On failure, request a stop through the Feather. If USB communication is lost,
    the Feather's selected run duration and the P4's duration-plus-five-seconds
    timeout provide fallback stops. A PC timeout does not prove queued SD writes
    have finished; keep the card installed after an aborted test.
    """
    if frequencyHz not in (0.5, 1000) or (frequencyHz == 1000 and scopeLevels is None):
        raise ValueError('1 kHz requires external-generator mode')
    import serial
    if seconds not in (30, 300, 600):
        raise ValueError('Duration must be 30, 300 or 600 seconds')
    if featherPort.upper() == p4Port.upper():
        raise ValueError('Feather and P4 must use different ports')
    if scopeLevels is not None:
        from externalGeneratorReport import validateLevels
        validateLevels(scopeLevels)
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
        configured = set()
        captureStarted = None
        prepareStarted = None
        nextPrepareProgress = 15
        lastPreparation = 'P4 has not reported receiving the Feather request'
        nextProgress = 15
        nonce = time.time_ns() % 4000000000 + 1
        linkReplies = set()
        linkQueued = False
        nextLinkQuery = 0
        lastLinkStatus = 'P4 has not answered a polling-status query'
        deadline = time.monotonic() + 12
        print('Checking boards...')
        while not finished:
            if time.monotonic() >= deadline:
                reasons = {'identity': 'Boards did not identify as idle smoke firmware; close monitors and reset both boards',
                           'linkArm': 'P4 did not arm the link diagnostic; upload diagnostic firmware',
                           'link': 'Serial probe failed: ' + lastLinkStatus,
                           'clock': 'P4 did not accept UTC; check its log and reset before retrying',
                           'configure': 'Both boards must acknowledge the selected duration before starting',
                           'prepare': 'Storage or receiver did not become ready; check both logs',
                           'capture': 'Recording or SD readback did not finish'}
                detail = '; last preparation status: ' + lastPreparation if phase == 'prepare' else ''
                raise ValueError(reasons[phase] + detail)
            if phase == 'link' and time.monotonic() >= nextLinkQuery:
                ports['P4'].write(b'@LINKSTATUS\n')
                nextLinkQuery = time.monotonic() + 2
            if phase == 'prepare' and prepareStarted is not None:
                elapsed = int(time.monotonic() - prepareStarted)
                if elapsed >= nextPrepareProgress:
                    print(f'Preparation: {elapsed}s elapsed; {lastPreparation}', flush=True)
                    nextPrepareProgress = elapsed + 15
            if phase == 'capture' and captureStarted is not None:
                elapsed = int(time.monotonic() - captureStarted)
                if elapsed >= nextProgress:
                    if nextProgress <= seconds:
                        print(f'Recording: {min(elapsed, seconds)} / {seconds} seconds', flush=True)
                        nextProgress += 15
                    else:
                        print('Capture time elapsed; waiting for readback and reports...', flush=True)
                        nextProgress = 10**9
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
                if 'task_wdt:' in line or 'Task watchdog got triggered' in line:
                    raise ValueError(name + ' watchdog reported a task running too long; serial report may be interrupted. See its board log.')
                if any(marker in line for marker in ('Guru Meditation Error', 'ESP-ROM:', 'Rebooting...', 'abort() was called')):
                    raise ValueError(name + ' crashed or rebooted; see its log')
                if 'ready=0' in line and line.startswith('SMOKE_'):
                    raise ValueError(name + ' is not ready; reset both boards before retrying')
                if phase == 'identity':
                    expected = 'SMOKE_FEATHER' if name == 'Feather' else 'SMOKE_P4'
                    version = (6 if scopeLevels is not None else 4) if name == 'Feather' else (8 if frequencyHz == 1000 else (7 if scopeLevels is not None else 5))
                    compatibleP4 = name == 'P4' and (re.fullmatch(r'SMOKE_P4 version=8 ready=1 seconds=(30|300|600)', line) or (frequencyHz == 0.5 and scopeLevels is None and re.fullmatch(r'SMOKE_P4 version=7 ready=1 seconds=(30|300|600)', line)))
                    if re.fullmatch(expected + rf' version={version} ready=1 seconds=(30|300|600)', line) or compatibleP4:
                        ready.add(name)
                    elif line.startswith(expected + ' version='):
                        raise ValueError(f'Wrong {name} firmware: expected version {version}; received {line}')
                    if len(ready) == 2:
                        # Arm receive diagnostics first so the probe cannot arrive
                        # before raw-byte reporting starts. No storage is touched.
                        ports['P4'].write(f'@LINK {nonce}\n'.encode('ascii'))
                        phase = 'linkArm'
                        deadline = time.monotonic() + 10
                        print('Checking Feather/P4 serial link (10-second limit)...', flush=True)
                elif phase == 'linkArm':
                    if line.startswith('LINK rejected'):
                        raise ValueError(f'{name}: {line}')
                    if name == 'P4' and line == f'LINK ARMED {nonce}':
                        ports['Feather'].write(f'@LINK {nonce}\n'.encode('ascii'))
                        phase = 'link'
                elif phase == 'link':
                    if line.startswith(('LINK', 'ADC127 LINK_OK')):
                        print(f'{name}: {line}', flush=True)
                    if name == 'P4' and line.startswith('LINK STATUS'):
                        lastLinkStatus = line
                    if line.startswith(('LINK rejected', 'TEST rejected')):
                        raise ValueError(f'{name}: {line}; upload diagnostic firmware on both boards')
                    queued = re.fullmatch(r'LINK TX nonce=(\d+) queued=(\d+) expected=(\d+) RX=\d+ TX=\d+', line)
                    if name == 'Feather' and queued and int(queued[1]) == nonce:
                        expectedBytes = len(f'ADC127 LINK {nonce}\n'.encode('ascii'))
                        linkQueued = int(queued[2]) == int(queued[3]) == expectedBytes
                        if not linkQueued:
                            raise ValueError('Feather did not queue the complete serial probe')
                    if line == f'ADC127 LINK_OK {nonce}':
                        linkReplies.add(name)
                    if linkQueued and linkReplies == {'Feather', 'P4'}:
                        print('Serial link PASS: complete probe acknowledged in both directions.', flush=True)
                        if linkOnly:
                            (session / 'result.txt').write_text('PASS: serial link only; ADC and SD not started\n', encoding='utf-8')
                            return
                        ports['P4'].write(f'@UTC {int(time.time())}\n'.encode('ascii'))
                        phase = 'clock'
                        deadline = time.monotonic() + 12
                elif phase == 'clock':
                    if name == 'P4' and line == 'UTC rejected':
                        raise ValueError('P4 rejected UTC; reset it before retrying')
                    if name == 'P4' and line == 'UTC OK':
                        for port in ports.values():
                            port.write(f'@TEST {seconds}\n'.encode('ascii'))
                        phase = 'configure'
                        deadline = time.monotonic() + 12
                elif phase == 'configure':
                    if line == 'TEST rejected':
                        raise ValueError(name + ' rejected the selected duration; reset both boards')
                    if line == f'TEST OK seconds={seconds}':
                        configured.add(name)
                    if len(configured) == 2:
                        # Mark started before requesting the capture so an
                        # interrupted command still requests source shutdown.
                        started = True
                        ports['Feather'].write(b'g')
                        phase = 'prepare'
                        prepareStarted = time.monotonic()
                        deadline = prepareStarted + 10
                        print('Preparing SD card and receiver...', flush=True)
                elif phase == 'prepare':
                    if name == 'P4' and line == 'PREP: Feather request received':
                        # The short command-delivery deadline ends only when P4
                        # recognizes ARM. Actual disk preparation gets its own limit.
                        deadline = time.monotonic() + 200
                    if line.startswith('LINK'):
                        print(f'{name}: {line}', flush=True)
                    armQueued = re.fullmatch(r'LINK ARM queued=(\d+) expected=(\d+)', line)
                    if name == 'Feather' and armQueued and armQueued[1] != armQueued[2]:
                        raise ValueError('Feather did not queue the complete preparation command')
                    # Show actual milestones without claiming that an unreported
                    # step completed. Keep the last one beside timeout/failure text.
                    if name == 'P4' and line.startswith(('PREP:', 'LINK ERROR:', 'ADC127 READY3')):
                        lastPreparation = line
                        print(f'P4: {line}', flush=True)
                    if name == 'Feather' and line.startswith('ADC START echo confirmed.'):
                        phase = 'capture'
                        captureStarted = time.monotonic()
                        deadline = captureStarted + seconds + 300
                        print(f'Recording {seconds} seconds, then reading retained SD files back...')
                    elif ('FAILED' in line or 'PREPARE FAILED' in line or
                          line.startswith(('No matching receiver READY', 'Test already pending/running',
                                           'Source/ADC START failed', 'DAC setup failed',
                                           'ADC127 FAULT', 'ADC127 TIMEOUT'))):
                        raise ValueError(f'{name}: {line}; last preparation status: {lastPreparation}')
                elif phase == 'capture':
                    refused = ('No matching receiver READY', 'ADC127 FAILED',
                               'Source/ADC START failed', 'DAC setup failed',
                               'Test already pending/running')
                    if name == 'Feather' and line.startswith(refused):
                        raise ValueError('ADC capture could not start; check the Feather log')
                    if name == 'Feather' and line.startswith('EXTERNAL_END run=' if scopeLevels is not None else 'INPUT_END run='):
                        ended.add(name)
                    if name == 'P4' and line.startswith('SMOKE_SD_END run='):
                        ended.add(name)
                    finished = len(ended) == 2
        if scopeLevels is None:
            result = analyze('\n'.join(records['Feather']), '\n'.join(records['P4']),
                             showProgress, session / 'comparison.html')
        else:
            from hardwareSmokeRetainedReport import analyzeRetained
            result = analyzeRetained('\n'.join(records['Feather']), '\n'.join(records['P4']),
                                     showProgress, session / 'comparison.html', scopeLevels, frequencyHz)
        (session / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        report = summary(result)
        if result['calibration'].startswith('ESTIMATE'):
            report += '\nInput voltage uses nominal calibration; absolute accuracy is limited.'
        (session / 'result.txt').write_text(report + '\n', encoding='utf-8')
        print(summary(result, includeChecks=False))
        if not result['passed']:
            raise ValueError('External waveform checks failed; see comparison.html and result.json')
        if result['calibration'].startswith('ESTIMATE'):
            print('Input voltage uses nominal calibration; absolute accuracy is limited.')
    except BaseException as error:
        # Preserve the full measured error report when the waveform failed after
        # analysis; early failures have only the checks established so far.
        if not (session / 'result.txt').exists():
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
        if scopeLevels is not None:
            print('Rigol output remains under manual control; switch it off before removing ADC power.')


def main():
    """Select ports/output from command line; return a nonzero exit code on failure."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--feather', default='COM4')
    parser.add_argument('--p4', default='COM3')
    parser.add_argument('--seconds', type=int, choices=(30, 300, 600), help='Omit to select duration from a menu')
    parser.add_argument('--external', action='store_true', help='Use external-generator Feather firmware; DAC stays disabled')
    parser.add_argument('--frequency', type=float, choices=(0.5, 1000), default=0.5, help='External square-wave frequency in Hz')
    parser.add_argument('--scope-low', type=float, help='Measured scope Vbase in volts')
    parser.add_argument('--scope-high', type=float, help='Measured scope Vtop in volts')
    parser.add_argument('--link-only', action='store_true', help='Check serial link only; do not start ADC or prepare SD')
    parser.add_argument('--output', type=Path, default=Path('captures/hardware-smoke'))
    args = parser.parse_args()
    result = 0
    try:
        if args.frequency == 1000 and not args.external:
            raise ValueError('--frequency 1000 requires --external')
        scopeLevels = None
        if args.external:
            from externalGeneratorReport import validateLevels
            low = args.scope_low
            high = args.scope_high
            if low is None:
                low = float(input('Scope Vbase (low), volts: '))
            if high is None:
                high = float(input('Scope Vtop (high), volts: '))
            validateLevels((low, high))
            scopeLevels = (low, high)
            print('External source: Feather DAC disabled; generator is controlled manually.')
        elif args.scope_low is not None or args.scope_high is not None:
            raise ValueError('Scope readings require --external')
        seconds = args.seconds
        if args.link_only and seconds is None:
            seconds = 30  # Unused by the probe; bypass the recording-duration menu.
        if seconds is None:
            print('Recording duration:\n1. 30 seconds\n2. 5 minutes\n3. 10 minutes')
            while seconds is None:
                seconds = {'1': 30, '2': 300, '3': 600}.get(input('Select [1/2/3]: ').strip())
                if seconds is None:
                    print('Enter 1, 2 or 3.')
        capture(args.feather, args.p4, args.output, seconds, args.link_only, scopeLevels, args.frequency)
    except (OSError, ValueError, ImportError, KeyboardInterrupt) as error:
        print('FAIL:', str(error) or 'Interrupted')
        print('Keep the SD card installed if recording/readback was interrupted.')
        result = 1
    return result


if __name__ == '__main__':
    raise SystemExit(main())
