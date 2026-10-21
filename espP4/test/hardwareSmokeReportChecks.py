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
from hardwareSmokeReport import analyze, summary


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
    for index in range(300):
        mv = 1500 if index // 10 % 2 == 0 else 1800
        feather.append(f'INPUT,{index},{50000+index*100000},{mv},{mv-1},{mv+1},50,500')
        p4.append(f'SMOKE_SD,{index},102400,{mv*8388608/2500:.6f}')
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
                index = int(fields[1])
                fields[3] = str(float(fields[3]) + (30 if index % 2 else -30)*8388608/2500)
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
