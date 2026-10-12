/**
 * @file ads127Dma.h
 * @brief Memory-only ADS127 capture using the S3's I2S receiver and DMA.
 */
#pragma once
#include <driver/i2s.h>
#include <cstdint>
class Print;

/**
 * @brief Own an I2S input stream, internal DMA buffers and test counters.
 * Arduino loop calls begin/poll/end; no application data is shared across tasks.
 * Espressif's driver handles buffer-completion interrupts and its own queue/lock.
 * We never run an interrupt for each sample. DMA means hardware writes received
 * bits into RAM while the CPU does other work. poll() copies completed data out.
 */
class ads127Dma
{
public:
    bool begin(unsigned stage = 0);
    void poll();
    void end();
    bool active() const;
    void reportDiagnostics(Print& output, unsigned long runId) const;
    uint64_t words = 0;
    uint64_t valid = 0;
    uint32_t badHeaders = 0, overruns = 0, errors = 0, queueFullObservations = 0;
    uint32_t latestWord = 0;
    int32_t latestSample = 0;
private:
    bool installed = false;
    QueueHandle_t events = nullptr;

    // High-rate stages read up to 4096 words, allowing several milliseconds of
    // samples per loop despite its 1-ms yield. Lower stages keep 1024-word reads
    // so filling a large batch cannot delay commands at the slower sample rates.
    size_t readWords = 1024;
    uint32_t readBuffer[4096] = {};

    // The Arduino task counts every observed header, including valid ones.
    // Keep the first eight bad words and their zero-based positions; these are
    // examples, not a complete copy of the acquisition. Reset on each begin().
    uint64_t headerCounts[256] = {};
    uint32_t badExamples[8] = {};
    uint64_t badPositions[8] = {};
    unsigned exampleCount = 0;

    // Snapshot hardware while installed. Uninstalling can reset registers, so
    // post-stop reporting must use this saved copy, not reread disabled hardware.
    uint32_t savedConf = 0, savedConf1 = 0, savedTdm = 0, savedTiming = 0;
    uint32_t savedClock = 0, savedClockDivider = 0;
    void drainEvents();
};
