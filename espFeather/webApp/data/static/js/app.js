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

// Browser entry point loaded by index.html as a module after the page is parsed.
// Start the independent DAC and S3 controllers; each owns its own request queue
// and polling timer. S3 failures therefore do not disable local DAC controls.
// Query suffixes identify asset revisions so browsers fetch changed modules after
// a web-files upload. The legacy blink module is deliberately not initialized.
import { appendEvent } from "./eventLog.js";
import { initializeDacControl } from "./dacControl.js?v=20260930-counters";
import { initializeS3Monitor } from "./s3Monitor.js?v=20260930-counters";

// Module scripts run after the document has been parsed, so the log is ready.
appendEvent("Browser initialized.");
initializeDacControl();
initializeS3Monitor();
