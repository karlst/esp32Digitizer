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

/** @file RecordingController.cpp
 * @brief Console and Feather commands, including their required handshake replies.
 * Read initialize, poll and handleLine. ARM3 asks P4 to receive; READY3 grants
 * permission for Feather to start the ADC. STOP reports that Feather stopped.
 * UART is the serial link between boards; samples arrive on separate ADC pins.
 */
#include "RecordingController.h"
#include <cstdarg>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

/** @brief Borrow permanent application/reporting services and bind control notifications. */
RecordingController::RecordingController(RecordingApplication& recordingApplication,
    RecordingReporter& recordingReporter, RecordingConsoleCommands* recordingConsoleCommands)
    : _recordingApplication(recordingApplication), _recordingReporter(recordingReporter),
      _recordingConsoleCommands(recordingConsoleCommands)
{ _recordingApplication.setEvents(*this); }

/**
 * @brief Core 0 opens nonblocking console input and UART1, without starting capture.
 * Nonblocking reads let polling supervise the application even when no user
 * types. UART errors are read separately so damaged commands are discarded.
 */
void RecordingController::initialize()
{
    const int consoleFlags = fcntl(STDIN_FILENO, F_GETFL, 0);
    ESP_ERROR_CHECK(consoleFlags >= 0 && fcntl(STDIN_FILENO, F_SETFL,
        consoleFlags | O_NONBLOCK) == 0 ? ESP_OK : ESP_FAIL);

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

/**
 * @brief Core 0 sends a control reply to Feather and offers an optional PC echo.
 * Callers supply printf-style format text and values. Replies describe setup,
 * stop requests, or results; READY3 and RAMFULL can be sent during an active
 * capture. This function is never called from the receive interrupt handler.
 * uart_write_bytes() queues text for UART1 transmission on GPIO5; the reporter
 * optionally prints it on the PC. Neither confirms that Feather received it.
 * Formatting failure or a line too large for the 384-byte array sends nothing.
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

/**
 * @brief Parse a complete Feather line on Core 0; ignore unknown or malformed lines.
 * The trailing %c rejects extra arguments. The application enforces preparation
 * and run sequencing; the controller only translates its result into protocol.
 * Duplicate active ARM3 replies READY3 without restarting. Matching STOP after
 * completion replies DONE without repeating cleanup, preserving the handshake.
 */
void RecordingController::handleLine(const char* line)
{
    unsigned long run = 0;
    unsigned long samplesPerSecond = 0;
    char extra = 0;

    if (sscanf(line, "ADC127 ARM3 %lu %lu %c", &run, &samplesPerSecond, &extra) == 2)
    {
        using StartResult = RecordingApplication::StartResult;
        // The wire carries samples/second, never a table index. Only known
        // configurations can reach the application. ARM3 distinguishes this
        // meaning from old ARM2 messages, which must not start a recording.
        const Ads127Config* adsConfigRequested = getAds127Config(samplesPerSecond);
        const StartResult result = adsConfigRequested != nullptr
            ? _recordingApplication.start(run, *adsConfigRequested) : StartResult::invalidRequest;
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
        if (!wasActive) { reply("ADC127 DONE %lu\n", run); }
    }
}

/** @brief Request source shutdown when RAM is full; this is not confirmation that ADC stopped. */
void RecordingController::destinationFull(uint32_t run)
{ reply("ADC127 RAMFULL %lu\n", static_cast<unsigned long>(run)); }

/** @brief Notify Feather of an unrecoverable decoder stall before the application aborts. */
void RecordingController::decoderStalled(uint32_t run)
{ reply("ADC127 FAULT %lu reason=decoder-stalled\n", static_cast<unsigned long>(run)); }

/** @brief Send required final control reply first, then optional PC diagnostics. */
void RecordingController::stopped(const RecordingResult& recordingResult)
{
    const auto& captureStats = recordingResult._captureStats;
    const auto& receiverStats = recordingResult._receiverStats;
    const char* kind = recordingResult._reason == RecordingResult::Reason::requested ? "DONE" :
        recordingResult._reason == RecordingResult::Reason::fault ? "FAULT" : "TIMEOUT";
    reply("ADC127 %s %lu words=%lu bad=%lu periodErrors=%lu short=%lu drops=%lu errors=%lu rate=%.1f/s\n",
        kind, static_cast<unsigned long>(recordingResult._run),
        static_cast<unsigned long>(captureStats._words), static_cast<unsigned long>(captureStats._badHeaders),
        static_cast<unsigned long>(captureStats._badPeriods), static_cast<unsigned long>(captureStats._shortWords),
        static_cast<unsigned long>(receiverStats._droppedBlocks),
        static_cast<unsigned long>(receiverStats._callbackErrors + recordingResult._commandErrors),
        recordingResult._observedRate);
    _recordingReporter.capture(recordingResult);
}

/** @brief Service console and Feather once without blocking; startup separately polls the application. */
void RecordingController::poll()
{
    char consoleKey = 0;
    if (read(STDIN_FILENO, &consoleKey, 1) == 1)
    {
        if (_recordingConsoleCommands != nullptr)
        { _recordingConsoleCommands->command(_recordingApplication, consoleKey); }
    }

    // A serial error invalidates the partial command and queued unread input.
    // During capture, notify the application so it ends the run as faulty.
    uart_event_t event = {};
    bool linkError = false;
    while (xQueueReceive(_uartEvents, &event, 0) == pdTRUE)
    {
        if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
            event.type == UART_PARITY_ERR || event.type == UART_FRAME_ERR)
        { linkError = true; }
    }
    if (linkError)
    {
        uart_flush_input(_featherPort);
        _benchLine = BenchLine{};
        _recordingApplication.commandFailed();
    }

    // Copy at most 256 command bytes per pass. Feed assembles complete lines;
    // a zero timeout means no waiting for the next byte or user command.
    uint8_t incoming[256];
    const int received = uart_read_bytes(_featherPort, incoming, sizeof(incoming), 0);
    for (int index = 0; index < received; ++index)
    {
        if (_benchLine.feed(static_cast<char>(incoming[index]))) { handleLine(_benchLine.getValue()); }
    }
}


/** @brief The application confirms storage and receiver readiness; now allow ADC start. */
void RecordingController::ready(uint32_t run, const Ads127Config& adsConfigRequested)
{ reply("ADC127 READY3 %lu %lu\n", static_cast<unsigned long>(run),
        static_cast<unsigned long>(adsConfigRequested._samplesPerSecond)); }

/** @brief Convert a failed/cancelled pending start into a required Feather failure reply. */
void RecordingController::startFailed(uint32_t run, StartFailure reason, int error)
{
    const char* detail = "preparation-failed";
    switch (reason)
    {
    case StartFailure::preparation: break;
    case StartFailure::preparationTimeout: detail = "preparation-timeout"; break;
    case StartFailure::receiver: detail = esp_err_to_name(error); break;
    case StartFailure::cancelled: detail = "cancelled"; break;
    case StartFailure::commandLink: detail = "command-link-error"; break;
    }
    reply("ADC127 FAILED %lu reason=%s\n", static_cast<unsigned long>(run), detail);
}
