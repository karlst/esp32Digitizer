/**
 * @file recordingSpace.h
 * @brief Estimate remaining card space from a measured baseline and file growth.
 */
#pragma once
#include <cstdint>
/**
 * @brief Keep slow free-space scans out of the recording loop.
 * The disk task measures free bytes while stopped, then subtracts the estimated
 * extra allocation of this session's files. A file consumes whole clusters
 * (filesystem allocation blocks), even when its last cluster is mostly empty.
 * Directory growth and library allocation ahead of file length are not included;
 * this is a display estimate, never permission to write or a full-card safeguard.
 */
class recordingSpace
{
public:
    /**
     * @brief Save a successful scan and the session allocation already included in it.
     * Call after opening the initial file, before allowing samples into the ring.
     */
    void begin(uint64_t freeBytes, uint64_t allocatedBytes)
    {
        baselineFree = freeBytes;
        baselineAllocated = allocatedBytes;
    }
    /**
     * @brief Subtract only growth after the scan; saturate rather than underflow.
     * allocatedBytes is the sum of rounded lengths of this session's files,
     * not a count of write calls. Rewriting a header must not reduce free space.
     */
    uint64_t remaining(uint64_t allocatedBytes) const
    {
        const uint64_t growth = allocatedBytes > baselineAllocated ? allocatedBytes - baselineAllocated : 0;
        const uint64_t retVal = growth < baselineFree ? baselineFree - growth : 0;
        return retVal;
    }
    /**
     * @brief Round one file's length up to its allocation block size, in bytes.
     * A zero block size means no mounted filesystem and yields zero, not division
     * by zero. Each split recording file is rounded separately by the SD adapter.
     */
    static uint64_t allocation(uint64_t fileBytes, uint32_t clusterBytes)
    {
        const uint64_t retVal = clusterBytes ?
            (fileBytes / clusterBytes + (fileBytes % clusterBytes != 0)) * clusterBytes : 0;
        return retVal;
    }
private:
    uint64_t baselineFree = 0, baselineAllocated = 0;
};
