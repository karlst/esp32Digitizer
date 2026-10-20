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
 * @file S3Monitor.h
 * @brief Receive bounded S3 status messages without disturbing DAC sampling.
 */
#pragma once
#include <Arduino.h>
#include "RecordingMonitor.h"

/**
 * @brief Own the UART receive buffer and last validated S3 acquisition status.
 *
 * begin(), update(), and stateJson() run only in Arduino's loop task. Unlike the
 * DAC generator, this class has no worker accessing its state and needs no mutex.
 * The UART driver buffers incoming bytes; update() drains only a bounded amount
 * per loop pass so a noisy/disconnected link cannot monopolize web servicing.
 * Wire format and pin assignments are documented in shared/s3StatusProtocol.md.
 */
class S3Monitor
{
public:
    /**
     * @brief Retain the dedicated serial port; USB debugging uses a different UART.
     */
    explicit S3Monitor(HardwareSerial& hardwareSerial);
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
     * record applies only to Start and requires recording-capable S3 firmware.
     */
    bool sendCommand(const char* action, uint32_t rate, bool record = false);
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
    HardwareSerial& _hardwareSerial;
    // Room for 45 fields even when every 64-bit number uses its full decimal width.
    static constexpr size_t _lineCapacity = 1536;
    static constexpr uint32_t _connectionTimeoutMs = 3000;
    char _line[_lineCapacity] = {};
    size_t _lineLength = 0;
    bool _discardLine = false;
    uint32_t _lastByteMs = 0;
    // Connection age and sample-progress age are different: messages can continue
    // arriving while the digitizer is stopped or stuck on one count.
    bool _receivedFrame = false;
    bool _observedProgress = false;
    uint32_t _lastFrameMs = 0;
    uint32_t _lastProgressMs = 0;
    // Bad serial reports, not rejected ADC reads. This belongs to the Feather
    // link receiver and resets when Feather reboots, not when S3 reboots.
    uint32_t _rejectedFrames = 0;

    // These values all belong to the same validated frame. Counters describe ADC
    // conversions read by S3, not UART bytes, DRDY edges, or generated mock readings.
    uint32_t _uptimeMs = 0;
    bool _adcReady = false;
    uint64_t _sampleCount = 0;
    // Version 2 has no loss diagnostics: show unknown, never invented zero totals.
    bool _diagnosticsAvailable = false;
    uint64_t _missedEdges = 0;
    uint64_t _rejectedReads = 0;
    uint64_t _readFailures = 0;
    uint64_t _overlapReads = 0;
    uint64_t _readyTimeouts = 0;
    uint32_t _readFault = 0;
    uint32_t _samplesPerSecond = 0;
    int32_t _latestRaw = 0;
    uint32_t _sampleAgeMs = UINT32_MAX;
    uint32_t _errorCode = 0;
    // Version 2 reports acquisition intent separately from actual arriving samples.
    bool _acquisitionRunning = false;
    uint32_t _appliedRate = 1000;
    // Latest outgoing request and its pending/confirmed/rejected/timeout state.
    // These live in firmware, so refreshing the browser cannot forget a request.
    uint32_t _commandId = 0;
    uint32_t _commandSentMs = 0;
    uint32_t _requestedRate = 0;
    bool _requestedRecording = false;
    RecordingMonitor _recordingMonitor;
    String _commandAction;
    String _commandStatus = "idle";
};
