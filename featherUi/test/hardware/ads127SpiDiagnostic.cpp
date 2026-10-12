/**
 * @file ads127SpiDiagnostic.cpp
 * @brief Manual, read-only Feather communication test for the new ADC.
 * Arduino calls setup once and loop repeatedly in its main task. No web server,
 * DAC task, Feather-S3 commands or acquisition code is present in this build.
 * Read setup() for the idle pin states, then runCheck() for the manual test.
 * USB 'r' is the operator's statement that power/level-shifter checks passed;
 * this firmware cannot measure ADC IOVDD or verify those checks automatically.
 */
#include <Arduino.h>
#include <driver/dac.h>
#include "ads127l18.h"
#include "ads127Pins.h"

// Global construction only stores references. SPI begins on the first USB 'r'.
static ads127l18 adc(SPI, ads127Pins::featherClock, ads127Pins::featherMiso,
                     ads127Pins::featherMosi, ads127Pins::featherSelect);

/**
 * @brief Print instructions without changing GPIOs or communicating with the ADC.
 */
static void printHelp()
{
    Serial.println("ADS127L18 READ-ONLY CHECK: DAC off; no sampling, web UI or S3 commands.");
    Serial.println("First verify ADC power, 1.8-V IOVDD, translator supplies and MODE/RESET/START wiring.");
    Serial.println("Then send r to read ID/revision/status three times at 100 kHz SPI mode 1; h repeats help.");
    Serial.println("No SPI traffic until r. Power-on default SPI CRC must be disabled. No registers are written.");
}

/**
 * @brief Read identification and status three times and report raw bytes/results.
 * Register 0 is device ID (0x06 for L18); 1 is silicon revision (not fixed);
 * 2 contains diagnostic flags. A stable ID/revision is evidence of this slow
 * command/reply link only, not proof of analog accuracy or high-speed capture.
 */
static void runCheck()
{
    adc.begin();
    bool identityOk = true;
    uint8_t firstRevision = 0;
    uint8_t combinedStatus = 0;
    Serial.println("READ CHECK BEGIN");
    for (unsigned pass = 0; pass < 3; ++pass)
    {
        uint8_t device = 0, revision = 0, status = 0;
        const bool transferred = adc.readRegister(0x00, device) &&
            adc.readRegister(0x01, revision) && adc.readRegister(0x02, status);
        if (pass == 0)
        {
            firstRevision = revision;
        }
        identityOk = identityOk && transferred && device == 0x06 && revision == firstRevision;
        combinedStatus |= status;
        Serial.printf("Read %u/3: DEV_ID=0x%02X (L18 expected 0x06), REV_ID=0x%02X, STATUS=0x%02X\n",
                      pass + 1, device, revision, status);

        // Separate observations without flooding the terminal. delay() yields
        // this task; there is no sample producer or interrupt reader to service.
        delay(20);
    }

    if (identityOk)
    {
        Serial.println("IDENTITY MATCH: L18 replied consistently at 100 kHz; inspect status below.");
        Serial.printf("Status across reads: analog-low history=%u, reset history=%u, SPI-CRC error=%u, "
                      "register-CRC error=%u, ADC error=%u, address error=%u, clock-count error=%u\n",
                      !!(combinedStatus & 0x40), !!(combinedStatus & 0x20), !!(combinedStatus & 0x10),
                      !!(combinedStatus & 0x08), !!(combinedStatus & 0x04), !!(combinedStatus & 0x02),
                      !!(combinedStatus & 0x01));
        Serial.println("Analog-low and reset flags both default to 1 at power-on (STATUS 0x60).");
        Serial.println("They are history flags, not a live voltage measurement; this test leaves them uncleared.");
        if (combinedStatus & 0x9f)
        {
            Serial.println("STATUS NEEDS INVESTIGATION: error/reserved bits set; do not proceed to acquisition.");
        }
    }
    else
    {
        Serial.println("IDENTITY CHECK FAILED: missing/wrong/unstable reply. Raw status cannot be trusted.");
        Serial.println("All 00 or FF can mean wiring/power/enable/mode trouble; they do not identify one cause.");
    }
    Serial.println("READ CHECK END: no start, reset, configuration writes or DAC output requested.");
}

/**
 * @brief Disable both DAC outputs and leave SPI pins undriven until requested.
 * DAC is the Feather's analog voltage generator. Disabling it and selecting
 * ordinary inputs stops this firmware from feeding an unpowered ADC analog pin.
 * This describes application startup, not the ROM bootloader's transient states.
 */
void setup()
{
    dac_output_disable(DAC_CHANNEL_1);
    dac_output_disable(DAC_CHANNEL_2);
    pinMode(25, INPUT);
    pinMode(26, INPUT);
    pinMode(ads127Pins::featherClock, INPUT);
    pinMode(ads127Pins::featherMosi, INPUT);
    pinMode(ads127Pins::featherMiso, INPUT);
    pinMode(ads127Pins::featherSelect, INPUT);
    Serial.begin(115200);
    delay(500);
    printHelp();
}

/**
 * @brief Accept one manual diagnostic command; never run on a timer or at boot.
 * Newlines are ignored so the terminal's line-ending setting does not matter.
 * A finite test returns to this loop; there are no automatic retry attempts.
 */
void loop()
{
    if (Serial.available())
    {
        const char command = static_cast<char>(Serial.read());
        if (command == 'r' || command == 'R')
        {
            runCheck();
        }
        else if (command == 'h' || command == 'H')
        {
            printHelp();
        }
    }
    delay(1);
}
