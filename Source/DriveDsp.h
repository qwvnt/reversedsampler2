#pragma once
// Full-band distortion for the FX rack (no JUCE dependency, so it is unit-tested on its own).
//
// Signal flow per channel, all inside a 4x oversampled domain:
//   in -> 4x up (polyphase IIR half-bands) -> [type: pre-filter -> drive gain -> curve/model -> post-filter]
//      -> auto-gain -> DC block -> Tone low-pass -> Output -> dry/wet (dry from the same upsampled signal) -> 4x down -> out
// Mixing before the down-sampler keeps dry and wet phase-locked, so Mix never comb-filters.
//
// Types (each modelled on the circuit / medium it is named after):
//   Tube      vintage tube make-up stage: biased tanh, mostly 2nd harmonic, bias and gain sag with the level, soft 13 kHz top.
//   Soft Clip op-amp with silicon diodes in the feedback loop (Tube Screamer style): the 1N4148 diode equation is solved
//             exactly and tabulated; the clean signal leaks through around the clipped part, which is what makes it "soft".
//             The two diodes are slightly mismatched (as real parts are), giving a trace of 2nd harmonic.
//   Tape      Jiles-Atherton magnetic hysteresis (the physics of the tape coating), integrated with RK2 along H,
//             wrapped in record pre-emphasis / playback de-emphasis (50 us, so highs saturate earlier, as on tape),
//             playback head bump (+1.6 dB at 60 Hz) and a gentle gap loss.
//   Triode    12AX7 common-cathode stage (Koren model, B+ 300 V, Ra 100k, Rk 1.5k bypassed) with grid conduction through
//             a 68k grid stopper and Miller low-pass; the plate curve is solved offline and tabulated. Grid conduction
//             shifts the bias when pushed (blocking), so it "blooms" and gets more asymmetric with level.
// Tube, Soft Clip and Triode use first-order antiderivative anti-aliasing (ADAA) on top of the 4x oversampling.
// Auto-gain: each type/drive is loudness-matched for a -12 dBFS sine, so Drive changes the character, not the level.
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace rs::drive {

enum Type : int { tube = 0, softClip = 1, tape = 2, triode = 3, numTypes = 4 };
inline constexpr float maxDriveDb = 36.0f;          // Drive 0..100 % = 0..+36 dB, linear in dB
inline constexpr const char* typeNames[numTypes] { "Tube", "Soft Clip", "Tape", "Triode" };

struct Settings {
    float drive01 = 0.5f;   // 0..1
    float tone01 = 0.5f;    // 0..1: 500 Hz .. 20 kHz low-pass, fully open at the top
    float output = 1.0f;    // linear wet gain
    float mix = 1.0f;       // 0..1
    int type = tube;
};

inline float driveDb (float drive01) noexcept { return maxDriveDb * std::clamp (drive01, 0.0f, 1.0f); }

namespace detail {
constexpr double pi = 3.14159265358979323846;

// ================================================================ Tube (analytic, unchanged from 0.11)
constexpr float tubeBias = 0.45f, tubeBiasSwing = 0.3f, tubeSag = 0.12f;
struct Bias { float b = tubeBias, tb = 0.0f, norm = 1.0f; };
inline Bias makeBias (float b) noexcept { const float tb = std::tanh (b); return { b, tb, 1.0f / (1.0f - tb * tb) }; }
inline float tubeShape (float u, const Bias& bias) noexcept { return (std::tanh (u + bias.b) - bias.tb) * bias.norm; }
inline double logCosh (double x) noexcept { const double a = std::abs (x); return a + std::log1p (std::exp (-2.0 * a)) - 0.69314718055994531; }
inline double tubeAnti (double u, const Bias& bias) noexcept { return (logCosh (u + bias.b) - bias.tb * u) * bias.norm; }
// Level -> bias/sag amount (shared by Tube sag and Triode blocking).
inline float levelEnv (float level) noexcept { return std::min (2.0f, std::max (0.0f, level - 0.3f)); }

// ================================================================ tabulated curves with exact ADAA
// y(u) on [-64, 64], 32 nodes per unit, cubic Hermite with exact slopes. The antiderivative is the exact
// integral of that same Hermite interpolant, so ADAA is consistent with the interpolated curve.
// Outside the range the curve continues linearly with the end slope.
struct CurveTable {
    static constexpr double range = 64.0;
    static constexpr int perUnit = 32, size = (int) (2 * range) * perUnit + 1;
    static constexpr double h = 1.0 / perUnit;
    std::vector<double> y, m, P;
    template <class F> explicit CurveTable (F f) : y ((size_t) size), m ((size_t) size), P ((size_t) size) {
        for (int i = 0; i < size; ++i) f (-range + i * h, y[(size_t) i], m[(size_t) i]);
        for (int i = 1; i < size; ++i)
            P[(size_t) i] = P[(size_t) i - 1] + h * 0.5 * (y[(size_t) i - 1] + y[(size_t) i]) + h * h / 12.0 * (m[(size_t) i - 1] - m[(size_t) i]);
    }
    double value (double u) const noexcept {
        if (u <= -range) return y.front() + m.front() * (u + range);
        if (u >= range) return y.back() + m.back() * (u - range);
        const double pos = (u + range) * perUnit; const int i = std::min ((int) pos, size - 2); const double t = pos - i;
        const double t2 = t * t, t3 = t2 * t; const size_t k = (size_t) i;
        return (2 * t3 - 3 * t2 + 1) * y[k] + (t3 - 2 * t2 + t) * h * m[k] + (-2 * t3 + 3 * t2) * y[k + 1] + (t3 - t2) * h * m[k + 1];
    }
    double anti (double u) const noexcept {
        if (u <= -range) { const double d = u + range; return P.front() + y.front() * d + 0.5 * m.front() * d * d; }
        if (u >= range) { const double d = u - range; return P.back() + y.back() * d + 0.5 * m.back() * d * d; }
        const double pos = (u + range) * perUnit; const int i = std::min ((int) pos, size - 2); const double t = pos - i;
        const double t2 = t * t, t3 = t2 * t, t4 = t2 * t2; const size_t k = (size_t) i;
        return P[k] + h * ((0.5 * t4 - t3 + t) * y[k] + (0.25 * t4 - t3 * (2.0 / 3.0) + 0.5 * t2) * h * m[k]
                           + (-0.5 * t4 + t3) * y[k + 1] + (0.25 * t4 - t3 / 3.0) * h * m[k + 1]);
    }
};

// ---- Soft Clip: op-amp feedback diodes. Diode voltage d solves  x = d + Rf * Id(d)  (x = u * 0.5 V).
constexpr double diodeIs = 2.52e-9, diodeN = 1.752, thermalVt = 0.02585, diodeMismatch = 1.2, softRf = 1.0e5, softVolts = 0.5;
inline const CurveTable& softTable() {
    static const CurveTable t ([] (double u, double& y, double& m) {
        const double nvt = diodeN * thermalVt, x = u * softVolts;
        const auto current = [nvt] (double d) { return diodeIs * std::expm1 (d / nvt) - diodeIs * diodeMismatch * std::expm1 (-d / nvt); };
        double lo = x < 0 ? std::max (x, -2.0) : 0.0, hi = x < 0 ? 0.0 : std::min (x, 2.0);
        for (int k = 0; k < 200 && hi - lo > 1e-15; ++k) { const double mid = 0.5 * (lo + hi); (mid + softRf * current (mid) > x ? hi : lo) = mid; }
        const double d = 0.5 * (lo + hi);
        const double di = (diodeIs * std::exp (d / nvt) + diodeIs * diodeMismatch * std::exp (-d / nvt)) / nvt;
        y = d / softVolts; m = 1.0 / (1.0 + softRf * di);
    });
    return t;
}

// ---- Triode: 12AX7 (Koren), common cathode, bypassed cathode, grid conduction.
namespace triode12ax7 {
constexpr double mu = 100.0, ex = 1.4, kg1 = 1060.0, kp = 600.0, kvb = 300.0;
constexpr double bPlus = 300.0, ra = 100.0e3, rk = 1.5e3, rGrid = 68.0e3, kGrid = 3.0e-4;
inline double softplus (double z) noexcept { return z > 30.0 ? z : std::log1p (std::exp (z)); }
inline double plateCurrent (double vgk, double vpk) noexcept {
    if (vpk <= 0.0) return 0.0;
    const double e1 = vpk / kp * softplus (kp * (1.0 / mu + vgk / std::sqrt (kvb + vpk * vpk)));
    return e1 > 0.0 ? std::pow (e1, ex) / kg1 : 0.0;
}
inline double plateVoltage (double vgk, double vk) noexcept {   // load line: Vp = B+ - Ra * Ip
    double lo = vk, hi = bPlus;
    for (int k = 0; k < 70; ++k) { const double v = 0.5 * (lo + hi); (v - bPlus + ra * plateCurrent (vgk, v - vk) > 0.0 ? hi : lo) = v; }
    return 0.5 * (lo + hi);
}
struct Bias { double vk = 0, vp = 0; };
inline Bias quiescent() noexcept {   // Vk = Rk * Ip at Vgk = -Vk
    double lo = 0.0, hi = 10.0;
    for (int k = 0; k < 70; ++k) {
        const double vk = 0.5 * (lo + hi), vp = plateVoltage (-vk, vk);
        (vk - rk * plateCurrent (-vk, vp - vk) > 0.0 ? hi : lo) = vk;
    }
    const double vk = 0.5 * (lo + hi);
    return { vk, plateVoltage (-vk, vk) };
}
// Grid voltage swing s for a source swing x through the grid stopper: x = s + Rg * Ig(Vgk), Ig = kGrid * Vgk^1.5.
inline double gridSwing (double x, double vk) noexcept {
    if (x <= vk) return x;
    double lo = vk, hi = x;
    for (int k = 0; k < 70; ++k) { const double s = 0.5 * (lo + hi); (s + rGrid * kGrid * std::pow (s - vk, 1.5) > x ? hi : lo) = s; }
    return 0.5 * (lo + hi);
}
inline double plateSwing (double x, const Bias& q) noexcept {   // inverted, so the stage keeps the dry polarity
    return q.vp - plateVoltage (-q.vk + gridSwing (x, q.vk), q.vk);
}
constexpr double slopeDx = 1.0e-5;   // central-difference step for the plate curve slope (V)
} // namespace triode12ax7
inline const CurveTable& triodeTable() {
    static const CurveTable t = [] {
        using triode12ax7::plateSwing;
        const triode12ax7::Bias q = triode12ax7::quiescent();
        const double dx = triode12ax7::slopeDx;
        const double gain0 = (plateSwing (dx, q) - plateSwing (-dx, q)) / (2 * dx);
        // Everything is captured explicitly: MSVC rejects implicit use of enclosing locals (C3493).
        return CurveTable ([q, gain0, dx] (double u, double& y, double& m) {   // 1 unit = 1 V at the grid
            y = plateSwing (u, q) / gain0;
            m = (plateSwing (u + dx, q) - plateSwing (u - dx, q)) / (2 * dx * gain0);
        });
    }();
    return t;
}
constexpr float triodeBlock = 0.25f;   // grid-conduction bias shift per unit of level envelope (V)

// ADAA through a table. raw = drive * input, shift = slow bias offset (constant within a call).
struct AdaaState { double u = 0, a = 0; bool valid = false; };
inline float tableAdaa (const CurveTable& t, double raw, double shift, AdaaState& st) noexcept {
    constexpr double eps = 1.0e-5;
    const double A = t.anti (raw + shift);
    float y;
    if (! st.valid) y = (float) t.value (raw + shift);
    else { const double du = raw - st.u; y = std::abs (du) > eps ? (float) ((A - st.a) / du) : (float) t.value (0.5 * (raw + st.u) + shift); }
    st.u = raw; st.a = A; st.valid = true;
    return y;
}
inline float tubeAdaa (double raw, const Bias& bias, AdaaState& st) noexcept {
    constexpr double eps = 1.0e-5;
    const double A = tubeAnti (raw, bias);
    float y;
    if (! st.valid) y = tubeShape ((float) raw, bias);
    else { const double du = raw - st.u; y = std::abs (du) > eps ? (float) ((A - st.a) / du) : tubeShape ((float) (0.5 * (raw + st.u)), bias); }
    st.u = raw; st.a = A; st.valid = true;
    return y;
}

// ================================================================ Tape: Jiles-Atherton hysteresis
// dM/dH = [ (1-c) dM_irr + c dMan/dH ] / (1 - c alpha dMan/dH), integrated along H (the model is rate-independent).
constexpr double tapeMs = 1.0, tapeA = 0.4, tapeK = 0.48, tapeC = 0.75, tapeAlpha = 1.6e-3;
constexpr float tapeOut = 1.6f;   // ~ 1 / small-signal susceptibility
struct TapeState { double M = 0, H = 0; };
// Langevin L(q) = coth q - 1/q and L'(q), from one exp (float is plenty here; series near 0 avoids cancellation).
inline void langevin (float q, float& L, float& dL) noexcept {
    const float a = std::abs (q);
    if (a < 0.1f) { const float q2 = q * q; L = q * (1.0f / 3.0f - q2 * (1.0f / 45.0f - q2 * (2.0f / 945.0f))); dL = 1.0f / 3.0f - q2 * (1.0f / 15.0f - q2 * (2.0f / 189.0f)); return; }
    const float e = std::exp (-2.0f * a), inv = 1.0f / (1.0f - e), ia = 1.0f / a;
    L = std::copysign ((1.0f + e) * inv - ia, q);
    dL = ia * ia - 4.0f * e * inv * inv;
}
inline float tapeSlope (float M, float H, float dir) noexcept {
    constexpr float Ms = (float) tapeMs, ia = (float) (1.0 / tapeA), k = (float) tapeK, c = (float) tapeC, al = (float) tapeAlpha;
    float L, dL; langevin ((H + al * M) * ia, L, dL);
    const float diff = Ms * L - M, dMan = Ms * ia * dL;
    const float irr = (dir > 0.0f) == (diff > 0.0f) ? (1.0f - c) * diff / ((1.0f - c) * dir * k - al * diff) : 0.0f;
    return (irr + c * dMan) / (1.0f - c * al * dMan);
}
// Advances the magnetisation from st.H to H and returns M averaged over the step (Simpson over each RK2 sub-step).
// The average is the continuous-time output box-filtered over one sample, i.e. first-order ADAA for a system with
// memory: it suppresses the aliasing from the sharp corners hysteresis makes at every turning point.
inline float tapeStep (TapeState& st, double H) noexcept {
    const double dH = H - st.H;
    if (dH == 0.0) return (float) st.M;
    const int steps = std::clamp ((int) std::ceil (std::abs (dH) * 4.0), 1, 4);
    const float h = (float) (dH / steps), dir = dH > 0.0 ? 1.0f : -1.0f;
    double M = st.M; float h0 = (float) st.H, avg = 0.0f;
    for (int i = 0; i < steps; ++i) {   // RK2 (midpoint)
        const float m0 = (float) M, k1 = tapeSlope (m0, h0, dir);
        const float mid = m0 + 0.5f * h * k1, k2 = tapeSlope (mid, h0 + 0.5f * h, dir);
        M += (double) (h * k2); h0 += h;
        avg += m0 + 4.0f * mid + (float) M;
    }
    if (! std::isfinite (M)) { M = 0.0; avg = 0.0f; }
    st.M = std::clamp (M, -tapeMs, tapeMs); st.H = H;
    return std::clamp (avg / (6.0f * (float) steps), (float) -tapeMs, (float) tapeMs);
}

// ================================================================ loudness compensation (-12 dBFS sine, DC removed)
inline float staticCurve (int type, float v, float g, float env) noexcept {
    switch (type) {
        case softClip: return (float) softTable().value ((double) g * v) + v;
        case triode:   return (float) triodeTable().value ((double) g * v - triodeBlock * env);
        default: { const Bias b = makeBias (tubeBias + tubeBiasSwing * 0.5f * env); return tubeShape (g / (1.0f + tubeSag * env) * v, b); }
    }
}
constexpr int compPoints = 65;
using CompTable = std::array<std::array<float, compPoints>, numTypes>;
inline const CompTable& compTable() {
    static const CompTable table = [] {
        CompTable t {};
        constexpr int n = 512; constexpr double amp = 0.25;
        for (int type = 0; type < numTypes; ++type)
            for (int k = 0; k < compPoints; ++k) {
                const float g = std::pow (10.0f, maxDriveDb * (float) k / (compPoints - 1) / 20.0f);
                const float env = levelEnv ((float) amp * g);
                double sum = 0.0, sq = 0.0; TapeState ts;
                for (int i = 0; i < (type == tape ? 3 * n : n); ++i) {   // tape: two cycles to settle the loop
                    const float v = (float) (amp * std::sin (2.0 * pi * (i + 0.5) / n));
                    const double y = type == tape ? tapeOut * tapeStep (ts, (double) g * v) : staticCurve (type, v, g, env);
                    if (type == tape && i < 2 * n) continue;
                    sum += y; sq += y * y;
                }
                const double mean = sum / n, rms = std::sqrt (std::max (1.0e-12, sq / n - mean * mean));
                t[(size_t) type][(size_t) k] = (float) std::clamp (amp / std::sqrt (2.0) / rms, 0.01, 16.0);
            }
        return t;
    }();
    return table;
}
inline float compensation (int type, float drive01) noexcept {
    const auto& row = compTable()[(size_t) std::clamp (type, 0, numTypes - 1)];
    const float x = std::clamp (drive01, 0.0f, 1.0f) * (compPoints - 1);
    const int i = std::min ((int) x, compPoints - 2);
    return row[(size_t) i] + (row[(size_t) i + 1] - row[(size_t) i]) * (x - (float) i);
}

// ================================================================ polyphase IIR half-band (2x)
// H(z) = 0.5 [A0(z^2) + z^-1 A1(z^2)], A0/A1 = cascades of first-order all-passes (de Soras design).
// Stage 1: 10 coefficients, pass to 20 kHz at 44.1 kHz, >106 dB rejection. Stage 2: 4 coefficients, >81 dB.
constexpr float stage1[10] { 0.035604506f, 0.132901414f, 0.268418803f, 0.415748456f, 0.554672513f,
                             0.674439735f, 0.772419248f, 0.851129568f, 0.915694006f, 0.972407675f };
constexpr float stage2[4]  { 0.064637835f, 0.239844409f, 0.489987741f, 0.804484862f };

template <int N>
struct Polyphase {
    static constexpr int half = N / 2;
    float x0[half] {}, y0[half] {}, x1[half] {}, y1[half] {};
    void reset() noexcept { std::fill (std::begin (x0), std::end (x0), 0.0f); std::fill (std::begin (y0), std::end (y0), 0.0f);
                            std::fill (std::begin (x1), std::end (x1), 0.0f); std::fill (std::begin (y1), std::end (y1), 0.0f); }
    inline float path0 (float v, const float* c) noexcept {
        for (int k = 0; k < half; ++k) { const float y = c[2 * k] * (v - y0[k]) + x0[k]; x0[k] = v; y0[k] = y; v = y; }
        return v;
    }
    inline float path1 (float v, const float* c) noexcept {
        for (int k = 0; k < half; ++k) { const float y = c[2 * k + 1] * (v - y1[k]) + x1[k]; x1[k] = v; y1[k] = y; v = y; }
        return v;
    }
};
template <int N>
struct Up2 : Polyphase<N> {
    inline void process (float x, float& first, float& second, const float* c) noexcept { first = this->path0 (x, c); second = this->path1 (x, c); }
};
template <int N>
struct Down2 : Polyphase<N> {
    float held = 0.0f;
    void reset() noexcept { Polyphase<N>::reset(); held = 0.0f; }
    inline float process (float first, float second, const float* c) noexcept {
        const float y = 0.5f * (this->path0 (first, c) + held);
        held = this->path1 (second, c);
        return y;
    }
};

// First-order section y = b0 x + b1 x1 - a1 y1 (bilinear).
struct FirstOrder { float b0 = 1, b1 = 0, a1 = 0; };
// Shelf (1 + s/wz) / (1 + s/wp) at rate fs.
inline FirstOrder shelf (double fz, double fp, double fs) noexcept {
    const double K = 2.0 * fs, tz = K / (2 * pi * fz), tp = K / (2 * pi * fp), n = 1.0 + tp;
    return { (float) ((1.0 + tz) / n), (float) ((1.0 - tz) / n), (float) ((1.0 - tp) / n) };
}
} // namespace detail

class Processor {
public:
    static constexpr int tick = 16;   // parameter ramps are recomputed every 16 samples

    void prepare (double sampleRate) {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        const double fs4 = 4.0 * fs;
        dcR = (float) (1.0 - 2.0 * detail::pi * 8.0 / fs4);
        tubeTop = onePole (13000.0, fs4);
        millerG = onePole (20000.0, fs4);
        gapG = onePole (26000.0, fs4);
        emphasis = detail::shelf (3183.0, 10066.0, fs4);     // 50 us record pre-emphasis, +10 dB shelf
        deemphasis = detail::shelf (10066.0, 3183.0, fs4);   // exact inverse on playback
        {   // head bump: Simper SVF bell, +1.6 dB at 60 Hz, Q 1
            const double A = std::pow (10.0, 1.6 / 40.0), g = std::tan (detail::pi * 60.0 / fs4), k = 1.0 / A;
            bumpA1 = 1.0 / (1.0 + g * (g + k)); bumpA2 = g * bumpA1; bumpA3 = g * bumpA2; bumpM1 = k * (A * A - 1.0);
        }
        smooth = (float) (1.0 - std::exp (-(double) tick / (0.015 * fs)));
        fadeStep = (float) (1.0 / (0.02 * fs));
        envAttack = (float) (1.0 - std::exp (-1.0 / (0.005 * fs)));
        envRelease = (float) (1.0 - std::exp (-1.0 / (0.12 * fs)));
        (void) detail::softTable(); (void) detail::triodeTable(); (void) detail::compTable();   // build tables off the audio thread
        reset();
    }
    void reset() noexcept {
        for (int l = 0; l < 2; ++l) resetLayer (l);
        current = target; type = prevType = target.type; fade = 1.0f;
        from = derive (current, type, prevType); to = from; remaining = 0;
    }
    void resetLayer (int layer) noexcept { for (auto& c : channels[(size_t) std::clamp (layer, 0, 1)]) c = Channel {}; }
    void setTargets (const Settings& s, bool snap) noexcept {
        target = s; target.type = std::clamp (s.type, 0, numTypes - 1);
        target.drive01 = std::clamp (s.drive01, 0.0f, 1.0f); target.tone01 = std::clamp (s.tone01, 0.0f, 1.0f);
        target.mix = std::clamp (s.mix, 0.0f, 1.0f); target.output = std::max (0.0f, s.output);
        if (snap) reset();
    }
    // ch: 4 channel pointers (layer 1 L/R, layer 2 L/R), processed in place.
    void process (float* const* ch, const bool* enabled, int n) noexcept {
        int done = 0;
        while (done < n) {
            if (remaining == 0) nextTick();
            const int len = std::min (remaining, n - done);
            const int offset = tick - remaining;   // position inside the tick, so ramps never depend on block size
            for (int layer = 0; layer < 2; ++layer)
                if (enabled[layer])
                    for (int c = 0; c < 2; ++c) run (channels[(size_t) layer][(size_t) c], ch[2 * layer + c] + done, len, offset);
            if (fade < 1.0f) fade = std::min (1.0f, fade + fadeStep * (float) len);
            remaining -= len; done += len;
        }
    }

private:
    struct Derived { float gain = 1, compCur = 1, compPrev = 1, toneG = 0, open = 1, output = 1, mix = 1; };
    // Per-type state (one per type, so a crossfade can run two types side by side).
    struct Voice {
        detail::AdaaState adaa; detail::TapeState tape;
        float pre = 0, post = 0, ex = 0, ey = 0, dx = 0, dy = 0;
        double ic1 = 0, ic2 = 0;
    };
    struct Channel {
        detail::Up2<10> up1; detail::Up2<4> up2; detail::Down2<4> down2; detail::Down2<10> down1;
        float dcX = 0, dcY = 0, tone = 0, env = 0;
        std::array<Voice, numTypes> voices {};
    };
    struct Context { detail::Bias bias; float sag = 1, shift = 0; };

    static float onePole (double hz, double rate) noexcept { return (float) (1.0 - std::exp (-2.0 * detail::pi * hz / rate)); }
    static float runFirstOrder (const detail::FirstOrder& f, float x, float& x1, float& y1) noexcept {
        const float y = f.b0 * x + f.b1 * x1 - f.a1 * y1; x1 = x; y1 = y; return y;
    }

    // One oversampled sample of one type (before auto-gain).
    inline float voice (int t, float v, float gain, const Context& c, Voice& z) const noexcept {
        switch (t) {
            case softClip:
                return detail::tableAdaa (detail::softTable(), (double) gain * v, 0.0, z.adaa) + v;   // clipped diodes + clean leak
            case triode:
                z.pre += millerG * (v - z.pre);                                                     // Miller capacitance
                return detail::tableAdaa (detail::triodeTable(), (double) gain * z.pre, c.shift, z.adaa);
            case tape: {
                const float e = runFirstOrder (emphasis, v, z.ex, z.ey);
                const float m = detail::tapeStep (z.tape, (double) gain * e) * detail::tapeOut;
                const float d = runFirstOrder (deemphasis, m, z.dx, z.dy);
                z.post += gapG * (d - z.post);                                                      // gap loss
                const double v3 = z.post - z.ic2, v1 = bumpA1 * z.ic1 + bumpA2 * v3, v2 = z.ic2 + bumpA2 * z.ic1 + bumpA3 * v3;
                z.ic1 = 2.0 * v1 - z.ic1; z.ic2 = 2.0 * v2 - z.ic2;
                return (float) (z.post + bumpM1 * v1);                                              // head bump
            }
            default: {
                const float y = detail::tubeAdaa ((double) gain * c.sag * v, c.bias, z.adaa);
                z.post += tubeTop * (y - z.post);                                                   // soft vintage top
                return z.post;
            }
        }
    }

    Derived derive (const Settings& s, int cur, int prev) const noexcept {
        Derived d;
        d.gain = std::pow (10.0f, driveDb (s.drive01) / 20.0f);
        d.compCur = detail::compensation (cur, s.drive01); d.compPrev = detail::compensation (prev, s.drive01);
        const double fc = 500.0 * std::pow (40.0, (double) s.tone01), g = std::tan (detail::pi * std::min (fc, 0.45 * 4.0 * fs) / (4.0 * fs));
        d.toneG = (float) (g / (1.0 + g));
        d.open = std::clamp ((s.tone01 - 0.9f) * 10.0f, 0.0f, 1.0f);
        d.output = s.output; d.mix = s.mix;
        return d;
    }
    void nextTick() noexcept {
        from = to;
        if (target.type != type) {   // start a 20 ms crossfade from the curve currently heard
            prevType = fade < 0.5f ? prevType : type; type = target.type; fade = 0.0f;
            for (auto& layer : channels) for (auto& c : layer) c.voices[(size_t) type] = Voice {};   // stale history
            from.compPrev = detail::compensation (prevType, current.drive01);
            from.compCur = detail::compensation (type, current.drive01);
        }
        const auto step = [this] (float& v, float t) { v += (t - v) * smooth; if (std::abs (t - v) < 1.0e-6f) v = t; };
        step (current.drive01, target.drive01); step (current.tone01, target.tone01);
        step (current.output, target.output); step (current.mix, target.mix);
        to = derive (current, type, prevType);
        remaining = tick;
    }
    void run (Channel& s, float* x, int len, int offset) noexcept {
        const float inv = 1.0f / (float) tick;
        const float dGain = (to.gain - from.gain) * inv, dCur = (to.compCur - from.compCur) * inv, dPrev = (to.compPrev - from.compPrev) * inv;
        const float dG = (to.toneG - from.toneG) * inv, dOpen = (to.open - from.open) * inv, dOut = (to.output - from.output) * inv, dMix = (to.mix - from.mix) * inv;
        const float o = (float) offset;
        float gain = from.gain + dGain * o, compCur = from.compCur + dCur * o, compPrev = from.compPrev + dPrev * o;
        float toneG = from.toneG + dG * o, open = from.open + dOpen * o, out = from.output + dOut * o, mix = from.mix + dMix * o;
        float f = fade;
        const bool silentWet = from.mix == 0.0f && to.mix == 0.0f;
        // Slow, level-dependent bias: Tube sag and Triode grid-conduction blocking (updated per sample, applied per call).
        const float envNow = std::min (s.env, 2.0f);
        Context c;
        c.bias = detail::makeBias (detail::tubeBias + detail::tubeBiasSwing * 0.5f * envNow);
        c.sag = 1.0f / (1.0f + detail::tubeSag * envNow);
        c.shift = -detail::triodeBlock * envNow;
        if (auto& a = s.voices[(size_t) tube].adaa; a.valid) a.a = detail::tubeAnti (a.u, c.bias);   // bias moved since last call
        if (auto& a = s.voices[(size_t) triode].adaa; a.valid) a.a = detail::triodeTable().anti (a.u + c.shift);
        Voice& vc = s.voices[(size_t) type]; Voice& vp = s.voices[(size_t) prevType];
        for (int i = 0; i < len; ++i) {
            const float in = x[i];
            float a0, a1, u[4];
            s.up1.process (in, a0, a1, detail::stage1);
            s.up2.process (a0, u[0], u[1], detail::stage2);
            s.up2.process (a1, u[2], u[3], detail::stage2);
            if (! silentWet) {
                for (float& v : u) {
                    float w = voice (type, v, gain, c, vc) * compCur;
                    if (f < 1.0f && prevType != type) { const float p = voice (prevType, v, gain, c, vp) * compPrev; w = p + (w - p) * f; }
                    const float hp = w - s.dcX + dcR * s.dcY; s.dcX = w; s.dcY = hp; w = hp;
                    const float vv = (w - s.tone) * toneG, lp = vv + s.tone; s.tone = lp + vv;
                    w = lp + open * (w - lp);
                    v += mix * (w * out - v);
                }
            }
            const float b0 = s.down2.process (u[0], u[1], detail::stage2);   // keep the order explicit:
            const float b1 = s.down2.process (u[2], u[3], detail::stage2);   // argument evaluation order is unspecified
            x[i] = s.down1.process (b0, b1, detail::stage1);
            const float lvl = detail::levelEnv (std::abs (in) * gain);
            s.env += (lvl > s.env ? envAttack : envRelease) * (lvl - s.env);
            gain += dGain; compCur += dCur; compPrev += dPrev; toneG += dG; open += dOpen; out += dOut; mix += dMix;
            if (f < 1.0f) f = std::min (1.0f, f + fadeStep);
        }
    }

    double fs = 48000.0;
    float dcR = 0.9999f, tubeTop = 0.3f, millerG = 0.5f, gapG = 0.5f, smooth = 0.1f, fadeStep = 0.001f, envAttack = 0.01f, envRelease = 0.001f;
    detail::FirstOrder emphasis, deemphasis;
    double bumpA1 = 1, bumpA2 = 0, bumpA3 = 0, bumpM1 = 0;
    Settings target, current;
    int type = tube, prevType = tube, remaining = 0;
    float fade = 1.0f;
    Derived from, to;
    std::array<std::array<Channel, 2>, 2> channels {};
};

} // namespace rs::drive
