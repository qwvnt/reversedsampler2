// The only file that includes Signalsmith Stretch (MIT licence, see ThirdParty/).
#include "SamplerEngine.h"

#include <cmath>

#if defined (__clang__)
 #pragma clang diagnostic push
 #pragma clang diagnostic ignored "-Weverything"
#elif defined (_MSC_VER)
 #pragma warning (push, 0)
#endif

#include <signalsmith-stretch/signalsmith-stretch.h>

#if defined (__clang__)
 #pragma clang diagnostic pop
#elif defined (_MSC_VER)
 #pragma warning (pop)
#endif

namespace rs
{

struct LayerPlayer::Stretcher
{
    // Fixed seeds: renders are repeatable. A mono sample uses the one-channel engine (half the FFT work).
    signalsmith::stretch::SignalsmithStretch<float> stereo { 0x5eedL }, mono { 0x5eedL };
    bool configured = false;

    signalsmith::stretch::SignalsmithStretch<float>& get (bool isMono) noexcept   { return isMono ? mono : stereo; }
};

namespace
{
    /** Windowed-sinc interpolation (Kaiser window). 24 taps at full bandwidth: flat to about 0.4 of
        the source rate, images and aliases above 0.55 of it at least 43 dB down, above 0.6 more
        than 80 dB (linear interpolation: about 12 dB). */
    struct SincTable
    {
        static constexpr int halfWidth = 12;                // zero crossings on each side
        static constexpr int taps = 2 * halfWidth;
        static constexpr int phases = 512;                  // tabulated fractions (interpolated between)
        static constexpr int resolution = 512;              // points per zero crossing of the wide kernel
        static constexpr double maxRatio = 4.0;             // the kernel widens up to 4x (two octaves up)
        static constexpr int maxTaps = 2 * halfWidth * (int) maxRatio + 2;
        static constexpr int widePhases = 128;              // tabulated fractions of a wide kernel
        static constexpr double cutoff = 0.93;              // of the Nyquist frequency
        static constexpr double beta = 8.0;                 // Kaiser window

        std::vector<float> polyphase;    // (phases + 1) x taps, each phase sums to 1
        std::vector<float> shape;        // the kernel from 0 to halfWidth, for wider kernels

        static double besselI0 (double x) noexcept
        {
            double sum = 1.0, term = 1.0;

            for (int k = 1; k < 50; ++k)
            {
                term *= (x * 0.5 / k) * (x * 0.5 / k);
                sum += term;

                if (term < sum * 1.0e-12)
                    break;
            }

            return sum;
        }

        static double kernel (double x) noexcept
        {
            const auto u = std::abs (x) / halfWidth;

            if (u >= 1.0)
                return 0.0;

            const auto t = juce::MathConstants<double>::pi * cutoff * x;
            const auto sinc = std::abs (t) < 1.0e-9 ? 1.0 : std::sin (t) / t;
            return cutoff * sinc * besselI0 (beta * std::sqrt (1.0 - u * u)) / besselI0 (beta);
        }

        SincTable()
        {
            polyphase.resize ((size_t) ((phases + 1) * taps));

            for (int p = 0; p <= phases; ++p)
            {
                const auto fraction = (double) p / phases;
                auto* row = polyphase.data() + p * taps;
                double sum = 0.0;

                for (int k = 0; k < taps; ++k)
                    sum += (row[k] = (float) kernel ((double) (k - halfWidth + 1) - fraction));

                for (int k = 0; k < taps; ++k)
                    row[k] = (float) (row[k] / sum);
            }

            shape.resize ((size_t) (halfWidth * resolution + 2));

            for (size_t i = 0; i < shape.size(); ++i)
                shape[i] = (float) kernel ((double) i / resolution);
        }

        static const SincTable& get()
        {
            static const SincTable table;
            return table;
        }
    };

    // Signalsmith Stretch switches to a plain pass-through after a stretch of digital
    // silence at its input. The player feeds the stretcher ahead of the output, so on a
    // squeezed region the reader runs out early and that switch would cut the last part
    // of the note. A constant offset far below the 24-bit noise floor (-140 dBFS) keeps
    // the stretcher processing to the end.
    constexpr float silenceGuard = 1.0e-7f;

    /** Gives the stretcher random access to a player's reader, from an offset onwards. */
    struct ReaderInput
    {
        const LayerPlayer* player;
        juce::int64 offset;

        struct Channel
        {
            const LayerPlayer* player;
            int channel;
            juce::int64 offset;

            float operator[] (int index) const noexcept   { return player->readSample (channel, (double) (offset + index)) + silenceGuard; }
        };

        Channel operator[] (int channel) const noexcept    { return { player, channel, offset }; }
    };
}

//==============================================================================
LayerPlayer::LayerPlayer()
    : stretcher (std::make_unique<Stretcher>())
{
    for (auto& channel : stretchOutput)
        channel.assign ((size_t) stretchChunk, 0.0f);
}

LayerPlayer::~LayerPlayer() = default;

namespace
{
    /** Weighted sum with four running totals: no long chain of dependent additions. */
    inline float dot (const float* weights, const float* x, int count) noexcept
    {
        float s0 = 0.0f, s1 = 0.0f, s2 = 0.0f, s3 = 0.0f;
        int k = 0;

        for (; k + 4 <= count; k += 4)
        {
            s0 += weights[k]     * x[k];
            s1 += weights[k + 1] * x[k + 1];
            s2 += weights[k + 2] * x[k + 2];
            s3 += weights[k + 3] * x[k + 3];
        }

        for (; k < count; ++k)
            s0 += weights[k] * x[k];

        return (s0 + s1) + (s2 + s3);
    }
}

struct LayerPlayer::Kernel
{
    alignas (16) float weights[SincTable::maxTaps];
    int first = 0, count = 0;   // keeps alignment without implicit trailing padding (MSVC C4324)
};

void LayerPlayer::prepare (double hostSampleRate)
{
    static_assert (sizeof (Kernel) == sizeof (float) * SincTable::maxTaps + 2 * sizeof (int),
                   "Kernel must stay aligned without implicit padding");
    SincTable::get();   // built here, never on the audio thread
    wideTable.assign ((size_t) ((SincTable::widePhases + 1) * SincTable::maxTaps), 0.0f);
    wideRatio = 0.0;
    active = false;
    hostRate = hostSampleRate;

    // Denser overlap reduces frame-rate modulation and transient roughness.
    // Split computation still spreads FFT work over the audio blocks.
    stretcher->stereo.presetDefault (2, (float) hostSampleRate, true);
    stretcher->mono.presetDefault (1, (float) hostSampleRate, true);
    stretcher->configured = true;

    // The reader is rendered once per chunk for the stretcher (which reads its input twice).
    const auto maxInput = (size_t) std::ceil (stretchChunk * (maxSqueeze + 1.0)) + 16;

    for (auto& channel : stretchInput)
        channel.assign (maxInput, 0.0f);

    // The note-on pre-roll reads its input region once, both channels from one kernel, instead of
    // rebuilding a sinc kernel for every random access the stretcher makes.
    const auto maxSeek = (size_t) stretcher->stereo.outputSeekLength ((float) maxSqueeze) + 16;

    for (auto& channel : seekInput)
        channel.assign (maxSeek, 0.0f);

    // Grains for Freeze: up to 120 ms, Hann window.
    const auto maxGrain = juce::jmax (64, juce::roundToInt (0.12 * hostSampleRate));
    grainWindow.resize ((size_t) maxGrain + 1);

    for (size_t i = 0; i < grainWindow.size(); ++i)
        grainWindow[i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) maxGrain);
}

void LayerPlayer::start (const SampleData& sample, const LayerTiming& newTiming, bool shouldBeReversed,
                         const Envelope& layerEnvelope, PeakHold hold, juce::int64 skipSamples) noexcept
{
    active = false;
    holding = false;
    sampleLength = sample.audio.getNumSamples();

    if (sampleLength <= 0 || hostRate <= 0.0 || newTiming.regionEnd <= newTiming.regionStart)
        return;

    channels[0] = sample.audio.getReadPointer (0);
    channels[1] = sample.audio.getReadPointer (sample.audio.getNumChannels() > 1 ? 1 : 0);
    timing = newTiming;
    reversed = shouldBeReversed;

    // The stream plays to the PEAK (Release: on to the end of the build); the layer envelope spans the whole build.
    const auto naturalLength = (juce::int64) std::ceil (timing.outputLength - 1.0e-6);
    const auto toPeak = juce::jlimit ((juce::int64) 0, naturalLength, (juce::int64) std::llround (hold.peakSamples));
    outputLength = hold.mode == AfterPeak::release ? naturalLength : toPeak;
    fullOutputLength = (double) juce::jmax ((juce::int64) 1, naturalLength);
    fadeGain = 1.0f;
    fadeStep = 0.0f;
    envelope = layerEnvelope;
    envelopeIsFlat = envelope.isFlat();
    envelopeCursor.reset();
    envelopeAtPeak = envelope.valueAt ((float) ((double) toPeak / fullOutputLength));
    peakHold = hold;

    // Freeze: the last 20 ms before the PEAK cross-fade into the grains, so the hold starts without a gap.
    holdAtPeak = hold.mode == AfterPeak::freeze && hold.reachesPeak && toPeak > 0;
    holdFadeLength = holdAtPeak ? (int) juce::jlimit ((juce::int64) 1, juce::jmax ((juce::int64) 1, toPeak / 2),
                                                      (juce::int64) juce::roundToInt (0.02 * hostRate))
                                : 1;
    peakPosition = toPeak;
    holdStart = toPeak - holdFadeLength;

    if (outputLength <= 0)
        return;

    // A build that would start before the note (moved earlier, or longer than the Arrive latency) starts part-way through.
    const auto skip = juce::jlimit ((juce::int64) 0, outputLength, skipSamples);
    outputPosition = skip;
    fadeInStart = skip;

    // Short fades where the region cuts into the sound, so the cut does not click.
    const auto declickLength = juce::jmax (1, juce::roundToInt (0.002 * hostRate));
    const bool cutAtStart = skip > 0 || (reversed ? timing.regionEnd < sampleLength : timing.regionStart > 0);
    const bool cutAtEnd   = ! holdAtPeak && (outputLength < naturalLength || (reversed ? timing.regionStart > 0 : timing.regionEnd < sampleLength));
    fadeInLength  = cutAtStart ? declickLength : 0;
    fadeOutLength = cutAtEnd   ? declickLength : 0;

    stretching = timing.useStretcher && stretcher->configured;
    stretchMono = channels[0] == channels[1];

    // Faster than the source: the wide kernel for this speed (the stream's, or else the Freeze grains').
    if (timing.readIncrement > 1.0)
        prepareWideKernel (timing.readIncrement);
    else if (holdAtPeak && timing.readIncrement * timing.transpose > 1.0)
        prepareWideKernel (timing.readIncrement * timing.transpose);

    if (skip >= outputLength)
    {
        // Arrive: the note starts after the PEAK. Freeze holds it from the start, with a short fade-in.
        if (holdAtPeak)
        {
            active = true;
            startHold (0, juce::roundToInt (0.01 * hostRate));
        }

        return;
    }

    if (stretching)
    {
        auto& engine = stretcher->get (stretchMono);
        engine.setTransposeFactor ((float) timing.transpose);

        // Pre-rolls the stretcher so its first output lines up with the start (or the skip) of the region.
        const auto inputStart = (juce::int64) std::floor ((double) skip * timing.inputPerOutput);
        const auto seekLength = engine.outputSeekLength ((float) timing.inputPerOutput);

        if (seekLength <= (int) seekInput[0].size())
        {
            fillInput (seekInput, inputStart, seekLength);
            const float* inputs[2] = { seekInput[0].data(), seekInput[1].data() };
            engine.outputSeek (inputs, seekLength);
        }
        else
        {
            engine.outputSeek (ReaderInput { this, inputStart }, seekLength);
        }

        inputFed = inputStart + seekLength;
    }

    active = true;

    // Arrive can start inside the cross-fade.
    if (holdAtPeak && skip >= holdStart)
        startHold (skip - holdStart, holdFadeLength);
}

//==============================================================================
// Freeze holds the PEAK with a cloud of four overlapping grains taken at random from
// the 300 ms of the build before it. Grains play at the layer's pitch.
void LayerPlayer::startHold (juce::int64 alreadyHeld, int fadeLength) noexcept
{
    holding = true;
    holdPosition = alreadyHeld;
    holdFadeLength = juce::jmax (1, fadeLength);

    grainRate = juce::jmax (1.0e-3, timing.transpose);   // Resample: the reader already has the pitch
    const auto readerPerOutput = stretching ? timing.inputPerOutput : 1.0;
    windowEnd = juce::jlimit (0.0, timing.readLength, (double) peakPosition * readerPerOutput);
    windowStart = juce::jmax (0.0, windowEnd - 0.3 * hostRate);

    const auto maxGrain = (int) grainWindow.size() - 1;
    grainLength = juce::jlimit (16, juce::jmax (16, maxGrain), (int) ((windowEnd - windowStart) / grainRate * 0.9));
    randomState = 0x9e3779b9u ^ (juce::uint32) peakPosition;

    for (int k = 0; k < numGrains; ++k)
    {
        auto& grain = grains[(size_t) k];
        respawnGrain (grain);
        grain.age = k * grainLength / numGrains;   // already overlapping, so the hold starts at full level
    }
}

void LayerPlayer::respawnGrain (Grain& grain) noexcept
{
    randomState ^= randomState << 13;
    randomState ^= randomState >> 17;
    randomState ^= randomState << 5;

    const auto random = (double) (randomState >> 8) / 16777216.0;
    const auto span = juce::jmax (0.0, windowEnd - windowStart - grainLength * grainRate);
    grain.position = windowStart + span * random;
    grain.age = 0;
}

void LayerPlayer::renderHold (float* left, float* right, const float* gains, int numSamples) noexcept
{
    constexpr float grainNormalisation = 0.8165f;   // four Hann grains with random phases add up to sqrt (1.5)
    const auto maxGrain = (double) (grainWindow.size() - 1);

    for (int i = 0; i < numSamples; ++i)
    {
        float l = 0.0f, r = 0.0f;

        for (auto& grain : grains)
        {
            const auto w = grainWindow[(size_t) ((double) grain.age * maxGrain / (double) grainLength)];
            float grainLeft, grainRight;
            readFrame (grain.position + (double) grain.age * grainRate, grainRate, grainLeft, grainRight);
            l += w * grainLeft;
            r += w * grainRight;

            if (++grain.age >= grainLength)
                respawnGrain (grain);
        }

        auto gain = gains[i] * fadeGain * envelopeAtPeak * grainNormalisation;

        // Equal-power fade-in, against the stream's fade-out (the grains and the stream are not correlated).
        if (holdPosition < holdFadeLength)
            gain *= std::sin (juce::MathConstants<float>::halfPi * (float) (holdPosition + 1) / (float) holdFadeLength);

        left[i]  += l * gain;
        right[i] += r * gain;
        ++holdPosition;

        if (fadeStep < 0.0f && outputPosition >= outputLength)   // during the cross-fade the stream steps the fade
        {
            fadeGain = juce::jmax (0.0f, fadeGain + fadeStep);

            if (fadeGain <= 0.0f)
            {
                active = false;
                return;
            }
        }
    }
}

void LayerPlayer::startFadeOut() noexcept
{
    if (active && hostRate > 0.0)
        fadeStep = -1.0f / (float) juce::jmax (1, juce::roundToInt (0.01 * hostRate));
}

bool LayerPlayer::makeKernel (double readerIndex, double readerStep, Kernel& kernel) const noexcept
{
    const auto position = readerIndex * timing.readIncrement;   // source samples from the playback start
    const auto regionLength = timing.regionEnd - timing.regionStart;

    if (position < 0.0 || position >= (double) regionLength)
        return false;

    // Reverse playback reads the region from its last sample towards its first: the same kernel,
    // at the mirrored position (the kernel is symmetric), so the samples are always read forwards.
    const auto forward = reversed ? (double) (regionLength - 1) - position : position;
    const auto index = (int) std::floor (forward);
    const auto fraction = forward - (double) index;
    const auto ratio = timing.readIncrement * juce::jmax (0.0, readerStep);   // source samples per output sample
    const auto& table = SincTable::get();

    // The file at its own rate: the samples themselves.
    if (juce::exactlyEqual (fraction, 0.0) && juce::exactlyEqual (ratio, 1.0))
    {
        kernel.first = index;
        kernel.count = 1;
        kernel.weights[0] = 1.0f;
        return true;
    }

    if (ratio <= 1.0)
    {
        const auto phase = fraction * SincTable::phases;
        const auto p = juce::jmin ((int) phase, SincTable::phases - 1);
        const auto mix = (float) (phase - (double) p);
        const auto* row0 = table.polyphase.data() + p * SincTable::taps;
        const auto* row1 = row0 + SincTable::taps;

        kernel.first = index - SincTable::halfWidth + 1;
        kernel.count = SincTable::taps;

        for (int k = 0; k < SincTable::taps; ++k)
            kernel.weights[k] = row0[k] + mix * (row1[k] - row0[k]);

        return true;
    }

    // Faster than the source: a wider kernel with a lower cutoff (anti-aliasing).
    if (juce::exactlyEqual (ratio, wideRatio))
    {
        const auto phase = fraction * SincTable::widePhases;
        const auto p = juce::jmin ((int) phase, SincTable::widePhases - 1);
        const auto mix = (float) (phase - (double) p);
        const auto taps = 2 * wideReach;
        const auto* row0 = wideTable.data() + p * SincTable::maxTaps;
        const auto* row1 = row0 + SincTable::maxTaps;

        kernel.first = index - wideReach + 1;
        kernel.count = taps;

        for (int k = 0; k < taps; ++k)
            kernel.weights[k] = row0[k] + mix * (row1[k] - row0[k]);

        return true;
    }

    const auto scale = 1.0 / juce::jmin (ratio, SincTable::maxRatio);
    const auto reach = (int) std::ceil (SincTable::halfWidth / scale);
    kernel.first = index - reach + 1;
    kernel.count = 2 * reach;

    double sum = 0.0;

    for (int k = 0; k < kernel.count; ++k)
    {
        const auto u = std::abs ((double) (k - reach + 1) - fraction) * scale * SincTable::resolution;
        const auto i = (int) u;
        float w = 0.0f;

        if (i < SincTable::halfWidth * SincTable::resolution)
            w = table.shape[(size_t) i] + (float) (u - (double) i) * (table.shape[(size_t) i + 1] - table.shape[(size_t) i]);

        kernel.weights[k] = w;
        sum += w;
    }

    const auto norm = sum > 1.0e-9 ? (float) (1.0 / sum) : 0.0f;

    for (int k = 0; k < kernel.count; ++k)
        kernel.weights[k] *= norm;

    return true;
}

void LayerPlayer::prepareWideKernel (double ratio) noexcept
{
    if (juce::exactlyEqual (ratio, wideRatio) || wideTable.empty())
        return;

    const auto& table = SincTable::get();
    const auto scale = 1.0 / juce::jmin (ratio, SincTable::maxRatio);
    wideReach = (int) std::ceil (SincTable::halfWidth / scale);
    const auto taps = 2 * wideReach;

    for (int p = 0; p <= SincTable::widePhases; ++p)
    {
        const auto fraction = (double) p / SincTable::widePhases;
        auto* row = wideTable.data() + p * SincTable::maxTaps;
        double sum = 0.0;

        for (int k = 0; k < taps; ++k)
        {
            const auto u = std::abs ((double) (k - wideReach + 1) - fraction) * scale * SincTable::resolution;
            const auto i = (int) u;
            float w = 0.0f;

            if (i < SincTable::halfWidth * SincTable::resolution)
                w = table.shape[(size_t) i] + (float) (u - (double) i) * (table.shape[(size_t) i + 1] - table.shape[(size_t) i]);

            row[k] = w;
            sum += w;
        }

        const auto norm = sum > 1.0e-9 ? (float) (1.0 / sum) : 0.0f;

        for (int k = 0; k < taps; ++k)
            row[k] *= norm;
    }

    wideRatio = ratio;
}

float LayerPlayer::applyKernel (const Kernel& kernel, int channel) const noexcept
{
    const auto* data = channels[channel];
    const auto last = timing.regionEnd - timing.regionStart - 1;
    float sum = 0.0f;

    if (kernel.first >= 0 && kernel.first + kernel.count - 1 <= last)
        return dot (kernel.weights, data + (timing.regionStart + kernel.first), kernel.count);

    // At the edges of the region its first and last samples carry on.
    for (int k = 0; k < kernel.count; ++k)
        sum += kernel.weights[k] * data[timing.regionStart + juce::jlimit (0, last, kernel.first + k)];

    return sum;
}

float LayerPlayer::readSample (int channel, double readerIndex, double readerStep) const noexcept
{
    Kernel kernel;
    return makeKernel (readerIndex, readerStep, kernel) ? applyKernel (kernel, channel) : 0.0f;
}

void LayerPlayer::readFrame (double readerIndex, double readerStep, float& left, float& right) const noexcept
{
    Kernel kernel;

    if (! makeKernel (readerIndex, readerStep, kernel))
    {
        left = right = 0.0f;
        return;
    }

    left = applyKernel (kernel, 0);
    right = channels[1] == channels[0] ? left : applyKernel (kernel, 1);
}

void LayerPlayer::fillInput (std::array<std::vector<float>, 2>& into, juce::int64 readerStart, int numFrames) noexcept
{
    auto* l = into[0].data();
    auto* r = into[1].data();

    for (int i = 0; i < numFrames; ++i)
    {
        float sampleLeft, sampleRight;
        readFrame ((double) (readerStart + i), 1.0, sampleLeft, sampleRight);
        l[i] = sampleLeft + silenceGuard;
        r[i] = sampleRight + silenceGuard;
    }
}

float LayerPlayer::nextLayerGain() noexcept
{
    auto gain = fadeGain;

    if (! envelopeIsFlat)
        gain *= envelopeCursor.valueAt (envelope, (float) ((double) outputPosition / fullOutputLength));

    if (outputPosition - fadeInStart < fadeInLength)
        gain *= (float) (outputPosition - fadeInStart + 1) / (float) (fadeInLength + 1);

    if (outputLength - outputPosition <= fadeOutLength)
        gain *= (float) (outputLength - outputPosition) / (float) (fadeOutLength + 1);

    // Freeze: the stream fades out while the grains fade in.
    if (holding && outputPosition >= holdStart)
        gain *= std::cos (juce::MathConstants<float>::halfPi * (float) juce::jmin ((juce::int64) holdFadeLength, outputPosition - holdStart + 1)
                          / (float) holdFadeLength);

    if (fadeStep < 0.0f)
        fadeGain = juce::jmax (0.0f, fadeGain + fadeStep);

    return gain;
}

void LayerPlayer::render (float* left, float* right, const float* gains, int numSamples) noexcept
{
    for (int done = 0; active && done < numSamples;)
    {
        // Past the PEAK, Freeze plays only the grains.
        if (holding && outputPosition >= outputLength)
        {
            renderHold (left + done, right + done, gains + done, numSamples - done);
            break;
        }

        auto chunk = (int) juce::jmin ((juce::int64) (numSamples - done), (juce::int64) stretchChunk,
                                       outputLength - outputPosition);

        // Freeze: the grains join where the cross-fade starts.
        if (holdAtPeak && ! holding)
        {
            if (outputPosition >= holdStart)
            {
                startHold (outputPosition - holdStart, holdFadeLength);
                continue;
            }

            chunk = (int) juce::jmin ((juce::int64) chunk, holdStart - outputPosition);
        }

        if (chunk <= 0)
        {
            active = false;
            continue;
        }

        const auto chunkStart = done;

        if (stretching)
        {
            // Feeds the reader to the stretcher at the speed that gives the target length.
            const auto inputBefore = (juce::int64) std::floor ((double) outputPosition * timing.inputPerOutput);
            const auto inputAfter  = (juce::int64) std::floor ((double) (outputPosition + chunk) * timing.inputPerOutput);
            const auto numInput    = (int) (inputAfter - inputBefore);

            auto& engine = stretcher->get (stretchMono);
            float* outputs[2] = { stretchOutput[0].data(), stretchOutput[stretchMono ? 0 : 1].data() };

            if (numInput <= (int) stretchInput[0].size())
            {
                fillInput (stretchInput, inputFed, numInput);
                const float* inputs[2] = { stretchInput[0].data(), stretchInput[1].data() };
                engine.process (inputs, numInput, outputs, chunk);
            }
            else
            {
                engine.process (ReaderInput { this, inputFed }, numInput, outputs, chunk);
            }

            inputFed += numInput;

            for (int i = 0; i < chunk; ++i)
            {
                const auto gain = gains[done + i] * nextLayerGain();
                left[done + i]  += outputs[0][i] * gain;
                right[done + i] += outputs[1][i] * gain;
                ++outputPosition;
            }
        }
        else
        {
            for (int i = 0; i < chunk; ++i)
            {
                float sampleLeft, sampleRight;
                readFrame ((double) outputPosition, 1.0, sampleLeft, sampleRight);
                const auto gain = gains[done + i] * nextLayerGain();
                left[done + i]  += sampleLeft * gain;
                right[done + i] += sampleRight * gain;
                ++outputPosition;
            }
        }

        // During the cross-fade the grains sound together with the stream.
        if (holding)
            renderHold (left + chunkStart, right + chunkStart, gains + chunkStart, chunk);

        done += chunk;

        if (fadeStep < 0.0f && fadeGain <= 0.0f)
            active = false;
        else if (outputPosition >= outputLength && ! holding)
            active = false;
    }
}

float LayerPlayer::getForwardPosition() const noexcept
{
    if (! active || sampleLength <= 0)
        return -1.0f;

    const auto readerIndex  = holding ? 0.5 * (windowStart + windowEnd)
                            : stretching ? (double) outputPosition * timing.inputPerOutput : (double) outputPosition;
    const auto regionLength = (double) (timing.regionEnd - timing.regionStart);
    const auto offset       = juce::jlimit (0.0, regionLength, readerIndex * timing.readIncrement);
    const auto position     = reversed ? (double) timing.regionEnd - offset : (double) timing.regionStart + offset;

    return (float) (position / (double) sampleLength);
}

} // namespace rs
