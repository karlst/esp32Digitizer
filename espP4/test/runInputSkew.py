# Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
"""Run P4-only GPIO1/GPIO6 calibration after disconnecting FSYNC from GPIO1.

Use the same approximately 997-Hz, 50% generator signal on both inputs. No ADC,
Feather or SD commands. The runner requires the dedicated input-skew firmware.

Read main -> capture -> readLine, then inputSkewReport.analyze. A run consists of
firmware identity query h, one finite capture request g, and its complete report.
The PC's deadlines govern waiting only; the P4 hardware timestamps each edge.
"""
import argparse
import json
from pathlib import Path
import time
from inputSkewReport import analyze, writeReport


def capture(address, runs, output):
    """Collect finite hardware runs; retain every console line and partial result.

    DTR and RTS stay low to avoid intentional resets. A timeout closes the port;
    firmware stops its own capture after 100 ms even if the PC disconnects.

    address is the P4 serial port; runs is 1..100; output is the parent directory
    for a new session. Results are written after each trial, not returned. Errors
    propagate to main with the completed log lines and earlier results retained.
    """
    import serial
    if not 1 <= runs <= 100:
        raise ValueError('Use 1 through 100 runs')
    session = output / time.strftime('%Y%m%d-%H%M%S')
    session.mkdir(parents=True, exist_ok=False)
    port = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=1)
    port.dtr = port.rts = False
    port.port = address
    results = []
    try:
        port.open()
        time.sleep(2)
        port.reset_input_buffer()
        with (session / 'p4.txt').open('w', encoding='utf-8') as log:
            pending = bytearray()

            def readLine(deadline):
                """Return one logged line before the absolute monotonic deadline.

                USB reads may split a line or deliver several at once. pending
                keeps bytes after the first newline for the next call; a 16-KiB
                limit rejects a stream without line endings. This runs in the
                calling thread: there is only one port and no reader thread here.
                """
                while b'\n' not in pending:
                    if time.monotonic() >= deadline:
                        raise ValueError('Timed out waiting for P4; inspect p4.txt')
                    pending.extend(port.read(max(1, min(port.in_waiting, 4096))))
                    if len(pending) > 16384:
                        raise ValueError('Oversized or damaged P4 console line')
                raw, _, remainder = pending.partition(b'\n')
                pending[:] = remainder
                line = raw.decode('utf-8', errors='replace').strip()
                log.write(line + '\n')
                log.flush()
                if any(marker in line for marker in ('Guru Meditation', 'abort() was called',
                                                     'task_wdt:', 'ESP-ROM:', 'Rebooting...')):
                    raise ValueError('P4 reported a crash/reset: ' + line)
                return line

            for iRun in range(runs):
                # Require the correct idle firmware before sending g. This avoids
                # interpreting the same command in a different diagnostic image.
                port.write(b'h')
                deadline = time.monotonic() + 5
                ready = False
                while not ready:
                    line = readLine(deadline)
                    ready = line == 'INPUT_SKEW_P4 version=1 ready=1 hz=160000000 gpioA=1 gpioB=6'
                    if 'version=' in line and not ready:
                        raise ValueError('Wrong firmware or P4 not idle: ' + line)
                print(f'Input comparison {iRun + 1}/{runs}: capturing both pins...', flush=True)
                port.write(b'g')
                # Sending g requests capture; SKEW RESULT ends the transcript.
                # Only analyze can establish that the two event streams match.
                lines = []
                deadline = time.monotonic() + 15
                finished = False
                while not finished:
                    line = readLine(deadline)
                    lines.append(line)
                    if line.startswith('SKEW rejected'):
                        raise ValueError(line)
                    finished = line.startswith('SKEW RESULT ')
                result = analyze(lines)
                if any(previous['run'] == result['run'] for previous in results):
                    raise ValueError('Repeated run number')
                results.append(result)
                # Preserve completed trials before starting another. writeReport
                # redraws the combined HTML from saved observations, not a new run.
                (session / 'result.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
                writeReport(session / 'comparison.html', results)
                print(f"MEASURED GPIO1 minus GPIO6: rising mean {result['rising']['meanNs']:+.3f} ns; "
                      f"falling mean {result['falling']['meanNs']:+.3f} ns; {len(result['pairs'])} matched edges", flush=True)
            (session / 'result.txt').write_text(
                f'MEASURED: {runs} input comparisons. Positive means GPIO1 later than GPIO6.\n'
                'Relative input and jumper-path timing only; no absolute-latency accuracy claim.\n', encoding='utf-8')
    except (Exception, KeyboardInterrupt) as error:
        (session / 'result.txt').write_text('FAIL: ' + (str(error) or 'Interrupted') + '\n', encoding='utf-8')
        raise
    finally:
        port.close()
        print('Logs:', session.resolve())


def main():
    """Read explicit capture options; no firmware build or upload is performed."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--p4', default='COM3')
    parser.add_argument('--runs', type=int, default=5)
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[2] / 'captures/input-skew')
    args = parser.parse_args()
    try:
        capture(args.p4, args.runs, args.output)
    except (Exception, KeyboardInterrupt) as error:
        print('FAIL:', str(error) or 'Interrupted')
        raise SystemExit(1)


if __name__ == '__main__':
    main()
