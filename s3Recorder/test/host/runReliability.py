"""Log the authorized timed run; the S3 owns the 1800-second deadline.

This monitor prints a compact progress snapshot every 45 seconds, retains every
serial line, and waits for orderly drain/close after the producer stops. A host
watchdog sends Stop if the device never starts or exceeds its expected duration.
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
