# Copyright (C) 2026 Karl Stock
# SPDX-License-Identifier: GPL-3.0-or-later
"""Export an already stopped Rigol DHO900 acquisition through PyVISA.

Run on the PC, not on either board. This script never sends RUN, SINGLE, or
changes channel enables, gain, trigger, or memory depth. Leave the scope controls
alone during export so all channels retain the same acquisition and settings.

Reading path: main selects enabled channels; exportChannel reads internal memory
in bounded transfers. Each channel gets a compressed CSV of time/voltage pairs
and a compressed copy of the scope's original ASCII voltage replies. manifest.json
keeps the preamble, settings, transfer checksums, and success/failure status.
ASCII is slower than BYTE but avoids reducing the 12-bit scope data to 8 bits.
Only PyVISA and a working VISA driver are required; no NumPy installation.

Reference: Rigol DHO800/DHO900 Programming Guide, section 3.28.
https://download.rigol.com/en/Manual/Digital%20Oscilloscope/DHO900/DHO800900_ProgrammingGuide_EN.pdf
"""

import argparse
from contextlib import closing
from datetime import datetime, timezone
import csv
import gzip
import hashlib
import json
import math
from pathlib import Path
import sys


def requireStopped(scope):
    """Ask the connected instrument whether acquisition is stopped.

    main and exportChannel call this before/after transfers. scope is an open
    PyVISA connection; query sends a command and waits for its text reply.
    Return normally only for STOP, otherwise raise RuntimeError. This check
    cannot prevent someone restarting the scope between queries, so leave its
    controls alone throughout export.
    """
    status = scope.query(':TRIG:STAT?').strip().upper()
    if status != 'STOP':
        raise RuntimeError(f'Scope status is {status}; press RUN/STOP to stop it first.')


def checkErrors(scope):
    """Report a scope command error instead of accepting a silently ignored setting.

    Reading the error queue removes one entry from the instrument. A zero code
    returns normally; a nonzero code raises RuntimeError with the instrument's
    reply. main saves that exception in the failed export's manifest. This
    checks command handling, not the electrical quality of the recorded signal.
    """
    reply = scope.query(':SYST:ERR?').strip()
    if int(reply.split(',')[0]) != 0:
        raise RuntimeError(f'Scope error: {reply}')


def readPreamble(scope):
    """Return the original preamble and named scaling fields for the current channel.

    exportChannel calls this after selecting a channel and its full record.
    Return the original reply and a dictionary of numeric fields. The preamble
    describes the number of points and how their indices map to time/voltage.
    RAW means acquisition memory, not the reduced screen image. ASCII replies
    already contain volts, so Y scaling is retained as evidence but must not be
    applied again. Invalid format, field count, or time spacing raises an error.
    """
    reply = scope.query(':WAV:PRE?').strip()
    fields = reply.split(',')
    names = ('format', 'type', 'points', 'count', 'xIncrement', 'xOrigin',
             'xReference', 'yIncrement', 'yOrigin', 'yReference')
    if len(fields) != len(names):
        raise RuntimeError(f'Unexpected preamble: {reply}')
    values = dict(zip(names, (float(field) for field in fields)))
    if not all(math.isfinite(value) for value in values.values()):
        raise RuntimeError(f'Nonfinite preamble: {reply}')
    if values['format'] != 2 or values['type'] != 2 or values['xIncrement'] <= 0:
        raise RuntimeError(f'Expected ASCII/RAW with positive time spacing: {reply}')
    return reply, values


def writeChecked(scope, command):
    """Send a transfer-setting command, then read the instrument's error queue.

    exportChannel supplies the open connection and command string. Return no
    value on success; attach the command to any scope error so the terminal and
    manifest identify the failed request. These commands select data for export;
    they do not start a new acquisition.
    """
    scope.write(command)
    try:
        checkErrors(scope)
    except RuntimeError as error:
        raise RuntimeError(f'After {command}: {error}') from error


def exportChannel(scope, channel, destination, chunkPoints):
    """Save one full channel record, checking every requested point range.

    main supplies an open scope connection, channel number 1..4, an existing
    output directory, and the maximum points per query. Return file names,
    settings, scaling, point count, voltage range and a checksum for the manifest.
    Exceptions propagate to main; partially written files remain for inspection.

    The scope uses one-based inclusive transfer positions. CSV point index i is
    zero-based across the entire record, so its time is
    (i - xReference) * xIncrement + xOrigin, in seconds relative to the trigger.
    This is scope time, not GPS time. No interpolation or averaging is performed.
    """
    requireStopped(scope)
    writeChecked(scope, f':WAV:SOUR CHAN{channel}')
    writeChecked(scope, ':WAV:MODE RAW')
    writeChecked(scope, ':WAV:FORM ASC')

    # Explicitly select full memory: the earlier 1,000-point test changed the
    # transfer window. Acquisition depth is queried, never changed here.
    depth = int(float(scope.query(':ACQ:MDEP?').strip()))
    if depth <= 0:
        raise RuntimeError('Scope did not report a positive numeric memory depth.')

    # A previous export leaves START at its final segment. Reset START before
    # asking for the full point count, so the request cannot extend past memory.
    writeChecked(scope, ':WAV:STAR 1')
    writeChecked(scope, f':WAV:STOP {depth}')
    writeChecked(scope, f':WAV:POIN {depth}')
    originalPreamble, preamble = readPreamble(scope)
    if preamble['points'] != depth:
        raise RuntimeError(f'CH{channel}: preamble points differ from memory depth.')

    # Keep the channel settings with the data so later analysis can distinguish
    # a probe-factor or coupling change from a change in the circuit itself.
    settings = {}
    for name, suffix in (('probe', 'PROB'), ('coupling', 'COUP'),
                         ('scale', 'SCAL'), ('offset', 'OFFS'), ('invert', 'INV')):
        settings[name] = scope.query(f':CHAN{channel}:{suffix}?').strip()

    csvName = f'CH{channel}.csv.gz'
    rawName = f'CH{channel}.scope-ascii.txt.gz'
    digest = hashlib.sha256()
    minimum = math.inf
    maximum = -math.inf
    print(f'CH{channel}: {depth:,} points; spacing {preamble["xIncrement"] * 1e9:g} ns', flush=True)

    # Stream to disk so even a deep record does not require large Python lists.
    # An interrupted export leaves partial files and a failed manifest, never a
    # success report. The raw file preserves replies one transfer per line.
    with gzip.open(destination / csvName, 'wt', newline='', encoding='ascii') as csvFile, \
            gzip.open(destination / rawName, 'wt', encoding='ascii') as rawFile:
        writer = csv.writer(csvFile)
        writer.writerow(('time_s', 'voltage_V'))
        for firstPoint in range(1, depth + 1, chunkPoints):
            # Extend STOP before advancing START: START must not pass the old
            # endpoint. For 10,000-point transfers, the second range is
            # 10,001..20,000, inclusive. Readback checks the accepted range.
            lastPoint = min(firstPoint + chunkPoints - 1, depth)
            scope.write(f':WAV:STOP {lastPoint}')
            scope.write(f':WAV:STAR {firstPoint}')
            checkErrors(scope)
            if int(scope.query(':WAV:STAR?')) != firstPoint or int(scope.query(':WAV:STOP?')) != lastPoint:
                raise RuntimeError('Scope did not accept the requested transfer range.')
            # This query actually downloads the voltages. The checksum covers
            # the saved ASCII replies (with one newline per reply), not the CSV
            # or compressed bytes. It identifies these saved data; it is not an
            # independent checksum supplied by the scope.
            reply = scope.query(':WAV:DATA?').strip()
            voltages = reply.split(',')
            if len(voltages) != lastPoint - firstPoint + 1:
                raise RuntimeError(f'CH{channel}: wrong point count at {firstPoint}.')
            rawFile.write(reply + '\n')
            digest.update((reply + '\n').encode('ascii'))
            for iLocal, textVoltage in enumerate(voltages):
                voltage = float(textVoltage)
                if not math.isfinite(voltage) or abs(voltage) > 1e20:
                    raise RuntimeError(f'CH{channel}: invalid voltage {textVoltage}.')
                # Keep one continuous time axis across transfers: the first
                # voltage in the second 10,000-point range has index 10,000,
                # not zero. Seventeen significant digits retain float precision.
                iPoint = firstPoint - 1 + iLocal
                timeSeconds = (iPoint - preamble['xReference']) * preamble['xIncrement'] + preamble['xOrigin']
                writer.writerow((format(timeSeconds, '.17g'), textVoltage))
                minimum = min(minimum, voltage)
                maximum = max(maximum, voltage)
            if lastPoint == depth or lastPoint % (chunkPoints * 10) == 0:
                print(f'  {lastPoint:,}/{depth:,}', flush=True)

    requireStopped(scope)
    # Compare the same full-record selection at both ends. Our final transfer
    # selected only the last segment; transfer-dependent preamble fields can
    # therefore differ even though the stopped acquisition has not changed.
    writeChecked(scope, ':WAV:STAR 1')
    writeChecked(scope, f':WAV:STOP {depth}')
    writeChecked(scope, f':WAV:POIN {depth}')
    finalPreamble, finalValues = readPreamble(scope)
    changes = {key: {'before': value, 'after': finalValues[key]}
               for key, value in preamble.items() if value != finalValues[key]}
    if changes:
        raise RuntimeError(f'CH{channel}: full-record parameters changed: {changes}; '
                           f'before={originalPreamble}; after={finalPreamble}')
    checkErrors(scope)
    return {'channel': channel, 'csv': csvName, 'raw': rawName,
            'rawSha256': digest.hexdigest(), 'preambleText': originalPreamble,
            'preamble': preamble, 'settings': settings, 'pointsSaved': depth,
            'minimumV': minimum, 'maximumV': maximum}


def main():
    """Select a scope, export enabled channels, and leave a durable outcome report.

    Called by the command-line entry point. Defaults select all enabled analog
    channels. Explicit --channels refuses disabled channels: enabling them now
    cannot recover data that was never acquired. A new directory prevents
    overwriting earlier measurements. Return 0 for a completed export or 1 for
    a caught transfer/setup failure. Argument and directory-creation errors
    occur before the transfer exception handler and may have no manifest.
    """
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--resource', default='USB0::0x1AB1::0x044C::DHO9S287M00773::INSTR')
    parser.add_argument('--channels', nargs='+', type=int, choices=(1, 2, 3, 4))
    parser.add_argument('--output', type=Path)
    parser.add_argument('--chunk-points', type=int, default=10000)
    args = parser.parse_args()
    if not 1 <= args.chunk_points <= 100000:
        parser.error('--chunk-points must be between 1 and 100000.')
    stamp = datetime.now(timezone.utc)
    destination = args.output or (Path(__file__).resolve().parents[2] / 'captures' /
                                  'scope' / stamp.strftime('%Y%m%d-%H%M%S-%f'))
    destination.mkdir(parents=True, exist_ok=False)
    manifest = {'status': 'incomplete', 'exportUtc': stamp.isoformat(),
                'resource': args.resource, 'channels': [],
                'timeFormula': '(zeroBasedIndex - xReference) * xIncrement + xOrigin',
                'voltageFormat': 'Scope ASCII volts; no additional Y scaling',
                'chunkPoints': args.chunk_points}
    retVal = 0
    try:
        # Import here so --help works without PyVISA. No connection is attempted
        # until Karl explicitly runs the exporter from his PC terminal.
        import pyvisa
        with closing(pyvisa.ResourceManager()) as manager:
            with closing(manager.open_resource(args.resource)) as scope:
                # VISA is the installed instrument communication driver.
                # timeout is milliseconds per operation, not the total export
                # duration. chunk_size controls transport reads in bytes; it is
                # separate from chunkPoints, the waveform points per query.
                # closing releases both handles even if a transfer fails.
                scope.timeout = 30000
                scope.chunk_size = 1024 * 1024
                scope.read_termination = '\n'
                scope.write_termination = '\n'
                requireStopped(scope)
                manifest['identity'] = scope.query('*IDN?').strip()
                if ',DHO9' not in manifest['identity']:
                    raise RuntimeError('This exporter is intended for the Rigol DHO900 series.')
                # Preserve and drain errors left by earlier commands. Otherwise
                # an old failure would be wrongly blamed on this export's setup.
                manifest['priorScopeErrors'] = []
                queueEmpty = False
                for iError in range(32):
                    if not queueEmpty:
                        reply = scope.query(':SYST:ERR?').strip()
                        queueEmpty = int(reply.split(',')[0]) == 0
                        if not queueEmpty:
                            manifest['priorScopeErrors'].append(reply)
                            print(f'Previous scope error: {reply}', flush=True)
                if not queueEmpty:
                    raise RuntimeError('Scope error queue did not clear after 32 entries.')

                # Export only channels that already contain acquired data.
                # Downloading them sequentially is safe while the stored record
                # remains stopped: their acquisition was simultaneous.
                enabled = []
                for channel in range(1, 5):
                    if scope.query(f':CHAN{channel}:DISP?').strip().upper() in ('1', 'ON'):
                        enabled.append(channel)
                channels = sorted(set(args.channels or enabled))
                if not channels or not set(channels).issubset(enabled):
                    raise RuntimeError(f'Requested channels must be enabled in the acquisition; enabled: {enabled}')
                manifest['enabledChannels'] = enabled
                manifest['requestedChannels'] = channels
                manifest['acquisitionType'] = scope.query(':ACQ:TYPE?').strip()
                print(f'Saving to {destination}', flush=True)
                for channel in channels:
                    record = exportChannel(scope, channel, destination, args.chunk_points)
                    manifest['channels'].append(record)

                # A shared time grid is necessary for later cross-channel edge
                # comparisons. It does not establish probe skew or absolute accuracy.
                grids = [tuple(record['preamble'][key] for key in
                               ('points', 'xIncrement', 'xOrigin', 'xReference'))
                         for record in manifest['channels']]
                if any(grid != grids[0] for grid in grids):
                    raise RuntimeError('Exported channels do not share the same time grid.')
                requireStopped(scope)
                manifest['status'] = 'complete'
    except (Exception, KeyboardInterrupt) as error:
        manifest['status'] = 'failed'
        manifest['error'] = str(error) or type(error).__name__
        print(f'FAILED: {manifest["error"]}', file=sys.stderr)
        retVal = 1
    finally:
        # Write the outcome after normal completion, an exception, or Ctrl+C.
        # A forced process termination/power loss can prevent this final write;
        # files without a complete manifest must not be treated as a valid set.
        (destination / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    if retVal == 0:
        print(f'EXPORT COMPLETE: {len(manifest["channels"])} channels. Scope remains stopped.')
    print(f'Results: {destination}')
    return retVal


if __name__ == '__main__':
    sys.exit(main())
