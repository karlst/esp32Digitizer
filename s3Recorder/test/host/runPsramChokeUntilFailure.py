"""Start the installed Choke firmware on COM4 and log until it stops.

Run manually with an output-log path. Start records synthetic data to a new file;
this script does not select a firmware or a rate. The installed build does that.
Keep the fixed-duration runner for reliability tests; this ramp has no deadline.
A finally block sends Stop and releases the serial port on normal interruption.
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





