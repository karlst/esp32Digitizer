// Coordinate DAC settings, measurement polling, and confirmed Feather restarts.
import { appendEvent } from "./eventLog.js";
import { drawWaveform } from "./waveformGraph.js";
import { initializeNumericControl, readNumericValue } from "./numericControl.js?v=20260929-spin";

/** Connect controls while preserving unfinished edits across measurement updates. */
export function initializeDacControl()
{
    const form = document.getElementById("dac-form");
    const applyButton = document.getElementById("dac-apply");
    const outputButton = document.getElementById("dac-button");
    const rebootButton = document.getElementById("reboot-button");
    const message = document.getElementById("dac-message");
    const canvas = document.getElementById("dac-graph");
    const graphStatus = document.getElementById("graph-status");
    const frequencyInput = document.getElementById("dac-frequency");
    const amplitudeInput = document.getElementById("dac-amplitude");
    const offsetInput = document.getElementById("dac-offset");
    const validationMessage = document.getElementById("dac-validation");
    let state = null;
    let dirty = false;
    // Draft preservation is separate from numeric dirty state: reverting a field
    // while an earlier command is in flight must survive that command's response.
    let preserveDraft = false;
    let editRevision = 0;
    let requestPending = false;
    // Background reads do not disable controls. A click during a read reserves
    // the next request slot; commandPending prevents a second, conflicting click.
    let commandPending = false;
    let queuedCommand = null;
    let connected = false;
    let connectionFailed = false;
    let rebootUntil = 0;

    /**
     * Explain invalid drafts immediately without changing what the user is typing.
     * Editors allow temporary empty/out-of-range text during editing. Validate
     * all three values and their combined envelope
     * here, and keep this feedback separate from connection/command messages.
     */
    function validateDraft()
    {
        const minFrequency = state?.minFrequencyHz ?? 1;
        const maxFrequency = state?.maxFrequencyHz ?? 100;
        const minSignal = state?.minSignalVolts ?? 0.2;
        const maxSignal = state?.maxSignalVolts ?? 2.4;
        const frequency = readNumericValue(frequencyInput);
        const amplitude = readNumericValue(amplitudeInput);
        const offset = readNumericValue(offsetInput);
        const errors = [];
        const frequencyInvalid = !Number.isInteger(frequency) || frequency < minFrequency || frequency > maxFrequency;
        const amplitudeInvalid = !Number.isFinite(amplitude) || amplitude < 0;
        const offsetInvalid = !Number.isFinite(offset) || offset < minSignal || offset > maxSignal;
        // Field-specific explanations identify which value must change. The envelope
        // check follows only when its operands are individually valid numbers.
        if (frequencyInvalid)
        {
            errors.push(`Frequency must be ${minFrequency}–${maxFrequency} Hz (whole numbers).`);
        }
        if (amplitudeInvalid)
        {
            errors.push("Amplitude must be a number at least 0 V peak.");
        }
        if (offsetInvalid)
        {
            errors.push(`Offset must be ${minSignal}–${maxSignal} V.`);
        }
        const envelopeInvalid = !amplitudeInvalid && !offsetInvalid &&
            (offset - amplitude < minSignal - 0.000001 || offset + amplitude > maxSignal + 0.000001);
        if (envelopeInvalid)
        {
            errors.push(`Offset ± amplitude must stay within ${minSignal}–${maxSignal} V.`);
        }
        frequencyInput.setAttribute("aria-invalid", String(frequencyInvalid));
        amplitudeInput.setAttribute("aria-invalid", String(amplitudeInvalid || envelopeInvalid));
        offsetInput.setAttribute("aria-invalid", String(offsetInvalid || envelopeInvalid));
        // Keep assistive-technology spinbutton values synchronized with drafts.
        [frequencyInput, amplitudeInput, offsetInput].forEach((input) =>
        {
            const value = readNumericValue(input);
            if (Number.isFinite(value))
            {
                input.setAttribute("aria-valuenow", value);
            }
            else
            {
                input.removeAttribute("aria-valuenow");
            }
        });
        validationMessage.textContent = errors.join(" ");
        const retVal = errors.length === 0;
        return retVal;
    }

    /** Enable Apply only for a valid draft and explain every unavailable state. */
    function updateButtons()
    {
        const unavailable = commandPending || !connected || Date.now() < rebootUntil;
        const draftValid = validateDraft();
        // Compare numeric values, not input events or spelling: 0.50 equals 0.5,
        // and editing back to the applied value clears dirty immediately.
        dirty = Boolean(state?.ready) && (
            readNumericValue(frequencyInput) !== state.frequencyHz ||
            readNumericValue(amplitudeInput) !== state.amplitudeVolts ||
            readNumericValue(offsetInput) !== state.offsetVolts);
        const applyDisabled = unavailable || !state?.ready || !draftValid || !dirty;
        if (applyButton.disabled !== applyDisabled)
        {
            applyButton.disabled = applyDisabled;
        }
        outputButton.disabled = unavailable || !state?.ready;
        rebootButton.disabled = unavailable;
        // A disabled button must have a visible reason, not just a dim appearance.
        applyButton.title = commandPending ? "Waiting for the current command to finish." :
            !connected ? "Waiting for a connection to the Feather." :
            !state?.ready ? "DAC initialization failed." :
            !draftValid ? validationMessage.textContent :
            !dirty ? "Settings match the Feather; no changes to apply." : "Send these settings to the Feather.";
    }

    /** Show validation and command outcomes next to the controls. */
    function showMessage(text, error = false)
    {
        message.textContent = text;
        message.dataset.error = String(error);
    }

    /** Render confirmed settings without overwriting an unfinished settings edit. */
    function renderState()
    {
        document.getElementById("status-system").textContent = "Connected";
        document.getElementById("status-dac").textContent = state.ready ?
            (state.enabled ? "Enabled" : "Disabled (0 V commanded)") : "Initialization failed";
        outputButton.textContent = state.ready && state.enabled ? "Stop DAC" : "Start DAC";
        if (state.ready)
        {
            // Limits come from the firmware so its maximum-frequency constant is authoritative.
            frequencyInput.setAttribute("aria-valuemin", state.minFrequencyHz);
            frequencyInput.setAttribute("aria-valuemax", state.maxFrequencyHz);
            offsetInput.setAttribute("aria-valuemin", state.minSignalVolts);
            offsetInput.setAttribute("aria-valuemax", state.maxSignalVolts);
            amplitudeInput.setAttribute("aria-valuemax", Number(((state.maxSignalVolts - state.minSignalVolts) / 2).toFixed(6)));
            document.getElementById("dac-limits").textContent =
                `Output range (offset ± amplitude): ${state.minSignalVolts}–${state.maxSignalVolts} V. Frequency: ${state.minFrequencyHz}–${state.maxFrequencyHz} Hz.`;
            if (!preserveDraft)
            {
                frequencyInput.value = state.frequencyHz;
                amplitudeInput.value = state.amplitudeVolts;
                offsetInput.value = state.offsetVolts;
            }
            document.getElementById("status-sine").textContent =
                `${state.frequencyHz} Hz / ${state.amplitudeVolts} V peak / ${state.offsetVolts} V offset`;
            document.getElementById("status-sampling").textContent =
                `${state.sampleRateHz} Hz nominal; ${state.missedIntervals} missed intervals`;
            document.getElementById("status-calibration").textContent = state.calibration;
            graphStatus.textContent = state.samples.length ?
                "Live A1 → A2 loopback • latest two periods" : "Collecting samples...";
        }
        else
        {
            graphStatus.textContent = "DAC/ADC unavailable.";
            showMessage("DAC/ADC initialization failed. Try rebooting the Feather.", true);
        }
        drawWaveform(canvas, state.ready ? state : null);
    }

    /**
     * Keep one HTTP request in flight, giving a user command the next slot after a poll.
     * Polling preserves the last confirmed button state; only a command, reboot,
     * or failed connection disables controls. Capture the edit revision at click
     * time so edits made while a command is queued are not overwritten afterward.
     */
    async function synchronize(action = null, body = null, requestedRevision = editRevision)
    {
        // A poll may already be reading the device when a still-enabled button is
        // clicked. Queue that command instead of dropping it or racing two responses.
        if (requestPending && action && !commandPending)
        {
            queuedCommand = { action, body, requestedRevision };
            commandPending = true;
            updateButtons();
        }
        if (!requestPending && Date.now() >= rebootUntil)
        {
            requestPending = true;
            commandPending = Boolean(action);
            updateButtons();
            const sentRevision = requestedRevision;
            const abortController = new AbortController();
            const timeoutId = window.setTimeout(() => abortController.abort(), 5000);
            try
            {
                // Explicit start/stop commands remain safe if a response is lost.
                const path = action === "reboot" ? "/api/reboot" :
                    action ? `/api/dac/${action}` : "/api/dac";
                const response = await fetch(path, {
                    method: action ? "POST" : "GET",
                    body,
                    cache: "no-store",
                    signal: abortController.signal
                });
                const result = await response.json();
                if (!response.ok)
                {
                    throw new Error(result.error || `HTTP ${response.status}`);
                }
                if (action === "reboot")
                {
                    // Pause polling through the reboot, then resynchronize without enabling output.
                    rebootUntil = Date.now() + 3000;
                    connected = false;
                    state = null;
                    document.getElementById("status-system").textContent = "Rebooting...";
                    document.getElementById("status-dac").textContent = "Waiting for restart";
                    graphStatus.textContent = "Rebooting — waiting for reconnection...";
                    drawWaveform(canvas, null);
                    showMessage("Reboot requested. Reconnect to ESP32-Digitizer if needed.");
                    appendEvent("Feather reboot requested.");
                }
                else
                {
                    // Reject incomplete data before graphing or enabling hardware commands.
                    if (typeof result.ready !== "boolean" || (result.ready &&
                        (typeof result.enabled !== "boolean" || !Array.isArray(result.samples) ||
                        !Number.isFinite(result.frequencyHz) || result.frequencyHz <= 0)))
                    {
                        throw new Error("Invalid DAC state response");
                    }
                    state = result;
                    connected = true;
                    if (action === "settings" && sentRevision === editRevision)
                    {
                        dirty = false;
                        preserveDraft = false;
                    }
                    if (action)
                    {
                        const text = action === "settings" ? "DAC settings applied." :
                            action === "start" ? "DAC output started." : "DAC output stopped.";
                        showMessage(text);
                        appendEvent(text);
                    }
                    else if (connectionFailed || message.textContent === "Connecting..." || rebootUntil)
                    {
                        showMessage(dirty ? "Connected. Unapplied edits preserved." : "Connected. Settings synchronized.");
                    }
                    if (connectionFailed || rebootUntil)
                    {
                        appendEvent("Feather connection restored.");
                    }
                    rebootUntil = 0;
                    connectionFailed = false;
                    renderState();
                }
            }
            catch (error)
            {
                // Never imply that a lost command response confirms the physical output state.
                connected = false;
                document.getElementById("status-system").textContent = "Reconnecting...";
                document.getElementById("status-dac").textContent = "Unknown — waiting for Feather";
                graphStatus.textContent = "Connection unavailable — graph paused (last received data).";
                showMessage(error.message, true);
                if (!connectionFailed || action)
                {
                    appendEvent(`DAC request failed: ${error.message}`);
                }
                connectionFailed = true;
            }
            finally
            {
                window.clearTimeout(timeoutId);
                requestPending = false;
                commandPending = false;
                // Hand the slot directly to the waiting command. Do not briefly
                // re-enable buttons between the poll and that command's response.
                if (queuedCommand)
                {
                    const nextCommand = queuedCommand;
                    queuedCommand = null;
                    synchronize(nextCommand.action, nextCommand.body, nextCommand.requestedRevision);
                }
                else
                {
                    updateButtons();
                }
            }
        }
    }

    // Register field filtering before the bubbling form handler reads the values.
    [frequencyInput, amplitudeInput, offsetInput].forEach(initializeNumericControl);
    // Mark drafts explicitly so recurring graph polls cannot undo keyboard edits.
    form.addEventListener("input", () =>
    {
        editRevision += 1;
        updateButtons();
        preserveDraft = dirty || commandPending || !connected;
        showMessage(dirty ? "Unapplied settings. Select Apply Settings to send them." : "Settings match the Feather.");
    });
    form.addEventListener("submit", (event) =>
    {
        event.preventDefault();
        if (state?.ready && connected && !commandPending && dirty)
        {
            // Recheck on submission too, covering Enter and programmatic submission.
            // The inline validator replaces browser popups that can block submit
            // before our handler has an opportunity to explain the problem.
            if (validateDraft())
            {
                synchronize("settings", new URLSearchParams(new FormData(form)));
            }
            else
            {
                updateButtons();
            }
        }
    });
    outputButton.addEventListener("click", () =>
    {
        // Start uses the last applied settings; pending edits never silently reach hardware.
        synchronize(state.enabled ? "stop" : "start");
    });
    rebootButton.addEventListener("click", () =>
    {
        if (window.confirm("Reboot the Feather? Output will stop and settings will reset to their startup defaults."))
        {
            synchronize("reboot");
        }
    });
    const resizeObserver = new ResizeObserver(() =>
    {
        drawWaveform(canvas, state?.ready ? state : null);
    });
    resizeObserver.observe(canvas);
    synchronize();
    window.setInterval(() => synchronize(), 500);
}
