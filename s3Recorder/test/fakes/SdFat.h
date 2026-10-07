/**
 * @file SdFat.h
 * @brief Storage member types for desktop acquisition tests, which replace the
 * recordingService methods rather than accessing a filesystem. These substitutes
 * do not test SdFat itself; physical card checks validate the storage adapter.
 * Include-path order selects this file only in the native test build. Empty
 * types allow recordingService/sdRecordingSink members to be declared while
 * fakeRecordingService.cpp supplies the methods used by acquisition tests.
 * Firmware must include the real SdFat library instead; never add card behavior
 * here and mistake a passing fake for proof of real FAT/exFAT compatibility.
 */
#pragma once

/**
 * @brief Stand-in for the filesystem owned by the simulated recording service.
 */
class FsVolume {};

// Desktop command tests replace the entire disk service, including its device.
#define S3_STORAGE_TYPES_ONLY 1

/**
 * @brief Stand-in for a file owned by the simulated recording service.
 */
class FsFile {};
