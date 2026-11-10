#include "LayerBus.h"

namespace rs
{

namespace
{
    constexpr int filterUpdateInterval = 16;   // samples between cutoff updates while a knob moves

    bool isUnity (float value) noexcept   { return std::abs (value - 1.0f) < 1.0e-6f; }
}

void LayerBus::prepare (double sampleRate, int maximumBlockSize)
{
    rate = sampleRate;

    const juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) juce::jmax (1, maximumBlockSize), 2 };
    lowCut.prepare (spec);
    highCut.prepare (spec);
    lowCut.setType (juce::dsp::StateVariableTPTFilterType::highpass);
    highCut.setType (juce::dsp::StateVariableTPTFilterType::lowpass);

    for (auto* value : { &gainLeft, &gainRight, &width })
        value->reset (sampleRate, 0.02);

    lowCutHz.reset (sampleRate, 0.05);
    highCutHz.reset (sampleRate, 0.05);

    updateTargets();

    for (auto* value : { &gainLeft, &gainRight, &width })
        value->setCurrentAndTargetValue (value->getTargetValue());

    lowCutHz.setCurrentAndTargetValue (lowCutHz.getTargetValue());
    highCutHz.setCurrentAndTargetValue (highCutHz.getTargetValue());
    lowCutActive = highCutActive = false;
    fastRamps = false;
}

void LayerBus::reset() noexcept
{
    lowCut.reset();
    highCut.reset();
}

void LayerBus::updateTargets() noexcept
{
    if (params.levelDb == nullptr)
        return;

    const auto level = juce::Decibels::decibelsToGain (params.levelDb->load(), -60.0f);   // -60 dB = silent
    const auto pan   = juce::jlimit (-1.0f, 1.0f, params.pan->load() * 0.01f);

    // Balance-style pan for stereo sources: the far side is turned down, the centre stays at unity.
    gainLeft.setTargetValue  (level * juce::jmin (1.0f, 1.0f - pan));
    gainRight.setTargetValue (level * juce::jmin (1.0f, 1.0f + pan));
    width.setTargetValue (juce::jlimit (0.0f, 2.0f, params.width->load() * 0.01f));

    const auto nyquistLimit = (float) (rate * 0.45);
    lowCutHz.setTargetValue  (juce::jlimit (minCutoff, juce::jmin (maxCutoff, nyquistLimit), params.lowCutHz->load()));
    const auto high = params.highCutHz->load();
    highCutHz.setTargetValue (high >= 19950.0f ? juce::jmin (maxCutoff, nyquistLimit)
                                             : juce::jlimit (minCutoff, juce::jmin (maxCutoff, nyquistLimit), high));
}

void LayerBus::process (float* left, float* right, int numSamples) noexcept
{
    updateTargets();

    // Filters at their end stops are switched off completely.
    const bool wantLowCut  = lowCutHz.isSmoothing()  || lowCutHz.getTargetValue()  > minCutoff + 0.5f;
    const bool wantHighCut = highCutHz.isSmoothing() || highCutHz.getTargetValue() < juce::jmin (maxCutoff, (float) (rate * 0.45)) - 0.5f;

    if (wantLowCut && ! lowCutActive)    lowCut.reset();
    if (wantHighCut && ! highCutActive)  highCut.reset();

    lowCutActive  = wantLowCut;
    highCutActive = wantHighCut;

    const bool useWidth = width.isSmoothing() || ! isUnity (width.getTargetValue());
    const bool useGain  = gainLeft.isSmoothing() || gainRight.isSmoothing()
                       || ! isUnity (gainLeft.getTargetValue()) || ! isUnity (gainRight.getTargetValue());

    if (! lowCutActive && ! highCutActive && ! useWidth && ! useGain)
        return;

    for (int offset = 0; offset < numSamples; offset += filterUpdateInterval)
    {
        const int count = juce::jmin (filterUpdateInterval, numSamples - offset);

        if (lowCutActive)   lowCut.setCutoffFrequency  (lowCutHz.skip (count));
        if (highCutActive)  highCut.setCutoffFrequency (highCutHz.skip (count));

        for (int i = offset; i < offset + count; ++i)
        {
            auto l = left[i], r = right[i];

            if (lowCutActive)
            {
                l = lowCut.processSample (0, l);
                r = lowCut.processSample (1, r);
            }

            if (highCutActive)
            {
                l = highCut.processSample (0, l);
                r = highCut.processSample (1, r);
            }

            if (useWidth)
            {
                const auto mid  = 0.5f * (l + r);
                const auto side = 0.5f * (l - r) * width.getNextValue();
                l = mid + side;
                r = mid - side;
            }

            if (useGain)
            {
                l *= gainLeft.getNextValue();
                r *= gainRight.getNextValue();
            }

            left[i]  = l;
            right[i] = r;
        }
    }

    lowCut.snapToZero();
    highCut.snapToZero();
}

void LayerBus::setFastRamps (bool fast) noexcept
{
    if (fast == fastRamps)
        return;

    fastRamps = fast;
    const auto retime = [this] (auto& value, double seconds)
    {
        const auto current = value.getCurrentValue(), target = value.getTargetValue();
        value.reset (rate, seconds);
        value.setCurrentAndTargetValue (current);
        value.setTargetValue (target);
    };

    for (auto* value : { &gainLeft, &gainRight, &width })
        retime (*value, fast ? 0.001 : 0.02);

    retime (lowCutHz, fast ? 0.002 : 0.05);
    retime (highCutHz, fast ? 0.002 : 0.05);
}

} // namespace rs
