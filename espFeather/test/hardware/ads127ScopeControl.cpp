/*
 * Copyright (C) 2026 Karl Stock
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * Scope-only ADC control from the Feather USB terminal, at 115200 baud.
 * Reading path: setup leaves command pins undriven; loop accepts g/s/h;
 * startSampling configures the existing driver; stopSampling reports stop/status.
 * Arduino calls these from one task (its sequential setup/loop execution).
 * No interrupts, queues, P4 UART, sample buffers, or SD writes are used here.
 *
 * Rigol supplies the analog input and records ADC outputs. A sample is one
 * digitized CH0 voltage, sent as 24 data bits following 8 status bits. The shared
 * default configuration is the same as the earlier external-input calibration:
 * 1,024,000 samples/s, low-latency filter OSR 16, DCLK 32.768 MHz, 2.5-V reference.
 * The driver performs the actual SPI register writes and verifies readback.
 * This test does not measure output rate, decode samples, or establish delay.
 * SPI (Serial Peripheral Interface) is the command connection to the ADC;
 * DCLK/FSYNC/DOUT0 are separate sample-output signals recorded by the scope.
 * The ADC's own hardware produces those signals after START, independently
 * of how often Arduino schedules loop. No software timestamp is made here.
 */
#include <Arduino.h>
#include <driver/dac.h>
#include "Ads127LowRate.h"
#include "ads127Pins.h"

static Ads127L18 ads127L18(SPI, ads127Pins::featherClock, ads127Pins::featherMiso,
    ads127Pins::featherMosi, ads127Pins::featherSelect);
static Ads127LowRate ads127LowRate(ads127L18);
static constexpr uint32_t runLimitMs = 300000;
static uint32_t startMs = 0;
static bool running = false;
static bool faulted = false;

/* **
 * printHelp - Explain USB commands without touching the ADC. Starting requires
 * powered ADC/control translators and an already prepared scope; there is no
 * P4 readiness check. The timeout gives five minutes to freeze a scope record.
 */
static void printHelp()
{
    Serial.println("SCOPE_ONLY version=1; external CH0; P4 not required; Feather DAC disabled.");
    Serial.println("g=configure/start, s=stop, h=help; automatic STOP after 300 seconds.");
    Serial.println("Power ADC/control translators and prepare scope before g. No SD recording.");
}

/* **
 * stopSampling - Send STOP through the existing driver, then read ADC status.
 * An echoed command confirms acceptance, not instantaneous cessation of output.
 * Failure latches a fault: another start is refused until reset and inspection.
 * Manual s remains available to retry STOP. USB disconnection does not stop the
 * local timeout, but loss of Feather power prevents firmware from enforcing it.
 * reason is a printable label supplied by loop or startSampling. No result is
 * returned; the console reports command acknowledgement and status separately.
 */
static void stopSampling(const char* reason)
{
    // Reopen the command interface before asking the ADC to stop. Clear the
    // software running flag even on failure; faulted prevents another start.
    // This flag describes our command sequence, not measured output activity.
    ads127L18.begin();
    const bool stopped = ads127LowRate.stop();
    const bool clean = ads127LowRate.reportStatus("scope STOP");
    running = false;
    faulted = faulted || !stopped || !clean;
    Serial.printf("SCOPE_STOP reason=%s echo=%u statusClean=%u faulted=%u\n",
        reason, unsigned(stopped), unsigned(clean), unsigned(faulted));
    if (!stopped)
    {
        Serial.println("STOP unconfirmed: turn off ADC power before rewiring.");
    }
}

/* **
 * startSampling - Handle an explicit g from the operator. Reuse the calibration
 * driver's external-input setup, including stop, status checks and readback.
 * Only successful configuration permits START. Repeated g while running cannot
 * extend the timeout. A failed start attempts STOP and latches a fault.
 */
static void startSampling()
{
    if (running || faulted)
    {
        Serial.println("START refused: already running or fault latched; s stops, reset clears fault.");
    }
    else
    {
        // true selects the external analog source rather than the Feather DAC
        // (Digital-to-Analog Converter). A failed configuration short-circuits
        // the START call so an unchecked configuration cannot begin this run.
        const bool configured = ads127LowRate.configure(adsConfig, true);
        const bool started = configured && ads127LowRate.start();
        if (started)
        {
            // Start the local five-minute deadline only after acknowledgement.
            // The requested rate comes from configuration, not counted pulses.
            startMs = millis();
            running = true;
            Serial.printf("SCOPE_RUNNING requestedSamplesPerSecond=%lu timeoutSeconds=300\n",
                static_cast<unsigned long>(adsConfig._samplesPerSecond));
            Serial.println("START echo confirmed; inspect FSYNC/DCLK/DOUT0 on scope to verify output.");
        }
        else
        {
            faulted = true;
            stopSampling("start failed");
        }
    }
}

/* **
 * setup - Arduino calls once after boot. Disable the Feather analog generators
 * and leave SPI command pins as inputs until g or s. No ADC command is sent at
 * boot, so this cannot promise a separately powered ADC was already stopped.
 */
void setup()
{
    // Release the two built-in analog outputs and command pins at boot. The
    // external generator supplies CH0; the firmware must not drive that source.
    dac_output_disable(DAC_CHANNEL_1);
    dac_output_disable(DAC_CHANNEL_2);
    pinMode(25, INPUT);
    pinMode(26, INPUT);
    pinMode(ads127Pins::featherClock, INPUT);
    pinMode(ads127Pins::featherMosi, INPUT);
    pinMode(ads127Pins::featherMiso, INPUT);
    pinMode(ads127Pins::featherSelect, INPUT);

    // Allow the USB serial connection time to settle before printing help.
    // This wait does not establish whether the independently powered ADC runs.
    Serial.begin(115200);
    delay(500);
    printHelp();
}

/* **
 * loop - Arduino repeatedly services USB and the elapsed-time limit. Unsigned
 * subtraction handles the millisecond counter wrapping. One character per pass
 * prevents a busy terminal from starving the timeout; line endings are ignored.
 * delay(1) yields execution to framework tasks without slowing ADC hardware.
 */
void loop()
{
    if (running && static_cast<uint32_t>(millis() - startMs) >= runLimitMs)
    {
        stopSampling("timeout");
    }

    // Serial.read consumes one received character. Unrecognized characters,
    // including Enter's line endings, cause no command to be sent to the ADC.
    if (Serial.available())
    {
        const char command = static_cast<char>(Serial.read());
        if (command == 'g' || command == 'G')
        {
            startSampling();
        }
        else if (command == 's' || command == 'S')
        {
            stopSampling("operator");
        }
        else if (command == 'h' || command == 'H')
        {
            printHelp();
        }
    }

    delay(1);
}
