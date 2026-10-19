/** @file ramWaveform.cpp
 * @brief Retain waveform shape without disk I/O or printing during capture.
 */
#include "ramWaveform.h"

/** @brief Core 0 resets while decoder is idle; supported rates divide by 2000.
 * One second of decoded samples is skipped for analog startup settling.
 * Queue handoff subsequently gives Core 1 access to the initialized state.
 */
void ramWaveform::reset(uint32_t rate)
{
    ready.store(false);
    skip = rate;
    samplesPerBin = rate / 2000;
    inBin = completedBins = 0;
    for (auto& bin : bins) { bin = waveformBin{}; }
}

/** @brief Core 1 receives decoder batches; strip status and sign-extend bit 23.
 * The status byte is still checked by frameDecoder. Once the requested window
 * is full, discard later samples while Core 0 coordinates a normal ADC stop.
 * No pointer to the decoder's reusable batch is retained.
 */
void ramWaveform::accept(const uint32_t* words, size_t count)
{
    for (size_t index = 0; index < count && completedBins < binCount; ++index)
    {
        if (skip != 0) { --skip; }
        else if (samplesPerBin != 0)
        {
            const uint32_t raw = words[index] & 0xffffffU;
            const int32_t value = static_cast<int32_t>(raw) -
                ((raw & 0x800000U) ? 0x1000000 : 0);
            auto& bin = bins[completedBins];
            if (value < bin.minimum) { bin.minimum = value; }
            if (value > bin.maximum) { bin.maximum = value; }
            bin.sum += value;
            if (++inBin == samplesPerBin)
            {
                inBin = 0;
                ++completedBins;
                if (completedBins == binCount) { ready.store(true); }
            }
        }
    }
}

/** @brief Core 0 polls only this atomic flag until the decoder has drained. */
bool ramWaveform::complete() const
{
    return ready.load();
}
