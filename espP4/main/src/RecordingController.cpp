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
 * @file RecordingController.cpp
 * Console and Feather commands, including their required handshake replies.
 * Read initialize, poll and handleLine. ARM3 asks P4 to receive; READY3 grants
 * permission for Feather to start the ADC. STOP reports that Feather stopped.
 * UART is the serial link between boards; samples arrive on separate ADC pins.
 */
#include "RecordingController.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <sys/time.h>

/* **
 * RecordingController - Save the application and reporter references, and
 * register this controller to receive application events. The optional console
 * handler supplies test commands. All supplied objects must remain alive while
 * the controller uses them.
 */
RecordingController::RecordingController(RecordingApplication& recordingApplication,
    RecordingReporter& recordingReporter, RecordingConsoleCommands* recordingConsoleCommands)
    : _recordingApplication(recordingApplication), _recordingReporter(recordingReporter),
      _recordingConsoleCommands(recordingConsoleCommands)
{
    _recordingApplication.setEvents(*this);
}

/* **
 * initialize - Core 0 opens nonblocking console input and UART1, without
 * starting capture. Nonblocking reads let polling supervise the application even
 * when no user types. UART errors are read separately so damaged commands are
 * discarded.
 */
void RecordingController::initialize()
{
    const int consoleFlags = fcntl(STDIN_FILENO, F_GETFL, 0);
    ESP_ERROR_CHECK(consoleFlags >= 0 && fcntl(STDIN_FILENO, F_SETFL,
                                             consoleFlags | O_NONBLOCK) == 0
                        ? ESP_OK
                        : ESP_FAIL);

    // Eight data bits, no parity, one stop bit, no flow-control wires.
    // GPIO4 receives commands; GPIO5 sends replies. RX/TX buffers hold bytes,
    // not ADC samples. Sixteen event slots report serial errors to poll().
    uart_config_t uartConfig = {};
    uartConfig.baud_rate = 115200;
    uartConfig.data_bits = UART_DATA_8_BITS;
    uartConfig.parity = UART_PARITY_DISABLE;
    uartConfig.stop_bits = UART_STOP_BITS_1;
    uartConfig.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uartConfig.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(_featherPort, &uartConfig));
    ESP_ERROR_CHECK(uart_set_pin(_featherPort, 5, 4, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(_featherPort, 2048, 512, 16, &_uartEvents, 0));
    _recordingReporter.startup();
}

/* **
 * reply - Core 0 sends a control reply to Feather and offers an optional PC
 * echo. Callers supply printf-style format text and values. Replies describe
 * setup, stop requests, or results; READY3 and RAMFULL can be sent during an
 * active capture. This function is never called from the receive interrupt
 * handler. uart_write_bytes() queues text for UART1 transmission on GPIO5; the
 * reporter optionally prints it on the PC. Neither confirms that Feather
 * received it. Formatting failure or a line too large for the 384-byte array
 * sends nothing.
 */
void RecordingController::reply(const char* format, ...)
{
    // Build one complete line before sending it; length excludes the final
    // string terminator. Do not send a truncated command that Feather could
    // misinterpret as a different request.
    char message[384];
    va_list arguments;
    va_start(arguments, format);
    const int length = vsnprintf(message, sizeof(message), format, arguments);
    va_end(arguments);

    if (length > 0 && static_cast<size_t>(length) < sizeof(message))
    {
        uart_write_bytes(_featherPort, message, length);
        _recordingReporter.message(message);
    }
}

/* **
 * handleLine - Interpret one complete Feather command on Core 0. ARM3 requests
 * preparation for a run number and sample rate. A matching STOP ends that run.
 * LINK echoes a test number while idle without starting acquisition.
 *
 * Ignore malformed commands; the extra %c in sscanf detects unwanted trailing
 * arguments. Repeated ARM3 for the active run replies READY3 without restarting.
 * A repeated matching STOP replies DONE without repeating cleanup.
 */
void RecordingController::handleLine(const char* line)
{
    unsigned long run = 0;
    unsigned long samplesPerSecond = 0;
    char extra = 0;

    if (sscanf(line, "ADC127 LINK %lu %c", &run, &extra) == 1 && _recordingApplication.getIdle())
    {
        // Probe echoes a nonce without preparing storage or starting acquisition.
        reply("ADC127 LINK_OK %lu\n", run);
    }
    else if (sscanf(line, "ADC127 ARM3 %lu %lu %c", &run, &samplesPerSecond, &extra) == 2)
    {
        _linkDiagnostics = false;
        _recordingReporter.message("PREP: Feather request received\n");
        using StartResult = RecordingApplication::StartResult;
        // The wire carries samples/second, never a table index. Only known
        // configurations can reach the application. ARM3 distinguishes this
        // meaning from old ARM2 messages, which must not start a recording.
        const Ads127Config* adsConfigRequested = getAds127Config(samplesPerSecond);
        const StartResult result = adsConfigRequested != nullptr
                                       ? _recordingApplication.start(run, *adsConfigRequested)
                                       : StartResult::invalidRequest;
        switch (result)
        {
        case StartResult::preparing:
        case StartResult::alreadyPreparing:
            // Do not grant permission to start ADC sampling during disk setup.
            break;
        case StartResult::alreadyRunning:
            reply("ADC127 READY3 %lu %lu\n", run, samplesPerSecond);
            break;
        case StartResult::busy:
            reply("ADC127 BUSY %lu\n", run);
            break;
        case StartResult::invalidRequest:
            reply("ADC127 FAILED %lu reason=invalid-request\n", run);
            break;
        case StartResult::notInitialized:
            reply("ADC127 FAILED %lu reason=not-initialized\n", run);
            break;
        case StartResult::storageBusy:
            reply("ADC127 FAILED %lu reason=storage-busy-or-unavailable\n", run);
            break;
        }
    }

    else if (sscanf(line, "ADC127 STOP %lu %c", &run, &extra) == 1 &&
             run == _recordingApplication.getRecordingState()._run)
    {
        const bool wasActive = _recordingApplication.getRecordingState()._active;
        _recordingApplication.stop();
        // Active capture reports DONE through stopped(). A cancelled pending
        // request has no receive counters, but still acknowledges Feather STOP.
        if (!wasActive)
        {
            reply("ADC127 DONE %lu\n", run);
        }
    }
    else if (_linkDiagnostics)
    {
        _recordingReporter.message("LINK REJECTED: unrecognized UART line; see received hex\n");
    }
}

/* **
 * destinationFull - Request shutdown when the destination is full; ADC stop is
 * not yet confirmed. Keep the existing RAMFULL wire token for Feather
 * compatibility; it now also represents exhausted preallocated SD capacity.
 */
void RecordingController::destinationFull(uint32_t run)
{
    reply("ADC127 RAMFULL %lu\n", static_cast<unsigned long>(run));
}

/* **
 * decoderStalled - Notify Feather of an unrecoverable decoder stall before the
 * application aborts.
 */
void RecordingController::decoderStalled(uint32_t run)
{
    reply("ADC127 FAULT %lu reason=decoder-stalled\n", static_cast<unsigned long>(run));
}

/* **
 * stopped - Tell Feather that reception ended, then send the acquisition
 * statistics to the reporter. DONE, FAULT or TIMEOUT describes why reception
 * stopped. It does not confirm that pending SD writes or readback have finished.
 */
void RecordingController::stopped(const RecordingResult& recordingResult)
{
    const auto& captureStats = recordingResult._captureStats;
    const auto& receiverStats = recordingResult._receiverStats;
    const char* kind = recordingResult._reason == RecordingResult::Reason::requested ? "DONE" : recordingResult._reason == RecordingResult::Reason::fault ? "FAULT"
                                                                                                                                                          : "TIMEOUT";
    reply("ADC127 %s %lu words=%lu bad=%lu periodErrors=%lu short=%lu drops=%lu errors=%lu rate=%.1f/s\n",
        kind, static_cast<unsigned long>(recordingResult._run),
        static_cast<unsigned long>(captureStats._words), static_cast<unsigned long>(captureStats._badHeaders),
        static_cast<unsigned long>(captureStats._badPeriods), static_cast<unsigned long>(captureStats._shortWords),
        static_cast<unsigned long>(receiverStats._droppedBlocks),
        static_cast<unsigned long>(receiverStats._callbackErrors + recordingResult._commandErrors),
        recordingResult._observedRate);
    _recordingReporter.capture(recordingResult);
}

/* **
 * handleClockLine - Handle a complete console command beginning with @. @LINK
 * enables serial diagnostics while idle; @LINKSTATUS reports their counters.
 * Pass test commands to the optional test handler.
 *
 * For @UTC, set calendar time from seconds since 1970, accepting years 2020
 * through 2099 only. Refuse time changes after the destination is locked. Serial
 * delivery adds delay, so this timestamp is not a precise measurement of sample
 * time.
 */
void RecordingController::handleClockLine(const char* line)
{
    // Explicit diagnostic mode is idle-only. Queries run in this same polling
    // loop, so increasing polls establish that command processing is alive.
    unsigned long nonce = 0;
    char trailing = 0;
    if (strcmp(line, "@LINKSTATUS") == 0)
    {
        reportLinkStatus();
        return;
    }
    if (sscanf(line, "@LINK %lu %c", &nonce, &trailing) == 1)
    {
        if (_recordingApplication.getIdle() && !_recordingApplication.isRecordingDestinationLocked())
        {
            _linkBytes = _linkLines = _linkErrors = _linkPolls = 0;
            _linkHexBytes = 0;
            _linkDiagnostics = true;
            char message[64];
            snprintf(message, sizeof(message), "LINK ARMED %lu\n", nonce);
            _recordingReporter.message(message);
        }
        else
        {
            _recordingReporter.message("LINK rejected: P4 not idle\n");
        }
        return;
    }
    // Bench startup may handle @TEST; production has no test extension.
    if (_recordingConsoleCommands && _recordingConsoleCommands->line(_recordingApplication, line))
    {
        return;
    }
    unsigned long long seconds = 0;
    char extra = 0;
    bool accepted = sscanf(line, "@UTC %llu %c", &seconds, &extra) == 1 &&
                    seconds >= 1577836800ULL && seconds < 4102444800ULL &&
                    _recordingApplication.getIdle() && !_recordingApplication.isRecordingDestinationLocked();
    if (accepted)
    {
        timeval utc = {};
        utc.tv_sec = static_cast<time_t>(seconds);
        accepted = settimeofday(&utc, nullptr) == 0;
    }
    _recordingReporter.message(accepted ? "UTC OK\n" : "UTC rejected\n");
}

/* **
 * poll - Read console input and Feather serial commands once on Core 0. Reads do
 * not wait for new input. Startup separately calls RecordingApplication::poll to
 * check acquisition progress.
 *
 * Discard damaged serial input and notify the application of link errors.
 * Optional link diagnostics print a limited byte dump before recording starts;
 * these are command bytes, not ADC samples.
 */
void RecordingController::poll()
{
    ++_linkPolls;
    char consoleKey = 0;
    if (read(STDIN_FILENO, &consoleKey, 1) == 1)
    {
        if (consoleKey == '@' && !_collectingClock)
        {
            _consoleLine = BenchLine{};
            _collectingClock = true;
        }
        if (_collectingClock)
        {
            const bool complete = _consoleLine.feed(consoleKey);
            if (consoleKey == '\n')
            {
                _collectingClock = false;
                if (complete)
                {
                    handleClockLine(_consoleLine.getValue());
                }
                else
                {
                    _recordingReporter.message("UTC rejected\n");
                }
            }
        }
        else if (_recordingConsoleCommands != nullptr)
        {
            _recordingConsoleCommands->command(_recordingApplication, consoleKey);
        }
    }

    // A serial error invalidates the partial command and queued unread input.
    // During capture, notify the application so it ends the run as faulty.
    uart_event_t event = {};
    bool linkError = false;
    while (xQueueReceive(_uartEvents, &event, 0) == pdTRUE)
    {
        if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
            event.type == UART_PARITY_ERR || event.type == UART_FRAME_ERR)
        {
            linkError = true;
        }
    }
    if (linkError)
    {
        ++_linkErrors;
        _recordingReporter.message("LINK ERROR: Feather UART receive error; discarding damaged input\n");
        uart_flush_input(_featherPort);
        _benchLine = BenchLine{};
        _recordingApplication.commandFailed();
    }

    // Copy at most 256 command bytes per pass. Feed assembles complete lines;
    // a zero timeout means no waiting for the next byte or user command.
    uint8_t incoming[256];
    const int received = uart_read_bytes(_featherPort, incoming, sizeof(incoming), 0);
    if (received > 0)
    {
        _linkBytes += static_cast<uint32_t>(received);
    }
    if (_linkDiagnostics && received > 0 && _linkHexBytes < 128)
    {
        // Cap diagnostics at 128 bytes, before capture only. Hex exposes hidden
        // NULs, missing newline (0A) and boot garbage without terminal escapes.
        char message[410];
        unsigned used = static_cast<unsigned>(snprintf(message, sizeof(message), "LINK RX HEX:"));
        for (int iByte = 0; iByte < received && _linkHexBytes < 128; ++iByte, ++_linkHexBytes)
        {
            used += static_cast<unsigned>(snprintf(message + used, sizeof(message) - used, " %02X", incoming[iByte]));
        }
        snprintf(message + used, sizeof(message) - used, "\n");
        _recordingReporter.message(message);
    }
    for (int iByte = 0; iByte < received; ++iByte)
    {
        if (_benchLine.feed(static_cast<char>(incoming[iByte])))
        {
            ++_linkLines;
            handleLine(_benchLine.getValue());
        }
    }
}

/* **
 * reportLinkStatus - Report control-loop counters; bytes received do not prove a
 * valid command.
 */
void RecordingController::reportLinkStatus()
{
    char message[160];
    snprintf(message, sizeof(message), "LINK STATUS polls=%lu bytes=%lu lines=%lu errors=%lu\n",
        static_cast<unsigned long>(_linkPolls), static_cast<unsigned long>(_linkBytes),
        static_cast<unsigned long>(_linkLines), static_cast<unsigned long>(_linkErrors));
    _recordingReporter.message(message);
}

/* **
 * ready - The application confirms storage and receiver readiness; now allow ADC
 * start.
 */
void RecordingController::ready(uint32_t run, const Ads127Config& adsConfigRequested)
{
    _recordingReporter.message("PREP: storage and receiver ready; sending READY to Feather\n");
    reply("ADC127 READY3 %lu %lu\n", static_cast<unsigned long>(run),
        static_cast<unsigned long>(adsConfigRequested._samplesPerSecond));
}

/* **
 * startFailed - Convert a failed/cancelled pending start into a required Feather
 * failure reply.
 */
void RecordingController::startFailed(uint32_t run, StartFailure reason, int error)
{
    const char* detail = "preparation-failed";
    switch (reason)
    {
    case StartFailure::preparation:
        break;
    case StartFailure::preparationTimeout:
        detail = "preparation-timeout";
        break;
    case StartFailure::receiver:
        detail = esp_err_to_name(error);
        break;
    case StartFailure::cancelled:
        detail = "cancelled";
        break;
    case StartFailure::commandLink:
        detail = "command-link-error";
        break;
    }
    reply("ADC127 FAILED %lu reason=%s\n", static_cast<unsigned long>(run), detail);
}
