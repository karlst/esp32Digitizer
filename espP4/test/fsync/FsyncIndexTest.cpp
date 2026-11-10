/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Attach an FSYNC pulse number to a captured timer tick count. FSYNC is the
 * ADC's frame-start signal: one rising edge (LOW to HIGH) identifies one sample.
 * PCNT is the hardware pulse counter. MCPWM's capture channel saves the timer
 * count at an edge; it does not save the pulse number. Their registers update
 * independently, so reading both once is not enough to establish a matched pair.
 *
 * Reading path: reset clears a run; measure establishes the first usable pair
 * and maintains numbering; getFailed tells the controller to stop on a fault;
 * report checks the stopped counter and prints evidence after acquisition.
 * Core 0 calls measure about every millisecond, while Core 1 decodes samples.
 * Each call reads count/ticks/count/ticks. Before initialization, a usable pair
 * sets the starting reference. Later, elapsed captured ticks advance a predicted
 * pulse number; a usable PCNT pair checks that prediction without resetting it.
 *
 * A nominal sample period is 160000000 / 1024000 = 156.25 timer ticks.
 * Frequent updates limit accumulated error from the independent clock rates.
 * PpsSampleTest receives the numbered references and uses them to number block
 * boundaries after PPS (one pulse per second). This millisecond routine neither
 * creates a tBlock nor timestamps an edge with the time software reads it.
 */
#include "FsyncIndexTest.h"
#include "PpsSampleTest.h"
#include "FsyncPollTrace.h"
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
    FsyncPollTrace::reset();
}

/* **
 * measure - FsyncAssociationTest::poll calls this on Core 0 about every ms.
 * counter is the PCNT unit reset before ADC startup. One call performs one
 * four-read observation, not a loop of 32 retries. If startup cannot use it,
 * the next poll tries again. After startup, the first captured tick value can
 * advance numbering even when the repeated values fail the PCNT comparison
 * checks. Prediction guards still apply to every update.
 *
 * There is no return value: counters retain the result. A usable PCNT count
 * that disagrees increments _mismatched; a prediction gap over 10 ms or residual
 * >=0.25 sample sets _lost. getFailed exposes either condition to the recording
 * controller, which requests source shutdown. This function does not itself
 * stop the ADC. A mismatch is never corrected by adopting the PCNT value.
 *
 * A residual is the distance from the calculated pulse increment to its nearest
 * integer. The 0.25 guard leaves room before rounding changes at 0.5; it is not
 * an absolute-time accuracy claim. Four stable reads are a software check, not
 * hardware capture of count and time together. report supplies an additional
 * check against the settled registers after the ADC stops.
 */
void FsyncIndexTest::measure(pcnt_unit_handle_t counter)
{
    FsyncPollTrace::mark(FsyncPollTrace::Stage::registers);

    // Vary the observation phase by 0..31 microseconds so this diagnostic does
    // not repeatedly sample the same part of an FSYNC period. This delay affects
    // when registers are read, never the tick count already saved by hardware.
    _random ^= _random << 13;
    _random ^= _random >> 17;
    _random ^= _random << 5;
    esp_rom_delay_us(_random & 31);

    // PCNT's raw register resets after 32767 pulses. The driver keeps the total
    // across those resets; extendCount later uses this total to interpret the
    // raw register. Require that baseline to be no more than 100 us old, much
    // shorter than a full counter cycle (~32 ms), to avoid attaching an old cycle.
    const int64_t baselineAt = esp_timer_get_time();
    int baseline = 0;
    ESP_ERROR_CHECK(pcnt_unit_get_count(counter, &baseline));
    const int64_t now = esp_timer_get_time();

    // Briefly exclude local interrupts while reading the four registers. This
    // reduces software interruptions; it does not stop FSYNC or make the two
    // hardware peripherals update together. CPU-cycle reads measure duration.
    // The fences bound the measured register sequence; there is no fence between
    // each of its four reads. The empty asm keeps all four results observable to
    // the compiler before the ending timing read.
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

    // Do not extend the register-read critical section. Freeze evidence once
    // numbering is lost so a shutdown delay cannot replace the capture stall.
    if (!_lost)
    {
        FsyncPollTrace::checkpoint();
    }
    FsyncPollTrace::mark(FsyncPollTrace::Stage::application);

    ++_observations;

    // These checks decide whether PCNT can establish/check numbering. At the
    // fixed 360-MHz CPU rate, 352 cycles is about 978 ns, one sample period.
    // A moving, slow or stale observation is skipped for comparison, not treated
    // as a pulse-number mismatch. Pulse 1 is the discarded startup frame.
    const bool moving = countBefore != countAfter || ticks != ticksAfter;
    const bool slow = uint32_t(end - begin) >= 352;
    const bool stale = esp_timer_get_time() - baselineAt > 100;
    const uint32_t referencePulse = baseline >= 0 ?
        FsyncAssociationAnalysis::extendCount(uint32_t(baseline), countBefore) : 0;
    const bool usableReference = !moving && !slow && !stale && referencePulse >= 2;
    _movingReference += moving;
    _slowReference += slow;
    _staleReference += stale;

    // Establish the reference only once. A rejected observation increments a
    // diagnostic counter and returns to the caller; retry occurs on a later poll.
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
        // Advance from the previous numbered timestamp, not the startup time.
        // 156.25 = 625/4, so (4*deltaTicks + 312)/625 rounds to a whole pulse
        // with integer division. Example: 160000 ticks adds 1024 pulses.
        // Unsigned subtraction handles one 32-bit timer wrap between reads.
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

            // PCNT independently checks the prediction only when its four-read
            // observation was usable. A rejected observation does not prevent
            // the timestamp-based update above. The first mismatch makes
            // getFailed true; the eight-entry array is diagnostic capacity, not
            // permission to tolerate eight mismatches. report prints it later.
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
        // Supply a recent numbered FSYNC reference. PpsSampleTest uses it when
        // PPS is pending; this does not emit one file timing block per call.
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
    FsyncPollTrace::report();
    printf("INDEX LIMIT: sample zero uses checked startup convention; stopped endpoint checks absolute offset; no fabricated sample-zero timestamp; nominal clock ratio; frequent anchors; SD load depends on selected mode\n");
    printf("INDEX RESULT run=%lu status=%s agreement=%s\n", (unsigned long)run,
        clean && _checked != 0 ? "MEASURED" : "FAIL",
        sampleZeroPassed ? "YES" : "NO");
    return sampleZeroPassed;
}
