// Manage messages displayed in the Recent Events panel.

/**
 * Append one plain-text line to Recent Events, if that panel exists.
 * message is displayed literally via textContent, never interpreted as HTML.
 * This is a browser-session log, not a device recording: reload clears it, and
 * this helper does not timestamp, persist, or limit the number of entries.
 * Callers should log meaningful transitions rather than every polling response.
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
