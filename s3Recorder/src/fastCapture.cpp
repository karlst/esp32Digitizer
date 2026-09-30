/**
 * @file fastCapture.cpp
 * @brief Read 24 hardware-clocked bits immediately on Data Ready, without DMA.
 *
 * Arduino FSPI on this ESP32-S3 is GPSPI2. ads1256::start() has already selected
 * mode 1, MSB first, 1.9 MHz, manual CS low, and continuous-read mode on the ADC.
 * These registers are the same ones Arduino's SPI transfer function uses, but
 * this path is fixed to three bytes and placed in instruction RAM (IRAM) so it
 * does not wait for flash code to load. It never changes the clock or pin routing.
 */
#include "fastCapture.h"
#include <soc/spi_struct.h>
#include <soc/gpio_struct.h>
#include <esp_timer.h>

/**
 * @brief Read a wrapping microsecond clock without calling flash-resident code.
 * This framework's Arduino micros() wrapper lives in flash; esp_timer_get_time()
 * itself lives in instruction RAM. Unsigned subtraction handles 32-bit wrap.
 */
static uint32_t IRAM_ATTR fastMicros()
{
    const uint32_t retVal = static_cast<uint32_t>(esp_timer_get_time());
    return retVal;
}

/**
 * @brief Clear this run's counters before attaching the Data Ready interrupt.
 * The caller must detach any previous interrupt first; no ISR may be active.
 */
void fastCapture::reset()
{
    state = {};
}

/**
 * @brief Read one sample immediately when S3 GPIO9 signals Data Ready.
 *
 * No task wake-up is needed. Hardware still generates every SPI clock pulse;
 * the CPU waits only for this short transfer. All hardware waits share a 25 us
 * deadline so a controller fault cannot trap the CPU indefinitely in an ISR.
 *
 * Clear the current GPIO event before starting. If another falling edge arrives
 * during the transfer, the hardware relatches it even while this ISR is running.
 * Reject that overlap, or a DRDY pin that fails to rise, just as the task path
 * does. A fault freezes capture; the task reports it and puts the ADC in standby.
 */
void IRAM_ATTR fastCapture::onReady()
{
    if (state.fault == 0)
    {
        constexpr uint32_t readyMask = 1U << 9;
        const uint32_t started = fastMicros();
        ++state.events;
        if (state.previousEventUs && started - state.previousEventUs > state.maxGapUs)
        {
            state.maxGapUs = started - state.previousEventUs;
        }
        state.previousEventUs = started;
        // The ESP GPIO dispatcher normally clears events after the callback. We
        // clear ours early so a second event can be detected during this read.
        GPIO.status_w1tc = readyMask;

        // The ADC must be ready, and no earlier hardware transfer may be active.
        bool complete = (GPIO.in & readyMask) == 0 && GPSPI2.cmd.usr == 0;
        uint32_t stage = GPSPI2.cmd.usr ? 4 : (complete ? 0 : 2);
        if (complete)
        {
            // Length registers count bits minus one. Zeros on DIN supply clocks
            // without accidentally issuing stop/reset commands to the digitizer.
            GPSPI2.ms_dlen.ms_data_bitlen = 23;
            GPSPI2.data_buf[0] = 0;
            GPSPI2.cmd.update = 1;
            while (GPSPI2.cmd.update && fastMicros() - started < 25)
            {
                // Hardware latches the prepared transfer settings.
            }
            if (GPSPI2.cmd.update == 0)
            {
                GPSPI2.cmd.usr = 1;
                while (GPSPI2.cmd.usr && fastMicros() - started < 25)
                {
                    // Hardware clocks the three bytes; no operating-system wait.
                }
            }
            complete = GPSPI2.cmd.update == 0 && GPSPI2.cmd.usr == 0 && fastMicros() - started < 25;
            stage = complete ? 0 : 4;
            if (complete)
            {
                // The FIFO stores received bytes in little-endian memory order,
                // while the ADC sends its most-significant byte first.
                const uint32_t word = GPSPI2.data_buf[0];
                const uint32_t raw = ((word & 0xff) << 16) | (word & 0xff00) | ((word >> 16) & 0xff);
                const int32_t sample = (raw & 0x800000) ? static_cast<int32_t>(raw) - 0x1000000 :
                    static_cast<int32_t>(raw);
                // Allow pin propagation after the last SPI clock, bounded by both
                // three microseconds and the overall 25-microsecond deadline.
                const uint32_t finished = fastMicros();
                while ((GPIO.in & readyMask) == 0 && fastMicros() - finished < 3 && fastMicros() - started < 25)
                {
                    // Wait only for DRDY to rise; this is not a new-sample wait.
                }
                complete = (GPIO.in & readyMask) != 0;
                stage = complete ? 0 : 3;
                const bool overlapped = (GPIO.status & readyMask) != 0;
                state.fault = (complete ? 0U : 1U) | (overlapped ? 2U : 0U);
                if (state.fault == 0)
                {
                    ++state.count;
                    state.raw = sample;
                    state.sampleUs = started;
                    // Preserve EACH accepted value before the next interrupt. The
                    // formatter only copies four bytes; disk I/O runs on core 1.
                    if (destination)
                    {
                        sampleFormatter::submit(*destination, sample);
                    }
                }
            }
        }
        if (!complete)
        {
            state.fault |= 1;
        }
        state.stage = stage;
        state.readUs = fastMicros() - started;
        if (state.readUs > state.maxReadUs)
        {
            state.maxReadUs = state.readUs;
        }
    }
}

/**
 * @brief Copy capture results from the acquisition task on the same CPU core.
 *
 * Mask interrupts just for the copy so count/raw/timestamp belong together. The
 * interrupt writer cannot run during this copy; nothing formats or transmits
 * while the lock is held. Other cores must read acquisition::snapshot() instead.
 */
fastCapture::captureSnapshot fastCapture::snapshot()
{
    portENTER_CRITICAL(&copyLock);
    const captureSnapshot retVal = state;
    portEXIT_CRITICAL(&copyLock);
    return retVal;
}

/**
 * @brief Attach/detach recording while the GPIO interrupt is disabled.
 * A null destination keeps ordinary monitoring without writing any samples.
 */
void fastCapture::setWriter(bufferedWriter* writer)
{
    destination = writer;
}
