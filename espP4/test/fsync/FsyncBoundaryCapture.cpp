// Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Capture two adjacent FSYNC times for one sample-block boundary. The caller
 * in PpsSampleTest supplies a recently numbered reference after receiving PPS.
 * MCPWM (the chip's motor-control PWM peripheral) is used only for its hardware
 * edge-capture timer here, not to drive a motor. Startup reserves group 0,
 * channel 0 for FSYNC. Read capture below, then FsyncBoundaryMath::resolve
 * for the separate conversion from edge times to sample numbers.
 */
#include "FsyncBoundaryCapture.h"
#include "FsyncBoundaryMath.h"
#include "hal/mcpwm_ll.h"
#include "esp_cpu.h"
#include "esp_attr.h"
#include "sdkconfig.h"

static_assert(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ == 360, "Review boundary-read limit for another CPU clock");

/* **
 * capture - Read until two consecutive FSYNC captures appear, within 32 reads.
 * referencePulse identifies the FSYNC edge saved at referenceTicks. On success,
 * boundary contains the two observed edge times and their numbered pulses.
 * Briefly exclude local interrupts so the ordinary receive interrupt cannot
 * interrupt this search. Other delays can still separate reads by several
 * sample periods, so each skipped edge starts another attempt. Hardware DMA and Core 1
 * continue; no console output, allocation or peripheral-driver lock occurs.
 *
 * Both returned timestamps came from hardware. Software elapsed time only
 * enforces a 20-us guard; it is not used as a sample timestamp. The nominal
 * sample ratio is used only to attach the pair to the prior numbered anchor.
 * Exhausted retries, an absent clock or an excessive bracket returns false.
 * readResult retains both raw timestamps and the elapsed CPU cycles on failure;
 * the caller prints these only after acquisition stops, outside this timed loop.
 * Keep this routine in internal instruction RAM: excluding interrupts does not
 * prevent stalls fetching code from flash. The register-read loop has no calls;
 * anchor arithmetic runs after interrupts are restored. RAM placement removes
 * flash instruction-fetch delays, not possible peripheral-bus delays.
 */
bool IRAM_ATTR FsyncBoundaryCapture::capture(uint32_t referencePulse, uint32_t referenceTicks,
    FsyncBoundary& boundary, ReadResult& readResult)
{
    portENTER_CRITICAL(&_mux);

    // Channel 0 holds the timer value saved by the latest FSYNC rising edge.
    // Reading it does not trigger a new measurement or reset the timer. The CPU
    // cycle counter below measures search duration separately: 7200 cycles at
    // 360 MHz is 20 microseconds. Unsigned subtraction tolerates its wraparound.
    const uint32_t began = esp_cpu_get_cycle_count();
    uint32_t before = mcpwm_ll_capture_get_value(MCPWM_LL_GET_HW(0), 0);
    uint32_t after = before;
    bool adjacent = false;
    uint32_t elapsed = esp_cpu_get_cycle_count() - began;

    // This is at most 32 additional reads after the initial register read,
    // within 20 us total, not 32 millisecond observations or startup retries.
    // A delayed read may jump four edges, for example. Keep that newer actual
    // capture and look for the edge after it instead of rejecting the PPS.
    // Both limits bound local interrupt exclusion; reject even an adjacent
    // pair if its read finished at or beyond the 20-us elapsed-time limit.
    for (unsigned attempt = 0; attempt < 32 && !adjacent && elapsed < 7200; ++attempt)
    {
        const uint32_t next = mcpwm_ll_capture_get_value(MCPWM_LL_GET_HW(0), 0);
        adjacent = FsyncBoundaryMath::advance(before, after, next);
        elapsed = esp_cpu_get_cycle_count() - began;
    }
    portEXIT_CRITICAL(&_mux);

    // Preserve the separate verdicts: a slow read and a pair that cannot be
    // numbered must not collapse into an unexplained false in the saved log.
    const bool numbered = FsyncBoundaryMath::resolve(referencePulse, referenceTicks, before, after, boundary);
    readResult = {before, after, elapsed, numbered};
    const bool retVal = adjacent && elapsed < 7200 && numbered;
    return retVal;
}
