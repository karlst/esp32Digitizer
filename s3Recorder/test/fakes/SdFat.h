/**
 * @file SdFat.h
 * @brief Storage member types for desktop acquisition tests, which replace the
 * recordingService methods rather than accessing a filesystem. These substitutes
 * do not test SdFat itself; physical card checks validate the storage adapter.
 */
#pragma once
/** @brief Stand-in for the filesystem owned by the simulated recording service. */
class SdFs {};
/** @brief Stand-in for a file owned by the simulated recording service. */
class FsFile {};
