#include "SamplerEngine.h"

#include <cmath>
#include <cstring>
#include <limits>

namespace rs
{

//==============================================================================
void SampleData::buildOverview (int maxNumBins)
{
    const int numSamples = audio.getNumSamples();
    const int numBins = juce::jmax (1, juce::jmin (maxNumBins, numSamples));

    overviewMin.assign ((size_t) numBins, 0.0f);
    overviewMax.assign ((size_t) numBins, 0.0f);
    overviewPeak = 0.0f;

    if (numSamples <= 0)
        return;

    for (int bin = 0; bin < numBins; ++bin)
    {
        const auto start = (int) ((juce::int64) bin * numSamples / numBins);
        const auto end   = juce::jmax (start + 1, (int) ((juce::int64) (bin + 1) * numSamples / numBins));

        float low = 0.0f, high = 0.0f;

        for (int channel = 0; channel < audio.getNumChannels(); ++channel)
        {
            const auto range = audio.findMinMax (channel, start, end - start);
            low  = juce::jmin (low,  range.getStart());
            high = juce::jmax (high, range.getEnd());
        }

        overviewMin[(size_t) bin] = low;
        overviewMax[(size_t) bin] = high;
        overviewPeak = juce::jmax (overviewPeak, -low, high);
    }
}

// Onsets: an RMS curve in 5 ms frames; an onset is a frame at least 9 dB louder than
// the four frames before it, no quieter than 40 dB under the loudest frame, and at
// least 50 ms after the previous onset. The position is refined to the sample where
// the waveform first reaches a quarter of the attack's level.
void SampleData::detectOnsets()
{
    onsets.clear();

    const int numSamples = audio.getNumSamples();
    const int numChannels = audio.getNumChannels();
    const int hop = juce::jmax (16, juce::roundToInt (0.005 * sampleRate));
    const int numFrames = numSamples / hop;

    if (numFrames < 6 || numChannels <= 0)
        return;

    std::vector<double> power ((size_t) numFrames, 0.0);
    double loudest = 0.0;

    for (int f = 0; f < numFrames; ++f)
    {
        double sum = 0.0;

        for (int channel = 0; channel < numChannels; ++channel)
        {
            const auto* data = audio.getReadPointer (channel, f * hop);

            for (int i = 0; i < hop; ++i)
                sum += (double) data[i] * (double) data[i];
        }

        power[(size_t) f] = sum / (double) (hop * numChannels);
        loudest = juce::jmax (loudest, power[(size_t) f]);
    }

    if (loudest <= 1.0e-12)
        return;

    constexpr double riseFactor = 7.943;    // +9 dB in power
    const auto floorPower = loudest * 1.0e-4;  // -40 dB
    const auto minGap = juce::jmax (1, juce::roundToInt (0.05 * sampleRate / hop));
    int lastOnsetFrame = -minGap;

    for (int f = 1; f < numFrames; ++f)
    {
        const auto p = power[(size_t) f];

        if (p < floorPower || f - lastOnsetFrame < minGap)
            continue;

        double before = 0.0;
        int count = 0;

        for (int k = juce::jmax (0, f - 4); k < f; ++k, ++count)
            before += power[(size_t) k];

        before /= juce::jmax (1, count);

        if (p < riseFactor * juce::jmax (before, 1.0e-12))
            continue;

        // Refine: the first sample (from the previous frame on) above a quarter of the attack's peak.
        const auto from = (f - 1) * hop, to = juce::jmin (numSamples, (f + 2) * hop);
        float attackPeak = 0.0f;

        for (int channel = 0; channel < numChannels; ++channel)
            attackPeak = juce::jmax (attackPeak, audio.getMagnitude (channel, f * hop, to - f * hop));

        int position = f * hop;

        for (int i = from; i < to; ++i)
        {
            bool above = false;

            for (int channel = 0; channel < numChannels && ! above; ++channel)
                above = std::abs (audio.getSample (channel, i)) >= 0.25f * attackPeak;

            if (above)
            {
                position = i;
                break;
            }
        }

        onsets.push_back (position);
        lastOnsetFrame = f;
    }
}

//==============================================================================
Envelope::Envelope() noexcept
{
    x[0] = 0.0f;  y[0] = 1.0f;
    x[1] = 1.0f;  y[1] = 1.0f;
}

float Envelope::valueAt (float position) const noexcept
{
    if (position <= x[0])
        return y[0];

    for (int i = 1; i < numPoints; ++i)
    {
        if (position <= x[(size_t) i])
        {
            const auto span = x[(size_t) i] - x[(size_t) i - 1];
            const auto t = span > 1.0e-6f ? (position - x[(size_t) i - 1]) / span : 1.0f;
            return y[(size_t) i - 1] + t * (y[(size_t) i] - y[(size_t) i - 1]);
        }
    }

    return y[(size_t) numPoints - 1];
}

bool Envelope::isFlat() const noexcept
{
    for (int i = 0; i < numPoints; ++i)
        if (y[(size_t) i] < 0.9999f)
            return false;

    return true;
}

void Envelope::sanitise() noexcept
{
    numPoints = juce::jlimit (2, maxPoints, numPoints);

    for (int i = 0; i < numPoints; ++i)
    {
        auto& px = x[(size_t) i];
        auto& py = y[(size_t) i];
        px = std::isfinite (px) ? juce::jlimit (0.0f, 1.0f, px) : 0.0f;
        py = std::isfinite (py) ? juce::jlimit (0.0f, 1.0f, py) : 1.0f;
    }

    for (int i = 1; i < numPoints; ++i)
    {
        for (int j = i; j > 0 && x[(size_t) j] < x[(size_t) j - 1]; --j)
        {
            std::swap (x[(size_t) j], x[(size_t) j - 1]);
            std::swap (y[(size_t) j], y[(size_t) j - 1]);
        }
    }

    x[0] = 0.0f;
    x[(size_t) numPoints - 1] = 1.0f;
}

juce::String Envelope::toString() const
{
    juce::StringArray points;

    for (int i = 0; i < numPoints; ++i)
        points.add (juce::String (x[(size_t) i], 4) + ":" + juce::String (y[(size_t) i], 4));

    return points.joinIntoString (" ");
}

Envelope Envelope::fromString (const juce::String& text)
{
    const auto tokens = juce::StringArray::fromTokens (text, " ", {});
    Envelope e;

    if (tokens.size() < 2)
        return e;

    e.numPoints = 0;

    for (const auto& token : tokens)
    {
        if (e.numPoints >= maxPoints || ! token.containsChar (':'))
            continue;

        e.x[(size_t) e.numPoints] = token.upToFirstOccurrenceOf (":", false, false).getFloatValue();
        e.y[(size_t) e.numPoints] = token.fromFirstOccurrenceOf (":", false, false).getFloatValue();
        ++e.numPoints;
    }

    if (e.numPoints < 2)
        return {};

    e.sanitise();
    return e;
}

Envelope Envelope::dissolveDefault() noexcept
{
    Envelope e;
    e.numPoints = 4;
    e.x[0] = 0.0f;  e.y[0] = 1.0f;
    e.x[1] = 0.3f;  e.y[1] = 0.49f;
    e.x[2] = 0.6f;  e.y[2] = 0.16f;
    e.x[3] = 1.0f;  e.y[3] = 0.0f;
    return e;
}

bool Envelope::operator== (const Envelope& other) const noexcept
{
    if (numPoints != other.numPoints)
        return false;

    for (int i = 0; i < numPoints; ++i)
        if (std::abs (x[(size_t) i] - other.x[(size_t) i]) > 1.0e-6f || std::abs (y[(size_t) i] - other.y[(size_t) i]) > 1.0e-6f)
            return false;

    return true;
}

float EnvelopeCursor::valueAt (const Envelope& e, float position) noexcept
{
    if (segment < 1 || segment >= e.numPoints || position < e.x[(size_t) segment - 1])
        segment = 1;

    while (segment < e.numPoints - 1 && position > e.x[(size_t) segment])
        ++segment;

    const auto x0 = e.x[(size_t) segment - 1], x1 = e.x[(size_t) segment];
    const auto y0 = e.y[(size_t) segment - 1], y1 = e.y[(size_t) segment];

    if (position <= x0)  return y0;
    if (position >= x1)  return y1;

    return y0 + (position - x0) / (x1 - x0) * (y1 - y0);
}

SharedEnvelope::SharedEnvelope() noexcept
{
    for (size_t i = 0; i < xs.size(); ++i)
    {
        xs[i].store (0.0f);
        ys[i].store (1.0f);
    }

    set (Envelope {});
}

void SharedEnvelope::set (const Envelope& envelope) noexcept
{
    auto clean = envelope;
    clean.sanitise();

    // A sequence lock: readers retry if the sequence is odd or changed while they read.
    const auto before = sequence.load (std::memory_order_relaxed);
    sequence.store (before + 1, std::memory_order_relaxed);
    std::atomic_thread_fence (std::memory_order_release);

    numPoints.store (clean.numPoints, std::memory_order_relaxed);

    for (int i = 0; i < clean.numPoints; ++i)
    {
        xs[(size_t) i].store (clean.x[(size_t) i], std::memory_order_relaxed);
        ys[(size_t) i].store (clean.y[(size_t) i], std::memory_order_relaxed);
    }

    sequence.store (before + 2, std::memory_order_release);
}

Envelope SharedEnvelope::get() const noexcept
{
    Envelope e;

    for (int attempt = 0; attempt < 8; ++attempt)
    {
        const auto before = sequence.load (std::memory_order_acquire);

        if ((before & 1u) != 0)
            continue;

        e.numPoints = juce::jlimit (2, Envelope::maxPoints, numPoints.load (std::memory_order_relaxed));

        for (int i = 0; i < e.numPoints; ++i)
        {
            e.x[(size_t) i] = xs[(size_t) i].load (std::memory_order_relaxed);
            e.y[(size_t) i] = ys[(size_t) i].load (std::memory_order_relaxed);
        }

        std::atomic_thread_fence (std::memory_order_acquire);

        if (sequence.load (std::memory_order_relaxed) == before)
            return e;
    }

    e.sanitise();   // the writer was busy all the time: use what was read, cleaned up
    return e;
}

//==============================================================================
LayerSettings readLayerSettings (const LayerParameters& p) noexcept
{
    LayerSettings s;
    s.enabled       = p.enabled->load() >= 0.5f;
    s.reversed      = p.reverse->load() >= 0.5f;
    s.timeMode      = p.timeMode->load() >= 0.5f ? TimeMode::resample : TimeMode::stretch;
    s.lengthMode    = (LengthMode) juce::jlimit (0, 2, juce::roundToInt (p.lengthMode->load()));
    s.lengthSeconds = p.lengthSeconds->load();
    s.lengthBeats   = p.lengthBeats->load();
    s.pitch         = juce::roundToInt (p.pitch->load());
    s.rootNote      = juce::roundToInt (p.rootNote->load());
    s.start         = p.start->load() * 0.01;
    s.end           = p.end->load() * 0.01;
    s.peakOffset    = p.peakOffset != nullptr ? (double) p.peakOffset->load() : 0.0;
    return s;
}

LayerTiming computeLayerTiming (const SampleData& sample, const LayerSettings& settings,
                                int midiNote, double hostRate, double bpm) noexcept
{
    LayerTiming t;
    const int numSamples = sample.audio.getNumSamples();

    if (numSamples <= 0 || hostRate <= 0.0)
        return t;

    // The region lies between the two markers, whichever order they are in.
    const auto low  = juce::jlimit (0.0, 1.0, juce::jmin (settings.start, settings.end));
    const auto high = juce::jlimit (0.0, 1.0, juce::jmax (settings.start, settings.end));
    const auto minRegion = juce::jmin (numSamples, juce::jmax (1, (int) (0.001 * sample.sampleRate)));

    t.regionStart = juce::jlimit (0, numSamples - minRegion, (int) std::lround (low * numSamples));
    t.regionEnd   = juce::jlimit (t.regionStart + minRegion, numSamples, (int) std::lround (high * numSamples));

    const auto regionLength = (double) (t.regionEnd - t.regionStart);
    const auto rateRatio    = sample.sampleRate / hostRate;
    const auto semitones    = midiNote - settings.rootNote + settings.pitch;
    const auto pitchFactor  = std::pow (2.0, semitones / 12.0);

    if (settings.timeMode == TimeMode::resample)
    {
        // Like a tape: the pitch changes the speed, and so the length.
        t.readIncrement = pitchFactor * rateRatio;
        t.readLength    = regionLength / t.readIncrement;
        t.outputLength  = t.readLength;
        return t;
    }

    // Stretch: the reader only converts the sample rate, the stretcher does the rest.
    t.readIncrement = rateRatio;
    t.readLength    = regionLength / rateRatio;

    auto target = t.readLength;

    if (settings.lengthMode == LengthMode::seconds)
        target = settings.lengthSeconds * hostRate;
    else if (settings.lengthMode == LengthMode::beats)
        target = settings.lengthBeats * 60.0 / juce::jmax (1.0, bpm) * hostRate;

    const auto requested = t.readLength / juce::jmax (1.0, target);
    t.inputPerOutput = juce::jlimit (1.0 / maxStretch, maxSqueeze, requested);
    t.stretchLimited = std::abs (t.inputPerOutput - requested) > 1.0e-9;
    if (std::abs (t.inputPerOutput - 1.0) <= 1.0e-9)
        t.inputPerOutput = 1.0;   // the bypass reader and the layout must agree
    t.outputLength   = t.readLength / t.inputPerOutput;
    t.transpose      = pitchFactor;
    t.useStretcher   = semitones != 0 || std::abs (t.inputPerOutput - 1.0) > 1.0e-9;
    return t;
}

NoteLayout computeNoteLayout (const std::array<const SampleData*, numLayers>& samples,
                              const std::array<LayerSettings, numLayers>& settings,
                              int midiNote, PlayMode mode, double arriveLatency, double hostRate, double bpm,
                              double peakCutSeconds) noexcept
{
    NoteLayout n;
    std::array<double, numLayers> offsets {};
    std::array<bool, numLayers> present {};
    n.cut = std::round (juce::jmax (0.0, peakCutSeconds) * hostRate);
    bool anyBuild = false;

    for (int layer = 0; layer < numLayers; ++layer)
    {
        auto& l = n.layers[(size_t) layer];
        const auto* sample = samples[(size_t) layer];
        const auto& s = settings[(size_t) layer];

        if (sample == nullptr || sample->audio.getNumSamples() == 0 || ! s.enabled)
            continue;

        l.timing = computeLayerTiming (*sample, s, midiNote, hostRate, bpm);
        l.fullLength = std::ceil (l.timing.outputLength - 1.0e-6);
        offsets[(size_t) layer] = std::round (s.peakOffset * hostRate);
        present[(size_t) layer] = l.fullLength > 0.0;
        anyBuild = anyBuild || present[(size_t) layer];
    }

    if (! anyBuild)
        return n;

    // The natural peak: where the builds end together (each moved by its offset).
    // Swell: the build that needs the most time starts with the key press. Only a positive
    // offset (a layer moved later) counts here: a layer moved earlier keeps the PEAK and
    // the other layer in place, and its start is trimmed where it would begin before the key.
    double naturalPeak = 0.0;

    if (mode == PlayMode::arrive)
    {
        n.peak = std::round (juce::jmax (0.0, arriveLatency));
        naturalPeak = n.peak + n.cut;
    }
    else
    {
        bool first = true;

        for (int layer = 0; layer < numLayers; ++layer)
        {
            if (! present[(size_t) layer])
                continue;

            const auto needed = n.layers[(size_t) layer].fullLength - juce::jmax (0.0, offsets[(size_t) layer]);
            naturalPeak = first ? needed : juce::jmax (naturalPeak, needed);
            first = false;
        }

        n.peak = juce::jmax (0.0, naturalPeak - n.cut);
    }

    // Every layer plays from the key (its start is trimmed when the build would start earlier)
    // until the PEAK, or until its build ends, if that comes first.
    bool first = true;

    for (int layer = 0; layer < numLayers; ++layer)
    {
        auto& l = n.layers[(size_t) layer];

        if (! present[(size_t) layer])
            continue;

        l.start = naturalPeak + offsets[(size_t) layer] - l.fullLength;
        l.length = juce::jlimit (0.0, l.fullLength, n.peak - l.start);
        l.active = l.length > 0.0 && l.getEnd() > 0.0;
        l.cutAway = ! l.active;
        l.reachesPeak = l.active && l.getEnd() >= n.peak;

        if (! l.active)
            continue;

        n.buildStart = first ? juce::jmax (0.0, l.start) : juce::jmin (n.buildStart, juce::jmax (0.0, l.start));
        n.end        = first ? l.getEnd() : juce::jmax (n.end, l.getEnd());
        n.anyLayer = true;
        first = false;
    }

    return n;
}

int computeArriveLatency (const std::array<const SampleData*, numLayers>& samples,
                          const std::array<LayerSettings, numLayers>& settings, double hostRate, double bpm,
                          double peakCutSeconds) noexcept
{
    double naturalPeak = 0.0;

    for (int layer = 0; layer < numLayers; ++layer)
    {
        const auto* sample = samples[(size_t) layer];

        if (sample == nullptr || sample->audio.getNumSamples() == 0)
            continue;

        // Switching a layer off does not change the latency, so the host does not have to re-sync.
        const auto& s = settings[(size_t) layer];
        const auto timing = computeLayerTiming (*sample, s, s.rootNote, hostRate, bpm);
        const auto length = std::ceil (timing.outputLength - 1.0e-6);

        if (length > 0.0)
            naturalPeak = juce::jmax (naturalPeak, length - juce::jmax (0.0, std::round (s.peakOffset * hostRate)));
    }

    const auto latency = naturalPeak - std::round (juce::jmax (0.0, peakCutSeconds) * hostRate);
    return (int) juce::jlimit (0.0, maxArriveSeconds * hostRate, latency);
}

//==============================================================================
void AmpEnvelope::noteOn (float attackSeconds) noexcept
{
    if (attackSeconds > 0.0f && rate > 0.0)
    {
        state = State::attack;
        level = 0.0f;
        attackStep = (float) (1.0 / (attackSeconds * rate));
    }
    else
    {
        state = State::sustain;
        level = 1.0f;
    }
}

void AmpEnvelope::noteOff (float releaseSeconds, float curve) noexcept
{
    if (state == State::idle || state == State::release)
        return;

    if (releaseSeconds <= 0.0f || rate <= 0.0)
    {
        reset();
        return;
    }

    state = State::release;
    releaseStart = level;
    releaseProgress = 0.0f;
    releaseStep = (float) (1.0 / (releaseSeconds * rate));
    releasePower = std::pow (2.0f, juce::jlimit (-100.0f, 100.0f, curve) / 50.0f);   // 0.25 .. 4
}

float AmpEnvelope::getNextSample() noexcept
{
    switch (state)
    {
        case State::idle:
            return 0.0f;

        case State::attack:
            level += attackStep;

            if (level >= 1.0f)
            {
                level = 1.0f;
                state = State::sustain;
            }

            return level;

        case State::sustain:
            return level;

        case State::release:
            releaseProgress += releaseStep;

            if (releaseProgress >= 1.0f)
            {
                reset();
                return 0.0f;
            }

            {
                const auto remaining = 1.0f - releaseProgress;
                level = releaseStart * (std::abs (releasePower - 1.0f) < 1.0e-6f ? remaining : std::pow (remaining, releasePower));
            }

            return level;
    }

    return 0.0f;
}

//==============================================================================
SamplerVoice::SamplerVoice (const GlobalParameters& globalParameters,
                            const std::array<LayerParameters, numLayers>& layerParameters,
                            const std::atomic<double>& tempo)
    : globals (globalParameters), layerParams (layerParameters), bpm (tempo)
{
}

bool SamplerVoice::canPlaySound (juce::SynthesiserSound* sound)
{
    return dynamic_cast<InstrumentSound*> (sound) != nullptr;
}

void SamplerVoice::setCurrentPlaybackSampleRate (double newRate)
{
    juce::SynthesiserVoice::setCurrentPlaybackSampleRate (newRate);

    if (newRate > 0.0)
    {
        envelope.setSampleRate (newRate);

        for (auto& player : players)
            player.prepare (newRate);

        // Half a cosine: starts flat, so the fade itself adds no click.
        const auto length = juce::jmax (16, juce::roundToInt (tailSeconds * newRate));
        tailBuffer.setSize (2 * numLayers, length);
        tailScratch.setSize (2 * numLayers, length);
        tailBuffer.clear();
        tailFade.resize ((size_t) length);

        for (int i = 0; i < length; ++i)
            tailFade[(size_t) i] = 0.5f + 0.5f * std::cos (juce::MathConstants<float>::pi * ((float) i + 0.5f) / (float) length);

        tailLength = tailPosition = 0;
    }
}

void SamplerVoice::startNote (int midiNoteNumber, float velocity, juce::SynthesiserSound* sound, int)
{
    instrument = dynamic_cast<const InstrumentSound*> (sound);
    const auto hostRate = getSampleRate();

    for (auto& player : players)
        player.stop();

    if (instrument == nullptr || hostRate <= 0.0)
    {
        finishNote();
        return;
    }

    std::array<const SampleData*, numLayers> samples {};

    for (int layer = 0; layer < numLayers; ++layer)
    {
        samples[(size_t) layer] = instrument->getSample (layer);
        settings[(size_t) layer] = readLayerSettings (layerParams[(size_t) layer]);
        layerEnvelopes[(size_t) layer] = globals.layerEnvelopes[(size_t) layer] != nullptr ? globals.layerEnvelopes[(size_t) layer]->get() : Envelope {};
    }

    // Solo plays its layer even when it is switched off.
    if (juce::isPositiveAndBelow (soloLayer, numLayers))
        settings[(size_t) soloLayer].enabled = true;

    const auto tempo = bpm.load();
    const auto mode = globals.playMode != nullptr && globals.playMode->load() >= 0.5f ? PlayMode::arrive : PlayMode::swell;
    const auto latency = mode == PlayMode::arrive && globals.arriveLatency != nullptr ? (double) globals.arriveLatency->load() : 0.0;
    const auto cutSeconds = globals.peakCutSeconds != nullptr ? juce::jmax (0.0, (double) globals.peakCutSeconds->load()) : 0.0;

    layout = computeNoteLayout (samples, settings, midiNoteNumber, mode, latency, hostRate, tempo, cutSeconds);
    loopLayout = mode == PlayMode::swell ? layout
                                         : computeNoteLayout (samples, settings, midiNoteNumber, PlayMode::swell, 0.0, hostRate, tempo, cutSeconds);

    // Solo: the other layers stay silent, but everything keeps its place (the final envelope too).
    juce::int64 soloStart = 0;

    if (juce::isPositiveAndBelow (soloLayer, numLayers))
    {
        for (int layer = 0; layer < numLayers; ++layer)
        {
            if (layer != soloLayer)
            {
                layout.layers[(size_t) layer].active = false;
                loopLayout.layers[(size_t) layer].active = false;
            }
        }

        const auto& solo = layout.layers[(size_t) soloLayer];
        layout.anyLayer = solo.active;
        soloStart = solo.active ? juce::jmax ((juce::int64) 0, (juce::int64) std::llround (solo.start)) : 0;
    }

    if (! layout.anyLayer)
    {
        finishNote();
        return;
    }

    noteLatency = (juce::int64) layout.peak;
    noteIsArrive = mode == PlayMode::arrive && soloLayer < 0;   // solo: the note-off stops it at once
    afterPeak = globals.afterPeak != nullptr ? (AfterPeak) juce::jlimit (0, 3, juce::roundToInt (globals.afterPeak->load())) : AfterPeak::cut;
    fadeSamples = (globals.dissolveSeconds != nullptr ? juce::jmax (0.01, (double) globals.dissolveSeconds->load()) : 1.0) * hostRate;
    fadeEnvelope = globals.dissolveEnvelope != nullptr ? globals.dissolveEnvelope->get() : Envelope::dissolveDefault();
    fadeEndsSilent = fadeEnvelope.y[(size_t) (fadeEnvelope.numPoints - 1)] <= 1.0e-4f;
    fadeCursor.reset();
    mainEnvelope = globals.mainEnvelope != nullptr ? globals.mainEnvelope->get() : Envelope {};
    mainEnvelopeIsFlat = mainEnvelope.isFlat();

    velocityGain = velocity;   // velocity only changes the volume
    envelope.noteOn (globals.attackMs->load() * 0.001f);

    releaseTime = -1;
    cycleIndex = 0;
    startCycle (0);
    noteTime = soloStart;   // solo skips the silence before its layer
    handleEvents();         // layers that start right away
}

void SamplerVoice::stopNote (float, bool allowTailOff)
{
    if (! allowTailOff)
    {
        captureTail();
        finishNote();
        return;
    }

    // Arrive: everything the note does is one latency late, the note-off too.
    if (noteIsArrive && noteLatency > 0)
    {
        if (releaseTime < 0)
            releaseTime = noteTime + noteLatency;

        return;
    }

    envelope.noteOff (globals.releaseMs->load() * 0.001f, globals.releaseCurve != nullptr ? globals.releaseCurve->load() : 0.0f);

    if (! envelope.isActive())   // a release time of zero ends the note at once
        finishNote();
}

void SamplerVoice::stopSilently()
{
    tailLength = tailPosition = 0;   // a fade from an earlier stop is dropped as well
    finishNote();
}

void SamplerVoice::captureTail() noexcept
{
    const auto length = tailScratch.getNumSamples();

    if (length <= 0 || getCurrentlyPlayingSound() == nullptr || instrument == nullptr || ! envelope.isActive())
        return;

    // What is left of an earlier fade moves to the front.
    const auto left = juce::jmax (0, tailLength - tailPosition);

    for (int channel = 0; channel < tailBuffer.getNumChannels(); ++channel)
    {
        auto* data = tailBuffer.getWritePointer (channel);

        if (left > 0 && tailPosition > 0)
            std::memmove (data, data + tailPosition, sizeof (float) * (size_t) left);

        std::fill (data + left, data + length, 0.0f);
    }

    // The next few milliseconds of the note, faded out.
    tailScratch.clear();
    renderNextBlock (tailScratch, 0, length);

    for (int channel = 0; channel < tailBuffer.getNumChannels(); ++channel)
    {
        auto* data = tailBuffer.getWritePointer (channel);
        const auto* note = tailScratch.getReadPointer (channel);

        for (int i = 0; i < length; ++i)
            data[i] += note[i] * tailFade[(size_t) i];
    }

    tailLength = length;
    tailPosition = 0;
}

void SamplerVoice::renderTail (juce::AudioBuffer<float>& output, int startSample, int numSamples) noexcept
{
    const auto count = juce::jmin (numSamples, tailLength - tailPosition);

    if (count <= 0)
        return;

    for (int channel = 0; channel < juce::jmin (output.getNumChannels(), tailBuffer.getNumChannels()); ++channel)
        output.addFrom (channel, startSample, tailBuffer, channel, tailPosition, count);

    tailPosition += count;
}

void SamplerVoice::finishNote()
{
    envelope.reset();

    for (auto& player : players)
        player.stop();

    layerStartTime.fill (-1);
    releaseTime = -1;
    instrument = nullptr;
    clearCurrentNote();
}

void SamplerVoice::startCycle (juce::int64 cycleStartTime)
{
    cycleStart = cycleStartTime;
    mainCursor.reset();

    const auto& l = currentLayout();

    for (int layer = 0; layer < numLayers; ++layer)
    {
        const auto& layerLayout = l.layers[(size_t) layer];
        layerStartTime[(size_t) layer] = layerLayout.active ? cycleStart + juce::jmax ((juce::int64) 0, (juce::int64) std::llround (layerLayout.start))
                                                            : -1;
    }
}

void SamplerVoice::startLayer (int layer)
{
    layerStartTime[(size_t) layer] = -1;

    const auto& l = currentLayout().layers[(size_t) layer];
    const auto* sample = instrument != nullptr ? instrument->getSample (layer) : nullptr;

    // A layer switched off since the note started stays silent.
    if (sample == nullptr || ! l.active || layerSwitchedOff (layer))
        return;

    const auto skip = l.start < 0.0 ? (juce::int64) std::llround (-l.start) : (juce::int64) 0;
    players[(size_t) layer].start (*sample, l.timing, settings[(size_t) layer].reversed, layerEnvelopes[(size_t) layer],
                                   { afterPeak, l.length, l.reachesPeak }, skip);
}

bool SamplerVoice::layerSwitchedOff (int layer) const noexcept
{
    return ! layerIsSolo (layer) && layerParams[(size_t) layer].enabled->load() < 0.5f;
}

juce::int64 SamplerVoice::getCycleEnd() const noexcept
{
    const auto minimumCycle = (juce::int64) std::ceil (0.01 * getSampleRate());
    return cycleStart + juce::jmax (minimumCycle, (juce::int64) std::llround (currentLayout().peak));
}

juce::int64 SamplerVoice::nextEventTime() const noexcept
{
    auto next = std::numeric_limits<juce::int64>::max();

    for (const auto t : layerStartTime)
        if (t >= 0)
            next = juce::jmin (next, t);

    if (releaseTime >= 0)
        next = juce::jmin (next, releaseTime);

    if (isLooping())
        next = juce::jmin (next, getCycleEnd());

    return next;
}

void SamplerVoice::handleEvents()
{
    if (releaseTime >= 0 && noteTime >= releaseTime)
    {
        releaseTime = -1;
        envelope.noteOff (globals.releaseMs->load() * 0.001f, globals.releaseCurve != nullptr ? globals.releaseCurve->load() : 0.0f);
    }

    // Loop: at the PEAK the whole build starts again, all layers together.
    if (isLooping() && noteTime >= getCycleEnd())
    {
        cycleIndex = 1;
        startCycle (noteTime);
    }

    for (int layer = 0; layer < numLayers; ++layer)
        if (layerStartTime[(size_t) layer] >= 0 && noteTime >= layerStartTime[(size_t) layer])
            startLayer (layer);
}

float SamplerVoice::nextReleaseFadeGain (juce::int64 time) noexcept
{
    // Release: after the PEAK the layers play on and fade out along the drawn curve.
    const auto sincePeak = (double) time - layout.peak;

    if (sincePeak < 0.0)
        return 1.0f;

    return fadeCursor.valueAt (fadeEnvelope, (float) juce::jmin (1.0, sincePeak / juce::jmax (1.0, fadeSamples)));
}

double SamplerVoice::getTimelinePosition() const noexcept
{
    if (getCurrentlyPlayingNote() < 0)
        return -1.0;

    if (cycleIndex == 0)
        return (double) noteTime;

    // Later loop cycles are shown on the first one, peak on peak.
    return (double) (noteTime - cycleStart) + layout.peak - loopLayout.peak;
}

void SamplerVoice::renderNextBlock (juce::AudioBuffer<float>& output, int startSample, int numSamples)
{
    if (getCurrentlyPlayingSound() == nullptr || instrument == nullptr || output.getNumChannels() < 2 * numLayers)
        return;

    // Switching a layer off silences it in the notes that are already playing.
    for (int layer = 0; layer < numLayers; ++layer)
    {
        auto& player = players[(size_t) layer];

        if (player.isActive() && ! player.isFadingOut() && layerSwitchedOff (layer))
            player.startFadeOut();
    }

    for (int done = 0; done < numSamples;)
    {
        handleEvents();

        // Chunks end where the next layer starts, a loop restarts or a delayed note-off falls.
        auto chunk = (juce::int64) juce::jmin (numSamples - done, (int) gains.size());
        chunk = juce::jmax ((juce::int64) 1, juce::jmin (chunk, nextEventTime() - noteTime));

        const auto& l = currentLayout();
        const auto envelopeStart = (double) cycleStart + l.buildStart;
        const auto envelopeLength = l.peak - l.buildStart;

        for (int i = 0; i < (int) chunk; ++i)
        {
            auto gain = envelope.getNextSample() * velocityGain;

            if (! mainEnvelopeIsFlat)
            {
                const auto position = envelopeLength > 0.0 ? ((double) (noteTime + i) - envelopeStart) / envelopeLength : 1.0;
                gain *= mainCursor.valueAt (mainEnvelope, (float) juce::jlimit (0.0, 1.0, position));
            }

            if (afterPeak == AfterPeak::release)
                gain *= nextReleaseFadeGain (noteTime + i);

            gains[(size_t) i] = gain;
        }

        for (int layer = 0; layer < numLayers; ++layer)
        {
            auto& player = players[(size_t) layer];

            if (player.isActive())
                player.render (output.getWritePointer (2 * layer,     startSample + done),
                               output.getWritePointer (2 * layer + 1, startSample + done),
                               gains.data(), (int) chunk);
        }

        done += (int) chunk;
        noteTime += chunk;

        bool anyActive = false, anyPending = false;

        for (int layer = 0; layer < numLayers; ++layer)
        {
            anyActive  = anyActive  || players[(size_t) layer].isActive();
            anyPending = anyPending || layerStartTime[(size_t) layer] >= 0;
        }

        // Release: a fade that ends at zero ends the note.
        const auto faded = afterPeak == AfterPeak::release && fadeEndsSilent && (double) noteTime >= layout.peak + fadeSamples;

        // Loop keeps the note alive between cycles; the release ends it.
        if (! envelope.isActive() || faded || (! anyActive && ! anyPending && ! isLooping()))
        {
            finishNote();
            break;
        }
    }
}

//==============================================================================
SamplerSynth::SamplerSynth()
{
    for (auto& playhead : playheads)
        playhead.store (-1.0f);
}

void SamplerSynth::addSamplerVoice (SamplerVoice* voice)
{
    const juce::ScopedLock sl (lock);
    samplerVoices.push_back (voice);
    addVoice (voice);
}

void SamplerSynth::replaceSound (juce::SynthesiserSound::Ptr newSound)
{
    // The old sound is released after the lock is left (locals are destroyed in
    // reverse order), so a large buffer is never freed while the audio thread waits.
    juce::ReferenceCountedArray<juce::SynthesiserSound> previousSounds;

    const juce::ScopedLock sl (lock);
    allNotesOff (0, false);
    previousSounds.swapWith (sounds);

    if (newSound != nullptr)
        sounds.add (newSound);
}

void SamplerSynth::silenceAll()
{
    const juce::ScopedLock sl (lock);

    for (auto* voice : samplerVoices)
        voice->stopSilently();

    allNotesOff (0, false);   // the voices are silent already; this also forgets the sustain pedal
    timeline.store (-1.0, std::memory_order_relaxed);

    for (auto& playhead : playheads)
        playhead.store (-1.0f, std::memory_order_relaxed);
}

void SamplerSynth::renderVoices (juce::AudioBuffer<float>& buffer, int startSample, int numSamples)
{
    juce::Synthesiser::renderVoices (buffer, startSample, numSamples);

    for (auto* voice : samplerVoices)
        voice->renderTail (buffer, startSample, numSamples);

    {
        const SamplerVoice* newest = nullptr;

        for (const auto* voice : samplerVoices)
            if (voice->getCurrentlyPlayingNote() >= 0 && (newest == nullptr || newest->wasStartedBefore (*voice)))
                newest = voice;

        timeline.store (newest != nullptr ? newest->getTimelinePosition() : -1.0, std::memory_order_relaxed);
        buildPeak.store (newest != nullptr ? newest->getBuildPeak() : 0.0, std::memory_order_relaxed);
    }

    // This runs with the synth lock held, so reading the voices here is safe.
    for (int layer = 0; layer < numLayers; ++layer)
    {
        const SamplerVoice* newest = nullptr;

        for (const auto* voice : samplerVoices)
            if (voice->isLayerActive (layer) && (newest == nullptr || newest->wasStartedBefore (*voice)))
                newest = voice;

        playheads[(size_t) layer].store (newest != nullptr ? newest->getForwardPosition (layer) : -1.0f,
                                         std::memory_order_relaxed);
    }
}

} // namespace rs
