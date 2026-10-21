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
 * @brief Run the same ADC handshake for ordinary recording and injected test services.
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

/**
 * @brief Borrow services and retain millisecond limits; construction touches no hardware.
 * Startup must keep both supplied objects alive for the application lifetime.
 */
AcquisitionApplication::AcquisitionApplication(Ads127LowRate& ads127LowRate,
    AcquisitionServices& acquisitionServices, uint32_t runMs, uint32_t receiverTimeoutMs)
    : _ads127LowRate(ads127LowRate), _acquisitionServices(acquisitionServices),
      _runMs(runMs), _receiverTimeoutMs(receiverTimeoutMs) {}

/** @brief Print operator commands without accessing ADC registers. */
void AcquisitionApplication::printHelp()
{
    _acquisitionServices.describe(_state == State::idle);
    Serial.printf("g=default (1024k), 0=16k, 1=32k, 2=64k, 3=128k, 4=256k, 5=512k, 6=1024k samples/s; %lu seconds each. s=stop; h=help.\n",
                  static_cast<unsigned long>(_runMs / 1000));
    Serial.printf("Matching receiver firmware must allow %lu seconds after receiver READY.\n",
                  static_cast<unsigned long>(_receiverTimeoutMs / 1000));
    Serial.println("No automatic ADC commands at boot. Confirm ADC/translator power before g.");
}

/**
 * @brief Request ADC stop, then receiver stop, retaining uncertainty if SPI echo fails.
 * A stop request is not proof of stopped hardware; the echo and subsequent
 * DONE are separate confirmations. After this function returns, poll() waits
 * at most two seconds for receiver DONE; that limit does not bound the time
 * spent making SPI calls or printing the optional input report here.
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

/**
 * @brief Configure while stopped and ask for DMA readiness using a fresh run ID.
 * The supplied configuration provides the complete settings; the application saves a copy. Requests are
 * refused unless idle, and no ADC START is issued if setup fails or no matching
 * READY arrives. A random nonzero ID links later replies to this request.
 * ARM3 does not negotiate duration: verify both firmware banners before a run.
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
        Serial1.printf("ADC127 ARM3 %lu %lu\n", _runId,
                       static_cast<unsigned long>(_adsConfig._samplesPerSecond));
        _state = State::waiting;
        _stateMs = millis();
    }
}

/**
 * @brief Forward bench status and act only on a matching readiness/completion ID.
 * A ready receiver is not evidence of arriving data. Receiver counters report that
 * separately. Unexpected READY or stale replies never start the ADC.
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
                  sscanf(line, "ADC127 FAILED %lu", &received) == 1) && received == _runId &&
                 (_state == State::waiting || _state == State::running))
        {
            Serial.println("Receiver reported a fault, setup failure or timeout; requesting ADC stop.");
            requestStop();
        }
    }
}

/**
 * @brief Leave analog outputs off, open USB/UART, and initialize borrowed services.
 * Arduino startup calls once. poll() alone owns ADC SPI and command handling.
 * A supplied service may prepare its timer or measurements, but this method
 * never starts ADC conversion or a waveform. Initialization failure blocks runs.
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
    if (!_servicesReady) { Serial.println("Signal service initialization FAILED; capture will be refused."); }
    delay(500);
    printHelp();
}

/**
 * @brief Process manual commands, UART replies and bounded run/handshake timers.
 * Serial polling is bounded, but command handlers can spend time configuring
 * hardware or printing reports. Unplugging the PC terminal does
 * not disable the automatic stop while Feather remains powered. The run timer
 * starts after ADC START echo, whereas the receiver deadline starts when its receiver becomes ready.
 */
void AcquisitionApplication::poll()
{
    if (Serial.available())
    {
        const char command = static_cast<char>(Serial.read());
        if (command == 'g' || command == 'G' || command == '6')
        {
            requestRun(adsConfig);
        }
        else if (command >= '0' && command <= '5')
        {
            // Keys choose actual rates; their numeric value is never passed
            // into the driver, recording application or serial protocol.
            const uint32_t samplesPerSecond = 16000U << (command - '0');
            const Ads127Config* selectedConfig = getAds127Config(samplesPerSecond);
            if (selectedConfig != nullptr) { requestRun(*selectedConfig); }
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
    // a source timer callback. A latched failure stops both ends of the capture.
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
    if (_state == State::running && _acquisitionServices.getSampling()) { delayMicroseconds(50); }
    else { delay(1); }
}
