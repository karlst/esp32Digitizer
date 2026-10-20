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

"""Start and log the S3 timed storage test on COM4; argv[1] is the log path.

Assumes the installed Choke test firmware sets an 1800-second duration. It does
not identify the board or firmware. Start requests recording; normal firmware
would interpret the same command as ADC capture at 30000 samples/second.
Progress prints every 45 seconds. After a running report followed by any other
test result, collect 15 more seconds of logs; this does not confirm file closure.
A 90-second start timeout or 1920-second overall timeout raises an error. The
finally block attempts Stop and then closes the port, without waiting for an
acknowledgment. If that write raises an error, the explicit close is not reached.
"""
import pathlib
import serial
import sys
import time

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
p = serial.Serial()
p.port = 'COM4'
p.baudrate = 115200
p.timeout = .2
p.dtr = False
p.rts = False
p.open()
command_id = int(time.time())
began = time.monotonic()
next_report = began
started = False
finished = None
latest_sd = ''
latest_choke = ''
try:
    p.reset_input_buffer()
    p.write(f'CMD,3,{command_id},start,30000,1\n'.encode())
    with pathlib.Path(sys.argv[1]).open('w', encoding='utf-8') as log:
        while True:
            line = p.readline().decode('utf-8', 'replace').strip()
            now = time.monotonic()
            if line:
                log.write(line + '\n')
                log.flush()
                if line.startswith('SD '):
                    latest_sd = line
                if line.startswith('CHOKE target='):
                    latest_choke = line
                    if 'result=running' in line:
                        started = True
                    elif started and finished is None:
                        finished = now
                        print(line, flush=True)
                if 'SD readback' in line or 'event=final' in line or 'SDMMC' in line:
                    print(line, flush=True)
            if now >= next_report:
                print(latest_choke, flush=True)
                print(latest_sd, flush=True)
                next_report = now + 45
            if finished and now - finished > 15:
                print('FINAL ' + latest_choke, flush=True)
                print('FINAL ' + latest_sd, flush=True)
                break
            if (not started and now - began > 90) or now - began > 1920:
                raise RuntimeError('Host watchdog: expected start/completion did not arrive; sending Stop.')
finally:
    p.write(f'CMD,3,{command_id + 1},stop,0,0\n'.encode())
    p.close()
