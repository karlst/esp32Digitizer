"""Reboot the connected S3 on COM4 and save eight seconds of boot/status output.

Run manually with one output-log path. Opening the port keeps DTR/RTS low; the
explicit reboot command restarts firmware. This is a hardware operation, not a
desktop unit test. Existing card contents are not altered by this script itself.
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
