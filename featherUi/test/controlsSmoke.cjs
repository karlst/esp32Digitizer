// Exercise the Start-applies-settings UI with delayed HTTP and explicit S3 acknowledgements.
const { chromium } = require("playwright");
const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const path = require("node:path");

/** Test controls, failures, confirmed state, and responsive layout without commanding hardware. */
async function runChecks()
{
    const browser = await chromium.launch({ channel: "msedge", headless: true });
    try
    {
        const page = await browser.newPage({ viewport: { width: 1100, height: 1000 } });
        const errors = [];
        const commands = [];
        let offline = false;
        let rejectDac = false;
        let dac = { ready: true, enabled: false, frequencyHz: 10, amplitudeVolts: 0.5,
            offsetVolts: 1.5, minFrequencyHz: 1, maxFrequencyHz: 100, minSignalVolts: 0.2,
            maxSignalVolts: 2.4, sampleRateHz: 2000, missedIntervals: 0, calibration: "Factory eFuse", samples: [] };
        let s3 = { connected: false, commandStatus: "idle", commandId: 0, commandAction: "", lastMessageAgeMs: null };
        page.on("pageerror", (error) => { errors.push(error.message); });
        /** Wait for transport acceptance before injecting a simulated S3 acknowledgement. */
        async function clickS3(selector, action)
        {
            const accepted = page.waitForResponse((response) =>
            {
                return new URL(response.url()).pathname === `/api/s3/${action}`;
            });
            await page.locator(selector).click();
            await accepted;
        }
        // Keep requests slow enough to exercise clicks arriving during background reads.
        await page.route("**/*", async (route) =>
        {
            const request = route.request();
            const pathname = new URL(request.url()).pathname;
            if (pathname.startsWith("/api/"))
            {
                await new Promise((resolve) => { setTimeout(resolve, 150); });
                if (offline)
                {
                    await route.abort("connectionrefused");
                }
                else
                {
                    let response;
                    let status = 200;
                    if (request.method() === "POST")
                    {
                        commands.push({ path: pathname, body: request.postData() });
                    }
                    if (pathname.startsWith("/api/s3"))
                    {
                        if (request.method() === "POST")
                        {
                            s3 = { ...s3, commandStatus: "pending", commandAction: pathname.split("/").pop(), commandId: s3.commandId + 1 };
                            status = 202;
                        }
                        response = s3;
                    }
                    else if (pathname === "/api/reboot")
                    {
                        dac = { ...dac, enabled: false, frequencyHz: 10, amplitudeVolts: 0.5, offsetVolts: 1.5 };
                        response = { rebooting: true };
                        status = 202;
                    }
                    else
                    {
                        if (pathname === "/api/dac/start")
                        {
                            const body = new URLSearchParams(request.postData());
                            if (!rejectDac)
                            {
                                dac = { ...dac, enabled: true, frequencyHz: Number(body.get("frequencyHz")),
                                    amplitudeVolts: Number(body.get("amplitudeVolts")), offsetVolts: Number(body.get("offsetVolts")) };
                            }
                            else
                            {
                                status = 400;
                            }
                        }
                        if (pathname === "/api/dac/stop")
                        {
                            dac.enabled = false;
                        }
                        response = status === 400 ? { error: "Start rejected by device" } : dac;
                    }
                    await route.fulfill({ status, contentType: "application/json", body: JSON.stringify(response) });
                }
            }
            else
            {
                const file = path.join(__dirname, "../data", pathname === "/" ? "index.html" : pathname);
                await route.fulfill({ body: await fs.readFile(file), contentType:
                    { ".html": "text/html", ".js": "text/javascript", ".css": "text/css" }[path.extname(file)] });
            }
        });
        await page.goto("http://feather.test/");
        await page.waitForFunction(() => { return !document.getElementById("dac-button").disabled; });
        assert.equal(await page.locator("#blink-button, #dac-apply").count(), 0);
        assert.equal(await page.locator("#s3-acquisition-button").isDisabled(), true);
        // Preserve strict numeric entry and the three distinct spin increments.
        await page.locator("#dac-frequency").focus();
        await page.locator("#dac-frequency").press("End");
        await page.locator("#dac-frequency").pressSequentially("e+-abc.");
        assert.equal(await page.locator("#dac-frequency").inputValue(), "10");
        await page.getByRole("button", { name: "Increase amplitude by 0.05 V", exact: true }).click();
        assert.equal(await page.locator("#dac-amplitude").inputValue(), "0.55");
        await page.getByRole("button", { name: "Increase offset by 0.1 V", exact: true }).click();
        assert.equal(await page.locator("#dac-offset").inputValue(), "1.6");
        await page.locator("#dac-frequency").press("ArrowUp");
        assert.equal(await page.locator("#dac-frequency").inputValue(), "11");
        await page.locator("#dac-frequency").fill("250");
        assert.equal(await page.locator("#dac-button").isDisabled(), true);
        await page.locator("#dac-frequency").fill("25");
        await page.waitForTimeout(800);
        assert.equal(await page.locator("#dac-frequency").inputValue(), "25");
        // A click during a poll carries all settings and executes exactly once.
        await page.waitForRequest((request) => { return new URL(request.url()).pathname === "/api/dac"; });
        await page.locator("#dac-button").click();
        await page.waitForFunction(() => { return document.getElementById("dac-running").textContent.includes("25 Hz"); });
        assert.equal(commands.filter((command) => { return command.path === "/api/dac/start"; }).length, 1);
        assert.match(commands.at(-1).body, /frequencyHz=25/);
        assert.match(await page.locator("#dac-running").innerText(), /0.55 V peak, 1.60 V offset/);
        assert.equal(await page.locator("#dac-frequency").isDisabled(), true);
        // Stop must ignore even an invalid field forced in by another script.
        await page.locator("#dac-frequency").evaluate((input) => { input.value = "bad"; input.dispatchEvent(new Event("input", { bubbles: true })); });
        await page.locator("#dac-button").click();
        await page.waitForFunction(() => { return document.getElementById("dac-running").textContent === "DAC stopped."; });
        assert.equal(commands.at(-1).path, "/api/dac/stop");
        assert.equal(commands.at(-1).body, null);
        assert.equal(await page.locator("#dac-frequency").isEnabled(), true);
        rejectDac = true;
        await page.locator("#dac-frequency").fill("30");
        await page.locator("#dac-button").click();
        await page.waitForFunction(() => { return document.getElementById("event-log").textContent.includes("Start rejected by device"); });
        assert.equal(dac.enabled, false);
        rejectDac = false;

        // S3 Start remains pending until an explicit device acknowledgement arrives.
        s3 = { ...s3, connected: true, adcReady: true, receiving: false, sampleCount: "0",
            samplesPerSecond: 0, latestRaw: null, errorCode: 0, acquisitionRunning: false, appliedRate: 1000 };
        await page.waitForFunction(() => { return !document.getElementById("s3-acquisition-button").disabled; });
        await page.locator("#s3-sample-rate").selectOption("7500");
        await clickS3("#s3-acquisition-button", "start");
        await page.waitForFunction(() => { return document.getElementById("s3-command-message").textContent.includes("pending"); });
        assert.equal(await page.locator("#s3-running").innerText(), "Acquisition stopped.");
        assert.equal(await page.locator("#s3-acquisition-button").isDisabled(), true);
        assert.match(commands.at(-1).body, /sampleRate=7500/);
        s3 = { ...s3, commandStatus: "confirmed", acquisitionRunning: true, appliedRate: 7500, receiving: true };
        await page.waitForFunction(() => { return document.getElementById("s3-running").textContent.includes("7,500"); });
        assert.equal(await page.locator("#s3-sample-rate").isDisabled(), true);
        await clickS3("#s3-acquisition-button", "stop");
        await page.waitForFunction(() => { return document.getElementById("s3-acquisition-button").disabled; });
        s3 = { ...s3, commandStatus: "confirmed", acquisitionRunning: false, receiving: false };
        await page.waitForFunction(() => { return document.getElementById("s3-running").textContent === "Acquisition stopped."; });
        // Rejection/timeout never turn a requested rate into confirmed running state.
        await clickS3("#s3-acquisition-button", "start");
        s3.commandStatus = "rejected";
        await page.waitForFunction(() => { return document.getElementById("s3-command-message").textContent.includes("rejected"); });
        await clickS3("#s3-acquisition-button", "start");
        s3.commandStatus = "timeout";
        await page.waitForFunction(() => { return document.getElementById("s3-command-message").textContent.includes("timed out"); });
        // A terminal timeout must release both controls. Their appearance must
        // distinguish availability; the DAC remains independent of the S3 command.
        assert.equal(await page.locator("#s3-acquisition-button").isEnabled(), true);
        assert.equal(await page.locator("#dac-button").isEnabled(), true);
        assert.equal(await page.locator("#s3-unavailable").innerText(), "");
        assert.equal(await page.locator("#dac-unavailable").innerText(), "");
        assert.equal(await page.locator("#s3-acquisition-button").evaluate((button) =>
        { return getComputedStyle(button).backgroundColor; }), "rgb(24, 89, 181)");
        page.once("dialog", (dialog) => { dialog.dismiss(); });
        const beforeCancel = commands.length;
        await page.locator("#s3-reboot-button").click();
        assert.equal(commands.length, beforeCancel);
        page.once("dialog", (dialog) => { dialog.accept(); });
        await clickS3("#s3-reboot-button", "reboot");
        s3.commandStatus = "confirmed";
        await page.waitForFunction(() => { return document.getElementById("s3-command-message").textContent.includes("reboot confirmed"); });
        s3.connected = false;
        await page.waitForFunction(() => { return document.getElementById("s3-connected").textContent === "No"; });
        assert.equal(await page.locator("#s3-acquisition-button").isDisabled(), true);
        offline = true;
        await page.waitForFunction(() => { return document.getElementById("status-dac").textContent.includes("Unknown"); });
        offline = false;
        await page.waitForFunction(() => { return document.getElementById("status-system").textContent === "Connected"; });
        await page.screenshot({ path: path.join(__dirname, "../.pio/controls-desktop.png"), fullPage: true });
        await page.setViewportSize({ width: 375, height: 950 });
        await page.screenshot({ path: path.join(__dirname, "../.pio/controls-mobile.png"), fullPage: true });
        assert.equal(await page.evaluate(() => { return document.documentElement.scrollWidth <= innerWidth; }), true);
        assert.deepEqual(errors, []);
        console.log("PASS: strict numeric/spin controls, draft preservation, Start sends settings, Stop ignores drafts, running locks, S3 pending/confirmed/rejected/timeout/reboot, disconnect/recovery, desktop/mobile layout.");
    }
    finally
    {
        await browser.close();
    }
}

// Surface failures to the command runner instead of leaving a browser open.
runChecks().catch((error) =>
{
    console.error(error);
    process.exitCode = 1;
});
