#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>

namespace rs::chorus {

enum Model : int { classic = 0, spatial, liquid, digital, poly, sine, ensemble, numModels };
inline constexpr const char* modelNames[numModels] { "Classic", "Spatial", "Liquid", "Digital", "Poly", "Sine", "Ensemble" };
inline constexpr int modeCounts[numModels] { 2, 7, 2, 3, 3, 1, 2 };
enum ClassicMode : int { classicChorus = 0, classicVibrato };
enum SpatialMode : int { spatial1 = 0, spatial2, spatial3, spatial4, spatial1and4, spatial2and4, spatial3and4 };
enum LiquidDepth : int { liquidNormal = 0, liquidDeep };
enum DigitalMode : int { pitchShift = 0, modDelay, multiVoice };
enum PolyMode : int { polyI = 0, polyII, polyBoth };
enum EnsembleLines : int { ensembleThree = 0, ensembleSix };
inline constexpr int minVoices = 2, maxVoices = 8, maxLines = 6, maxBuckets = 512;
inline constexpr float minDelayMs = 1.0f, maxDelayMs = 50.0f, maxFeedback = 0.95f, holdSeconds = 0.25f;

struct Settings {
    int model = classic;
    int mode = 0;
    float rate01 = 0.5f;
    float depth01 = 0.5f;
    float width01 = 1.0f;
    float mix01 = 0.5f;
    bool variant = false;
    bool dualStereo = false;
    float delayMs = 15.0f;
    float feedback = 0.0f;
    int voices = 4;
    float outputDb = 0.0f;
    bool hq = true;
    float vibratoRate01 = 0.5f;
    float vibratoDepth01 = 0.3f;
};

inline bool modeRestarts (int model) noexcept { return model == digital || model == ensemble; }
inline bool variantRestarts (int model) noexcept { return model == liquid; }

inline double classicChorusHz (float intensity01) noexcept { return 0.3 * std::pow (10.0, (double) std::clamp (intensity01, 0.0f, 1.0f)); }
inline double classicVibratoHz (float rate01) noexcept { return 1000.0 / (325.0 - 235.0 * (double) std::clamp (rate01, 0.0f, 1.0f)); }
inline double liquidHz (float rate01) noexcept { return 0.1 * std::pow (80.0, (double) std::clamp (rate01, 0.0f, 1.0f)); }
inline double digitalHz (float rate01) noexcept { return 0.05 * std::pow (200.0, (double) std::clamp (rate01, 0.0f, 1.0f)); }
inline double ensembleChorusHz (float rate01) noexcept { return 0.2 * std::pow (8.0, (double) std::clamp (rate01, 0.0f, 1.0f)); }
inline double ensembleVibratoHz (float rate01) noexcept { return 3.0 * std::pow (4.0, (double) std::clamp (rate01, 0.0f, 1.0f)); }
inline double digitalDepthMs (float depth01, float delayMs) noexcept {
    const double d = std::clamp ((double) depth01, 0.0, 1.0);
    return std::min (8.0 * d * d, 0.9 * (double) delayMs);
}
inline double detuneCents (float depth01, int channel) noexcept { return (channel == 0 ? -9.0 : 11.0) * 2.0 * (double) std::clamp (depth01, 0.0f, 1.0f); }

namespace detail {
constexpr double pi = 3.14159265358979323846;
using Complex = std::complex<double>;

inline float softClip (float x) noexcept {
    const float c = std::clamp (x, -3.0f, 3.0f), c2 = c * c;
    return c * (27.0f + c2) / (27.0f + 9.0f * c2);
}
inline double logCosh (double x) noexcept { const double a = std::abs (x); return a + std::log1p (std::exp (-2.0 * a)) - 0.69314718055994531; }

struct Random {
    std::uint32_t state = 0x9E3779B9u;
    void seed (std::uint32_t s) noexcept { state = s != 0 ? s : 0x9E3779B9u; }
    float uniform() noexcept { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return (float) state * 2.3283064e-10f; }
    float unitNoise() noexcept { return (uniform() + uniform() - 1.0f) * 2.4494897f; }
};

enum Shape : int { sineShape = 0, triangleShape, roundedShape };
inline float lfo (int shape, double phase) noexcept {
    phase -= std::floor (phase);
    if (shape == sineShape) return (float) std::sin (2.0 * pi * phase);
    const float t = (float) (phase < 0.5 ? 4.0 * phase - 1.0 : 3.0 - 4.0 * phase);
    return shape == triangleShape ? t : t * (1.5f - 0.5f * t * t);
}

struct Biquad { float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f; };
inline Biquad butterworthLowPass (double hz, double fs) noexcept {
    const double w = 2.0 * pi * std::min (hz, 0.45 * fs) / fs, c = std::cos (w), alpha = std::sin (w) / (2.0 * 0.70710678118654752), a0 = 1.0 + alpha;
    return { (float) ((1.0 - c) * 0.5 / a0), (float) ((1.0 - c) / a0), (float) ((1.0 - c) * 0.5 / a0), (float) (-2.0 * c / a0), (float) ((1.0 - alpha) / a0) };
}
struct BiquadState {
    float z1 = 0.0f, z2 = 0.0f;
    float run (const Biquad& f, float x) noexcept { const float y = f.b0 * x + z1; z1 = f.b1 * x - f.a1 * y + z2; z2 = f.b2 * x - f.a2 * y; return y; }
};

struct ZohFilter {
    static constexpr int pairs = 2, tableSize = 257;
    std::array<Complex, pairs> residue {}, decay {}, hold {};
    std::array<std::array<Complex, tableSize>, pairs> step {};
    void design (double hz, double fs) {
        const double wc = 2.0 * pi * std::min (hz, 0.45 * fs), period = 1.0 / fs;
        std::array<Complex, 2 * pairs> poles;
        for (int k = 0; k < 2 * pairs; ++k) poles[(size_t) k] = wc * std::exp (Complex (0.0, pi * 0.5 + pi * (2.0 * k + 1.0) / (4.0 * pairs)));
        for (int k = 0; k < pairs; ++k) {
            const auto p = poles[(size_t) k];
            Complex denominator = 1.0;
            for (int j = 0; j < 2 * pairs; ++j) if (j != k) denominator *= p - poles[(size_t) j];
            residue[(size_t) k] = std::pow (wc, 2.0 * pairs) / denominator;
            decay[(size_t) k] = std::exp (p * period);
            hold[(size_t) k] = (decay[(size_t) k] - 1.0) / p;
            for (int i = 0; i < tableSize; ++i)
                step[(size_t) k][(size_t) i] = (std::exp (p * (1.0 - (double) i / (tableSize - 1)) * period) - 1.0) / p;
        }
    }
};
struct ZohState {
    std::array<Complex, ZohFilter::pairs> x {};
    float held = 0.0f;
    void begin (const ZohFilter& f) noexcept {
        for (size_t k = 0; k < x.size(); ++k) x[k] = f.decay[k] * x[k] + f.hold[k] * (double) held;
    }
    void stepAt (const ZohFilter& f, float tau, float value) noexcept {
        const double change = (double) (value - held);
        held = value;
        if (change == 0.0) return;
        const float position = std::clamp (tau, 0.0f, 1.0f) * (float) (ZohFilter::tableSize - 1);
        const int i = std::min ((int) position, ZohFilter::tableSize - 2);
        const double fraction = (double) (position - (float) i);
        for (size_t k = 0; k < x.size(); ++k) {
            const auto& t = f.step[k];
            x[k] += change * (t[(size_t) i] + (t[(size_t) i + 1] - t[(size_t) i]) * fraction);
        }
    }
    float output (const ZohFilter& f) const noexcept {
        double y = 0.0;
        for (size_t k = 0; k < x.size(); ++k) y += 2.0 * (f.residue[k] * x[k]).real();
        return (float) y;
    }
};

inline float hermite (float ym, float y0, float y1, float y2, float f) noexcept {
    return y0 + 0.5f * f * (y1 - ym + f * (2.0f * ym - 5.0f * y0 + 4.0f * y1 - y2 + f * (3.0f * (y0 - y1) + y2 - ym)));
}
}

class Processor {
public:
    static constexpr int tick = 16;

    void prepare (double sampleRate) {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        int size = 1;
        while (size < (int) std::ceil (0.25 * fs) + 8) size <<= 1;
        ringMask = size - 1;
        for (auto& layer : layers) for (auto& ring : layer.rings) ring.assign ((size_t) size, 0.0f);
        smoothing = (float) (1.0 - std::exp (-(double) tick / (0.04 * fs)));
        companderAttack = (float) (1.0 - std::exp (-1.0 / (0.003 * fs)));
        companderRelease = (float) (1.0 - std::exp (-1.0 / (0.03 * fs)));
        dcCoefficient = (float) std::exp (-2.0 * detail::pi * 10.0 / fs);
        highPassCoefficient = (float) std::exp (-2.0 * detail::pi * 80.0 / fs);
        configured = -1;
        configure (target);
        reset();
    }
    void reset() noexcept {
        for (int l = 0; l < 2; ++l) resetLayer (l);
        current = derive (target); preampTanh = std::tanh ((double) current.preampBias);
        remaining = 0;
    }
    void resetLayer (int layer) noexcept {
        auto& s = layers[(size_t) std::clamp (layer, 0, 1)];
        for (auto& line : s.lines) resetLine (line);
        for (auto& ring : s.rings) std::fill (ring.begin(), ring.end(), 0.0f);
        s.position = 0;
        s.phase = s.panPhase = s.vibratoPhase = 0.0;
        for (int v = 0; v < maxVoices; ++v) s.heads[(size_t) v] = (double) v / maxVoices;
        s.voiceTone = {};
        s.wetPrevious[0] = s.wetPrevious[1] = 0.0f;
        s.preampPrevious[0] = s.preampPrevious[1] = 0.0f;
        s.preampAntiGain = -1.0;
        s.dc[0] = s.dc[1] = s.dcPrevious[0] = s.dcPrevious[1] = 0.0f;
        s.drift = 0.0f;
        s.random.seed (layer == 0 ? 0x1234567u : 0x7654321u);
        s.activeLines = topology.lines;
    }
    void setTargets (const Settings& s, bool snap) noexcept {
        target = sanitize (s);
        if (restartKey (target) != configured) {
            configure (target);
            for (int l = 0; l < 2; ++l) resetLayer (l);
            snap = true;
        } else topology = makeTopology (target);
        if (snap) { current = derive (target); preampTanh = std::tanh ((double) current.preampBias); remaining = 0; }
    }
    void process (float* const* ch, const bool* enabled, int n) noexcept {
        int done = 0;
        while (done < n) {
            if (remaining == 0) nextTick();
            const int length = std::min (remaining, n - done);
            for (int layer = 0; layer < 2; ++layer)
                if (enabled[layer]) run (layers[(size_t) layer], ch[2 * layer] + done, ch[2 * layer + 1] + done, length);
            remaining -= length; done += length;
        }
    }
    const Settings& settings() const noexcept { return target; }

private:
    struct Topology {
        int lines = 2, buckets = 256, shape = detail::triangleShape;
        std::array<double, maxLines> offset {};
        std::array<int, maxLines> source {};
        std::array<float, maxLines> toLeft {}, toRight {};
        bool analog = true, compander = false, highPass = false, preamp = false, classicLeftDry = false, drift = true;
        int digitalMode = -1, voices = 4;
        double inputHz = 8000.0, outputHz = 8000.0;
    };
    struct Derived {
        double rateHz = 1.0, vibratoHz = 6.0, panHz = 0.3;
        std::array<double, maxLines> clockMid {}, clockSwing {}, vibratoSwing {};
        double delaySamples = 480.0, depthSamples = 0.0, detune = 0.0;
        float wetGain = 1.0f, feedback = 0.0f, dryMix = 0.7071f, wetMix = 0.7071f, width = 1.0f, output = 1.0f;
        float preampGain = 0.5f, preampBias = 0.08f;
    };
    struct Line {
        std::array<float, maxBuckets> bucket {};
        int position = 0;
        double clockPhase = 0.0;
        std::array<float, 4> history {};
        detail::BiquadState inputFilter;
        float highPass = 0.0f, highPassPrevious = 0.0f;
        detail::ZohState output;
        float compressorEnvelope = 0.0f, expanderEnvelope = 0.0f;
    };
    struct Layer {
        std::array<Line, maxLines> lines;
        std::array<std::vector<float>, 2> rings;
        int position = 0;
        double phase = 0.0, panPhase = 0.0, vibratoPhase = 0.0;
        std::array<double, maxVoices> heads {};
        std::array<float, maxVoices> voiceTone {};
        float wetPrevious[2] {}, preampPrevious[2] {}, dc[2] {}, dcPrevious[2] {};
        double preampAntiPrevious[2] {};   // the preamp antiderivative at preampPrevious, under preampAntiGain / preampAntiBias
        double preampAntiGain = -1.0, preampAntiBias = 0.0;
        float drift = 0.0f;
        detail::Random random;
        int activeLines = 0;
    };

    static Settings sanitize (Settings s) noexcept {
        s.model = std::clamp (s.model, 0, numModels - 1);
        s.mode = std::clamp (s.mode, 0, modeCounts[s.model] - 1);
        const auto unit = [] (float v) { return std::isfinite (v) ? std::clamp (v, 0.0f, 1.0f) : 0.0f; };
        s.rate01 = unit (s.rate01); s.depth01 = unit (s.depth01); s.width01 = unit (s.width01); s.mix01 = unit (s.mix01);
        s.vibratoRate01 = unit (s.vibratoRate01); s.vibratoDepth01 = unit (s.vibratoDepth01);
        s.outputDb = std::isfinite (s.outputDb) ? std::clamp (s.outputDb, -24.0f, 12.0f) : 0.0f;
        s.delayMs = std::isfinite (s.delayMs) ? std::clamp (s.delayMs, minDelayMs, maxDelayMs) : 15.0f;
        s.feedback = std::isfinite (s.feedback) ? std::clamp (s.feedback, -maxFeedback, maxFeedback) : 0.0f;
        s.voices = std::clamp (s.voices, minVoices, maxVoices);
        return s;
    }
    static int restartKey (const Settings& s) noexcept {
        return s.model * 64 + (modeRestarts (s.model) ? s.mode : 0) * 2 + (variantRestarts (s.model) && s.variant ? 1 : 0);
    }
    static Topology makeTopology (const Settings& s) noexcept {
        Topology t;
        const auto stereoPair = [&t] (double secondOffset, int firstSource, int secondSource) {
            t.lines = 2; t.offset = { 0.0, secondOffset }; t.source = { firstSource, secondSource };
            t.toLeft = { 1.0f, 0.0f }; t.toRight = { 0.0f, 1.0f };
        };
        switch (s.model) {
            case classic:
                t.buckets = 256; t.inputHz = 7500.0; t.outputHz = 7500.0; t.preamp = true;
                t.shape = s.mode == classicVibrato ? detail::sineShape : detail::triangleShape;
                if (s.dualStereo) stereoPair (0.25, 0, 0);
                else { t.lines = 1; t.toLeft = { 1.0f }; t.toRight = { 1.0f }; t.classicLeftDry = s.mode == classicChorus; }
                break;
            case spatial:
                t.buckets = 512; t.inputHz = 10000.0; t.outputHz = 10000.0; t.compander = true; t.highPass = true;
                t.lines = 2; t.offset = { 0.0, 0.5 }; t.source = { 1, 2 };
                t.toLeft = { 0.15f, 0.85f }; t.toRight = { 0.85f, 0.15f };
                break;
            case liquid:
                t.buckets = 512; t.inputHz = s.variant ? 5800.0 : 7000.0; t.outputHz = t.inputHz; t.shape = detail::roundedShape;
                stereoPair (0.0, 1, 2);
                break;
            case poly:
                t.buckets = 128; t.inputHz = 7800.0; t.outputHz = 7800.0;
                stereoPair (s.mode == polyBoth ? 0.0 : 0.5, 0, 0);
                break;
            case ensemble: {
                t.buckets = 256; t.inputHz = 7000.0; t.outputHz = 7000.0; t.shape = detail::sineShape;
                const bool six = s.mode == ensembleSix;
                t.lines = six ? 6 : 3;
                for (int i = 0; i < t.lines; ++i) {
                    t.offset[(size_t) i] = six ? i / 6.0 : i / 3.0;
                    t.source[(size_t) i] = 0;
                    t.toLeft[(size_t) i] = six ? (i % 2 == 0 ? 2.0f / 3.0f : 0.0f) : 1.0f / 3.0f;
                    t.toRight[(size_t) i] = six ? (i % 2 == 0 ? 0.0f : 2.0f / 3.0f) : 1.0f / 3.0f;
                }
                break;
            }
            case digital:
            case sine:
            default:
                t.analog = false; t.drift = false; t.lines = 0; t.shape = detail::sineShape;
                t.digitalMode = s.model == sine ? -2 : s.mode; t.voices = s.voices;
                break;
        }
        return t;
    }
    void configure (const Settings& s) {
        topology = makeTopology (s);
        inputFilter = detail::butterworthLowPass (topology.inputHz, fs);
        outputFilter.design (topology.outputHz, fs);
        for (int v = 0; v < maxVoices; ++v)
            voiceToneCoefficient[(size_t) v] = (float) (1.0 - std::exp (-2.0 * detail::pi * (7000.0 + 1300.0 * ((v * 3) % maxVoices)) / fs));
        configured = restartKey (s);
    }
    Derived derive (const Settings& s) const noexcept {
        Derived d;
        const double buckets = (double) topology.buckets;
        const auto range = [&] (int line, double minMs, double maxMs, double depth) {
            const double high = buckets / (minMs * 0.001), low = buckets / (maxMs * 0.001);
            d.clockMid[(size_t) line] = 0.5 * (high + low);
            d.clockSwing[(size_t) line] = 0.5 * (high - low) * std::clamp (depth, 0.0, 1.0);
        };
        const auto allLines = [&] (double minMs, double maxMs, double depth) { for (int i = 0; i < maxLines; ++i) range (i, minMs, maxMs, depth); };
        float internalMix = s.mix01;
        switch (s.model) {
            case classic:
                if (s.mode == classicVibrato) { d.rateHz = classicVibratoHz (s.rate01); allLines (1.3, 4.3, s.depth01); }
                else { d.rateHz = classicChorusHz (s.rate01); allLines (1.3, 4.3, 0.25 + 0.75 * s.rate01); }
                d.preampGain = s.variant ? 1.8f : 0.5f;
                d.preampBias = s.variant ? 0.22f : 0.08f;
                break;
            case spatial: {
                static constexpr double centre[] { 10.0, 7.5, 7.5, 7.5, 10.0, 7.5, 7.5 };
                static constexpr double deviation[] { 2.0, 2.5, 1.5, 1.5, 2.0, 2.5, 1.5 };
                static constexpr double hz[] { 0.25, 0.25, 0.5, 0.5, 0.25, 0.25, 0.5 };
                const auto m = (size_t) s.mode;
                d.rateHz = hz[m];
                allLines (centre[m] - deviation[m], centre[m] + deviation[m], 1.0);
                d.wetGain = s.mode >= spatial4 ? 0.85f : 0.62f;
                break;
            }
            case liquid: {
                const double scale = s.variant ? 1.5 : 1.0;
                d.rateHz = liquidHz (s.rate01);
                if (s.mode == liquidDeep) allLines (2.6 * scale, 9.0 * scale, 1.0);
                else allLines (4.0 * scale, 6.5 * scale, 1.0);
                break;
            }
            case poly:
                if (s.mode == polyBoth) { d.rateHz = 9.75; allLines (3.3, 3.7, 1.0); }
                else { d.rateHz = s.mode == polyI ? 0.513 : 0.863; allLines (1.66, 5.35, 1.0); }
                break;
            case ensemble:
                d.rateHz = ensembleChorusHz (s.rate01);
                d.vibratoHz = ensembleVibratoHz (s.vibratoRate01);
                allLines (3.0, 9.0, s.depth01);
                for (int i = 0; i < maxLines; ++i) d.vibratoSwing[(size_t) i] = d.clockMid[(size_t) i] * 0.12 * (double) s.vibratoDepth01;
                break;
            case digital:
            case sine:
            default:
                d.rateHz = digitalHz (s.rate01);
                d.panHz = d.rateHz * 0.618;
                d.delaySamples = (double) s.delayMs * 0.001 * fs;
                d.depthSamples = digitalDepthMs (s.depth01, s.delayMs) * 0.001 * fs;
                d.detune = (double) s.depth01;
                d.feedback = s.feedback;
                break;
        }
        const float angle = internalMix * (float) (detail::pi * 0.5);
        d.dryMix = std::cos (angle);
        d.wetMix = std::sin (angle);
        d.width = s.width01;
        d.output = std::pow (10.0f, s.outputDb / 20.0f);
        return d;
    }
    void nextTick() noexcept {
        const Derived goal = derive (target);
        const auto glide = [this] (auto& value, auto goalValue) { value += (goalValue - value) * (decltype (value + goalValue)) smoothing; };
        glide (current.rateHz, goal.rateHz); glide (current.vibratoHz, goal.vibratoHz); glide (current.panHz, goal.panHz);
        for (size_t i = 0; i < (size_t) maxLines; ++i) {
            glide (current.clockMid[i], goal.clockMid[i]); glide (current.clockSwing[i], goal.clockSwing[i]); glide (current.vibratoSwing[i], goal.vibratoSwing[i]);
        }
        glide (current.delaySamples, goal.delaySamples); glide (current.depthSamples, goal.depthSamples); glide (current.detune, goal.detune);
        glide (current.wetGain, goal.wetGain); glide (current.feedback, goal.feedback); glide (current.dryMix, goal.dryMix);
        glide (current.wetMix, goal.wetMix); glide (current.width, goal.width); glide (current.output, goal.output);
        glide (current.preampGain, goal.preampGain); glide (current.preampBias, goal.preampBias);
        preampTanh = std::tanh ((double) current.preampBias);
        for (auto& layer : layers) {
            if (topology.drift) {
                layer.drift = layer.drift * 0.998f + 0.0015f * layer.random.unitNoise();
                layer.drift = std::clamp (layer.drift, -0.05f, 0.05f);
            } else layer.drift = 0.0f;
        }
        remaining = tick;
    }
    void resetLine (Line& line) noexcept {
        line.bucket.fill (0.0f);
        line.position = 0; line.clockPhase = 0.0;
        line.history.fill (0.0f);
        line.inputFilter = {};
        line.highPass = line.highPassPrevious = 0.0f;
        line.output = {};
        line.compressorEnvelope = line.expanderEnvelope = 0.0f;
    }
    float preamp (Layer& layer, int channel, float x) const noexcept {
        const double gain = current.preampGain, bias = current.preampBias, tb = preampTanh;
        const double u1 = gain * (double) x, u0 = gain * (double) layer.preampPrevious[channel];
        layer.preampPrevious[channel] = x;
        const auto anti = [bias, tb] (double u) { return detail::logCosh (u + bias) - tb * u; };
        // The previous sample's antiderivative is reused while the preamp settings stand still (per tick).
        const bool cached = std::memcmp (&layer.preampAntiGain, &gain, sizeof (double)) == 0 && std::memcmp (&layer.preampAntiBias, &bias, sizeof (double)) == 0;
        const double anti1 = anti (u1), anti0 = cached ? layer.preampAntiPrevious[channel] : anti (u0);
        layer.preampAntiPrevious[channel] = anti1;
        if (! cached && channel == 1) { layer.preampAntiGain = gain; layer.preampAntiBias = bias; }
        const double shaped = std::abs (u1 - u0) > 1.0e-5 ? (anti1 - anti0) / (u1 - u0) : std::tanh (0.5 * (u0 + u1) + bias) - tb;
        return (float) (shaped / (gain * (1.0 - tb * tb)));
    }
    float runLine (Line& line, float input, double clockHz) noexcept {
        constexpr float reference = 0.3f, floor = 3.0e-5f;
        float x = input;
        if (topology.compander) {
            const float level = std::abs (x);
            line.compressorEnvelope += (level > line.compressorEnvelope ? companderAttack : companderRelease) * (level - line.compressorEnvelope);
            x *= std::sqrt (reference / std::max (line.compressorEnvelope, floor));
        }
        if (topology.highPass) {
            const float y = highPassCoefficient * (line.highPass + x - line.highPassPrevious);
            line.highPassPrevious = x; line.highPass = y; x = y;
        }
        x = line.inputFilter.run (inputFilter, x);
        auto& h = line.history;
        h[0] = h[1]; h[1] = h[2]; h[2] = h[3]; h[3] = x;
        line.output.begin (outputFilter);
        const double advance = std::max (1.0e-3, clockHz / fs), interval = 1.0 / advance;
        const bool hq = target.hq;
        for (double tau = (1.0 - line.clockPhase) * interval; tau < 1.0; tau += interval) {
            const float f = (float) tau;
            float v = hq ? detail::hermite (h[0], h[1], h[2], h[3], f) : h[1] + f * (h[2] - h[1]);
            v = detail::softClip (v);
            const float out = line.bucket[(size_t) line.position];
            line.bucket[(size_t) line.position] = v;
            if (++line.position >= topology.buckets) line.position = 0;
            line.output.stepAt (outputFilter, hq ? f : 0.0f, out);
        }
        line.clockPhase += advance;
        line.clockPhase -= std::floor (line.clockPhase);
        float y = line.output.output (outputFilter);
        if (topology.compander) {
            const float level = std::abs (y);
            line.expanderEnvelope += (level > line.expanderEnvelope ? companderAttack : companderRelease) * (level - line.expanderEnvelope);
            y *= line.expanderEnvelope / reference;
        }
        return y;
    }
    float readRing (const std::vector<float>& ring, int position, double delay) const noexcept {
        const double clamped = std::clamp (delay, 2.0, (double) (ringMask - 3));
        const int whole = (int) clamped;
        const float f = (float) (clamped - whole);
        const auto at = [&] (int d) { return ring[(size_t) ((position - d) & ringMask)]; };
        if (! target.hq) return at (whole) + f * (at (whole + 1) - at (whole));
        return detail::hermite (at (whole - 1), at (whole), at (whole + 1), at (whole + 2), f);
    }
    void runDigital (Layer& s, const float* x, float* wet) noexcept {
        const double twoPi = 2.0 * detail::pi;
        for (int c = 0; c < 2; ++c)
            s.rings[(size_t) c][(size_t) s.position] = x[c] + current.feedback * detail::softClip (s.wetPrevious[c]);
        const int mode = topology.digitalMode;
        const double delay = current.delaySamples, depth = current.depthSamples;
        if (mode == pitchShift) {
            const double window = 0.05 * fs;
            for (int c = 0; c < 2; ++c) {
                const double ratio = std::pow (2.0, detuneCents ((float) current.detune, c) / 1200.0);
                auto& phase = s.heads[(size_t) c];
                phase += (1.0 - ratio) / window;
                phase -= std::floor (phase);
                const double base = delay + (c == 1 ? 0.01 * fs : 0.0);
                const double other = phase + 0.5 - std::floor (phase + 0.5);
                const float w = (float) std::pow (std::sin (detail::pi * phase), 2.0);
                const auto& ring = s.rings[(size_t) c];
                wet[c] = w * readRing (ring, s.position, base + window * phase) + (1.0f - w) * readRing (ring, s.position, base + window * other);
            }
        } else if (mode == modDelay) {
            const double sweep = delay + depth * std::sin (twoPi * s.phase);
            const float pan = 0.5f * (float) std::sin (twoPi * s.panPhase);
            const float motion = 1.0f + 0.1f * (float) std::sin (twoPi * (s.phase + 0.25));
            wet[0] = readRing (s.rings[0], s.position, sweep) * (1.0f - pan) * motion;
            wet[1] = readRing (s.rings[1], s.position, sweep) * (1.0f + pan) * motion;
        } else if (mode == multiVoice) {
            const int count = topology.voices;
            const float norm = std::sqrt (2.0f / (float) count);
            wet[0] = wet[1] = 0.0f;
            for (int v = 0; v < count; ++v) {
                const double spread = count > 1 ? (double) v / (count - 1) : 0.5;
                const double golden = (double) v * 0.6180339887 - std::floor ((double) v * 0.6180339887);
                auto& voicePhase = s.heads[(size_t) v];
                voicePhase += current.rateHz * (0.8 + 0.45 * golden) / fs;
                voicePhase -= std::floor (voicePhase);
                const double voiceDelay = delay * (0.7 + 0.6 * spread) + depth * (0.85 + 0.3 * golden) * std::sin (twoPi * voicePhase);
                const int slot = v % 2 == 0 ? v / 2 : count - 1 - v / 2;
                const float pan = count > 1 ? -1.0f + 2.0f * (float) slot / (float) (count - 1) : 0.0f;
                const float toLeft = 0.5f * (1.0f - pan), toRight = 0.5f * (1.0f + pan);
                const float tap = toLeft * readRing (s.rings[0], s.position, voiceDelay) + toRight * readRing (s.rings[1], s.position, voiceDelay);
                auto& tone = s.voiceTone[(size_t) v];
                tone += voiceToneCoefficient[(size_t) v] * (tap - tone);
                wet[0] += tone * std::sqrt (toLeft) * norm;
                wet[1] += tone * std::sqrt (toRight) * norm;
            }
        } else {
            for (int c = 0; c < 2; ++c)
                wet[c] = readRing (s.rings[(size_t) c], s.position, delay + depth * std::sin (twoPi * (s.phase + 0.25 * c)));
        }
        s.position = (s.position + 1) & ringMask;
        for (int c = 0; c < 2; ++c) s.wetPrevious[c] = wet[c];
    }
    void run (Layer& s, float* left, float* right, int n) noexcept {
        const auto& t = topology;
        if (t.analog && s.activeLines != t.lines) {
            for (int i = s.activeLines; i < t.lines; ++i) resetLine (s.lines[(size_t) i]);
            s.activeLines = t.lines;
        }
        const double rate = current.rateHz * (1.0 + 0.15 * (double) s.drift) / fs;
        const double vibratoRate = current.vibratoHz / fs, panRate = current.panHz / fs;
        for (int k = 0; k < n; ++k) {
            float x[2] { left[k], right[k] };
            if (t.preamp) { x[0] = preamp (s, 0, x[0]); x[1] = preamp (s, 1, x[1]); }
            float wet[2] {};
            if (t.analog) {
                const float sources[3] { 0.5f * (x[0] + x[1]), x[0], x[1] };
                for (int i = 0; i < t.lines; ++i) {
                    auto& line = s.lines[(size_t) i];
                    const auto ui = (size_t) i;
                    double clock = current.clockMid[ui] + current.clockSwing[ui] * (double) detail::lfo (t.shape, s.phase + t.offset[ui]);
                    if (current.vibratoSwing[ui] > 0.0) clock += current.vibratoSwing[ui] * (double) detail::lfo (detail::sineShape, s.vibratoPhase + t.offset[ui]);
                    const float in = sources[t.source[ui]];
                    const float y = runLine (line, in, clock);
                    wet[0] += t.toLeft[ui] * y;
                    wet[1] += t.toRight[ui] * y;
                }
                for (int c = 0; c < 2; ++c) {
                    const float y = s.dc[c] * dcCoefficient + wet[c] - s.dcPrevious[c];
                    s.dcPrevious[c] = wet[c]; s.dc[c] = y; wet[c] = y;
                }
            } else runDigital (s, x, wet);
            const float mid = 0.5f * (wet[0] + wet[1]), side = 0.5f * (wet[0] - wet[1]) * current.width;
            const float wetGain = current.wetMix * current.wetGain;
            float outL = current.dryMix * x[0] + wetGain * (mid + side);
            float outR = current.dryMix * x[1] + wetGain * (mid - side);
            if (t.classicLeftDry) outL += current.width * (x[0] - outL);
            left[k] = outL * current.output;
            right[k] = outR * current.output;
            s.phase += rate; s.phase -= std::floor (s.phase);
            s.vibratoPhase += vibratoRate; s.vibratoPhase -= std::floor (s.vibratoPhase);
            s.panPhase += panRate; s.panPhase -= std::floor (s.panPhase);
        }
    }

    double fs = 48000.0;
    double preampTanh = 0.0;   // tanh (current.preampBias), once per tick
    Settings target;
    Topology topology;
    Derived current;
    int configured = -1, remaining = 0, ringMask = 0;
    float smoothing = 0.01f, companderAttack = 0.007f, companderRelease = 0.0007f;
    float dcCoefficient = 0.9987f, highPassCoefficient = 0.99f;
    detail::Biquad inputFilter;
    detail::ZohFilter outputFilter;
    std::array<float, maxVoices> voiceToneCoefficient {};
    std::array<Layer, 2> layers;
};
}
