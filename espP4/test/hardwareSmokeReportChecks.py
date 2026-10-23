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

"""PC-only rejection tests for the hardware smoke result; no device access."""
import unittest
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
from hardwareSmokeReport import analyze, summary
from runHardwareSmoke import capture


class linkChecks(unittest.TestCase):
    """Exercise the PC probe without opening real serial ports or touching boards."""

    def checkProbe(self, reply):
        """Fake readers deliver identities and optionally a matching round-trip echo."""
        writes, readers = [], {}
        ticks = [0]

        def monotonic():
            """Advance a deterministic clock so silence reaches the deadline quickly."""
            ticks[0] += .5
            return ticks[0]

        class Reader:
            """Register the event queue instead of starting a real reader thread."""
            def __init__(self, target, args, daemon):
                self.args = args

            def start(self):
                readers[self.args[1]] = self.args[2]

            def join(self, timeout):
                pass

        class Port:
            """Minimal serial transport; every write is retained for safety assertions."""
            def __init__(self, **kwargs):
                self.is_open = False

            def open(self):
                self.is_open = True

            def close(self):
                self.is_open = False

            def reset_input_buffer(self):
                pass

            def write(self, data):
                name = 'Feather' if self.port == 'COM4' else 'P4'
                writes.append(data)
                if data == b'h':
                    identity = 'SMOKE_FEATHER version=4' if name == 'Feather' else 'SMOKE_P4 version=5'
                    readers[name].put((name, identity + ' ready=1 seconds=600'))
                elif data.startswith(b'@LINK '):
                    nonce = int(data.split()[1])
                    if name == 'P4':
                        readers[name].put((name, f'LINK ARMED {nonce}'))
                    else:
                        size = len(f'ADC127 LINK {nonce}\n'.encode())
                        readers[name].put((name, f'LINK TX nonce={nonce} queued={size} expected={size} RX=7 TX=8'))
                        if reply:
                            for board in ('P4', 'Feather'):
                                readers[board].put((board, f'ADC127 LINK_OK {nonce}'))
                elif data == b'@LINKSTATUS\n':
                    readers[name].put((name, 'LINK STATUS polls=100 bytes=0 lines=0 errors=0'))
                return len(data)

        serial = SimpleNamespace(Serial=Port, SerialException=OSError)
        with tempfile.TemporaryDirectory() as directory, patch.dict('sys.modules', {'serial': serial}), \
                patch('runHardwareSmoke.threading.Thread', Reader), \
                patch('runHardwareSmoke.time.sleep'), patch('runHardwareSmoke.time.monotonic', monotonic):
            if reply:
                capture('COM4', 'COM3', Path(directory), linkOnly=True)
                self.assertIn('serial link only', next(Path(directory).glob('*/result.txt')).read_text())
            else:
                with self.assertRaisesRegex(ValueError, 'Serial probe failed'):
                    capture('COM4', 'COM3', Path(directory), linkOnly=True)
        self.assertNotIn(b'g', writes)
        self.assertFalse(any(command.startswith((b'@UTC', b'@TEST')) for command in writes))

    def testLinkOnly(self):
        """A remembered 600-second setting is accepted; success never starts recording."""
        self.checkProbe(True)

    def testSilentLink(self):
        """No echo produces bounded failure without ADC or SD commands."""
        self.checkProbe(False)


def fixture():
    """Make a complete synthetic log pair with an exact 1500/1800-mV signal."""
    feather = ['SMOKE_FEATHER version=3 ready=1 seconds=30',
               'DAC MODE: SQUARE codes=110,135 levelSeconds=1',
               'ADC START echo confirmed.', 'ADC STOP command echo confirmed',
               'DAC OFF updates=60000 missedIntervals=0 maxGapUs=501 driverFault=0',
               'INPUT_CAL run=7 source=0 defaultVrefMv=1100']
    feather += [f'CAL,{raw},{raw}' for raw in list(range(0,4096,32)) + [4095]]
    feather += ['INPUT_BEGIN run=7 count=300 clean=1 missed=0 maxGapUs=100010 mode=burst']
    p4 = ['SMOKE_P4 version=3 ready=1 seconds=30',
          'ADC127 DONE 7 words=30720000 bad=0 periodErrors=0 short=0 drops=0 errors=0 rate=1024000.0/s',
          'SD SAVED file=0:/test.BIN bytes=92160000 crc=12345678 ringPeak=500/4194304 maxWriteOrFlushUs=100 overflow=0 errors=0 unsaved=0',
          'SMOKE_SD_BEGIN run=7 rate=1024000 samples=30720000 bins=300 clean=1']
    for iBin in range(300):
        mv = 1500 if iBin // 10 % 2 == 0 else 1800
        feather.append(f'INPUT,{iBin},{50000+iBin*100000},{mv},{mv-1},{mv+1},50,500')
        p4.append(f'SMOKE_SD,{iBin},102400,{mv*8388608/2500:.6f}')
    feather.append('INPUT_END run=7')
    p4.append('SMOKE_SD_END run=7')
    return '\n'.join(feather), '\n'.join(p4)


def retainedFixture(seconds):
    """Build matching logs with only the latest file generations and A2 window."""
    feather, _ = fixture()
    feather = [line for line in feather.splitlines() if not line.startswith(('INPUT,', 'INPUT_BEGIN', 'INPUT_END'))]
    feather[0] = 'SMOKE_FEATHER version=4 ready=1 seconds=30'
    count = min(800, seconds * 10)
    feather += [f'TEST OK seconds={seconds}',
                f'INPUT_RANGE first={seconds*10-count} total={seconds*10} seconds={seconds}',
                f'INPUT_BEGIN run=7 count={count} clean=1 missed=0 maxGapUs=100010 mode=burst']
    rate = 1024000
    samples = seconds * rate
    files = 2 if seconds == 30 else 5
    changes = (samples - 1) // (16 * rate)
    first = max(0, changes - files + 1) * 16 * rate
    p4 = ['SMOKE_P4 version=5 ready=1 seconds=30', f'TEST OK seconds={seconds}',
          f'ADC127 DONE 7 words={samples} bad=0 periodErrors=0 short=0 drops=0 errors=0 rate=1024000.0/s',
          f'SD SESSION id=12 files={files} changes={changes} reuses={max(0,changes+1-files)} lost=0',
          f'SD SAVED file=0:/RECORD/session-12 bytes={samples*3} crc=12345678 ringPeak=500/16777216 maxWriteOrFlushUs=100 overflow=0 errors=0 unsaved=0',
          f'SD RETAINED firstByte={first*3} bytes={(samples-first)*3} totalBytes={samples*3}',
          f'SMOKE_SD_RANGE firstSample={first} retainedSamples={samples-first} totalSamples={samples}',
          f'SMOKE_SD_BEGIN run=7 rate={rate} samples={samples} bins={(samples-first)//102400} clean=1']
    for iBin in range(seconds * 10 - count, seconds * 10):
        mv = 1500 if iBin // 10 % 2 == 0 else 1800
        feather.append(f'INPUT,{iBin},{50000+iBin*100000},{mv},{mv-1},{mv+1},50,500')
    for iBin in range(first // 102400, seconds * 10):
        mv = 1500 if iBin // 10 % 2 == 0 else 1800
        p4.append(f'SMOKE_SD,{iBin},102400,{mv*8388608/2500:.6f}')
    feather.append('INPUT_END run=7')
    p4.append('SMOKE_SD_END run=7')
    return '\n'.join(feather), '\n'.join(p4)


class smokeReportChecks(unittest.TestCase):
    """Check successful comparison and failures that could otherwise look plausible."""

    def testGood(self):
        """Known matching voltage levels pass without estimating a scale factor."""
        result = analyze(*fixture())
        self.assertLess(result['errorPercent'], 0.001)
        self.assertEqual(result['samples'], 30720000)

    def testSelectableDurations(self):
        """All selections preserve absolute plot time and verify only surviving files."""
        for seconds, first in ((30, 0), (300, 224), (600, 528)):
            with self.subTest(seconds=seconds):
                result = analyze(*retainedFixture(seconds))
                self.assertEqual(result['requestedSeconds'], seconds)
                self.assertEqual(result['firstSecond'], first)
                self.assertEqual(result['bytesRetained'], (seconds-first)*1024000*3)
                self.assertLess(result['errorPercent'], .001)

    def testRetainedRejections(self):
        """Loss, inconsistent rotations, missing data and mismatched durations fail."""
        feather, p4 = retainedFixture(300)
        for before, after in [('TEST OK seconds=300', 'TEST OK seconds=600'),
                              ('changes=18', 'changes=19'), ('reuses=14', 'reuses=0'),
                              ('lost=0', 'lost=1'), ('SD SAVED', 'SD FAILED'),
                              ('firstSample=229376000', 'firstSample=0'),
                              ('SMOKE_SD,2240,', 'SMOKE_SD,2241,')]:
            with self.subTest(before=before), self.assertRaises(ValueError):
                analyze(feather, p4.replace(before, after))
        with self.assertRaises(ValueError):
            analyze(feather.replace('INPUT,2240,', 'INPUT,2241,'), p4)

    def testConfirmedProgress(self):
        """Checks appear once, survive later voltage failure, and never imply PASS."""
        feather, p4 = fixture()
        messages = []
        result = analyze(feather, p4, messages.append)
        self.assertEqual(messages, result['checks'])
        self.assertEqual(len(messages), 5)
        self.assertIn('SD written: 92,160,000 sample bytes', messages)
        self.assertNotIn('Recording buffer:', summary(result, includeChecks=False))
        self.assertIn('Recording buffer: no overflow', summary(result))
        changed = []
        for line in p4.splitlines():
            if line.startswith('SMOKE_SD,'):
                fields = line.split(',')
                fields[3] = str(float(fields[3]) + 60*8388608/2500)
                line = ','.join(fields)
            changed.append(line)
        messages = []
        with self.assertRaisesRegex(ValueError, 'exceeds 10%'):
            analyze(feather, '\n'.join(changed), messages.append)
        self.assertEqual(len(messages), 5)
        self.assertFalse(any('PASS' in message for message in messages))
        messages = []
        with self.assertRaises(ValueError):
            analyze(feather, p4.replace('overflow=0', 'overflow=1'), messages.append)
        self.assertNotIn('Recording buffer: no overflow', messages)

    def testHistoricalFirmwareReports(self):
        """Saved older reports remain readable; live runs require version 3."""
        feather, p4 = fixture()
        for version in (1, 2):
            result = analyze(feather.replace('version=3', f'version={version}'),
                             p4.replace('version=3', f'version={version}'))
            self.assertLess(result['errorPercent'], 0.001)

    def testBadMetadata(self):
        """Wrong IDs, missing data, overflow and failed disk readback cannot pass."""
        feather, p4 = fixture()
        mutations = [('clean=1', 'clean=0'), ('overflow=0', 'overflow=1'),
                     ('drops=0', 'drops=1'), ('SD SAVED', 'SD FAILED'),
                     ('SMOKE_SD_END run=7', 'SMOKE_SD_END run=8'),
                     ('samples=30720000', 'samples=30000000'),
                     ('SMOKE_SD,29,', 'SMOKE_SD,28,')]
        for before, after in mutations:
            with self.subTest(before=before), self.assertRaises(ValueError):
                analyze(feather, p4.replace(before, after))
        for before, after in [('missed=0', 'missed=1'), ('source=0', 'source=99'),
                              ('INPUT_END run=7', 'INPUT_END run=8'),
                              ('INPUT,1,150000', 'INPUT,1,250000'),
                              ('INPUT,0,50000,1500', 'INPUT,0,50000,nan')]:
            with self.subTest(before=before), self.assertRaises(ValueError):
                analyze(feather.replace(before, after), p4)

    def testFileRingCompletion(self):
        """New firmware must prove one boundary crossing and no missing/reused data."""
        feather, p4 = fixture()
        p4 = p4.replace('SMOKE_P4 version=3', 'SMOKE_P4 version=4')
        line = 'SD SESSION id=12 files=2 changes=1 reuses=0 lost=0'
        with self.assertRaisesRegex(ValueError, 'File-ring completion'):
            analyze(feather, p4)
        result = analyze(feather, p4 + '\n' + line)
        self.assertIn('SD files: two files verified, one changeover, no overwrite', result['checks'])
        for before, after in [('files=2', 'files=1'), ('changes=1', 'changes=0'),
                              ('reuses=0', 'reuses=1'), ('lost=0', 'lost=3'), ('id=12', 'id=0')]:
            with self.subTest(before=before), self.assertRaises(ValueError):
                analyze(feather, p4 + '\n' + line.replace(before, after))

    def testFileCapacityFailure(self):
        """Identify the exact-capacity failure before the generic unclean-report check."""
        feather, p4 = fixture()
        p4 = p4.replace('SMOKE_P4 version=3', 'SMOKE_P4 version=4')
        p4 = p4.replace('clean=1', 'clean=0').replace('SD SAVED', 'SD FAILED')
        p4 = p4.replace('ADC127 DONE 7 words=30720000', 'ADC127 FAULT 7 words=30720503')
        with self.assertRaisesRegex(ValueError, 'SD file capacity exhausted; received 503 more samples'):
            analyze(feather, p4 + '\nADC127 RAMFULL 7')
        # A stale indication from another run must not explain this failure.
        with self.assertRaisesRegex(ValueError, 'marked the capture or SD verification unclean'):
            analyze(feather, p4 + '\nADC127 RAMFULL 8')

    def testVoltageOffset(self):
        """A 60-mV output offset fails the 30-mV limit; it is not fitted away."""
        feather, p4 = fixture()
        changed = []
        for line in p4.splitlines():
            if line.startswith('SMOKE_SD,'):
                fields = line.split(',')
                fields[3] = str(float(fields[3]) + 60*8388608/2500)
                line = ','.join(fields)
            changed.append(line)
        with self.assertRaisesRegex(ValueError, 'exceeds 10%'):
            analyze(feather, '\n'.join(changed))

    def testPlateauAveraging(self):
        """Alternating within-plateau noise cancels, but one bad plateau fails."""
        feather, p4 = fixture()
        changed = []
        for line in p4.splitlines():
            if line.startswith('SMOKE_SD,'):
                fields = line.split(',')
                iBin = int(fields[1])
                fields[3] = str(float(fields[3]) + (30 if iBin % 2 else -30)*8388608/2500)
                line = ','.join(fields)
            changed.append(line)
        self.assertLess(analyze(feather, '\n'.join(changed))['errorPercent'], 0.001)
        changed = []
        for line in p4.splitlines():
            if line.startswith('SMOKE_SD,'):
                fields = line.split(',')
                if 12 <= int(fields[1]) <= 17:
                    fields[3] = str(float(fields[3]) + 60*8388608/2500)
                line = ','.join(fields)
            changed.append(line)
        with self.assertRaisesRegex(ValueError, 'exceeds 10%'):
            analyze(feather, '\n'.join(changed))

    def testStuckSignal(self):
        """A full-length, CRC-clean file still fails if the waveform is stuck."""
        feather, p4 = fixture()
        with self.assertRaisesRegex(ValueError, '29 level changes'):
            analyze(feather, p4.replace(f'{1800*8388608/2500:.6f}', f'{1500*8388608/2500:.6f}'))

    def testMissingRows(self):
        """Truncated serial evidence cannot turn a partial check into PASS."""
        feather, p4 = fixture()
        with self.assertRaises(ValueError):
            analyze(feather, '\n'.join(line for line in p4.splitlines() if not line.startswith('SMOKE_SD,100,')))


if __name__ == '__main__':
    unittest.main()
