# Copyright (C) 2026 Karl Stock
# SPDX-License-Identifier: GPL-3.0-or-later
"""Set P4 UTC from the PC, select a fresh/add-files choke run, and retain its output.

Karl runs this after uploading and resetting the choke firmware. It checks the
firmware identity before setting time or sending the selected run command. It does not build or upload.
The PC's clock must be correct. Serial transfer adds small synchronization error;
this is a readable file timestamp, not precision timing of ADC samples.
"""
import argparse
from datetime import datetime, timezone
from pathlib import Path
import time

import serial


def readLine(port, log):
    """Read a bounded console line, print it, and save it to the full run log."""
    line = port.read_until(b'\n', 2048).decode('utf-8', errors='replace').strip()
    if line:
        print(line, flush=True)
        log.write(line + '\n')
        log.flush()
    return line


def waitFor(port, log, prefix, seconds):
    """Wait for a specific acknowledgement before sending the next command."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        line = readLine(port, log)
        if line.startswith(prefix):
            return
        if 'rejected' in line:
            raise RuntimeError(f'Command rejected while waiting for {prefix!r}: {line}')
        if line.startswith('CHOKE version='):
            raise RuntimeError(f'Expected idle timestamp-capable firmware; received: {line}')
    raise RuntimeError('P4 did not acknowledge; check the port, firmware and reset state.')


def main():
    """Open P4 only, verify idle firmware, set UTC, run the selection, and return its result."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--p4', default='COM3')
    parser.add_argument('--test', choices=('fresh', 'addFiles', 'existing'), help='Omit for the startup menu')
    parser.add_argument('--timeout', type=float, default=900,
                        help='Total run/readback timeout in seconds; default 900')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('--timeout must be positive')

    # Choosing fresh is the erase request; there is no second confirmation prompt.
    # Add-files never falls back to formatting if mount or allocation fails.
    selected = args.test
    if selected is None:
        print('1. Fresh disk: format, preallocate, run (ERASES card)')
        print('2. Add files: preserve existing files, preallocate additional files, run')
        print('3. Use existing / stop when full: preserve old files, prepare unused pool, test clean stop')
        while selected is None:
            choice = input('Test [1/2/3]: ').strip()
            selected = {'1': 'fresh', '2': 'addFiles', '3': 'existing'}.get(choice)
            if selected is None:
                print('Enter 1, 2 or 3.')

    # A unique directory preserves the complete history, including early overflow.
    repoRoot = Path(__file__).resolve().parents[2]
    session = repoRoot / 'captures' / 'sd-choke' / datetime.now(timezone.utc).strftime('%Y%m%d-%H%M%S-%fZ')
    session.mkdir(parents=True)
    logPath = session / 'p4.txt'
    print(f'Log: {logPath}', flush=True)
    started = False
    port = serial.Serial(port=None, baudrate=115200, timeout=0.5, write_timeout=2)
    port.dtr = port.rts = False
    port.port = args.p4
    try:
        with logPath.open('w', encoding='utf-8') as log:
            log.write(f'Selected test: {selected}\n')
            log.flush()
            port.open()
            # Allow USB/board startup to settle without deliberately resetting it.
            time.sleep(2)
            # Finish any partial console line before asking for identity. Read
            # and retain the boundary response, but never treat it as an ACK.
            port.write(b'\n')
            boundaryDeadline = time.monotonic() + 0.5
            while time.monotonic() < boundaryDeadline:
                readLine(port, log)
            port.reset_input_buffer()
            port.write(b'h\n')
            waitFor(port, log, 'CHOKE version=5 idle=1 ', 10)
            utcSeconds = int(time.time())
            log.write(f'PC UTC supplied: {datetime.fromtimestamp(utcSeconds, timezone.utc).isoformat()}\n')
            port.write(f'time {utcSeconds}\n'.encode('ascii'))
            waitFor(port, log, 'CHOKE UTC OK', 10)

            # The chosen mode travels explicitly; neither side guesses it.
            print(f'Starting {selected} test with the firmware settings.', flush=True)
            port.write(f'run {selected}\n'.encode('ascii'))
            started = True
            deadline = time.monotonic() + args.timeout
            while time.monotonic() < deadline:
                line = readLine(port, log)
                # A panic or a new boot means the test cannot finish. Leave the
                # serial port open briefly to retain the crash dump in its log.
                if any(marker in line for marker in ('Guru Meditation Error', 'ESP-ROM:', 'Rebooting...', 'abort() was called')):
                    started = False
                    crashDeadline = time.monotonic() + 3
                    while time.monotonic() < crashDeadline:
                        readLine(port, log)
                    raise RuntimeError(f'P4 crashed or rebooted during the test; see {logPath}')
                if line.startswith(('PASS:', 'FAIL:')):
                    started = False
                    return 0 if line.startswith('PASS:') else 1
                if line.startswith('CHOKE start rejected'):
                    raise RuntimeError(line)
            raise RuntimeError('Timed out waiting for run/readback completion; see the saved log.')
    except (RuntimeError, serial.SerialException, OSError, KeyboardInterrupt) as error:
        print(f'Stopped: {error or "interrupted"}', flush=True)
        return 1
    finally:
        # An interrupted PC session requests a graceful stop, not a card reset.
        # Finishing queued writes may continue after this port closes.
        if port.is_open:
            if started:
                try:
                    port.write(b's\n')
                except serial.SerialException:
                    pass
            port.close()


if __name__ == '__main__':
    raise SystemExit(main())
