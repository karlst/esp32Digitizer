/**
 * @file commandHistory.cpp
 * @brief Replay acknowledgements without replaying Start, Stop, or Reboot.
 */
#include "commandHistory.h"

/**
 * @brief Check whether this request ID is among the sixteen remembered results.
 * @param command The incoming request, including its ID, action and rate.
 * @param result Receives the old result for an exact duplicate, or rejection (2)
 * if the ID was reused for different contents. Unchanged when no ID is found.
 * @return True means this ID was handled; the caller must NOT execute it again.
 * False means it is not remembered, so the caller may process it as a new request.
 *
 * This is called only by the acquisition task. An old Start arriving after a
 * Stop must not restart acquisition merely because its reply was lost.
 */
bool commandHistory::lookup(const acquisitionCommand& command, uint32_t& result) const
{
    bool retVal = false;
    // A zero result marks an unused slot. Search all slots because next points
    // to the next replacement position, not necessarily the end of valid entries.
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
 * @brief Store the outcome of a new request after the acquisition task handles it.
 * @param command Request that lookup() did not already find.
 * @param result Completed result: 1 accepted or 2 rejected, never 0 (unused).
 *
 * The fixed-size history overwrites its oldest entry after sixteen requests.
 * It is held in RAM and disappears on reboot; it is not permanent replay protection.
 */
void commandHistory::remember(const acquisitionCommand& command, uint32_t result)
{
    // Save request and result together, then wrap the next slot from 15 to 0.
    entries[next] = command;
    results[next] = result;
    next = (next + 1) % 16;
}
