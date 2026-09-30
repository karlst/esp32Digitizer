// Display acquisition evidence reported by S3; never invent sample activity in the browser.
import { appendEvent } from "./eventLog.js";

/**
 * Poll Feather's validated UART snapshot once per second without overlapping requests.
 * Firmware evaluates S3 freshness. HTTP failure is separate: if the browser loses
 * Feather, it cannot know whether S3 is still connected. Clear readings in both cases.
 */
export function initializeS3Monitor()
{
    const connected = document.getElementById("s3-connected");
    const adc = document.getElementById("s3-adc");
    const receiving = document.getElementById("s3-receiving");
    const count = document.getElementById("s3-count");
    const rate = document.getElementById("s3-rate");
    const raw = document.getElementById("s3-raw");
    const error = document.getElementById("s3-error");
    const message = document.getElementById("s3-message");
    const form = document.getElementById("s3-form");
    const rateInput = document.getElementById("s3-sample-rate");
    const acquisitionButton = document.getElementById("s3-acquisition-button");
    const rebootButton = document.getElementById("s3-reboot-button");
    const runningMessage = document.getElementById("s3-running");
    const commandMessage = document.getElementById("s3-command-message");
    const unavailableMessage = document.getElementById("s3-unavailable");
    let latestState = null;
    let commandBusy = false;
    let queuedCommand = null;
    let rateDirty = false;
    let lastOutcome = "";
    let lastLinkState = null;
    let requestPending = false;
    let browserOffline = false;

    /** Preserve editable stopped drafts, but display only device-confirmed running state. */
    function updateControls()
    {
        const available = latestState?.connected && !browserOffline;
        const pending = commandBusy || latestState?.commandStatus === "pending";
        const running = available && latestState.acquisitionRunning;
        acquisitionButton.disabled = !available || pending || (!running && !latestState.adcReady);
        unavailableMessage.textContent = browserOffline ? "Controls unavailable: cannot reach Feather." :
            !latestState?.connected ? "Controls unavailable: S3 is disconnected." :
            pending ? "Controls unavailable: waiting for S3 command confirmation." :
            !running && !latestState.adcReady ? "Start unavailable: digitizer is not ready." : "";
        rebootButton.disabled = !available || pending;
        rateInput.disabled = !available || pending || running;
        acquisitionButton.textContent = running ? "Stop Acquisition" : "Start Acquisition";
        runningMessage.textContent = !available ? "Acquisition state unknown — S3 disconnected." : running ?
            `Acquisition started — ${latestState.appliedRate.toLocaleString("en-US")} samples/s` : "Acquisition stopped.";
        if (available && (running || !rateDirty))
        {
            rateInput.value = String(latestState.appliedRate);
        }
        if (pending)
        {
            commandMessage.textContent = "Command pending — waiting for S3 confirmation...";
        }
        else if (!available)
        {
            commandMessage.textContent = "Waiting for S3 connection.";
        }
        else if (latestState.commandStatus === "idle" && !rateDirty)
        {
            commandMessage.textContent = "Settings take effect on Start Acquisition.";
        }
    }

    /** Clear measurements when their freshness can no longer be established. */
    function clearMeasurements()
    {
        adc.textContent = "Unknown";
        receiving.textContent = "Unknown";
        count.textContent = "—";
        rate.textContent = "—";
        raw.textContent = "—";
        error.textContent = "Unknown";
    }

    /** Read and render one telemetry snapshot; log transitions rather than every poll. */
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
            const abortController = new AbortController();
            const timeoutId = window.setTimeout(() => abortController.abort(), 3000);
            try
            {
                const response = await fetch(action ? `/api/s3/${action}` : "/api/s3", {
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
                    throw new Error("Invalid S3 status response");
                }
                connected.textContent = state.connected ? "Yes" : "No";
                latestState = state;
                if (state.connected)
                {
                    adc.textContent = state.adcReady ? "Yes" : "No";
                    receiving.textContent = state.receiving ? "Yes" : "No";
                    // Keep the 64-bit count as text; conversion to Number can lose digits.
                    count.textContent = state.sampleCount;
                    rate.textContent = String(state.samplesPerSecond);
                    raw.textContent = state.latestRaw === null ? "—" : String(state.latestRaw);
                    const errorNames = ["None", "ADC initialization failed", "ADC data-ready timeout", "ADC read/configuration error"];
                    error.textContent = errorNames[state.errorCode] ?? `Code ${state.errorCode}`;
                    message.textContent = state.receiving ? "S3 is reading digitizer samples." :
                        "S3 is reporting; waiting for fresh digitizer samples.";
                }
                else
                {
                    clearMeasurements();
                    message.textContent = state.lastMessageAgeMs === null ? "Waiting for S3 messages." :
                        "S3 messages stopped; measurements are unknown.";
                }
                // Firmware uses conversion-count progress, not raw-value changes: a
                // constant analog voltage still produces valid successive conversions.
                if (lastLinkState !== state.connected && (lastLinkState !== null || state.connected))
                {
                    appendEvent(state.connected ? "S3 connected." : "S3 connection lost.");
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
                        (!state.connected ? "Last S3 command confirmed; current state unknown." :
                        state.commandAction === "start" ? `Acquisition started — ${state.appliedRate.toLocaleString("en-US")} samples/s` :
                        state.commandAction === "stop" ? "Acquisition stopped." : "S3 reboot confirmed.") :
                        state.commandStatus === "timeout" ? "S3 command timed out — outcome unconfirmed." : "S3 rejected the command.";
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
    form.addEventListener("submit", (event) =>
    {
        event.preventDefault();
        if (!acquisitionButton.disabled)
        {
            const action = latestState.acquisitionRunning ? "stop" : "start";
            const body = action === "start" ? new URLSearchParams({ sampleRate: rateInput.value }) : null;
            refreshStatus(action, body);
        }
    });
    rebootButton.addEventListener("click", () =>
    {
        if (!rebootButton.disabled && window.confirm("Reboot S3? Acquisition will stop."))
        {
            refreshStatus("reboot");
        }
    });
    // Background status reads never manipulate local DAC controls.
    refreshStatus();
    window.setInterval(() => refreshStatus(), 500);
}
