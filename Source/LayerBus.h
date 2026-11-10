#pragma once

#include <juce_dsp/juce_dsp.h>

namespace rs
{

/** Pointers to the parameter values of a layer's mixer section. */
struct LayerBusParameters
{
    std::atomic<float>* levelDb = nullptr;
    std::atomic<float>* pan = nullptr;        // -100..100
    std::atomic<float>* width = nullptr;      // 0..200 %
    std::atomic<float>* lowCutHz = nullptr;   // 20 Hz = off
    std::atomic<float>* highCutHz = nullptr;  // 20 kHz = off
};

/** Processes the sum of all notes of one layer: filters, stereo width, pan and level.
    At the default settings the audio passes through untouched.
*/
class LayerBus
{
public:
    static constexpr float minCutoff = 20.0f, maxCutoff = 20000.0f;

    void setParameters (const LayerBusParameters& p) noexcept   { params = p; }
    void prepare (double sampleRate, int maximumBlockSize);
    void process (float* left, float* right, int numSamples) noexcept;
    /** Clears the filters' history (host bypass); the settings stay. */
    void reset() noexcept;
    void setFastRamps (bool fast) noexcept;

private:
    void updateTargets() noexcept;

    LayerBusParameters params;
    double rate = 44100.0;

    juce::dsp::StateVariableTPTFilter<float> lowCut, highCut;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> lowCutHz { minCutoff }, highCutHz { maxCutoff };
    juce::SmoothedValue<float> gainLeft { 1.0f }, gainRight { 1.0f }, width { 1.0f };
    bool lowCutActive = false, highCutActive = false;
    bool fastRamps = false;
};

} // namespace rs
