/**
 * @file s3Monitor.h
 * @brief Receive bounded S3 status messages without disturbing DAC sampling.
 */
#pragma once
#include <Arduino.h>

/**
 * @brief Own the UART receive buffer and last validated S3 acquisition status.
 *
 * begin(), update(), and stateJson() run only in Arduino's loop task. Unlike the
 * DAC generator, this class has no worker accessing its state and needs no mutex.
 * The UART driver buffers incoming bytes; update() drains only a bounded amount
 * per loop pass so a noisy/disconnected link cannot monopolize web servicing.
 * Wire format and pin assignments are documented in shared/s3StatusProtocol.md.
 */
class s3Monitor
{
public:
    /**
     * @brief Retain the dedicated serial port; USB debugging uses a different UART.
     */
    explicit s3Monitor(HardwareSerial& serialPort);
    /**
     * @brief Configure Feather RX/TX header pins for 115200-baud 3.3 V UART.
     */
    void begin();
    /**
     * @brief Drain available bytes, assemble lines, and accept complete valid frames.
     */
    void update();
    /**
     * @brief Return current connection/data freshness, hiding stale measurements.
     */
    String stateJson() const;
    /**
     * @brief Send one command; its eventual result is reported by stateJson().
     */
    bool sendCommand(const char* action, uint32_t rate);
    /**
     * @brief Accept only the supported rates exposed by the initial UI.
     */
    static bool validRate(uint32_t rate);

private:
    /**
     * @brief Parse an entire line before replacing the last accepted snapshot.
     */
    bool acceptLine(uint32_t nowMs);
    /**
     * @brief Parse one unsigned decimal field with explicit overflow detection.
     */
    static bool readUnsigned(const char* text, uint64_t maximum, uint64_t& value);

    // No dynamic receive allocation: oversize/corrupt lines are discarded through
    // their newline. Partial lines expire, preventing old fragments joining new data.
    HardwareSerial& serialPort;
    static constexpr size_t lineCapacity = 384;
    static constexpr uint32_t connectionTimeoutMs = 3000;
    char line[lineCapacity] = {};
    size_t lineLength = 0;
    bool discardLine = false;
    uint32_t lastByteMs = 0;
    // Connection age and sample-progress age are different: messages can continue
    // arriving while the digitizer is stopped or stuck on one count.
    bool receivedFrame = false;
    bool observedProgress = false;
    uint32_t lastFrameMs = 0;
    uint32_t lastProgressMs = 0;
    // Bad serial reports, not rejected ADC reads. This belongs to the Feather
    // link receiver and resets when Feather reboots, not when S3 reboots.
    uint32_t rejectedFrames = 0;

    // These values all belong to the same validated frame. Counters describe ADC
    // conversions read by S3, not UART bytes, DRDY edges, or generated mock readings.
    uint32_t uptimeMs = 0;
    bool adcReady = false;
    uint64_t sampleCount = 0;
    // Version 2 has no loss diagnostics: show unknown, never invented zero totals.
    bool diagnosticsAvailable = false;
    uint64_t missedEdges = 0;
    uint64_t rejectedReads = 0;
    uint64_t readFailures = 0;
    uint64_t overlapReads = 0;
    uint64_t readyTimeouts = 0;
    uint32_t readFault = 0;
    uint32_t samplesPerSecond = 0;
    int32_t latestRaw = 0;
    uint32_t sampleAgeMs = UINT32_MAX;
    uint32_t errorCode = 0;
    // Version 2 reports acquisition intent separately from actual arriving samples.
    bool acquisitionRunning = false;
    uint32_t appliedRate = 1000;
    // Latest outgoing request and its pending/confirmed/rejected/timeout state.
    // These live in firmware, so refreshing the browser cannot forget a request.
    uint32_t commandId = 0;
    uint32_t commandSentMs = 0;
    uint32_t requestedRate = 0;
    String commandAction;
    String commandStatus = "idle";
};
