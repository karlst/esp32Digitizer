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
    // Last fully validated S3 recording values; retained after Stop so P3 can
    // show the completed run. Retaining a copy is not evidence of a live connection.
    recordingStatus status;
    // True only when the accepted UART frame supports recording statistics.
    // This is protocol capability, not card-present or recording-active status.
    bool available = false;
    String stateJson(bool connected) const;
};
