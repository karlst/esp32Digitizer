/** @file inputSnapshot.h
 * @brief Independent A2 observations during the P4 RAM waveform experiment.
 * Arduino loop owns all fields; the DAC timer never accesses this object.
 * The A1->A2 jumper measures the physical voltage, not the requested DAC code.
 */
#pragma once
#if ADS127_COMPARE_INPUT
#include <Arduino.h>

/** @brief Fixed-size timestamped input record, printed only after ADC/DAC stop.
 * A2 is Feather GPIO34 / ADC1 channel 6. Counts are uncalibrated 12-bit values.
 * This sensor limits comparison accuracy; it is not a reference-grade ADC.
 */
class inputSnapshot
{
public:
    bool initialize();
    void reset();
    void begin();
    void poll();
    bool sampling() const;
    bool failed() const;
    void report(unsigned long run, bool hardwareClean);
private:
    /** @brief Burst bounds relative to START echo; sum/count retain its mean. */
    struct point
    {
        uint32_t timeUs;
        uint32_t durationUs;
        uint32_t reads;
        uint32_t sum;
        int minimum;
        int maximum;
    };
    point points[320] = {};
    unsigned count = 0;
    uint32_t missed = 0, maxGapUs = 0;
    int64_t origin = 0, nextRead = 0, lastRead = 0;
    bool collecting = false, broken = false;
};
#endif
