/**
 * @file nativeChecks.cpp
 * @brief Test real command/acquisition/link code on the PC using simulated hardware.
 * The runner compiles production .cpp files with test/fakes headers first in the
 * include search path. ADC methods below supply controlled success/failure values;
 * no real SPI, serial port, thread or interrupt is started. Tests call finite
 * worker steps through the friend class nativeChecks instead of its endless loop.
 * These assertions prove state transitions, not electrical timing or lossless sampling.
 */
#include "chokeEvents.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <limits>
#include "Arduino.h"
#include "commandProtocol.h"
#include "commandHistory.h"
#include "acquisitionStatus.h"
#include "ads1256.h"
#include "freertos/FreeRTOS.h"
#include "nativeChecks.h"
#include "acquisition.h"
#include "featherLink.h"
#include <soc/spi_struct.h>
#include <soc/gpio_struct.h>

// Tests advance simulated clocks explicitly and choose whether fake reads fail.
// edgeDuringRead injects an observed ready event in the middle of a fake SPI read.
uint32_t testMs = 0;
uint32_t testUs = 0;
int testReady = LOW;
fakeSerial Serial;
fakeSerial Serial1;
fakeEsp ESP;
static bool startOk = true;
static bool stopOk = true;
static bool readOk = true;
static volatile uint32_t* edgeDuringRead = nullptr;
static bool queueFull = false;
static acquisitionCommand queued;
static int startCalls = 0;
static int stopCalls = 0;
static int32_t rawValue = -123456;
static int abandonCalls = 0;

/**
 * @brief Simulate ADC initialization.
 */
bool ads1256::begin() { return true; }
/**
 * @brief Count actual starts to detect duplicate side effects.
 */
bool ads1256::start(uint32_t) { ++startCalls; return startOk; }
/**
 * @brief Simulate either successful standby or a missing ADC.
 */
bool ads1256::stop() { ++stopCalls; return stopOk; }
/**
 * @brief Simulate deselection without touching a failed controller.
 */
void ads1256::abandon() { ++abandonCalls; }
/**
 * @brief Simulate a complete or failed conversion transfer.
 */
bool ads1256::read(int32_t& value)
{
    value = rawValue;
    testReady = HIGH;
    if (edgeDuringRead)
    {
        ++*edgeDuringRead;
    }
    return readOk;
}
/**
 * @brief Fake an incomplete transfer diagnostic when readOk is false.
 */
uint32_t ads1256::readDiagnostic() const { return readOk ? 0 : 3; }
/**
 * @brief No SPI time elapses in the state-machine fake.
 */
uint32_t ads1256::transferMicros() const { return 0; }
/**
 * @brief Allocate a single simulated queue.
 */
QueueHandle_t xQueueCreate(int, size_t) { queueFull = false; return &queued; }
/**
 * @brief Release the simulated queue.
 */
void vQueueDelete(QueueHandle_t) { queueFull = false; }
/**
 * @brief Preserve the pending item when the one-slot queue is full.
 */
int xQueueSend(QueueHandle_t, const void* item, int)
{
    const int retVal = queueFull ? pdFALSE : pdTRUE;
    if (!queueFull) { queued = *static_cast<const acquisitionCommand*>(item); queueFull = true; }
    return retVal;
}
/**
 * @brief Consume the simulated request.
 */
int xQueueReceive(QueueHandle_t, void* item, int)
{
    const int retVal = queueFull ? pdTRUE : pdFALSE;
    if (queueFull) { *static_cast<acquisitionCommand*>(item) = queued; queueFull = false; }
    return retVal;
}
/**
 * @brief Suppress actual task creation; tests invoke finite worker steps.
 */
int xTaskCreatePinnedToCore(void (*)(void*), const char*, int, void*, int, TaskHandle_t* task, int)
{ *task = &queued; return pdPASS; }
/**
 * @brief Model a readiness timeout.
 */
uint32_t ulTaskNotifyTake(int, uint32_t wait) { testMs += wait; return 0; }
/**
 * @brief No scheduler exists in the deterministic tests.
 */
void vTaskNotifyGiveFromISR(TaskHandle_t, BaseType_t*) {}
/**
 * @brief Advance simulated scheduler time.
 */
void vTaskDelay(uint32_t wait) { testMs += wait; }

/**
 * @brief Parse a mutable copy without modifying the test's string literal.
 */
static bool parse(const char* text, acquisitionCommand& command)
{
    char copy[160];
    assert(std::strlen(text) < sizeof(copy));
    std::memcpy(copy, text, std::strlen(text) + 1);
    const bool retVal = commandProtocol::parse(copy, command);
    return retVal;
}

/**
 * @brief Feed a complete UART fragment; count only accepted complete frames.
 */
static int feed(commandProtocol& parser, const std::string& text, uint32_t now)
{
    int retVal = 0;
    acquisitionCommand command;
    for (char byte : text)
    {
        if (parser.feed(byte, now, command)) { ++retVal; }
    }
    return retVal;
}

/**
 * @brief Reject malformed command text and verify the supported rates and signed values.
 *
 * Feed real parser code fragmented/oversized/expired input, including a clock wrap.
 * Then check all seven rate-to-register mappings and the positive/negative limits
 * of the digitizer's three-byte number format. No chip or serial port is involved.
 */
static void checkProtocol()
{
    acquisitionCommand command;
    assert(parse("CMD,2,4294967295,start,30000", command));
    assert(command.id == UINT32_MAX && command.rate == 30000);
    const char* invalid[] = {"CMD,1,1,start,1000", "CMD,2,0,stop,0", "CMD,2,4294967296,stop,0",
        "CMD,2,-1,stop,0", "CMD,2,1,start,250", "CMD,2,1,start,1000.0", "CMD,2,1,start,1e3",
        "CMD,2,1,start, 1000", "CMD,2,1,stop,1000", "CMD,2,1,reboot,1", "CMD,2,1,bogus,0",
        "CMD,2,1,stop,0,junk", "CMD,2,1,stop", "CMD,2,,stop,0", "", "CMD,2,1,stop,0 "};
    for (const char* text : invalid) { assert(!parse(text, command)); }
    assert(parse("CMD,3,45,start,30000,1", command) && command.record);
    assert(parse("CMD,3,46,delete,0,0", command) && command.action == acquisitionCommand::Action::erase);
    assert(!parse("CMD,3,47,stop,0,1", command));
    assert(!parse("CMD,3,48,start,1000,2", command));
    assert(!parse("CMD,2,49,delete,0", command));
    assert(!parse("CMD,3,50,start,1000", command));
    commandProtocol parser;
    assert(feed(parser, "CMD,2,2,stop,0\r\n", 1) == 1);
    assert(feed(parser, "CMD,2,3,sto\rp,0\n", 2) == 0);
    assert(feed(parser, std::string(100, 'x') + "CMD,2,4,stop,0\n", 3) == 0);
    assert(feed(parser, "CMD,2,5,stop,0\n", 4) == 1);
    assert(feed(parser, "CMD,2,6,", 10) == 0);
    assert(feed(parser, "stop,0\n", 511) == 0);
    assert(feed(parser, "CMD,2,7,stop,0\n", 512) == 1);
    assert(feed(parser, "CMD,2,8,", UINT32_MAX - 10) == 0);
    assert(feed(parser, "stop,0\n", 5) == 1);
    const uint32_t rates[] = {100,500,1000,2000,7500,15000,30000};
    const uint8_t registers[] = {0x82,0x92,0xa1,0xb0,0xd0,0xe0,0xf0};
    for (size_t index = 0; index < 7; ++index)
    {
        uint8_t value = 0;
        assert(commandProtocol::rateRegister(rates[index], value) && value == registers[index]);
    }
    const uint8_t values[][3] = {{0,0,0},{0x7f,0xff,0xff},{0x80,0,0},{0xff,0xff,0xff}};
    const int32_t expected[] = {0,8388607,-8388608,-1};
    for (size_t index = 0; index < 4; ++index)
    { assert(commandProtocol::signedSample(values[index]) == expected[index]); }
}

/**
 * @brief Exercise Start/Stop/repeated commands and each acquisition failure reason.
 *
 * Drive one collect() call at a time using simulated ready levels, event counts,
 * and read results. Check that accepted counts exclude rejected reads, diagnostic
 * totals survive Start, last-fault flags reset, and a failed read stops acquisition.
 * A read failing both checks increments rejectedReads once, not twice.
 */
void nativeChecks::checkAcquisition()
{
    acquisition recorder;
    assert(recorder.begin());
    assert(!recorder.snapshot().running);
    acquisitionCommand start{1, acquisitionCommand::Action::start, 1000};
    recorder.execute(start);
    assert(recorder.snapshot().ackResult == 2 && startCalls == 0);
    recorder.current.ready = true;
    start.id = 2;
    recorder.execute(start);
    assert(recorder.snapshot().running && startCalls == 1);
    recorder.execute(start);
    assert(startCalls == 1);
    auto changed = start;
    changed.rate = 7500;
    recorder.execute(changed);
    assert(recorder.snapshot().ackResult == 2 && recorder.snapshot().rate == 1000);
    changed.id = 3;
    recorder.execute(changed);
    assert(recorder.snapshot().ackResult == 2 && recorder.snapshot().running);
    testReady = LOW;
    testMs = 1000;
    recorder.readyEdges = 3;
    recorder.collect();
    assert(recorder.current.sampleCount == 1 && recorder.current.latestRaw == rawValue);
    assert(recorder.current.missedEdges == 2 && recorder.current.measuredRate == 1);
    recorder.execute({4, acquisitionCommand::Action::stop, 0});
    assert(!recorder.snapshot().running && recorder.snapshot().measuredRate == 0);
    recorder.execute(start);
    assert(!recorder.snapshot().running && startCalls == 1); // delayed duplicate Start
    start.id = 5;
    recorder.execute(start);
    assert(recorder.current.sampleCount == 1 && startCalls == 2);
    testReady = HIGH;
    testMs += 101;
    recorder.collect();
    assert(!recorder.snapshot().ready && recorder.snapshot().error == 2);
    assert(recorder.snapshot().readyTimeouts == 1 && recorder.snapshot().rejectedReads == 0);
    recorder.execute({6, acquisitionCommand::Action::stop, 0});
    assert(recorder.snapshot().ackResult == 1 && recorder.snapshot().error == 2);
    recorder.current.ready = true;
    startOk = false;
    start.id = 7;
    recorder.execute(start);
    assert(!recorder.snapshot().running && recorder.snapshot().ackResult == 2);
    startOk = true;
    recorder.current.ready = true;
    start.id = 8;
    recorder.execute(start);
    readOk = false;
    recorder.readyEdges = 3;
    edgeDuringRead = &recorder.readyEdges;
    testReady = LOW;
    recorder.collect();
    assert(recorder.snapshot().sampleCount == 1 && recorder.snapshot().error == 3);
    assert(recorder.snapshot().missedEdges == 4); // Pre-read misses count even on failure.
    assert(recorder.snapshot().rejectedReads == 1 && recorder.snapshot().readFailures == 1);
    assert(recorder.snapshot().overlapReads == 1 && recorder.snapshot().readFault == 3);
    // Start resets last-fault flags but preserves every total. An overlap alone
    // is another rejected read, not another driver failure.
    readOk = true;
    recorder.current.ready = true;
    recorder.execute({10, acquisitionCommand::Action::start, 1000});
    assert(recorder.snapshot().readFault == 0 && recorder.snapshot().rejectedReads == 1);
    testReady = LOW;
    recorder.collect();
    assert(recorder.snapshot().rejectedReads == 2 && recorder.snapshot().overlapReads == 2);
    assert(recorder.snapshot().readFailures == 1 && recorder.snapshot().readFault == 2);
    edgeDuringRead = nullptr;
    recorder.current.ready = true;
    recorder.execute({11, acquisitionCommand::Action::start, 1000});
    readOk = false;
    testReady = LOW;
    recorder.collect();
    assert(recorder.snapshot().rejectedReads == 3 && recorder.snapshot().readFailures == 2);
    assert(recorder.snapshot().overlapReads == 2 && recorder.snapshot().readFault == 1);
    readOk = true;
    stopOk = false;
    recorder.execute({9, acquisitionCommand::Action::reboot, 0});
    assert(recorder.snapshot().reboot && !recorder.snapshot().running && recorder.snapshot().ackResult == 1);
    stopOk = true;
}

/**
 * @brief Verify the actual status formatter and command-to-reboot ordering.
 *
 * Capture serial output in memory to check the version-3 frame and full-width
 * 64-bit counters. Simulate a full transmit buffer, one-slot command queue, and
 * reboot request. Require the acknowledgement to be flushed before restart.
 * The UART receive-mapping check guards against reintroducing the pinMode ordering bug.
 */
void nativeChecks::checkLink()
{
    acquisition recorder;
    recorder.begin();
    featherLink link(recorder);
    link.begin();
    // pinMode after UART begin breaks the S3's native GPIO18 receive route.
    assert(testUartRxMapped);
    const std::string usbRequest = "CMD,2,40,stop,0\n";
    Serial.input.assign(usbRequest.begin(), usbRequest.end());
    link.update();
    assert(queueFull && queued.id == 40 && link.receivedBytes == 0);
    queueFull = false;
    acquisitionStatus status;
    testMs = 1000;
    Serial1.output.clear();
    assert(link.report(status));
#ifdef S3_LEGACY_STATUS
    assert(Serial1.output == "S3,2,1000,0,0,0,0,4294967295,0,0,1000,0,0\n");
#else
    std::string expected = "S3,5,1000,0,0,0,0,4294967295,0,0,1000,0,0,0,0,0,0,0,0";
    for (size_t index = 0; index < recordingStatus::fieldCount; ++index) { expected += ",0"; }
    assert(Serial1.output == expected + "\n");
#endif
    status.sampleCount = UINT64_MAX;
    status.missedEdges = UINT64_MAX;
    status.rejectedReads = UINT64_MAX;
    status.readFailures = UINT64_MAX;
    status.overlapReads = UINT64_MAX;
    status.readyTimeouts = UINT64_MAX;
    status.readFault = 3;
    for (auto& value : status.recording.values) { value = UINT64_MAX; }
    Serial1.capacity = 2048; // Firmware allocates this much space for complete v4 frames.
    status.latestRaw = -8388608;
    status.ackId = UINT32_MAX;
    status.hasSample = true;
    status.lastSampleMs = UINT32_MAX - 9;
    testMs = 10;
    Serial1.output.clear();
    assert(link.report(status));
    assert(Serial1.output.find("18446744073709551615") != std::string::npos);
    assert(Serial1.output.find("-8388608,20,") != std::string::npos);
#ifndef S3_LEGACY_STATUS
    assert(Serial1.output.find(",18446744073709551615,18446744073709551615,18446744073709551615,18446744073709551615,18446744073709551615,3,") != std::string::npos);
#endif
    assert(Serial1.output.size() < 1536);
    Serial1.capacity = 0;
    assert(!link.report(status));
    Serial1.capacity = 512;
    const std::string request = "CMD,2,42,reboot,0\n";
    Serial1.input.assign(request.begin(), request.end());
    link.update();
    assert(queueFull && queued.id == 42);
    assert(link.receivedBytes == request.size() && link.validCommands == 1 && link.queuedCommands == 1);
    assert(!recorder.submit({43, acquisitionCommand::Action::stop, 0}));
    assert(queued.id == 42);
    recorder.execute(queued);
    Serial1.output.clear();
    link.update();
#ifdef S3_LEGACY_STATUS
    assert(Serial1.flushed && Serial1.output.find(",42,1\n") != std::string::npos);
#else
    assert(Serial1.flushed && Serial1.output.find(",42,1,0,0,0,0,0,0,") != std::string::npos);
#endif
    assert(ESP.restarts == 0);
    testMs += 100;
    link.update();
    assert(ESP.restarts == 1);
}

/**
 * @brief Run finite desktop checks; never touch USB devices or serial ports.
 */
int main()
{
    checkProtocol();
#if S3_CHOKE_TEST
    nativeChecks::checkChoke();
    std::cout << "PASS: Choke Start/Stop, recording override, replay, failure/restart, status and zero ADC calls.\n";
#else
    nativeChecks::checkAcquisition();
    nativeChecks::checkLink();
    nativeChecks::checkFastAcquisition();
    nativeChecks::checkRecording();
    std::cout << "PASS: protocol, framing, signed samples, acquisition state/faults, replay protection, UART and reboot ordering.\n";
#endif
    return 0;
}

/**
 * @brief Finish simulated SPI immediately; dedicated reader tests cover its timing.
 */
static void finishFastTransfer()
{
    ++testUs;
    GPIO.status = 0;
    GPSPI2.cmd.update = 0;
    if (GPSPI2.cmd.usr)
    {
        GPSPI2.cmd.usr = 0;
        GPSPI2.data_buf[0] = 0x563412;
        GPIO.in = 1U << 9;
    }
}

/**
 * @brief Verify task/ISR handoff, final Stop accounting, restart and controller faults.
 */
void nativeChecks::checkFastAcquisition()
{
    acquisition recorder;
    recorder.current.ready = true;
    startOk = true;
    stopOk = true;
    testMs = 100;
    testUs = 100000;
    testClockHook = finishFastTransfer;
    recorder.execute({100, acquisitionCommand::Action::start, 30000});
    assert(recorder.fastMode);
    // Two interrupt reads occur before the worker copies anything. Count both,
    // retain only the latest value, and never count them again on the next copy.
    GPIO.in = 0;
    acquisition::readyInterrupt(&recorder);
    GPIO.in = 0;
    acquisition::readyInterrupt(&recorder);
    recorder.collectFast();
    recorder.collectFast();
    assert(recorder.current.sampleCount == 2 && recorder.current.latestRaw == 0x123456);
    GPIO.in = 0;
    acquisition::readyInterrupt(&recorder);
    recorder.execute({101, acquisitionCommand::Action::stop, 0});
    assert(!recorder.fastMode && !recorder.current.running && recorder.current.sampleCount == 3);
    recorder.execute({102, acquisitionCommand::Action::start, 30000});
    GPIO.in = 0;
    acquisition::readyInterrupt(&recorder);
    recorder.collectFast();
    assert(recorder.current.sampleCount == 4);
    // A controller already busy at entry must bypass ordinary SPI stop calls;
    // preserve the four good readings and account for one rejected attempt.
    testClockHook = nullptr;
    GPSPI2.cmd.usr = 1;
    GPIO.in = 0;
    acquisition::readyInterrupt(&recorder);
    recorder.collectFast();
    assert(abandonCalls == 1 && !recorder.current.ready && !recorder.current.running);
    assert(recorder.current.sampleCount == 4 && recorder.current.rejectedReads == 1);
    assert(recorder.current.readFailures == 1 && recorder.current.error == 3);
    GPSPI2 = {};
}

extern bool testStorageOpen;
extern bool testStorageFinish;
extern bool testStorageComplete;
extern uint32_t testStorageDeletes;
/**
 * @brief Verify file-before-ADC ordering, recording-aware deduplication, safe
 * deletion gating, every interrupt sample queued, and failure acknowledgements.
 */
void nativeChecks::checkRecording()
{
    acquisition recorder;
    recorder.current.ready = true;
    testStorageOpen = false;
    const int previousStarts = startCalls;
    recorder.execute({200, acquisitionCommand::Action::start, 30000, true});
    assert(startCalls == previousStarts && !recorder.current.running);
    assert(recorder.current.ready && recorder.current.ackResult == 2);
    testStorageOpen = true; startOk = true; stopOk = true;
    testClockHook = finishFastTransfer;
    recorder.execute({201, acquisitionCommand::Action::start, 30000, true});
    assert(recorder.current.running && recorder.recordingBuffer);
    GPIO.in = 0; acquisition::readyInterrupt(&recorder);
    GPIO.in = 0; acquisition::readyInterrupt(&recorder);
    assert(recorder.storage.buffer().snapshot().used == 8);
    recorder.execute({201, acquisitionCommand::Action::start, 30000, false});
    assert(recorder.current.ackResult == 2 && recorder.current.running);
    recorder.execute({202, acquisitionCommand::Action::erase, 0});
    assert(recorder.current.ackResult == 2 && testStorageDeletes == 0);
    recorder.execute({203, acquisitionCommand::Action::stop, 0});
    assert(!recorder.current.running && testStorageComplete && recorder.current.ackResult == 1);
    recorder.execute({204, acquisitionCommand::Action::erase, 0});
    assert(recorder.current.ackResult == 1 && testStorageDeletes == 1);
    recorder.execute({205, acquisitionCommand::Action::start, 1000, true});
    testStorageFinish = false;
    recorder.execute({206, acquisitionCommand::Action::stop, 0});
    assert(!recorder.current.running && recorder.current.ackResult == 2);
    testStorageFinish = true; testClockHook = nullptr;
}

#if S3_CHOKE_TEST
/**
 * @brief Step the real Choke command path while storage opening/closing is faked.
 * Check the override when the browser's recording checkbox is clear, no ADC
 * Start/Stop calls, duplicate protection, and automatic failure finalization.
 */
void nativeChecks::checkChoke()
{
    acquisition recorder;
    recorder.current.ready = true;
    testUs = testMs = 0;
    testStorageOpen = false;
    recorder.execute({300, acquisitionCommand::Action::start, 30000, false});
    assert(!recorder.current.running && recorder.current.ackResult == 2);
    testStorageOpen = true;
    recorder.execute({301, acquisitionCommand::Action::start, 30000, false});
    assert(recorder.current.running && recorder.current.recordingRequested && recorder.recordingBuffer);
    testUs = 1000; testMs = 1;
    recorder.stepChoke();
    recorder.publishChoke();
    assert(recorder.current.sampleCount == 23 && recorder.storage.buffer().snapshot().used == 92);
    recorder.execute({301, acquisitionCommand::Action::start, 30000, false});
    assert(recorder.current.sampleCount == 23 && recorder.choke.samples == 23);
    recorder.execute({302, acquisitionCommand::Action::erase, 0, false});
    assert(recorder.current.ackResult == 2 && testStorageDeletes == 0);
    recorder.execute({303, acquisitionCommand::Action::stop, 0, false});
    assert(!recorder.current.running && testStorageComplete && recorder.current.ackResult == 1);
    assert(recorder.current.chokeResult == chokeTest::stopped);
    recorder.execute({301, acquisitionCommand::Action::start, 30000, false});
    assert(!recorder.current.running); // Replayed old Start must not launch again.
    recorder.execute({304, acquisitionCommand::Action::start, 1000, true});
    assert(recorder.choke.samples == 0 && recorder.current.chokeTargetBps == 750000);
    for (uint32_t tick = 2; recorder.current.running && tick < 1000; ++tick)
    {
        testUs = tick * 1000; testMs = tick;
        recorder.stepChoke(); // No pumping: overflow must cause an automatic stop.
    }
    assert(!recorder.current.running && !testStorageComplete && recorder.current.chokeResult == chokeTest::storageLimit);
    assert(recorder.storage.buffer().snapshot().overflows == 1);
    const auto previousCount = recorder.current.sampleCount;
    recorder.execute({305, acquisitionCommand::Action::start, 1000, true});
    testUs += 1000; ++testMs;
    recorder.stepChoke();
    testStorageFinish = false;
    recorder.execute({306, acquisitionCommand::Action::stop, 0, false});
    assert(recorder.current.ackResult == 2 && recorder.current.chokeResult == chokeTest::storageLimit);
    assert(recorder.current.sampleCount == previousCount + 23);
    testStorageFinish = true;
    recorder.execute({307, acquisitionCommand::Action::reboot, 0, false});
    assert(recorder.current.reboot && startCalls == 0 && stopCalls == 0);
    // The ordinary v4 status contract still accepts the command's dropdown rate.
    featherLink link(recorder);
    link.report(recorder.snapshot());
    assert(Serial1.output.find("S3,5,") != std::string::npos);
    // Real command path crosses a ten-second boundary while a fast fake sink
    // drains the real ring. Verify the transition survives until USB is drained.
    acquisition rampRecorder;
    const uint32_t began = testUs;
    rampRecorder.execute({400, acquisitionCommand::Action::start, 1000, true});
    for (uint32_t tick = 1; tick <= 10001; ++tick)
    {
        testUs = began + tick * 1000;
        testMs = testUs / 1000;
        rampRecorder.stepChoke();
        rampRecorder.storage.buffer().pump();
    }
    assert(rampRecorder.current.running);
    rampRecorder.execute({401, acquisitionCommand::Action::stop, 0, false});
    Serial.output.clear();
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        chokeEvents::drain();
    }
    assert(Serial.output.find("rate-change [oldBps,newBps,queuedBytes,lastWriteBps]=750000,1000000,") != std::string::npos);
    assert(Serial.output.find("t=10000ms rate-change") != std::string::npos);
    assert(Serial.output.find("final-buffer") != std::string::npos);

    // Congested USB must retain a pending event. Routine details fill only 48
    // slots; a subsequent rate transition still gets one of the reserved slots.
    Serial.output.clear();
    chokeEvents::beginRun();
    Serial.capacity = 0;
    chokeEvents::drain();
    assert(Serial.output.empty());
    for (int index = 0; index < 60; ++index)
    {
        chokeEvents::add("detail", "unused", 0, 0, 0, 0, false);
    }
    chokeEvents::add("reserved-rate", "oldBps,newBps", 750000, 1000000);
    Serial.capacity = 512;
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        chokeEvents::drain();
    }
    assert(Serial.output.find("start-request") != std::string::npos);
    assert(Serial.output.find("reserved-rate") != std::string::npos);
    assert(Serial.output.find("dropped=12") != std::string::npos);

}
#endif
