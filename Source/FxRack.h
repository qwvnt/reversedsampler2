#pragma once
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>
#include <array>
#include <atomic>
#include <functional>
#include "EqDsp.h"
#include "DriveDsp.h"
#include "ReverbDsp.h"
#include "ChorusDsp.h"
#include "CompressorDsp.h"

namespace rs {
constexpr int fxSlots = 5;
constexpr int numFxTypes = 7;
constexpr int maxDelayReflections = 1;   // one pitch-shift generation; legacy host ID remains reserved
constexpr double maxDelaySeconds = 2.0;
constexpr double maxSpliceSeconds = 0.35;
inline juce::String fxID (int slot, const char* name) { return "fx" + juce::String (slot + 1) + "_" + name; }
inline constexpr const char* fxParameterNames[] = { "type", "a", "b", "p1", "p2", "p3", "mix", "delayStyle", "sync", "division", "diffusion", "bias", "replica", "reflections", "amSplice", "amReverse", "replicaBrown", "eqAir", "eqLowCut", "eqHighCut", "eqMode", "driveType", "rvMode", "rvPreDelay", "rvLowCut", "rvChar",
                                                    "chModel", "chMode", "chVariant", "chStereo", "chDelay", "chFeedback", "chVoices", "chOutput", "chHQ", "chVibRate", "chVibDepth",
                                                    "cpMode", "cpKnee", "cpAttack", "cpRelease", "cpTime" };
inline constexpr const char* fxTypeNames[7] = { "Empty", "Delay", "EQ", "Drive", "Reverb", "Chorus", "Compressor" };
inline constexpr const char* fxKnobNames[7][4] = { { "", "", "", "" }, { "Time", "Feedback", "", "Mix" }, { "Low", "Mid", "Treble", "Mix" }, { "Drive", "Tone", "Output", "Mix" },
                                                   { "Size", "Decay", "Tone", "Mix" }, { "Rate", "Depth", "Width", "Mix" }, { "Threshold", "Ratio", "Makeup", "Mix" } };
// Quarter-note multiples, in the same order as delayDivisions().
// p1/p2/p3/mix are shared by every effect type (host automation IDs never change), but they mean
// different things in each one. Choosing a type in the editor loads these neutral starting values,
// so e.g. a former Delay Time never turns a new EQ into a hidden low-shelf cut.
//                                              p1     p2     p3     mix
inline constexpr float fxTypeDefaults[7][4] = { { 50.0f, 50.0f, 50.0f,  50.0f },   // Empty (unused)
                                                { 50.0f, 35.0f, 50.0f,  35.0f },   // Delay: 520 ms, 35 % feedback
                                                { 50.0f, 50.0f, 50.0f, 100.0f },   // EQ: flat
                                                { 30.0f, 50.0f, 50.0f, 100.0f },   // Drive: +10.8 dB, neutral tone, 0 dB out
                                                { 50.0f, 40.0f, 50.0f,  30.0f },   // Reverb: medium, 30 % wet
                                                { 50.0f, 50.0f, 100.0f, 50.0f },
                                                { 70.0f, 46.276f, 0.0f, 100.0f } };
inline constexpr double divisionBeats[] = { 1.0/12, 0.125, 0.1875, 1.0/6, 0.25, 0.375, 1.0/3, 0.5, 0.75, 2.0/3, 1.0, 1.5, 4.0/3, 2.0, 3.0, 8.0/3, 4.0, 6.0 };
inline juce::StringArray delayDivisions() { return { "1/32T", "1/32", "1/32.", "1/16T", "1/16", "1/16.", "1/8T", "1/8", "1/8.", "1/4T", "1/4", "1/4.", "1/2T", "1/2", "1/2.", "1/1T", "1/1", "1/1." }; }

// EQ knobs are 0..100 with 50 = flat; the full turn is +/-12 dB in both modes.
inline float eqKnobToDb (float v) noexcept { return (std::clamp (v, 0.0f, 100.0f) - 50.0f) * 0.24f; }
inline eq::Settings eqSettings (float low, float mid, float treble, float air, float lowCutHz, float highCutHz, float mode) noexcept {
    eq::Settings s;
    s.lowDb = eqKnobToDb (low); s.midDb = eqKnobToDb (mid); s.trebleDb = eqKnobToDb (treble); s.airDb = eqKnobToDb (air);
    s.lowCutHz = lowCutHz; s.highCutHz = highCutHz; s.vintage = mode < 0.5f;
    return s;
}

struct SpectrumTap {
    static constexpr int size = 8192;
    std::array<std::atomic<float>, size> pre {}, post {};
    std::atomic<unsigned> written { 0 };
    std::atomic<bool> wanted { false };
    void read (float* preOut, float* postOut, int count) const noexcept {
        const unsigned end = written.load (std::memory_order_acquire);
        for (int k = 0; k < count; ++k) {
            const auto index = (end - (unsigned) count + (unsigned) k) & (unsigned) (size - 1);
            preOut[k] = pre[index].load (std::memory_order_relaxed);
            postOut[k] = post[index].load (std::memory_order_relaxed);
        }
    }
};

// Serial rack, with completely independent histories for each layer.
// Changes that restart an effect (Type, delay character, Vintage/Modern, Amethyst direction, the A/B
// route, a slot swap, a project load) crossfade: the running effect fades to dry in 20 ms with the knob
// values it had, its history is cleared, then the new setting fades its input in (20 ms, output still
// dry) and then its output (20 ms). A slot that is silent switches at once.
class FxRackDSP {
public:
    void connect (juce::AudioProcessorValueTreeState& state, std::function<std::atomic<float>* (const juce::String&)> resolve = {});
    // maximumBlockSize sizes the crossfade scratch; larger host blocks are processed in pieces.
    void prepare (double sampleRate, int maximumBlockSize = 512);
    // Clears every history now and jumps to the current settings without fades (prepare, host bypass).
    // Audio thread only, or while process() cannot run.
    void reset() noexcept;
    // Thread-safe: served by the next process(), with a crossfade where audio is flowing. To rewrite
    // several parameters of a slot at once (slot swap, project load), request the reset before writing
    // them and again after: the old effect then fades out with its own values, never with a mixture.
    void requestReset (int slot) noexcept { resetMask.fetch_or (1u << (unsigned) slot, std::memory_order_release); }
    void requestResetAll() noexcept { resetMask.fetch_or ((1u << fxSlots) - 1u, std::memory_order_release); }
    void process (juce::AudioBuffer<float>& layers, int samples, double bpm = 120.0) noexcept;
    comp::Meters& compressorMeters (int slot) noexcept { return compressors[(size_t) juce::jlimit (0, fxSlots - 1, slot)].meters(); }
    SpectrumTap& spectrumTap (int slot) noexcept { return taps[(size_t) juce::jlimit (0, fxSlots - 1, slot)]; }
private:
    struct Parameters {
        std::atomic<float>* type = nullptr; std::atomic<float>* layer[2]{}; std::atomic<float>* p[3]{}; std::atomic<float>* mix = nullptr;
        std::atomic<float>* style = nullptr; std::atomic<float>* sync = nullptr; std::atomic<float>* division = nullptr;
        std::atomic<float>* diffusion = nullptr; std::atomic<float>* bias = nullptr; std::atomic<float>* replica = nullptr; std::atomic<float>* reflections = nullptr;
        std::atomic<float>* amSplice = nullptr; std::atomic<float>* amReverse = nullptr; std::atomic<float>* replicaBrown = nullptr;
        std::atomic<float>* eqAir = nullptr; std::atomic<float>* eqLowCut = nullptr; std::atomic<float>* eqHighCut = nullptr; std::atomic<float>* eqMode = nullptr;
        std::atomic<float>* driveType = nullptr;
        std::atomic<float>* rvMode = nullptr; std::atomic<float>* rvPreDelay = nullptr; std::atomic<float>* rvLowCut = nullptr; std::atomic<float>* rvChar = nullptr;
        std::atomic<float>* chModel = nullptr; std::atomic<float>* chMode = nullptr; std::atomic<float>* chVariant = nullptr; std::atomic<float>* chStereo = nullptr;
        std::atomic<float>* chDelay = nullptr; std::atomic<float>* chFeedback = nullptr; std::atomic<float>* chVoices = nullptr;
        std::atomic<float>* chOutput = nullptr; std::atomic<float>* chHQ = nullptr; std::atomic<float>* chVibRate = nullptr; std::atomic<float>* chVibDepth = nullptr;
        std::atomic<float>* cpMode = nullptr; std::atomic<float>* cpKnee = nullptr; std::atomic<float>* cpAttack = nullptr; std::atomic<float>* cpRelease = nullptr; std::atomic<float>* cpTime = nullptr;
    };
    struct Allpass {
        juce::AudioBuffer<float> data; int pos = 0, valid = 0;
        void reset() noexcept { pos = valid = 0; }
        void process (float& l, float& r, float gain) noexcept;
    };
    struct State {
        // Ordinary delay and reverse history, plus one Amethyst pitch-shift generation.
        juce::AudioBuffer<float> delay, input, grains;
        std::array<Allpass, 4> diffusion;
        int pos = 0, valid = 0, inputPos = 0, inputValid = 0, pitchPos = 0, pitchValid = 0;
        int reversePhase = 0, delayTime = 0, previousReverseLength = 0, lastType = -1, lastStyle = -1, lastVintage = -1;
        int grainPos = 0, grainValid = 0, grainLength = 0, lastReflections = 0, lastDirection = -1;
        float grainFilter[maxDelayReflections][2][4]{}; int grainOffset[maxDelayReflections][2]{};
        float previousTape[2]{};
        bool wasEnabled = false;
        int silentSamples = 0;   // idle gate: input and output below -120 dB for this long
        float low[2]{}, high[2]{}, tone[2]{}, brown[2]{};
        double pitchPhase = 0.0, previousPitchPhase = -1.0, wowPhase = 0.0, flutterPhase = 0.0;
        float delayFrom = 0.0f, delayTo = 0.0f, delayFade = 1.0f;
        // Tape/Amethyst read head (samples), see runSlot. Double: in float, the 5 ms one-pole at a
        // 520 ms delay stalls up to 0.23 samples short of its target and never arrives.
        double headTarget = 0.0, headDelay = 0.0;
        juce::SmoothedValue<float> mix, smoothDelay, smoothFeedback, smoothDiffusion, smoothBias, smoothBrown;
        void reset() noexcept;
    };
    // What a slot runs: everything whose change restarts the effect.
    struct Config {
        int type = 0, style = 0, vintage = 0, direction = 0;   // Delay: style, Replica, Amethyst direction. Chorus: model, mode, variant
        bool operator== (const Config& o) const noexcept {
            return type == o.type && style == o.style && vintage == o.vintage && direction == o.direction;
        }
    };
    struct Transition {
        Config active;                  // the effect being run (still the old one while a change fades out)
        float gain[2] { 0.0f, 0.0f };   // per layer, output: 0 = dry, 1 = the effect
        float feed[2] { 1.0f, 1.0f };   // per layer, input of a freshly started effect (fades in with it)
        bool quiet[2] { true, true };   // this slot's input and output were silent in the last block
        bool pending = false;           // reset requested: fade out, clear, fade back in
    };
    static Config requested (const Parameters& p) noexcept;
    static chorus::Settings chorusSettings (const Parameters& p) noexcept;
    static comp::Settings compressorSettings (const Parameters& p) noexcept;
    void resetSlot (int slot) noexcept;
    void processChunk (juce::AudioBuffer<float>& layers, int samples, double tempo) noexcept;
    void runSlot (int slot, const Parameters& p, juce::AudioBuffer<float>& layers, int samples, double tempo, const bool* running) noexcept;
    void holdLive (int slot) noexcept;   // the held knob values jump to the host values
    std::array<Parameters, fxSlots> params;
    // v0.16: the knob values each slot's effect actually runs with. They follow the host parameters
    // while the slot is settled and stay frozen while a change fades the old effect out, so the
    // outgoing effect is never driven by values meant for the new one (a Tape or Amethyst delay would
    // zip as its Time jumped by half a second during the fade). The message thread writes a reset
    // request first, then the Type, then the knobs; processChunk reads in the opposite order.
    static constexpr int numFields = 42;
    static_assert (sizeof (Parameters) == (size_t) numFields * sizeof (std::atomic<float>*), "forEachField must list every parameter");
    struct Held {
        std::array<std::atomic<float>*, numFields> live {};   // the host parameters (null if missing)
        std::array<std::atomic<float>, numFields> values {};  // what the effect reads
        Parameters view;                                      // the Parameters layout, pointing into values
    };
    template <typename P, typename F> static void forEachField (P& p, F&& f) {
        f (p.type); f (p.layer[0]); f (p.layer[1]); f (p.p[0]); f (p.p[1]); f (p.p[2]); f (p.mix);
        f (p.style); f (p.sync); f (p.division); f (p.diffusion); f (p.bias); f (p.replica); f (p.reflections);
        f (p.amSplice); f (p.amReverse); f (p.replicaBrown); f (p.eqAir); f (p.eqLowCut); f (p.eqHighCut); f (p.eqMode);
        f (p.driveType); f (p.rvMode); f (p.rvPreDelay); f (p.rvLowCut); f (p.rvChar);
        f (p.chModel); f (p.chMode); f (p.chVariant); f (p.chStereo); f (p.chDelay); f (p.chFeedback);
        f (p.chVoices); f (p.chOutput); f (p.chHQ); f (p.chVibRate); f (p.chVibDepth);
        f (p.cpMode); f (p.cpKnee); f (p.cpAttack); f (p.cpRelease); f (p.cpTime);
    }
    std::array<Held, fxSlots> held;
    std::array<std::array<State, 2>, fxSlots> states;
    std::array<eq::Processor, fxSlots> eqs;   // both layers of a slot share coefficients, never history
    std::array<drive::Processor, fxSlots> drives;   // full-band 4-type distortion, 4x oversampled
    std::array<reverb::Processor, fxSlots> reverbs; // 6-mode stereo reverb, settings shared by both layers, history never
    std::array<chorus::Processor, fxSlots> choruses;
    std::array<comp::Processor, fxSlots> compressors;
    std::array<SpectrumTap, fxSlots> taps;
    double rate = 44100;
    std::atomic<unsigned> resetMask { 0 };
    std::array<Transition, fxSlots> transitions;
    juce::AudioBuffer<float> dry;   // the dry input of the layers that are changing over
    float fadeStep = 0.001f;        // 1 / 20 ms
    struct DelayConstants { float replicaToneVintage = 0.0f, replicaToneModern = 0.0f, brownTone = 0.0f; double headCoeff = 0.0; };
    DelayConstants delayConstants;
};
}
