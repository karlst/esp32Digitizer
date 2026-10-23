// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file P4RecordingDisk.cpp
 * Initialize onboard four-wire SD; format only on an explicit caller request.
 * LDO4 supplies the SD interface voltage, CLK43/CMD44 control communication and
 * GPIO39–42 carry data. FatFs calls use a drive number, not a VFS filename mount.
 */
#include "P4RecordingDisk.h"
#include "driver/sdmmc_host.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "esp_heap_caps.h"

/* **
 * getDrive - Return the filesystem drive name, such as 0:. It is valid after
 * prepare succeeds; it is not a recording filename.
 */
const char* P4RecordingDisk::getDrive() const
{
    return _drive;
}

/* **
 * prepare - Initialize the P4's SD hardware and mount the card's filesystem. The
 * disk task calls this before selecting recording files. Return true when the
 * mounted drive can be used.
 *
 * format must be explicitly requested by the preparation mode. When true,
 * replace the filesystem with a fresh exFAT filesystem; all files must already
 * be closed. A mount failure never triggers formatting automatically. A failed
 * hardware initialization requires a reset before retrying.
 */
bool P4RecordingDisk::prepare(bool format)
{
    bool retVal = _initialized;
    if (!_attempted)
    {
        _attempted = true;
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
        sd_pwr_ctrl_ldo_config_t power = {};
        power.ldo_chan_id = 4;
        sd_pwr_ctrl_handle_t powerHandle = nullptr;
        retVal = sd_pwr_ctrl_new_on_chip_ldo(&power, &powerHandle) == ESP_OK;
        host.pwr_ctrl_handle = powerHandle;
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 4;
        slot.clk = GPIO_NUM_43;
        slot.cmd = GPIO_NUM_44;
        slot.d0 = GPIO_NUM_39;
        slot.d1 = GPIO_NUM_40;
        slot.d2 = GPIO_NUM_41;
        slot.d3 = GPIO_NUM_42;
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
        if (retVal)
        {
            retVal = sdmmc_host_init() == ESP_OK;
        }
        if (retVal)
        {
            retVal = sdmmc_host_init_slot(host.slot, &slot) == ESP_OK;
        }
        if (retVal)
        {
            retVal = sdmmc_card_init(&host, &_card) == ESP_OK;
        }
        BYTE drive = 0;
        if (retVal)
        {
            retVal = ff_diskio_get_drive(&drive) == ESP_OK;
        }
        if (retVal)
        {
            ff_diskio_register_sdmmc(drive, &_card);
            _drive[0] = static_cast<char>('0' + drive);
            _drive[1] = ':';
            _initialized = true;
        }
    }

    // Files must already be closed by the session backend before formatting.
    if (retVal && format)
    {
        if (_mounted)
        {
            retVal = f_mount(nullptr, _drive, 0) == FR_OK;
            _mounted = false;
        }
        void* work = heap_caps_aligned_alloc(64, 16384, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
        retVal = retVal && work != nullptr;
        MKFS_PARM options = {};
        options.fmt = FM_EXFAT;
        options.n_fat = 1;
        options.au_size = 128 * 1024;
        if (retVal)
        {
            retVal = f_mkfs(_drive, &options, work, 16384) == FR_OK;
        }
        heap_caps_free(work);
    }
    if (retVal && !_mounted)
    {
        retVal = f_mount(&_fatfs, _drive, 1) == FR_OK;
        _mounted = retVal;
    }
    return retVal;
}
