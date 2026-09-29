// Load the UI modules through this browser entry point.
import { appendEvent } from "./eventLog.js";
import { initializeBlinkControl } from "./blinkControl.js?v=20260929-spin";
import { initializeDacControl } from "./dacControl.js?v=20260929-spin";

// Module scripts run after the document has been parsed, so the log is ready.
appendEvent("Browser initialized.");
initializeBlinkControl();
initializeDacControl();
