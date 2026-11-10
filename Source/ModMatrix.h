#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <array>
#include <atomic>
#include <vector>
#include "ModSources.h"

namespace rs {

class ModMatrix {
public:
    struct Destination {
        juce::String paramID, name;
        int fxSlot = -1, layer = -1;
        juce::String fxParameter;
    };

    static const std::vector<Destination>& destinations();
    static int numDestinations() { return (int) destinations().size(); }
    static int destinationIndex (const juce::String& paramID);
    static inline constexpr int gainDestination = 1;
    static juce::String rowID (int row, const char* name) { return "mod" + juce::String (row + 1) + name; }
    static juce::String lfoID (int lfo, const char* name) { return "lfo" + juce::String (lfo + 1) + name; }
    static juce::String macroID (int macro) { return "macro" + juce::String (macro + 1); }
    static inline constexpr const char* envAttack = "envA";
    static inline constexpr const char* envDecay = "envD";
    static inline constexpr const char* envSustain = "envS";
    static inline constexpr const char* envRelease = "envR";
    static inline constexpr const char* rowFields[] { "Src", "Aux", "Dst", "Amt", "Curve", "Bip" };
    static void addParameters (juce::AudioProcessorValueTreeState::ParameterLayout&);
    static juce::StringArray allParameterIDs();

    void connect (juce::AudioProcessorValueTreeState&);
    std::atomic<float>* resolve (const juce::String& paramID) noexcept;

    void prepare (double sampleRate);
    void resetSources() noexcept;

    bool refreshActive() noexcept;
    bool isActive() const noexcept { return active; }
    bool layerIsModulated (int layer) const noexcept { return layerModulated[(size_t) juce::jlimit (0, 1, layer)]; }
    bool fxIsModulated() const noexcept { return fxModulated; }
    bool gainIsModulated() const noexcept { return gainModulated; }
    void passThrough() noexcept;

    struct BlockInfo {
        int numSamples = 0;
        double bpm = 120.0;
        bool playing = false;
        double ppqStart = 0.0;
        double buildEnd = -1.0;
        double buildPeak = 0.0;
    };
    void beginBlock (const juce::MidiBuffer&, const BlockInfo&) noexcept;
    void processChunk (int start, int length) noexcept;

    rs::mod::RowSettings readRow (int row) const noexcept;
    float sourceValue (int source) const noexcept { return juce::isPositiveAndBelow (source, (int) mod::numSources) ? sourceDisplay[(size_t) source].load (std::memory_order_relaxed) : 0.0f; }
    float rowValue (int row) const noexcept { return rowDisplay[(size_t) juce::jlimit (0, mod::numRows - 1, row)].load (std::memory_order_relaxed); }
    float lfoPhase (int lfo) const noexcept { return lfoDisplayPhase[(size_t) juce::jlimit (0, mod::numLfos - 1, lfo)].load (std::memory_order_relaxed); }
    int envelopeStage() const noexcept { return envelopeDisplayStage.load (std::memory_order_relaxed); }
    float destinationOffset (int destination) const noexcept;
    bool isDestinationRouted (int destination) const noexcept;
    float baseNormalised (int destination) const noexcept;
    float displayNormalised (int destination) const noexcept;
    float displayValue (const juce::String& paramID, float base) const noexcept;
    juce::RangedAudioParameter* destinationParameter (int destination) const noexcept;
    mod::LfoSettings readLfo (int lfo) const noexcept;
    mod::EnvelopeSettings readEnvelope() const noexcept;

private:
    struct Target {
        juce::RangedAudioParameter* parameter = nullptr;
        std::atomic<float>* base = nullptr;
        std::atomic<float> effective { 0.0f };
        std::atomic<float> offset { 0.0f };
        float sum = 0.0f;
    };
    struct RowParameters { std::atomic<float>* field[6] {}; };
    void handleMidi (const juce::MidiMessage&) noexcept;
    void noteOn (int note, float velocity) noexcept;
    void noteOff (int note) noexcept;
    void releaseAll() noexcept;

    std::vector<std::unique_ptr<Target>> targets;
    std::array<RowParameters, mod::numRows> rows {};
    std::array<std::atomic<float>*, mod::numMacros> macros {};
    struct LfoParameters { std::atomic<float>* shape = nullptr, *rate = nullptr, *sync = nullptr, *division = nullptr, *trigger = nullptr, *phase = nullptr, *smooth = nullptr; };
    std::array<LfoParameters, mod::numLfos> lfoParams {};
    std::array<std::atomic<float>*, 4> envParams {};

    std::array<mod::Lfo, mod::numLfos> lfos;
    mod::Envelope envelope;
    mod::Random random;
    std::array<float, mod::numSources> sources {};
    std::array<std::atomic<float>, mod::numSources> sourceDisplay {};
    std::array<std::atomic<float>, mod::numRows> rowDisplay {};
    std::array<std::atomic<float>, mod::numLfos> lfoDisplayPhase {};
    std::atomic<int> envelopeDisplayStage { 0 };
    std::array<mod::RowSettings, mod::numRows> rowCache {};
    std::array<int, mod::numRows> routedTargets {};
    int numRouted = 0;
    std::array<mod::LfoSettings, mod::numLfos> lfoCache {};
    mod::EnvelopeSettings envelopeCache {};
    std::array<bool, 128> held {};
    std::array<bool, 128> sustained {};
    int heldCount = 0;
    bool pedal = false;
    bool active = false;
    std::array<bool, 2> layerModulated {};
    bool fxModulated = false, gainModulated = false;
    double fs = 48000.0;

    const juce::MidiBuffer* midi = nullptr;
    BlockInfo block;
    float lastBuild = 0.0f;
};

}
