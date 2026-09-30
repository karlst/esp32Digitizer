/**
 * @file ads1256.cpp
 * @brief Set up the ADS1256 digitizer and read its samples over SPI.
 *
 * A sample is one signed 24-bit number measuring the voltage at AIN0 minus
 * the voltage at AIN1. The digitizer makes samples at its configured rate;
 * the S3 supplies the SPI clock pulses needed to retrieve each number.
 *
 * DRDY (Data Ready, S3 pin 9) goes from high to low when a sample is ready.
 * CS (Chip Select, pin 10) is held low while we talk to the digitizer.
 * SCLK (pin 12) is the clock; DIN (pin 11) carries commands to the digitizer;
 * DOUT (pin 13) carries results back to the S3.
 *
 * acquisition.cpp decides WHEN to read. This file implements the task reader;
 * at 30k, fastCapture.cpp instead reads the same SPI hardware in the interrupt.
 * Only the acquisition background task calls this driver. SPI hardware moves
 * the bits; the CPU waits for each transfer to finish. This code uses no DMA.
 * It returns samples to its caller; it does not save a recording.
 *
 * Command values and delays follow TI's ADS1256 datasheet, SBAS288K.
 * Timing assumes a 7.68 MHz digitizer clock, separate from our SPI clock.
 * Reference: https://www.ti.com/lit/ds/symlink/ads1256.pdf
 */
#include "ads1256.h"
#include "commandProtocol.h"
#include <freertos/FreeRTOS.h>

/**
 * @brief Wait for the digitizer to pull its Data Ready wire low.
 *
 * Used during setup and stopping, not for the normal per-sample wait.
 * Depending on the preceding command, low means a sample is ready or a
 * reset/calibration has finished. A disconnected board must not hang the S3.
 * @param timeoutMs Maximum wait in milliseconds.
 * @return True if DRDY is low when checked; false if it remains high.
 */
bool ads1256::waitReady(uint32_t timeoutMs)
{
    // Yield between checks so other tasks can run during this longer wait.
    const uint32_t started = millis();
    while (digitalRead(readyPin) != LOW && millis() - started < timeoutMs)
    {
        delay(1);
    }
    const bool retVal = digitalRead(readyPin) == LOW;
    return retVal;
}

/**
 * @brief Send a one-byte digitizer command, then allow time for it to take effect.
 * @param opcode Command byte defined by the ADS1256 datasheet.
 *
 * The caller must already have selected the digitizer by pulling CS low.
 * This transfer does not confirm that the digitizer accepted the command;
 * callers check DRDY or read settings back where needed.
 */
void ads1256::command(uint8_t opcode)
{
    bus.transfer(opcode);
    // The datasheet requires a delay between certain commands and the next
    // transfer. Seven microseconds covers its 50-digitizer-clock read delay
    // (called t6) and 24-clock SYNC delay (t11) at our assumed 7.68 MHz clock.
    // Longer operations such as reset/calibration need additional caller waits.
    delayMicroseconds(7);
}

/**
 * @brief Read back the four configuration registers so we can verify our settings.
 * @param values Caller-provided storage for at least four bytes, in order:
 * STATUS (general options), MUX (input selection), ADCON (gain and auxiliary
 * options), and DRATE (sample rate). The caller must hold CS low.
 */
void ads1256::readRegisters(uint8_t* values)
{
    // RREG (0x10) means read registers starting at address zero.
    // The second byte is the number of registers MINUS ONE: 3 requests four.
    bus.transfer(0x10);
    bus.transfer(3);
    delayMicroseconds(7);
    // SPI needs outgoing clock pulses even when we only want to receive.
    // Sending harmless zero bytes supplies those clocks and collects the reply.
    uint8_t zeros[4] = {};
    bus.transferBytes(zeros, values, 4);
    delayMicroseconds(1);
}

/**
 * @brief Select AIN0 minus AIN1, set the sample rate, and calibrate the digitizer.
 * @param rate Requested samples per second, not the SPI clock frequency.
 * @return True if the rate is supported, settings read back correctly, and
 * calibration finishes within one second. The caller must hold CS low.
 */
bool ads1256::configure(uint32_t rate)
{
    // Convert a human-readable rate such as 1000 to the device's register code.
    // Unsupported rates fail without writing a new configuration.
    uint8_t rateByte = 0;
    bool retVal = commandProtocol::rateRegister(rate, rateByte);
    if (retVal)
    {
        // Send WREG (0x50: write registers starting at zero), then 3 (four
        // registers minus one), followed by the four register values:
        // STATUS=0x00: input buffer and automatic calibration off. The buffer
        // has input-voltage restrictions; disabling it suits our grounded AIN1.
        // MUX=0x01: measure AIN0 as positive input and AIN1 as negative input.
        // ADCON=0x00: gain 1 (no extra amplification), unused clock output and
        // sensor-test current sources off. DRATE=rateByte: requested sample rate.
        const uint8_t settings[] = {0x50, 3, 0x00, 0x01, 0x00, rateByte};
        bus.writeBytes(settings, sizeof(settings));
        delayMicroseconds(7);
        // Verify communication by reading back what we just wrote.
        uint8_t actual[4] = {};
        readRegisters(actual);
        // STATUS includes read-only identification and readiness bits, so mask
        // those out with 0x0e and check only the writable options. Compare the
        // other three registers in full. This also rejects replies consisting
        // entirely of zeros or ones, as can happen with missing connections.
        retVal = (actual[0] & 0x0e) == 0 && actual[1] == 0x01 &&
            actual[2] == 0 && actual[3] == rateByte;
        if (retVal)
        {
            // SELFCAL (0xf0) asks the chip to calibrate its internal offset and
            // gain. Wait for it to begin, then for DRDY to signal completion.
            // This is chip calibration, not proof of whole-board voltage accuracy.
            command(0xf0);
            delayMicroseconds(10);
            retVal = waitReady(1000);
        }
    }
    return retVal;
}

/**
 * @brief Initialize SPI, reset and check the digitizer, then leave it in standby.
 *
 * Called by the acquisition task at startup. A default rate of 1000 samples
 * per second is configured, but collection waits for a later Start command.
 * @return True if the digitizer responds and its setup/calibration succeeds.
 */
bool ads1256::begin()
{
    // Pull DRDY high when nothing drives it, and deselect the digitizer while
    // setting up SPI. bus.begin arguments are clock, incoming data, outgoing
    // data, and chip select: S3 pins 12, 13, 11, and 10 respectively.
    pinMode(readyPin, INPUT_PULLUP);
    pinMode(10, OUTPUT);
    digitalWrite(10, HIGH);
    bus.begin(12, 13, 11, 10);
    // Set a 1 MHz SPI clock, highest bit first, and the clock timing called
    // mode 1 required by this device. beginTransaction reserves/configures the
    // bus; it does not read a sample. Only this task uses this bus, so we keep
    // that reservation open rather than acquire/release its lock on every read.
    bus.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE1));
    // Select the chip and send WAKEUP (0x00) in case it was left in standby.
    digitalWrite(10, LOW);
    command(0x00);
    bool retVal = waitReady(1000);
    if (retVal)
    {
        // The S3 may have restarted without the digitizer losing power.
        // SDATAC (0x0f) exits continuous-read mode left by an earlier run.
        // RESET (0xfe) restores the chip's defaults before we configure it.
        command(0x0f);
        command(0xfe);
        delay(5);
        retVal = waitReady(1000) && configure(1000);
    }
    // STANDBY (0xfd) stops conversions; CS high ends our conversation with the
    // chip. Attempt this even if setup failed, but do not claim setup succeeded.
    command(0xfd);
    digitalWrite(10, HIGH);
    return retVal;
}

/**
 * @brief Wake the digitizer and prepare it to supply samples at the requested rate.
 * @param rate Samples per second. Call this while acquisition is stopped.
 * @return True if configuration/calibration succeeds and reads can begin.
 *
 * This starts the device; acquisition.cpp chooses the task or interrupt reader.
 * CS stays low during acquisition so those SPI reads reach the chip.
 */
bool ads1256::start(uint32_t rate)
{
    // Release the old SPI settings and select the clock for this run. Up to
    // 2000 samples/second we use a conservative 1 MHz clock on the jumper wires.
    // Higher rates use 1.9 MHz to shorten each transfer. This is the speed of
    // transferring bits, not the rate at which the digitizer measures voltages.
    bus.endTransaction();
    bus.beginTransaction(SPISettings(rate <= 2000 ? 1000000 : 1900000, MSBFIRST, SPI_MODE1));
    digitalWrite(10, LOW);
    // WAKEUP (0x00) leaves standby. Only configure after the chip responds.
    command(0x00);
    bool retVal = waitReady(1000) && configure(rate);
    if (retVal)
    {
        // Low rates send a separate RDATA (read data) command for every sample.
        // Higher rates use RDATAC (0x03: read data continuously), which lets us
        // fetch each new sample without repeating that command and its delay.
        // At 30000 samples/second only 33.3 microseconds separate samples;
        // fastCapture handles those reads without a per-sample task switch.
        continuousRead = rate > 2000;
        if (continuousRead)
        {
            command(0x03);
        }
        // Despite its name, continuous means "this driver is actively reading"
        // in EITHER mode. continuousRead specifically means RDATAC mode is used.
        continuous = true;
    }
    else
    {
        // Setup failed: request standby and deselect the chip instead of reading.
        command(0xfd);
        digitalWrite(10, HIGH);
    }
    return retVal;
}

/**
 * @brief Stop reading samples and ask the digitizer to enter standby.
 * @return True if already stopped or the chip becomes ready for the stop
 * commands. False if DRDY does not go low within 100 milliseconds.
 *
 * On failure we still stop software reads and deselect the chip, but cannot
 * confirm that the chip itself stopped converting the analog input.
 */
bool ads1256::stop()
{
    bool retVal = true;
    if (continuous)
    {
        // Wait for a sample boundary before issuing stop commands. Exit RDATAC
        // with SDATAC (0x0f) only if that mode was used, then send STANDBY (0xfd).
        retVal = waitReady(100);
        if (retVal)
        {
            if (continuousRead)
            {
                // SDATAC must fit entirely between a fresh DRDY falling edge
                // and the following update. A millisecond-scale waitReady()
                // finds an arbitrary point in that window and can be too late
                // at 30k. Briefly prevent task preemption, observe high then low,
                // and send the eight stop bits immediately. Bound the whole
                // edge wait to 1 ms (even 7500/s supplies an edge within 134 us).
                portMUX_TYPE stopLock = portMUX_INITIALIZER_UNLOCKED;
                portENTER_CRITICAL(&stopLock);
                const uint32_t started = micros();
                while (digitalRead(readyPin) == LOW && micros() - started < 1000)
                {
                    // Discard the age-unknown ready interval.
                }
                const bool sawHigh = digitalRead(readyPin) == HIGH;
                while (sawHigh && digitalRead(readyPin) == HIGH && micros() - started < 1000)
                {
                    // The next falling edge opens a full command window.
                }
                retVal = sawHigh && digitalRead(readyPin) == LOW && micros() - started < 1000;
                if (retVal)
                {
                    command(0x0f);
                }
                portEXIT_CRITICAL(&stopLock);
            }
            if (retVal)
            {
                command(0xfd);
            }
        }
        // Block further read() calls even if the device did not respond.
        continuous = false;
    }
    // Deselect the digitizer whether the stop succeeded or failed.
    digitalWrite(10, HIGH);
    return retVal;
}

/**
 * @brief Stop software access without waiting on an already failed SPI controller.
 * The interrupt reader has a hardware deadline; calling the ordinary library's
 * unbounded transfer afterward would defeat that protection. Deselect the ADC
 * and require reboot. We cannot claim the chip entered standby in this case.
 */
void ads1256::abandon()
{
    continuous = false;
    digitalWrite(10, HIGH);
}

/**
 * @brief Read one signed 24-bit sample from the digitizer over SPI.
 * @param sample Receives the raw number (not volts), stored in a 32-bit integer.
 * The caller must discard it if this function returns false.
 * @return True if reading is active, a sample is ready, and DRDY rises after
 * the three bytes are read. This function does not wait for a new sample.
 *
 * acquisition.cpp also checks whether another sample became ready during this
 * read. It rejects that case because the three bytes might not all belong to
 * the same sample. A true result here alone does not perform that extra check.
 */
bool ads1256::read(int32_t& sample)
{
    // Remember why a read failed without slow printing inside the read itself.
    // Diagnostic codes: 1=driver stopped, 2=no ready sample, 3=DRDY did not
    // rise after reading, 0=success. Refuse SPI access unless active AND ready.
    readDetail = continuous ? 2 : 1;
    bool retVal = continuous && digitalRead(readyPin) == LOW;
    if (retVal)
    {
        // SPI sends and receives simultaneously. Send zeros to supply clocks
        // without accidentally sending a command such as stop or reset.
        // Reserve four bytes even though a sample needs only three: the SPI
        // library accesses outgoing storage in four-byte units internally.
        uint8_t zeros[4] = {};
        uint8_t bytes[4] = {};
        const uint32_t startedUs = micros();
        if (!continuousRead)
        {
            // In the low-rate mode, request this sample with RDATA (0x01).
            // command() also waits the required seven microseconds before data.
            command(0x01);
        }
        // Hardware generates 24 SPI clock pulses and receives three bytes.
        // The library waits for completion and copies them into bytes; no DMA
        // or per-bit software reads are involved. Then preserve the sample's
        // sign while expanding it from 24 bits into a signed 32-bit integer.
        // Measure only the transfer separately from the command and ready checks.
        // Store numbers here; printing in this time-sensitive path would change it.
        const uint32_t transferStartUs = micros();
        bus.transferBytes(zeros, bytes, 3);
        transferUs = micros() - transferStartUs;
        sample = commandProtocol::signedSample(bytes);
        // DRDY should rise after the sample is read. Checking immediately after
        // SPI completion rejected reads during bench testing. Allow up to three
        // one-microsecond waits for the pin to read high (plus checking overhead).
        // A pin that stays low still fails; this is not an unlimited wait.
        for (uint8_t attempt = 0; attempt < 3 && digitalRead(readyPin) == LOW; ++attempt)
        {
            delayMicroseconds(1);
        }
        retVal = digitalRead(readyPin) == HIGH;
        // Pack elapsed read time above the lowest eight bits, which hold the
        // diagnostic code. Time includes the command, delays, SPI, and checks.
        readDetail = ((micros() - startedUs) << 8) | (retVal ? 0U : 3U);
    }
    return retVal;
}

/**
 * @brief Return the saved diagnostic from the last read attempt; do not read again.
 * @return Lowest eight bits: result code described in read(). Remaining bits:
 * elapsed microseconds if a transfer was attempted, otherwise zero.
 * The caller decodes the code with value & 0xff and the time with value >> 8.
 */
uint32_t ads1256::readDiagnostic() const
{
    const uint32_t retVal = readDetail;
    return retVal;
}

/**
 * @brief Return the saved SPI-call duration without touching hardware again.
 */
uint32_t ads1256::transferMicros() const
{
    const uint32_t retVal = transferUs;
    return retVal;
}
