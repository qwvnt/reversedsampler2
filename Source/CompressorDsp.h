#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>

namespace rs::comp {

enum Mode : int { vintage = 0, modern = 1, numModes = 2 };
enum Time : int { time1 = 0, time2, time3, time4, time5, time6, opto, numTimes };
inline constexpr const char* timeNames[numTimes] { "1", "2", "3", "4", "5", "6", "OPTO" };
inline constexpr float minThresholdDb = -60.0f, maxThresholdDb = 0.0f, maxRatio = 20.0f, maxMakeupDb = 24.0f;
inline constexpr float vintageKneeDb = 18.0f, maxKneeDb = 24.0f;
inline constexpr float minAttackMs = 0.05f, maxAttackMs = 250.0f, minReleaseMs = 5.0f, maxReleaseMs = 2500.0f;
inline constexpr double holdSeconds = 30.0;

struct Settings {
    int mode = vintage;
    float thresholdDb = -18.0f;
    float ratio = 4.0f;
    float kneeDb = 6.0f;
    float attackMs = 10.0f;
    float releaseMs = 150.0f;
    int time = time2;
    float makeupDb = 0.0f;
    float mix = 1.0f;
};

inline float thresholdFromKnob (float v) noexcept { return minThresholdDb + (maxThresholdDb - minThresholdDb) * std::clamp (v, 0.0f, 100.0f) * 0.01f; }
inline float ratioFromKnob (float v) noexcept { return std::pow (maxRatio, std::clamp (v, 0.0f, 100.0f) * 0.01f); }
inline float ratioToKnob (float r) noexcept { return 100.0f * std::log (std::clamp (r, 1.0f, maxRatio)) / std::log (maxRatio); }
inline float makeupFromKnob (float v) noexcept { return maxMakeupDb * std::clamp (v, 0.0f, 100.0f) * 0.01f; }

inline float kneeOf (const Settings& s) noexcept { return s.mode == vintage ? vintageKneeDb : std::clamp (s.kneeDb, 0.0f, maxKneeDb); }

inline float gainReductionDb (float levelDb, float thresholdDb, float ratio, float kneeDb) noexcept {
    const float over = levelDb - thresholdDb, slope = 1.0f - 1.0f / std::max (1.0f, ratio);
    if (kneeDb > 0.0f && 2.0f * std::abs (over) <= kneeDb) {
        const float x = over + 0.5f * kneeDb;
        return slope * x * x / (2.0f * kneeDb);
    }
    return over > 0.0f ? slope * over : 0.0f;
}
inline float gainReductionDb (const Settings& s, float levelDb) noexcept { return gainReductionDb (levelDb, s.thresholdDb, s.ratio, kneeOf (s)); }
inline float outputDb (const Settings& s, float levelDb) noexcept { return levelDb - gainReductionDb (s, levelDb) + s.makeupDb; }

struct VintageTiming { float attackMs; int stages; float releaseSeconds[3]; };
inline VintageTiming vintageTiming (int time) noexcept {
    switch (std::clamp (time, 0, numTimes - 1)) {
        case time1: return { 0.2f, 1, { 0.3f, 0.0f, 0.0f } };
        case time2: return { 0.2f, 1, { 0.8f, 0.0f, 0.0f } };
        case time3: return { 0.4f, 1, { 2.0f, 0.0f, 0.0f } };
        case time4: return { 0.4f, 1, { 5.0f, 0.0f, 0.0f } };
        case time5: return { 0.4f, 2, { 2.0f, 10.0f, 0.0f } };
        case time6: return { 0.2f, 3, { 0.3f, 10.0f, 25.0f } };
        default:    return { 10.0f, 2, { 0.06f, 1.0f, 0.0f } };
    }
}

class Meters {
public:
    void push (float inputPeak, float outputPeak, float reductionDb, float levelDb) noexcept {
        raise (input, inputPeak); raise (output, outputPeak); raise (reduction, reductionDb); raise (level, levelDb + 200.0f);
    }
    struct Reading { float inputPeak = 0.0f, outputPeak = 0.0f, reductionDb = 0.0f, levelDb = -200.0f; };
    Reading take() noexcept {
        Reading r;
        r.inputPeak = input.exchange (0.0f, std::memory_order_relaxed);
        r.outputPeak = output.exchange (0.0f, std::memory_order_relaxed);
        r.reductionDb = reduction.exchange (0.0f, std::memory_order_relaxed);
        r.levelDb = level.exchange (0.0f, std::memory_order_relaxed) - 200.0f;
        return r;
    }
private:
    static void raise (std::atomic<float>& a, float v) noexcept {
        float current = a.load (std::memory_order_relaxed);
        while (v > current && ! a.compare_exchange_weak (current, v, std::memory_order_relaxed)) {}
    }
    std::atomic<float> input { 0.0f }, output { 0.0f }, reduction { 0.0f }, level { 0.0f };
};

namespace detail {
constexpr double pi = 3.14159265358979323846;
constexpr double tubeBias = 0.18;
inline double logCosh (double x) noexcept { const double a = std::abs (x); return a + std::log1p (std::exp (-2.0 * a)) - 0.69314718055994531; }
inline float toDb (float power) noexcept { return 10.0f * std::log10 (power + 1.0e-20f); }
}

class Processor {
public:
    static constexpr int tick = 16;

    void prepare (double sampleRate) {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        const auto onePole = [this] (double hz) { return (float) (1.0 - std::exp (-2.0 * detail::pi * std::min (hz, 0.45 * fs) / fs)); };
        smoothing = (float) (1.0 - std::exp (-1.0 / (0.02 * fs)));
        sidechainHighPass = (float) std::exp (-2.0 * detail::pi * 60.0 / fs);
        dcCoefficient = (float) std::exp (-2.0 * detail::pi * 8.0 / fs);
        transformerLow = onePole (90.0);
        transformerTop = onePole (21000.0);
        chargeUp = (float) (1.0 - std::exp (-1.0 / (3.0 * fs)));
        chargeDown = (float) (1.0 - std::exp (-1.0 / (12.0 * fs)));
        reset();
    }
    void reset() noexcept {
        for (int l = 0; l < 2; ++l) resetLayer (l);
        current = derive (target);
        remaining = 0;
    }
    void resetLayer (int layer) noexcept { layers[(size_t) std::clamp (layer, 0, 1)] = Layer {}; }
    void setTargets (const Settings& s, bool snap) noexcept {
        target = sanitize (s);
        if (snap) { current = derive (target); remaining = 0; }
    }
    void process (float* const* ch, const bool* enabled, int n) noexcept {
        float inPeak = 0.0f, outPeak = 0.0f, reduction = 0.0f, level = -200.0f;
        bool any = false;
        int done = 0;
        while (done < n) {
            if (remaining == 0) nextTick();
            const int length = std::min (remaining, n - done);
            for (int layer = 0; layer < 2; ++layer)
                if (enabled[layer]) {
                    any = true;
                    run (layers[(size_t) layer], ch[2 * layer] + done, ch[2 * layer + 1] + done, length, inPeak, outPeak, reduction, level);
                }
            remaining -= length; done += length;
        }
        if (any) meterValues.push (inPeak, outPeak, reduction, level);
    }
    Meters& meters() noexcept { return meterValues; }
    const Settings& settings() const noexcept { return target; }

private:
    struct Derived {
        float thresholdDb = -18.0f, ratio = 4.0f, kneeDb = 6.0f, makeup = 1.0f, mix = 1.0f;
        float attack = 0.1f, release = 0.001f, detector = 0.01f;
        std::array<float, 3> stageRelease {};
        int stages = 1;
        bool vintage = true, opto = false;
    };
    struct Layer {
        float detector = 0.0f, reductionDb = 0.0f;
        float gainForDb = 0.0f, gain = 1.0f;
        std::array<float, 3> stage {};
        float charge = 0.0f, optoFor = -1.0f, optoRelease = 0.0f;
        float highPass[2] {}, highPassPrevious[2] {};
        float tubePrevious[2] {}, dcIn[2] {}, dcOut[2] {}, low[2] {}, top[2] {};
        double tubeAntiPrevious[2] {};   // the antiderivative at tubePrevious, so each sample evaluates it once
        bool tubeAntiValid[2] {};
    };
    static Settings sanitize (Settings s) noexcept {
        const auto finite = [] (float v, float fallback) { return std::isfinite (v) ? v : fallback; };
        s.mode = std::clamp (s.mode, 0, numModes - 1);
        s.thresholdDb = std::clamp (finite (s.thresholdDb, -18.0f), minThresholdDb, maxThresholdDb);
        s.ratio = std::clamp (finite (s.ratio, 4.0f), 1.0f, maxRatio);
        s.kneeDb = std::clamp (finite (s.kneeDb, 6.0f), 0.0f, maxKneeDb);
        s.attackMs = std::clamp (finite (s.attackMs, 10.0f), minAttackMs, maxAttackMs);
        s.releaseMs = std::clamp (finite (s.releaseMs, 150.0f), minReleaseMs, maxReleaseMs);
        s.time = std::clamp (s.time, 0, numTimes - 1);
        s.makeupDb = std::clamp (finite (s.makeupDb, 0.0f), 0.0f, maxMakeupDb);
        s.mix = std::clamp (finite (s.mix, 1.0f), 0.0f, 1.0f);
        return s;
    }
    float coefficient (double seconds) const noexcept { return (float) (1.0 - std::exp (-1.0 / (std::max (seconds, 1.0e-6) * fs))); }
    Derived derive (const Settings& s) const noexcept {
        Derived d;
        d.vintage = s.mode == vintage;
        d.thresholdDb = s.thresholdDb; d.ratio = s.ratio; d.kneeDb = kneeOf (s);
        d.makeup = std::pow (10.0f, s.makeupDb / 20.0f); d.mix = s.mix;
        if (d.vintage) {
            const auto timing = vintageTiming (s.time);
            d.opto = s.time == opto;
            d.attack = coefficient (timing.attackMs * 0.001);
            d.stages = timing.stages;
            for (int i = 0; i < 3; ++i) d.stageRelease[(size_t) i] = coefficient (std::max (0.01f, timing.releaseSeconds[i]));
            d.detector = coefficient (d.opto ? 0.01 : 0.004);
        } else {
            d.attack = coefficient (s.attackMs * 0.001);
            d.release = coefficient (s.releaseMs * 0.001);
        }
        return d;
    }
    void nextTick() noexcept {
        const Derived goal = derive (target);
        const float a = std::min (1.0f, smoothing * (float) tick);
        current.thresholdDb += a * (goal.thresholdDb - current.thresholdDb);
        current.ratio += a * (goal.ratio - current.ratio);
        current.kneeDb += a * (goal.kneeDb - current.kneeDb);
        current.makeup += a * (goal.makeup - current.makeup);
        current.mix += a * (goal.mix - current.mix);
        current.attack = goal.attack; current.release = goal.release; current.detector = goal.detector;
        current.stageRelease = goal.stageRelease; current.stages = goal.stages;
        current.vintage = goal.vintage; current.opto = goal.opto;
        remaining = tick;
    }
    float vintageReduction (Layer& s, float targetDb) noexcept {
        const auto& d = current;
        const bool compressing = targetDb > 1.0f;
        s.charge += compressing ? chargeUp * (1.0f - s.charge) : -chargeDown * s.charge;
        s.charge = std::clamp (s.charge, 0.0f, 1.0f);
        float release[3] { d.stageRelease[0], d.stageRelease[1], d.stageRelease[2] };
        if (d.opto) {   // the exp only runs while the charge moves
            if (std::memcmp (&s.optoFor, &s.charge, sizeof (float)) != 0) { s.optoFor = s.charge; s.optoRelease = coefficient (1.0 + 14.0 * (double) s.charge); }
            release[1] = s.optoRelease;
        }
        for (int i = 0; i < d.stages; ++i) {
            auto& g = s.stage[(size_t) i];
            g += (targetDb > g ? d.attack : release[i]) * (targetDb - g);
        }
        const float c = s.charge;
        if (d.opto) return 0.5f * (s.stage[0] + s.stage[1]);
        if (d.stages == 2) return (1.0f - c) * s.stage[0] + c * s.stage[1];
        if (d.stages == 3) return (1.0f - c) * (1.0f - c) * s.stage[0] + 2.0f * c * (1.0f - c) * s.stage[1] + c * c * s.stage[2];
        return s.stage[0];
    }
    float tube (Layer& s, int c, float x, float drive) const noexcept {
        const double tb = tubeTanh, norm = tubeNorm;
        const double u1 = (double) (x * drive), u0 = (double) s.tubePrevious[c];
        s.tubePrevious[c] = (float) u1;
        const auto anti = [tb] (double u) { return detail::logCosh (u + detail::tubeBias) - tb * u; };
        const double anti1 = anti (u1);
        const double anti0 = s.tubeAntiValid[c] ? s.tubeAntiPrevious[c] : anti (u0);
        s.tubeAntiPrevious[c] = anti1; s.tubeAntiValid[c] = true;
        const double shaped = std::abs (u1 - u0) > 1.0e-5 ? (anti1 - anti0) / (u1 - u0) : std::tanh (0.5 * (u0 + u1) + detail::tubeBias) - tb;
        return (float) (shaped * norm) / drive;
    }
    void run (Layer& s, float* left, float* right, int n, float& inPeak, float& outPeak, float& reduction, float& level) noexcept {
        const auto& d = current;
        for (int k = 0; k < n; ++k) {
            const float x[2] { left[k], right[k] };
            inPeak = std::max ({ inPeak, std::abs (x[0]), std::abs (x[1]) });
            float levelDb;
            if (d.vintage) {
                float side = 0.0f;
                for (int c = 0; c < 2; ++c) {
                    const float hp = sidechainHighPass * (s.highPass[c] + x[c] - s.highPassPrevious[c]);
                    s.highPassPrevious[c] = x[c]; s.highPass[c] = hp;
                    side = std::max (side, hp * hp);
                }
                s.detector += d.detector * (side - s.detector);
                levelDb = detail::toDb (2.0f * s.detector);
                s.reductionDb = vintageReduction (s, gainReductionDb (levelDb, d.thresholdDb, d.ratio, d.kneeDb));
            } else {
                levelDb = 20.0f * std::log10 (std::max (std::abs (x[0]), std::abs (x[1])) + 1.0e-10f);
                const float targetDb = gainReductionDb (levelDb, d.thresholdDb, d.ratio, d.kneeDb);
                s.reductionDb += (targetDb > s.reductionDb ? d.attack : d.release) * (targetDb - s.reductionDb);
            }
            if (! (s.reductionDb == s.gainForDb)) {
                s.gainForDb = s.reductionDb;
                s.gain = s.reductionDb == 0.0f ? 1.0f : std::pow (10.0f, -0.05f * s.reductionDb);
            }
            const float gain = s.gain;
            float wet[2];
            for (int c = 0; c < 2; ++c) {
                float y = x[c] * gain;
                if (d.vintage) {
                    y = tube (s, c, y, 0.6f + 0.04f * std::min (s.reductionDb, 25.0f));
                    s.low[c] += transformerLow * (y - s.low[c]);
                    y += 0.6f * (std::tanh (1.8f * s.low[c]) / 1.8f - s.low[c]);
                    s.top[c] += transformerTop * (y - s.top[c]);
                    y = 0.5f * (y + s.top[c]);
                    const float blocked = dcCoefficient * (s.dcOut[c] + y - s.dcIn[c]);
                    s.dcIn[c] = y; s.dcOut[c] = blocked; y = blocked;
                }
                wet[c] = y * d.makeup;
            }
            left[k] = x[0] + d.mix * (wet[0] - x[0]);
            right[k] = x[1] + d.mix * (wet[1] - x[1]);
            outPeak = std::max ({ outPeak, std::abs (left[k]), std::abs (right[k]) });
            reduction = std::max (reduction, s.reductionDb);
            level = std::max (level, levelDb);
        }
    }

    double fs = 48000.0;
    Settings target;
    Derived current;
    int remaining = 0;
    float smoothing = 0.001f, sidechainHighPass = 0.99f, dcCoefficient = 0.999f, transformerLow = 0.01f, transformerTop = 0.9f;
    float chargeUp = 1.0e-5f, chargeDown = 1.0e-6f;
    double tubeTanh = std::tanh (detail::tubeBias), tubeNorm = 1.0 / (1.0 - tubeTanh * tubeTanh);
    std::array<Layer, 2> layers;
    Meters meterValues;
};
}
