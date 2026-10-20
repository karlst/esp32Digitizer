# P4 capture wiring plan - October 17, 2026

Status: Karl has moved the signal wires and verified common ground continuity.
The standalone `espP4` diagnostic is implemented, but has not yet been
compiled or uploaded. P4 currently runs Hello World. See
[capture instructions](../espP4/README.md) for the next steps and limitations.

## Hardware and purpose

Waveshare ESP32-P4-Module-DEV-KIT, chip revision 1.3. ESP-IDF 5.5.6.
Feather ESP32 V2 continues configuring the ADS127L18 through the existing
voltage translators. P4 replaces the S3 as the sample receiver. The first test
captures into memory only; it does not write to SD. C6 wireless is not needed.

## Confirmed signal assignments

Numbers below are GPIO labels, NOT physical header positions. Identify the
printed board labels before connecting. The existing ADC wiring comes from
shared/ads127Pins.h; the Feather link comes from the two capture test sources.

| Signal | Former S3 endpoint | Current P4 endpoint |
|---|---|---|
| ADC DCLK through translator A5: incoming data clock | GPIO9 | GPIO0, input |
| ADC FSYNC through translator A6: sample frame marker | GPIO10 | GPIO1, input |
| ADC DOUT0 through translator A7: sample data | GPIO11 | GPIO2, input |
| Feather TX / GPIO8: commands to receiver | GPIO18 | GPIO4, UART receive |
| Feather RX / GPIO7: replies from receiver | GPIO17 | GPIO5, UART transmit |
| Shared signal ground | Existing common GND | P4 header GND |

Keep GPIO37/38 reserved for the P4 USB-to-UART console. GPIO0/1/2/4/5 are
exposed on the carrier header; the reviewed carrier schematic does not attach
these five nets to its Ethernet, audio or SD devices. The installed PARLIO driver
routes external clock and data inputs through the GPIO matrix, so we need not
reuse the S3 pin numbers. Check actual board markings against the manufacturer
pinout; do not use Raspberry Pi header assumptions.

The translator A side is the existing 3.3 V side. Keep the ADC's 1.8 V side and
Feather-to-ADC configuration wiring intact. P4 capture GPIOs must remain inputs;
they must not drive the ADC signals. The S3 ends have been moved;
do not leave extra branches to the old receiver. Keep sample-clock/data
wires short with a nearby ground return.

## Confirmed power arrangement

The old Feather USB-pin-to-S3 5 V jumper has been removed. Feather and P4 use
separate USB power/data cables; their 5 V and 3.3 V rails are not joined.
Translator VDDA now comes from Feather 3.3 V; VDDB comes from the ADC assembly.
A separate 6 V supply powers the ADC assembly. Common signal ground remains
connected and has been checked for continuity. Feather alone was verified to
have 5 V while the disconnected S3 remained unpowered. P4 was identified on
COM3 and Feather on COM4; recheck after changing USB ports.

Avoid driving signals into an unpowered board. Perform rewiring with all USB
and ADC supplies disconnected. Keep the DAC coax disconnected; this test uses
the ADC internal-short input. The first coordinated power-up remains to be
reviewed before operating the newly linked assembly.

## Bring-up order

1. Power wiring and physical header labels have been checked by Karl.
2. Standalone P4 diagnostic is written and starts idle, with capture pins as
   inputs and no automatic ADC start. The Feather arm/ready/start/stop
   handshake and finite tests are retained. Run its PC decoder checks first.
3. Build for revision <3.0 and 16 MB flash; upload while P4 is disconnected
   from the capture assembly, then power everything down for rewiring.
4. Check the Feather/P4 command link and idle inputs before starting capture.
5. Start with the existing low-rate ADC configuration and inspect frame markers,
   sample counts and error/overflow counters. Advance rates only after review.
6. Establish an independent known-data test before claiming bit-perfect capture;
   the ADC internal-short input is near-zero analog data, not a fixed pattern.
7. Test sustained capture before integrating the SD backend.

Implemented PARLIO approach: record DOUT0 and FSYNC as two bits on each external
DCLK edge, then extract complete ADC words in batches. Sampling edge, bit order,
framing and DMA buffer handling must be verified in firmware against the ADC
configuration. Header matches alone do not prove every payload bit is correct.

## References

- [Waveshare board and header pinout](https://docs.waveshare.com/ESP32-P4-Module-DEV-KIT)
- [Waveshare schematic](https://files.waveshare.com/wiki/ESP32-P4-Module-DEV-KIT/ESP32-P4-Module-DEV-KIT.pdf)
- Existing project: shared/ads127Pins.h, espFeather/test/hardware/ads127CaptureControl.cpp,
  espS3/test/hardware/ads127CaptureDiagnostic.cpp, docs/P4-EARLY-TEST-HISTORY.md.
- Installed ESP-IDF: components/esp_driver_parlio/src/parlio_rx.c (GPIO routing).
