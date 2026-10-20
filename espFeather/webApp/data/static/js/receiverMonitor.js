/*
 * Copyright (C) 2026 Karl Stock
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of ESP32 Digitizer.
 * You may redistribute and/or modify it under the GNU General Public License
 * as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.
 */

// Browser-side receiver controls and diagnostics, reached through Feather's HTTP API.
// The browser does not talk directly to receiver. Feather's ReceiverMonitor.cpp receives UART
// reports, validates them, and matches command acknowledgements before this code
// displays them. receiver stores cumulative acquisition counters; browser reloads do not
// reset them. Read refreshStatus for requests, updateControls for button rules.
import { appendEvent } from "./eventLog.js";
import { createRecordingPanel } from "./recordingPanel.js";

/**
 * Connect receiver controls and poll Feather's validated status every half-second.
 * The receiver itself normally sends one serial report per second; faster browser polling
 * does not create more samples or new receiver reports. Firmware evaluates receiver link freshness.
 *
 * Two connections can fail independently: browser-to-Feather HTTP, and Feather-to-receiver
 * serial. Clear live readings in either case. A pending receiver command belongs to Feather
 * firmware, so reloading this page cannot erase it or falsely confirm it.
 */
export function initializeReceiverMonitor()
{
    const connected = document.getElementById("receiver-connected");
    const adc = document.getElementById("receiver-adc");
    const receiving = document.getElementById("receiver-receiving");
    const count = document.getElementById("receiver-count");
    // Preserve each 64-bit total as decimal text. Reasons are separate counters
    // because a single rejected read can fail both checks.
    const diagnosticCounters = [
        ["missedEdges", document.getElementById("receiver-missed")],
        ["rejectedReads", document.getElementById("receiver-rejected")],
        ["readFailures", document.getElementById("receiver-read-failures")],
        ["overlapReads", document.getElementById("receiver-overlaps")],
        ["readyTimeouts", document.getElementById("receiver-timeouts")]
    ];
    const readFault = document.getElementById("receiver-read-fault");
    const rate = document.getElementById("receiver-rate");
    const raw = document.getElementById("receiver-raw");
    const error = document.getElementById("receiver-error");
    const message = document.getElementById("receiver-message");
    const form = document.getElementById("receiver-form");
    const rateInput = document.getElementById("receiver-sample-rate");
    const acquisitionButton = document.getElementById("receiver-acquisition-button");
    const rebootButton = document.getElementById("receiver-reboot-button");
    const recordInput = document.getElementById("receiver-record");
    const deleteButton = document.getElementById("receiver-delete-button");
    const renderRecording = createRecordingPanel();
    const runningMessage = document.getElementById("receiver-running");
    const commandMessage = document.getElementById("receiver-command-message");
    const unavailableMessage = document.getElementById("receiver-unavailable");
    let latestState = null;
    let commandBusy = false;
    let queuedCommand = null;
    let rateDirty = false;
    let lastOutcome = "";
    let lastLinkState = null;
    let requestPending = false;
    let browserOffline = false;

    /**
     * Set controls from confirmed receiver state while preserving an unsubmitted rate choice.
     * Disable actions while disconnected or waiting for command confirmation. Start also
     * requires the digitizer to be ready; Stop remains available for a running acquisition.
     * The dropdown locks while running. Ordinary status polls do not disable controls.
     * A draft rate becomes applied only when a later receiver report confirms Start.
     */
    function updateControls()
    {
        const available = latestState?.connected && !browserOffline;
        const pending = commandBusy || latestState?.commandStatus === "pending";
        const running = available && latestState.acquisitionRunning;
        const rec = available ? latestState.recording : null;
        const storageBusy = rec && [1, 3, 6].includes(Number(rec.state));
        acquisitionButton.disabled = !available || pending || storageBusy || (!running && !latestState.adcReady);
        unavailableMessage.textContent = browserOffline ? "Controls unavailable: cannot reach Feather." :
            !latestState?.connected ? "Controls unavailable: receiver is disconnected." :
            pending ? "Controls unavailable: waiting for receiver command confirmation." :
            !running && !latestState.adcReady ? "Start unavailable: digitizer is not ready." : "";
        rebootButton.disabled = !available || pending || storageBusy;
        rateInput.disabled = !available || pending || running || storageBusy;
        recordInput.disabled = !available || !rec || pending || running || storageBusy;
        deleteButton.disabled = !available || !rec || pending || running || storageBusy || rec.card !== "1";
        // Running settings always come from RECEIVER, even after a page reload. Before
        // Start, retain the user's draft. Old firmware never gets a recording flag.
        if (running && rec) { recordInput.checked = rec.enabled === "1"; }
        if (available && !rec) { recordInput.checked = false; }
        document.getElementById("recording-unavailable").textContent = !available ? "Recording status unavailable." :
            !rec ? "Recording unavailable — receiver update required." : storageBusy ?
            "Card operation in progress. Please wait." : rec.card !== "1" ?
            "Card not ready. Acquisition without recording remains available." :
            "A checked box creates a new recording on Start; existing files are kept.";
        acquisitionButton.setAttribute("aria-busy", String(Boolean(pending || storageBusy)));
        acquisitionButton.classList.toggle("stop-button", Boolean(running));
        acquisitionButton.textContent = running ? "Stop Acquisition" : "Start Acquisition";
        runningMessage.textContent = !available ? "Acquisition state unknown — receiver disconnected." : running ?
            `Acquisition started — ${latestState.appliedRate.toLocaleString("en-US")} samples/s` : "Acquisition stopped.";
        if (available && (running || !rateDirty))
        {
            rateInput.value = String(latestState.appliedRate);
        }
        if (pending)
        {
            commandMessage.textContent = "Command pending — waiting for receiver confirmation...";
        }
        else if (!available)
        {
            commandMessage.textContent = "Waiting for receiver connection.";
        }
        else if (latestState.commandStatus === "idle" && !rateDirty)
        {
            commandMessage.textContent = "Settings take effect on Start Acquisition.";
        }
    }

    /**
     * Replace live measurements with unknown markers after either connection is lost.
     * Do not display zero: zero would claim a measured count rather than missing data.
     * This only changes browser text; it does not clear receiver counters or stop acquisition.
     */
    function clearMeasurements()
    {
        adc.textContent = "Unknown";
        receiving.textContent = "Unknown";
        count.textContent = "—";
        for (const [, element] of diagnosticCounters)
        {
            element.textContent = "—";
        }
        readFault.textContent = "Unknown";
        rate.textContent = "—";
        raw.textContent = "—";
        error.textContent = "Unknown";
    }

    /**
     * Fetch status, or send one action and then display the returned command state.
     * action is null for a GET poll or start/stop/reboot/delete for POST. body carries
     * Start's sample rate and recording choice, or Delete's confirmation; Stop and
     * Reboot need no body. Allow one request at a
     * time and reserve one clicked command behind a poll instead of losing that click.
     *
     * A 202 HTTP reply only means Feather accepted a command for delivery. Polling must
     * continue until firmware reports confirmed, rejected, or timeout. A timeout leaves
     * the actual outcome uncertain; this code never retries a hardware command itself.
     * Log transitions once, rather than filling Recent Events on every repeated report.
     */
    async function refreshStatus(action = null, body = null)
    {
        // A command clicked during a background read gets the next slot. Disable
        // only for user commands, not status polls, to avoid the old blinking UI.
        if (requestPending && action && !commandBusy)
        {
            queuedCommand = { action, body };
            commandBusy = true;
            updateControls();
        }
        if (!requestPending)
        {
            requestPending = true;
            commandBusy = Boolean(action);
            updateControls();
            // Bound this HTTP wait at three seconds. Aborting the browser request
            // does not cancel a serial command already handed to receiver by Feather.
            const abortController = new AbortController();
            const timeoutId = window.setTimeout(() => abortController.abort(), 3000);
            try
            {
                const response = await fetch(action ? `/api/receiver/${action}` : "/api/receiver", {
                    method: action ? "POST" : "GET", body, cache: "no-store", signal: abortController.signal
                });
                if (!response.ok)
                {
                    const problem = await response.json();
                    throw new Error(problem.error || `HTTP ${response.status}`);
                }
                const state = await response.json();
                // Reject malformed responses before treating any acquisition data as live.
                if (typeof state.connected !== "boolean" || (state.connected &&
                    (typeof state.acquisitionRunning !== "boolean" || !Number.isInteger(state.appliedRate) ||
                    typeof state.adcReady !== "boolean" || typeof state.receiving !== "boolean" ||
                    typeof state.sampleCount !== "string" || !/^\d+$/.test(state.sampleCount) ||
                    !Number.isFinite(state.samplesPerSecond) || !Number.isInteger(state.errorCode) ||
                    (state.latestRaw !== null && !Number.isInteger(state.latestRaw)))))
                {
                    throw new Error("Invalid receiver status response");
                }
                // Older receiver firmware has no diagnostics. Missing data stays unknown;
                // malformed new diagnostics must not appear as reassuring zeros.
                if (state.connected && state.diagnosticsAvailable &&
                    (diagnosticCounters.some(([key]) =>
                    {
                        return typeof state[key] !== "string" || !/^\d+$/.test(state[key]);
                    }) || !Number.isInteger(state.readFault) || state.readFault < 0 || state.readFault > 3))
                {
                    throw new Error("Invalid receiver diagnostic counters");
                }
                connected.textContent = state.connected ? "Yes" : "No";
                latestState = state;
                renderRecording(state);
                if (state.connected)
                {
                    adc.textContent = state.adcReady ? "Yes" : "No";
                    receiving.textContent = state.receiving ? "Yes" : "No";
                    // Keep the 64-bit count as text; conversion to Number can lose digits.
                    count.textContent = state.sampleCount;
                    for (const [key, element] of diagnosticCounters)
                    {
                        element.textContent = state.diagnosticsAvailable ? state[key] : "Unavailable";
                    }
                    const faultNames = ["None since last Start", "Driver check failed",
                        "New sample arrived during read", "Driver check failed; new sample arrived during read"];
                    readFault.textContent = state.diagnosticsAvailable ? faultNames[state.readFault] : "Unavailable";
                    rate.textContent = String(state.samplesPerSecond);
                    raw.textContent = state.latestRaw === null ? "—" : String(state.latestRaw);
                    const errorNames = ["None", "ADC initialization failed", "ADC data-ready timeout", "ADC read/configuration error"];
                    error.textContent = errorNames[state.errorCode] ?? `Code ${state.errorCode}`;
                    message.textContent = state.receiving ? "receiver is reading digitizer samples." :
                        "receiver is reporting; waiting for fresh digitizer samples.";
                }
                else
                {
                    clearMeasurements();
                    message.textContent = state.lastMessageAgeMs === null ? "Waiting for receiver messages." :
                        "receiver messages stopped; measurements are unknown.";
                }
                // Firmware uses conversion-count progress, not raw-value changes: a
                // constant analog voltage still produces valid successive conversions.
                if (lastLinkState !== state.connected && (lastLinkState !== null || state.connected))
                {
                    appendEvent(state.connected ? "receiver connected." : "receiver connection lost.");
                }
                if (browserOffline)
                {
                    appendEvent("Digitizer monitor connection to Feather restored.");
                }
                lastLinkState = state.connected;
                browserOffline = false;
                // HTTP 202 is transport acceptance only. Display/log success solely
                // from the firmware's matched acknowledgement and applied-state checks.
                const outcome = `${state.commandId}:${state.commandStatus}`;
                if (lastOutcome && outcome !== lastOutcome && ["confirmed", "rejected", "timeout"].includes(state.commandStatus))
                {
                    const text = state.commandStatus === "confirmed" ?
                        (!state.connected ? "Last receiver command confirmed; current state unknown." :
                        state.commandAction === "start" ? `Acquisition started — ${state.appliedRate.toLocaleString("en-US")} samples/s` :
                        state.commandAction === "stop" ? "Acquisition stopped; recording file closed, if active." :
                        state.commandAction === "delete" ? "Recordings deleted." : "receiver reboot confirmed.") :
                        state.commandStatus === "timeout" ? "receiver command timed out — outcome unconfirmed." : "receiver rejected the command.";
                    commandMessage.textContent = text;
                    appendEvent(text);
                    if (state.commandStatus === "confirmed" && state.commandAction === "start")
                    {
                        rateDirty = false;
                    }
                }
                lastOutcome = outcome;
            }
            catch (failure)
            {
                connected.textContent = "Unknown";
                clearMeasurements();
                renderRecording(null);
                message.textContent = "Cannot reach Feather monitor; reconnecting...";
                if (!browserOffline)
                {
                    appendEvent(`Digitizer monitor unavailable: ${failure.message}`);
                }
                browserOffline = true;
                commandMessage.textContent = `Command/status unavailable: ${failure.message}`;
            }
            finally
            {
                window.clearTimeout(timeoutId);
                requestPending = false;
                commandBusy = false;
                // Give a waiting click the next request slot before reenabling
                // controls; otherwise a second click could slip into that gap.
                if (queuedCommand)
                {
                    const nextCommand = queuedCommand;
                    queuedCommand = null;
                    refreshStatus(nextCommand.action, nextCommand.body);
                }
                else
                {
                    updateControls();
                }
            }
        }
    }

    // Changing the dropdown only edits the next Start command. Stop ignores it.
    rateInput.addEventListener("change", () =>
    {
        rateDirty = true;
        commandMessage.textContent = "Selected rate will take effect on Start Acquisition.";
    });
    // Prevent a page reload; choose Start/Stop from receiver-confirmed running state.
    form.addEventListener("submit", (event) =>
    {
        event.preventDefault();
        if (!acquisitionButton.disabled)
        {
            const action = latestState.acquisitionRunning ? "stop" : "start";
            const body = action === "start" ? new URLSearchParams({ sampleRate: rateInput.value, record: recordInput.checked ? "1" : "0" }) : null;
            refreshStatus(action, body);
        }
    });
    // Only a deliberate confirmed click requests reboot; a poll never does.
    rebootButton.addEventListener("click", () =>
    {
        if (!rebootButton.disabled && window.confirm("Reboot receiver? Acquisition will stop."))
        {
            refreshStatus("reboot");
        }
    });
    // Destructive card management is explicit, stopped-only, and separate from
    // Start. Cancel sends nothing; firmware repeats the state and confirmation checks.
    deleteButton.addEventListener("click", () =>
    {
        if (!deleteButton.disabled && window.confirm("Delete all files in /recordings/ on the SD card? This cannot be undone. Other folders will be kept."))
        {
            refreshStatus("delete", new URLSearchParams({ confirm: "delete-recordings" }));
        }
    });
    // Background status reads never manipulate local DAC controls.
    refreshStatus();
    window.setInterval(() => refreshStatus(), 500);
}
