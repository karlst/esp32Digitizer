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
 * @file AcquisitionApplication.h
 * @brief Feather ADC command coordination, independent of the selected test harness.
 */
#pragma once
#include <Arduino.h>
#include "adc/Ads127LowRate.h"
#include "AcquisitionServices.h"
#include "BenchLine.h"
/**
 * @brief Configure the ADC, wait for P4 readiness, then run until stop or timeout.
 * Arduino startup calls initialize once and poll repeatedly on the same task.
 * This object borrows the configuration driver and optional services; it owns
 * command parsing and run state. USB commands select a sampling rate, while
 * Serial1 carries matching run IDs between boards. No capture begins at boot.
 * Read requestRun, acceptReply and requestStop for the handshake sequence.
 */
class AcquisitionApplication
{
public:
    AcquisitionApplication(Ads127LowRate& ads127LowRate, AcquisitionServices& acquisitionServices,
                           uint32_t runMs, uint32_t receiverTimeoutMs);
    void initialize();
    void poll();
private:
    void printHelp();
    void requestRun(unsigned requestedStage);
    void requestStop();
    void acceptReply(const char* line);
    enum class State { idle, waiting, running, stopping };
    Ads127LowRate& _ads127LowRate;
    AcquisitionServices& _acquisitionServices;
    const uint32_t _runMs;
    const uint32_t _receiverTimeoutMs;
    BenchLine _benchLine;
    State _state = State::idle;
    unsigned long _runId = 0;
    unsigned _stage = 0;
    uint32_t _stateMs = 0;
    bool _servicesReady = false;
};
