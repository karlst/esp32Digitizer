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
 * @file ads127Dma.cpp
 * @brief Historical S3 I2S capture diagnostic; not a qualified 1.024-Msample/s receiver.
 * Short tests passed through 256k samples/s. At 512k and 1024k, DCLK is 32.768 MHz
 * and headers failed. I2S slave operation requires internal clock >= 8 * DCLK;
 * the selected 160-MHz source cannot supply the required 262.144 MHz. Preserve
 * this diagnostic for comparison, not as the future production capture driver.
 * Reading path: begin() installs the hardware receiver; poll() calls i2s_read()
 * and interprets completed words; drainEvents() counts driver-reported overruns;
 * end() releases buffers. No SD writer, PSRAM ring or ADC SPI code is required.
 *
 * Stage zero: DCLK=4.096 MHz, FSYNC=16 kHz: 256 clocks per sample frame.
 * Higher stages use four, two or one 32-bit time slots; only slot zero enters DMA.
 * Those slots describe TIME on DOUT0, not eight enabled ADC channels. Slot zero
 * contains ADC status (8 bits) then one signed sample (24 bits). Remaining clocks
 * are ignored. The status byte helps check frame alignment during these tests;
 * it is separate from a recording file's header.
 */
#include "ads127Dma.h"
#include "ads127Pins.h"
#include <Arduino.h>
#include <cstring>
#include <soc/i2s_struct.h>
#include "ads127BenchRate.h"

/**
 * @brief Allocate DMA buffers and listen on GPIO9/10/11; never drive ADC clocks.
 * True means installation succeeded, not that the ADC is sending valid frames.
 * stage indexes the shared ADC/receiver timing table; invalid stages are rejected.
 * Driver installation briefly starts the peripheral; stop it before routing pins
 * and applying ADC-specific alignment, then restart before reporting READY.
 */
bool ads127Dma::begin(unsigned stage)
{
    end();
    if (stage >= ads127BenchRateCount)
    {
        return false;
    }
    const ads127BenchRate& rate = ads127BenchRates[stage];
    const bool highRate = rate.samplesPerSecond >= 512000;
    readWords = highRate ? 4096 : 1024;
    words = valid = 0;
    badHeaders = overruns = errors = queueFullObservations = latestWord = 0;
    latestSample = 0;

    // Diagnostics belong to this run only. Counting adds no serial output to
    // the capture path; print the saved information after acquisition stops.
    memset(headerCounts, 0, sizeof(headerCounts));
    exampleCount = 0;
    savedConf = savedConf1 = savedTdm = savedTiming = savedClock = savedClockDivider = 0;
    i2s_config_t config = {};
    config.mode = static_cast<i2s_mode_t>(I2S_MODE_SLAVE | I2S_MODE_RX);
    config.sample_rate = rate.samplesPerSecond;
    config.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;
    config.bits_per_chan = I2S_BITS_PER_CHAN_32BIT;
    config.channel_format = I2S_CHANNEL_FMT_MULTIPLE;
    config.communication_format = I2S_COMM_FORMAT_STAND_MSB;
    config.chan_mask = I2S_TDM_ACTIVE_CH0;
    config.total_chan = rate.slots;
    config.left_align = true;
    config.big_edin = false;
    config.bit_order_msb = false; // IDF 4.4 name is misleading: hardware 0 means MSB first.
    config.dma_buf_count = 16;
    config.dma_buf_len = highRate ? 1000 : 256;
    config.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
    config.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    // Low stages retain sixteen 1024-byte blocks. High stages use sixteen
    // 4000-byte blocks (below the driver's 4092-byte descriptor limit), holding
    // about 15.6 ms at 1.024M/s. This is 64,000 bytes of internal DMA memory.
    // The 64-entry event queue contains notifications, not sample data.
    // High-rate DMA interrupts occur once per 1000 words, up to 1024 times/s.
    esp_err_t result = i2s_driver_install(I2S_NUM_0, &config, 64, &events);
    installed = result == ESP_OK;
    if (installed)
    {

        // IDF 4.4's installer does not copy config.total_chan into its working
        // configuration. With only CH0 active it derives a one-slot frame instead
        // of the selected multi-slot frame. The public clock setter supports a
        // different spelling: MONO keeps the lowest selected slot, while the
        // highest selected bit establishes frame length. DMA receives only CH0.
        // For example, CH0|CH3|MONO describes four slots with only CH0 retained.
        // Apply this before routing the input pins; set_clk briefly restarts the receiver.
        const i2s_channel_t layout = static_cast<i2s_channel_t>(
            I2S_CHANNEL_MONO | I2S_TDM_ACTIVE_CH0 | (1u << (16 + rate.slots - 1)));
        result = i2s_set_clk(I2S_NUM_0, rate.samplesPerSecond, I2S_BITS_PER_SAMPLE_32BIT, layout);
        if (result == ESP_OK)
        {
            result = i2s_stop(I2S_NUM_0);
        }
        i2s_pin_config_t pins = {};
        pins.mck_io_num = I2S_PIN_NO_CHANGE;
        pins.bck_io_num = ads127Pins::s3DataClock;
        pins.ws_io_num = ads127Pins::s3FrameSync;
        pins.data_out_num = I2S_PIN_NO_CHANGE;
        pins.data_in_num = ads127Pins::s3Data;
        if (result == ESP_OK)
        {
            result = i2s_set_pin(I2S_NUM_0, &pins);
        }
        if (result == ESP_OK)
        {

            // set_clk corrects slot count, half-frame length and WS width, but
            // does not reapply the common-mode FSYNC polarity chosen at install.
            // Restore the high-first TDM convention: ADC FSYNC rises at bit 0.
            // This is an S3/IDF4.4-specific adjustment while the receiver is stopped.
            I2S0.rx_conf.rx_ws_idle_pol = 1;

            // Restore bypass for all receiver input delays on every run. The
            // BCK_IN_DM=1 clock-delay experiment made all 4096 headers fail at
            // 512k/s, so that setting is not retained. Explicit initialization
            // prevents an earlier experiment from leaving a delay enabled.
            const uint32_t inputTiming = 0;
            I2S0.rx_timing.val = inputTiming;

            // At 1.024M/s, all 32 DCLK pulses carry one status+sample word;
            // there is no second slot. IDF's public setter clamps slot count to
            // at least two. While stopped, program the S3's N-1 hardware fields
            // directly for one 32-bit slot and two 16-clock FSYNC half-periods.
            // Active mask and DMA word size stay unchanged. The high-rate board test
            // failed header validation; this preserves that experiment. Never infer success
            // merely from writing these fields. No later set_clk call is allowed
            // before capture, since it would reinstate the two-slot layout.
            if (rate.slots == 1)
            {
                I2S0.rx_tdm_ctrl.rx_tdm_tot_chan_num = 0;
                I2S0.rx_conf1.rx_tdm_ws_width = 15;
                I2S0.rx_conf1.rx_half_sample_bits = 15;
            }

            // Check actual hardware fields, not just the requested C++ settings.
            // Register counts store N-1; each half-frame is slots*32/2 clocks.
            // Refuse READY if an SDK change leaves a different frame layout.
            const bool layoutReady = I2S0.rx_tdm_ctrl.rx_tdm_tot_chan_num == rate.slots - 1 &&
                (I2S0.rx_tdm_ctrl.val & 0xffff) == 1 &&
                I2S0.rx_conf1.rx_tdm_ws_width == rate.slots * 16 - 1 &&
                I2S0.rx_conf1.rx_half_sample_bits == rate.slots * 16 - 1 &&
                I2S0.rx_conf.rx_ws_idle_pol && I2S0.rx_timing.val == inputTiming;
            result = layoutReady ? i2s_start(I2S_NUM_0) : ESP_ERR_INVALID_STATE;
            savedConf = I2S0.rx_conf.val;
            savedConf1 = I2S0.rx_conf1.val;
            savedTdm = I2S0.rx_tdm_ctrl.val;
            savedTiming = I2S0.rx_timing.val;
            savedClock = I2S0.rx_clkm_conf.val;
            savedClockDivider = I2S0.rx_clkm_div_conf.val;
        }
    }
    const bool retVal = result == ESP_OK;
    if (!retVal)
    {
        Serial.printf("DMA install failed: %s\n", esp_err_to_name(result));
        end();
    }
    return retVal;
}

/**
 * @brief Collect driver error notifications without waiting for any new event.
 * An RX queue overrun means at least one completed buffer was discarded before
 * we read it. Counts are observed events, NOT an exact lost-sample count. If the
 * event queue fills, events can themselves be lost; report that limitation too.
 */
void ads127Dma::drainEvents()
{
    if (events)
    {
        if (uxQueueMessagesWaiting(events) >= 63)
        {
            ++queueFullObservations;
        }
        i2s_event_t event = {};
        while (xQueueReceive(events, &event, 0) == pdTRUE)
        {
            if (event.type == I2S_EVENT_RX_Q_OVF)
            {
                ++overruns;
            }
            else if (event.type == I2S_EVENT_DMA_ERROR)
            {
                ++errors;
            }
        }
    }
}

/**
 * @brief Copy a bounded batch (1024 or 4096 words) from DMA RAM and count it.
 * i2s_read() is the actual data-copy call. Its internal queue/mutex retains and
 * coordinates driver buffers; readBuffer belongs to this object and is reused
 * only after this function finishes processing it. Pass a five-ms driver wait
 * timeout so a silent ADC does not cause an indefinite wait for data. A successful
 * read returning zero bytes is not counted as an error here.
 */
void ads127Dma::poll()
{
    if (installed)
    {
        drainEvents();
        size_t bytes = 0;
        const esp_err_t result = i2s_read(I2S_NUM_0, readBuffer, readWords * sizeof(uint32_t),
                                         &bytes, pdMS_TO_TICKS(5));
        if (result != ESP_OK || bytes % sizeof(uint32_t) != 0)
        {
            ++errors;
        }
        for (size_t index = 0; index < bytes / sizeof(uint32_t); ++index)
        {
            latestWord = readBuffer[index];
            ++words;

            // On little-endian S3 the 32-bit integer retains wire significance:
            // top byte is status, low 24 bits are sample. Preserve raw words to
            // diagnose bit/byte alignment if the expected header doesn't match.
            const uint8_t header = latestWord >> 24;
            ++headerCounts[header];
            if ((header & 0x5f) == 0x08)
            {
                ++valid;
            }
            else
            {
                ++badHeaders;
                if (exampleCount < 8)
                {
                    badExamples[exampleCount] = latestWord;
                    badPositions[exampleCount] = words - 1;
                    ++exampleCount;
                }
            }
            const uint32_t raw = latestWord & 0x00ffffff;
            latestSample = raw & 0x00800000 ? static_cast<int32_t>(raw) - 0x01000000 :
                                             static_cast<int32_t>(raw);
        }
        drainEvents();
    }
}

/**
 * @brief Stop the receiver and free driver memory, keeping final counters readable.
 * Stopping DMA does not send an ADC command. Feather separately stops the ADC.
 */
void ads127Dma::end()
{
    if (installed)
    {
        i2s_driver_uninstall(I2S_NUM_0);
        installed = false;
        events = nullptr;
    }
}

/** @brief Return whether the receiver is installed, not whether samples arrived. */
bool ads127Dma::active() const
{
    const bool retVal = installed;
    return retVal;
}

/**
 * @brief Print saved timing registers, header counts and examples after STOP.
 * The bench controller calls this once per output port, after sending DONE.
 * output is USB or the Feather UART; runId associates every line with its test.
 * Each line fits the Feather's 384-byte input limit. No output is allowed while
 * capture is active: waiting for serial transmission could cause an overrun.
 * A histogram is a count per header value; omitted values occurred zero times.
 * Matching headers do not verify all sample bits, and shifts are not corrected.
 */
void ads127Dma::reportDiagnostics(Print& output, unsigned long runId) const
{
    if (!installed)
    {
        char text[240];
        snprintf(text, sizeof(text), "ADC127 REGS %lu conf=%08lX conf1=%08lX tdm=%08lX timing=%08lX clock=%08lX clockDivider=%08lX\n",
            runId, static_cast<unsigned long>(savedConf), static_cast<unsigned long>(savedConf1),
            static_cast<unsigned long>(savedTdm), static_cast<unsigned long>(savedTiming),
            static_cast<unsigned long>(savedClock), static_cast<unsigned long>(savedClockDivider));
        output.print(text);

        // Group eight possible headers per line to keep even a noisy run short.
        // Counts cover every word read, not just the eight examples below.
        for (unsigned base = 0; base < 256; base += 8)
        {
            size_t used = snprintf(text, sizeof(text), "ADC127 HEADERS %lu", runId);
            bool present = false;
            for (unsigned header = base; header < base + 8; ++header)
            {
                if (headerCounts[header])
                {
                    used += snprintf(text + used, sizeof(text) - used, " %02X=%llu", header,
                        static_cast<unsigned long long>(headerCounts[header]));
                    present = true;
                }
            }
            if (present)
            {
                output.println(text);
            }
        }

        // Position zero denotes the first word returned by i2s_read(), not a
        // known ADC sequence number. Capture does not supply sequence numbers.
        for (unsigned index = 0; index < exampleCount; ++index)
        {
            snprintf(text, sizeof(text), "ADC127 BADWORD %lu index=%llu raw=%08lX\n", runId,
                static_cast<unsigned long long>(badPositions[index]),
                static_cast<unsigned long>(badExamples[index]));
            output.print(text);
        }
        snprintf(text, sizeof(text), "ADC127 DIAGEND %lu\n", runId);
        output.print(text);
    }
}
