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

/** @file BenchAcquisitionServices.cpp
 * @brief Connect test-only signal generation and observation to acquisition lifecycle calls.
 */
#include <Arduino.h>
#include "BenchAcquisitionServices.h"
/** @brief Prepare stopped source and measurement hardware; report combined readiness. */
bool BenchAcquisitionServices::initialize()
{
    _ready = true;
#if ADS127_P4_DAC_TEST
    _ready = _benchDac.initialize();
#endif
#if ADS127_COMPARE_INPUT
    _ready = _inputSnapshot.initialize() && _ready;
#endif
    return _ready;
}
/** @brief DAC tests use the external input; internal-short tests leave it disconnected. */
bool BenchAcquisitionServices::externalInput() const
{
#if ADS127_P4_DAC_TEST
    return true;
#else
    return false;
#endif
}
/** @brief Print the selected test identity and voltage pattern without starting it. */
void BenchAcquisitionServices::describe(bool idle)
{
    (void)idle;
#if ADS127_HARDWARE_SMOKE
    Serial.printf("SMOKE_FEATHER version=1 ready=%u seconds=30\n",
                  unsigned(_ready && idle));
#endif
#if ADS127_COMPARE_INPUT && !ADS127_HARDWARE_SMOKE
    Serial.println(_ready ? "COMPARE READY: simultaneous A1->A2 input and P4 RAM output; keep coax connected."
                            : "COMPARE UNAVAILABLE: DAC/input ADC initialization failed.");
#endif
#if ADS127_P4_DAC_TEST
    Serial.println("ADS127 DAC TEST: external CH0; A1/GPIO25 -> CH0P, CH0N grounded; SD controlled by P4.");
#if ADS127_HARDWARE_SMOKE
    Serial.println("DAC MODE: SQUARE codes=110,135 levelSeconds=1");
#elif ADS127_DAC_CONSTANT
    Serial.println("DAC MODE: CONSTANT code=135 nominalVolts=1.747; 500 us repeated writes.");
#else
    Serial.println("DAC MODE: SINE");
    Serial.println("DAC: nominal 10 Hz sine, 1.25 V offset, 0.5 V peak (0.75..1.75 V); 500 us updates.");
#endif
    Serial.println("DAC stays OFF until receiver READY; verify powered analog wiring before starting.");
#else
    Serial.println("ADS127 DMA CONTROL: DAC off; coax stays disconnected; SD untouched.");
#endif
}
/** @brief Discard observations from the previous run before ARM is sent. */
void BenchAcquisitionServices::reset()
{
#if ADS127_COMPARE_INPUT
    _inputSnapshot.reset();
#endif
}
/** @brief Enable the source only after P4's matching READY reply. */
bool BenchAcquisitionServices::start()
{
    bool retVal = true;
#if ADS127_P4_DAC_TEST
    retVal = _benchDac.start();
#endif
    return retVal;
}
/** @brief Start the input-observation clock after ADC START is acknowledged. */
void BenchAcquisitionServices::begin()
{
#if ADS127_COMPARE_INPUT
    _inputSnapshot.begin();
#endif
}
/** @brief Collect pending input observations on Arduino's task without printing. */
void BenchAcquisitionServices::poll()
{
#if ADS127_COMPARE_INPUT
    _inputSnapshot.poll();
#endif
}
/** @brief Combine latched source and observation faults for coordinated shutdown. */
bool BenchAcquisitionServices::failed()
{
    bool retVal = false;
#if ADS127_P4_DAC_TEST
    retVal = _benchDac.failed();
#endif
#if ADS127_COMPARE_INPUT
    retVal = retVal || _inputSnapshot.failed();
#endif
    return retVal;
}
/** @brief Request fast polling only while the local ADC is measuring a window. */
bool BenchAcquisitionServices::sampling() const
{
    bool retVal = false;
#if ADS127_COMPARE_INPUT
    retVal = _inputSnapshot.sampling();
#endif
    return retVal;
}
/** @brief Disable the source and export retained observations after ADC stop request. */
void BenchAcquisitionServices::finish(unsigned long runId, bool clean)
{
    (void)runId;
    (void)clean;
#if ADS127_P4_DAC_TEST
    _benchDac.stop();
#endif
#if ADS127_COMPARE_INPUT
    _inputSnapshot.report(runId, clean && !_benchDac.failed());
#endif
}
