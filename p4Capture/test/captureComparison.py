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

"""Collect simultaneous Feather/P4 RAM images and compare waveform shape.

Karl runs this with both boards and the ADC powered, A1->A2 jumper and TI coax
connected. Close both serial monitors and reset P4 before running. No SD commands.
Two PC reader threads drain USB concurrently; acquisition starts only after both
firmware modes have been positively identified. Requires pyserial, not numpy.
"""
import argparse
from pathlib import Path
import queue
import threading
import time
from compareWaveforms import analyze

# Seconds, not firmware run lengths: allow boot/USB setup, then enough time for
# the bounded RAM capture, stop handshake and text report to cross 115200-baud USB.
preparationTimeoutSeconds = 12
captureTimeoutSeconds = 20
# A valid INPUT/WAVE/CAL row is much shorter; cap an unterminated serial line.
maximumLineBytes = 8192


def readPort(port, name, events, stop):
    """Drain a port independently and join partial lines across read timeouts."""
    pending = bytearray()
    try:
        while not stop.is_set():
            pending.extend(port.read(max(1, port.in_waiting)))
            while b'\n' in pending:
                raw, _, rest = pending.partition(b'\n')
                pending = bytearray(rest)
                events.put((name, raw.decode('utf-8', errors='replace').strip()))
            if len(pending) > maximumLineBytes:
                raise ValueError('Overlong serial line; refusing malformed stream')
    except Exception as error:
        events.put((name, f'READER_ERROR {error}'))


def capture(inputPort, outputPort, folder, constant=False):
    """Capture paired RAM reports and save their analysis in a new session folder.

    inputPort names Feather, outputPort names P4, and folder holds timestamped
    sessions. constant selects fixed-code firmware and a no-fit voltage report.
    Reader threads only collect bytes; this calling thread owns log files, mode
    checks and start/stop commands. Timeout/validation failures raise ValueError
    with logs retained; finally closes ports and requests stop if still unfinished.
    Return the analysis dictionary for waveform mode, or the HTML path for constant
    mode, matching the selected report function. Opening ports does not upload code.
    """
    import serial
    if inputPort.upper() == outputPort.upper():
        raise ValueError('Feather and P4 must use different ports')
    folder.mkdir(parents=True, exist_ok=True)
    # A dedicated folder keeps complete raw evidence alongside every analysis.
    session = folder / time.strftime('%Y%m%d-%H%M%S')
    session.mkdir(exist_ok=False)
    ports, threads, files = {}, [], {}
    records = {'Feather': [], 'P4': []}
    ready, ended = set(), set()
    events, stop = queue.Queue(), threading.Event()
    started = False
    constantIdentified = False
    modeIdentified = False
    try:
        for name, address in (('Feather', inputPort), ('P4', outputPort)):
            port = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=1)
            port.dtr = port.rts = False
            port.port = address
            ports[name] = port
            port.open()
            files[name] = (session / (name.lower() + '.txt')).open('w', encoding='utf-8')
        time.sleep(2)
        for name, port in ports.items():
            # Discard pre-session boot/old output before requesting mode identity.
            port.reset_input_buffer()
            thread = threading.Thread(target=readPort, args=(port, name, events, stop), daemon=True)
            thread.start()
            threads.append(thread)
        # Ask for identities before starting: h reports source mode, w locks
        # P4 to RAM. Stage 6 is sent only after both boards and source mode agree.
        ports['Feather'].write(b'h')
        ports['P4'].write(b'w')
        deadline = time.monotonic() + preparationTimeoutSeconds
        print('Checking Feather comparison firmware and P4 RAM mode...')
        while time.monotonic() < deadline and len(ended) < 2:
            try:
                name, line = events.get(timeout=0.2)
            except queue.Empty:
                name, line = None, None
            if name is not None:
                records[name].append(line)
                files[name].write(line + '\n')
                files[name].flush()
                if not line.startswith(('INPUT,', 'WAVE,')):
                    print(f'{name}: {line}')
                if line.startswith('READER_ERROR') or 'Mode already selected' in line or 'COMPARE UNAVAILABLE' in line:
                    raise ValueError('Preparation failed. Close readers, reset P4 and check Feather firmware; logs retained.')
                if name == 'Feather' and line.startswith('COMPARE READY:'):
                    ready.add(name)
                if name == 'Feather' and line.startswith('DAC MODE:'):
                    modeIdentified = True
                    constantIdentified = line.startswith('DAC MODE: CONSTANT code=135 ')
                    if constant != constantIdentified:
                        raise ValueError('DAC firmware mode does not match --constant selection; no capture started.')
                if name == 'P4' and line.startswith('RAM READY:'):
                    ready.add(name)
                if len(ready) == 2 and not started and modeIdentified:
                    ports['Feather'].write(b'6')
                    started = True
                    deadline = time.monotonic() + captureTimeoutSeconds
                    print('Starting one simultaneous capture. No SD access.')
                if started and name == 'Feather' and line.startswith('INPUT_END run='):
                    ended.add(name)
                if started and name == 'P4' and line.startswith('WAVE_END run='):
                    ended.add(name)
        if len(ended) != 2:
            raise ValueError('Timed out before both complete reports. Logs retained; check board stop messages.')
        if constant:
            from constantReport import analyzeConstant
            return analyzeConstant('\n'.join(records['Feather']), '\n'.join(records['P4']), session / 'constant.html')
        return analyze('\n'.join(records['Feather']), '\n'.join(records['P4']), session / 'comparison.html')
    finally:
        # Abort on an interrupted/failed PC session as well. Firmware has its
        # own bounded stop, but USB disconnection is not proof of stopped ADC.
        feather = ports.get('Feather')
        if started and feather is not None and feather.is_open and len(ended) != 2:
            try:
                feather.write(b's')
                time.sleep(0.3)
            except serial.SerialException:
                pass
        stop.set()
        for thread in threads:
            thread.join(timeout=1)
        for port in ports.values():
            port.close()
        for log in files.values():
            log.close()
        print('Capture folder:', session.resolve())


def main():
    """Open only the explicitly named serial ports; never build or upload firmware."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--feather', default='COM4')
    parser.add_argument('--p4', default='COM3')
    parser.add_argument('--output', type=Path, default=Path('work/waveform-comparison'))
    parser.add_argument('--constant', action='store_true', help='Require constant-code firmware; report fluctuations without fitting')
    args = parser.parse_args()
    capture(args.feather, args.p4, args.output, args.constant)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as error:
        raise SystemExit(str(error))
