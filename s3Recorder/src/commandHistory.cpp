/**
 * @file commandHistory.cpp
 * @brief Replay acknowledgements without replaying Start, Stop, or Reboot.
 */
#include "commandHistory.h"

/**
 * @brief Find a recent id, returning rejection when its contents have changed.
 */
bool commandHistory::lookup(const acquisitionCommand& command, uint32_t& result) const
{
    bool retVal = false;
    for (size_t index = 0; index < 16 && !retVal; ++index)
    {
        if (results[index] && entries[index].id == command.id)
        {
            result = entries[index].action == command.action && entries[index].rate == command.rate ?
                results[index] : 2;
            retVal = true;
        }
    }
    return retVal;
}

/**
 * @brief Save a completed outcome; called only after lookup found no existing id.
 */
void commandHistory::remember(const acquisitionCommand& command, uint32_t result)
{
    entries[next] = command;
    results[next] = result;
    next = (next + 1) % 16;
}
