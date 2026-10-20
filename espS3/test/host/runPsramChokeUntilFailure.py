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

"""Start the installed Choke firmware on COM4 and log until it stops.

Run manually with an output-log path. Start records synthetic data to a new file;
the installed Choke build selects the generation rate. The script assumes that
firmware is installed and does not check. With normal firmware, the same Start
command instead requests ADC recording at 30000 samples/second.
Keep the fixed-duration runner for reliability tests; this ramp has no deadline.
After a non-idle, non-running test report it collects eight more seconds of logs.
The finally block attempts Stop, then closes the port; a failed Stop write can
prevent that close. No acknowledgment of Stop is required by this script.
"""
import serial,time,pathlib,sys
sys.stdout.reconfigure(encoding='utf-8',errors='replace')
p=serial.Serial();p.port='COM4';p.baudrate=115200;p.timeout=.2;p.dtr=False;p.rts=False;p.open()
commandId=int(time.time());lastStage=None;finished=None;started=False
try:
 p.reset_input_buffer();p.write(('CMD,3,%d,start,30000,1\n'%commandId).encode())
 with pathlib.Path(sys.argv[1]).open('w',encoding='utf-8') as f:
  while True:
   line=p.readline().decode('utf8','replace').strip()
   if line:
    f.write(line+'\n');f.flush()
    if line.startswith('CHOKE target='):
     stage=line.split(' generated=')[0]
     if 'result=running' in line: started=True
     if stage!=lastStage or 'result=running' not in line:
      print(line,flush=True);lastStage=stage
     if 'result=idle' not in line and 'result=running' not in line and finished is None: finished=time.monotonic()
    elif 'SDMMC' in line or 'SD readback' in line or 'error' in line.lower() and not line.startswith(('ADC ','SD ','STAT,')):
     print(line,flush=True)
    if finished and line.startswith('SD '): print(line,flush=True)
   if finished and time.monotonic()-finished>8: break

finally:
 p.write(('CMD,3,%d,stop,0,0\n'%(commandId+1)).encode());p.close()





