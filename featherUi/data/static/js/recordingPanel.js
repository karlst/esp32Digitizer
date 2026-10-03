// P3 renders validated S3 recording telemetry. It never controls the card itself.
// UART byte totals arrive as decimal strings; BigInt preserves their exact value.
// History is a bounded browser-session display, not a substitute for recorded data.

/**
 * Format bytes with a readable unit while retaining exact bytes in the main total.
 * value is a decimal-string byte count supplied by S3. BigInt keeps every
 * integer bit; converting the original total to Number could round large counts.
 * Division here truncates to one decimal place. KiB/MiB/GiB mean powers of 1024,
 * not disk manufacturers' decimal KB/MB/GB. This is display formatting only.
 */
function byteSize(value)
{
    const bytes = BigInt(value);
    const units = [[1073741824n, "GiB"], [1048576n, "MiB"], [1024n, "KiB"]];
    const unit = units.find(([size]) => bytes >= size);
    return unit ? `${bytes / unit[0]}.${(bytes % unit[0]) * 10n / unit[0]} ${unit[1]}` : `${bytes} B`;
}

/**
 * Format a recording-relative duration; no synchronization of board clocks is needed.
 * value is elapsed milliseconds measured on S3, passed as a decimal string.
 * Drop fractional seconds, split into hours/minutes/seconds, and zero-pad the last
 * two components. Hours can exceed 24; this is a duration, not a time of day.
 */
function elapsed(value)
{
    const seconds = BigInt(value) / 1000n;
    return `${seconds / 3600n}:${String(seconds / 60n % 60n).padStart(2, "0")}:${String(seconds % 60n).padStart(2, "0")}`;
}

/**
 * Attach P3 once and return a renderer called after each successful status poll.
 * The main S3 UI module calls this once, then calls the returned render function
 * with each HTTP status snapshot. The closure retains chart history between calls.
 * No sample payload arrives here: this page receives counters/positions/durations,
 * not the digitizer's 30000 samples per second. P3 is a monitor, not a recorder.
 */
export function createRecordingPanel()
{
    const get = (id) => document.getElementById(id);
    // Keep at most 240 observations and two minutes of time. This is a small
    // display history in this browser tab; reloading discards it.
    const history = [];
    const maxima = [];
    const canvas = get("recording-history");
    let session = null;
    let lastKey = null;
    let previousMaxDelay = 0;

    /**
     * Draw occupancy and write-delay history with separate explicitly labelled scales.
     * The newest status time is at the right; 120000 ms (two minutes) fills the width.
     * Blue uses a fixed 0-100 percent scale for RAM occupancy. Orange uses the displayed
     * millisecond delay scale, which expands to fit the visible observations/maxima.
     * Canvas pixel coordinates increase downward, hence subtracting from 78 for y.
     * Resizing resets the canvas, so rebuild the drawing from retained observations.
     */
    function draw()
    {
        // Use CSS pixels for layout but more backing pixels on high-density screens
        // so the small chart remains sharp. Avoid a zero-sized drawing surface.
        const width = Math.max(1, canvas.clientWidth);
        canvas.width = width * window.devicePixelRatio;
        canvas.height = 80 * window.devicePixelRatio;
        const ctx = canvas.getContext("2d");
        ctx.scale(window.devicePixelRatio, window.devicePixelRatio);
        ctx.clearRect(0, 0, width, 80);
        // A minimum 1-ms scale avoids division by zero when no delays are measured.
        // Maximum markers may exceed the latest per-report delay, so include both.
        const maxDelay = Math.max(1, ...history.map((point) => point.delay), ...maxima.map((point) => point.delay));
        get("rec-chart-scale").textContent = `· delay scale 0–${maxDelay.toFixed(1)} ms`;
        ctx.strokeStyle = "#d6dde5";
        ctx.strokeRect(0.5, 0.5, width - 1, 79);
        for (const [key, color, scale] of [["fill", "#1859b5", 100], ["delay", "#b74e0b", maxDelay]])
        {
            ctx.beginPath();
            ctx.strokeStyle = color;
            history.forEach((point, index) =>
            {
                const x = width - (history[history.length - 1].time - point.time) / 120000 * width;
                const y = 78 - point[key] / scale * 76;
                if (index) { ctx.lineTo(x, y); } else { ctx.moveTo(x, y); }
            });
            ctx.stroke();
        }
        // S3 timestamps maximum delays when they happen, not when the browser
        // receives them. Mark those times explicitly, without inventing a buffer
        // occupancy measurement at the earlier instant.
        if (history.length)
        {
            const now = history[history.length - 1].time;
            ctx.fillStyle = "#b74e0b";
            for (const point of maxima)
            {
                const x = width - (now - point.time) / 120000 * width;
                ctx.beginPath();
                ctx.arc(x, 78 - point.delay / maxDelay * 76, 2.5, 0, Math.PI * 2);
                ctx.fill();
            }
        }
    }

    /**
     * Render one coherent snapshot; clear live claims when either connection fails.
     * state is the browser's parsed S3 status object. It may be missing or stale if
     * a link is lost. Hide live values in that case rather than showing zero losses or
     * an empty buffer as if they were measured. The history arrays remain in memory.
     * On valid status, replace labels and geometry, add a changed observation to the
     * chart, then redraw. A displayed Saved state is S3's report, not a browser guess.
     */
    return function render(state)
    {
        const rec = state?.connected ? state.recording : null;
        const message = get("recording-message");
        const ids = ["state", "file", "card", "bytes", "samples", "rate", "delay", "max-delay", "open-time", "buffer", "pointers", "errors"];
        if (!rec)
        {
            message.textContent = state?.connected ? "Recording unavailable — S3 update required." : "Recording state unknown — disconnected.";
            for (const id of ids) { get(`rec-${id}`).textContent = "—"; }
            get("rec-fill").style.width = "0%";
            get("rec-wrap").style.width = "0%";
            get("rec-meter").removeAttribute("aria-valuenow");
            canvas.hidden = true;
            get("rec-read-marker").hidden = true;
            get("rec-write-marker").hidden = true;
        }
        else
        {
            // Firmware validates every field. Check again before BigInt conversion
            // so a malformed HTTP response cannot leave stale numbers on screen.
            if (Object.values(rec).some((value) => typeof value !== "string" || !/^\d+$/.test(value)))
            {
                throw new Error("Invalid recording status");
            }
            const labels = ["Idle", "Preparing…", "Recording", "Saving…", "Saved", "Incomplete — error", "Deleting…"];
            const cardLabels = ["Unknown", "Ready", "No card", "Unsupported filesystem", "Card I/O error"];
            const active = [1, 2, 3, 6].includes(Number(rec.state));
            message.textContent = rec.state === "6" ? `Deleting recordings — ${rec.deletedFiles} files removed.` :
                state.acquisitionRunning && rec.enabled === "0" ? "Not recording. Showing last recording, if any." :
                rec.state === "5" ? "Recording incomplete. Check the error counters." :
                active ? "Current recording" : rec.session !== "0" ? "Last recording" : "Ready for the first recording.";
            get("rec-state").textContent = `${labels[Number(rec.state)]} / ${elapsed(rec.elapsedMs)}`;
            get("rec-file").textContent = rec.session === "0" ? "—" : `recording_${rec.session.padStart(6, "0")}_${rec.part.padStart(3, "0")}.bin`;
            // Calculate a bounded percentage AFTER exact integer arithmetic. Converting
            // that small scaled result to Number is safe for chart/display purposes.
            const cardPercent = BigInt(rec.cardBytes) ? Number((BigInt(rec.cardBytes) - BigInt(rec.freeBytes)) * 1000n / BigInt(rec.cardBytes)) / 10 : 0;
            // Label estimates and stale scans explicitly; old firmware is unknown.
            const spaceLabel = ["last reported free", "measured free", "estimated free", "stale free"][Number(rec.freeSpaceMode || 0)];
            get("rec-card").textContent = `${cardLabels[Number(rec.card)]} · ${cardPercent}% used · ${byteSize(rec.freeBytes)} ${spaceLabel}`;
            get("rec-open-time").textContent = Number(rec.fileOpenUs || 0) ? `${(Number(rec.fileOpenUs) / 1000).toFixed(3)} ms` : "—";
            get("rec-bytes").textContent = `${BigInt(rec.bytesWritten).toLocaleString("en-US")} bytes (${byteSize(rec.bytesWritten)})`;
            get("rec-samples").textContent = BigInt(rec.samplesWritten).toLocaleString("en-US");
            get("rec-rate").textContent = `${byteSize(rec.bytesPerSecond)}/s`;
            get("rec-delay").textContent = `${(Number(rec.latestDelayUs) / 1000).toFixed(3)} ms`;
            const kind = ["none", "write", "flush"][Number(rec.maxDelayKind)];
            get("rec-max-delay").textContent = `${(Number(rec.maxDelayUs) / 1000).toFixed(3)} ms · ${kind} · at ${elapsed(rec.maxDelayAtMs)}`;
            // These values were restricted to 32-bit positions by Feather validation,
            // so Number can represent them exactly. They index the ring, not a disk file.
            const capacity = Number(rec.bufferBytes);
            const fill = capacity ? Number(rec.usedBytes) * 100 / capacity : 0;
            const peak = capacity ? Number(rec.peakBytes) * 100 / capacity : 0;
            // Shade the actual unread region, splitting at the end of the ring.
            // A full ring shades all positions even when read and write are equal.
            const readOffset = Number(rec.readPosition);
            // Example: 12 queued bytes starting four bytes before the array end become
            // a four-byte segment on the right plus an eight-byte segment on the left.
            const first = Math.min(Number(rec.usedBytes), capacity - readOffset);
            get("rec-fill").style.left = `${capacity ? readOffset * 100 / capacity : 0}%`;
            get("rec-fill").style.width = `${capacity ? first * 100 / capacity : 0}%`;
            get("rec-wrap").style.width = `${capacity ? (Number(rec.usedBytes) - first) * 100 / capacity : 0}%`;
            get("rec-meter").setAttribute("aria-valuenow", fill.toFixed(1));
            get("rec-buffer").textContent = `Waiting: ${byteSize(rec.usedBytes)} / ${byteSize(rec.bufferBytes)} · ${fill.toFixed(1)}% · Peak: ${peak.toFixed(1)}%`;
            get("rec-pointers").textContent = `Write: ${rec.writePosition} B · Read: ${rec.readPosition} B (wrap at ${rec.bufferBytes} B)`;
            for (const [id, position] of [["rec-write-marker", rec.writePosition], ["rec-read-marker", rec.readPosition]])
            {
                get(id).hidden = !capacity;
                get(id).style.left = `${capacity ? Number(position) * 100 / capacity : 0}%`;
            }
            get("rec-errors").textContent = `Overflows: ${rec.overflows} · Samples lost: ${rec.lostSamples} · Write errors: ${rec.writeErrors}`;
            get("rec-errors").classList.toggle("has-errors", [rec.overflows, rec.lostSamples, rec.writeErrors].some((v) => BigInt(v) > 0n));
            // Sample only changed telemetry, not duplicate HTTP polls. Reset for
            // a new session; keep the last chart after Stop. Store fullness with
            // each point for comparison, even though its main display is above.
            if (session !== rec.session)
            {
                history.length = 0; maxima.length = 0; lastKey = null; previousMaxDelay = 0; session = rec.session;
            }
            // HTTP may poll the same UART report more than once. This key avoids
            // drawing duplicate points just because the browser polled again.
            const key = `${rec.elapsedMs}:${rec.bytesWritten}:${rec.latestDelayUs}:${rec.usedBytes}`;
            if (key !== lastKey && rec.session !== "0")
            {
                const time = Number(rec.elapsedMs);
                // Include a newly reported maximum even if the last write was fast.
                // Lesser stalls between reports cannot all be reconstructed here.
                const maximum = Number(rec.maxDelayUs);
                if (maximum > previousMaxDelay)
                {
                    maxima.push({ time: Number(rec.maxDelayAtMs), delay: maximum / 1000 });
                }
                history.push({ time, fill, delay: Number(rec.latestDelayUs) / 1000, cardPercent });
                previousMaxDelay = maximum;
                // Discard old observations to bound browser memory. The chart cannot recover
                // all short stalls between reports; S3 separately preserves peak/max values.
                while (history.length > 240 || (history.length && time - history[0].time > 120000)) { history.shift(); }
                while (maxima.length > 240 || (maxima.length && time - maxima[0].time > 120000)) { maxima.shift(); }
                lastKey = key;
            }
            canvas.hidden = false;
            draw();
        }
    };
}
