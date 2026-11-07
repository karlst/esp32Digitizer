/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Dedicated P4 input timing comparison. No acquisition or storage application.
 */
#include "InputSkewTest.h"
#include "freertos/task.h"

static InputSkewTest inputSkewTest;

/* **
 * app_main - Start only the calibration service on Core 0. Wait for the PC to
 * request each capture; the one-scheduler-tick delay gives other tasks CPU time.
 * Feather and ADC are never commanded by this image.
 */
extern "C" void app_main()
{
    inputSkewTest.initialize();
    for (;;)
    {
        inputSkewTest.poll();
        vTaskDelay(1);
    }
}
