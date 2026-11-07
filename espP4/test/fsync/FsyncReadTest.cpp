/* Copyright (C) 2026 Karl Stock. SPDX-License-Identifier: GPL-3.0-or-later
 * Read reset, measure, then report. The measured comparison is count/time,
 * count/time, with the same register order for each pair. No serial printing
 * occurs during acquisition. CPU cycles measure software elapsed time; MCPWM
 * ticks are the separate hardware timestamps being compared.
 */
#include "FsyncReadTest.h"
#include "esp_cpu.h"
#include "esp_rom_sys.h"
#include "hal/gpio_ll.h"
#include "hal/pcnt_ll.h"
#include "hal/mcpwm_ll.h"
#include "sdkconfig.h"
#include <algorithm>
#include <cstdio>

static_assert(CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ == 360, "Review cycle conversion for this CPU clock");
#if CONFIG_PM_ENABLE
#error "Read timing requires a fixed CPU frequency"
#endif

/* **
 * reset - Core 0 clears a completed run and measures the minimum empty timing
 * bracket over 32 trials. This overhead is reported but never subtracted from
 * acceptance tests, keeping their elapsed-time bound conservative.
 */
void FsyncReadTest::reset()
{
    for (unsigned iLevel = 0; iLevel < 3; ++iLevel)
    {
        _attempts[iLevel] = _inconsistent[iLevel] = _slow[iLevel] = _rejected[iLevel] = 0;
    }
    _readTrials = 0;
    for (unsigned iRead = 0; iRead < 8; ++iRead)
    {
        _readMin[iRead] = UINT32_MAX;
        _readMax[iRead] = 0;
        _readTotal[iRead] = 0;
    }
    _sets = _exhausted = _allInconsistent = _maxAttempts = _same = _bothChanged = 0;
    _comparisonMin = _setMin = _overhead = UINT32_MAX;
    _comparisonMax = _setMax = 0;
    _comparisonTotal = _setTotal = 0;
    for (unsigned iTrial = 0; iTrial < 32; ++iTrial)
    {
        const uint32_t begin = esp_cpu_get_cycle_count();
        asm volatile("fence iorw, iorw" ::: "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        const uint32_t end = esp_cpu_get_cycle_count();
        _overhead = std::min(_overhead, uint32_t(end - begin));
    }
}

/* **
 * measure - Called by Core 0's roughly one-millisecond application poll after
 * decoded data confirms the ADC is running. A changing 0..31 us delay moves
 * observations relative to FSYNC; interrupts remain enabled during that delay.
 * Each attempt compares two fresh pairs, rather than reusing an old accepted
 * baseline across milliseconds. Both fields must change or both must stay put.
 *
 * The critical section masks local interrupts only for each short comparison;
 * ADC hardware and Core 1 decoding continue. RISC-V fences order peripheral
 * reads with the CPU cycle counter. Unsigned subtraction handles counter wrap.
 * 360 MHz / 1.024 MHz = 351.5625 cycles per FSYNC: 352 or more is rejected.
 * GPIO levels label results but do not reject HIGH. The timing bracket contains
 * the four register reads and two boundary fences only, with no inter-read
 * fences. ESP32-P4 v1.3 TRM section 1.6.5.2 guarantees memory ordering; SDK
 * volatile register accesses preserve source read order. Boundary fences remain
 * for cycle-counter measurement. Comparison and statistics occur afterward.
 * Set timing includes GPIO reads, retry logic, lock entry/exit and intervening
 * interrupts. Reasons can overlap. The consistency rule is still an experiment,
 * not independent proof that both hardware fields belong to the same edge.
 */
void FsyncReadTest::measure()
{
    _random ^= _random << 13;
    _random ^= _random >> 17;
    _random ^= _random << 5;
    esp_rom_delay_us(_random & 31);
    measureReadTimes();
    const uint32_t setBegin = esp_cpu_get_cycle_count();
    bool accepted = false;
    unsigned attempts = 0;
    bool allInconsistent = true;
    while (attempts < 32 && !accepted)
    {
        portENTER_CRITICAL(&_mux);

        // Level observations surround, but are not part of, the timed reads.
        // Compiler barriers keep them outside the CPU-counter bracket. They
        // classify endpoints only; equal levels do not prove no edge occurred.
        const int highBefore = gpio_ll_get_level(GPIO_LL_GET_HW(0), 1);
        asm volatile("" : : "r"(highBefore) : "memory");
        const uint32_t begin = esp_cpu_get_cycle_count();
        asm volatile("fence iorw, iorw" ::: "memory");
        const int countFirst = pcnt_ll_get_count(PCNT_LL_GET_HW(0), 0);
        const uint32_t tickFirst = mcpwm_ll_capture_get_value(MCPWM_LL_GET_HW(0), 0);
        const int countLast = pcnt_ll_get_count(PCNT_LL_GET_HW(0), 0);
        const uint32_t tickLast = mcpwm_ll_capture_get_value(MCPWM_LL_GET_HW(0), 0);
        asm volatile("" : : "r"(countFirst), "r"(tickFirst),
            "r"(countLast), "r"(tickLast) : "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        const uint32_t end = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        const int highAfter = gpio_ll_get_level(GPIO_LL_GET_HW(0), 1);
        portEXIT_CRITICAL(&_mux);

        // Use raw counter equality only: a wrap counts as a change. No pulse
        // number extension or timestamp-to-sample association is needed here.
        const uint32_t cycles = end - begin;
        const unsigned iLevel = highBefore != highAfter ? 2 : unsigned(highBefore);
        const bool countChanged = countFirst != countLast;
        const bool tickChanged = tickFirst != tickLast;
        const bool inconsistent = countChanged != tickChanged;
        const bool slow = cycles >= 352;
        allInconsistent = allInconsistent && inconsistent;
        accepted = !inconsistent && !slow;
        ++attempts;
        ++_attempts[iLevel];
        _inconsistent[iLevel] += inconsistent;
        _slow[iLevel] += slow;
        _rejected[iLevel] += !accepted;
        _same += accepted && !countChanged;
        _bothChanged += accepted && countChanged;
        _comparisonMin = std::min(_comparisonMin, cycles);
        _comparisonMax = std::max(_comparisonMax, cycles);
        _comparisonTotal += cycles;
    }
    const uint32_t setCycles = esp_cpu_get_cycle_count() - setBegin;
    ++_sets;
    _exhausted += !accepted;
    _allInconsistent += !accepted && allInconsistent;
    _maxAttempts = std::max(_maxAttempts, uint32_t(attempts));
    _setMin = std::min(_setMin, setCycles);
    _setMax = std::max(_setMax, setCycles);
    _setTotal += setCycles;
}

/* **
 * measureReadTimes - Core 0 measures individual reads separately from the
 * shortened retry experiment. Each bracket has its own critical section;
 * interrupt masking entry/exit, GPIO reads, comparisons and statistics are all
 * outside its elapsed time. ADC hardware and Core 1 decoding continue running.
 *
 * Cases 0..2 measure the CPU-counter bracket alone, with two boundary fences,
 * and with five fences. A fence orders hardware accesses; its cost with pending
 * peripheral reads can differ from its empty cost, so subtraction is only an
 * estimate. Cases 3..6 have boundary fences but no fences between reads. Case 7
 * adds three inter-read fences, matching the older ordered four-read sequence.
 * These cost measurements do not check same-edge association. The retry test
 * uses the boundary-only four-read sequence and separately checks consistency.
 *
 * Volatile SDK register loads actually perform the reads. Empty compiler
 * barriers consume their results before the ending counter read, retaining
 * value extraction without storing results or comparing them inside the timing
 * bracket. They generate no CPU instruction. Unsigned subtraction handles a
 * CPU-counter wrap. Fixed straight-line blocks keep case selection and loops
 * outside timing; all cases run once per poll. Statistics are internal RAM.
 */
void FsyncReadTest::measureReadTimes()
{
    uint32_t elapsed[8] = {};

    // Counter bracket only.
    {
        portENTER_CRITICAL(&_mux);
        asm volatile("" ::: "memory");
        const uint32_t begin = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        const uint32_t end = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        portEXIT_CRITICAL(&_mux);
        elapsed[0] = end - begin;
    }

    // Empty bracket with two boundary fences.
    {
        portENTER_CRITICAL(&_mux);
        asm volatile("" ::: "memory");
        const uint32_t begin = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        const uint32_t end = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        portEXIT_CRITICAL(&_mux);
        elapsed[1] = end - begin;
    }

    // Empty bracket with five fences.
    {
        portENTER_CRITICAL(&_mux);
        asm volatile("" ::: "memory");
        const uint32_t begin = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        const uint32_t end = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        portEXIT_CRITICAL(&_mux);
        elapsed[2] = end - begin;
    }

    // One pulse-count read.
    {
        portENTER_CRITICAL(&_mux);
        asm volatile("" ::: "memory");
        const uint32_t begin = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        const int countFirst = pcnt_ll_get_count(PCNT_LL_GET_HW(0), 0);
        asm volatile("" : : "r"(countFirst) : "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        const uint32_t end = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        portEXIT_CRITICAL(&_mux);
        elapsed[3] = end - begin;
    }

    // One captured-timestamp read.
    {
        portENTER_CRITICAL(&_mux);
        asm volatile("" ::: "memory");
        const uint32_t begin = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        const uint32_t tickFirst = mcpwm_ll_capture_get_value(MCPWM_LL_GET_HW(0), 0);
        asm volatile("" : : "r"(tickFirst) : "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        const uint32_t end = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        portEXIT_CRITICAL(&_mux);
        elapsed[4] = end - begin;
    }

    // One count/timestamp pair.
    {
        portENTER_CRITICAL(&_mux);
        asm volatile("" ::: "memory");
        const uint32_t begin = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        const int countFirst = pcnt_ll_get_count(PCNT_LL_GET_HW(0), 0);
        const uint32_t tickFirst = mcpwm_ll_capture_get_value(MCPWM_LL_GET_HW(0), 0);
        asm volatile("" : : "r"(countFirst), "r"(tickFirst) : "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        const uint32_t end = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        portEXIT_CRITICAL(&_mux);
        elapsed[5] = end - begin;
    }

    // Two count/timestamp pairs, boundary fences only.
    {
        portENTER_CRITICAL(&_mux);
        asm volatile("" ::: "memory");
        const uint32_t begin = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        const int countFirst = pcnt_ll_get_count(PCNT_LL_GET_HW(0), 0);
        const uint32_t tickFirst = mcpwm_ll_capture_get_value(MCPWM_LL_GET_HW(0), 0);
        const int countLast = pcnt_ll_get_count(PCNT_LL_GET_HW(0), 0);
        const uint32_t tickLast = mcpwm_ll_capture_get_value(MCPWM_LL_GET_HW(0), 0);
        asm volatile("" : : "r"(countFirst), "r"(tickFirst), "r"(countLast), "r"(tickLast) : "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        const uint32_t end = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        portEXIT_CRITICAL(&_mux);
        elapsed[6] = end - begin;
    }

    // Two pairs with three additional inter-read fences.
    {
        portENTER_CRITICAL(&_mux);
        asm volatile("" ::: "memory");
        const uint32_t begin = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        const int countFirst = pcnt_ll_get_count(PCNT_LL_GET_HW(0), 0);
        asm volatile("fence iorw, iorw" ::: "memory");
        const uint32_t tickFirst = mcpwm_ll_capture_get_value(MCPWM_LL_GET_HW(0), 0);
        asm volatile("fence iorw, iorw" ::: "memory");
        const int countLast = pcnt_ll_get_count(PCNT_LL_GET_HW(0), 0);
        asm volatile("fence iorw, iorw" ::: "memory");
        const uint32_t tickLast = mcpwm_ll_capture_get_value(MCPWM_LL_GET_HW(0), 0);
        asm volatile("" : : "r"(countFirst), "r"(tickFirst), "r"(countLast), "r"(tickLast) : "memory");
        asm volatile("fence iorw, iorw" ::: "memory");
        const uint32_t end = esp_cpu_get_cycle_count();
        asm volatile("" ::: "memory");
        portEXIT_CRITICAL(&_mux);
        elapsed[7] = end - begin;
    }

    // Accumulation occurs after all timed blocks; no printing while receiving.
    ++_readTrials;
    for (unsigned iRead = 0; iRead < 8; ++iRead)
    {
        _readMin[iRead] = std::min(_readMin[iRead], elapsed[iRead]);
        _readMax[iRead] = std::max(_readMax[iRead], elapsed[iRead]);
        _readTotal[iRead] += elapsed[iRead];
    }
}

/* **
 * report - After decoding drains, print counts and percentages, then conservative
 * elapsed times in nanoseconds. clean describes acquisition integrity, not a
 * required rejection rate. Zero observations invalidate the run. A finite run
 * cannot establish worst-case latency or prove same-edge hardware association.
 */
void FsyncReadTest::report(uint32_t run, bool clean) const
{
    // Raw times are primary evidence. Subtracting the matching empty bracket
    // estimates added read work; it cannot remove bus-dependent fence waiting.
    static const char* const names[8] = {"COUNTER_ONLY", "EMPTY_2_FENCES", "EMPTY_5_FENCES",
        "COUNT", "TIMESTAMP", "ONE_PAIR", "TWO_PAIRS", "TWO_PAIRS_ORDERED"};
    for (unsigned iRead = 0; iRead < 8; ++iRead)
    {
        const unsigned iBaseline = iRead == 7 ? 2 : 1;
        const double mean = _readTrials ? double(_readTotal[iRead]) / _readTrials : 0.0;
        const double baseline = _readTrials ? double(_readTotal[iBaseline]) / _readTrials : 0.0;
        printf("READ COST case=%s trials=%lu minCycles=%lu meanCycles=%.3f maxCycles=%lu minNs=%.3f meanNs=%.3f maxNs=%.3f\n",
            names[iRead], (unsigned long)_readTrials, (unsigned long)(_readTrials ? _readMin[iRead] : 0),
            mean, (unsigned long)_readMax[iRead], _readTrials ? _readMin[iRead] / 0.36 : 0.0,
            mean / 0.36, _readMax[iRead] / 0.36);
        if (iRead >= 3)
        {
            printf("READ ESTIMATE case=%s baseline=%s addedMeanNs=%.3f\n",
                names[iRead], names[iBaseline], (mean - baseline) / 0.36);
        }
    }
    printf("READ COST_LIMIT: raw brackets include fences; subtraction is approximate; cost measurements do not prove same-edge association\n");
    uint32_t attempts = 0, rejected = 0, inconsistent = 0;
    for (unsigned iLevel = 0; iLevel < 3; ++iLevel)
    {
        attempts += _attempts[iLevel];
        rejected += _rejected[iLevel];
        inconsistent += _inconsistent[iLevel];
        printf("READ LEVEL level=%s attempts=%lu inconsistent=%lu slow=%lu rejected=%lu rejectedPct=%.6f\n",
            iLevel == 0 ? "LOW" : iLevel == 1 ? "HIGH" : "TRANSITION",
            (unsigned long)_attempts[iLevel], (unsigned long)_inconsistent[iLevel],
            (unsigned long)_slow[iLevel], (unsigned long)_rejected[iLevel],
            _attempts[iLevel] ? 100.0 * _rejected[iLevel] / _attempts[iLevel] : 0.0);
    }
    printf("READ TOTAL sets=%lu exhausted=%lu exhaustedPct=%.6f attempts=%lu rejected=%lu rejectedPct=%.6f meanAttempts=%.3f maxAttempts=%lu same=%lu bothChanged=%lu\n",
        (unsigned long)_sets, (unsigned long)_exhausted, _sets ? 100.0 * _exhausted / _sets : 0.0,
        (unsigned long)attempts, (unsigned long)rejected, attempts ? 100.0 * rejected / attempts : 0.0,
        _sets ? double(attempts) / _sets : 0.0, (unsigned long)_maxAttempts,
        (unsigned long)_same, (unsigned long)_bothChanged);
    printf("READ CONSISTENCY inconsistent=%lu inconsistentPct=%.6f all32Inconsistent=%lu all32InconsistentPct=%.6f\n",
        (unsigned long)inconsistent, attempts ? 100.0 * inconsistent / attempts : 0.0,
        (unsigned long)_allInconsistent, _sets ? 100.0 * _allInconsistent / _sets : 0.0);
    printf("READ TIME cpuMHz=360 emptyCycles=%lu comparisonMinNs=%.3f comparisonMeanNs=%.3f comparisonMaxNs=%.3f setMinNs=%.3f setMeanNs=%.3f setMaxNs=%.3f\n",
        (unsigned long)_overhead, attempts ? _comparisonMin / 0.36 : 0.0,
        attempts ? double(_comparisonTotal) / attempts / 0.36 : 0.0, _comparisonMax / 0.36,
        _sets ? _setMin / 0.36 : 0.0, _sets ? double(_setTotal) / _sets / 0.36 : 0.0, _setMax / 0.36);
    printf("READ LIMIT: consistency only; reasons overlap; HIGH is not rejected; SD load depends on selected mode; measured maxima are not worst-case bounds\n");
    printf("READ ACQUISITION status=%s consistencyExhausted=%lu\n", clean && _sets != 0 ? "CLEAN" : "INVALID", (unsigned long)_exhausted);
    printf("READ RESULT run=%lu status=%s\n", (unsigned long)run, clean && _sets != 0 && _readTrials != 0 ? "MEASURED" : "FAIL");
}
