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

"""PC-only tests of saved-file validation and signed sample interpretation."""
import contextlib
import io
import math
from pathlib import Path
import struct
import tempfile
import unittest
import zlib
from inspectAnalogRecording import inspectRecording, signed24


class AnalogReaderChecks(unittest.TestCase):
    """Use a small independent synthetic file; no SD card or board access."""

    def makeFile(self, path, valid=1):
        """Generate a 4s 10Hz signal at 1kHz with a negative-to-positive swing."""
        payload = bytearray()
        for index in range(4000):
            value = round(100000 * math.sin(2 * math.pi * 10 * index / 1000))
            payload.extend((value & 0xffffff).to_bytes(3, 'little'))
        header = bytearray(512)
        header[:8] = b'P4ADC03\0'
        struct.pack_into('<5I', header, 8, 3, valid, 123, 6, 1000)
        struct.pack_into('<Q', header, 28, len(payload))
        struct.pack_into('<2I', header, 36, zlib.crc32(payload), 3)
        struct.pack_into('<Q', header, 44, 4000)
        path.write_bytes(header + payload)

    def testSignedLimits(self):
        """Check byte order and both signed 24-bit endpoints directly."""
        self.assertEqual(signed24(b'\xff\xff\xff', 0), -1)
        self.assertEqual(signed24(b'\x00\x00\x80', 0), -8388608)
        self.assertEqual(signed24(b'\xff\xff\x7f', 0), 8388607)
        self.assertEqual(signed24(b'\x56\x34\x12', 0), 0x123456)

    def testFileAndFrequency(self):
        """Require CRC acceptance and the expected crossing-derived frequency."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.bin'
            self.makeFile(path)
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                inspectRecording(path)
            self.assertIn('frequency=10.000 Hz', output.getvalue())
            self.assertIn('min=-100000 max=100000', output.getvalue())

    def testReservedHeaderField(self):
        """Both zeroed new headers and historical index values retain the same layout."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.bin'
            for reserved in (0, 6):
                self.makeFile(path)
                data = bytearray(path.read_bytes())
                struct.pack_into('<I', data, 20, reserved)
                path.write_bytes(data)
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    inspectRecording(path)
                self.assertIn('frequency=10.000 Hz', output.getvalue())

    def testRejectInvalidCorruptAndTruncated(self):
        """A readable waveform must not conceal capture failure or storage damage."""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'test.bin'
            for kind in ('invalid', 'corrupt', 'truncated'):
                self.makeFile(path, valid=0 if kind == 'invalid' else 1)
                data = bytearray(path.read_bytes())
                if kind == 'corrupt':
                    data[1000] ^= 1
                elif kind == 'truncated':
                    del data[-3:]
                path.write_bytes(data)
                with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(ValueError):
                    inspectRecording(path)


if __name__ == '__main__':
    unittest.main()
