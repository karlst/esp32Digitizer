/**
 * @file recordingMonitor.h
 * @brief Keep the last fully validated recording status, owned by Feather's loop.
 */
#pragma once
#include <Arduino.h>
#include "../../shared/recordingStatus.h"

/**
 * @brief Format S3 recording telemetry without claiming stale data is current.
 * s3Monitor commits this object only after validating the entire UART frame.
 * No other task touches it, so it requires no separate mutex.
 */
class recordingMonitor
{
public:
    recordingStatus status;
    bool available = false;
    String stateJson(bool connected) const;
};
