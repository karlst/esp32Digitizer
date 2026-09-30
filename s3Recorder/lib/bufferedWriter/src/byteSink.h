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
 * "Sink" simply means a destination for bytes. These pure virtual functions
 * (= 0) are a contract, not file-writing implementations. sdRecordingSink
 * supplies them for this project; a different project can supply another class
 * without changing the buffer. The default destructor does not save/close data.
 */
class byteSink
{
public:
    /**
     * @brief Allow destruction through the interface after the writer stops.
     */
    virtual ~byteSink() = default;
    // The ring calls this from its consumer task. Read length bytes at data and
    // return how many the storage API accepted. Do not retain data after returning:
    // the ring can reuse that memory immediately. A DMA adapter must wait for DMA
    // completion here before returning (DMA = hardware transfers without CPU copying).
    virtual size_t write(const uint8_t* data, size_t length) = 0;
    // Finish pending storage synchronization. Return false on failure. This does
    // not mean close the file; the project adapter controls file lifetime.
    virtual bool flush() = 0;
};
