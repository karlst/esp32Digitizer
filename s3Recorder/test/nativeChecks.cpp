/** @file nativeChecks.cpp
 * @brief Run real parser, acquisition state, and UART code with deterministic fake hardware.
 */
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

uint32_t testMs = 0;
uint32_t testUs = 0;
int testReady = LOW;
fakeSerial Serial;
fakeSerial Serial1;
fakeEsp ESP;
static bool startOk = true;
static bool stopOk = true;
static bool readOk = true;
static bool queueFull = false;
static acquisitionCommand queued;
static int startCalls = 0;
static int32_t rawValue = -123456;

/** @brief Simulate ADC initialization. */
bool ads1256::begin() { return true; }
/** @brief Count actual starts to detect duplicate side effects. */
bool ads1256::start(uint32_t) { ++startCalls; return startOk; }
/** @brief Simulate either successful standby or a missing ADC. */
bool ads1256::stop() { return stopOk; }
/** @brief Simulate a complete or failed conversion transfer. */
bool ads1256::read(int32_t& value) { value = rawValue; testReady = HIGH; return readOk; }
/** @brief Fake an incomplete transfer diagnostic when readOk is false. */
uint32_t ads1256::readDiagnostic() const { return readOk ? 0 : 3; }
/** @brief Allocate a single simulated queue. */
QueueHandle_t xQueueCreate(int, size_t) { queueFull = false; return &queued; }
/** @brief Release the simulated queue. */
void vQueueDelete(QueueHandle_t) { queueFull = false; }
/** @brief Preserve the pending item when the one-slot queue is full. */
int xQueueSend(QueueHandle_t, const void* item, int)
{
    const int retVal = queueFull ? pdFALSE : pdTRUE;
    if (!queueFull) { queued = *static_cast<const acquisitionCommand*>(item); queueFull = true; }
    return retVal;
}
/** @brief Consume the simulated request. */
int xQueueReceive(QueueHandle_t, void* item, int)
{
    const int retVal = queueFull ? pdTRUE : pdFALSE;
    if (queueFull) { *static_cast<acquisitionCommand*>(item) = queued; queueFull = false; }
    return retVal;
}
/** @brief Suppress actual task creation; tests invoke finite worker steps. */
int xTaskCreatePinnedToCore(void (*)(void*), const char*, int, void*, int, TaskHandle_t* task, int)
{ *task = &queued; return pdPASS; }
/** @brief Model a readiness timeout. */
uint32_t ulTaskNotifyTake(int, uint32_t wait) { testMs += wait; return 0; }
/** @brief No scheduler exists in the deterministic tests. */
void vTaskNotifyGiveFromISR(TaskHandle_t, BaseType_t*) {}
/** @brief Advance simulated scheduler time. */
void vTaskDelay(uint32_t wait) { testMs += wait; }

/** @brief Parse a mutable copy without modifying the test's string literal. */
static bool parse(const char* text, acquisitionCommand& command)
{
    char copy[160];
    assert(std::strlen(text) < sizeof(copy));
    std::memcpy(copy, text, std::strlen(text) + 1);
    const bool retVal = commandProtocol::parse(copy, command);
    return retVal;
}

/** @brief Feed a complete UART fragment; count only accepted complete frames. */
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

/** @brief Check untrusted UART input, resynchronization, rate encoding, and signed limits. */
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

/** @brief Check actual acquisition state changes, idempotence, faults, and cumulative counts. */
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
    testReady = LOW;
    recorder.collect();
    assert(recorder.snapshot().sampleCount == 1 && recorder.snapshot().error == 3);
    readOk = true;
    stopOk = false;
    recorder.execute({9, acquisitionCommand::Action::reboot, 0});
    assert(recorder.snapshot().reboot && !recorder.snapshot().running && recorder.snapshot().ackResult == 1);
    stopOk = true;
}

/** @brief Check frame shape, 64-bit counts, congestion, and acknowledgement-before-reboot. */
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
    assert(Serial1.output == "S3,2,1000,0,0,0,0,4294967295,0,0,1000,0,0\n");
    status.sampleCount = UINT64_MAX;
    status.latestRaw = -8388608;
    status.ackId = UINT32_MAX;
    status.hasSample = true;
    status.lastSampleMs = UINT32_MAX - 9;
    testMs = 10;
    Serial1.output.clear();
    assert(link.report(status));
    assert(Serial1.output.find("18446744073709551615") != std::string::npos);
    assert(Serial1.output.find("-8388608,20,") != std::string::npos);
    assert(Serial1.output.size() < 160);
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
    assert(Serial1.flushed && Serial1.output.find(",42,1\n") != std::string::npos);
    assert(ESP.restarts == 0);
    testMs += 100;
    link.update();
    assert(ESP.restarts == 1);
}

/** @brief Run finite desktop checks; never touch USB devices or serial ports. */
int main()
{
    checkProtocol();
    nativeChecks::checkAcquisition();
    nativeChecks::checkLink();
    std::cout << "PASS: protocol, framing, signed samples, acquisition state/faults, replay protection, UART and reboot ordering.\n";
    return 0;
}
