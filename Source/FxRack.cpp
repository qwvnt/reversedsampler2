#include "FxRack.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace rs {
namespace {
// First-order antiderivative anti-aliasing: integrate the nonlinearity between
// adjacent samples rather than generating unfiltered harmonics at a point.
// log(cosh(v)), constant cancels in the difference.
double saturatePrimitive (double v) noexcept {
    const double m = std::abs (v);
    return m + std::log1p (std::exp (-2.0 * m));
}
// previousPrimitive caches saturatePrimitive (previous): the previous sample's value is reused
// instead of being evaluated twice (exp + log1p per sample per channel).
float saturate (float x, float& previous, double& previousPrimitive) noexcept {
    const double a = x, b = previous;
    const double primitiveA = saturatePrimitive (a);
    const double primitiveB = previousPrimitive;
    previous = x; previousPrimitive = primitiveA;
    return std::abs (a-b) > 1.0e-4 ? (float) ((primitiveA - primitiveB) / (a-b))
                                 : (float) std::tanh (0.5 * (a+b));
}
// Four-point Hermite interpolation. Integer delays remain exactly transparent.
float readDelay (const float* line, int size, int position, int valid, float distance) noexcept {
    const int base = (int) distance;
    const float f = distance - (float) base;
    const auto at = [&] (int d) {
        if (d < 1 || d > valid) return 0.0f;
        const int index = position - d;
        return line[index < 0 ? index + size : index];
    };
    const float y0 = at (base), y1 = at (base+1);
    if (f == 0.0f) return y0;
    const float ym = at (base-1), y2 = at (base+2);
    return y0 + 0.5f*f*(y1-ym + f*(2.0f*ym-5.0f*y0+4.0f*y1-y2 + f*(3.0f*(y0-y1)+y2-ym)));
}
}
void FxRackDSP::Allpass::process (float& l, float& r, float gain) noexcept {
    const int size = data.getNumSamples();
    float* samples[] { &l, &r };
    for (int ch = 0; ch < 2; ++ch) {
        auto* line = data.getWritePointer (ch);
        const float delayed = valid >= size ? line[pos] : 0.0f;
        const float in = *samples[ch];
        const float out = delayed - gain * in;
        line[pos] = in + gain * out;
        *samples[ch] = out;
    }
    if (++pos >= size) pos = 0;
    valid = juce::jmin (size, valid + 1);
}
void FxRackDSP::State::reset() noexcept {
    lastType = lastStyle = -1; wasEnabled = false; silentSamples = 0;
    pos = valid = inputPos = inputValid = pitchPos = pitchValid = reversePhase = 0;
    grainPos = grainValid = lastReflections = 0; lastDirection = -1;
    for (auto& stage : grainFilter) for (auto& channel : stage) std::fill (std::begin (channel), std::end (channel), 0.0f);
    std::fill (std::begin (previousTape), std::end (previousTape), 0.0f);
    std::fill (std::begin (previousTapePrimitive), std::end (previousTapePrimitive), saturatePrimitive (0.0));
    tapeToneFor = -1.0f; tapeTone = 0.0f;
    delayFrom = delayTo = 0.0f; delayFade = 1.0f; headTarget = headDelay = 0.0;
    delayTime = previousReverseLength = 0; pitchPhase = wowPhase = flutterPhase = 0.0; previousPitchPhase = -1.0;
    for (auto& stage : grainOffset) for (auto& head : stage) head = 0;
    std::fill (std::begin (low), std::end (low), 0.0f);
    std::fill (std::begin (high), std::end (high), 0.0f);
    std::fill (std::begin (tone), std::end (tone), 0.0f);
    std::fill (std::begin (brown), std::end (brown), 0.0f);
    for (auto& allpass : diffusion) allpass.reset();
}
void FxRackDSP::connect (juce::AudioProcessorValueTreeState& s, std::function<std::atomic<float>* (const juce::String&)> resolve) {
    for (int i = 0; i < fxSlots; ++i) {
        auto& p = params[(size_t)i];
        auto get = [&] (const char* n) {
            if (resolve) if (auto* modulated = resolve (fxID (i, n))) return modulated;
            return s.getRawParameterValue (fxID (i, n));
        };
        p.type = get("type"); p.layer[0] = get("a"); p.layer[1] = get("b");
        p.p[0] = get("p1"); p.p[1] = get("p2"); p.p[2] = get("p3"); p.mix = get("mix");
        p.style = get("delayStyle"); p.sync = get("sync"); p.division = get("division");
        p.diffusion = get("diffusion"); p.bias = get("bias"); p.replica = get("replica"); p.reflections = get("reflections");
        p.amSplice = get("amSplice"); p.amReverse = get("amReverse"); p.replicaBrown = get("replicaBrown");
        p.eqAir = get("eqAir"); p.eqLowCut = get("eqLowCut"); p.eqHighCut = get("eqHighCut"); p.eqMode = get("eqMode");
        p.driveType = get("driveType");
        p.rvMode = get("rvMode"); p.rvPreDelay = get("rvPreDelay"); p.rvLowCut = get("rvLowCut"); p.rvChar = get("rvChar");
        p.chModel = get("chModel"); p.chMode = get("chMode"); p.chVariant = get("chVariant"); p.chStereo = get("chStereo");
        p.chDelay = get("chDelay"); p.chFeedback = get("chFeedback"); p.chVoices = get("chVoices");
        p.cpMode = get("cpMode"); p.cpKnee = get("cpKnee"); p.cpAttack = get("cpAttack"); p.cpRelease = get("cpRelease"); p.cpTime = get("cpTime");
        p.chOutput = get("chOutput"); p.chHQ = get("chHQ"); p.chVibRate = get("chVibRate"); p.chVibDepth = get("chVibDepth");
        // The effect reads a held copy (see Held): the same layout, pointing at the held values.
        auto& h = held[(size_t)i];
        h.view = p;
        int k = 0;
        forEachField (h.view, [&h, &k] (std::atomic<float>*& field) {
            h.live[(size_t)k] = field;
            h.values[(size_t)k].store (field != nullptr ? field->load() : 0.0f, std::memory_order_relaxed);
            if (field != nullptr) field = &h.values[(size_t)k];
            ++k;
        });
    }
}
void FxRackDSP::prepare (double sr, int maximumBlockSize) {
    rate = juce::jmax (1.0, sr);
    for (auto& e : eqs) e.prepare (rate);
    for (auto& d : drives) d.prepare (rate);
    for (auto& r : reverbs) r.prepare (rate);
    for (auto& c : choruses) c.prepare (rate);
    for (auto& c : compressors) c.prepare (rate);
    const int maxDelay = (int) std::ceil (maxDelaySeconds * rate);
    // Short, mutually incommensurate stages make a compact diffuser, not a
    // second long echo. Gain (not dry/wet comb mixing) controls its density.
    constexpr double allpassTimes[] { 0.00317, 0.00473, 0.00731, 0.01097 };
    for (auto& slot : states) for (auto& s : slot) {
        s.delay.setSize (2, maxDelay + 2);
        s.input.setSize (2, maxDelay * 2 + (int) std::ceil (rate * 0.006) + 4);
        s.grainLength = (int) std::ceil (rate * (maxDelaySeconds + 3.0 * maxSpliceSeconds + 0.054)) + 4;
        s.grains.setSize (2, s.grainLength * maxDelayReflections);
        // All reads are guarded by valid counters after reset, so avoid touching
        // tens of megabytes of untouched history when the rack is empty.
        for (int i = 0; i < 4; ++i) s.diffusion[(size_t)i].data.setSize (2, juce::jmax (1, (int) (rate * allpassTimes[i])));
        s.reset(); s.grainLength = s.grains.getNumSamples() / maxDelayReflections; s.lastType = s.lastStyle = -1; s.wasEnabled = false;
        s.mix.reset (rate, 0.02); s.mix.setCurrentAndTargetValue (0.0f);
        s.smoothDelay.reset (rate, 0.03); s.smoothDelay.setCurrentAndTargetValue (0.0f);
        for (auto* smooth : { &s.smoothFeedback, &s.smoothDiffusion, &s.smoothBias, &s.smoothBrown }) {
            smooth->reset (rate, 0.02); smooth->setCurrentAndTargetValue (0.0f);
        }
    }
    // Crossfade scratch: the dry input of the layers that are changing over. A host block larger
    // than announced is processed in pieces (see process), so the audio thread never allocates.
    dry.setSize (4, juce::jlimit (64, 8192, maximumBlockSize));
    fadeStep = (float) (1.0 / (0.02 * rate));   // 20 ms each way
    const auto onePole = [this] (float hz) { return 1.0f - std::exp (-juce::MathConstants<float>::twoPi * juce::jmin (hz, (float) rate * 0.45f) / (float) rate); };
    delayConstants.replicaToneVintage = onePole (6200.0f);
    delayConstants.replicaToneModern = onePole (14500.0f);
    delayConstants.brownTone = onePole (3500.0f);
    delayConstants.headCoeff = 1.0 - std::exp (-1.0 / (0.005 * rate));   // read-head inertia: 5 ms
    reset();
}
namespace {
// Idle gate threshold (-120 dB). Namespace scope, so lambdas never need to capture it.
constexpr float idleSilence = 1.0e-6f;
float stereoPeak (const float* l, const float* r, int n) noexcept {
    float m = 0.0f;
    for (int k = 0; k < n; ++k) m = std::max (m, std::max (std::abs (l[k]), std::abs (r[k])));
    return m;
}
}
// One slot. running[] replaces the A/B switches: a layer that is fading out keeps running.
void FxRackDSP::runSlot (int i, const Parameters& p, juce::AudioBuffer<float>& layers, int samples, double tempo, const bool* running) noexcept {
    const auto coef = [this] (float hz) { return 1.0f - std::exp (-juce::MathConstants<float>::twoPi * juce::jmin (hz, (float) rate * 0.45f) / (float) rate); };
    // Idle gate: when a layer's input and its effect output both stay below -120 dB for longer
    // than the effect's longest internal delay, the layer is skipped (its history cleared once).
    // A sampler is silent most of the time, so an idle rack now costs almost nothing. Any input
    // wakes the layer in the same block, starting from a clean state, so nothing is ever cut.
    const auto blockPeak = [samples] (const float* l, const float* r) noexcept {
        float m = 0.0f;
        for (int k = 0; k < samples; ++k) m = std::max (m, std::max (std::abs (l[k]), std::abs (r[k])));
        return m;
    };
    const auto holdFor = [this] (int fxType) noexcept {
        // Delay: 2 s line, reverse reads up to 2x back, Amethyst grains add ~1 s. Reverb: 250 ms pre-delay.
        const double seconds = fxType == 1 ? 6.5 : fxType == 2 ? 0.3 : fxType == 3 ? 0.1 : fxType == 5 ? (double) chorus::holdSeconds : fxType == 6 ? comp::holdSeconds : 0.5;
        return (int) (seconds * rate);
    };
    const auto gated = [&layers, &blockPeak, samples] (auto& proc, std::array<State, 2>& st, const bool* enabled, int hold) noexcept {
        float* channels[4] { layers.getWritePointer (0), layers.getWritePointer (1), layers.getWritePointer (2), layers.getWritePointer (3) };
        bool run[2] {}, quiet[2] {};
        for (int layer = 0; layer < 2; ++layer) {
            auto& s = st[(size_t) layer];
            quiet[layer] = blockPeak (channels[2 * layer], channels[2 * layer + 1]) < idleSilence;
            if (! quiet[layer]) s.silentSamples = 0;
            run[layer] = enabled[layer] && s.silentSamples < hold;
        }
        proc.process (channels, run, samples);
        for (int layer = 0; layer < 2; ++layer) {
            if (! run[layer]) continue;
            auto& s = st[(size_t) layer];
            if (quiet[layer] && blockPeak (channels[2 * layer], channels[2 * layer + 1]) < idleSilence) {
                s.silentSamples = std::min (hold, s.silentSamples + samples);
                if (s.silentSamples >= hold) proc.resetLayer (layer);   // falls asleep from a clean state
            } else s.silentSamples = 0;
        }
    };
    // p: the held knob values (see processChunk), not the live host parameters.
    const auto& cfg = transitions[(size_t)i].active;   // the running effect: still the old one while a change fades out
    const int type = cfg.type;
    if (type == 0) { for (auto& s : states[(size_t)i]) { if (s.lastType != 0 || s.wasEnabled) s.reset(); s.lastType = 0; s.wasEnabled = false; } return; }
    if (type == 2) {
        // Dedicated path: always 100% wet (a dry/wet blend of an EQ is a comb filter),
        // coefficients shared by both layers and only recomputed while a control moves.
        auto& eq = eqs[(size_t)i];
        bool enabled[2] {};
        bool fresh = true;
        for (int layer = 0; layer < 2; ++layer) {
            enabled[layer] = running[layer];
            if (states[(size_t)i][(size_t)layer].lastType == 2) fresh = false;
        }
        if (fresh) eq.reset();
        for (int layer = 0; layer < 2; ++layer) {
            auto& s = states[(size_t)i][(size_t)layer];
            const bool wasEq = s.lastType == 2;
            if (! wasEq) s.reset();
            if (! wasEq || s.wasEnabled != enabled[layer]) eq.resetLayer (layer);
            s.lastType = 2; s.wasEnabled = enabled[layer];
        }
        eq.setTargets (eqSettings (p.p[0]->load(), p.p[1]->load(), p.p[2]->load(), p.eqAir->load(),
                                   p.eqLowCut->load(), p.eqHighCut->load(), p.eqMode->load()), fresh);
        auto& tap = taps[(size_t)i];
        const bool tapping = tap.wanted.load (std::memory_order_relaxed);
        const auto mono = [&layers, &enabled] (int k) noexcept {
            float sum = 0.0f;
            for (int layer = 0; layer < 2; ++layer)
                if (enabled[layer]) sum += layers.getSample (2 * layer, k) + layers.getSample (2 * layer + 1, k);
            return 0.5f * sum;
        };
        const unsigned tapStart = tap.written.load (std::memory_order_relaxed);
        if (tapping)
            for (int k = 0; k < samples; ++k) tap.pre[(tapStart + (unsigned) k) & (unsigned) (SpectrumTap::size - 1)].store (mono (k), std::memory_order_relaxed);
        gated (eq, states[(size_t)i], enabled, holdFor (2));
        if (tapping) {
            for (int k = 0; k < samples; ++k) tap.post[(tapStart + (unsigned) k) & (unsigned) (SpectrumTap::size - 1)].store (mono (k), std::memory_order_relaxed);
            tap.written.store (tapStart + (unsigned) samples, std::memory_order_release);
        }
        return;
    }
    if (type == 3) {
        // Full-band Drive: the same curve on every frequency, 4x oversampled with the dry/wet mix inside
        // the oversampled domain. Settings are shared by both layers, history never is.
        auto& drive = drives[(size_t)i];
        bool enabled[2] {};
        bool fresh = true;
        for (int layer = 0; layer < 2; ++layer) {
            enabled[layer] = running[layer];
            if (states[(size_t)i][(size_t)layer].lastType == 3) fresh = false;
        }
        drive::Settings d;
        d.drive01 = juce::jlimit (0.0f, 1.0f, p.p[0]->load() * 0.01f);
        d.tone01 = juce::jlimit (0.0f, 1.0f, p.p[1]->load() * 0.01f);
        d.output = juce::jlimit (0.0f, 2.0f, p.p[2]->load() * 0.02f);   // 50 = unity, 100 = +6 dB
        d.mix = juce::jlimit (0.0f, 1.0f, p.mix->load() * 0.01f);
        d.type = p.driveType != nullptr ? juce::jlimit (0, drive::numTypes - 1, (int) std::round (p.driveType->load())) : 0;
        drive.setTargets (d, fresh);
        for (int layer = 0; layer < 2; ++layer) {
            auto& s = states[(size_t)i][(size_t)layer];
            const bool wasDrive = s.lastType == 3;
            if (! wasDrive) s.reset();
            if (! wasDrive || s.wasEnabled != enabled[layer]) drive.resetLayer (layer);
            s.lastType = 3; s.wasEnabled = enabled[layer];
        }
        gated (drive, states[(size_t)i], enabled, holdFor (3));
        return;
    }
    if (type == 4) {
        // Reverb: Size/Decay/Tone/Mix on p1..p3/mix, mode, pre-delay, low cut and the per-mode
        // Character control on their own parameters. Both layers share settings, never history.
        auto& rv = reverbs[(size_t)i];
        bool enabled[2] {};
        bool fresh = true;
        for (int layer = 0; layer < 2; ++layer) {
            enabled[layer] = running[layer];
            if (states[(size_t)i][(size_t)layer].lastType == 4) fresh = false;
        }
        const auto value = [] (const std::atomic<float>* v, float fallback) { return v != nullptr ? v->load() : fallback; };
        reverb::Settings r;
        r.mode = juce::jlimit (0, reverb::numModes - 1, (int) std::round (value (p.rvMode, 0.0f)));
        r.size01 = juce::jlimit (0.0f, 1.0f, p.p[0]->load() * 0.01f);
        r.decay01 = juce::jlimit (0.0f, 1.0f, p.p[1]->load() * 0.01f);
        r.tone01 = juce::jlimit (0.0f, 1.0f, p.p[2]->load() * 0.01f);
        r.mix = juce::jlimit (0.0f, 1.0f, p.mix->load() * 0.01f);
        r.preDelayMs = juce::jlimit (0.0f, (float) reverb::maxPreDelayMs, value (p.rvPreDelay, 10.0f));
        r.lowCutHz = juce::jlimit (20.0f, 1000.0f, value (p.rvLowCut, 80.0f));
        r.character01 = juce::jlimit (0.0f, 1.0f, value (p.rvChar, 50.0f) * 0.01f);
        rv.setTargets (r, fresh);
        for (int layer = 0; layer < 2; ++layer) {
            auto& s = states[(size_t)i][(size_t)layer];
            const bool wasReverb = s.lastType == 4;
            if (! wasReverb) s.reset();
            if (! wasReverb || s.wasEnabled != enabled[layer]) rv.resetLayer (layer);
            s.lastType = 4; s.wasEnabled = enabled[layer];
        }
        gated (rv, states[(size_t)i], enabled, holdFor (4));
        return;
    }
    if (type == 5) {
        auto& ch = choruses[(size_t)i];
        bool enabled[2] {};
        bool fresh = true;
        for (int layer = 0; layer < 2; ++layer) {
            enabled[layer] = running[layer];
            if (states[(size_t)i][(size_t)layer].lastType == 5) fresh = false;
        }
        ch.setTargets (chorusSettings (p), fresh);
        for (int layer = 0; layer < 2; ++layer) {
            auto& s = states[(size_t)i][(size_t)layer];
            const bool wasChorus = s.lastType == 5;
            if (! wasChorus) s.reset();
            if (! wasChorus || s.wasEnabled != enabled[layer]) ch.resetLayer (layer);
            s.lastType = 5; s.wasEnabled = enabled[layer];
        }
        gated (ch, states[(size_t)i], enabled, holdFor (5));
        return;
    }
    if (type == 6) {
        auto& cp = compressors[(size_t)i];
        bool enabled[2] {};
        bool fresh = true;
        for (int layer = 0; layer < 2; ++layer) {
            enabled[layer] = running[layer];
            if (states[(size_t)i][(size_t)layer].lastType == 6) fresh = false;
        }
        cp.setTargets (compressorSettings (p), fresh);
        for (int layer = 0; layer < 2; ++layer) {
            auto& s = states[(size_t)i][(size_t)layer];
            const bool wasCompressor = s.lastType == 6;
            if (! wasCompressor) s.reset();
            if (! wasCompressor || s.wasEnabled != enabled[layer]) cp.resetLayer (layer);
            s.lastType = 6; s.wasEnabled = enabled[layer];
        }
        gated (cp, states[(size_t)i], enabled, holdFor (6));
        return;
    }
    const float a = juce::jlimit (0.0f, 1.0f, p.p[0]->load() * 0.01f);
    const float b = juce::jlimit (0.0f, 1.0f, p.p[1]->load() * 0.01f);
    const float mix = juce::jlimit (0.0f, 1.0f, p.mix->load() * 0.01f);
    const int style = cfg.style;
    const int div = juce::jlimit (0, (int) std::size (divisionBeats) - 1, (int) std::round (p.division->load()));
    const double time = p.sync->load() >= 0.5f ? 60.0 / tempo * divisionBeats[div] : 0.04 + a * 0.96;
    const int delayLength = juce::jlimit (1, (int) (rate * maxDelaySeconds), (int) std::round (time * rate));
    const float feedback = b * 0.65f;
    const float diffusion = juce::jlimit (0.0f, 1.0f, p.diffusion->load() * 0.01f);
    const float bias = juce::jlimit (0.0f, 1.0f, p.bias->load() * 0.01f);
    const bool vintage = cfg.vintage != 0;
    constexpr int reflections = 1;
    const float replicaTone = vintage ? delayConstants.replicaToneVintage : delayConstants.replicaToneModern;
    const float brownTone = delayConstants.brownTone;
    const bool brownOn = p.replicaBrown->load() >= 0.5f;
    // Two Butterworth biquads form a fourth-order anti-alias filter
    // before every octave-up generation (cutoff ~0.19 * sample rate).
    struct Biquad { float b0, b1, b2, a1, a2; };
    static const std::array<Biquad, 2> grainBiquads = [] {
        const float w = juce::MathConstants<float>::twoPi * 0.19f;
        const float cs = std::cos (w), sn = std::sin (w);
        const auto design = [cs, sn] (float q) {
            const float alpha = sn / (2.0f * q), inv = 1.0f / (1.0f + alpha);
            return Biquad { (1.0f - cs) * 0.5f * inv, (1.0f - cs) * inv,
                            (1.0f - cs) * 0.5f * inv, -2.0f * cs * inv, (1.0f - alpha) * inv };
        };
        return std::array<Biquad, 2> { design (0.5411961f), design (1.306563f) };
    }();
    const int grainDuration = juce::jmax (8, (int) std::round (rate * juce::jlimit (150.0f, 700.0f, p.amSplice->load()) * 0.0005));
    const bool grainReverse = cfg.direction != 0;
    const double headCoeff = delayConstants.headCoeff;
    const float delayFadeStep = (float) (1.0 / (0.03 * rate));

    for (int layer = 0; layer < 2; ++layer) {
        auto& s = states[(size_t)i][(size_t)layer];
        const bool enabled = running[layer];   // a fading layer keeps running until it is silent
        const auto targets = [&] (bool snap) {
            const auto set = [snap] (juce::SmoothedValue<float>& v, float target) {
                if (snap) v.setCurrentAndTargetValue (target); else v.setTargetValue (target);
            };
            set (s.smoothFeedback, feedback); set (s.smoothDiffusion, diffusion);
            set (s.smoothBias, bias); set (s.smoothBrown, brownOn ? 1.0f : 0.0f);
        };
        if (s.lastType != type || enabled != s.wasEnabled || (type == 1 && (s.lastStyle != style || (style == 2 && s.lastVintage != (int) vintage)))) {
            s.reset(); s.mix.setCurrentAndTargetValue (enabled ? mix : 0.0f);
            s.smoothDelay.setCurrentAndTargetValue ((float) delayLength);
            s.delayFrom = s.delayTo = (float) delayLength;
            s.headTarget = s.headDelay = (double) delayLength;
            targets (true);
            s.lastType = type; s.lastStyle = style; s.lastVintage = (int) vintage; s.wasEnabled = enabled;
        }
        if (! enabled) continue;
        const int hold = holdFor (1);
        const bool quietIn = blockPeak (layers.getReadPointer (2*layer), layers.getReadPointer (2*layer+1)) < idleSilence;
        if (! quietIn) s.silentSamples = 0;
        else if (s.silentSamples >= hold) continue;   // asleep: every repeat is already below -120 dB
        // Keep history. Reverse changes length only at a grain boundary.
        if (s.delayTime == 0) s.delayTime = delayLength;
        targets (false);
        s.mix.setTargetValue (mix); s.smoothDelay.setTargetValue ((float) delayLength);
        auto* left = layers.getWritePointer (2*layer); auto* right = layers.getWritePointer (2*layer+1);
        const int size = s.delay.getNumSamples(), inputSize = s.input.getNumSamples();
        auto* dl = s.delay.getWritePointer (0); auto* dr = s.delay.getWritePointer (1);
        auto* il = s.input.getWritePointer (0); auto* ir = s.input.getWritePointer (1);
        auto* grainLeft = s.grains.getWritePointer (0); auto* grainRight = s.grains.getWritePointer (1);
        if (type == 1 && style == 3 && (s.lastReflections != reflections || s.lastDirection != (int) grainReverse)) {
            s.grainPos = s.grainValid = 0; s.pitchPhase = 0.0; s.previousPitchPhase = -1.0;
            for (auto& stage : s.grainOffset) for (auto& head : stage) head = 0;
            for (auto& stage : s.grainFilter) for (auto& channel : stage) std::fill (std::begin (channel), std::end (channel), 0.0f);
            s.lastReflections = reflections; s.lastDirection = (int) grainReverse;
        }
        for (int n = 0; n < samples; ++n) {
            const float wetMix = s.mix.getNextValue();
            // Tape and Amethyst read heads move like a tape motor: never slower than 1/4 or faster than
            // 4x the write head (+/-24 semitones while Time moves), never backwards, and they change
            // speed over ~5 ms instead of at once, so a glide has no kink at its start or end. Small
            // moves keep the 30 ms glide; 40 -> 520 ms takes 0.65 s instead of zipping backwards at 15x.
            s.headTarget += juce::jlimit (-3.0, 0.75, (double) s.smoothDelay.getNextValue() - s.headTarget);
            s.headDelay += (s.headTarget - s.headDelay) * headCoeff;
            const float smoothedDelay = (float) s.headDelay;
            const float loopGain = s.smoothFeedback.getNextValue();
            const float density = s.smoothDiffusion.getNextValue();
            const float tapeBias = s.smoothBias.getNextValue();
            const float hiCutMix = s.smoothBrown.getNextValue();
            // Crossfade stationary read heads in Replica: no Doppler sweep
            // when a sync division, tempo or Time value changes.
            if (s.delayFade >= 1.0f && s.delayTo != (float) delayLength) {
                s.delayFrom = s.delayTo; s.delayTo = (float) delayLength; s.delayFade = 0.0f;
            }
            const float delayBlend = s.delayFade >= 1.0f ? 1.0f : 0.5f - 0.5f*std::cos (juce::MathConstants<float>::pi*s.delayFade);
            float* outs[2] = { left+n, right+n }; float* rings[2] = { dl, dr }; float* inputs[2] { il, ir }; float* grainRings[2] { grainLeft, grainRight };
            const float x[2] { *outs[0], *outs[1] }; float wet[2] { x[0], x[1] };
            if (type == 1) {
                if (style == 0) {
                    if (s.reversePhase == 0) {
                        s.previousReverseLength = s.delayTime;
                        s.delayTime = delayLength;
                    }
                    const int length = s.delayTime;
                    const int distance = 1 + 2 * s.reversePhase;
                    const int fade = juce::jmax (1, juce::jmin (length / 8, (int) (rate * 0.003)));
                    const float blend = s.reversePhase < fade
                        ? 0.5f - 0.5f*std::cos (juce::MathConstants<float>::pi*(float)s.reversePhase/(float)fade) : 1.0f;
                    float reversed[2]{};
                    for (int ch=0; ch<2; ++ch) {
                        const float current = readDelay (inputs[ch], inputSize, s.inputPos, s.inputValid, (float)distance);
                        const float previous = blend < 1.0f ? readDelay (inputs[ch], inputSize, s.inputPos, s.inputValid, (float)(distance+2*s.previousReverseLength)) : 0.0f;
                        reversed[ch] = previous + blend*(current-previous);
                        inputs[ch][s.inputPos] = x[ch];
                    }
                    // A tunable allpass is spectrally flat at every settled density.
                    // At zero it becomes identity; no parallel dry/allpass comb filter.
                    if (density > 0.0f) {
                        const float gain = -1.0f + 0.48f*density;
                        for (auto& allpass : s.diffusion) allpass.process (reversed[0], reversed[1], gain);
                    } else for (auto& allpass : s.diffusion) allpass.reset();
                    const int echoIndex = s.pos - length;
                    const int readEcho = echoIndex < 0 ? echoIndex + size : echoIndex;
                    for (int ch=0; ch<2; ++ch) { const float echo = s.valid >= length ? rings[ch][readEcho] : 0.0f; wet[ch] = reversed[ch] + loopGain * echo; rings[ch][s.pos] = wet[ch]; }
                    if (++s.inputPos >= inputSize) s.inputPos = 0;
                    s.inputValid = juce::jmin (inputSize, s.inputValid + 1);
                    if (++s.reversePhase >= length) s.reversePhase = 0;
                } else if (style == 3) {
                    // Independent, finite generations: each stage records the
                    // previous wet generation, then plays two overlapping grains
                    // at +/-2x. This is a granular echo, not a model of Soundtoys.
                    // Splice is the captured source length; playback at 2x takes half.
                    const double phase1 = s.pitchPhase;
                    const double phase2 = phase1 < 0.5 ? phase1 + 0.5 : phase1 - 0.5;
                    const float w1 = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) phase1);
                    const auto distanceFor = [&] (double phase) {
                        return (double) smoothedDelay + 2.0 + (grainReverse ? 3.0 * phase : 1.0 - phase) * (double) grainDuration;
                    };
                    const double distances[2] { distanceFor (phase1), distanceFor (phase2) };
                    const bool starts[2] { s.previousPitchPhase >= 0.0 && phase1 < s.previousPitchPhase,
                                           s.previousPitchPhase >= 0.0 && s.previousPitchPhase < 0.5 && phase1 >= 0.5 };
                    float stageIn[2] { x[0], x[1] }; wet[0] = wet[1] = 0.0f;
                    for (int stage = 0; stage < reflections; ++stage) {
                        float* lines[2] { grainRings[0] + stage * s.grainLength, grainRings[1] + stage * s.grainLength };
                        for (int ch=0; ch<2; ++ch) {
                            float filtered = stageIn[ch];
                            for (int biquad = 0; biquad < 2; ++biquad) {
                                const auto& filter = grainBiquads[biquad];
                                auto& z1 = s.grainFilter[stage][ch][biquad * 2];
                                auto& z2 = s.grainFilter[stage][ch][biquad * 2 + 1];
                                const float y = filter.b0 * filtered + z1;
                                z1 = filter.b1 * filtered - filter.a1 * y + z2;
                                z2 = filter.b2 * filtered - filter.a2 * y;
                                filtered = y;
                            }
                            lines[ch][s.grainPos] = filtered;
                        }
                        // Align each new grain to the other head while its window
                        // is zero. A short stereo correlation reduces phase jumps
                        // and tonal cancellation without changing +12 semitones.
                        for (int head=0; head<2; ++head) if (starts[head]) {
                            const int reference = 1-head;
                            const int span = juce::jmin (96, (int) (rate * 0.002));
                            const int window = juce::jlimit (16, 144, (int) (rate * 0.003));
                            const int refBase = (int) distances[reference] + s.grainOffset[stage][reference];
                            const int base = (int) distances[head];
                            float best = -2.0f; int bestOffset = 0;
                            for (int delta=-span; delta<=span; delta+=2) {
                                const int candidate = base + delta;
                                if (candidate < 2 || candidate + window + 2 > s.grainValid || refBase < 2 || refBase + window + 2 > s.grainValid) continue;
                                double dot=0.0, e1=0.0, e2=0.0;
                                for (int k=0; k<window; k+=4) {
                                    int idx1 = s.grainPos - candidate - k, idx2 = s.grainPos - refBase - k;
                                    if (idx1 < 0) idx1 += s.grainLength;
                                    if (idx2 < 0) idx2 += s.grainLength;
                                    for (int ch=0; ch<2; ++ch) {
                                        const float v1 = lines[ch][idx1], v2 = lines[ch][idx2];
                                        dot += (double)v1*v2; e1 += (double)v1*v1; e2 += (double)v2*v2;
                                    }
                                }
                                if (e1 < 1.0e-12 || e2 < 1.0e-12) continue;
                                const float score = (float) (dot / std::sqrt (e1*e2)) - 0.0001f * (float) std::abs (delta);
                                if (score > best) { best = score; bestOffset = delta; }
                            }
                            s.grainOffset[stage][head] = bestOffset;
                        }
                        for (int ch=0; ch<2; ++ch) {
                            const auto read = [&] (int head) {
                                const double distance = distances[head] + s.grainOffset[stage][head];
                                return readDelay (lines[ch], s.grainLength, s.grainPos, s.grainValid, (float)distance);
                            };
                            const float shifted = w1 * read (0) + (1.0f - w1) * read (1);
                            wet[ch] += shifted; stageIn[ch] = shifted * loopGain;
                        }
                    }
                    // Feedback repeats the single octave-shifted generation, rather
                    // than adding more pitch shifters or silently disabling Feedback.
                    for (int ch=0; ch<2; ++ch) {
                        wet[ch] += loopGain * readDelay (rings[ch], size, s.pos, s.valid, smoothedDelay);
                        rings[ch][s.pos] = wet[ch];
                    }
                    s.previousPitchPhase = s.pitchPhase;
                    s.grainPos = (s.grainPos + 1) % s.grainLength;
                    s.grainValid = juce::jmin (s.grainLength - 1, s.grainValid + 1);
                    s.pitchPhase += 1.0 / (double) grainDuration;
                    if (s.pitchPhase >= 1.0) s.pitchPhase -= 1.0;
                } else {
                    // Tape is bandwidth-limited and gently saturated, with wow.
                    // Replica Modern is clean; Vintage has a softer, darker repeat.
                    // Fixed, low-level wow plus faster flutter: the small delay
                    // wander keeps Tape repeats from sounding perfectly static.
                    const float mod = style == 1
                        ? (float) (std::sin (s.wowPhase) * rate * 0.00045
                                  + std::sin (s.flutterPhase) * rate * 0.000035)
                        : 0.0f;
                    const float d = juce::jlimit (1.0f, (float) (size - 2), smoothedDelay + mod);
                    if (std::memcmp (&tapeBias, &s.tapeToneFor, sizeof (float)) != 0) {   // pow + exp only while Bias moves
                        s.tapeTone = coef (1500.0f * std::pow (12.0f, tapeBias));
                        s.tapeToneFor = tapeBias;
                    }
                    const float tapeTone = s.tapeTone;
                    for (int ch=0; ch<2; ++ch) {
                        const float delayed = style == 1 ? readDelay (rings[ch], size, s.pos, s.valid, d)
                            : (1.0f-delayBlend)*readDelay (rings[ch], size, s.pos, s.valid, s.delayFrom)
                                + delayBlend*readDelay (rings[ch], size, s.pos, s.valid, s.delayTo);
                        if (style == 1) {
                            s.tone[ch] += tapeTone * (delayed - s.tone[ch]);
                            const float saturation = 1.0f + 0.5f * (1.0f - tapeBias);
                            wet[ch] = saturate (s.tone[ch] * saturation, s.previousTape[ch], s.previousTapePrimitive[ch]) / saturation;
                        } else if (vintage) {
                            // Gentle bandwidth/saturation, without mismatched encoder
                            // and decoder envelopes that pump the first repeat.
                            s.tone[ch] += replicaTone * (delayed - s.tone[ch]);
                            wet[ch] = saturate (s.tone[ch] * 0.75f, s.previousTape[ch], s.previousTapePrimitive[ch]) / 0.75f;
                        } else wet[ch] = delayed;
                        if (style == 2) {
                            s.brown[ch] += brownTone * (wet[ch] - s.brown[ch]);
                            wet[ch] += hiCutMix * (s.brown[ch] - wet[ch]);
                        }
                        rings[ch][s.pos] = x[ch] + loopGain * wet[ch];
                    }
                    if (style == 1) {
                        s.wowPhase += juce::MathConstants<double>::twoPi * 0.31 / rate;
                        s.flutterPhase += juce::MathConstants<double>::twoPi * 4.7 / rate;
                        if (s.wowPhase >= juce::MathConstants<double>::twoPi) s.wowPhase -= juce::MathConstants<double>::twoPi;
                        if (s.flutterPhase >= juce::MathConstants<double>::twoPi) s.flutterPhase -= juce::MathConstants<double>::twoPi;
                    }
                }
                s.delayFade = juce::jmin (1.0f, s.delayFade + delayFadeStep);
                if (++s.pos >= size) s.pos = 0;
                s.valid = juce::jmin (size, s.valid + 1);
            }
            for (int ch=0; ch<2; ++ch) *outs[ch] = x[ch] + wetMix * (wet[ch] - x[ch]);
        }
        if (quietIn && blockPeak (left, right) < idleSilence) s.silentSamples = juce::jmin (hold, s.silentSamples + samples);
        else s.silentSamples = 0;
    }
}
// ---------------------------------------------------------------- v0.16 change-overs
// Type, delay character, Vintage/Modern, Amethyst direction, the A/B route, a slot swap or a project
// load used to restart the effect in one sample: a click whenever audio was flowing. Now the running
// effect fades to dry (20 ms) with the knob values it had, its history is cleared, and the new setting
// starts: its input fades in first (20 ms, output still dry), so it never starts in the middle of a
// waveform, then its output fades in (20 ms): 60 ms in all. Where the slot was silent in the previous
// block nothing can be heard, so the change is instant.
FxRackDSP::Config FxRackDSP::requested (const Parameters& p) noexcept {
    Config c;
    if (p.type == nullptr) return c;
    c.type = juce::jlimit (0, numFxTypes - 1, (int) std::round (p.type->load()));
    if (c.type == 1 && p.style != nullptr) {
        c.style = juce::jlimit (0, 3, (int) std::round (p.style->load()));
        if (c.style == 2 && p.replica != nullptr) c.vintage = p.replica->load() < 0.5f ? 1 : 0;
        if (c.style == 3 && p.amReverse != nullptr) c.direction = p.amReverse->load() >= 0.5f ? 1 : 0;
    }
    if (c.type == 5) {
        const auto s = chorusSettings (p);
        c.style = s.model;
        c.vintage = chorus::modeRestarts (s.model) ? s.mode : 0;
        c.direction = chorus::variantRestarts (s.model) && s.variant ? 1 : 0;
    }
    if (c.type == 6) c.style = compressorSettings (p).mode;
    return c;
}
comp::Settings FxRackDSP::compressorSettings (const Parameters& p) noexcept {
    const auto value = [] (const std::atomic<float>* v, float fallback) { return v != nullptr ? v->load() : fallback; };
    comp::Settings s;
    s.mode = juce::jlimit (0, comp::numModes - 1, (int) std::round (value (p.cpMode, 0.0f)));
    s.thresholdDb = comp::thresholdFromKnob (value (p.p[0], 70.0f));
    s.ratio = comp::ratioFromKnob (value (p.p[1], 46.276f));
    s.makeupDb = comp::makeupFromKnob (value (p.p[2], 0.0f));
    s.mix = juce::jlimit (0.0f, 1.0f, value (p.mix, 100.0f) * 0.01f);
    s.kneeDb = value (p.cpKnee, 6.0f);
    s.attackMs = value (p.cpAttack, 10.0f);
    s.releaseMs = value (p.cpRelease, 150.0f);
    s.time = juce::jlimit (0, comp::numTimes - 1, (int) std::round (value (p.cpTime, 1.0f)));
    return s;
}
chorus::Settings FxRackDSP::chorusSettings (const Parameters& p) noexcept {
    const auto value = [] (const std::atomic<float>* v, float fallback) { return v != nullptr ? v->load() : fallback; };
    chorus::Settings s;
    s.model = juce::jlimit (0, chorus::numModels - 1, (int) std::round (value (p.chModel, 0.0f)));
    s.mode = juce::jlimit (0, chorus::modeCounts[s.model] - 1, (int) std::round (value (p.chMode, 0.0f)));
    s.rate01 = juce::jlimit (0.0f, 1.0f, value (p.p[0], 50.0f) * 0.01f);
    s.depth01 = juce::jlimit (0.0f, 1.0f, value (p.p[1], 50.0f) * 0.01f);
    s.width01 = juce::jlimit (0.0f, 1.0f, value (p.p[2], 100.0f) * 0.01f);
    s.mix01 = juce::jlimit (0.0f, 1.0f, value (p.mix, 50.0f) * 0.01f);
    s.variant = value (p.chVariant, 0.0f) >= 0.5f;
    s.dualStereo = value (p.chStereo, 0.0f) >= 0.5f;
    s.delayMs = value (p.chDelay, 15.0f);
    s.feedback = value (p.chFeedback, 0.0f) * 0.01f;
    s.voices = (int) std::round (value (p.chVoices, 4.0f));
    s.outputDb = value (p.chOutput, 0.0f);
    s.hq = value (p.chHQ, 1.0f) >= 0.5f;
    s.vibratoRate01 = juce::jlimit (0.0f, 1.0f, value (p.chVibRate, 50.0f) * 0.01f);
    s.vibratoDepth01 = juce::jlimit (0.0f, 1.0f, value (p.chVibDepth, 30.0f) * 0.01f);
    return s;
}
void FxRackDSP::resetSlot (int slot) noexcept {
    const auto i = (size_t) slot;
    for (auto& s : states[i]) s.reset();
    eqs[i].reset(); drives[i].reset(); reverbs[i].reset(); choruses[i].reset(); compressors[i].reset();
}
void FxRackDSP::reset() noexcept {
    resetMask.store (0, std::memory_order_relaxed);   // everything is cleared right here
    for (int i = 0; i < fxSlots; ++i) {
        resetSlot (i);
        const auto& p = params[(size_t) i];
        auto& t = transitions[(size_t) i];
        t.active = requested (p); t.pending = false;
        for (int l = 0; l < 2; ++l) {
            const bool routed = p.layer[l] != nullptr && p.layer[l]->load() >= 0.5f;
            t.gain[l] = t.active.type != 0 && routed ? 1.0f : 0.0f;
            t.feed[l] = 1.0f; t.quiet[l] = true;
        }
        holdLive (i);
    }
}
void FxRackDSP::holdLive (int slot) noexcept {
    auto& h = held[(size_t) slot];
    for (size_t k = 0; k < (size_t) numFields; ++k)
        if (h.live[k] != nullptr) h.values[k].store (h.live[k]->load(), std::memory_order_relaxed);
}
void FxRackDSP::process (juce::AudioBuffer<float>& layers, int samples, double bpm) noexcept {
    juce::ScopedNoDenormals noDenormals;
    // Reset requests are taken per slot in processChunk, after that slot's knob values are read.
    samples = juce::jmin (samples, layers.getNumSamples());
    if (samples <= 0 || layers.getNumChannels() < 4) return;
    const double tempo = std::isfinite (bpm) && bpm > 1.0 ? bpm : 120.0;
    const int chunk = juce::jmax (1, dry.getNumSamples());
    float* const* channels = layers.getArrayOfWritePointers();
    for (int done = 0; done < samples; done += chunk) {
        const int n = juce::jmin (chunk, samples - done);
        float* part[4] { channels[0] + done, channels[1] + done, channels[2] + done, channels[3] + done };
        juce::AudioBuffer<float> view (part, 4, n);   // refers to the host's data, allocates nothing
        processChunk (view, n, tempo);
    }
}
void FxRackDSP::processChunk (juce::AudioBuffer<float>& layers, int samples, double tempo) noexcept {
    float* ch[4] { layers.getWritePointer (0), layers.getWritePointer (1), layers.getWritePointer (2), layers.getWritePointer (3) };
    bool inQuiet[2] { stereoPeak (ch[0], ch[1], samples) < idleSilence, stereoPeak (ch[2], ch[3], samples) < idleSilence };
    const bool canFade = dry.getNumChannels() >= 4 && dry.getNumSamples() >= samples;
    for (int i = 0; i < fxSlots; ++i) {
        const auto& p = params[(size_t) i];
        auto& t = transitions[(size_t) i];
        auto& h = held[(size_t) i];
        // The knobs first, then the Type and the reset request. The message thread writes in the
        // opposite order (reset request, Type, knobs), so a knob value that is already new is always
        // seen together with the change it belongs to, and is never applied to the old effect.
        float knobs[numFields];
        for (size_t k = 0; k < (size_t) numFields; ++k) knobs[k] = h.live[k] != nullptr ? h.live[k]->load() : 0.0f;
        const Config wanted = requested (p);
        const unsigned bit = 1u << (unsigned) i;
        if ((resetMask.load (std::memory_order_acquire) & bit) != 0
            && (resetMask.fetch_and (~bit, std::memory_order_acq_rel) & bit) != 0) t.pending = true;
        bool routed[2] {};
        float target[2] {};
        for (int l = 0; l < 2; ++l) routed[l] = p.layer[l] != nullptr && p.layer[l]->load() >= 0.5f;
        const auto aim = [&] {
            const bool settled = ! t.pending && wanted == t.active;
            for (int l = 0; l < 2; ++l) {
                target[l] = settled && t.active.type != 0 && routed[l] ? 1.0f : 0.0f;
                if (t.quiet[l] || ! canFade) { t.gain[l] = target[l]; t.feed[l] = 1.0f; }   // silent here: nothing to fade
            }
        };
        aim();
        if ((t.pending || ! (wanted == t.active)) && t.gain[0] <= 0.0f && t.gain[1] <= 0.0f) {
            // The old effect is no longer heard: clear it and switch. The new one starts from silence.
            resetSlot (i);
            t.active = wanted; t.pending = false;
            t.feed[0] = t.feed[1] = 0.0f;
            aim();
        }
        if (! t.pending && wanted == t.active)   // settled: the effect follows the knobs
            for (size_t k = 0; k < (size_t) numFields; ++k) h.values[k].store (knobs[k], std::memory_order_relaxed);
        // A freshly started effect first gets its input faded in (20 ms) while its output is still dry,
        // so no filter, delay line or reverb starts in the middle of a waveform (a delay would repeat
        // that edge later, a +12 dB low shelf would ring for 100 ms); then its output fades in. One
        // after the other, not together: together the level dipped by up to 2.5 dB halfway through.
        bool running[2] {}, fading[2] {};
        int feedDone[2] {};   // the first sample with the input fully in: the output fades in from here
        for (int l = 0; l < 2; ++l) {
            running[l] = t.gain[l] > 0.0f || target[l] > 0.0f;
            fading[l] = running[l] && (t.gain[l] < 1.0f || target[l] < 1.0f);
            if (fading[l])
                for (int c = 2 * l; c < 2 * l + 2; ++c) std::copy (ch[c], ch[c] + samples, dry.getWritePointer (c));
            if (running[l] && t.feed[l] < 1.0f) {
                float f = t.feed[l];
                int k = 0;
                for (; k < samples && f < 1.0f; ++k) {
                    f = std::min (1.0f, f + fadeStep);
                    const float w = f * f * (3.0f - 2.0f * f);
                    ch[2 * l][k] *= w; ch[2 * l + 1][k] *= w;
                }
                t.feed[l] = f;
                feedDone[l] = k;
            }
        }
        runSlot (i, h.view, layers, samples, tempo, running);
        for (int l = 0; l < 2; ++l) {
            if (fading[l]) {
                const float goal = target[l];
                const float* dryL = dry.getReadPointer (2 * l);
                const float* dryR = dry.getReadPointer (2 * l + 1);
                float* outL = ch[2 * l];
                float* outR = ch[2 * l + 1];
                float g = t.gain[l];
                for (int k = 0; k < samples; ++k) {
                    if (k >= feedDone[l]) g = goal > g ? std::min (goal, g + fadeStep) : std::max (goal, g - fadeStep);
                    const float w = g * g * (3.0f - 2.0f * g);   // smoothstep: no corner at either end
                    outL[k] = dryL[k] + w * (outL[k] - dryL[k]);
                    outR[k] = dryR[k] + w * (outR[k] - dryR[k]);
                }
                t.gain[l] = g;
            }
            if (! running[l]) t.feed[l] = 0.0f;   // its history is cleared: the next start is a fresh one
            const bool outQuiet = running[l] ? stereoPeak (ch[2 * l], ch[2 * l + 1], samples) < idleSilence : inQuiet[l];
            t.quiet[l] = inQuiet[l] && outQuiet;
            inQuiet[l] = outQuiet;   // this slot's output is the next slot's input
        }
    }
}
}
