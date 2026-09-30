/**
 * @file nativeChecks.h
 * @brief Controlled access for finite worker/link tests without an RTOS thread.
 */
#pragma once
/** @brief Let desktop tests call finite private steps instead of launching forever loops.
 * acquisition and featherLink declare this class a friend. That provides test
 * access without exposing internal execute/collect/report methods in their public
 * API. Its methods are defined in nativeChecks.cpp, never linked into firmware. */
class nativeChecks
{
public:
    static void checkAcquisition();
    static void checkLink();
    static void checkFastAcquisition();
};
