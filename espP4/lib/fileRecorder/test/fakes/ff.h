// Copyright (C) 2026 Karl Stock
// SPDX-License-Identifier: GPL-3.0-or-later
/**
 * @file ff.h
 * Minimal in-memory FatFs substitute for backend tests; no disk/device access.
 * Framework-required names match FatFs. This header is never a firmware include.
 */
#pragma once
#include <cstdint>
#include <string>
#include <vector>
using UINT = unsigned;
using BYTE = unsigned char;
using FSIZE_t = uint64_t;

enum FRESULT
{
    FR_OK,
    FR_DISK_ERR,
    FR_NO_FILE,
    FR_EXIST
};

constexpr BYTE FA_READ = 1, FA_WRITE = 2, FA_CREATE_NEW = 4, AM_DIR = 16;

/**
 * Fake open file cursor; preserves the SDK type name.
 */
struct FIL
{
    std::string _path;
    uint64_t _position = 0;
    BYTE _mode = 0;
    bool _open = false;
};

/**
 * Stable directory snapshot; FF_DIR matches ESP-IDF's FatFs type name.
 */
struct FF_DIR
{
    std::vector<std::string> _paths;
    size_t _next = 0;
};

/**
 * SDK-shaped directory result; field spellings are required by FatFs.
 */
struct FILINFO
{
    char fname[256] = {};
    BYTE fattrib = 0;
    FSIZE_t fsize = 0;
};

FRESULT f_open(FIL*, const char*, BYTE);
FRESULT f_close(FIL*);
FRESULT f_read(FIL*, void*, UINT, UINT*);
FRESULT f_write(FIL*, const void*, UINT, UINT*);
FRESULT f_sync(FIL*);
FRESULT f_lseek(FIL*, FSIZE_t);
FRESULT f_expand(FIL*, FSIZE_t, BYTE);
FSIZE_t f_size(FIL*);
FRESULT f_mkdir(const char*);
FRESULT f_opendir(FF_DIR*, const char*);
FRESULT f_readdir(FF_DIR*, FILINFO*);
FRESULT f_closedir(FF_DIR*);
