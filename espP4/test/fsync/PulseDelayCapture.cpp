/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Measure the analog path by giving the same generator transition to ADC CH0
 * through a divider and directly to P4 GPIO6. FSYNC (frame synchronization)
 * tells us when each ADC output frame appears; its timestamps come from
 * FsyncAssociationTest. Both inputs share one hardware timer.
 *
 * Reading path: initialize attaches GPIO6; start clears and arms the event
 * array; captured saves each hardware timestamp; stop freezes that array;
 * report pairs the saved edges with nearby ADC words. pulseDelayReport.py on
 * the PC converts those words to voltage and estimates the response delay.
 * No SD operations occur. Core 0 manages the test; an interrupt writes events
 * under a lock. Reports are printed after acquisition and decoding have stopped.
 */
#include "PulseDelayCapture.h"
#include <cmath>
#include <cstdio>

/* **
 * initialize - FsyncAssociationTest supplies its timer and calls this once.
 * Allocate the second channel (index 1); FSYNC already occupies index 0.
 * GPIO6 is an input; its weak pull-down prevents an unplugged lead floating.
 * Both edges are recorded. The caller must supply a safe 3.3-V logic signal.
 */
void PulseDelayCapture::initialize(mcpwm_cap_timer_handle_t timer)
{
    mcpwm_capture_channel_config_t config = {};
    config.gpio_num = 6;

    // Capture every edge in both directions: rising is LOW to HIGH, falling
    // is HIGH to LOW. The weak pull-down gives a disconnected input a low bias.
    config.prescale = 1;
    config.flags.pos_edge = true;
    config.flags.neg_edge = true;
    config.flags.pull_down = true;
    ESP_ERROR_CHECK(mcpwm_new_capture_channel(timer, &config, &_channel));
    mcpwm_capture_event_callbacks_t callbacks = {};
    callbacks.on_cap = captured;
    ESP_ERROR_CHECK(mcpwm_capture_channel_register_event_callbacks(_channel, &callbacks, this));
}

/* **
 * start - Core 0 clears the previous run before enabling the input interrupt.
 * The timer is already running; it is shared with FSYNC and is never reset here.
 */
void PulseDelayCapture::start()
{
    portENTER_CRITICAL(&_mux);
    _count = 0;
    _overflow = false;
    _armed = true;
    portEXIT_CRITICAL(&_mux);
    ESP_ERROR_CHECK(mcpwm_capture_channel_enable(_channel));
}

/* **
 * captured - The driver calls this Interrupt Service Routine (ISR) after a
 * voltage transition. context is the object registered in initialize; channel
 * is unused because this object has only one input channel.
 * cap_value is the time saved by hardware at the edge, not the current CPU time.
 * No allocation or printing occurs here. The lock prevents start/stop from
 * resetting or reading an event halfway through a callback. _count is the next
 * free slot; an exhausted array sets overflow and rejects the whole run.
 * False means no waiting task needs to be awakened by this callback.
 */
bool PulseDelayCapture::captured(mcpwm_cap_channel_handle_t channel,
    const mcpwm_capture_event_data_t* event, void* context)
{
    (void)channel;
    auto& pulseDelayCapture = *static_cast<PulseDelayCapture*>(context);
    portENTER_CRITICAL_ISR(&pulseDelayCapture._mux);
    if (pulseDelayCapture._armed)
    {
        if (pulseDelayCapture._count < _eventCapacity)
        {
            pulseDelayCapture._events[pulseDelayCapture._count++] =
                {event->cap_value, event->cap_edge == MCPWM_CAP_EDGE_POS};
        }
        else
        {
            pulseDelayCapture._overflow = true;
        }
    }
    portEXIT_CRITICAL_ISR(&pulseDelayCapture._mux);
    return false;
}

/* **
 * stop - Core 0 prevents further event writes before reading the array. The
 * lock also excludes a pending callback; disabling the channel leaves GPIO6
 * as an input. The caller subsequently stops the shared timer.
 */
void PulseDelayCapture::stop()
{
    portENTER_CRITICAL(&_mux);
    const bool wasArmed = _armed;
    _armed = false;
    portEXIT_CRITICAL(&_mux);
    if (wasArmed)
    {
        ESP_ERROR_CHECK(mcpwm_capture_channel_disable(_channel));
    }
}

/* **
 * report - After stop and decoder drain, export forty consecutive raw ADC words
 * around each direct edge bracketed by saved FSYNC observations. The PC computes
 * voltage transition delay from these individual samples. Start twelve samples
 * before the edge; the remaining samples include response and settled plateau.
 * Data are printed only now, so USB transfer cannot delay ADC acquisition.
 * Export at most the first 128 eligible consecutive edges to bound serial
 * transfer time at 10 kHz. Captured includes events outside that export range.
 * READY means data were exported, not that delay or accuracy passed a limit.
 * run labels the transcript; associated is the caller's complete FSYNC verdict.
 * words/count describe retained ADC memory. anchors/result select the accepted
 * timing observations. No pointer is saved here; all remain owned by the caller.
 */
void PulseDelayCapture::report(uint32_t run, bool associated, const uint32_t* words,
    uint32_t count, const FsyncAssociationAnalysis::Anchor* anchors,
    const FsyncAssociationAnalysis::Result& result)
{
    size_t exported = 0;
    if (associated && !_overflow && result._passed)
    {
        // firstMatched/matched select only FSYNC observations whose ADC words
        // remain in RAM. Subtracting unsigned 32-bit timestamps handles a timer
        // wrap; an edge before first then has a large elapsed value and fails
        // the span check. Runs are much shorter than the timer's wrap period.
        const auto& first = anchors[result._firstMatched];
        const auto& last = anchors[result._firstMatched + result._matched - 1];
        const uint32_t span = last._ticks - first._ticks;
        for (size_t iEvent = 0; iEvent < _count; ++iEvent)
        {
            const auto& event = _events[iEvent];
            const uint32_t elapsed = event._ticks - first._ticks;

            // Pulse 2 is stored word 0 because pulse 1 was discarded at startup.
            // Convert elapsed ticks to a fractional word position. For example,
            // position=100.4 means the edge occurred between words 100 and 101;
            // floor(position)-12 starts the exported window at word 88.
            const double position = double(first._pulseCount - 2) +
                double(elapsed) / result._ticksPerSample;
            const int64_t start = static_cast<int64_t>(std::floor(position)) - 12;
            if (exported < 128 && elapsed <= span && start >= 0 && start + 40 <= count)
            {
                // Forty uint32_t words become 320 hex characters. Their upper
                // byte is ADC status; the lower 24 bits are the signed reading.
                // start is a word offset, not a byte offset. The event index
                // lets the PC reject missing internal edges in the transcript.
                printf("DELAY EDGE run=%lu index=%u ticks=%lu rising=%u start=%lu words=",
                    (unsigned long)run, unsigned(iEvent), (unsigned long)event._ticks,
                    unsigned(event._rising), (unsigned long)start);
                for (uint32_t iWord = 0; iWord < 40; ++iWord)
                {
                    printf("%08lx", (unsigned long)words[start + iWord]);
                }
                printf("\n");
                ++exported;
            }
        }
    }
    printf("DELAY RESULT run=%lu status=%s captured=%u exported=%u overflow=%u\n",
        (unsigned long)run, associated && !_overflow && exported >= 20 ? "READY" : "FAIL",
        unsigned(_count), unsigned(exported), unsigned(_overflow));
}
