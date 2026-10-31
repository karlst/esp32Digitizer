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
 * @file BenchAcquisitionServices.cpp
 * Connect test-only signal generation and observation to acquisition lifecycle calls.
 */
#include <Arduino.h>
#if ADS127_EXTERNAL_GENERATOR
#include <driver/dac.h>
#endif
#include "BenchAcquisitionServices.h"

/* **
 * initialize - Prepare stopped source and measurement hardware; report combined
 * readiness.
 */
bool BenchAcquisitionServices::initialize()
{
    _ready = true;
#if ADS127_EXTERNAL_GENERATOR
    // Disable A1 explicitly. The Rigol supplies voltage independently; this
    // firmware never enables the Feather DAC or samples Feather A2.
    _ready = dac_output_disable(DAC_CHANNEL_1) == ESP_OK;
#endif
#if ADS127_P4_DAC_TEST
    _ready = _benchDac.initialize();
#endif
#if ADS127_COMPARE_INPUT
    _ready = _inputSnapshot.initialize() && _ready;
#endif
    return _ready;
}

/* **
 * configureDuration - Configure stopped smoke observations; other bench builds
 * refuse the extension.
 */
bool BenchAcquisitionServices::configureDuration(unsigned seconds)
{
    bool retVal = false;
#if ADS127_HARDWARE_SMOKE
    retVal = seconds == 30 || seconds == 300 || seconds == 600;
    if (retVal)
    {
        _seconds = seconds;
#if ADS127_COMPARE_INPUT
        _inputSnapshot.setDuration(seconds);
#endif
    }
#else
    (void)seconds;
#endif
    return retVal;
}

/* **
 * getExternalInput - Both Feather DAC and Rigol tests use CH0P/CH0N. Internal-
 * short tests leave that external input disconnected inside the ADC.
 */
bool BenchAcquisitionServices::getExternalInput() const
{
#if ADS127_P4_DAC_TEST || ADS127_EXTERNAL_GENERATOR
    return true;
#else
    return false;
#endif
}

/* **
 * describe - Print the selected test identity and voltage pattern without
 * starting it.
 */
void BenchAcquisitionServices::describe(bool idle)
{
    (void)idle;
#if ADS127_EXTERNAL_GENERATOR
    Serial.printf("SMOKE_FEATHER version=6 ready=%u seconds=%u\n",
        unsigned(_ready && idle), _seconds);
    Serial.println("SOURCE: EXTERNAL generator; Feather DAC disabled; no A2 measurements.");
#elif ADS127_HARDWARE_SMOKE
    Serial.printf("SMOKE_FEATHER version=4 ready=%u seconds=%u\n",
        unsigned(_ready && idle), _seconds);
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
#elif !ADS127_EXTERNAL_GENERATOR
    Serial.println("ADS127 DMA CONTROL: DAC off; coax stays disconnected; SD untouched.");
#endif
}

/* **
 * reset - Discard observations from the previous run before ARM is sent.
 */
void BenchAcquisitionServices::reset()
{
#if ADS127_COMPARE_INPUT
    _inputSnapshot.reset();
#endif
}

/* **
 * start - Enable the Feather DAC only after P4's matching READY reply. In the
 * Rigol build this succeeds without changing the independently running source.
 */
bool BenchAcquisitionServices::start()
{
    bool retVal = true;
#if ADS127_P4_DAC_TEST
    retVal = _benchDac.start();
#endif
    return retVal;
}

/* **
 * begin - Start the input-observation clock after ADC START is acknowledged.
 */
void BenchAcquisitionServices::begin()
{
#if ADS127_COMPARE_INPUT
    _inputSnapshot.begin();
#endif
}

/* **
 * poll - Collect pending input observations on Arduino's task without printing.
 */
void BenchAcquisitionServices::poll()
{
#if ADS127_COMPARE_INPUT
    _inputSnapshot.poll();
#endif
}

/* **
 * getFailed - Combine latched source and observation faults for coordinated
 * shutdown.
 */
bool BenchAcquisitionServices::getFailed()
{
    bool retVal = false;
#if ADS127_P4_DAC_TEST
    retVal = _benchDac.getFailed();
#endif
#if ADS127_COMPARE_INPUT
    retVal = retVal || _inputSnapshot.getFailed();
#endif
    return retVal;
}

/* **
 * getSampling - Request fast polling only while the local ADC is measuring a
 * window.
 */
bool BenchAcquisitionServices::getSampling() const
{
    bool retVal = false;
#if ADS127_COMPARE_INPUT
    retVal = _inputSnapshot.getSampling();
#endif
    return retVal;
}

/* **
 * finish - Disable the Feather DAC and export observations after ADC stop.
 * The Rigol build reports controller completion without touching the generator.
 */
void BenchAcquisitionServices::finish(unsigned long runId, bool clean)
{
    (void)runId;
    (void)clean;
#if ADS127_EXTERNAL_GENERATOR
    // Report controller completion, not generator shutdown. Karl switches the
    // independent generator off after the test, before removing board power.
    Serial.printf("EXTERNAL_END run=%lu clean=%u\n", runId, unsigned(clean));
#endif
#if ADS127_P4_DAC_TEST
    _benchDac.stop();
#endif
#if ADS127_COMPARE_INPUT
    _inputSnapshot.report(runId, clean && !_benchDac.getFailed());
#endif
}
