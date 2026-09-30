/**
 * @file gpio_struct.h
 * @brief Desktop GPIO register storage; tests simulate write-one-to-clear behavior.
 */
#pragma once
#include <cstdint>
/** @brief The three GPIO registers required by the interrupt reader. */
struct fakeGpioRegisters
{
    uint32_t in = 0;
    uint32_t status = 0;
    uint32_t status_w1tc = 0;
};
inline fakeGpioRegisters GPIO;
