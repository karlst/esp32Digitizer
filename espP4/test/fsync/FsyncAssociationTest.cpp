/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Purpose: connect an ADC reading to the FSYNC pulse that announced its frame.
 * A sample is one channel's voltage reading; its serial frame contains that
 * 24-bit value plus status bits. FSYNC (frame synchronization) marks the frame.
 *
 * Reading path: main.cpp supplies this service to RecordingApplication.
 * initialize reserves resources. prepare and started check stopped-ADC startup.
 * Once Feather starts the ADC, Core 1 calls submit with decoded words while
 * Core 0 calls poll to run the selected timing diagnostic. The original short
 * association mode uses takeSnapshot; the timed-file path uses FsyncIndexTest
 * for millisecond numbering and PpsSampleTest for once-per-PPS boundaries.
 * stopping freezes hardware; finish checks and prints only after decoding has
 * drained, meaning all receiver data accepted for decoding has been processed.
 * In the delay build, PulseDelayCapture adds timestamps of the generator input.
 *
 * Hardware captures EVERY FSYNC rise without a per-edge interrupt. Core 0
 * occasionally reads the latest timestamp and pulse count. The original short
 * association test requires FSYNC LOW; read/index/PPS modes use their separate
 * comparison and numbering rules. Only this dedicated startup owns
 * MCPWM group 0 and PCNT, so the first allocated capture channel and pulse counter
 * are both numbered 0. Direct register reads below are tied to ESP-IDF 5.5.6.
 * They avoid driver locks in the short register-read window. The public PCNT
 * getter supplies its accumulated count outside that window. FsyncIndexTest
 * explains timestamp-derived numbering; PpsSampleTest attaches PPS references.
 */
#include "FsyncAssociationTest.h"
#include "RecordingApplication.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "hal/mcpwm_ll.h"
#include "hal/pcnt_ll.h"
#include "hal/gpio_ll.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(5, 5, 6)
#error "Review the MCPWM register access and allocation order for this IDF version before using this test."
#endif

/* **
 * initialize - Startup reserves one hardware capture channel, one pulse counter,
 * and RAM. External PSRAM holds retained ADC words; snapshots stay in the
 * object's internal memory. No ADC start is requested. The decoder is read after
 * drain. ESP_ERROR_CHECK stops startup on an allocation/configuration failure.
 * A non-null pulseDelayCapture adds the direct-input channel to the same timer;
 * its lifetime must cover this service. Null leaves the association test alone.
 */
void FsyncAssociationTest::initialize(const FrameDecoder& frameDecoder, PulseDelayCapture* pulseDelayCapture)
{
    _frameDecoder = &frameDecoder;
    _pulseDelayCapture = pulseDelayCapture;

    // Each retained word uses four bytes: one status byte plus a 24-bit reading.
    // The allocation lasts for this firmware's lifetime; no later run reallocates.
    _words = static_cast<uint32_t*>(heap_caps_malloc(
        _wordCapacity * sizeof(uint32_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    ESP_ERROR_CHECK(_words != nullptr ? ESP_OK : ESP_ERR_NO_MEM);

    // MCPWM is a timer peripheral with input capture. Capture copies a running
    // counter into a register on a rising edge (LOW to HIGH). FSYNC needs no
    // per-edge ISR (Interrupt Service Routine); the separate PPS channel does
    // use one. 160 MHz gives a nominal 6.25 ns/tick, not an absolute error bound.
    mcpwm_capture_timer_config_t timerConfig = {};
    timerConfig.group_id = 0;
    timerConfig.clk_src = MCPWM_CAPTURE_CLK_SRC_PLL160M;
    timerConfig.resolution_hz = 160000000;
    ESP_ERROR_CHECK(mcpwm_new_capture_timer(&timerConfig, &_timer));
    uint32_t resolution = 0;
    ESP_ERROR_CHECK(mcpwm_capture_timer_get_resolution(_timer, &resolution));
    ESP_ERROR_CHECK(resolution == 160000000 ? ESP_OK : ESP_ERR_INVALID_STATE);
    mcpwm_capture_channel_config_t captureConfig = {};
    captureConfig.gpio_num = 1;

    // prescale=1 captures every rising edge, not one out of several pulses.
    captureConfig.prescale = 1;
    captureConfig.flags.pos_edge = true;
    ESP_ERROR_CHECK(mcpwm_new_capture_channel(_timer, &captureConfig, &_capture));

    // The optional direct-pulse input shares this timer, so both paths have the
    // same time origin and tick duration. The association-only build omits it.
    if (_pulseDelayCapture != nullptr)
    {
        _pulseDelayCapture->initialize(_timer);
    }
    else
    {

        // The ordinary FSYNC build reserves GPIO6 for optional PPS capture.
        // It stays disabled unless the PC selects PPS mode while idle.
        _ppsSampleTest.initialize(_timer);
    }
    ESP_ERROR_CHECK(mcpwm_capture_timer_enable(_timer));

    // PCNT counts the same physical FSYNC rises. Its driver extends the small
    // hardware counter across 32767-count wraps; only those wraps interrupt.
    // The current count getter also accounts for a pending wrap interrupt.
    pcnt_unit_config_t counterConfig = {};
    counterConfig.low_limit = -32767;
    counterConfig.high_limit = 32767;
    counterConfig.flags.accum_count = true;
    ESP_ERROR_CHECK(pcnt_new_unit(&counterConfig, &_counter));
    pcnt_chan_config_t channelConfig = {};
    channelConfig.edge_gpio_num = 1;

    // No second input controls counting direction: each rise adds one, each
    // fall leaves the count alone. Watch points let the driver extend wraps.
    channelConfig.level_gpio_num = -1;
    ESP_ERROR_CHECK(pcnt_new_channel(_counter, &channelConfig, &_counterChannel));
    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(_counterChannel,
        PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_HOLD));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(_counterChannel,
        PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_KEEP));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(_counter, 32767));
    ESP_ERROR_CHECK(pcnt_unit_add_watch_point(_counter, -32767));
    ESP_ERROR_CHECK(pcnt_unit_enable(_counter));
    ESP_ERROR_CHECK(gpio_set_pull_mode(GPIO_NUM_1, GPIO_FLOATING));
}

/* **
 * prepare - Core 0 arms both counters while Feather still holds the ADC stopped.
 * Accept only the previously characterized 1.024-MSPS settings. Observe LOW and
 * zero pulses over a scheduler delay before allowing receiver setup. A failure
 * refuses preparation, so READY is not sent and Feather must not start the ADC.
 */
bool FsyncAssociationTest::prepare(uint32_t run, const Ads127Config& config)
{

    // The application labels the final report with run; this method only arms
    // the next acquisition. Clear all prior-run evidence before testing startup.
    (void)run;
    stopHardware();
    _ready = _failed = _idleLow = false;
    _submitted.store(0);
    _storedWords = 0;
    _decodedBlocks = _pauseCount = 0;
    _maxWaitUs = _maxDecodeUs = _maxPauseUs = _waitAfterPauseUs = 0;
    _afterPause = false;
    _decodeTotalUs = _rawTotalBytes = _ramCopyUs = 0;
    _anchorCount = _rejectedSnapshots = 0;
    _highSnapshots = _movingSnapshots = _slowSnapshots = 0;
    _finalCount = 0;
    _fsyncReadTest.reset();
    _fsyncIndexTest.reset(_ppsTest ? &_ppsSampleTest : nullptr);
    bool retVal = config.matches(ads127DefaultConfig) && gpio_get_level(GPIO_NUM_1) == 0;
    if (retVal)
    {
        ESP_ERROR_CHECK(pcnt_unit_clear_count(_counter));
        ESP_ERROR_CHECK(mcpwm_capture_timer_start(_timer));
        ESP_ERROR_CHECK(mcpwm_capture_channel_enable(_capture));
        ESP_ERROR_CHECK(pcnt_unit_start(_counter));
        if (_pulseDelayCapture != nullptr)
        {
            _pulseDelayCapture->start();
        }
        if (_ppsTest)
        {
            _ppsSampleTest.start();
        }
        _running = true;
        _armedAt = esp_timer_get_time();

        // Yield for one FreeRTOS scheduler tick (not one 160-MHz timer tick).
        // A zero count afterward checks that Feather has not started the ADC.
        vTaskDelay(1);
        int count = 0;
        ESP_ERROR_CHECK(pcnt_unit_get_count(_counter, &count));
        retVal = count == 0 && gpio_get_level(GPIO_NUM_1) == 0;
    }
    _ready = _idleLow = retVal;
    _failed = !retVal;
    if (!retVal)
    {
        stopHardware();
        printf("FSYNC PREP FAIL: require stopped ADC, idle LOW FSYNC and 1024000 samples/s\n");
    }
    return retVal;
}

/* **
 * getReady - Core 0 reads whether the stopped-input checks succeeded.
 */
bool FsyncAssociationTest::getReady() const
{
    return _ready;
}

/* **
 * getInitialFsyncLow - Count the first captured HIGH as frame zero only for
 * this checked, stopped-ADC startup. It will be discarded, not stored as a word.
 */
bool FsyncAssociationTest::getInitialFsyncLow() const
{
    return _idleLow && _ready;
}

/* **
 * getDiscardStartupFrame - Start stored words at pulse two, whether the first
 * captured frame contains 31 or 32 clocks. The decoder reports the omission
 * separately and still rejects other initial lengths and all later errors.
 */
bool FsyncAssociationTest::getDiscardStartupFrame() const
{
    return _idleLow && _ready;
}

/* **
 * cancelPreparation - Core 0 releases active measurement state after cancellation.
 */
void FsyncAssociationTest::cancelPreparation()
{
    stopHardware();
    _ready = false;
}

/* **
 * started - Called after receiver arm but before READY permits ADC start. Verify
 * that no FSYNC pulse occurred during receiver setup. A failure invalidates the
 * whole test; the application's next poll requests source shutdown.
 */
void FsyncAssociationTest::started(TaskHandle_t control, TaskHandle_t decoder)
{
    (void)control;
    (void)decoder;
    int count = 0;
    ESP_ERROR_CHECK(pcnt_unit_get_count(_counter, &count));
    _failed = _failed || count != 0 || gpio_get_level(GPIO_NUM_1) != 0;
}

/* **
 * submit - The decoder task (a scheduled execution thread) on Core 1 supplies
 * count words at the words pointer. memcpy below copies them into retained RAM.
 * For example, storedWords=100 and copied=20 fills word slots 100 through 119;
 * pointer arithmetic advances in uint32_t words, while memcpy takes bytes. Copy
 * only the remaining capacity; still count later words while Feather stops.
 * No pointer is retained and no disk access occurs. Core 0 reads the words only
 * after drain. The atomic submitted count lets Core 0 request stop at the
 * selected mode's target, even after this small retained-word array is full.
 * True means the diagnostic accepted the submission, not that every word was
 * retained: after capacity is reached, counting continues but storage stops.
 */
bool FsyncAssociationTest::submit(const uint32_t* words, size_t count)
{

    // Time the diagnostic copy separately from the ordinary SD submission.
    const int64_t began = esp_timer_get_time();
    const size_t copied = std::min(count, size_t(_wordCapacity - _storedWords));
    memcpy(_words + _storedWords, words, copied * sizeof(uint32_t));
    _storedWords += copied;
    _submitted.fetch_add(static_cast<uint32_t>(count));
    _ramCopyUs += esp_timer_get_time() - began;
    return true;
}

/* **
 * decoded - Core 1 calls after each raw buffer has been decoded and submitted.
 * Keep maximum queue wait and processing time in microseconds without printing
 * during acquisition. Processing includes synchronous output callbacks (RAM
 * retention and, when selected, SD packing/copy/checksum work). Elapsed time
 * also includes interruptions; it is not exclusive CPU execution time.
 * Also retain the wait for the first buffer after a pause.
 * Core 0 reads these measurements only after decoding has finished.
 */
void FsyncAssociationTest::decoded(int64_t waitUs, int64_t decodeUs, uint32_t bytes)
{
    _rawTotalBytes += bytes;
    _decodeTotalUs += decodeUs;
    ++_decodedBlocks;
    _maxWaitUs = std::max(_maxWaitUs, waitUs);
    _maxDecodeUs = std::max(_maxDecodeUs, decodeUs);
    if (_afterPause)
    {
        _waitAfterPauseUs = std::max(_waitAfterPauseUs, waitUs);
        _afterPause = false;
    }
}

/* **
 * paused - Core 1 calls after its deliberate scheduler pause ends. Record actual
 * elapsed microseconds, including any delayed resumption. No locks or console
 * output interrupt acquisition; reporting waits for decoder shutdown.
 */
void FsyncAssociationTest::paused(int64_t elapsedUs)
{
    ++_pauseCount;
    _maxPauseUs = std::max(_maxPauseUs, elapsedUs);
    _afterPause = true;
}

/* **
 * getFull - Core 0 requests RAMFULL/STOP at 32768 words normally, or
 * ten seconds in read/index mode or the selected PPS duration. Only the first
 * 65536 words are retained;
 * all later words still pass through the real decoder and are counted.
 * RAMFULL asks Feather to stop the ADC; it is not confirmation of stopped
 * hardware. The larger 65536-word allocation leaves room while replies arrive.
 */
bool FsyncAssociationTest::getFull() const
{

    // Read mode keeps decoding for ten seconds; retained RAM remains bounded.
    return _submitted.load() >= getTargetWords();
}

/* **
 * getTargetWords - Core 0 chooses the finite run length. PPS mode covers 60 or
 * 300 seconds at the nominal sample rate; read/index modes retain ten seconds.
 * Sample storage stays bounded at 65536 words regardless of the run length.
 */
uint32_t FsyncAssociationTest::getTargetWords() const
{
    const uint32_t retVal = _ppsTest ? _ppsSeconds * 1024000U :
        (_readTest || _indexTest) ? 10240000U : _targetWords;
    return retVal;
}

/* **
 * getFailed - Core 0 combines setup faults with live PPS/numbering failures.
 * PPS overflow is read through its ISR lock. No invalid timing is silently used.
 */
bool FsyncAssociationTest::getFailed() const
{
    return _failed || (_ppsTest && (_ppsSampleTest.getFailed() || _fsyncIndexTest.getFailed()));
}

/* **
 * takeSnapshot - Core 0 reads a matched pulse count and latest capture register.
 * GPIO1 must be LOW before and after, so readings avoid the rising transition.
 * First obtain the accumulated count through the driver. Then read both small
 * hardware count and timestamp twice without driver calls between them. The
 * short critical section prevents a task switch or interrupt from stretching
 * this register-read sequence. It never stops either hardware counter. Hardware
 * counts every pulse even though most timestamps are intentionally overwritten.
 * Reject changed registers, a HIGH level, or more than 100 us since the baseline
 * read; the latter prevents an unnoticed wrap while attaching the small count
 * to its accumulated value. No copying, logging or waiting occurs under the lock.
 * An accepted count N identifies retained sample N-2, after discarding pulse one,
 * provided the checked startup and decoder continuity checks also pass. For
 * example, pulse 102 belongs to stored word 100. This is not the time at which
 * this function runs: ticks was latched earlier by the FSYNC rising edge.
 */
bool FsyncAssociationTest::takeSnapshot()
{
    bool retVal = false;
    const int64_t beganAt = esp_timer_get_time();
    int baseline = 0;
    ESP_ERROR_CHECK(pcnt_unit_get_count(_counter, &baseline));

    // This startup alone allocated PCNT unit 0 and MCPWM capture channel 0.
    // Keep the sequence short enough to finish within one FSYNC LOW interval.
    portENTER_CRITICAL(&_snapshotMux);
    const int highBefore = gpio_ll_get_level(GPIO_LL_GET_HW(0), 1);
    const int before = pcnt_ll_get_count(PCNT_LL_GET_HW(0), 0);
    const uint32_t ticks = mcpwm_ll_capture_get_value(MCPWM_LL_GET_HW(0), 0);
    const uint32_t again = mcpwm_ll_capture_get_value(MCPWM_LL_GET_HW(0), 0);
    const int after = pcnt_ll_get_count(PCNT_LL_GET_HW(0), 0);
    const int highAfter = gpio_ll_get_level(GPIO_LL_GET_HW(0), 1);
    portEXIT_CRITICAL(&_snapshotMux);
    const bool tooSlow = esp_timer_get_time() - beganAt > 100;

    // The driver's earlier total and the later small register may straddle a
    // hardware wrap. extendCount attaches the later register to that total.
    const uint32_t pulseCount = baseline >= 0 ?
        FsyncAssociationAnalysis::extendCount(static_cast<uint32_t>(baseline), before) : 0;
    if (highBefore != 0 || highAfter != 0)
    {
        ++_highSnapshots;
    }
    else if (before != after || ticks != again)
    {
        ++_movingSnapshots;
    }
    else if (tooSlow)
    {
        ++_slowSnapshots;
    }
    else if (pulseCount != 0 &&
        (_anchorCount == 0 || pulseCount > _anchors[_anchorCount - 1]._pulseCount))
    {
        _anchors[_anchorCount++] = {pulseCount, ticks};
        retVal = true;
    }
    return retVal;
}

/* **
 * poll - Core 0 dispatches one observation to the selected test per control-loop
 * pass. Index/PPS/timed-file modes call FsyncIndexTest::measure once; they do not
 * enter the 32-attempt takeSnapshot loop below. That loop belongs only to the
 * original short association/delay modes. FsyncBoundaryCapture has another,
 * separate 32-read limit for the once-per-PPS search for consecutive edges.
 * Read mode instead gathers comparison statistics for ten seconds, without
 * saving anchors. Index mode instead predicts pulse numbers from timestamps
 * over ten seconds, or the selected duration with PPS capture. Association and delay modes
 * remain unchanged.
 * Rejected reads are normal when a pulse arrives during the read. No printing
 * occurs while samples arrive. Watchdogs allow five seconds for the short test,
 * fourteen for read/index, and four seconds beyond the PPS target. These bounds detect a missing
 * source/stop handshake; they are not the requested sample-capture durations.
 */
void FsyncAssociationTest::poll()
{
    if (_running && _readTest && _submitted.load() != 0 && _submitted.load() < getTargetWords())
    {
        _fsyncReadTest.measure();
    }
    if (_running && _indexTest && _submitted.load() != 0 && _submitted.load() < getTargetWords())
    {
        _fsyncIndexTest.measure(_counter);
    }
    if (_running && !_readTest && !_indexTest && _anchorCount < 128)
    {
        bool captured = false;
        for (unsigned attempt = 0; attempt < 32 && !captured; ++attempt)
        {
            captured = takeSnapshot();
            if (!captured)
            {
                ++_rejectedSnapshots;
            }
        }
    }
    if (_running && esp_timer_get_time() - _armedAt > (_ppsTest ? int64_t(_ppsSeconds + 4) * 1000000 : (_readTest || _indexTest) ? 14000000 : 5000000))
    {
        _failed = true;
    }
}

/* **
 * stopHardware - Core 0 freezes measurements once; resources stay allocated for
 * the next run. Stopping the counter does not stop the ADC or its receiver.
 */
void FsyncAssociationTest::stopHardware()
{
    if (_running)
    {
        if (_pulseDelayCapture != nullptr)
        {
            _pulseDelayCapture->stop();
        }
        if (_ppsTest)
        {
            _ppsSampleTest.stop();
        }
        ESP_ERROR_CHECK(pcnt_unit_stop(_counter));
        ESP_ERROR_CHECK(mcpwm_capture_channel_disable(_capture));
        ESP_ERROR_CHECK(mcpwm_capture_timer_stop(_timer));
        ESP_ERROR_CHECK(pcnt_unit_get_count(_counter, &_finalCount));
        _running = false;
    }
}

/* **
 * stopping - Before receiver shutdown, verify the source has actually stopped.
 * The ordinary controller reaches here after Feather STOP, or on a fault. A
 * changing count across a scheduler tick rejects the measurement rather than pretending
 * receiver shutdown and ADC shutdown happened at the same instant.
 */
void FsyncAssociationTest::stopping()
{
    int before = 0;
    int after = 0;
    ESP_ERROR_CHECK(pcnt_unit_get_count(_counter, &before));
    vTaskDelay(1);
    ESP_ERROR_CHECK(pcnt_unit_get_count(_counter, &after));
    _failed = _failed || before != after || gpio_get_level(GPIO_NUM_1) != 0;
    stopHardware();
}

/* **
 * finish - Core 0 reports only after decoder drain. Check timestamp spacing,
 * normal receiver integrity, sample counts, and the bounded undelivered DMA
 * tail. The tail is reported, never silently counted as retained samples.
 * PASS means this startup's observations agree; it does not calibrate ADC delay,
 * prove nanosecond accuracy, or exclude a constant numbering error without the
 * startup assumptions. Matched anchors include the actual retained raw word.
 */
void FsyncAssociationTest::finish(uint32_t run, uint32_t rate, bool clean, uint32_t samples)
{
    stopHardware();

    // Decoder shutdown has completed, so these Core 1 measurements are frozen.
    printf("FSYNC CPU rawBytes=%llu decodeTotalUs=%llu diagnosticCopyUs=%llu\n",
        (unsigned long long)_rawTotalBytes, (unsigned long long)_decodeTotalUs,
        (unsigned long long)_ramCopyUs);

    // Decode duration includes packing, ring copying and checksum work in SD mode.
    printf("FSYNC WORK blocks=%lu pauses=%lu maxWaitUs=%lld maxDecodeUs=%lld maxPauseUs=%lld waitAfterPauseUs=%lld\n",
        (unsigned long)_decodedBlocks, (unsigned long)_pauseCount,
        (long long)_maxWaitUs, (long long)_maxDecodeUs, (long long)_maxPauseUs,
        (long long)_waitAfterPauseUs);
    const auto& stats = _frameDecoder->getCaptureStats();
    const uint32_t discarded = stats._startupDiscardedFrames;
    const auto result = FsyncAssociationAnalysis::analyze(_anchors, _anchorCount, _storedWords, discarded);

    // DMA (Direct Memory Access) moves input bytes to memory in batches. At
    // shutdown, some counted frames can remain in an undelivered final batch.
    // The tail is hardware pulses minus decoded frames, not lost stored words.
    const int64_t tail = int64_t(_finalCount) - stats._frames;
    const bool passed = clean && !_failed && _idleLow && rate == 1024000 &&
        samples == _submitted.load() && samples >= _targetWords &&
        discarded == 1 && (stats._startupFrameClocks == 31 || stats._startupFrameClocks == 32) &&
        stats._frames >= uint64_t(samples) + discarded &&
        stats._frames <= uint64_t(samples) + discarded + 1 &&
        tail >= 0 && tail <= 512 &&
        ((_readTest || _indexTest) ? samples >= getTargetWords() && stats._badPeriods == 0 && stats._badHeaders == 0 : result._passed);

    // These named console rows are the PC parser's evidence. Prefix bytes and
    // first-frame clock positions explain startup failures without printing
    // from the acquisition tasks while the ADC is running.
    printf("FSYNC COUNTS run=%lu hardware=%d frames=%lu words=%lu tail=%lld rejected=%lu\n",
        (unsigned long)run, _finalCount, (unsigned long)stats._frames,
        (unsigned long)samples, (long long)tail, (unsigned long)_rejectedSnapshots);
    printf("FSYNC READS saved=%u high=%lu changed=%lu slow=%lu\n", unsigned(_anchorCount),
        (unsigned long)_highSnapshots, (unsigned long)_movingSnapshots, (unsigned long)_slowSnapshots);
    printf("FSYNC ALIGN run=%lu discarded=%lu clocks=%lu firstPulse=2\n",
        (unsigned long)run, (unsigned long)discarded, (unsigned long)stats._startupFrameClocks);
    printf("FSYNC PREFIX bytes=");
    for (unsigned iByte = 0; iByte < stats._prefixLength; ++iByte)
    {
        printf("%02x", unsigned(stats._prefixBytes[iByte]));
    }
    printf("\n");
    for (unsigned iFrame = 0; iFrame < 4 && iFrame < stats._frames; ++iFrame)
    {
        printf("FSYNC FIRST frame=%u clock=%llu\n", iFrame,
            (unsigned long long)stats._firstFrameClocks[iFrame]);
    }
    if (stats._badPeriods != 0)
    {
        printf("FSYNC BAD_PERIOD clock=%llu previous=%llu bits=%lu\n",
            (unsigned long long)stats._firstBadPeriodClock,
            (unsigned long long)stats._firstBadPeriodPreviousClock,
            (unsigned long)stats._firstBadPeriodBits);
    }
    for (size_t iAnchor = 0; iAnchor < result._matched; ++iAnchor)
    {
        const auto& anchor = _anchors[result._firstMatched + iAnchor];
        const uint32_t sample = anchor._pulseCount - discarded - 1;
        printf("FSYNC %s run=%lu sample=%lu pulse=%lu ticks=%lu word=%08lx\n",
            passed ? "ANCHOR" : "CANDIDATE", (unsigned long)run, (unsigned long)sample,
            (unsigned long)anchor._pulseCount, (unsigned long)anchor._ticks,
            (unsigned long)_words[sample]);
    }
    printf("FSYNC RESULT run=%lu status=%s anchors=%u ticksPerSample=%.9f residualTicks=%.3f startup=%u clean=%u\n",
        (unsigned long)run, _indexTest ? "INDEX_ONLY" : _readTest ? "READ_ONLY" : passed ? "PASS" : "FAIL", unsigned(result._matched),
        result._ticksPerSample, result._maxResidualTicks, unsigned(_idleLow && !_failed), unsigned(clean));
    printf("FSYNC LIMIT: consistency test only; DMA tail excluded; no ADC-delay or absolute-time calibration\n");
    if (_pulseDelayCapture != nullptr)
    {
        _pulseDelayCapture->report(run, passed, _words, _storedWords, _anchors, result);
    }
    if (_readTest)
    {
        _fsyncReadTest.report(run, passed);
    }
    if (_indexTest)
    {

        // The source is stopped, timing hardware is frozen, and decoder drain
        // completed before finish. Read the settled final FSYNC timestamp for
        // an endpoint comparison that does not depend on racing two registers.
        const uint32_t finalTicks = mcpwm_ll_capture_get_value(MCPWM_LL_GET_HW(0), 0);
        const bool numbered = _fsyncIndexTest.report(run, passed, _finalCount >= 0 ? uint32_t(_finalCount) : 0,
            finalTicks, _words, _storedWords, stats);
        if (_ppsTest)
        {
            _ppsSampleTest.report(run, numbered, samples, _ppsSeconds);
            _failed = _failed || !numbered;
        }
    }
    _ready = false;
}

/* **
 * getBoundary - Control task forwards actual PPS boundary metadata. Both the
 * timing poll and this copy run on Core 0; the caller queues its own copy for SD.
 */
bool FsyncAssociationTest::getBoundary(unsigned iBoundary, TimedBoundary& boundary) const
{
    return _ppsTest && _ppsSampleTest.getBoundary(iBoundary, boundary);
}

/* **
 * command - h identifies the existing build. r selects the ten-second read
 * experiment; n selects timestamp-derived numbering. Both are idle-only and
 * unavailable with the separate pulse-delay input. p adds rising-edge PPS
 * capture on GPIO6 for 60 seconds; l selects 300 seconds for the timed SD adapter.
 * Selection persists until reset; the PC runner waits for this acknowledgement.
 */
void FsyncAssociationTest::command(RecordingApplication& application, char key)
{
    if (key == 'r' && application.getIdle() && _pulseDelayCapture == nullptr)
    {
        _ppsTest = false;
        _readTest = true;
        _indexTest = false;
        printf("READ MODE version=3 seconds=10\n");
    }
    if ((key == 'p' || key == 'l') && application.getIdle() && _pulseDelayCapture == nullptr)
    {
        _ppsTest = true;
        _indexTest = true;
        _readTest = false;
        _ppsSeconds = key == 'l' ? 300 : 60;
        printf("PPS MODE version=2 seconds=%lu gpio=6\n", (unsigned long)_ppsSeconds);
    }
    if (key == 'n' && application.getIdle() && _pulseDelayCapture == nullptr)
    {
        _ppsTest = false;
        _indexTest = true;
        _readTest = false;
        printf("INDEX MODE version=2 seconds=10\n");
    }
    if (key == 'h')
    {
        printf("%s ready=%u\n", _pulseDelayCapture != nullptr ?
            "PULSE_DELAY_P4 version=2" : "FSYNC_P4 version=3", unsigned(application.getIdle()));
    }
}
