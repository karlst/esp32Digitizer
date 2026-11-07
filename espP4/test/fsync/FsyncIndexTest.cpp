/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Read reset, measure, report in that order. Core 1 still runs the real decoder.
 * A nominal sample period is 160000000 / 1024000 = 156.25 capture-timer ticks.
 * For example, 160000 ticks between observations rounds to 1024 samples.
 * Updating from every timestamp avoids extrapolating the nominal rate for ten
 * seconds. It does require frequent observations, unlike a once-per-PPS read.
 */
#include "FsyncIndexTest.h"
#include "PpsSampleTest.h"
#include "esp_cpu.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "hal/pcnt_ll.h"
#include "hal/mcpwm_ll.h"
#include "sdkconfig.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

static_assert(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ == 360, "Review timing threshold for changed CPU clock");
#if CONFIG_PM_ENABLE
#error "Index experiment requires fixed CPU frequency"
#endif

/* **
 * reset - Called before arming the stopped ADC. Clear counters and the previous
 * anchor. Optional ppsSampleTest receives numbered observations; its lifetime
 * must cover this run. Null keeps the index-only test. No previous run's timestamp or predicted pulse number is retained.
 */
void FsyncIndexTest::reset(PpsSampleTest* ppsSampleTest)
{
    _ppsSampleTest = ppsSampleTest;
    _exampleCount = 0;
    _observations = _initialSkipped = _checked = _matched = _mismatched = 0;
    _movingReference = _slowReference = _staleReference = _unsafeSteps = _maxGapTicks = 0;
    _firstPulse = _calculatedPulse = _previousTicks = 0;
    _firstTicks = 0;
    _sampleAnchorCount = 0;
    _advancedPulses = 0;
    _maxResidualSamples = 0;
    _previousAt = 0;
    _initialized = _lost = false;
}

/* **
 * measure - Called roughly each millisecond while real samples are being decoded.
 * The counter argument is the PCNT unit cleared before the ADC starts. Its
 * driver extends wraps, making the first reference an absolute pulse number.
 * PCNT reads bracket one timestamp read for startup/comparison only. A second timestamp
 * read screens the reference for changes. No retry loop is used: if either
 * register changed or the bracket took a whole FSYNC period, this observation
 * cannot validate the prediction. Once initialized, its first timestamp still
 * advances the predictor, independent of the reference count and its validity.
 *
 * The initial anchor uses a stable observed pair, not an independently latched
 * atomic pair. report therefore also checks the predicted absolute number
 * against the final, settled count/timestamp after ADC stop. Hardware-update
 * latency remains a limitation of the comparison reference, so disagreements are evidence to inspect, not proof
 * that the predictor alone was wrong.
 *
 * Reject prediction gaps above 10 ms or a rounding residual >= 0.25 sample.
 * This deliberately leaves at least a quarter-sample margin to the half-sample
 * rounding boundary. It is an experimental guard, not a proven oscillator bound.
 * After a guard failure stop advancing; never silently restart from PCNT.
 * Unsigned timestamp subtraction handles one timer wrap; wall time guards the
 * maximum gap. PCNT resets at 32767; extendCount attaches the raw count to the
 * driver total, so comparisons include every wrap rather than only a modulus.
 */
void FsyncIndexTest::measure(pcnt_unit_handle_t counter)
{
    _random ^= _random << 13;
    _random ^= _random >> 17;
    _random ^= _random << 5;
    esp_rom_delay_us(_random & 31);

    // The driver's accumulated count includes all 32767-pulse hardware wraps.
    // Attach the subsequent raw count to it only within a bounded 100-us age.
    // This is comparison/startup evidence, never feedback into the predictor.
    const int64_t baselineAt = esp_timer_get_time();
    int baseline = 0;
    ESP_ERROR_CHECK(pcnt_unit_get_count(counter, &baseline));
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&_mux);
    const uint32_t begin = esp_cpu_get_cycle_count();
    asm volatile("fence iorw, iorw" ::: "memory");
    const int countBefore = pcnt_ll_get_count(PCNT_LL_GET_HW(0), 0);
    const uint32_t ticks = mcpwm_ll_capture_get_value(MCPWM_LL_GET_HW(0), 0);
    const int countAfter = pcnt_ll_get_count(PCNT_LL_GET_HW(0), 0);
    const uint32_t ticksAfter = mcpwm_ll_capture_get_value(MCPWM_LL_GET_HW(0), 0);
    asm volatile("" : : "r"(countBefore), "r"(countAfter), "r"(ticks), "r"(ticksAfter) : "memory");
    asm volatile("fence iorw, iorw" ::: "memory");
    const uint32_t end = esp_cpu_get_cycle_count();
    portEXIT_CRITICAL(&_mux);

    ++_observations;
    const bool moving = countBefore != countAfter || ticks != ticksAfter;
    const bool slow = uint32_t(end - begin) >= 352;
    const bool stale = esp_timer_get_time() - baselineAt > 100;
    const uint32_t referencePulse = baseline >= 0 ?
        FsyncAssociationAnalysis::extendCount(uint32_t(baseline), countBefore) : 0;
    const bool usableReference = !moving && !slow && !stale && referencePulse >= 2;
    _movingReference += moving;
    _slowReference += slow;
    _staleReference += stale;
    if (!_initialized)
    {
        if (usableReference)
        {
            _firstPulse = _calculatedPulse = referencePulse;
            _firstTicks = ticks;
            _previousTicks = ticks;
            _previousAt = now;
            _initialized = true;
        }
        else
        {
            ++_initialSkipped;
        }
    }
    else if (!_lost)
    {
        // Multiply ticks by four to represent the 156.25-tick period exactly.
        // Adding half the divisor implements nearest-whole-sample rounding.
        const uint32_t deltaTicks = ticks - _previousTicks;
        const uint32_t deltaPulses = uint32_t((uint64_t(deltaTicks) * 4 + 312) / 625);
        const double residual = std::abs(double(deltaTicks) / 156.25 - deltaPulses);
        _maxGapTicks = std::max(_maxGapTicks, deltaTicks);
        _maxResidualSamples = std::max(_maxResidualSamples, residual);
        if (now - _previousAt > 10000 || deltaTicks > 1600000 || residual >= 0.25)
        {
            ++_unsafeSteps;
            _lost = true;
        }
        else
        {
            _calculatedPulse += deltaPulses;
            _advancedPulses += deltaPulses;
            _previousTicks = ticks;
            _previousAt = now;
            if (usableReference)
            {
                ++_checked;
                const int error = int(_calculatedPulse) - int(referencePulse);
                _matched += error == 0;
                _mismatched += error != 0;
                if (error != 0 && _exampleCount < 8)
                {
                    _examples[_exampleCount++] = {_observations, _calculatedPulse, int(referencePulse), ticks, error};
                }
            }
        }
    }

    // Keep the predicted number, never the comparison count, for later lookup
    // in the decoded sample array. Pulse 2 is index 0; pulse 65537 is index 65535.
    if (_initialized && !_lost && _sampleAnchorCount < 64 &&
        _calculatedPulse >= 2 && _calculatedPulse <= 65537 &&
        (_sampleAnchorCount == 0 || _calculatedPulse > _sampleAnchors[_sampleAnchorCount - 1]._pulseCount))
    {
        _sampleAnchors[_sampleAnchorCount++] = {_calculatedPulse, _previousTicks};
    }
    if (_initialized && !_lost && _ppsSampleTest != nullptr)
    {
        _ppsSampleTest->observe(_calculatedPulse, _previousTicks);
    }

}

/* **
 * getFailed - Core 0 stops PPS recording if numbering lost its rounding margin
 * or disagreed with a usable counter observation. Startup remains pending until
 * a usable reference exists; final endpoint validation still runs after stop.
 */
bool FsyncIndexTest::getFailed() const
{
    return _lost || _mismatched != 0;
}

/* **
 * report - After source shutdown and decoder drain, report every limitation and
 * summary. MEASURED means clean acquisition with reference comparisons; it does
 * not hide mismatches or claim the design passed. Prediction is never corrected
 * from reference observations. finalPulse/finalTicks are read after the source
 * stopped and capture hardware froze: neither is changing while read. Extend
 * the prediction to that last edge and require exact agreement, without using
 * the final count to adjust it. A wrong initial offset therefore fails unless
 * another prediction error cancels it; running comparisons provide extra checks.
 *
 * words/storedWords and stats are safe to read only after the real decoder has
 * drained. Require exactly one discarded startup frame, its checked length,
 * continuous frames, and enough retained anchors. Map pulse N to words[N-2].
 * This checks indexing under the established startup convention, not analog
 * waveform content. No measured timestamp for sample zero is fabricated: the
 * usable time reference is a captured timestamp for a known later sample.
 * Return the full sample-numbering verdict so PPS reports cannot accept an
 * unverified reference; printing a measurement alone is not a successful check.
 */
bool FsyncIndexTest::report(uint32_t run, bool clean, uint32_t finalPulse, uint32_t finalTicks,
    const uint32_t* words, uint32_t storedWords, const CaptureStats& stats) const
{
    // Test the stopped endpoint independently of transient register updates.
    const uint32_t tailTicks = finalTicks - _previousTicks;
    const uint32_t tailPulses = uint32_t((uint64_t(tailTicks) * 4 + 312) / 625);
    const double tailResidual = std::abs(double(tailTicks) / 156.25 - tailPulses);
    const uint32_t predictedFinal = _calculatedPulse + tailPulses;
    const bool endpointPassed = _initialized && !_lost && tailTicks <= 1600000 &&
        tailResidual < 0.25 && finalPulse == predictedFinal;
    const auto anchors = FsyncAssociationAnalysis::analyze(_sampleAnchors,
        _sampleAnchorCount, storedWords, stats._startupDiscardedFrames);
    const bool startupPassed = clean && _initialized && words != nullptr && storedWords != 0 &&
        stats._startupDiscardedFrames == 1 &&
        (stats._startupFrameClocks == 31 || stats._startupFrameClocks == 32) &&
        stats._badPeriods == 0 && stats._badHeaders == 0 && stats._shortWords == 0;
    const bool sampleZeroPassed = startupPassed && endpointPassed && anchors._passed &&
        _mismatched == 0 && _checked != 0 && _firstPulse >= 2 && _firstPulse - 2 < storedWords;
    printf("INDEX ENDPOINT observedPulse=%lu calculatedPulse=%lu errorSamples=%lld gapUs=%.3f residualSamples=%.6f status=%s\n",
        (unsigned long)finalPulse, (unsigned long)predictedFinal,
        (long long)predictedFinal - finalPulse, tailTicks / 160.0, tailResidual,
        endpointPassed ? "PASS" : "FAIL");
    printf("INDEX START status=%s firstStoredPulse=2 discarded=%lu startupClocks=%lu retainedAnchors=%u residualTicks=%.6f\n",
        sampleZeroPassed ? "PASS" : "FAIL", (unsigned long)stats._startupDiscardedFrames,
        (unsigned long)stats._startupFrameClocks, unsigned(anchors._matched), anchors._maxResidualTicks);
    if (startupPassed && _firstPulse >= 2 && _firstPulse - 2 < storedWords)
    {
        printf("INDEX REFERENCE pulse=%lu sample=%lu ticks=%lu word=%08lx sampleZeroWord=%08lx\n",
            (unsigned long)_firstPulse, (unsigned long)(_firstPulse - 2), (unsigned long)_firstTicks,
            (unsigned long)words[_firstPulse - 2], (unsigned long)words[0]);
    }
    for (unsigned iAnchor = 0; startupPassed && iAnchor < _sampleAnchorCount && iAnchor < 8; ++iAnchor)
    {
        const auto& anchor = _sampleAnchors[iAnchor];
        if (anchor._pulseCount >= 2 && anchor._pulseCount - 2 < storedWords)
        {
            printf("INDEX SAMPLE sample=%lu pulse=%lu ticks=%lu word=%08lx\n",
                (unsigned long)(anchor._pulseCount - 2), (unsigned long)anchor._pulseCount,
                (unsigned long)anchor._ticks, (unsigned long)words[anchor._pulseCount - 2]);
        }
    }
    printf("INDEX TOTAL observations=%lu initialSkipped=%lu checked=%lu matched=%lu mismatched=%lu movingReference=%lu slowReference=%lu staleReference=%lu\n",
        (unsigned long)_observations, (unsigned long)_initialSkipped, (unsigned long)_checked,
        (unsigned long)_matched, (unsigned long)_mismatched, (unsigned long)_movingReference, (unsigned long)_slowReference, (unsigned long)_staleReference);
    printf("INDEX MODEL ticksPerSample=156.25 firstPulse=%lu advancedPulses=%llu maxGapUs=%.3f maxResidualSamples=%.6f minRoundingMarginSamples=%.6f unsafeSteps=%lu lost=%u\n",
        (unsigned long)_firstPulse, (unsigned long long)_advancedPulses, _maxGapTicks / 160.0,
        _maxResidualSamples, 0.5 - _maxResidualSamples, (unsigned long)_unsafeSteps, unsigned(_lost));
    for (unsigned iExample = 0; iExample < _exampleCount; ++iExample)
    {
        const auto& example = _examples[iExample];
        printf("INDEX MISMATCH observation=%lu calculatedPulse=%lu observedPulse=%d ticks=%lu errorSamples=%d\n",
            (unsigned long)example._observation, (unsigned long)example._calculated,
            example._observed, (unsigned long)example._ticks, example._error);
    }
    printf("INDEX LIMIT: sample zero uses checked startup convention; stopped endpoint checks absolute offset; no fabricated sample-zero timestamp; nominal clock ratio; frequent anchors; SD load depends on selected mode\n");
    printf("INDEX RESULT run=%lu status=%s agreement=%s\n", (unsigned long)run,
        clean && _checked != 0 ? "MEASURED" : "FAIL",
        sampleZeroPassed ? "YES" : "NO");
    return sampleZeroPassed;
}
