// Browser integration checks with simulated Feather responses; no hardware is commanded.
// Run with Node and playwright available in NODE_PATH.
const { chromium } = require("playwright");
const assert = require("node:assert/strict");
const fs = require("node:fs/promises");
const path = require("node:path");

/** Exercise settings, output commands, reboot, reconnection, and responsive layout. */
async function runChecks()
{
    const browser = await chromium.launch({ headless: true, channel: process.env.UI_TEST_BROWSER || "msedge" });
    try
    {
        const page = await browser.newPage({ viewport: { width: 1100, height: 900 } });
        const errors = [];
        const commands = [];
        const dataRoot = path.resolve(__dirname, "../data");
        let offline = false;
        let rejectSettings = false;
        let slowSettings = false;
        let ready = true;
        let enabled = false;
        let frequencyHz = 10;
        let amplitudeVolts = 0.5;
        let offsetVolts = 1.5;

        // Capture script failures so successful button checks cannot mask broken rendering.
        page.on("pageerror", (error) => { errors.push(error.message); });
        await page.route("**/*", async (route) =>
        {
            const request = route.request();
            const pathname = new URL(request.url()).pathname;
            if (pathname.startsWith("/api/"))
            {
                // Slow background reads expose transient disabling and clicks that
                // arrive before a poll completes; instant mocks hid the original bug.
                if (request.method() === "GET")
                {
                    await new Promise((resolve) => { setTimeout(resolve, 250); });
                }
                if (offline)
                {
                    await route.abort("connectionrefused");
                }
                else
                {
                    let response = {};
                    let status = 200;
                    if (request.method() === "POST")
                    {
                        commands.push(pathname);
                    }
                    if (pathname.startsWith("/api/blink"))
                    {
                        response = { enabled: false };
                    }
                    else if (pathname === "/api/reboot")
                    {
                        enabled = false;
                        frequencyHz = 10;
                        amplitudeVolts = 0.5;
                        offsetVolts = 1.5;
                        response = { rebooting: true };
                        status = 202;
                    }
                    else
                    {
                        if (pathname === "/api/dac/start")
                        {
                            enabled = true;
                        }
                        if (pathname === "/api/dac/stop")
                        {
                            enabled = false;
                        }
                        if (pathname === "/api/dac/settings" && !rejectSettings)
                        {
                            const body = new URLSearchParams(request.postData());
                            frequencyHz = Number(body.get("frequencyHz"));
                            amplitudeVolts = Number(body.get("amplitudeVolts"));
                            offsetVolts = Number(body.get("offsetVolts"));
                            if (slowSettings)
                            {
                                await new Promise((resolve) => { setTimeout(resolve, 400); });
                            }
                        }
                        // Use timestamped samples to exercise the actual canvas drawing code.
                        response = {
                            ready, enabled, frequencyHz, amplitudeVolts, offsetVolts,
                            minFrequencyHz: 1, maxFrequencyHz: 100,
                            minSignalVolts: 0.2, maxSignalVolts: 2.4,
                            sampleRateHz: 2000, missedIntervals: 2, calibration: "Factory eFuse",
                            samples: Array.from({ length: 256 }, (_, index) =>
                            {
                                const volts = enabled ? offsetVolts + amplitudeVolts * Math.sin(index / 20) : 0;
                                return [index * 0.5, volts, volts + 0.02];
                            })
                        };
                        if (pathname === "/api/dac/settings" && rejectSettings)
                        {
                            status = 400;
                            response = { error: "Settings rejected by firmware" };
                        }
                    }
                    await route.fulfill({ status, contentType: "application/json", body: JSON.stringify(response) });
                }
            }
            else
            {
                const filePath = path.join(dataRoot, pathname === "/" ? "index.html" : pathname);
                const mime = { ".html": "text/html", ".js": "text/javascript", ".css": "text/css" };
                await route.fulfill({ contentType: mime[path.extname(filePath)], body: await fs.readFile(filePath) });
            }
        });
        await page.goto("http://feather.test/");
        await page.waitForFunction(() => { return document.getElementById("status-system").textContent === "Connected"; });
        assert.match(await page.locator("#status-dac").innerText(), /Disabled/);
        assert.equal(await page.locator("#dac-apply").isDisabled(), true);

        // Grammar must reject letters, exponent/sign syntax, duplicate decimal
        // points and bad pastes, while allowing decimal editing and shortcuts.
        await page.locator("#dac-frequency").focus();
        await page.locator("#dac-frequency").press("End");
        await page.locator("#dac-frequency").pressSequentially("eE+-abc.");
        assert.equal(await page.locator("#dac-frequency").inputValue(), "10");
        await page.locator("#dac-amplitude").focus();
        await page.locator("#dac-amplitude").press("End");
        await page.locator("#dac-amplitude").pressSequentially(".e-+x");
        assert.equal(await page.locator("#dac-amplitude").inputValue(), "0.5");
        assert.equal(await page.locator("#dac-amplitude").evaluate((input) =>
        {
            const clipboard = new DataTransfer();
            clipboard.setData("text", "1e2");
            const event = new ClipboardEvent("paste", { clipboardData: clipboard, bubbles: true, cancelable: true });
            input.dispatchEvent(event);
            return event.defaultPrevented;
        }), true);

        // Explicit buttons and keyboard arrows use the requested decimal increments.
        await page.getByRole("button", { name: "Increase frequency by 1 Hz", exact: true }).click();
        assert.equal(await page.locator("#dac-frequency").inputValue(), "11");
        assert.equal(await page.locator("#dac-apply").isEnabled(), true);
        await page.locator("#dac-frequency").press("ArrowDown");
        assert.equal(await page.locator("#dac-frequency").inputValue(), "10");
        assert.equal(await page.locator("#dac-apply").isDisabled(), true);
        await page.getByRole("button", { name: "Increase amplitude by 0.05 V", exact: true }).click();
        assert.equal(await page.locator("#dac-amplitude").inputValue(), "0.55");
        await page.getByRole("button", { name: "Decrease amplitude by 0.05 V", exact: true }).click();
        await page.getByRole("button", { name: "Increase offset by 0.1 V", exact: true }).click();
        assert.equal(await page.locator("#dac-offset").inputValue(), "1.6");
        await page.getByRole("button", { name: "Decrease offset by 0.1 V", exact: true }).click();
        await page.locator("#dac-amplitude").fill("0.50");
        assert.equal(await page.locator("#dac-apply").isDisabled(), true);
        await page.locator("#dac-frequency").fill("100");
        await page.locator("#dac-frequency").press("ArrowUp");
        assert.equal(await page.locator("#dac-frequency").inputValue(), "100");
        await page.locator("#dac-frequency").fill("1");
        await page.locator("#dac-frequency").press("ArrowDown");
        assert.equal(await page.locator("#dac-frequency").inputValue(), "1");
        await page.locator("#dac-frequency").fill("10");
        assert.equal(await page.locator("#dac-apply").isDisabled(), true);

        // Observe several DAC and LED poll cycles: no healthy background read may
        // disable a button, even briefly. Only explicit commands should do so.
        await page.waitForFunction(() => { return !document.getElementById("blink-button").disabled; });
        await page.evaluate(() =>
        {
            window.disabledDuringPoll = [];
            window.buttonObserver = new MutationObserver((records) =>
            {
                for (const record of records)
                {
                    if (record.target.disabled)
                    {
                        window.disabledDuringPoll.push(record.target.id);
                    }
                }
            });
            for (const id of ["dac-apply", "dac-button", "reboot-button", "blink-button"])
            {
                window.buttonObserver.observe(document.getElementById(id), { attributes: true, attributeFilter: ["disabled"] });
            }
        });
        await page.waitForTimeout(4500);
        assert.deepEqual(await page.evaluate(() =>
        {
            window.buttonObserver.disconnect();
            return window.disabledDuringPoll;
        }), []);

        // Click during a delayed poll and require one command after it completes.
        await page.waitForRequest((request) => { return new URL(request.url()).pathname === "/api/dac"; });
        await page.locator("#dac-button").click();
        await page.waitForFunction(() => { return document.getElementById("dac-button").textContent === "Stop DAC"; });
        assert.equal(commands.filter((command) => { return command === "/api/dac/start"; }).length, 1);
        await page.locator("#dac-button").click();
        await page.waitForFunction(() => { return document.getElementById("dac-button").textContent === "Start DAC"; });
        await page.waitForRequest((request) => { return new URL(request.url()).pathname === "/api/blink"; });
        await page.locator("#blink-button").click();
        await page.waitForResponse((response) => { return new URL(response.url()).pathname === "/api/blink/start"; });
        assert.equal(commands.filter((command) => { return command === "/api/blink/start"; }).length, 1);

        // Polls must preserve drafts, while applying must update authoritative settings.
        await page.locator("#dac-frequency").fill("20");
        await page.waitForTimeout(1100);
        assert.equal(await page.locator("#dac-frequency").inputValue(), "20");
        await page.locator("#dac-apply").click();
        await page.waitForFunction(() => { return document.getElementById("status-sine").textContent.startsWith("20 Hz"); });
        assert.equal(await page.locator("#dac-apply").isDisabled(), true);
        await page.locator("#dac-button").click();
        await page.waitForFunction(() => { return document.getElementById("dac-button").textContent === "Stop DAC"; });
        await page.locator("#dac-button").click();
        await page.waitForFunction(() => { return document.getElementById("dac-button").textContent === "Start DAC"; });
        assert.ok(commands.includes("/api/dac/start") && commands.includes("/api/dac/stop"));

        // The combined envelope must be validated even when both numeric fields are valid.
        await page.locator("#dac-amplitude").fill("2");
        const beforeInvalid = commands.length;
        assert.equal(await page.locator("#dac-apply").isDisabled(), true);
        await page.locator("#dac-form").dispatchEvent("submit");
        assert.equal(commands.length, beforeInvalid);
        assert.match(await page.locator("#dac-validation").innerText(), /Offset ± amplitude/);
        await page.locator("#dac-amplitude").fill("1.1");
        await page.locator("#dac-offset").fill("1.3");
        await page.locator("#dac-apply").click();
        await page.waitForFunction(() => { return document.getElementById("status-sine").textContent.includes("1.1 V peak"); });

        // Invalid frequency must explain itself immediately; correcting it must
        // restore Apply without needing a reload, blur, or a hardware state change.
        await page.locator("#dac-frequency").fill("250");
        assert.equal(await page.locator("#dac-apply").isDisabled(), true);
        assert.equal(await page.locator("#dac-button").isDisabled(), false);
        assert.equal(await page.locator("#reboot-button").isDisabled(), false);
        assert.match(await page.locator("#dac-validation").innerText(), /Frequency must be 1–100 Hz/);
        await page.waitForTimeout(700);
        assert.match(await page.locator("#dac-validation").innerText(), /Frequency must be/);
        await page.locator("#dac-frequency").fill("");
        assert.equal(await page.locator("#dac-apply").isDisabled(), true);
        await page.locator("#dac-frequency").fill("25");
        assert.equal(await page.locator("#dac-apply").isEnabled(), true);
        await page.locator("#dac-apply").click();
        await page.waitForFunction(() => { return document.getElementById("status-sine").textContent.startsWith("25 Hz"); });

        // Typing during an in-flight settings command must survive the returned old values.
        slowSettings = true;
        await page.locator("#dac-frequency").fill("30");
        await page.locator("#dac-apply").click();
        await page.locator("#dac-frequency").fill("40");
        await page.waitForTimeout(900);
        assert.equal(await page.locator("#dac-frequency").inputValue(), "40");
        slowSettings = false;
        rejectSettings = true;
        await page.locator("#dac-apply").click();
        await page.waitForFunction(() => { return document.getElementById("event-log").textContent.includes("Settings rejected by firmware"); });
        assert.equal(await page.locator("#dac-frequency").inputValue(), "40");
        rejectSettings = false;

        // Lost connections disable controls, retain edits, and recover automatically.
        offline = true;
        await page.waitForFunction(() => { return document.getElementById("status-dac").textContent.startsWith("Unknown"); });
        assert.equal(await page.locator("#dac-button").isDisabled(), true);
        offline = false;
        await page.waitForFunction(() => { return document.getElementById("status-system").textContent === "Connected"; });
        assert.equal(await page.locator("#dac-frequency").inputValue(), "40");
        await page.locator("#dac-apply").click();
        await page.waitForFunction(() => { return document.getElementById("status-sine").textContent.startsWith("40 Hz"); });

        // Cancellation sends nothing; confirmation resets defaults and leaves output stopped.
        page.once("dialog", (dialog) => { dialog.dismiss(); });
        const beforeCancel = commands.length;
        await page.locator("#reboot-button").click();
        assert.equal(commands.length, beforeCancel);
        page.once("dialog", (dialog) => { dialog.accept(); });
        await page.locator("#reboot-button").click();
        await page.waitForFunction(() => { return document.getElementById("status-system").textContent === "Rebooting..."; });
        await page.waitForFunction(() => { return document.getElementById("status-system").textContent === "Connected"; });
        assert.equal(await page.locator("#dac-frequency").inputValue(), "10");
        assert.match(await page.locator("#status-dac").innerText(), /Disabled/);

        // Save desktop/mobile screenshots and detect horizontal overflow on a small phone.
        const outputDir = path.resolve(__dirname, "../.pio");
        await fs.mkdir(outputDir, { recursive: true });
        await page.screenshot({ path: path.join(outputDir, "ui-desktop.png"), fullPage: true });
        await page.setViewportSize({ width: 375, height: 900 });
        await page.screenshot({ path: path.join(outputDir, "ui-mobile.png"), fullPage: true });
        assert.equal(await page.evaluate(() => { return document.documentElement.scrollWidth <= window.innerWidth; }), true);
        assert.equal(await page.locator("#dac-frequency").evaluate((element) =>
        {
            // A fitting page alone does not prove that controls stay inside their panel.
            return element.getBoundingClientRect().right <= element.closest(".panel").getBoundingClientRect().right - 4;
        }), true);
        ready = false;
        await page.waitForFunction(() => { return document.getElementById("status-dac").textContent === "Initialization failed"; });
        assert.equal(await page.locator("#dac-button").isDisabled(), true);
        assert.deepEqual(errors, []);
        console.log("PASS: initial state, polling/drafts, apply, start/stop, envelope validation, boundary rounding, in-flight edits, server rejection, disconnect/recovery, reboot/cancel, mobile layout, initialization failure; no browser errors.");
    }
    finally
    {
        await browser.close();
    }
}

// Report a useful failing assertion and a nonzero status for command-line runs.
runChecks().catch((error) =>
{
    console.error(error);
    process.exitCode = 1;
});
