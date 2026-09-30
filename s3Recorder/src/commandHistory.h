/**
 * @file commandHistory.h
 * @brief Remember recent command results without repeating hardware actions.
 */
#pragma once
#include "commandProtocol.h"

/**
 * @brief Bounded replay protection for the single-command Feather protocol.
 * Keeps the last sixteen completed commands until S3 reboot. Reusing an id
 * with different contents is rejected, including after intervening commands.
 */
class commandHistory
{
public:
    bool lookup(const acquisitionCommand& command, uint32_t& result) const;
    void remember(const acquisitionCommand& command, uint32_t result);
private:
    // Parallel arrays: entries holds request contents, results its recorded answer.
    // Result zero marks an unused slot; next wraps to replace the oldest pair.
    // Only the acquisition task uses this object, so it needs no separate lock.
    acquisitionCommand entries[16];
    uint32_t results[16] = {};
    size_t next = 0;
};
