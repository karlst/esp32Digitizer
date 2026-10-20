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
 * @file BenchAcquisitionServices.h
 * @brief Test-only source and input-observation adapter for the real ADC controller.
 */
#pragma once
#include "AcquisitionServices.h"
#include "BenchDac.h"
#include "InputSnapshot.h"
/**
 * @brief Own the selected DAC test and optional A2 measurements for one firmware image.
 * Arduino's task owns these objects; BenchDac owns its separately scheduled timer.
 * Production startup does not include or instantiate this class.
 */
class BenchAcquisitionServices : public AcquisitionServices
{
public:
    bool initialize() override;
    bool externalInput() const override;
    void describe(bool idle) override;
    void reset() override;
    bool start() override;
    void begin() override;
    void poll() override;
    bool failed() override;
    bool sampling() const override;
    void finish(unsigned long runId, bool clean) override;
private:
    bool _ready = false;
#if ADS127_P4_DAC_TEST
    BenchDac _benchDac;
#endif
#if ADS127_COMPARE_INPUT
    InputSnapshot _inputSnapshot;
#endif
};
