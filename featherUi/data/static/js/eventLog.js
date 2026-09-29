// Manage messages displayed in the Recent Events panel.

/**
 * Append a plain-text message when the event log is present.
 */
export function appendEvent(message)
{
    // Look up the panel and insert text safely without interpreting HTML.
    const eventLog = document.getElementById("event-log");

    if (eventLog)
    {
        const eventLine = document.createElement("div");
        eventLine.textContent = message;
        eventLog.appendChild(eventLine);
    }
}
