// Load the UI modules through this browser entry point.
import { appendEvent } from "./eventLog.js";
import { initializeDacControl } from "./dacControl.js?v=20260929-button-state";
import { initializeS3Monitor } from "./s3Monitor.js?v=20260929-button-state";

// Module scripts run after the document has been parsed, so the log is ready.
appendEvent("Browser initialized.");
initializeDacControl();
initializeS3Monitor();
