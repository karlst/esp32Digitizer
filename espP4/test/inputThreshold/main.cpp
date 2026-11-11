/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Dedicated GPIO threshold firmware. Does not construct the recording system.
 */
#include "InputThresholdTest.h"
#include "freertos/task.h"

static InputThresholdTest inputThresholdTest;

/* **
 * app_main - ESP-IDF calls this startup function on Core 0. Start capture, then
 * audit the marker from this task (a scheduled thread). Sleep one scheduler
 * tick between audits so idle tasks and the watchdog get CPU time. Capture
 * interrupts continue during the sleep. No console start command is required.
 */
extern "C" void app_main()
{
    inputThresholdTest.initialize();
    for (;;)
    {
        inputThresholdTest.poll();
        vTaskDelay(1);
    }
}
