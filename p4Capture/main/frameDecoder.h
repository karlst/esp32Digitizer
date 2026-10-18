/**
 * @file frameDecoder.h
 * @brief Decode ADC data and its frame marker from paired PARLIO input bits.
 * Read feed() first, then beginFrame(), appendBits() and finishWord(). No hardware or RTOS
 * dependency: the same decoder is exercised by the desktop test program.
 */
#pragma once
#include <cstddef>
#include <cstdint>

/** @brief Observations of received data, not a claim that every ADC bit arrived. */
struct captureStats
{
    uint64_t clocks = 0;
    uint32_t frames = 0;
    uint32_t words = 0;
    uint32_t badHeaders = 0;
    uint32_t badPeriods = 0;
    uint32_t shortWords = 0;
    uint32_t latestWord = 0;
    int32_t latestSample = 0;
    uint32_t headers[256] = {};
    uint32_t badExamples[8] = {};
};

/**
 * @brief Worker-owned state spanning DMA blocks, including split ADC words.
 * A sample is one ADC conversion result. FSYNC (frame synchronization) rises
 * once per sample; DCLK provides one clock per serial bit. Each packed pair
 * contains DOUT0 in bit 0 and FSYNC in bit 1, earliest pair in byte bits 1:0.
 * Only the first 32 DCLKs of a frame form our 8-bit header + 24-bit sample.
 * Idle clocks in the rest of a frame are deliberately ignored.
 * An optional output callback receives copies of complete raw words in batches
 * of at most 512. A rejected batch latches failure but does not hide diagnostic
 * counts. reset clears that latch while preserving the selected callback.
 */
class frameDecoder
{
public:
    using outputCallback = bool (*)(const uint32_t*, size_t, void*);
    void setOutput(outputCallback callback, void* context);
    bool outputFailed() const;
    void reset(uint32_t clocksPerFrame);
    void feed(const uint8_t* data, size_t length);
    const captureStats& stats() const;
    uint32_t pendingBits() const;
private:
    void flushOutput();
    outputCallback output = nullptr;
    void* outputContext = nullptr;
    uint32_t outputWords[512] = {};
    size_t outputCount = 0;
    bool outputBroken = false;
    void beginFrame(uint64_t position);
    void appendBits(uint32_t value, unsigned count);
    void clockBit(unsigned pair);
    void finishWord();
    captureStats counts = {};
    uint64_t lastRise = 0;
    uint32_t frameClocks = 0;
    uint32_t word = 0;
    uint32_t bits = 0;
    bool previousHigh = true;
    bool synchronized = false;
};
