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
 * @file AcquisitionApplication.cpp
 * Run the same ADC handshake for ordinary recording and injected test services.
 * Read requestRun(), acceptReply(), requestStop(), then poll(). Hardware starts
 * only after a matching P4 READY reply. USB disappearance does not cancel the
 * local timeout. Source failures and receiver errors request a coordinated stop.
 */
#include "AcquisitionApplication.h"
#include "ads127Pins.h"
#include "Ads127Config.h"
#include "RecordingTiming.h"
#include <driver/dac.h>
#include <esp_system.h>
#include <cstring>

/* **
 * AcquisitionApplication - Save the ADC driver, application services and time
 * limits in milliseconds. Startup must keep both supplied objects alive.
 * Construction does not access hardware.
 */
AcquisitionApplication::AcquisitionApplication(Ads127LowRate& ads127LowRate,
    AcquisitionServices& acquisitionServices, uint32_t runMs, uint32_t receiverTimeoutMs)
    : _ads127LowRate(ads127LowRate), _acquisitionServices(acquisitionServices),
      _runMs(runMs), _receiverTimeoutMs(receiverTimeoutMs)
{
}

/* **
 * printHelp - Print operator commands without accessing ADC registers.
 */
void AcquisitionApplication::printHelp()
{
    _acquisitionServices.describe(_state == State::idle);
    Serial.printf("g=default (1024k), 0=16k, 1=32k, 2=64k, 3=128k, 4=256k, 5=512k, 6=1024k samples/s; %lu seconds each. s=stop; h=help.\n",
        static_cast<unsigned long>(_runMs / 1000));
    Serial.printf("Matching receiver firmware must allow %lu seconds after receiver READY.\n",
        static_cast<unsigned long>(_receiverTimeoutMs / 1000));
    Serial.println("No automatic ADC commands at boot. Confirm ADC/translator power before g.");
}

/* **
 * requestStop - Request ADC stop, then receiver stop, retaining uncertainty if
 * SPI echo fails. A stop request is not proof of stopped hardware; the echo and
 * subsequent DONE are separate confirmations. After this function returns,
 * poll() waits at most two seconds for receiver DONE; that limit does not bound
 * the time spent making SPI calls or printing the optional input report here.
 */
void AcquisitionApplication::requestStop()
{
    const bool stopped = _ads127LowRate.stop();
    Serial.printf("ADC STOP command %s\n", stopped ? "echo confirmed" : "FAILED / state unconfirmed");
    const bool statusClean = _ads127LowRate.reportStatus("after STOP");
    Serial1.printf("ADC127 STOP %lu\n", _runId);
    _acquisitionServices.finish(_runId, stopped && statusClean && !_acquisitionServices.getFailed());
    _state = State::stopping;
    _stateMs = millis();
}

/* **
 * requestRun - Configure the stopped ADC and ask the P4 to prepare storage and
 * reception. Save the requested ADC settings and generate a nonzero run number
 * so replies can be matched to this request.
 *
 * Refuse the request unless idle and the signal service is ready. Do not start
 * the ADC until a matching READY3 reply arrives. ARM3 sends the run number and
 * sample rate, but not the duration; both boards must already have compatible
 * time limits.
 */
void AcquisitionApplication::requestRun(const Ads127Config& adsConfigRequested)
{
    if (_state != State::idle)
    {
        Serial.println("Test already pending/running; use s to stop.");
    }
    else if (!_servicesReady)
    {
        Serial.println("Signal service setup failed; refusing capture. Reset after fixing initialization.");
    }
    else if (_ads127LowRate.configure(adsConfigRequested, _acquisitionServices.getExternalInput()))
    {
        _adsConfig = adsConfigRequested;
        _acquisitionServices.reset();
        Serial.printf("RECORDING: %lu samples/s for %lu seconds.\n",
            static_cast<unsigned long>(_adsConfig._samplesPerSecond),
            static_cast<unsigned long>(_runMs / 1000));
        _runId = esp_random();
        if (_runId == 0)
        {
            _runId = 1;
        }
        Serial.printf("ADC configured, stopped. Asking receiver to prepare storage and DMA, run %lu.\n", _runId);
        // Report local queue acceptance separately from P4's READY reply.
        char command[80];
        const int length = snprintf(command, sizeof(command), "ADC127 ARM3 %lu %lu\n", _runId,
            static_cast<unsigned long>(_adsConfig._samplesPerSecond));
        const size_t queued = Serial1.write(reinterpret_cast<const uint8_t*>(command), length);
        Serial.printf("LINK ARM queued=%u expected=%u\n", unsigned(queued), unsigned(length));
        _state = State::waiting;
        _stateMs = millis();
    }
}

/* **
 * acceptReply - Process a complete line from the P4. Start the ADC only for a
 * matching READY3 run number and sample rate received before the deadline.
 * Forward status lines to the PC.
 *
 * READY3 means the P4 is ready to receive, not that it has received samples.
 * Ignore stale or unexpected readiness replies. Completion and failure replies
 * drive the separate stopping sequence.
 */
void AcquisitionApplication::acceptReply(const char* line)
{
    if (strncmp(line, "ADC127 ", 7) == 0)
    {
        Serial.println(line);
        unsigned long received = 0;
        char extra = 0;
        unsigned long receivedRate = 0;
        if (sscanf(line, "ADC127 READY3 %lu %lu %c", &received, &receivedRate, &extra) == 2 &&
            receivedRate == _adsConfig._samplesPerSecond &&
            received == _runId && _state == State::waiting &&
            millis() - _stateMs < recordingTiming::readyWaitMs)
        {
            // Enable the source only after the receiver confirms this run ID.
            // No ADC START is sent if the source fails. Skip startup transients
            // when analyzing the file; this is a functional, not precision test.
            const bool sourceStarted = _acquisitionServices.start();
            if (sourceStarted && _ads127LowRate.start())
            {
                _acquisitionServices.begin();
                _state = State::running;
                _stateMs = millis();
                Serial.printf("ADC START echo confirmed. Waiting for receiver DMA data; auto-stop in %lu seconds.\n",
                    static_cast<unsigned long>(_runMs / 1000));
            }
            else
            {
                Serial.println("Source/ADC START failed; stopping both ends.");
                requestStop();
            }
        }
        else if (sscanf(line, "ADC127 RAMFULL %lu %c", &received, &extra) == 1 &&
                 received == _runId && _state == State::running)
        {
            // P4 retained its short RAM window. Stop hardware normally; the
            // matching STOP lets P4 drain decoding before printing the waveform.
            Serial.println("P4 RAM waveform complete; stopping ADC and DAC.");
            requestStop();
        }
        else if (sscanf(line, "ADC127 DONE %lu", &received) == 1 &&
                 received == _runId && _state == State::stopping)
        {
            _state = State::idle;
            Serial.println("Receiver stopped. Counts describe received DMA words; review bad/overrun/error counters.");
        }
        else if ((sscanf(line, "ADC127 TIMEOUT %lu", &received) == 1 ||
                     sscanf(line, "ADC127 FAULT %lu", &received) == 1 ||
                     sscanf(line, "ADC127 FAILED %lu", &received) == 1) &&
                 received == _runId &&
                 (_state == State::waiting || _state == State::running))
        {
            Serial.println("Receiver reported a fault, setup failure or timeout; requesting ADC stop.");
            requestStop();
        }
    }
}

/* **
 * initialize - Leave analog outputs off, open USB/UART, and initialize borrowed
 * services. Arduino startup calls once. poll() alone owns ADC SPI and command
 * handling. A supplied service may prepare its timer or measurements, but this
 * method never starts ADC conversion or a waveform. Initialization failure
 * blocks runs.
 */
void AcquisitionApplication::initialize()
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
    _servicesReady = _acquisitionServices.initialize();
    if (!_servicesReady)
    {
        Serial.println("Signal service initialization FAILED; capture will be refused.");
    }
    delay(500);
    printHelp();
}

/* **
 * poll - Process manual commands, UART replies and bounded run/handshake timers.
 * Serial polling is bounded, but command handlers can spend time configuring
 * hardware or printing reports. Unplugging the PC terminal does not disable the
 * automatic stop while Feather remains powered. The run timer starts after ADC
 * START echo, whereas the receiver deadline starts when its receiver becomes
 * ready.
 */
void AcquisitionApplication::poll()
{
    if (Serial.available())
    {
        const char command = static_cast<char>(Serial.read());
        if (command == '@' && !_collectingTest)
        {
            _consoleLine = BenchLine{};
            _collectingTest = true;
        }
        if (_collectingTest)
        {
            const bool complete = _consoleLine.feed(command);
            if (command == '\n')
            {
                _collectingTest = false;
                if (complete)
                {
                    handleTestLine(_consoleLine.getValue());
                }
                else
                {
                    Serial.println("TEST rejected");
                }
            }
        }
        else if (command == 'g' || command == 'G' || command == '6')
        {
            requestRun(adsConfig);
        }
        else if (command >= '0' && command <= '5')
        {
            // Keys choose actual rates; their numeric value is never passed
            // into the driver, recording application or serial protocol.
            const uint32_t samplesPerSecond = 16000U << (command - '0');
            const Ads127Config* selectedConfig = getAds127Config(samplesPerSecond);
            if (selectedConfig != nullptr)
            {
                requestRun(*selectedConfig);
            }
        }
        else if ((command == 's' || command == 'S') && _state != State::idle)
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
        if (_benchLine.feed(static_cast<char>(Serial1.read())))
        {
            acceptReply(_benchLine.getValue());
        }
    }

    // Services run on this task, so ADC stop and UART commands never run inside
    // a source timer callback. A recorded failure stops both ends of the capture.
    if (_state == State::running)
    {
        _acquisitionServices.poll();
        if (_acquisitionServices.getFailed())
        {
            Serial.println("Signal or measurement service failed; stopping capture.");
            requestStop();
        }
    }

    // Allow 185 seconds for P4's storage preparation and reply. P4's own limit
    // is 180 seconds; recording time begins only after READY and ADC startup.
    // Unsigned subtraction remains valid across the millis() counter wraparound.
    const uint32_t elapsed = millis() - _stateMs;
    if (_state == State::waiting && elapsed >= recordingTiming::readyWaitMs)
    {
        Serial.println("No matching receiver READY within preparation limit; ADC was not started.");
        requestStop();
    }
    else if (_state == State::running && elapsed >= _runMs)
    {
        requestStop();
    }
    else if (_state == State::stopping && elapsed >= 2000)
    {
        Serial.printf("No receiver DONE reply; receiver state unconfirmed. Expected local timeout is %lu seconds after receiver READY.\n",
            static_cast<unsigned long>(_receiverTimeoutMs / 1000));
        _state = State::idle;
    }
    // A service may request faster polling while collecting observations.
    if (_state == State::running && _acquisitionServices.getSampling())
    {
        delayMicroseconds(50);
    }
    else
    {
        delay(1);
    }
}

/* **
 * handleTestLine - Accept @TEST duration while idle, without starting source or
 * acquisition. The bench service validates supported durations. Ordinary
 * production services refuse this command. The local timer still stops the ADC
 * if the PC disconnects.
 */
void AcquisitionApplication::handleTestLine(const char* line)
{
    // Idle-only probe exercises Serial1 without touching ADC or disk. A successful
    // write means queued locally, not received by P4; its echoed nonce proves that.
    unsigned long nonce = 0;
    char trailing = 0;
    if (sscanf(line, "@LINK %lu %c", &nonce, &trailing) == 1)
    {
        if (_state == State::idle)
        {
            char command[64];
            const int length = snprintf(command, sizeof(command), "ADC127 LINK %lu\n", nonce);
            const size_t queued = Serial1.write(reinterpret_cast<const uint8_t*>(command), length);
            Serial.printf("LINK TX nonce=%lu queued=%u expected=%u RX=%u TX=%u\n",
                nonce, static_cast<unsigned>(queued), static_cast<unsigned>(length), unsigned(RX), unsigned(TX));
        }
        else
        {
            Serial.println("LINK rejected: Feather not idle");
        }
        return;
    }
    unsigned seconds = 0;
    char extra = 0;
    const bool accepted = _state == State::idle && sscanf(line, "@TEST %u %c", &seconds, &extra) == 1 &&
                          (seconds == 30 || seconds == 300 || seconds == 600) && _acquisitionServices.configureDuration(seconds);
    if (accepted)
    {
        _runMs = seconds * 1000;
        _receiverTimeoutMs = (seconds + 5) * 1000;
        Serial.printf("TEST OK seconds=%u\n", seconds);
    }
    else
    {
        Serial.println("TEST rejected");
    }
}
