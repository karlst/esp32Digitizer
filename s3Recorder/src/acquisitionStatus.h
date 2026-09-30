/**
 * @file acquisitionStatus.h
 * @brief Values copied from acquisition to communications and eventually Feather.
 * This structure contains numbers/flags only, not a buffer of recorded samples.
 */
#pragma once
#include <cstdint>

/**
 * @brief Keep actual device state, cumulative counts, and the latest command result.
 * The acquisition task updates its private instance; publish()/snapshot() copy it
 * under a short lock. Communications must use that copy, not read a live instance
 * while the other core updates it. Defaults describe stopped, uninitialized hardware.
 */
struct acquisitionStatus
{
    // ready: initialization succeeded and no blocking fault is outstanding.
    // running: software is collecting; hasSample: latestRaw has a real value.
    // reboot: communications should acknowledge first, then restart the S3.
    bool ready = false;
    bool running = false;
    bool hasSample = false;
    bool reboot = false;
    // Requested/applied production rate versus measured successful reads/second.
    // These differ when software falls behind; do not substitute one for the other.
    uint32_t rate = 1000;
    uint32_t measuredRate = 0;
    // Accepted reads since reboot. latestRaw is a signed ADC count, not volts;
    // lastSampleMs is its timestamp on the S3 clock, used to calculate freshness.
    uint64_t sampleCount = 0;
    int32_t latestRaw = 0;
    uint32_t lastSampleMs = 0;
    // Error: 0 none, 1 initialization, 2 Data Ready timeout, 3 setup/read failure.
    // ackId pairs a reply to a request; ackResult: 0 no reply yet, 1 accepted, 2 rejected.
    uint32_t error = 0;
    uint32_t ackId = 0;
    uint32_t ackResult = 0;
    // Extra observed high-to-low Data Ready events before a read. Events can
    // themselves be missed, so this is not an exact count of all samples lost.
    // This counter belongs to the task reader. At 30k the ISR reads immediately;
    // overlapping reads are rejected, but unseen/coalesced edges are not counted.
    uint64_t missedEdges = 0;
    // Totals since S3 boot; Stop/Start preserves them. A rejected read is counted
    // once even if both reasons apply. These are observations, not exact lost data.
    uint64_t rejectedReads = 0;
    uint64_t readFailures = 0;
    uint64_t overlapReads = 0;
    uint64_t readyTimeouts = 0;
    // Last read fault: bit 0 = driver failed, bit 1 = new DRDY event during read.
    // Start clears this last-fault detail, but never the cumulative counters above.
    uint32_t readFault = 0;
    // Packed driver diagnostic: low byte is stage/result; upper bits hold elapsed
    // microseconds. Retained for USB debugging, not sent in the Feather frame.
    uint32_t readDetail = 0;
    // Timing diagnostics reset on Start and are reported on USB only. Wake delay
    // starts at ISR entry, so it excludes any delay before the interrupt runs.
    uint32_t wakeUs = 0;
    uint32_t spiUs = 0;
    uint32_t maxWakeUs = 0;
    uint32_t maxReadUs = 0;
    uint32_t observedEdges = 0;
    // Largest interval between 30k ISR entries; a warning clue, not an exact
    // lost-sample count (GPIO can merge multiple edges while interrupts are masked).
    uint32_t maxGapUs = 0;
};
