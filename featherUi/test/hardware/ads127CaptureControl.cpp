/**
 * @file ads127CaptureControl.cpp
 * @brief Manual five-second ADC/DMA bench test, controlled through Feather USB.
 * setup() leaves DAC off and SPI inputs undriven. USB 'g' configures the ADC,
 * asks S3 to arm, waits for matching READY, then starts conversions. Five seconds
 * later (or on 's') it stops ADC then S3. UART reports are forwarded to USB so a
 * second USB cable is unnecessary. This is not the production web application.
 */
#include <Arduino.h>
#include <driver/dac.h>
#include <esp_system.h>
#include <cstring>
#include "ads127l18.h"
#include "ads127LowRate.h"
#include "ads127Pins.h"
#include "benchLine.h"
#include "ads127BenchRate.h"

static ads127l18 adc(SPI, ads127Pins::featherClock, ads127Pins::featherMiso,
                     ads127Pins::featherMosi, ads127Pins::featherSelect);
static ads127LowRate setupAdc(adc);
static benchLine replies;
enum class benchState { idle, waiting, running, stopping };
static benchState state = benchState::idle;
static unsigned long runId = 0;
static unsigned stage = 0;
static uint32_t stateMs = 0;

/** @brief Print operator commands without accessing ADC registers. */
static void printHelp()
{
    Serial.println("ADS127 DMA CONTROL: DAC off; coax stays disconnected; SD untouched.");
    Serial.println("g=16k, 1=32k, 2=64k, 3=128k, 4=256k, 5=512k, 6=1024k samples/s; 5 seconds each. s=stop; h=help.");
    Serial.println("No automatic ADC commands at boot. Confirm ADC/translator power before g.");
}

/**
 * @brief Request ADC stop, then S3 stop, retaining uncertainty if SPI echo fails.
 * A stop request is not proof of stopped hardware; the echo and subsequent S3
 * DONE are separate confirmations. Timer bounds waiting even if a wire fails.
 */
static void requestStop()
{
    const bool stopped = setupAdc.stop();
    Serial.printf("ADC STOP command %s\n", stopped ? "echo confirmed" : "FAILED / state unconfirmed");
    setupAdc.reportStatus("after STOP");
    Serial1.printf("ADC127 STOP %lu\n", runId);
    state = benchState::stopping;
    stateMs = millis();
}

/**
 * @brief Configure while stopped and ask for DMA readiness using a fresh run ID.
 * Nothing starts if register readback fails or if the S3 still has older firmware.
 */
static void requestRun(unsigned requestedStage)
{
    if (state != benchState::idle)
    {
        Serial.println("Test already pending/running; use s to stop.");
    }
    else if (setupAdc.configure(requestedStage))
    {
        stage = requestedStage;
        Serial.printf("TEST STAGE %u: %lu samples/s for 5 seconds.\n", stage,
                      static_cast<unsigned long>(ads127BenchRates[stage].samplesPerSecond));
        runId = esp_random();
        if (runId == 0)
        {
            runId = 1;
        }
        Serial.printf("ADC configured, stopped. Asking S3 to prepare DMA, run %lu.\n", runId);
        Serial1.printf("ADC127 ARM2 %lu %u\n", runId, stage);
        state = benchState::waiting;
        stateMs = millis();
    }
}

/**
 * @brief Forward bench status and act only on a matching readiness/completion ID.
 * A ready receiver is not evidence of arriving data. S3 counters report that
 * separately. Unexpected READY or stale replies never start the ADC.
 */
static void acceptReply(const char* line)
{
    if (strncmp(line, "ADC127 ", 7) == 0)
    {
        Serial.println(line);
        unsigned long received = 0;
        char extra = 0;
        unsigned receivedStage = 99;
        if (sscanf(line, "ADC127 READY2 %lu %u %c", &received, &receivedStage, &extra) == 2 &&
            receivedStage == stage &&
            received == runId && state == benchState::waiting)
        {
            if (setupAdc.start())
            {
                state = benchState::running;
                stateMs = millis();
                Serial.println("ADC START echo confirmed. Waiting for S3 DMA data; auto-stop in 5 seconds.");
            }
            else
            {
                Serial.println("ADC START failed; stopping both ends.");
                requestStop();
            }
        }
        else if (sscanf(line, "ADC127 DONE %lu", &received) == 1 &&
                 received == runId && state == benchState::stopping)
        {
            state = benchState::idle;
            Serial.println("S3 stopped. Counts describe received DMA words; review bad/overrun/error counters.");
        }
        else if ((sscanf(line, "ADC127 TIMEOUT %lu", &received) == 1 ||
                  sscanf(line, "ADC127 FAULT %lu", &received) == 1 ||
                  sscanf(line, "ADC127 FAILED %lu", &received) == 1) && received == runId &&
                 (state == benchState::waiting || state == benchState::running))
        {
            Serial.println("S3 reported a fault, setup failure or timeout; requesting ADC stop.");
            requestStop();
        }
    }
}

/**
 * @brief Disable analog outputs and open USB plus the existing inter-board UART.
 * Only loop() accesses these objects; no application mutex or worker task exists.
 */
void setup()
{
    dac_output_disable(DAC_CHANNEL_1);
    dac_output_disable(DAC_CHANNEL_2);
    pinMode(25, INPUT);
    pinMode(26, INPUT);
    pinMode(ads127Pins::featherClock, INPUT);
    pinMode(ads127Pins::featherMosi, INPUT);
    pinMode(ads127Pins::featherMiso, INPUT);
    pinMode(ads127Pins::featherSelect, INPUT);
    Serial.begin(115200);
    Serial1.setRxBufferSize(2048);
    Serial1.setTxBufferSize(512);
    pinMode(RX, INPUT_PULLUP);
    Serial1.begin(115200, SERIAL_8N1, RX, TX);
    delay(500);
    printHelp();
}

/**
 * @brief Process manual commands, UART replies and bounded run/handshake timers.
 * Short nonblocking steps keep Stop responsive. Unplugging the PC terminal does
 * not disable the five-second automatic stop while Feather remains powered.
 */
void loop()
{
    if (Serial.available())
    {
        const char command = static_cast<char>(Serial.read());
        if (command == 'g' || command == 'G')
        {
            requestRun(0);
        }
        else if (command >= '1' && command <= '6')
        {
            requestRun(static_cast<unsigned>(command - '0'));
        }
        else if ((command == 's' || command == 'S') && state != benchState::idle)
        {
            requestStop();
        }
        else if (command == 'h' || command == 'H')
        {
            printHelp();
        }
    }
    for (unsigned count = 0; count < 384 && Serial1.available(); ++count)
    {
        if (replies.feed(static_cast<char>(Serial1.read())))
        {
            acceptReply(replies.value());
        }
    }

    // Unsigned subtraction remains valid across the millis() counter wraparound.
    const uint32_t elapsed = millis() - stateMs;
    if (state == benchState::waiting && elapsed >= 2000)
    {
        Serial.println("No matching S3 READY within 2 seconds; ADC was not started.");
        requestStop();
    }
    else if (state == benchState::running && elapsed >= 5000)
    {
        requestStop();
    }
    else if (state == benchState::stopping && elapsed >= 2000)
    {
        Serial.println("No S3 DONE reply; receiver state unconfirmed. Its local timeout is 8 seconds from ARM.");
        state = benchState::idle;
    }
    delay(1);
}
