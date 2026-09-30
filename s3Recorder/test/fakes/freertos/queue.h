/**
 * @file queue.h
 * @brief Supply queue creation/send/receive names to desktop tests.
 * Real firmware uses the installed FreeRTOS header; this wrapper selects our
 * single-thread declarations in FreeRTOS.h without changing production includes.
 */
#pragma once
#include "FreeRTOS.h"
