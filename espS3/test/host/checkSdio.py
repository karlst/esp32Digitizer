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

"""Reboot the connected S3 on COM4 and save eight seconds of boot/status output.

Run manually with one output-log path. Opening the port keeps DTR/RTS low; the
explicit reboot command restarts firmware. This is a hardware operation, not a
desktop unit test. Startup firmware may write a card verification file.
COM4 must be the S3: this script does not identify the connected board.
The log records observations; it makes no pass/fail checks or reboot confirmation.
"""
import serial,time,pathlib,sys
sys.stdout.reconfigure(encoding="utf-8",errors="replace")
p=serial.Serial();p.port="COM4";p.baudrate=115200;p.timeout=.2;p.dtr=False;p.rts=False;p.open()
p.write(("CMD,3,%d,reboot,0,0\n"%int(time.time())).encode())
end=time.monotonic()+8
with pathlib.Path(sys.argv[1]).open("w",encoding="utf-8") as f:
 while time.monotonic()<end:
  line=p.readline().decode("utf8","replace").strip()
  if line:
   f.write(line+"\n");f.flush()
   if not line.startswith(("ADC ","Link ","Timing ")): print(line,flush=True)
p.close()
