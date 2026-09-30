// Shared browser input filtering and +/- step buttons for the three DAC fields.
// This controls permitted text, not hardware settings. dacControl validates ranges
// and the combined waveform before Start; firmware independently checks them again.
// A text input with role=spinbutton gives consistent visible buttons across browsers.

/**
 * Convert a complete unsigned decimal draft to a JavaScript number.
 * Accept digits with an optional decimal point, including .5 or 1.; reject empty text,
 * a lone dot, signs, exponents, or trailing letters. Return NaN (not a number) on
 * failure so a blank field cannot accidentally be treated as a valid zero.
 * Integer-only and voltage-range requirements are checked by the calling form.
 */
export function readNumericValue(input)
{
    const retVal = /^\d*\.?\d+$|^\d+\.$/.test(input.value) ? Number(input.value) : NaN;
    return retVal;
}

/**
 * Attach a numeric editor with explicit buttons and ArrowUp/ArrowDown support.
 * Text inputs avoid the browser's number-input grammar (which accepts e, + and -).
 * beforeinput rejects invalid insertion before display; paste is checked as a
 * whole, and the input fallback covers browser edits that bypass beforeinput.
 * Empty text and a decimal point are permitted intermediate edits, but cannot apply.
 */
export function initializeNumericControl(input)
{
    const integerOnly = input.dataset.step === "1";
    const grammar = integerOnly ? /^\d*$/ : /^\d*\.?\d*$/;
    let acceptedText = input.value;

    /**
     * Check the whole proposed text after replacing the currently selected characters.
     * This handles typing in the middle of a field and pasting over a selection; checking
     * only the inserted characters would wrongly permit a second decimal point.
     * Return true if the proposed edit fits this field's integer/decimal grammar.
     */
    function acceptsInsertion(text)
    {
        const candidate = input.value.slice(0, input.selectionStart) + text +
            input.value.slice(input.selectionEnd);
        const retVal = grammar.test(candidate);
        return retVal;
    }

    // Block invalid characters without interfering with deletion, selection or shortcuts.
    input.addEventListener("beforeinput", (event) =>
    {
        if (event.data !== null && !acceptsInsertion(event.data))
        {
            event.preventDefault();
        }
    });
    // Reject an invalid paste intact; silently extracting its digits could change meaning.
    input.addEventListener("paste", (event) =>
    {
        if (!acceptsInsertion(event.clipboardData.getData("text")))
        {
            event.preventDefault();
        }
    });
    // Save only accepted grammar. Form-level validation handles numeric range/envelope.
    input.addEventListener("input", () =>
    {
        if (grammar.test(input.value))
        {
            acceptedText = input.value;
        }
        else
        {
            input.value = acceptedText;
        }
    });
    // Programmatic device refreshes do not dispatch input; refresh the fallback on focus.
    input.addEventListener("focus", () =>
    {
        acceptedText = input.value;
    });

    /**
     * Add or subtract one field-specific increment and notify the containing form.
     * direction is +1 or -1; data-step supplies 1 Hz, 0.05 V, or 0.1 V from the HTML.
     * If the draft is unfinished/invalid, use the minimum as the starting value. Clamp
     * to the individual field limits, round floating-point residue, and emit one input
     * event. Cross-field voltage limits still belong to dacControl's validation.
     */
    function stepValue(direction)
    {
        const current = readNumericValue(input);
        const minimum = Number(input.getAttribute("aria-valuemin"));
        const maximum = Number(input.getAttribute("aria-valuemax"));
        const base = Number.isFinite(current) ? current : minimum;
        // Round decimal addition so repeated 0.05 steps do not accumulate binary tails.
        const nextValue = Math.min(maximum, Math.max(minimum,
            Number((base + direction * Number(input.dataset.step)).toFixed(6))));
        input.value = String(nextValue);
        acceptedText = input.value;
        input.dispatchEvent(new Event("input", { bubbles: true }));
    }

    // Explicit buttons stay visible on browsers that hide native spin controls.
    input.closest(".numeric-control").querySelectorAll("button").forEach((button) =>
    {
        button.addEventListener("click", () =>
        {
            stepValue(Number(button.dataset.direction));
        });
    });
    // Match the standard keyboard behavior of an accessible spinbutton.
    input.addEventListener("keydown", (event) =>
    {
        if (event.key === "ArrowUp" || event.key === "ArrowDown")
        {
            event.preventDefault();
            stepValue(event.key === "ArrowUp" ? 1 : -1);
        }
    });
}
