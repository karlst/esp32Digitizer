/**
 * @file fastCaptureChecks.cpp
 * @brief Exercise the real interrupt reader with simulated SPI and GPIO registers.
 * These checks cover decoding, deadlines and rejection; bench tests establish speed.
 */
#include "fastCapture.h"
#include <soc/spi_struct.h>
#include <soc/gpio_struct.h>
#include <cassert>
#include <iostream>

uint32_t testMs = 0;
uint32_t testUs = 0;
int testReady = LOW;
int testChipSelect = LOW;
static uint32_t reply = 0;
static uint32_t transferStart = 0;
static bool stuckController = false;
static bool stuckReady = false;
static bool overlap = false;

/**
 * @brief Advance a synthetic controller while the production code polls its registers.
 * Model a 13 us transfer, a second ready edge when requested, and W1TC semantics.
 */
static void advanceHardware()
{
    ++testUs;
    GPIO.status &= ~GPIO.status_w1tc;
    GPIO.status_w1tc = 0;
    if (!stuckController)
    {
        GPSPI2.cmd.update = 0;
        if (GPSPI2.cmd.usr)
        {
            if (!transferStart) { transferStart = testUs; }
            if (testUs - transferStart >= 13)
            {
                GPSPI2.cmd.usr = 0;
                GPSPI2.data_buf[0] = reply;
                GPIO.in = stuckReady ? 0 : 1U << 9;
                GPIO.status = overlap ? 1U << 9 : 0;
            }
        }
    }
}

/**
 * @brief Start one synthetic ready event with chosen incoming bytes and fault modes.
 */
static void prepare(uint32_t bytes, bool controller = false, bool ready = false, bool edge = false)
{
    GPSPI2 = {};
    GPIO = {};
    GPIO.status = 1U << 9;
    testUs = 100;
    transferStart = 0;
    reply = bytes;
    stuckController = controller;
    stuckReady = ready;
    overlap = edge;
    testClockHook = advanceHardware;
}

/**
 * @brief Check signed byte order, rejected overlaps, bounded waits and frozen faults.
 */
int main()
{
    fastCapture reader;
    const uint32_t bytes[] = {0x563412, 0xffff7f, 0x000080, 0xffffff};
    const int32_t expected[] = {0x123456, 8388607, -8388608, -1};
    for (unsigned index = 0; index < 4; ++index)
    {
        prepare(bytes[index]);
        reader.reset();
        reader.onReady();
        const auto result = reader.snapshot();
        assert(result.count == 1 && result.raw == expected[index] && result.fault == 0);
        assert(GPSPI2.ms_dlen.ms_data_bitlen == 23 && result.readUs < 25);
    }

    // A second edge makes even a plausible completed sample untrustworthy.
    prepare(0x563412, false, false, true);
    reader.reset();
    reader.onReady();
    assert(reader.snapshot().count == 0 && reader.snapshot().fault == 2);
    reader.onReady();
    assert(reader.snapshot().events == 1);

    // A stuck DRDY and a hung SPI controller produce distinct bounded failures.
    prepare(0, false, true);
    reader.reset();
    reader.onReady();
    assert(reader.snapshot().fault == 1 && reader.snapshot().stage == 3);
    prepare(0, true);
    reader.reset();
    reader.onReady();
    assert(reader.snapshot().fault == 1 && reader.snapshot().stage == 4);
    assert(reader.snapshot().count == 0 && reader.snapshot().readUs <= 27);
    std::cout << "PASS: interrupt byte order/sign, overlap rejection, DRDY failure, controller deadline and fault freeze.\n";
    return 0;
}
