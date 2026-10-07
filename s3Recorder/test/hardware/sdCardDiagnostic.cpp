/**
 * @file sdCardDiagnostic.cpp
 * @brief Isolated, read-only SDMMC test; no ADC, Feather UART or filesystem.
 * Build only with the sd-read-diagnostic environment. setup() opens USB serial;
 * loop() runs the comparison once, or again when the computer sends R.
 * runComparison() reads fixed 512-byte sectors with D0 alone, then all four
 * data wires. runPhase() owns the native controller until that phase ends.
 * No task, queue or mutex is needed: Arduino's single main thread owns all data.
 * Hardware DMA (direct memory access) fills aligned RAM inside read_sectors();
 * that function waits until completion. This program never writes card sectors.
 * A passing comparison proves only these reads, not sustained recording speed.
 */
#if S3_SD_READ_DIAGNOSTIC
#include <Arduino.h>
#include <driver/sdmmc_host.h>
#include <sdmmc_cmd.h>
#include <cstring>

// Sector zero normally contains the partition table; 32768 is the sector that
// failed in the recorder. 65654 and 66048 failed during the full filesystem
// check after the smaller raw test passed. Include both to avoid claiming a
// handful of successful sectors establishes reliable access to the whole card.
static const uint32_t sectors[] = {0, 32768, 32769, 65536, 65654, 66048};
static constexpr size_t sectorCount = sizeof(sectors) / sizeof(sectors[0]);
alignas(4) static uint8_t reference[sectorCount][512] = {};
alignas(4) static uint8_t received[512] = {};
static bool referenceValid[sectorCount] = {};
static bool pending = true;

/**
 * @brief Identify a byte sequence compactly in logs; not a replacement for CRC.
 * This FNV-style checksum is for comparing log lines. memcmp below compares all
 * 512 bytes; the controller's CRC separately checks the electrical transfer.
 */
static uint32_t fingerprint(const uint8_t* bytes)
{
    uint32_t retVal = 2166136261U;
    for (size_t index = 0; index < 512; ++index)
    {
        retVal = (retVal ^ bytes[index]) * 16777619U;
    }
    return retVal;
}

/**
 * @brief Examine untrusted DMA memory after a failed read, using a valid baseline.
 * ESP-IDF does not promise usable bytes after an error. Some or all bytes can
 * remain at our prefill value, so this is evidence about buffer changes only.
 * Bit counts describe positions 0..7 within bytes, NOT proven physical SD lines.
 * A truncated or shifted transfer can produce misleading bit patterns.
 * Dumps retain the exact baseline and failed buffer for independent analysis.
 */
static void describeFailure(size_t sectorIndex)
{
    if (referenceValid[sectorIndex])
    {
        unsigned changed = 0, different = 0;
        unsigned zeroToOne[8] = {}, oneToZero[8] = {};
        for (size_t index = 0; index < sizeof(received); ++index)
        {
            const uint8_t prefill = static_cast<uint8_t>(0xa5 ^ (index * 29));
            changed += received[index] != prefill ? 1 : 0;
            different += received[index] != reference[sectorIndex][index] ? 1 : 0;
            for (unsigned bit = 0; bit < 8; ++bit)
            {
                const uint8_t mask = static_cast<uint8_t>(1U << bit);
                const bool expected = (reference[sectorIndex][index] & mask) != 0;
                const bool actual = (received[index] & mask) != 0;
                zeroToOne[bit] += !expected && actual ? 1 : 0;
                oneToZero[bit] += expected && !actual ? 1 : 0;
            }
        }
        Serial.printf("CLUE ONLY: sector=%lu changedFromPrefill=%u/512 differentFromReference=%u/512\n",
            static_cast<unsigned long>(sectors[sectorIndex]), changed, different);
        for (unsigned bit = 0; bit < 8; ++bit)
        {
            Serial.printf("CLUE bit=%u expected0_got1=%u expected1_got0=%u\n",
                bit, zeroToOne[bit], oneToZero[bit]);
        }

        // Hex output describes existing data without decoding filenames or text.
        // It is sent after the failed transfer; no timing-critical task is active.
        Serial.print("CLUE referenceHex=");
        for (size_t index = 0; index < sizeof(received); ++index)
        {
            Serial.printf("%02x", reference[sectorIndex][index]);
        }
        Serial.println();
        Serial.print("CLUE failedHex=");
        for (size_t index = 0; index < sizeof(received); ++index)
        {
            Serial.printf("%02x", received[index]);
        }
        Serial.println();
    }
}

/**
 * @brief Initialize, read the listed fixed sectors, then release the native controller.
 * width is 1 or 4 data wires; clockKhz is the requested maximum clock in kHz.
 * saveReference records successful baseline reads. Later phases compare against
 * those bytes, skipping comparisons if the baseline failed for that sector.
 * A CRC error means an invalid transfer, not evidence of a damaged filesystem.
 * Reinitializing uses the card reset command; it does not remove physical power.
 */
static void runPhase(int width, int clockKhz, bool saveReference)
{
    Serial.printf("DIAG phase width=%d requestedKhz=%d reference=%d\n", width, clockKhz, saveReference);
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.flags = width == 1 ? SDMMC_HOST_FLAG_1BIT : SDMMC_HOST_FLAG_4BIT;
    host.max_freq_khz = clockKhz;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = width;
    slot.clk = GPIO_NUM_5; slot.cmd = GPIO_NUM_7;
    slot.d0 = GPIO_NUM_6; slot.d1 = GPIO_NUM_15;
    slot.d2 = GPIO_NUM_16; slot.d3 = GPIO_NUM_4;
    sdmmc_card_t card = {};
    esp_err_t error = sdmmc_host_init();
    const bool hostStarted = error == ESP_OK;
    if (error == ESP_OK)
    {
        error = sdmmc_host_init_slot(host.slot, &slot);
    }
    if (error == ESP_OK)
    {
        error = sdmmc_card_init(&host, &card);
    }
    Serial.printf("DIAG init=%s hostWidth=%d negotiatedKhz=%d sectorBytes=%d\n",
        esp_err_to_name(error), hostStarted ? sdmmc_host_get_slot_width(host.slot) : 0,
        card.max_freq_khz, card.csd.sector_size);
    size_t successful = 0;
    size_t matched = 0;

    // Calls below go straight to ESP-IDF, bypassing SdFat, our adapter, ring
    // buffer and all acquisition code. Failed buffers are logged as untrusted clues.
    if (error == ESP_OK && card.csd.sector_size == 512)
    {
        for (size_t index = 0; index < sectorCount; ++index)
        {
            // A position-dependent prefill distinguishes untouched memory from
            // bytes supplied by DMA; coincidental equal bytes remain possible.
            for (size_t byte = 0; byte < sizeof(received); ++byte)
            {
                received[byte] = static_cast<uint8_t>(0xa5 ^ (byte * 29));
            }
            error = sdmmc_read_sectors(&card, received, sectors[index], 1);
            const char* comparison = "unavailable";
            if (error == ESP_OK)
            {
                ++successful;
                if (saveReference)
                {
                    std::memcpy(reference[index], received, sizeof(received));
                    referenceValid[index] = true;
                    comparison = "saved";
                }
                else if (referenceValid[index])
                {
                    const bool same = std::memcmp(reference[index], received, sizeof(received)) == 0;
                    comparison = same ? "MATCH" : "MISMATCH";
                    matched += same ? 1 : 0;
                }
            }
            Serial.printf("DIAG sector=%lu read=%s hash=%08lx comparison=%s\n",
                static_cast<unsigned long>(sectors[index]), esp_err_to_name(error),
                static_cast<unsigned long>(error == ESP_OK ? fingerprint(received) : 0), comparison);
            if (error != ESP_OK && !saveReference)
            {
                describeFailure(index);
            }
        }
    }
    Serial.printf("DIAG phase-end width=%d requestedKhz=%d reads=%u/%u matches=%u\n",
        width, clockKhz, static_cast<unsigned>(successful),
        static_cast<unsigned>(sectorCount), static_cast<unsigned>(matched));
    if (hostStarted)
    {
        sdmmc_host_deinit();
    }

    // Let the controller settle between initializations; this is not a power cycle.
    delay(100);
}

/**
 * @brief Compare the same untouched sectors across widths and clock settings.
 * The final D0-only phase checks whether failures also prevent returning to the
 * known-good mode. Fresh reference flags prevent a previous run masking errors.
 */
static void runComparison()
{
    std::memset(referenceValid, 0, sizeof(referenceValid));
    Serial.println("DIAG BEGIN: read-only; no ADC/UART1/filesystem; CLK5 CMD7 D0=6 D1=15 D2=16 D3=4");
    runPhase(1, 4000, true);
    runPhase(4, 400, false);
    runPhase(4, 4000, false);
    runPhase(4, 20000, false);
    runPhase(1, 4000, false);
    Serial.println("DIAG DONE: stopped; send R to repeat. No card writes performed.");
}

/** @brief Arduino startup opens USB diagnostics; no other peripherals are started. */
void setup()
{
    Serial.begin(115200);

    // Give the host time to attach after upload. R can repeat a missed startup run.
    delay(3000);
}

/** @brief Run once or on R, then sleep briefly while waiting for another request. */
void loop()
{
    while (Serial.available())
    {
        if (Serial.read() == 'R')
        {
            pending = true;
        }
    }
    if (pending)
    {
        pending = false;
        runComparison();
    }
    delay(10);
}
#endif
