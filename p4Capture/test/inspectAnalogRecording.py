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

"""Read a copied P4ADC03 file, verify its payload CRC, and describe a waveform.

Uses only Python's standard library. Never writes to the recording or card.
Reports raw ADC counts, not calibrated volts. A frequency estimate and a matching
file CRC are separate observations, not a blanket analog-integrity PASS.
"""
import argparse
import math
import struct
import zlib
from pathlib import Path


def signed24(data, offset):
    """Decode one little-endian signed three-byte sample, preserving bit 23."""
    value = data[offset] | data[offset + 1] << 8 | data[offset + 2] << 16
    return value - 0x1000000 if value & 0x800000 else value


def inspectRecording(path):
    """Validate the entire file and examine two seconds after a 1s settling skip."""
    with path.open('rb') as recording:
        header = recording.read(512)
        if len(header) != 512 or header[:8] != b'P4ADC03\0':
            raise ValueError('Expected a P4ADC03 file with a complete 512-byte header')
        version, valid, runId, stage, rate = struct.unpack_from('<5I', header, 8)
        payloadBytes = struct.unpack_from('<Q', header, 28)[0]
        expectedCrc, width = struct.unpack_from('<2I', header, 36)
        count = struct.unpack_from('<Q', header, 44)[0]
        if version != 3 or width != 3 or payloadBytes != count * 3 or rate == 0:
            raise ValueError('Inconsistent version, width, count or rate')
        if path.stat().st_size != 512 + payloadBytes:
            raise ValueError('File size does not match finalized payload length')

        # Read all payload bytes in bounded chunks; no full-file RAM allocation.
        crc = 0
        while True:
            chunk = recording.read(1024 * 1024)
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
        print(f'run={runId} stage={stage} rate={rate}/s samples={count} valid={valid}')
        print(f'CRC read={crc:08x} header={expectedCrc:08x}')
        if crc != expectedCrc or valid != 1:
            raise ValueError('Invalid capture flag or payload CRC mismatch; not a clean recording')
        if count < 3 * rate:
            raise ValueError('Need at least 3 seconds for the selected settled analysis window')

        recording.seek(512 + rate * 3)
        data = recording.read(2 * rate * 3)
        if len(data) != 2 * rate * 3:
            raise ValueError('Incomplete analysis window')

    # Preserve all selected samples for extrema, but retain only ~1k points/s
    # for frequency/shape inspection. This is adequate for a 10 Hz functional
    # signal, not an anti-aliased spectrum or a high-frequency distortion test.
    stride = max(1, rate // 1000)
    points = []
    minimum, maximum = 0x7fffff, -0x800000
    for index in range(2 * rate):
        value = signed24(data, index * 3)
        minimum, maximum = min(minimum, value), max(maximum, value)
        if index % stride == 0:
            points.append(value)
    mean = sum(points) / len(points)
    rms = math.sqrt(sum((value - mean) ** 2 for value in points) / len(points))

    # Linear interpolation of upward mean crossings gives a rough frequency.
    # Reject flat data; discard closely spaced crossings from noise near zero.
    crossings = []
    pointSeconds = stride / rate
    if maximum - minimum > 100:
        for index in range(1, len(points)):
            before, after = points[index - 1], points[index]
            if before < mean <= after:
                at = (index - 1 + (mean - before) / (after - before)) * pointSeconds
                if not crossings or at - crossings[-1] > 0.05:
                    crossings.append(at)
    frequency = ((len(crossings) - 1) / (crossings[-1] - crossings[0])
                 if len(crossings) >= 2 else None)
    print(f'Window 1..3 s: min={minimum} max={maximum} mean={mean:.1f} AC-RMS={rms:.1f} counts')
    print(f'Estimated frequency={frequency:.3f} Hz (expected about 10 Hz)' if frequency else
          'No usable 10 Hz frequency estimate; inspect wiring/input selection/signal')
    print('CRC verified. Analog amplitude/offset/clipping still require interpretation; '
          'this is not a precision or whole-record waveform validation.')


def main():
    """Accept one local recording path; report malformed files without modifying them."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('recording', type=Path)
    arguments = parser.parse_args()
    try:
        inspectRecording(arguments.recording)
    except (OSError, ValueError) as error:
        parser.exit(1, f'ERROR: {error}\n')


if __name__ == '__main__':
    main()
