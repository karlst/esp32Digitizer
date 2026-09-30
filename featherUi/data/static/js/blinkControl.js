// Legacy browser LED test. app.js no longer imports or initializes this module,
// and index.html has no blink button. Kept for old pages that still use the routes.
// Its status describes requested blinking, not whether the LED is lit right now.
import { appendEvent } from "./eventLog.js";

/**
 * Connect a legacy blink button only if both button and status elements exist.
 * Poll every two seconds to recover actual firmware state after reload or another
 * client's command. A missing control causes no polling or hardware command.
 */
export function initializeBlinkControl()
{
    // Keep request and displayed state local to this control module.
    const button = document.getElementById("blink-button");
    const status = document.getElementById("status-blink");
    let blinkEnabled = false;
    let stateKnown = false;
    let requestPending = false;
    // Reads leave the button steady. Reserve one command if a click arrives
    // during a poll, then send it as soon as that read has finished.
    let commandPending = false;
    let queuedAction = null;
    let connectionFailed = false;

    /**
     * Read state (null action) or send an explicit start/stop request to Feather.
     * Only one HTTP request runs at a time. A click during a poll reserves the next slot;
     * commands disable the button, but routine polls do not make it blink disabled.
     *
     * Wait for the reply before changing the displayed enabled state. A lost reply
     * makes the state unknown even if the hardware acted; later polls recover it.
     * The five-second browser timeout releases a stuck request, not a running LED.
     */
    async function synchronizeBlink(action = null)
    {
        // Preserve a click during a read without letting overlapping HTTP replies
        // overwrite the result of the newer command with an older poll result.
        if (requestPending && action && !commandPending)
        {
            queuedAction = action;
            commandPending = true;
            button.disabled = true;
        }
        // Polling skips a cycle while a command or previous poll is still pending.
        if (!requestPending)
        {
            requestPending = true;
            commandPending = Boolean(action);
            button.disabled = commandPending || !stateKnown;

            // Bound network waits so a disconnected Feather can later recover.
            const abortController = new AbortController();
            const timeoutId = window.setTimeout(() => abortController.abort(), 5000);
            try
            {
                const path = action ? `/api/blink/${action}` : "/api/blink";
                const response = await fetch(path, {
                    method: action ? "POST" : "GET",
                    cache: "no-store",
                    signal: abortController.signal
                });
                if (!response.ok)
                {
                    throw new Error(`HTTP ${response.status}`);
                }

                // Accept only the expected state shape before enabling commands.
                const state = await response.json();
                if (typeof state.enabled !== "boolean")
                {
                    throw new Error("Invalid blink state response");
                }
                blinkEnabled = state.enabled;
                stateKnown = true;
                button.textContent = blinkEnabled ? "Stop Blink" : "Start Blink";
                status.textContent = blinkEnabled ? "Blinking" : "Stopped";

                // Log commands and connection recovery without logging every poll.
                if (action)
                {
                    appendEvent(blinkEnabled ? "LED blinking started." : "LED blinking stopped.");
                }
                else if (connectionFailed)
                {
                    appendEvent("Blink control connection restored.");
                }
                connectionFailed = false;
            }
            catch (error)
            {
                // A lost response may follow an applied command; resync before retrying.
                stateKnown = false;
                status.textContent = "Unavailable — reconnecting...";
                if (!connectionFailed)
                {
                    appendEvent(`Cannot confirm LED blink state: ${error.message}`);
                }
                connectionFailed = true;
            }
            finally
            {
                // Release the request slot and enable commands only with confirmed state.
                window.clearTimeout(timeoutId);
                requestPending = false;
                commandPending = false;
                // Keep the button disabled across the poll-to-command handoff.
                if (queuedAction)
                {
                    const nextAction = queuedAction;
                    queuedAction = null;
                    synchronizeBlink(nextAction);
                }
                else
                {
                    button.disabled = !stateKnown;
                }
            }
        }
    }

    // Only initialize on pages that contain the complete control.
    if (button && status)
    {
        button.addEventListener("click", () =>
        {
            // Send an explicit desired state rather than a server-side toggle.
            if (stateKnown)
            {
                synchronizeBlink(blinkEnabled ? "stop" : "start");
            }
        });
        synchronizeBlink();
        window.setInterval(() => synchronizeBlink(), 2000);
    }
}
