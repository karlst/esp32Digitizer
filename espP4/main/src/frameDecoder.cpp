/*
 * Copyright (C) 2026 Karl Stock
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This file is part of ESP32 Digitizer.
 * You may redistribute and/or modify it under the GNU General Public License
 * as published by the Free Software Foundation, either version 3 of the License,
 * or (at your option) any later version.
 *
 * Distributed WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General
 * Public License in the repository LICENSE file or <https://www.gnu.org/licenses/>.
 */

/**
 * @file FrameDecoder.cpp
 * Turn captured data bits into ADC numbers using the separate sample-start signal.
 * A frame is one sample's clock interval. Its start comes from FSYNC, not from
 * searching the data for a plausible value. Status errors are counted without
 * shifting the data until it looks right. This keeps errors visible to the caller.
 *
 * Reading path: feed() -> beginFrame()/appendBits() -> finishWord() ->
 * flushOutput(). Core 1's decodeTask calls feed() with a private raw block;
 * desktop tests call the same functions without any chip hardware.
 *
 * PARLIO has already captured DOUT0 (serial ADC bits) alongside FSYNC (the
 * separate frame-start wire) and DMA has moved them into RAM. Each byte holds
 * four clock instants, earliest in bits 1:0. We do not read GPIO pins here.
 * Only a captured FSYNC transition from 0 to 1 starts a frame. A continuing 1
 * does not start more frames. At 1,024,000 samples/s FSYNC is high for the first 16 clocks,
 * low for the next 16; falling halfway through does NOT end the sample.
 * DOUT0's first 8 bits are status, the next 24 the signed measurement value.
 * Thus 8 raw bytes reconstruct one 32-bit word; sdRecorder later retains only
 * 3 sample bytes. A raw block may start/end partway through a frame, so decoder
 * state persists between feed() calls. Only reset() starts a new acquisition.
 *
 * References: TI ADS127L18 datasheet section 5.7 (FSYNC half-period high/low),
 * sections 7.4.10.3-7.4.10.6 (status and data timing).
 * https://www.ti.com/lit/ds/symlink/ads127L18.pdf
 */
#include "FrameDecoder.h"
#include <array>

/* **
 * makeDataNibbles - Build a table that extracts four ADC data bits from each
 * captured byte. A nibble is four bits. The compiler calculates this table once;
 * receiving code indexes it instead of repeating the four bit-extraction
 * operations. Packed pairs have DOUT in bits 0,2,4,6. The earliest becomes
 * nibble bit 3. FSYNC bits are excluded here but still checked by the grouped
 * marker mask.
 */
static constexpr std::array<uint8_t, 256> makeDataNibbles()
{
    std::array<uint8_t, 256> retVal = {};
    for (unsigned packed = 0; packed < retVal.size(); ++packed)
    {
        retVal[packed] = static_cast<uint8_t>(((packed & 1U) << 3) |
                                              (packed & 4U) | ((packed & 16U) >> 3) | ((packed & 64U) >> 6));
    }
    return retVal;
}

static constexpr auto dataNibbles = makeDataNibbles();

/* **
 * makeBitPositions - Map a byte containing one set bit to its bit position, at
 * compile time. Entry zero is unused. This avoids a compiler-specific bit-scan
 * intrinsic so the same source builds with the P4 compiler and Microsoft's
 * desktop compiler.
 */
static constexpr std::array<uint8_t, 256> makeBitPositions()
{
    std::array<uint8_t, 256> retVal = {};
    for (unsigned bit = 0; bit < 8; ++bit)
    {
        retVal[size_t{1} << bit] = static_cast<uint8_t>(bit);
    }
    return retVal;
}

static constexpr auto bitPositions = makeBitPositions();

/* **
 * risingPosition - Return the clock position of a single rising marker in packed
 * pairs. Caller establishes that rising has exactly one set bit. FSYNC occupies
 * odd bit positions; bit 1 is clock 0, bit 3 is clock 1, through bit 31/clock
 * 15.
 */
static inline unsigned risingPosition(uint32_t rising)
{
    unsigned offset = 0;
    if ((rising & 0xffffU) == 0)
    {
        rising >>= 16;
        offset = 16;
    }
    if ((rising & 0xffU) == 0)
    {
        rising >>= 8;
        offset += 8;
    }
    const unsigned retVal = (offset + bitPositions[rising]) / 2;
    return retVal;
}

/* **
 * unpackData - Extract 16 MSB-first data bits from four packed bytes using four
 * lookups. Byte loads are safe at any address, including deliberately unaligned
 * test buffers. No pointer cast to a possibly unaligned uint32_t is required.
 */
static inline uint32_t unpackData(const uint8_t* data)
{
    const uint32_t retVal = (static_cast<uint32_t>(dataNibbles[data[0]]) << 12) |
                            (static_cast<uint32_t>(dataNibbles[data[1]]) << 8) |
                            (static_cast<uint32_t>(dataNibbles[data[2]]) << 4) | dataNibbles[data[3]];
    return retVal;
}

/* **
 * reset - Core 0 clears statistics and partial samples before a new capture.
 * Receiving must be stopped and Core 1 must have finished all queued blocks.
 * clocksPerFrame comes from the shared rate table (32 times the slot count).
 * Ignore an initial high FSYNC until a low-to-high transition is actually seen;
 * starting in the middle of a frame must not manufacture a complete sample.
 */
void FrameDecoder::reset(uint32_t clocksPerFrame)
{
    _captureStats = {};
    _outputCount = 0;
    _outputBroken = false;
    _frameClocks = clocksPerFrame;
    _lastRise = 0;
    _word = _bits = 0;
    _previousHigh = true;
    _synchronized = false;
}

/* **
 * feed - Worker consumes a private block before returning it to the free queue.
 * length is bytes, each containing four time-ordered pairs of input bits. Four
 * bytes contain sixteen clock instants. Check ALL sixteen FSYNC bits at once,
 * then append data in groups separated by a rising frame marker. A group can
 * contain a falling transition without needing separate bit handling. The normal
 * stream at 1.024M samples/s averages two groups per 32-clock frame even when a
 * frame spans group boundaries. Block tails use the bitwise path. Multiple rises
 * within sixteen clocks indicate unusual input: use clockBit() for the whole
 * group so no early frame, short word or error gets hidden. data is borrowed
 * only until this call returns; length is its valid byte count, not block
 * capacity. feed() returns no value: results accumulate in counts and complete
 * words go to the configured callback. No disk I/O occurs here.
 */
void FrameDecoder::feed(const uint8_t* data, size_t length)
{
    size_t iByte = 0;
    while (length - iByte >= 4)
    {
        const uint8_t* group = data + iByte;

        // Assemble explicitly in time order: earliest pair occupies bits 1:0.
        // Moving the marker mask left by two aligns each previous FSYNC level
        // with the current one. Bit 1 needs the preceding group's final level.
        const uint32_t packed = static_cast<uint32_t>(group[0]) |
                                (static_cast<uint32_t>(group[1]) << 8) |
                                (static_cast<uint32_t>(group[2]) << 16) |
                                (static_cast<uint32_t>(group[3]) << 24);
        // 0xaaaaaaaa is alternating 1/0 bits: keep FSYNC, remove DOUT0.
        // rising retains only positions where current FSYNC=1 and previous=0.
        // previousHigh carries the last level across groups AND raw blocks.
        const uint32_t markers = packed & 0xaaaaaaaaU;
        const uint32_t rising = markers & ~((markers << 2) | (_previousHigh ? 2U : 0U));
        if (rising == 0)
        {
            if (_synchronized && _bits < 32)
            {
                appendBits(unpackData(group), 16);
            }
            _captureStats._clocks += 16;
            _previousHigh = (markers & 0x80000000U) != 0;
        }
        else if ((rising & (rising - 1U)) == 0)
        {
            // A single rising edge splits the group into old-frame bits and
            // new-frame bits. For a rise at clock 3, append the leading three
            // bits first, start the frame, then append the remaining thirteen.
            const unsigned prefix = risingPosition(rising);
            const unsigned suffix = 16 - prefix;
            const uint32_t payload = unpackData(group);
            appendBits(payload >> suffix, prefix);
            beginFrame(_captureStats._clocks + prefix);
            appendBits(payload & ((1U << suffix) - 1U), suffix);
            _captureStats._clocks += 16;
            _previousHigh = (markers & 0x80000000U) != 0;
        }
        else
        {
            for (unsigned shift = 0; shift < 32; shift += 2)
            {
                clockBit((packed >> shift) & 3U);
            }
        }
        iByte += 4;
    }

    // Never read beyond the delivered block. Preserve incomplete frame/word
    // state so the next block resumes exactly where this one ends.
    for (; iByte < length; ++iByte)
    {
        for (unsigned shift = 0; shift < 8; shift += 2)
        {
            clockBit((data[iByte] >> shift) & 3U);
        }
    }
    // Deliver even the last short batch; no complete word waits for another DMA block.
    flushOutput();
}

/* **
 * beginFrame - Record a rising FSYNC at an absolute captured-clock position.
 * Used by both grouped and bitwise paths. An early next frame still records both
 * period and short-word errors before discarding that incomplete word. position
 * counts captured DCLK instants since reset, not CPU cycles or bytes.
 * frameClocks is 32 at 1,024,000 samples/s, larger in slower configurations.
 * Checking this spacing detects some missing/extra clocks but cannot prove none
 * were lost.
 */
void FrameDecoder::beginFrame(uint64_t position)
{
    if (_synchronized)
    {
        if (position - _lastRise != _frameClocks)
        {
            ++_captureStats._badPeriods;
        }
        if (_bits != 32)
        {
            ++_captureStats._shortWords;
        }
    }
    _lastRise = position;
    ++_captureStats._frames;
    _synchronized = true;
    _word = _bits = 0;
}

/* **
 * appendBits - Append zero to sixteen MSB-first bits, stopping at a complete
 * word. value contains exactly count relevant low bits; its highest is earliest.
 * For example, with 28 bits collected and count=16, keep the four leading bits
 * and ignore twelve padding bits. Initial unsynchronized data is also ignored.
 * finishWord runs once at 32 bits; later padding cannot produce extra samples.
 */
void FrameDecoder::appendBits(uint32_t value, unsigned count)
{
    if (_synchronized && _bits < 32 && count != 0)
    {
        const unsigned available = 32 - _bits;
        const unsigned take = count < available ? count : available;
        _word = (_word << take) | (value >> (count - take));
        _bits += take;
        if (_bits == 32)
        {
            finishWord();
        }
    }
}

/* **
 * clockBit - Process the two pin values already captured on one rising DCLK
 * edge. pair bit 0 is ADC data and bit 1 is FSYNC. This reads RAM, not the pins.
 * ADC data change on falling DCLK; the rising edge reads the settled value.
 * FSYNC's rising transition includes the first data bit in the current ADC
 * configuration. A period different from the configured frame length is an
 * error, even when the next header happens to look valid.
 */
void FrameDecoder::clockBit(unsigned pair)
{
    const bool high = (pair & 2U) != 0;
    if (high && !_previousHigh)
    {
        beginFrame(_captureStats._clocks);
    }
    if (_synchronized && _bits < 32)
    {
        _word = (_word << 1) | (pair & 1U);
        ++_bits;
        if (_bits == 32)
        {
            finishWord();
        }
    }
    _previousHigh = high;
    ++_captureStats._clocks;
}

/* **
 * finishWord - Count a complete received word, keeping invalid headers visible.
 * The first transmitted byte becomes word bits 31:24. TI STATUS_DP layout: bit 7
 * PWR_FLAG: power event since its underlying flags were cleared. bit 6 ERR_FLAG:
 * ADC error indication (not a complete explanation of its cause). bit 5
 * MOD_FLAG: modulator saturation inside the ADC conversion circuitry. bit 4
 * RPT_DATA: repeated conversion data, not merely equal numeric samples. bit 3
 * PWDN: 1 means NORMAL operation; 0 means power-down/standby. bits 2:0 CH_ID:
 * which ADC channel produced this word, numbered 0 through 7.
 *
 * Header mask 0x5f selects bits 6,4,3,2,1,0. Expected masked value 0x08 means no
 * error, new data, normal operation, channel zero. Power and saturation bits are
 * deliberately excluded from badHeaders, but included in headers[] counts. The
 * usual clean byte is 0x08; 0x88/0x28 also pass THIS mask. That does not make a
 * power event or saturated analog measurement harmless or correct.
 *
 * Current test policy: count one badHeaders per rejected header regardless of
 * how many bits are wrong; retain the first eight complete bad words. Continue
 * counting and forwarding their sample values: no automatic stop or filtering
 * occurs here. stopCapture() later invalidates the recording if badHeaders!=0.
 * The histogram records how often each byte occurred, not its sample position.
 * Per-sample status is omitted from the SD payload. Production responses and
 * persistent quality/event reporting are still undecided (repository TODO.md).
 * The 24-bit two's-complement sample is expanded to signed 32-bit arithmetic;
 * e.g. 0xffffff means -1, not 16777215. This does not check payload correctness.
 */
void FrameDecoder::finishWord()
{
    const unsigned header = _word >> 24;

    // The 256 histogram entries cover every possible status byte. This retains
    // excluded power/saturation flags for reporting even when badHeaders stays 0.
    ++_captureStats._words;
    ++_captureStats._headers[header];
    if ((header & 0x5fU) != 0x08U)
    {
        if (_captureStats._badHeaders < 8)
        {
            _captureStats._badExamples[_captureStats._badHeaders] = _word;
        }
        ++_captureStats._badHeaders;
    }

    // Interpret bit 23 as the sign. These latest values are diagnostics; the
    // output batch below carries the original status+sample word without changes.
    _captureStats._latestWord = _word;
    const int32_t value = static_cast<int32_t>(_word & 0xffffffU);
    _captureStats._latestSample = (value & 0x800000) ? value - 0x1000000 : value;

    // At most 512 complete words per callback. A failure stops later submissions
    // but not decoding/counters, so a storage problem cannot hide input errors.
    if (_output != nullptr && !_outputBroken)
    {
        _outputWords[_outputCount++] = _word;
        if (_outputCount == 512)
        {
            flushOutput();
        }
    }
}

/* **
 * setOutput - Choose the function that receives completed sample batches, while
 * stopped. callback is that function; context is the caller's optional pointer,
 * passed back unchanged. A null callback disables delivery while retaining
 * counting. The callback must copy the words before returning; this array is
 * reused. A false return records a failure until reset; subsequent words remain
 * counted.
 */
void FrameDecoder::setOutput(outputCallback callback, void* context)
{
    _output = callback;
    _outputContext = context;
}

/* **
 * flushOutput - Synchronously offer the completed words to the configured
 * receiver. In the P4 program output is recordWords(), which either copies
 * packed sample bytes into the SD ring or updates the RAM waveform summary
 * before returning. Therefore this outputWords array can be reused immediately;
 * it is not a queued pointer to the raw input block. A false result latches
 * outputBroken until reset().
 */
void FrameDecoder::flushOutput()
{
    if (_outputCount != 0)
    {
        _outputBroken = !_output(_outputWords, _outputCount, _outputContext);
        _outputCount = 0;
    }
}

/* **
 * getOutputFailed - True if a batch was refused since reset(); later batches are
 * then withheld. Call on the decoder task or after it has finished all queued
 * blocks.
 */
bool FrameDecoder::getOutputFailed() const
{
    return _outputBroken;
}

/* **
 * getCaptureStats - Borrow the counters without copying them; decoding can
 * change them. Core 0 must wait for all queued decoding to finish before reading
 * this reference.
 */
const CaptureStats& FrameDecoder::getCaptureStats() const
{
    return _captureStats;
}

/* **
 * getPendingBits - Return unfinished word bits at the captured tail, or zero if
 * complete.
 */
uint32_t FrameDecoder::getPendingBits() const
{
    return _synchronized && _bits < 32 ? _bits : 0;
}
