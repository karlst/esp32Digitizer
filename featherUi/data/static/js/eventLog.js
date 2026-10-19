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
