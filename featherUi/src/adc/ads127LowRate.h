/**
 * @file ads127LowRate.h
 * @brief Selectable ADC bench configurations, independent of capture success.
 */
#pragma once
#include "ads127l18.h"

/**
 * @brief Configure channel zero's internal short test and control conversions.
 * Called only by the Feather diagnostic's Arduino task. The ADC driver owns
 * SPI transfers; this class owns their sequence and checks configuration readback.
 * No output starts at construction or configuration time. A separate start()
 * follows the S3 READY reply so the receiver exists before samples are produced.
 */
class ads127LowRate
{
public:
    explicit ads127LowRate(ads127l18& adc);
    bool configure(unsigned stage = 0);
    bool start();
    bool stop();
    bool reportStatus(const char* phase);
private:
    ads127l18& adc;
    bool configured = false;
    bool setAndCheck(uint8_t address, uint8_t value);
    bool readStatus(const char* phase, uint8_t& status);
    bool clearStartupFlags();
};
