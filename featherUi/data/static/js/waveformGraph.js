// Draw Feather's local output/loopback graph using the browser's canvas API.
// No synthetic sine is generated here. Each firmware record contains elapsed ms,
// estimated commanded DAC volts, and measured ADC volts. These are Feather A1/A2
// measurements, independent of the S3 digitizer acquisition and its sample rate.

/**
 * Draw commanded and measured voltage against time within the latest two cycles.
 * canvas is the HTML drawing area; state is a validated DAC reply or null to show
 * empty axes. Records are [milliseconds, commanded volts, measured volts].
 *
 * Use a fixed 0-3.3 V vertical scale so amplitude changes remain visually comparable.
 * The time span is 2000/frequencyHz milliseconds (two periods). Device timestamps,
 * not the browser polling interval, position samples. Adjacent points are joined
 * for display; the lines are not extra measured samples between those points.
 */
export function drawWaveform(canvas, state)
{
    // Match the backing pixels to the displayed size for sharp labels after resizing.
    const width = Math.max(180, canvas.clientWidth);
    const height = Math.max(80, canvas.clientHeight);
    const pixelRatio = window.devicePixelRatio || 1;
    canvas.width = Math.round(width * pixelRatio);
    canvas.height = Math.round(height * pixelRatio);
    const context = canvas.getContext("2d");
    context.scale(pixelRatio, pixelRatio);
    context.clearRect(0, 0, width, height);
    const left = 34;
    const top = 10;
    const plotWidth = width - left - 12;
    const plotHeight = height - top - 28;
    // Keep the latest two periods by shifting the window origin as data advances.
    // Empty state uses a 200 ms window so the graph still has useful axes.
    const durationMs = state ? 2000 / state.frequencyHz : 200;
    const samples = state ? state.samples : [];
    const endMs = samples.length ? samples[samples.length - 1][0] : durationMs;
    const startMs = Math.max(0, endMs - durationMs);

    // Fixed 0–3.3 V scaling makes amplitude and offset changes directly comparable.
    context.font = "11px Arial";
    context.lineWidth = 1;
    for (let tick = 0; tick <= 3; tick += 1)
    {
        const y = top + plotHeight * (1 - tick / 3.3);
        context.strokeStyle = "#e2e2e2";
        context.beginPath();
        context.moveTo(left, y);
        context.lineTo(left + plotWidth, y);
        context.stroke();
        context.fillStyle = "#555";
        context.fillText(`${tick} V`, 3, y + 4);
    }
    // Horizontal labels show time relative to this visible window, not wall time.
    for (let tick = 0; tick <= 4; tick += 1)
    {
        const x = left + plotWidth * tick / 4;
        context.fillStyle = "#555";
        context.textAlign = tick === 0 ? "left" : tick === 4 ? "right" : "center";
        context.fillText(`${(durationMs * tick / 4).toFixed(durationMs < 100 ? 1 : 0)}`, x, height - 14);
    }
    context.textAlign = "center";
    context.fillText("Time within window (ms)", left + plotWidth / 2, height - 1);

    // Limit drawing to the plot rectangle. Samples outside the visible window or
    // voltage scale remain in the data but are not painted over axis labels.
    context.save();
    context.beginPath();
    context.rect(left, top, plotWidth, plotHeight);
    context.clip();
    // Record columns 1 and 2 select commanded and measured voltage respectively.
    // Solid blue and dashed orange distinguish them without changing their data.
    for (let trace = 1; trace <= 2; trace += 1)
    {
        context.strokeStyle = trace === 1 ? "#1859b5" : "#b74e0b";
        context.setLineDash(trace === 1 ? [] : [5, 3]);
        context.lineWidth = 1.6;
        context.beginPath();
        samples.forEach((sample, index) =>
        {
            // Both values share the ADC capture timestamp supplied by the firmware.
            const x = left + (sample[0] - startMs) / durationMs * plotWidth;
            const y = top + (1 - sample[trace] / 3.3) * plotHeight;
            if (index === 0)
            {
                context.moveTo(x, y);
            }
            else
            {
                context.lineTo(x, y);
            }
        });
        context.stroke();
    }
    context.restore();
}
