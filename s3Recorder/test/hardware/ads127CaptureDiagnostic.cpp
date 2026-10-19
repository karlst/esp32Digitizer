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
 * @file ads127CaptureDiagnostic.cpp
 * @brief Temporary UART-controlled memory-only capture; no web/SD/legacy ADC code.
 * Feather sends ARM with a run number; this program installs DMA then replies
 * READY with that number. Feather starts the ADC only after that reply. STOP
 * releases DMA. Eight seconds after ARM, a local deadline stops it even if the
 * control wire fails. Status is sent once per second to Feather and USB.
 */
#include <Arduino.h>
#include <cstring>
#include "ads127Pins.h"
#include "benchLine.h"
#include "ads127BenchRate.h"
#include "frontend/ads127Dma.h"

static ads127Dma capture;
static benchLine commands;
static unsigned long runId = 0;
static unsigned stage = 0;
static uint32_t armedMs = 0, reportMs = 0;
static uint64_t reportWords = 0;
static bool diagnosticsPending = false;

/**
 * @brief Report received words and validity separately; rate covers the last interval.
 * Raw latest word helps debug framing. Queued partial final buffers are not
 * counted until delivered; these totals are read words, not generated samples.
 */
static void report(const char* label)
{
    const uint32_t now = millis();
    const uint32_t elapsed = now - reportMs;
    const uint32_t rate = elapsed ? (capture.words - reportWords) * 1000 / elapsed : 0;
    char text[360];
    snprintf(text, sizeof(text), "ADC127 %s %lu words=%llu valid=%llu bad=%lu rate=%lu/s raw=%08lX sample=%ld overruns=%lu errors=%lu eventQueueFull=%lu\n",
             label, runId, static_cast<unsigned long long>(capture.words),
             static_cast<unsigned long long>(capture.valid), static_cast<unsigned long>(capture.badHeaders),
             static_cast<unsigned long>(rate), static_cast<unsigned long>(capture.latestWord),
             static_cast<long>(capture.latestSample), static_cast<unsigned long>(capture.overruns),
             static_cast<unsigned long>(capture.errors), static_cast<unsigned long>(capture.queueFullObservations));
    Serial.print(text);
    Serial1.print(text);
    reportMs = now;
    reportWords = capture.words;
}

/**
 * @brief Accept only complete run-numbered commands from the dedicated bench UART.
 * An old READY or STOP cannot affect a new run because Feather chooses a fresh ID.
 * ARM while already active is rejected, except a duplicate for the same run.
 */
static void accept(const char* line)
{
    unsigned long requested = 0;
    char extra = 0;
    unsigned requestedStage = 99;
    if (sscanf(line, "ADC127 ARM2 %lu %u %c", &requested, &requestedStage, &extra) == 2 &&
        requested != 0 && requestedStage < ads127BenchRateCount)
    {
        if (capture.active())
        {
            Serial1.printf("ADC127 %s %lu %u\n",
                requested == runId && requestedStage == stage ? "READY2" : "BUSY", requested, stage);
        }
        else
        {
            runId = requested;
            stage = requestedStage;
            const bool ready = capture.begin(stage);
            diagnosticsPending = ready;
            armedMs = reportMs = millis();
            reportWords = 0;
            Serial1.printf("ADC127 %s %lu %u\n", ready ? "READY2" : "FAILED", runId, stage);
            Serial.printf("Receiver %s for run %lu\n", ready ? "armed" : "failed", runId);
        }
    }
    else if (sscanf(line, "ADC127 STOP %lu %c", &requested, &extra) == 1 && requested == runId)
    {
        capture.poll();
        capture.end();
        report("DONE");

        // Send DONE first so Feather completes its stop handshake immediately.
        // Only then print diagnostics: both ADC and receiver have stopped, so
        // serial delays cannot spoil the capture being measured. Duplicate STOP
        // commands still receive DONE but do not repeat this longer report.
        if (diagnosticsPending)
        {
            capture.reportDiagnostics(Serial1, runId);
            capture.reportDiagnostics(Serial, runId);
            diagnosticsPending = false;
        }
    }
}

/** @brief Leave sample pins as inputs and start only the USB/control UARTs. */
void setup()
{
    pinMode(ads127Pins::s3DataClock, INPUT);
    pinMode(ads127Pins::s3FrameSync, INPUT);
    pinMode(ads127Pins::s3Data, INPUT);

    // Buffer the short once-per-second USB status line. At 256k/s the DMA queue
    // holds only 16 ms; waiting for every USB-UART byte on the wire risks filling it.
    Serial.setTxBufferSize(1024);
    Serial.begin(115200);
    Serial1.setRxBufferSize(1024);
    Serial1.setTxBufferSize(1024);
    pinMode(18, INPUT_PULLUP);
    Serial1.begin(115200, SERIAL_8N1, 18, 17);
    Serial.println("ADS127 DMA bench: waiting for Feather ARM; no SD writes or automatic capture.");
}

/**
 * @brief Drain DMA and bounded UART input, report progress, enforce local timeout.
 * All object/counter access happens in this one Arduino task. Only the framework
 * handles DMA completion interrupts; our UART and sample processing are task code.
 */
void loop()
{
    capture.poll();
    for (unsigned count = 0; count < 128 && Serial1.available(); ++count)
    {
        if (commands.feed(static_cast<char>(Serial1.read())))
        {
            accept(commands.value());
        }
    }
    if (capture.active())
    {

        // Any observed bad word or lost DMA block ends this stage immediately.
        // Feather receives FAULT and stops the ADC; no higher rate starts itself.
        if (capture.badHeaders || capture.overruns || capture.errors || capture.queueFullObservations)
        {
            capture.end();
            report("FAULT");
        }
        else if (millis() - armedMs >= 8000)
        {
            capture.end();
            report("TIMEOUT");
        }
        else if (millis() - reportMs >= 1000)
        {
            report("STATS");
        }
    }
    delay(1);
}
