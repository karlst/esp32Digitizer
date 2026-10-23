// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file main.cpp
 * Standalone SD tests: run fresh erases; run addFiles preserves; s stops.
 * USB supplies power and console access. No Feather, ADC supply or signal wiring
 * is needed. Formatting never occurs merely because the board boots.
 */
#include "SdChokeTest.h"
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <inttypes.h>
#include "esp_timer.h"

/* **
 * handleLine - Dispatch h, run fresh/addFiles/existing, s, or time plus UTC
 * seconds. All commands wait for newline, regardless of how UART splits incoming
 * bytes. The trailing character check rejects extra clock arguments. Only an
 * idle test accepts time, so a terminal cannot change timestamps during
 * recording.
 */
static void handleLine(SdChokeTest& sdChokeTest, const char* line)
{
    uint64_t seconds = 0;
    char extra = 0;
    if (strcmp(line, "h") == 0)
    {
        sdChokeTest.identify();
    }
    else if (strcmp(line, "run fresh") == 0 || strcmp(line, "run addFiles") == 0 || strcmp(line, "run existing") == 0)
    {
        const auto mode = strcmp(line, "run fresh") == 0 ? SdChokeTest::Mode::fresh : (strcmp(line, "run existing") == 0 ? SdChokeTest::Mode::existing : SdChokeTest::Mode::addFiles);
        if (!sdChokeTest.start(mode))
        {
            printf("CHOKE start rejected: set UTC first; reset before another run.\n");
        }
    }
    else if (strcmp(line, "s") == 0)
    {
        sdChokeTest.stop();
    }
    else if (sscanf(line, "time %" SCNu64 " %c", &seconds, &extra) == 1 && sdChokeTest.setUtc(seconds))
    {
        printf("CHOKE UTC OK\n");
    }
    else
    {
        // Hex exposes unexpected control bytes without interpreting them as
        // terminal commands. This runs only on a rejected, bounded input line.
        printf("CHOKE command rejected; received hex:");
        for (size_t iCharacter = 0; line[iCharacter] != '\0'; ++iCharacter)
        {
            printf(" %02X", static_cast<unsigned>(static_cast<unsigned char>(line[iCharacter])));
        }
        printf("\n");
    }
}

/* **
 * app_main - ESP-IDF calls this on Core 0 after startup; supervise the test
 * console. A task is a separately scheduled thread. The disk and generator use
 * their own tasks, so status printing cannot block their execution directly.
 * Printing still consumes CPU time; disable periodic reports in menuconfig to
 * measure its cost.
 */
extern "C" void app_main()
{
    static SdChokeTest sdChokeTest;
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    ESP_ERROR_CHECK(flags >= 0 && fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) == 0
                        ? ESP_OK
                        : ESP_FAIL);
    printf("SD test: set UTC using PC runner. run fresh = ERASE; run addFiles = preserve/add; run existing = test stop when full; s = stop; h = identify. Enter ends commands.\n");
    int64_t nextReport = 0;
    char line[64] = {};
    size_t used = 0;
    bool discard = false;
    while (true)
    {
        // Nonblocking read returns immediately if nobody has typed a command.
        char incoming[64];
        const ssize_t received = read(STDIN_FILENO, incoming, sizeof(incoming));
        for (ssize_t iCharacter = 0; iCharacter < received; ++iCharacter)
        {
            const char command = incoming[iCharacter];
            if (command == '\r' || command == '\n')
            {
                if (used != 0 && !discard)
                {
                    line[used] = '\0';
                    handleLine(sdChokeTest, line);
                }
                used = 0;
                discard = false;
            }
            else if (!discard)
            {
                // Keep a bounded line; an overlong command is discarded through
                // newline, so embedded r/s characters cannot become commands.
                if (command == '\0')
                {
                    discard = true;
                    printf("CHOKE command rejected: NUL byte\n");
                }
                else if (used < sizeof(line) - 1)
                {
                    line[used++] = command;
                }
                else
                {
                    discard = true;
                    printf("CHOKE command too long\n");
                }
            }
        }

        // report() prints phase changes and final results even with live reports off.
        if (esp_timer_get_time() >= nextReport)
        {
            sdChokeTest.report();
            nextReport = esp_timer_get_time() + 1000000;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
