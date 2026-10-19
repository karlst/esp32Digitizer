/*
 * Copyright (C) 2026 Karl Stock
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of ESP32 Digitizer.
 * You may redistribute and/or modify it under the GNU General Public License
 * as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.
 */

/**
 * @file ads127CaptureControl.cpp
 * @brief Manual finite ADC/DMA bench test, controlled through Feather USB.
 * setup() leaves DAC off and SPI inputs undriven. USB 'g' configures the ADC,
 * asks the receiver to arm, waits for matching READY, then starts conversions.
 * After the shared run duration (or on 's') it stops ADC then receiver. The named
 * P4 environment selects 60 seconds or 30 minutes; legacy S3 retains five.
 * The opt-in ads127-p4-dac-60s build selects external channel-0 input and starts
 * a fixed DAC sine only after receiver READY. The compare variant also samples
 * A2; ads127-p4-constant holds code 135 instead of generating a sine. Builds
 * without ADS127_P4_DAC_TEST retain internal short and DAC off. A failed start, requested stop or receiver fault disables the DAC.
 * UART reports are forwarded to USB. This is not the production web application.
 * ads127-p4-compare additionally measures A2 during the same run. It exports
 * timestamped input observations only after stopping the ADC and DAC.
 */
#include <Arduino.h>
#include <driver/dac.h>
#include <esp_system.h>
#include <cstring>
#include "ads127l18.h"
#include "ads127LowRate.h"
#include "ads127Pins.h"
#include "benchLine.h"
#include "ads127BenchRate.h"
#include "ads127BenchTiming.h"
#if ADS127_COMPARE_INPUT
#include "inputSnapshot.h"
static inputSnapshot inputTrace;
#endif
#if ADS127_P4_DAC_TEST
#include "benchDac.h"
static benchDac signalSource;
static bool dacReady = false;
static constexpr bool externalInput = true;
#else
static constexpr bool externalInput = false;
#endif

static ads127l18 adc(SPI, ads127Pins::featherClock, ads127Pins::featherMiso,
                     ads127Pins::featherMosi, ads127Pins::featherSelect);
static ads127LowRate setupAdc(adc);
static benchLine replies;
enum class benchState { idle, waiting, running, stopping };
static benchState state = benchState::idle;
static unsigned long runId = 0;
static unsigned stage = 0;
static uint32_t stateMs = 0;

/** @brief Print operator commands without accessing ADC registers. */
static void printHelp()
{
#if ADS127_COMPARE_INPUT
    Serial.println(dacReady ? "COMPARE READY: simultaneous A1->A2 input and P4 RAM output; keep coax connected."
                            : "COMPARE UNAVAILABLE: DAC/input ADC initialization failed.");
#endif
#if ADS127_P4_DAC_TEST
    Serial.println("ADS127 DAC TEST: external CH0; A1/GPIO25 -> CH0P, CH0N grounded; SD controlled by P4.");
#if ADS127_DAC_CONSTANT
    Serial.println("DAC MODE: CONSTANT code=135 nominalVolts=1.747; 500 us repeated writes.");
#else
    Serial.println("DAC MODE: SINE");
    Serial.println("DAC: nominal 10 Hz sine, 1.25 V offset, 0.5 V peak (0.75..1.75 V); 500 us updates.");
#endif
    Serial.println("DAC stays OFF until receiver READY; verify powered analog wiring before starting.");
#else
    Serial.println("ADS127 DMA CONTROL: DAC off; coax stays disconnected; SD untouched.");
#endif
    Serial.printf("g=16k, 1=32k, 2=64k, 3=128k, 4=256k, 5=512k, 6=1024k samples/s; %lu seconds each. s=stop; h=help.\n",
                  static_cast<unsigned long>(ads127BenchTiming::runMs / 1000));
    Serial.printf("Matching receiver firmware must allow %lu seconds from ARM.\n",
                  static_cast<unsigned long>(ads127BenchTiming::receiverTimeoutMs / 1000));
    Serial.println("No automatic ADC commands at boot. Confirm ADC/translator power before g.");
}

/**
 * @brief Request ADC stop, then receiver stop, retaining uncertainty if SPI echo fails.
 * A stop request is not proof of stopped hardware; the echo and subsequent
 * DONE are separate confirmations. Timer bounds waiting even if a wire fails.
 */
static void requestStop()
{
    const bool stopped = setupAdc.stop();
    Serial.printf("ADC STOP command %s\n", stopped ? "echo confirmed" : "FAILED / state unconfirmed");
    const bool statusClean = setupAdc.reportStatus("after STOP");
    Serial1.printf("ADC127 STOP %lu\n", runId);
#if ADS127_P4_DAC_TEST
    // ADC stop is requested first; timer shutdown then leaves A1 undriven. Any
    // final in-flight ADC conversion may include this transition in the tail.
    signalSource.stop();
#endif
#if ADS127_COMPARE_INPUT
    // UART STOP was sent first, allowing P4 to stop and export independently.
    // This local USB report cannot delay active acquisition on either board.
    inputTrace.report(runId, stopped && statusClean && !signalSource.failed());
#else
    (void)statusClean;
#endif
    state = benchState::stopping;
    stateMs = millis();
}

/**
 * @brief Configure while stopped and ask for DMA readiness using a fresh run ID.
 * Nothing starts if register readback fails or no matching READY arrives.
 * ARM2 does not negotiate duration: verify both firmware banners before a run.
 */
static void requestRun(unsigned requestedStage)
{
    if (state != benchState::idle)
    {
        Serial.println("Test already pending/running; use s to stop.");
    }
#if ADS127_P4_DAC_TEST
    else if (!dacReady)
    {
        Serial.println("DAC setup failed; refusing ADC test. Reset after fixing initialization.");
    }
#endif
    else if (setupAdc.configure(requestedStage, externalInput))
    {
        stage = requestedStage;
#if ADS127_COMPARE_INPUT
        inputTrace.reset();
#endif
        Serial.printf("TEST STAGE %u: %lu samples/s for %lu seconds.\n", stage,
                      static_cast<unsigned long>(ads127BenchRates[stage].samplesPerSecond),
                      static_cast<unsigned long>(ads127BenchTiming::runMs / 1000));
        runId = esp_random();
        if (runId == 0)
        {
            runId = 1;
        }
        Serial.printf("ADC configured, stopped. Asking receiver to prepare DMA, run %lu.\n", runId);
        Serial1.printf("ADC127 ARM2 %lu %u\n", runId, stage);
        state = benchState::waiting;
        stateMs = millis();
    }
}

/**
 * @brief Forward bench status and act only on a matching readiness/completion ID.
 * A ready receiver is not evidence of arriving data. Receiver counters report that
 * separately. Unexpected READY or stale replies never start the ADC.
 */
static void acceptReply(const char* line)
{
    if (strncmp(line, "ADC127 ", 7) == 0)
    {
        Serial.println(line);
        unsigned long received = 0;
        char extra = 0;
        unsigned receivedStage = 99;
        if (sscanf(line, "ADC127 READY2 %lu %u %c", &received, &receivedStage, &extra) == 2 &&
            receivedStage == stage &&
            received == runId && state == benchState::waiting)
        {
            // Enable the source only after the receiver confirms this run ID.
            // No ADC START is sent if the source fails. Skip startup transients
            // when analyzing the file; this is a functional, not precision test.
            bool sourceStarted = true;
#if ADS127_P4_DAC_TEST
            sourceStarted = signalSource.start();
            if (sourceStarted)
            {
#if ADS127_DAC_CONSTANT
                Serial.println("DAC ON: constant code 135; starting ADC external-input capture.");
#else
                Serial.println("DAC ON: 10 Hz sine; starting ADC external-input capture.");
#endif
            }
#endif
            if (sourceStarted && setupAdc.start())
            {
#if ADS127_COMPARE_INPUT
                inputTrace.begin();
#endif
                state = benchState::running;
                stateMs = millis();
                Serial.printf("ADC START echo confirmed. Waiting for receiver DMA data; auto-stop in %lu seconds.\n",
                              static_cast<unsigned long>(ads127BenchTiming::runMs / 1000));
            }
            else
            {
                Serial.println("Source/ADC START failed; stopping both ends.");
                requestStop();
            }
        }
        else if (sscanf(line, "ADC127 RAMFULL %lu %c", &received, &extra) == 1 &&
                 received == runId && state == benchState::running)
        {
            // P4 retained its short RAM window. Stop hardware normally; the
            // matching STOP lets P4 drain decoding before printing the waveform.
            Serial.println("P4 RAM waveform complete; stopping ADC and DAC.");
            requestStop();
        }
        else if (sscanf(line, "ADC127 DONE %lu", &received) == 1 &&
                 received == runId && state == benchState::stopping)
        {
            state = benchState::idle;
            Serial.println("Receiver stopped. Counts describe received DMA words; review bad/overrun/error counters.");
        }
        else if ((sscanf(line, "ADC127 TIMEOUT %lu", &received) == 1 ||
                  sscanf(line, "ADC127 FAULT %lu", &received) == 1 ||
                  sscanf(line, "ADC127 FAILED %lu", &received) == 1) && received == runId &&
                 (state == benchState::waiting || state == benchState::running))
        {
            Serial.println("Receiver reported a fault, setup failure or timeout; requesting ADC stop.");
            requestStop();
        }
    }
}

/**
 * @brief Disable analog outputs and open USB plus the existing inter-board UART.
 * loop() alone owns ADC SPI and UART. In the opt-in DAC build, initialize a
 * stopped timer; its task callback owns DAC updates with benchDac's mutex.
 * No waveform or ADC conversion is started here.
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
    Serial1.setRxBufferSize(2048);
    Serial1.setTxBufferSize(512);
    pinMode(RX, INPUT_PULLUP);
    Serial1.begin(115200, SERIAL_8N1, RX, TX);
#if ADS127_P4_DAC_TEST
    dacReady = signalSource.initialize();
#if ADS127_COMPARE_INPUT
    dacReady = inputTrace.initialize() && dacReady;
#endif
    if (!dacReady) { Serial.println("DAC initialization FAILED; capture will be refused."); }
#endif
    delay(500);
    printHelp();
}

/**
 * @brief Process manual commands, UART replies and bounded run/handshake timers.
 * Short nonblocking steps keep Stop responsive. Unplugging the PC terminal does
 * not disable the automatic stop while Feather remains powered. The run timer
 * starts after ADC START echo, whereas the receiver deadline starts at ARM.
 */
void loop()
{
    if (Serial.available())
    {
        const char command = static_cast<char>(Serial.read());
        if (command == 'g' || command == 'G')
        {
            requestRun(0);
        }
        else if (command >= '1' && command <= '6')
        {
            requestRun(static_cast<unsigned>(command - '0'));
        }
        else if ((command == 's' || command == 'S') && state != benchState::idle)
        {
            requestStop();
        }
        else if (command == 'h' || command == 'H')
        {
            printHelp();
        }
    }
    for (unsigned count = 0; count < 384 && Serial1.available(); ++count)
    {
        if (replies.feed(static_cast<char>(Serial1.read())))
        {
            acceptReply(replies.value());
        }
    }

#if ADS127_P4_DAC_TEST
    // The timer callback only latches errors; this task performs SPI stop and
    // UART coordination so callback timing never includes those slow operations.
    if (state == benchState::running && signalSource.failed())
    {
        Serial.println("DAC driver fault; requesting ADC/receiver stop.");
        requestStop();
    }
#endif

#if ADS127_COMPARE_INPUT
    if (state == benchState::running)
    {
        inputTrace.poll();
        if (inputTrace.failed())
        {
            Serial.println("Local input measurement failed; stopping comparison.");
            requestStop();
        }
    }
#endif

    // Unsigned subtraction remains valid across the millis() counter wraparound.
    const uint32_t elapsed = millis() - stateMs;
    if (state == benchState::waiting && elapsed >= 2000)
    {
        Serial.println("No matching receiver READY within 2 seconds; ADC was not started.");
        requestStop();
    }
    else if (state == benchState::running && elapsed >= ads127BenchTiming::runMs)
    {
        requestStop();
    }
    else if (state == benchState::stopping && elapsed >= 2000)
    {
        Serial.printf("No receiver DONE reply; receiver state unconfirmed. Expected local timeout is %lu seconds from ARM.\n",
                      static_cast<unsigned long>(ads127BenchTiming::receiverTimeoutMs / 1000));
        state = benchState::idle;
    }
#if ADS127_COMPARE_INPUT
    // Poll frequently for 300 ms so a 500 us measurement burst still fits into
    // each 1 ms slot. Interrupts and the higher-priority DAC timer remain enabled.
    // This diagnostic uses extra Feather CPU; it is not a production load test.
    if (state == benchState::running && inputTrace.sampling()) { delayMicroseconds(50); }
    else { delay(1); }
#else
    delay(1);
#endif
}
