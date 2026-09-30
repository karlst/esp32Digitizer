/**
 * @file byteSink.h
 * @brief Small storage interface used by the reusable buffered writer.
 */
#pragma once
#include <cstddef>
#include <cstdint>

/**
 * @brief Receive bytes on the disk task; implementations supply the actual file.
 * write must return the number really accepted, never pretend a short write
 * succeeded. flush must report whether storage synchronization succeeded.
 * Neither method is called from the sample-producing interrupt.
 */
class byteSink
{
public:
    /** @brief Allow destruction through the interface after the writer stops. */
    virtual ~byteSink() = default;
    virtual size_t write(const uint8_t* data, size_t length) = 0;
    virtual bool flush() = 0;
};
