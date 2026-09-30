/** @file nativeChecks.h
 * @brief Controlled access for finite worker/link tests without an RTOS thread.
 */
#pragma once
/** @brief Exercise the real private worker steps with deterministic hardware substitutes. */
class nativeChecks
{
public:
    static void checkAcquisition();
    static void checkLink();
};
