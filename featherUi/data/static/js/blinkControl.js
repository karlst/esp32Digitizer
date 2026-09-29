// Synchronize the blink control with the Feather's authoritative command state.
import { appendEvent } from "./eventLog.js";

/**
 * Connect the button and periodically refresh state for reloads and other clients.
 */
export function initializeBlinkControl()
{
    // Keep request and displayed state local to this control module.
    const button = document.getElementById("blink-button");
    const status = document.getElementById("status-blink");
    let blinkEnabled = false;
    let stateKnown = false;
    let requestPending = false;
    let connectionFailed = false;

    /**
     * Fetch or change blink state without overlapping requests or optimistic labels.
     */
    async function synchronizeBlink(action = null)
    {
        // Polling skips a cycle while a command or previous poll is still pending.
        if (!requestPending)
        {
            requestPending = true;
            button.disabled = true;

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
                button.disabled = !stateKnown;
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
