# Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
"""Run FSYNC diagnostics; SD writes require --pps-sd-test or --timed-sd-test.

Karl runs this after flashing the P4 FSYNC build. Feather must identify as the
external-source bench firmware (version 6), which leaves its DAC disabled.
For association-only runs, existing DCLK/FSYNC/DOUT0 and UART wiring suffices;
leave the Rigol output OFF. runPulseDelay.py imports capture with delay=True
and needs the verified external waveform and GPIO6 connection instead.
PPS modes require an external 1-Hz input on GPIO6. --pps-sd-test additionally
creates one new recording through the real SD writer; it preserves existing
files. PC calendar time labels that file but is not GPS synchronization.
--timed-sd-test writes version-1 tBlock/sBlock files using a synthetic 2000-01-01
UTC origin. Complete intervals only are retained, with full SD CRC readback.
The timed test acquires for five minutes and rotates across five preserved
files, with 60 pairs per file. Other PPS tests still acquire for one minute.

Reading path: main selects ports/options; capture opens both USB consoles,
identifies firmware, checks the separate board-to-board serial link once, then
requests captures. nextLine logs received lines and enforces deadlines. The
report modules validate completed transcripts; finally releases PC resources.
The PC does not timestamp physical pulses: hardware capture on P4 does that.
"""
import argparse
import json
from pathlib import Path
import queue
import re
import threading
import time
from captureComparison import readPort
from fsyncAssociationReport import analyze
from ppsBoundaryReport import analyzeBoundaries
from timedCaptureReport import parseSaved as parseTimedSaved, validateSaved as validateTimedSaved


def capture(featherPort, p4Port, runs, output, delay=False, linkOnly=False, frequencyHz=997, readTest=False, indexTest=False, ppsTest=False, sdTest=False, timedTest=False):
    """Identify both boards before each run, then request the selected acquisition.

    Reader threads keep USB logs flowing while Core 0 waits for the source-stop
    acknowledgement. A deadline or Ctrl+C sends Feather 's' as a best-effort stop.
    DTR and RTS stay low so opening a port does not intentionally reset a board.
    The pulse-delay entry point passes delay=True: require its distinct firmware,
    wait for exported ADC windows, and write its response graph. Ordinary calls
    retain the association-only checks and do not require a generator signal.

    featherPort/p4Port are Windows port names; runs is the requested trial count.
    output is the parent of a new session directory. linkOnly returns after the
    communication probe without starting acquisition. frequencyHz is the expected
    generator rate used by delay analysis, not a generator-control command.
    readTest selects a ten-second register-read experiment, with the same real
    receive/decode path but no association analysis. READ rows retain statistics.
    indexTest selects timestamp-derived pulse numbering, with PCNT comparison
    only. INDEX rows report disagreements and rounding margins, not a PPS test.
    ppsTest runs sixty seconds across timer wraps and configures Feather
    safety timeout to 300 seconds (reset restores its default). It adds GPIO6
    rising-edge capture on the shared timer and requires an
    external 1-Hz source and validates numbered FSYNC references, not UTC dates.
    sdTest selects the explicit combined SD/PPS mode. It preserves old files,
    writes new three-byte samples and waits for CRC-verified SD SAVED as well as
    timing success and Feather DONE. It permits only one run per invocation.
    timedTest selects version-1 tBlock/sBlock files instead of sample-only files;
    the board uses a synthetic UTC origin and reports independently checked
    complete-interval counts. It runs for 300 seconds with five 60-pair files
    and a 600-second Feather safety timeout. The host allows 20 minutes for SD
    preparation, acquisition, shutdown, console output and full readback.
    Results go to files, not a return value; exceptions retain logs and propagate
    to main, which exits unsuccessfully. A completed link probe proves a reply,
    not that the ADC can produce valid samples.
    """
    if timedTest:
        if sdTest:
            raise ValueError('Select either --timed-sd-test or --pps-sd-test, not both')
        sdTest = True
    if sdTest:
        if readTest or indexTest or delay or linkOnly or ppsTest or runs != 1:
            raise ValueError('Use one SD test option alone with --runs 1; reset between SD sessions')
        ppsTest = True
    if indexTest and (readTest or delay or linkOnly or ppsTest):
        raise ValueError("Index mode cannot be combined with another diagnostic mode")
    if ppsTest and (readTest or delay or linkOnly):
        raise ValueError("PPS mode cannot be combined with another diagnostic mode")
    import serial
    from pulseDelayReport import analyze as analyzeDelay, writeReport
    if featherPort.upper() == p4Port.upper() or not 1 <= runs <= 100:
        raise ValueError('Use different ports and 1 through 100 runs')
    session = output / time.strftime('%Y%m%d-%H%M%S')
    session.mkdir(parents=True, exist_ok=False)
    ports, logs, threads = {}, {}, []
    # One reader thread per USB port prevents a busy board hiding the other's
    # replies. The queue transfers complete (board-name, line) pairs to this
    # thread, which alone writes logs and advances the command sequence. stop
    # asks the readers to exit; it does not send a hardware stop command.
    events, stop = queue.Queue(), threading.Event()
    results = []
    sourceRequested = False

    def nextLine(deadline, queryLink=False):
        """Return the next (board-name, line), saving it before validation.

        deadline is an absolute time.monotonic value in seconds, so unrelated
        output cannot extend the wait forever. queryLink requests diagnostic
        counters while awaiting a probe response; it is false during acquisition.
        Reader-thread errors and reset/crash messages raise ValueError here.
        """
        retVal = None
        nextStatus = time.monotonic() + 1
        while retVal is None:
            if time.monotonic() >= deadline:
                raise ValueError('Timed out waiting for board response; inspect the saved logs')
            # While diagnosing a silent board-to-board link, query the P4 over
            # its independent USB console. These counters distinguish a live
            # polling loop with no incoming bytes from a stalled application.
            if queryLink and time.monotonic() >= nextStatus:
                ports['P4'].write(b'@LINKSTATUS\n')
                nextStatus = time.monotonic() + 1
            try:
                name, line = events.get(timeout=0.1)
            except queue.Empty:
                name, line = None, None
            if name is not None:
                logs[name].write(line + '\n')
                logs[name].flush()
                if any(marker in line for marker in ('READER_ERROR', 'Guru Meditation', 'abort() was called',
                                                     'task_wdt:', 'ESP-ROM:', 'Rebooting...')):
                    raise ValueError(name + ': ' + line)
                retVal = (name, line)
        return retVal

    try:
        # Open consoles without deliberately toggling DTR/RTS reset lines.
        # 0.1-s read timeouts let threads notice stop; a 1-s write timeout avoids
        # hanging forever on a disconnected board. No firmware is uploaded.
        for name, address in (('Feather', featherPort), ('P4', p4Port)):
            port = serial.Serial(port=None, baudrate=115200, timeout=0.1, write_timeout=1)
            port.dtr = port.rts = False
            port.port = address
            ports[name] = port
            port.open()
            logs[name] = (session / (name.lower() + '.txt')).open('w', encoding='utf-8')
        time.sleep(2)
        # Allow USB startup to settle, discard old output, then begin logging
        # this session's requested identities and measurements. Readers never
        # interpret firmware commands or write to either board.
        for name, port in ports.items():
            port.reset_input_buffer()
            thread = threading.Thread(target=readPort, args=(port, name, events, stop), daemon=True)
            thread.start()
            threads.append(thread)

        for iRun in range(runs):
            # Re-query readiness after every completed run. Version checks keep
            # this script from accidentally launching a normal SD smoke run.
            for port in ports.values():
                port.write(b'h')
            ready = set()
            deadline = time.monotonic() + 10
            while len(ready) < 2:
                name, line = nextLine(deadline)
                expectedP4 = 'PULSE_DELAY_P4 version=2 ready=1' if delay else 'FSYNC_P4 version=3 ready=1'
                if name == 'P4' and line == expectedP4:
                    ready.add(name)
                elif name == 'Feather' and re.fullmatch(r'SMOKE_FEATHER version=6 ready=1 seconds=(30|300|600)', line):
                    ready.add(name)
                elif line.startswith(('FSYNC_P4', 'PULSE_DELAY_P4', 'SMOKE_P4', 'SMOKE_FEATHER')):
                    raise ValueError('Wrong firmware or board not idle: ' + line)

            # The SD header requires a calendar date before preparation. Supply
            # PC time while the destination is still unlocked and require the
            # board's acknowledgement. This labels files only; it does not
            # establish GPS time or alter hardware pulse timestamps.
            if sdTest:
                ports['P4'].write(f'@UTC {int(time.time())}\n'.encode('ascii'))
                deadline = time.monotonic() + 10
                clockSet = False
                while not clockSet:
                    name, line = nextLine(deadline)
                    if name == 'P4' and line == 'UTC OK':
                        clockSet = True
                    elif name == 'P4' and line.startswith('UTC rejected'):
                        raise ValueError('P4 rejected PC time; reset both boards before retrying')

            # Link diagnostics must be armed before the first capture: firmware
            # locks destination selection after that, even while idle. The
            # completed capture handshakes check communication on later trials.
            if iRun == 0:
                # Check both directions before asking the ADC to start. P4 first
                # enables raw receive diagnostics, then Feather sends a numbered
                # probe. Both USB logs must acknowledge that exact probe number.
                # This uses existing firmware commands and performs no acquisition.
                nonce = time.monotonic_ns() & 0xffffffff
                # The nonce is a fresh 32-bit probe identifier, not a timing
                # measurement. Matching it rules out accepting an old reply.
                ports['P4'].write(f'@LINK {nonce}\n'.encode('ascii'))
                deadline = time.monotonic() + 10
                armed = False
                while not armed:
                    name, line = nextLine(deadline, queryLink=True)
                    if name == 'P4' and line == f'LINK ARMED {nonce}':
                        armed = True
                    if line.startswith('LINK rejected'):
                        raise ValueError(name + ': ' + line)
                ports['Feather'].write(f'@LINK {nonce}\n'.encode('ascii'))
                print('Checking Feather/P4 serial link; ADC remains stopped...', flush=True)
                deadline = time.monotonic() + 10
                replies = set()
                queued = False
                while replies != {'Feather', 'P4'} or not queued:
                    name, line = nextLine(deadline, queryLink=True)
                    if line.startswith(('LINK', 'ADC127 LINK_OK')):
                        print(f'{name}: {line}', flush=True)
                    transmitted = re.fullmatch(r'LINK TX nonce=(\d+) queued=(\d+) expected=(\d+) RX=\d+ TX=\d+', line)
                    if name == 'Feather' and transmitted and int(transmitted[1]) == nonce:
                        expectedBytes = len(f'ADC127 LINK {nonce}\n'.encode('ascii'))
                        queued = int(transmitted[2]) == int(transmitted[3]) == expectedBytes
                        if not queued:
                            raise ValueError('Feather did not queue the complete link probe')
                    if line == f'ADC127 LINK_OK {nonce}':
                        replies.add(name)
                print('Serial link PASS: acknowledged in both directions.', flush=True)
                if linkOnly:
                    (session / 'result.txt').write_text(
                        'PASS: serial link only; ADC acquisition not started.\n', encoding='utf-8')
                    return

            # Explicit acknowledgement prevents an older FSYNC build silently
            # running its short association test instead of the read experiment.
            if readTest or indexTest or ppsTest:
                ports['P4'].write(b't' if timedTest else b'd' if sdTest else b'p' if ppsTest else b'n' if indexTest else b'r')
                deadline = time.monotonic() + 5
                selected = False
                while not selected:
                    name, line = nextLine(deadline)
                    expectedMode = 'TIMED MODE version=2 seconds=300 pairsPerFile=60 files=5 syntheticUtc=1' if timedTest else 'PPS SD MODE version=1 seconds=60 preserve=1' if sdTest else 'PPS MODE version=2 seconds=60 gpio=6' if ppsTest else 'INDEX MODE version=2 seconds=10' if indexTest else 'READ MODE version=3 seconds=10'
                    selected = name == 'P4' and line == expectedMode
            if ppsTest:
                # Existing Feather firmware supports 30/300/600 seconds. Use
                # a timeout longer than acquisition; P4's RAMFULL/STOP handshake
                # ends capture at 60 or 300 seconds. Wait for confirmation
                # before starting. Reset restores Feather's default afterward.
                safetySeconds = 600 if timedTest else 300
                captureSeconds = 300 if timedTest else 60
                ports['Feather'].write(f'@TEST {safetySeconds}\n'.encode('ascii'))
                deadline = time.monotonic() + 5
                durationReady = False
                while not durationReady:
                    name, line = nextLine(deadline)
                    durationReady = name == 'Feather' and line == f'TEST OK seconds={safetySeconds}'
                    if name == 'Feather' and line == 'TEST rejected':
                        raise ValueError('Feather refused the longer safety timeout')
                print(f'{captureSeconds}-second PPS test; Feather safety timeout confirmed at {safetySeconds} seconds.', flush=True)
            label = 'Timed tBlock/sBlock + SD' if timedTest else 'PPS/sample + SD' if sdTest else 'PPS/sample association' if ppsTest else 'FSYNC numbering' if indexTest else 'FSYNC reads' if readTest else 'Pulse delay' if delay else 'FSYNC'
            destination = 'preparing SD, then capturing' if sdTest else 'capturing in RAM'
            print(f'{label} trial {iRun + 1}/{runs}: {destination}...', flush=True)
            sourceRequested = True
            # g asks Feather to perform its ADC-start handshake with P4. Sending
            # the byte is only a request. Completion requires both P4's validated
            # report and Feather's DONE for the same run, in either arrival order.
            ports['Feather'].write(b'g')
            deadline = time.monotonic() + (1200 if timedTest else 480 if sdTest else 90 if ppsTest else 30 if delay or readTest or indexTest else 12)
            # These seconds cover capture, shutdown and console transfer. They
            # are not the ADC capture duration or the pulse-timestamp resolution.
            p4Lines = []
            completed = None
            featherDone = set()
            sdSaved = None
            sdSession = None
            sdRetained = None
            sampleCount = None
            while completed is None or completed['run'] not in featherDone or (sdTest and sdSaved is None):
                name, line = nextLine(deadline)
                if name == 'P4':
                    p4Lines.append(line)
                    if sdTest and line.startswith(('SD ', 'PREP', 'TIMED ')):
                        print(line, flush=True)
                    if timedTest and line.startswith('TIMED FAILED'):
                        raise ValueError('Timed storage failed: ' + line)
                    if timedTest and line.startswith('TIMED SAVED '):
                        sdSaved = parseTimedSaved(line)
                    if sdTest and line.startswith(('SD FAILED', 'SD PREPARE FAILED', 'SD mount failed',
                                                  'SD preparation requires', 'SD prepare FatFS', 'PREP FAILED')):
                        raise ValueError('Storage failed: ' + line)
                    if sdTest:
                        counts = re.fullmatch(r'FSYNC COUNTS run=(\d+) hardware=\d+ frames=\d+ words=(\d+) tail=-?\d+ rejected=\d+', line)
                        if counts:
                            sampleCount = (int(counts[1]), int(counts[2]))
                        sessionRow = re.fullmatch(r'SD SESSION id=(\d+) files=(\d+) changes=(\d+) reuses=(\d+) lost=(\d+)', line)
                        if sessionRow:
                            sdSession = [int(value) for value in sessionRow.groups()]
                        retained = re.fullmatch(r'SD RETAINED firstByte=(\d+) bytes=(\d+) totalBytes=(\d+)', line)
                        if retained:
                            sdRetained = [int(value) for value in retained.groups()]
                        saved = re.fullmatch(r'SD SAVED file=(\S+) bytes=(\d+) crc=([0-9a-fA-F]+) ringPeak=(\d+)/(\d+) maxWriteOrFlushUs=(\d+) overflow=(\d+) errors=(\d+) unsaved=(\d+)', line)
                        if saved:
                            sdSaved = {'path': saved[1], 'bytes': int(saved[2]), 'crc': saved[3],
                                       'ringPeak': int(saved[4]), 'ringCapacity': int(saved[5]),
                                       'maxWriteOrFlushUs': int(saved[6]), 'overflows': int(saved[7]),
                                       'errors': int(saved[8]), 'unsavedBytes': int(saved[9])}
                if line.startswith(('FSYNC PREP FAIL', 'ADC127 FAILED', 'ADC127 FAULT', 'ADC127 TIMEOUT',
                                    'No matching receiver READY', 'Source/ADC START failed', 'No receiver DONE')):
                    # Fault reports precede shutdown statistics. Request stop
                    # and retain ten seconds of subsequent output so queue and
                    # decoder measurements are not lost when consoles close.
                    # This wait does not certify completion of SD writing.
                    if sdTest and line.startswith('ADC127 FAULT'):
                        print(name + ': ' + line, flush=True)
                        ports['Feather'].write(b's')
                        drainDeadline = time.monotonic() + 10
                        while time.monotonic() < drainDeadline:
                            try:
                                reportName, reportLine = nextLine(drainDeadline)
                                print(reportName + ': ' + reportLine, flush=True)
                            except ValueError as drainError:
                                if not str(drainError).startswith('Timed out waiting'):
                                    raise
                    raise ValueError(name + ': ' + line)
                if name == 'Feather':
                    done = re.match(r'ADC127 DONE (\d+)(?: |$)', line)
                    if done:
                        featherDone.add(int(done[1]))
                if name == 'P4' and line.startswith(('READ ', 'INDEX ', 'PPS ')):
                    print(line, flush=True)
                if name == 'P4' and line.startswith('PPS RESULT ') and ppsTest:
                    match = re.fullmatch(r'PPS RESULT run=(\d+) status=(MEASURED|FAIL)', line)
                    if not match or (match[2] != 'MEASURED' and not sdTest):
                        raise ValueError('PPS/sample association failed; inspect PPS TOTAL and INDEX START in p4.txt')
                    # In SD mode retain a timing failure but keep reading until
                    # storage finishes. A failed timing check does not mean the
                    # card has finished writing or CRC verification.
                    completed = {'run': int(match[1]), 'timingPassed': match[2] == 'MEASURED',
                                 'ppsReport': [row for row in p4Lines if row.startswith(('PPS ', 'INDEX '))]}
                    if match[2] == 'MEASURED':
                        completed['intervals'] = analyzeBoundaries(p4Lines)
                elif name == 'P4' and line.startswith('INDEX RESULT ') and indexTest:
                    match = re.fullmatch(r'INDEX RESULT run=(\d+) status=MEASURED agreement=(YES|NO)', line)
                    if not match:
                        raise ValueError('Index experiment acquisition failed; inspect p4.txt')
                    completed = {'run': int(match[1]), 'agreement': match[2],
                                 'indexReport': [row for row in p4Lines if row.startswith('INDEX ')]}
                elif name == 'P4' and line.startswith('READ RESULT ') and readTest:
                    match = re.fullmatch(r'READ RESULT run=(\d+) status=MEASURED', line)
                    if not match:
                        raise ValueError('Read experiment acquisition failed; inspect p4.txt')
                    completed = {'run': int(match[1]), 'readReport': [row for row in p4Lines if row.startswith('READ ')]}
                elif name == 'P4' and line.startswith('FSYNC RESULT ') and not delay and not readTest and not indexTest and not ppsTest:
                    completed = analyze(p4Lines)
                elif name == 'P4' and line.startswith('DELAY RESULT ') and delay:
                    completed = analyzeDelay(p4Lines, frequencyHz=frequencyHz)
            sourceRequested = False
            if timedTest:
                validateTimedSaved(sdSaved, completed, sampleCount, captureSeconds=300)
                completed['storage'] = sdSaved
                print('Timed PASS: five files, 60 pairs per full file, complete tBlock/sBlock counts and full SD CRC readback succeeded; UTC is synthetic.', flush=True)
            elif sdTest:
                # Timing may finish before SD readback. Require exact byte count
                # for the same acquisition, full retention, no reuse, no losses.
                validStorage = (sampleCount is not None and sampleCount[0] == completed['run'] and
                                sdSaved['bytes'] == sampleCount[1] * 3 and sdSession is not None and
                                sdSession[1:] == [1, 0, 0, 0] and
                                sdRetained == [0, sdSaved['bytes'], sdSaved['bytes']] and
                                sdSaved['overflows'] == sdSaved['errors'] == sdSaved['unsavedBytes'] == 0 and
                                0 < sdSaved['ringPeak'] <= sdSaved['ringCapacity'])
                if not validStorage:
                    raise ValueError('SD completion counts do not match capture or report lost/reused bytes')
                completed['storage'] = sdSaved
                completed['session'] = sdSession[0]
                if not completed['timingPassed']:
                    (session / 'result.json').write_text(json.dumps([completed], indent=2) + '\n', encoding='utf-8')
                    raise ValueError('SD readback completed successfully, but PPS/sample timing checks failed; inspect INDEX and PPS rows')
                print('Combined PASS: PPS/sample checks and complete SD CRC readback succeeded.', flush=True)
            if any(previous['run'] == completed['run'] for previous in results):
                raise ValueError('Repeated run identifier; cannot establish an independent restart')
            results.append(completed)
            # Save after every trial so later failure preserves earlier results.
            # Raw logs are the evidence; JSON and HTML are derived summaries.
            (session / 'result.json').write_text(json.dumps(results, indent=2) + '\n', encoding='utf-8')
            if delay:
                writeReport(session / 'comparison.html', results)
                print(f"MEASURED: rising center {completed['rising']['medianNs'] / 1000:.4f} us; "
                      f"falling center {completed['falling']['medianNs'] / 1000:.4f} us", flush=True)
            elif not readTest and not indexTest and not ppsTest:
                print(f"PASS: {len(completed['anchors'])} matched timestamps; "
                      f"largest spacing residual {completed['maxResidualTicks']:.3f} ticks", flush=True)
        (session / 'result.txt').write_text(
            'PASS: timed tBlock/sBlock intervals and complete SD CRC readback; synthetic UTC. See result.json.\n' if timedTest else
            'PASS: combined PPS/sample association and complete SD CRC readback. See result.json.\n' if sdTest else
            'MEASURED: PPS/sample association; no UTC/GPS-validity check. See result.json.\n' if ppsTest else
            'MEASURED: timestamp-derived numbering; see agreement and INDEX rows in result.json.\n' if indexTest else
            'MEASURED: FSYNC read consistency; see result.json and p4.txt.\n' if readTest else
            ((f'MEASURED: {runs} pulse-delay trials. See result.json and comparison.html.\n'
             'Path delay relative to GPIO6; no absolute-time accuracy calibration.\n') if delay else
            (f'PASS: {runs} repeated starts; timestamp/sample consistency only.\n'
             'No ADC-delay or absolute-time accuracy measurement.\n')), encoding='utf-8')
    except (Exception, KeyboardInterrupt) as error:
        (session / 'result.txt').write_text('FAIL: ' + (str(error) or 'Interrupted') + '\n', encoding='utf-8')
        raise
    finally:
        # An interrupted PC run must not leave an intentional start request
        # unanswered. s is best-effort only; a failed USB write cannot establish
        # that Feather stopped the ADC. The external generator is never stopped
        # by this script. Join readers before closing their underlying ports.
        feather = ports.get('Feather')
        if sourceRequested and feather is not None and feather.is_open:
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
        print('Logs:', session.resolve())


def main():
    """Parse explicit hardware-run options; report failure with nonzero exit status."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--feather', default='COM4')
    parser.add_argument('--p4', default='COM3')
    parser.add_argument('--runs', type=int, default=5)
    parser.add_argument('--pps-sd-test', action='store_true', help='Sixty-second PPS capture plus new preserved-files SD recording and readback; use --runs 1')
    parser.add_argument('--timed-sd-test', action='store_true', help='Five-minute LXDG version-1 test, 60 pairs/file across five files with full CRC readback; synthetic UTC, preserved old files; use --runs 1')
    parser.add_argument('--pps-test', action='store_true', help='Sixty-second PPS/sample association across timer wraps; rising PPS on GPIO6')
    parser.add_argument('--index-test', action='store_true', help='Ten-second timestamp-derived numbering experiment')
    parser.add_argument('--read-test', action='store_true', help='Ten-second read-consistency experiment; no PPS needed')
    parser.add_argument('--link-only', action='store_true', help='Check board-to-board serial communication without starting ADC capture')
    parser.add_argument('--output', type=Path, default=Path(__file__).resolve().parents[2] / 'captures/fsync-association')
    args = parser.parse_args()
    try:
        capture(args.feather, args.p4, args.runs, args.output, linkOnly=args.link_only, readTest=args.read_test, indexTest=args.index_test, ppsTest=args.pps_test, sdTest=args.pps_sd_test, timedTest=args.timed_sd_test)
    except (Exception, KeyboardInterrupt) as error:
        print('FAIL:', str(error) or 'Interrupted')
        raise SystemExit(1)


if __name__ == '__main__':
    main()
