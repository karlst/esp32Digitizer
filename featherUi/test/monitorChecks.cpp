/**
 * @file monitorChecks.cpp
 * @brief Exercise actual UART parsing and JSON output with no connected hardware.
 */
#include <cassert>
#include <iostream>
#include "s3Monitor.h"

uint32_t testMs = 0;
uint32_t testUs = 0;
int testReady = HIGH;
fakeSerial Serial;
fakeSerial Serial1;
fakeEsp ESP;

/**
 * @brief Place a serial line in the fake receive buffer and drain it through real code.
 *
 * monitor.update() handles only a bounded number of bytes per call, so repeat until
 * all bytes are consumed. This simulates successive Arduino loop passes without
 * opening a serial port or invoking a private parser directly.
 */
static void deliver(s3Monitor& monitor, HardwareSerial& port, const std::string& frame)
{
    port.input.assign(frame.begin(), frame.end());
    while (port.available())
    {
        monitor.update();
    }
}

/**
 * @brief Check the actual Feather serial parser and browser JSON against known messages.
 *
 * Verify exact 64-bit counter text, both status versions, rejection of malformed
 * new fields, and that bad messages do not refresh connection time. Simulate S3
 * reboot by sending reset counters and Feather disconnect by advancing its clock.
 * The test exercises code shared with firmware; it does not verify a physical UART.
 */
int main()
{
    HardwareSerial port;
    s3Monitor monitor(port);
    const std::string prefix = "S3,3,1000,1,9007199254740993,1000,-1,1,0,1,1000,0,0,";
    const std::string totals = "18446744073709551615,12,8,7,4,3\n";
    deliver(monitor, port, prefix + totals);
    std::string json = monitor.stateJson();
    assert(json.find("\"connected\":true") != std::string::npos);
    assert(json.find("\"sampleCount\":\"9007199254740993\"") != std::string::npos);
    assert(json.find("\"missedEdges\":\"18446744073709551615\"") != std::string::npos);
    assert(json.find("\"rejectedReads\":\"12\"") != std::string::npos);
    assert(json.find("\"readFailures\":\"8\"") != std::string::npos);
    assert(json.find("\"overlapReads\":\"7\"") != std::string::npos);
    assert(json.find("\"readyTimeouts\":\"4\"") != std::string::npos);
    assert(json.find("\"readFault\":3") != std::string::npos);

    // Reject malformed/overflowed/partial/extra fields without refreshing link age.
    testMs = 100;
    const std::string invalid[] = {
        "18446744073709551616,12,8,7,4,3\n", "-1,12,8,7,4,3\n",
        "1,,8,7,4,3\n", "1,12,8,7,4,4\n", "1,12,8,7,4\n", "1,12,8,7,4,3,0\n"
    };
    for (const auto& suffix : invalid)
    {
        deliver(monitor, port, prefix + suffix);
    }
    json = monitor.stateJson();
    assert(json.find("\"rejectedFrames\":6") != std::string::npos);
    assert(json.find("\"lastMessageAgeMs\":100") != std::string::npos);
    assert(json.find("\"rejectedReads\":\"12\"") != std::string::npos);

    // Older S3 firmware remains controllable, with unavailable diagnostics.
    deliver(monitor, port, "S3,2,2000,1,10,1000,1,0,0,1,1000,0,0\n");
    json = monitor.stateJson();
    assert(json.find("\"connected\":true") != std::string::npos);
    assert(json.find("\"diagnosticsAvailable\":false") != std::string::npos);
    assert(json.find("\"missedEdges\":null") != std::string::npos);
    assert(monitor.sendCommand("stop", 0));
    assert(port.output.find("CMD,2,") == 0);

    // A reboot legitimately resets every counter. Do not retain an old total.
    deliver(monitor, port, "S3,3,10,1,0,0,0,4294967295,0,0,1000,0,0,0,0,0,0,0,0\n");
    json = monitor.stateJson();
    assert(json.find("\"missedEdges\":\"0\"") != std::string::npos);
    testMs += 3000;
    json = monitor.stateJson();
    assert(json.find("\"connected\":false") != std::string::npos);
    assert(json.find("\"rejectedReads\":null") != std::string::npos);
    assert(json.find("\"readFault\":null") != std::string::npos);
    std::cout << "PASS: Feather v2/v3 parser, 64-bit diagnostic JSON, invalid frames, reset and disconnect.\n";
    return 0;
}
