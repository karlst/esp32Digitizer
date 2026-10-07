/**
 * @file sdmmcBlockDevice.cpp
 * @brief Move filesystem sectors over native SDMMC, preserving exFAT support.
 * The storage task owns this adapter. Calls block until a transfer finishes;
 * acquisition continues independently by submitting bytes to bufferedWriter.
 */
#include "sdmmcBlockDevice.h"
#include <driver/sdmmc_host.h>
#include <soc/sdmmc_struct.h>
#include <algorithm>
#include <cstring>

/**
 * @brief Calculate slot 1's configured clock from the S3 hardware dividers.
 * The storage task calls this while no transfers are active. The driver's
 * card.max_freq_khz is a requested limit, not a measurement of the active clock.
 * The PLL source is 160,000 kHz; the host divider is the stored value plus one,
 * then the card divider either bypasses (zero) or divides by twice its value.
 * Zero means an unexpected source/routing, so startup must not claim a frequency.
 * This checks register configuration, not the electrical waveform on the wire.
 */
static uint32_t configuredClockKhz()
{
    uint32_t retVal = 0;
    if (SDMMC.clock.clk_sel == 1 && SDMMC.clksrc.card1 == 1)
    {
        const uint32_t hostDivider = SDMMC.clock.div_factor_n + 1;
        const uint32_t cardDivider = SDMMC.clkdiv.div1;
        retVal = 160000 / hostDivider / (cardDivider ? 2 * cardDivider : 1);
    }
    return retVal;
}

/**
 * @brief Initialize the card slot on the agreed breakout wiring.
 * Called before FsVolume::begin(). A successful result identifies a card and
 * negotiates its bus width/clock; filesystem recognition happens afterward.
 * Failure releases the host so a later stopped-state mount can retry cleanly.
 */
bool sdmmcBlockDevice::begin()
{
    bool retVal = ready;
    if (!ready)
    {
        // Slot 1 is the controller's logical slot, not a GPIO number. The S3
        // routes these six signals to arbitrary available pins. Adafruit 4682
        // supplies external pullups; its power input must be 3.3 V.
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.flags = recordingConfig::sdWidth == 1 ? SDMMC_HOST_FLAG_1BIT : SDMMC_HOST_FLAG_4BIT;
        host.max_freq_khz = recordingConfig::sdClockKhz;
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();

        // One-bit diagnostic mode ignores D1/D2/D3 during transfers; the
        // normal build uses all four data wires. No physical rewiring is needed.
        slot.width = recordingConfig::sdWidth;
        slot.clk = static_cast<gpio_num_t>(recordingConfig::clockPin); slot.cmd = static_cast<gpio_num_t>(recordingConfig::commandPin);
        slot.d0 = static_cast<gpio_num_t>(recordingConfig::data0Pin); slot.d1 = static_cast<gpio_num_t>(recordingConfig::data1Pin);
        slot.d2 = static_cast<gpio_num_t>(recordingConfig::data2Pin); slot.d3 = static_cast<gpio_num_t>(recordingConfig::data3Pin);
        esp_err_t error = sdmmc_host_init();
        hostStarted = error == ESP_OK;
        if (error == ESP_OK)
        {
            error = sdmmc_host_init_slot(host.slot, &slot);
        }
        if (error == ESP_OK)
        {
            // Card initialization starts slowly, then negotiates the selected width
            // and the requested maximum clock (one-bit in the diagnostic build).
            // No filesystem sectors are erased.
            error = sdmmc_card_init(&host, &card);
        }
        if (error == ESP_OK)
        {
            // IDF 4.4 initialization chooses from preset clocks and may leave
            // an intermediate request at 400 kHz. Explicitly set the selected
            // clock before any filesystem access, then inspect actual dividers.
            Serial.printf("SDMMC after init: requested=%u actual=%lu kHz\n",
                static_cast<unsigned>(recordingConfig::sdClockKhz),
                static_cast<unsigned long>(configuredClockKhz()));
            const uint32_t targetKhz = std::min<uint32_t>(recordingConfig::sdClockKhz, card.max_freq_khz);
            error = sdmmc_host_set_card_clk(host.slot, targetKhz);
            if (error == ESP_OK && configuredClockKhz() == targetKhz * 2)
            {
                // This older S3 driver doubles intermediate clock requests.
                // Compensate only when the registers confirm that exact case;
                // the driver performs its normal clock-disable/update sequence.
                error = sdmmc_host_set_card_clk(host.slot, targetKhz / 2);
                if (error == ESP_OK)
                {
                    // The driver also derives its 100-ms data timeout from the
                    // argument. Preserve that duration at the true clock rate,
                    // rather than accidentally halving it with the workaround.
                    SDMMC.tmout.data = std::min<uint32_t>(targetKhz * 100, 0xffffff);
                }
            }
            if (error == ESP_OK && configuredClockKhz() != targetKhz)
            {
                Serial.printf("SDMMC clock mismatch: wanted=%lu actual=%lu kHz\n",
                    static_cast<unsigned long>(targetKhz),
                    static_cast<unsigned long>(configuredClockKhz()));
                error = ESP_ERR_INVALID_STATE;
            }
        }
        ready = error == ESP_OK && card.csd.sector_size == 512;
        retVal = ready;
        if (ready)
        {
            Serial.printf("SDMMC ready: width=%u clock=%u kHz sectors=%lu\n",
                static_cast<unsigned>(sdmmc_host_get_slot_width(host.slot)),
                static_cast<unsigned>(configuredClockKhz()),
                static_cast<unsigned long>(card.csd.capacity));
        }
        else
        {
            Serial.printf("SDMMC initialization failed: %s sectorBytes=%d\n",
                esp_err_to_name(error), card.csd.sector_size);
            end();
        }
    }
    return retVal;
}

/**
 * @brief Release the controller after a failed mount or explicit shutdown.
 * The caller must close files first. This does not flush filesystem caches.
 */
void sdmmcBlockDevice::end()
{
    ready = false;
    if (hostStarted)
    {
        sdmmc_host_deinit();
        hostStarted = false;
    }
}

/**
 * @brief Report no pending asynchronous transfer: this adapter waits in each call.
 * This describes host transfers, not whether card contents survive power loss.
 */
bool sdmmcBlockDevice::isBusy()
{
    const bool retVal = false;
    return retVal;
}

/** @brief Read one 512-byte filesystem sector into the caller's buffer. */
bool sdmmcBlockDevice::readSector(Sector_t sector, uint8_t* destination)
{
    const bool retVal = readSectors(sector, destination, 1);
    return retVal;
}

/** @brief Read count consecutive sectors; false means the read is incomplete. */
bool sdmmcBlockDevice::readSectors(Sector_t sector, uint8_t* destination, size_t count)
{
    const bool retVal = transfer(sector, destination, nullptr, count);
    return retVal;
}

/** @brief Write one 512-byte filesystem sector; no caller pointer is retained. */
bool sdmmcBlockDevice::writeSector(Sector_t sector, const uint8_t* source)
{
    const bool retVal = writeSectors(sector, source, 1);
    return retVal;
}

/** @brief Write count consecutive sectors; earlier chunks can persist on failure. */
bool sdmmcBlockDevice::writeSectors(Sector_t sector, const uint8_t* source, size_t count)
{
    const bool retVal = transfer(sector, nullptr, source, count);
    return retVal;
}

/** @brief Return the initialized card capacity in 512-byte sectors, or zero. */
Sector_t sdmmcBlockDevice::sectorCount()
{
    const Sector_t retVal = ready ? card.csd.capacity : 0;
    return retVal;
}

/**
 * @brief Check card status after SdFat has submitted its cached sectors.
 * Sector writes already wait for completion in ESP-IDF. This adapter has no
 * deferred writes; CMD13 checks card status and reports communication failure.
 * It does not promise persistence through sudden loss of power inside the card.
 */
bool sdmmcBlockDevice::syncDevice()
{
    const bool retVal = ready && sdmmc_get_status(&card) == ESP_OK;
    return retVal;
}

/**
 * @brief Copy sectors through aligned RAM and wait for each hardware DMA transfer.
 * Exactly one pointer is non-null: destination for reads, source for writes.
 * The disk task alone uses transferBuffer, so no lock is needed. For example,
 * with the default 4096-byte buffer, ten sectors become eight, then two.
 * Diagnostic builds can enlarge that buffer: 8192 bytes permits 16 sectors
 * per call and 16384 permits 32. The filesystem may still request fewer sectors.
 * On writes, memcpy copies caller bytes before sdmmc_write_sectors sends them.
 * On reads, sdmmc_read_sectors fills RAM before memcpy returns bytes to SdFat.
 * A failure stops immediately and returns false; already written chunks remain.
 */
bool sdmmcBlockDevice::transfer(Sector_t sector, uint8_t* destination,
    const uint8_t* source, size_t count)
{
    bool retVal = ready && ((destination != nullptr) != (source != nullptr)) &&
        sector <= card.csd.capacity && count <= card.csd.capacity - sector;
    size_t completed = 0;
    while (retVal && completed < count)
    {
        const size_t chunk = std::min(count - completed, sizeof(transferBuffer) / 512);
        esp_err_t error;
        if (source)
        {
            std::memcpy(transferBuffer, source + completed * 512, chunk * 512);
            error = sdmmc_write_sectors(&card, transferBuffer, sector + completed, chunk);
        }
        else
        {
            error = sdmmc_read_sectors(&card, transferBuffer, sector + completed, chunk);
            if (error == ESP_OK)
            {
                std::memcpy(destination + completed * 512, transferBuffer, chunk * 512);
            }
        }
        retVal = error == ESP_OK;
        if (!retVal)
        {
            Serial.printf("SDMMC %s failed: sector=%lu count=%u error=%s\n",
                source ? "write" : "read", static_cast<unsigned long>(sector + completed),
                static_cast<unsigned>(chunk), esp_err_to_name(error));
        }
        completed += chunk;
    }
    return retVal;
}
