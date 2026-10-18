/** @file frameDecoder.cpp
 * @brief Fixed framing; never search for a convenient header or shift bad data.
 */
#include "frameDecoder.h"
#include <array>

/**
 * @brief Build the byte-to-data-nibble lookup at compile time, not during capture.
 * Packed pairs have DOUT in bits 0,2,4,6. The earliest becomes nibble bit 3.
 * FSYNC bits are excluded here but still checked by the grouped marker mask.
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

/**
 * @brief Map a byte containing one set bit to its bit position, at compile time.
 * Entry zero is unused. This avoids a compiler-specific bit-scan intrinsic so
 * the same source builds with the P4 compiler and Microsoft's desktop compiler.
 */
static constexpr std::array<uint8_t, 256> makeBitPositions()
{
    std::array<uint8_t, 256> retVal = {};
    for (unsigned bit = 0; bit < 8; ++bit)
    {
        retVal[1U << bit] = static_cast<uint8_t>(bit);
    }
    return retVal;
}
static constexpr auto bitPositions = makeBitPositions();

/**
 * @brief Return the clock position of a single rising marker in packed pairs.
 * Caller establishes that rising has exactly one set bit. FSYNC occupies odd
 * bit positions; bit 1 is clock 0, bit 3 is clock 1, through bit 31/clock 15.
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

/**
 * @brief Extract 16 MSB-first data bits from four packed bytes using four lookups.
 * Byte loads are safe at any address, including deliberately unaligned test
 * buffers. No pointer cast to a possibly unaligned uint32_t is required.
 */
static inline uint32_t unpackData(const uint8_t* data)
{
    const uint32_t retVal = (static_cast<uint32_t>(dataNibbles[data[0]]) << 12) |
        (static_cast<uint32_t>(dataNibbles[data[1]]) << 8) |
        (static_cast<uint32_t>(dataNibbles[data[2]]) << 4) | dataNibbles[data[3]];
    return retVal;
}

/**
 * @brief Control task resets only while the worker is drained and DMA stopped.
 * clocksPerFrame comes from the shared rate table (32 times the slot count).
 * Ignore an initial high FSYNC until a low-to-high transition is actually seen;
 * starting in the middle of a frame must not manufacture a complete sample.
 */
void frameDecoder::reset(uint32_t clocksPerFrame)
{
    counts = {};
    outputCount = 0;
    outputBroken = false;
    frameClocks = clocksPerFrame;
    lastRise = 0;
    word = bits = 0;
    previousHigh = true;
    synchronized = false;
}

/**
 * @brief Worker consumes a private block before returning it to the free queue.
 * length is bytes, each containing four time-ordered pairs of input bits.
 * Four bytes contain sixteen clock instants. Check ALL sixteen FSYNC bits at
 * once, then append data in groups separated by a rising frame marker. A group
 * can contain a falling transition without needing separate bit handling.
 * The normal stream at 1.024M samples/s averages two groups per 32-clock frame
 * even when a frame spans group boundaries. Block tails use the bitwise path.
 * Multiple rises within sixteen clocks indicate unusual input: use clockBit()
 * for the whole group so no early frame, short word or error gets hidden.
 */
void frameDecoder::feed(const uint8_t* data, size_t length)
{
    size_t index = 0;
    while (length - index >= 4)
    {
        const uint8_t* group = data + index;

        // Assemble explicitly in time order: earliest pair occupies bits 1:0.
        // Moving the marker mask left by two aligns each previous FSYNC level
        // with the current one. Bit 1 needs the preceding group's final level.
        const uint32_t packed = static_cast<uint32_t>(group[0]) |
            (static_cast<uint32_t>(group[1]) << 8) |
            (static_cast<uint32_t>(group[2]) << 16) |
            (static_cast<uint32_t>(group[3]) << 24);
        const uint32_t markers = packed & 0xaaaaaaaaU;
        const uint32_t rising = markers & ~((markers << 2) | (previousHigh ? 2U : 0U));
        if (rising == 0)
        {
            if (synchronized && bits < 32)
            {
                appendBits(unpackData(group), 16);
            }
            counts.clocks += 16;
            previousHigh = (markers & 0x80000000U) != 0;
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
            beginFrame(counts.clocks + prefix);
            appendBits(payload & ((1U << suffix) - 1U), suffix);
            counts.clocks += 16;
            previousHigh = (markers & 0x80000000U) != 0;
        }
        else
        {
            for (unsigned shift = 0; shift < 32; shift += 2)
            {
                clockBit((packed >> shift) & 3U);
            }
        }
        index += 4;
    }

    // Never read beyond the delivered block. Preserve incomplete frame/word
    // state so the next block resumes exactly where this one ends.
    for (; index < length; ++index)
    {
        for (unsigned shift = 0; shift < 8; shift += 2)
        {
            clockBit((data[index] >> shift) & 3U);
        }
    }
    // Deliver even the last short batch; no complete word waits for another DMA block.
    flushOutput();
}

/**
 * @brief Record a rising FSYNC at an absolute captured-clock position.
 * Used by both grouped and bitwise paths. An early next frame still records
 * both period and short-word errors before discarding that incomplete word.
 */
void frameDecoder::beginFrame(uint64_t position)
{
    if (synchronized)
    {
        if (position - lastRise != frameClocks) { ++counts.badPeriods; }
        if (bits != 32) { ++counts.shortWords; }
    }
    lastRise = position;
    ++counts.frames;
    synchronized = true;
    word = bits = 0;
}

/**
 * @brief Append zero to sixteen MSB-first bits, stopping at a complete word.
 * value contains exactly count relevant low bits; its highest is earliest.
 * For example, with 28 bits collected and count=16, keep the four leading bits
 * and ignore twelve padding bits. Initial unsynchronized data is also ignored.
 * finishWord runs once at 32 bits; later padding cannot produce extra samples.
 */
void frameDecoder::appendBits(uint32_t value, unsigned count)
{
    if (synchronized && bits < 32 && count != 0)
    {
        const unsigned available = 32 - bits;
        const unsigned take = count < available ? count : available;
        word = (word << take) | (value >> (count - take));
        bits += take;
        if (bits == 32) { finishWord(); }
    }
}

/**
 * @brief Consume one rising DCLK edge (a low-to-high clock transition).
 * ADC data change on falling DCLK; the rising edge reads the settled value.
 * FSYNC's rising transition includes the first data bit in the current ADC
 * configuration. A period different from the configured frame length is an
 * error, even when the next header happens to look valid.
 */
void frameDecoder::clockBit(unsigned pair)
{
    const bool high = (pair & 2U) != 0;
    if (high && !previousHigh)
    {
        beginFrame(counts.clocks);
    }
    if (synchronized && bits < 32)
    {
        word = (word << 1) | (pair & 1U);
        ++bits;
        if (bits == 32) { finishWord(); }
    }
    previousHigh = high;
    ++counts.clocks;
}

/**
 * @brief Count a complete received word, keeping invalid headers visible.
 * Header mask 0x5f checks error, repeated data, channel-active and channel ID.
 * Power/modulator flags are retained in the histogram, as in the S3 diagnostic.
 * The 24-bit two's-complement sample is expanded to signed 32-bit arithmetic;
 * e.g. 0xffffff means -1, not 16777215. This does not check payload correctness.
 */
void frameDecoder::finishWord()
{
    const unsigned header = word >> 24;
    ++counts.words;
    ++counts.headers[header];
    if ((header & 0x5fU) != 0x08U)
    {
        if (counts.badHeaders < 8) { counts.badExamples[counts.badHeaders] = word; }
        ++counts.badHeaders;
    }
    counts.latestWord = word;
    const int32_t value = static_cast<int32_t>(word & 0xffffffU);
    counts.latestSample = (value & 0x800000) ? value - 0x1000000 : value;
    if (output != nullptr && !outputBroken)
    {
        outputWords[outputCount++] = word;
        if (outputCount == 512) { flushOutput(); }
    }
}

/** @brief Configure an optional nonblocking batch receiver while decoding is stopped.
 * The callback must copy the words before returning; this array is reused.
 * A false return latches failure until reset; subsequent words remain counted.
 */
void frameDecoder::setOutput(outputCallback callback, void* context)
{
    output = callback;
    outputContext = context;
}

/** @brief Submit the current batch without retaining a pointer in the receiver. */
void frameDecoder::flushOutput()
{
    if (outputCount != 0)
    {
        outputBroken = !output(outputWords, outputCount, outputContext);
        outputCount = 0;
    }
}

/** @brief Read the output failure latch only on the decoder task or after draining. */
bool frameDecoder::outputFailed() const
{
    return outputBroken;
}

/** @brief Return worker-owned counters; the caller may read only after draining. */
const captureStats& frameDecoder::stats() const
{
    return counts;
}

/** @brief Return unfinished word bits at the captured tail, or zero if complete. */
uint32_t frameDecoder::pendingBits() const
{
    return synchronized && bits < 32 ? bits : 0;
}
