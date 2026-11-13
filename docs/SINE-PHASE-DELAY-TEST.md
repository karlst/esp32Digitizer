# Measuring ADC delay with a sine wave

Test date: 11 November  2026

## Purpose and result

This document describes the method and results of a second calibration of signal delay through the ADS127L18 digitizer. This method employed a sine wave  supplied to the input; the phase shift of the output was measured. The input and digital outputs were recorded with a four-channel Rigol DHO914S oscilloscope. The output was decoded into samples by a Python program, the sine wave was reconstructed, and its delay relative to the input was calculated. The voltage translator was excluded from this measurement. Its delay was estimated separately with the scope at approximately 3–4 ns.

Delays ranging from approximately **2.52 to 2.73 microseconds over 5–200 kHz** were measured. A decrease from approximately 2730 ns at 5 kHz to 2524 ns at 200 kHz was observed—a difference of about 206 ns.

## Connections

The Rigol DHO914S signal generator was set to 2 V peak-to-peak with a +1 V offset. The actual sine-wave voltage at CH0P was measured by scope channel 1. CH0N was grounded. Scope channels 2, 3, and 4 were connected to FSYNC, DCLK, and DOUT0 on the digitizer.

All four scope channels were recorded simultaneously:

| Scope channel | Connection | Information recorded |
| --- | --- | --- |
| CH1 | ADC analog input, CH0P | The input sine wave, measured in volts |
| CH2 | FSYNC | Beginning of each output sample |
| CH3 | DCLK | One position per data bit in the recorded waveforms |
| CH4 | DOUT0 | The bits containing status and the digitized CH0 value |

The scope was connected by USB to a Windows PC for waveform export and analysis.

## Samples and capture

ADC samples were produced at 1,024,000 per second. In each capture, 1,000,000 points per scope channel were recorded, spaced 3.2 ns apart, covering 3.2 ms. The four stored waveforms were downloaded over USB by the Python exporter. Each capture included approximately 3276 complete ADC samples that were decoded by the analysis program.

## Rebuilding the sine wave from digital outputs

The three digital signals were used together. A sequence of high and low voltages was provided by DOUT0. The positions for interpreting those voltages were identified by DCLK, and the resulting bits were divided into samples using FSYNC. The following pseudocode outlines the reconstruction algorithm. It omits details such as the 8 status bits sent with each sample and the decoding of a 24-bit sample into a signed voltage.

```text
samples = []
sample = []
previousFsync = LOW

for each rising edge in dclks:
    position = position of that edge in the arrays

    fsync = HIGH or LOW from fsyncs[position]
    bit   = 1 or 0 from douts[position]

    if fsync is HIGH and previousFsync is LOW:
        if sample is complete:
            samples.append(sample)

        sample = []

    sample.append(bit)
    previousFsync = fsync

if sample is complete:
    samples.append(sample)
```

An ADC value was produced by decoding. Its time was assigned in a separate step. All four channels were recorded on the scope’s same time axis, with points spaced 3.2 ns apart. The FSYNC rising edge associated with each word was located, and that edge’s time was assigned to the decoded value.

 A sequence of `(FSYNC time, ADC value)` pairs was obtained, directly comparable with the input waveform’s recorded times.

## Measuring the phase shift

Phase shift was measured by the Python program as follows:

- The sine frequency was estimated from channel 1. Small differences from nominal were observed in the fitted frequencies.
- A sine wave of that frequency was fitted to the input voltages.
- A sine wave of the same frequency was fitted to the decoded ADC values.

To estimate frequency, 41 frequencies within ±2% of the generator setting were first tried. The search was then refined around the frequency giving the smallest fitting error. No FFT was used. The resulting frequency was used for both input and output fits.

Three quantities were adjusted by least squares: vertical offset, amplitude, and phase. The wave’s position in its cycle at a given time was described by phase. The same part of the delayed wave’s cycle was reached later.

Least squares calculated values a and b for the following functions.  Thus, thousands of points were used in the fit rather than one peak or crossing.

```text
value(t) = offset + a × sin(2πft) + b × cos(2πft)
phase    = atan2(b, a)
```
The phase difference was converted to delay as follows, with phases in radians and frequency in Hz:
```text
delay in ns = ((input phase − output phase) / (2π × frequency)) × 10^9
```
For example, at 50 kHz a period of 20 microseconds was used. An output lag of about 45.4 degrees, or 12.6% of a cycle, was measured. A delay of approximately 2.525 microseconds was obtained.

### The measured waves

Two cycles from representative captures are plotted below. The measured input is averaged in groups of 16 scope points for display only and plotted in gray. Its fitted sine wave is plotted in green. Decoded ADC samples are plotted as blue dots, and their fitted sine wave was plotted as a blue line. Amplitudes and offsets were normalized to make the timing difference easy to see.
![Input and decoded output at 10 kHz](images/sine-phase-delay-10khz.svg)

At 10 kHz, the roughly 2.67 microsecond delay was shown as a small fraction of the 100 microsecond period.

![Input and decoded output at 50 kHz](images/sine-phase-delay-50khz.svg)

At 50 kHz, approximately the same delay was shown as a much larger fraction of the cycle, making the displacement more apparent.

### Why the result can be finer than the sample interval

The ADC supplies only one ADC value every 977 ns. The possible position of the fitted sine wave was constrained by each measured value. A small shift in the fitted wave’s time position was reflected in changed predicted values at many sample positions. Its phase was therefore determined collectively from thousands of samples more finely than the spacing between individual samples. This approach was made possible by the use of a sine-wave test signal.

### The whole-cycle ambiguity

A delay could not be distinguished from that delay plus one or more complete periods by a repeating sine wave alone. The phase difference was initially reported by the program within half a cycle in either direction. The measurements at different frequencies provide evidence that results are not off by one cycle. An extra cycle means 20 µs at 50 kHz, but 100 µs at 10 kHz and 200 µs at 5 kHz. Choosing the smallest positive delays produced consistent results around 2.5–2.7 µs across those frequencies.

However, at 200 kHz, a period of approximately 5000 ns was obtained. The initial result was −2476.1 ns. One period was added to obtain **2523.9 ns**, consistent with the neighboring 50 and 100 kHz measurements. That choice was used for the 200 kHz result below.

## Results

| Input frequency | Separate captures | Mean phase delay |
| --- | ---: | ---: |
| 5 kHz | 1 | 2729.9 ns |
| 10 kHz | 6 | 2640.7 ns |
| 20 kHz | 1 | 2559.4 ns |
| 50 kHz | 5 | 2525.8 ns |
| 100 kHz | 1 | 2523.1 ns |
| 200 kHz | 1 | 2523.9 ns, with one period added |

In the sixth 10 kHz capture, taken after the initial 50 kHz group, a delay of 2628.1 ns was obtained, within the original range. The interpretation that the difference between the 10 and 50 kHz groups was associated with frequency rather than simply elapsed time was supported by this return measurement.

Values approaching approximately 2524 ns were obtained at the higher frequencies. Individual frequency results were retained rather than averaged into a single correction. The lag of a steady sine wave at each tested frequency was described by these results. An identical delay for every feature of an arbitrary waveform was not established by these measurements.

## Checks and uncertainty

Voltages were recorded by the scope, so a dividing voltage had to be selected to distinguish digital 0 from digital 1. Decoding was repeated using dividing voltages of 0.7, 0.9, and 1.1 V. Exactly the same ADC values were obtained with all three choices. The estimated FSYNC edge times were also affected slightly by this choice; a difference of about 0.9 ns was observed in the resulting delay estimates.

Each complete word was also checked for the expected 32 bits and acceptable status bits. These checks were passed by all included captures.

Each capture was also divided into eight disjoint time segments, and the fits were repeated using the frequency estimated from the complete capture. Variation within a recording was examined with these fits; the segments were not counted as eight independent experimental repeats.

For the original groups of five separate captures:

| Frequency | Mean delay | Standard deviation between captures | 95% confidence interval for the mean |
| --- | ---: | ---: | ---: |
| 10 kHz | 2643.2 ns | 25.7 ns | 2611.4–2675.1 ns |
| 50 kHz | 2525.8 ns | 0.8 ns | 2524.8–2526.8 ns |

Student t intervals were calculated from five whole-capture estimates, assuming independent repeats with quasi distributed errors. The later return capture was excluded from the 10 kHz row so that the original five-capture group was represented.

**Repeatability was described by these intervals; a complete 95% bound on absolute accuracy was not established.** Shifts caused by probe and channel timing differences, loading, and interference in the analog measurement were not ruled out by low scatter between repeated results. Appreciable high-frequency noise was observed in the measured input trace. Its influence was reduced by fitting a sine, but the absence of resulting bias was not established.

The delay was independently placed near 2.5 microseconds, and the connection between the input and each decoded sample was made explicit. The general scale of the earlier 2553 ns calibration was supported, while its GPIO switching-threshold measurement was avoided. The voltage translator was not included, and a universal nanosecond-accurate correction for every waveform was not established.

## Code and retained evidence

- [analyzeScopeDelay.py](../espP4/test/analyzeScopeDelay.py): samples were rebuilt and FSYNC times were assigned by `decode`; frequency was estimated by `fitFrequency`; each wave was fitted by `fitSine`; the phase difference was converted to nanoseconds by `phaseDelay`.
- [exportScopeWaveforms.py](../espP4/test/exportScopeWaveforms.py): the stopped scope record for each channel was downloaded by this script.
- [Frequency means and capture locations](../captures/scope/delayFrequencyRegression.json): every capture included in the results table was identified in this file. The descriptive regression was not needed for the delay measurement itself.
- [Repeated-capture statistics](../captures/scope/delayFrequencyComparison.json): individual results and confidence intervals for the original 10 and 50 kHz groups.
- [200 kHz whole-cycle adjustment](../captures/scope/20261111-212030-249050/phaseUnwrap.json).

The exported waveforms and manifest, decoded samples, analysis results, and comparison graph were retained in each capture directory. Copies of the two corresponding capture graphs were retained with the documentation and embedded here.
