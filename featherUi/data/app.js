document.addEventListener("DOMContentLoaded", () =>
{
    const eventLog = document.getElementById("event-log");

    if (eventLog)
    {
        const eventLine = document.createElement("div");
        eventLine.textContent = "Browser initialized.";
        eventLog.appendChild(eventLine);
    }
});
