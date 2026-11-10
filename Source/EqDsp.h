#pragma once
// Two-mode equaliser core (no JUCE dependency, so it is unit-tested on its own).
//
// * Every band is built from analog prototypes (RBJ bell / shelf, Butterworth
//   cuts) digitised with magnitude-matched second-order sections: poles by
//   impulse invariance, zeros by matching the analog magnitude at DC, Nyquist
//   and the band frequency (M. Vicanek, "Matched Second Order Digital Filters").
//   Unlike the bilinear transform, highs are not "cramped" towards Nyquist, so
//   Air and Treble keep their analog shape at 44.1/48 kHz without oversampling.
// * Vintage and Modern share one fixed topology: switching mode only glides
//   frequencies, Qs and gains, so it is click-free and needs no crossfade.
// * Vintage curves were fitted to the reference (Pro-Q 4) screenshots with all
//   knobs at maximum: rms error < 0.15 dB from 10 Hz to 20 kHz.
// * Coefficients are recomputed only while a control is gliding, at a fixed
//   32-sample grid (independent of the host block size). Bands at 0 dB are
//   skipped entirely once their filter memory has decayed.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace rs::eq {

enum class Shape : std::uint8_t { bell, lowShelf, highShelf, highPass, lowPass };
enum Band { low = 0, mid, treble, air, numBands };

struct Coeffs { double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0; };

// Front-panel settings. Gains are knob dB (-12..+12); in Vintage they act as an
// amount (+12 = reference curve, negative values mirror it into a cut).
struct Settings {
    float lowDb = 0.0f, midDb = 0.0f, trebleDb = 0.0f, airDb = 0.0f;
    bool vintage = true;
    float lowCutHz = 20.0f, highCutHz = 20000.0f;
};

constexpr float knobRangeDb = 12.0f;
constexpr float lowCutOffHz = 20.5f;      // at or below: Low Cut is off
constexpr float highCutOffHz = 19950.0f;  // at or above: High Cut is off
constexpr double butterworthQ = 0.70710678118654752;

struct Component {
    Shape shape; Band band;
    double vintageHz, vintageQ, vintageDb;  // at knob +12 dB
    double modernHz, modernQ, modernDb;     // at knob +12 dB
};

// Fixed topology. Vintage values: least-squares fit of the reference curves.
inline constexpr std::array<Component, 9> components {{
    // LOW: sub resonance + broad shelf + the classic Pultec dip above it.
    { Shape::bell,      low,    12.7,  1.10,  2.81,    12.7,  1.10,  0.0 },
    { Shape::lowShelf,  low,    106.0, 0.66,  3.81,    100.0, butterworthQ, 12.0 },
    { Shape::bell,      low,    236.0, 0.89, -0.94,    236.0, 0.89,  0.0 },
    // MID: wide, smooth presence of the mid section.
    { Shape::bell,      mid,    958.0, 0.54,  6.14,    1000.0, 0.70, 12.0 },
    // TREBLE: broad 3.4 kHz lift, low-mid dip and gentle top-end relief.
    { Shape::bell,      treble, 3424.0, 0.41, 6.17,    3500.0, 0.70, 12.0 },
    { Shape::bell,      treble, 863.0,  0.70, -3.17,   863.0,  0.70, 0.0 },
    { Shape::highShelf, treble, 9684.0, 0.70, -2.79,   9684.0, 0.70, 0.0 },
    // AIR: Pultec-style shelf with its pre-dip.
    { Shape::highShelf, air,    8895.0, 0.53, 10.39,   10000.0, butterworthQ, 12.0 },
    { Shape::bell,      air,    5902.0, 0.52, -4.04,   5902.0, 0.52, 0.0 },
}};
constexpr int numComponents = (int) components.size();

//==============================================================================
namespace detail {
constexpr double pi = 3.14159265358979323846;

// Normalised analog prototype N(s)/D(s), s = j*frequency/f0.
struct Prototype { double n0, n1, n2, d0, d1, d2; };

inline Prototype prototype (Shape shape, double A, double q) noexcept {
    const double r = std::sqrt (A);
    switch (shape) {
        case Shape::bell:      return { 1.0, A / q, 1.0, 1.0, 1.0 / (A * q), 1.0 };
        case Shape::lowShelf:  return { A * A, A * r / q, A, 1.0, r / q, A };
        case Shape::highShelf: return { A, A * r / q, A * A, A, r / q, 1.0 };
        case Shape::highPass:  return { 0.0, 0.0, 1.0, 1.0, 1.0 / q, 1.0 };
        case Shape::lowPass:   break;
    }
    return { 1.0, 0.0, 0.0, 1.0, 1.0 / q, 1.0 };
}

inline double magnitudeSquared (const Prototype& p, double x) noexcept {
    const double x2 = x * x;
    const double nr = p.n0 - p.n2 * x2, ni = p.n1 * x;
    const double dr = p.d0 - p.d2 * x2, di = p.d1 * x;
    return (nr * nr + ni * ni) / (dr * dr + di * di);
}
} // namespace detail

// Matched second-order design. f0 in Hz, gain in dB (ignored by cuts).
inline Coeffs design (Shape shape, double f0, double q, double gainDb, double sampleRate) noexcept {
    using namespace detail;
    const double fs = std::max (1000.0, sampleRate);
    f0 = std::clamp (f0, 1.0, 0.45 * fs);
    q = std::clamp (q, 0.05, 20.0);
    const double w0 = 2.0 * pi * f0 / fs;
    const double A = std::pow (10.0, gainDb / 40.0);
    const auto p = prototype (shape, A, q);

    // Poles by impulse invariance.
    const double wp = w0 * std::sqrt (p.d0 / p.d2);
    const double zeta = p.d1 / (2.0 * std::sqrt (p.d0 * p.d2));
    const double radius = std::exp (-zeta * wp);
    Coeffs c;
    c.a1 = zeta < 1.0 ? -2.0 * radius * std::cos (wp * std::sqrt (1.0 - zeta * zeta))
                      : -2.0 * radius * std::cosh (wp * std::sqrt (zeta * zeta - 1.0));
    c.a2 = radius * radius;

    // Zeros: |B|^2 = |H_analog|^2 |A|^2 at DC, Nyquist and wm.
    const double A0 = (1.0 + c.a1 + c.a2) * (1.0 + c.a1 + c.a2);
    const double A1 = (1.0 - c.a1 + c.a2) * (1.0 - c.a1 + c.a2);
    const double A2 = -4.0 * c.a2;
    const double wm = std::min (w0, 0.55 * pi);
    const double s = std::sin (0.5 * wm), phi1 = s * s, phi0 = 1.0 - phi1, phi2 = 4.0 * phi0 * phi1;
    const double B0 = magnitudeSquared (p, 0.0) * A0;
    const double B1 = magnitudeSquared (p, pi / w0) * A1;
    const double B2 = (magnitudeSquared (p, wm / w0) * (A0 * phi0 + A1 * phi1 + A2 * phi2) - B0 * phi0 - B1 * phi1) / phi2;
    const double r0 = std::sqrt (std::max (0.0, B0)), r1 = std::sqrt (std::max (0.0, B1));
    const double W = 0.5 * (r0 + r1);
    c.b0 = 0.5 * (W + std::sqrt (std::max (0.0, W * W + B2)));
    c.b1 = 0.5 * (r0 - r1);
    c.b2 = W - c.b0;
    return c;
}

// |H(e^jw)|^2 of a section at normalised frequency w (radians/sample).
inline double magnitudeSquared (const Coeffs& c, double w) noexcept {
    const double s = std::sin (0.5 * w), phi1 = s * s, phi0 = 1.0 - phi1, phi2 = 4.0 * phi0 * phi1;
    const auto poly = [&] (double c0, double c1, double c2) {
        return (c0 + c1 + c2) * (c0 + c1 + c2) * phi0 + (c0 - c1 + c2) * (c0 - c1 + c2) * phi1 - 4.0 * c0 * c2 * phi2;
    };
    return std::max (0.0, poly (c.b0, c.b1, c.b2)) / std::max (1.0e-300, poly (1.0, c.a1, c.a2));
}

// Component parameters for a knob amount (-1..1) and a Vintage weight (0..1).
struct ComponentState { double hz, q, db; };
inline ComponentState componentState (int index, double amount, double vintage) noexcept {
    const auto& k = components[(size_t) index];
    const auto mixLog = [vintage] (double modern, double old) { return modern * std::pow (old / modern, vintage); };
    return { mixLog (k.modernHz, k.vintageHz), mixLog (k.modernQ, k.vintageQ),
             amount * (k.modernDb + vintage * (k.vintageDb - k.modernDb)) };
}

inline double amountForBand (const Settings& s, Band b) noexcept {
    const float db[] { s.lowDb, s.midDb, s.trebleDb, s.airDb };
    return std::clamp ((double) db[b] / knobRangeDb, -1.0, 1.0);
}

// Settled response (dB) for drawing; costs no allocation.
inline void response (const Settings& s, double sampleRate, const float* hz, float* db, int count) noexcept {
    std::array<Coeffs, numComponents + 2> sections;
    std::array<bool, numComponents + 2> used {};
    for (int i = 0; i < numComponents; ++i) {
        const double amount = amountForBand (s, components[(size_t) i].band);
        const auto st = componentState (i, amount, s.vintage ? 1.0 : 0.0);
        if (st.db != 0.0) { sections[(size_t) i] = design (components[(size_t) i].shape, st.hz, st.q, st.db, sampleRate); used[(size_t) i] = true; }
    }
    if (s.lowCutHz > lowCutOffHz) { sections[numComponents] = design (Shape::highPass, s.lowCutHz, butterworthQ, 0.0, sampleRate); used[numComponents] = true; }
    if (s.highCutHz < highCutOffHz) { sections[numComponents + 1] = design (Shape::lowPass, s.highCutHz, butterworthQ, 0.0, sampleRate); used[numComponents + 1] = true; }
    for (int n = 0; n < count; ++n) {
        const double w = 2.0 * detail::pi * std::clamp ((double) hz[n], 1.0, 0.4999 * sampleRate) / sampleRate;
        double m = 1.0;
        for (size_t i = 0; i < sections.size(); ++i) if (used[i]) m *= magnitudeSquared (sections[i], w);
        db[n] = (float) (10.0 * std::log10 (std::max (m, 1.0e-12)));
    }
}

//==============================================================================
// Stereo, two-layer processor for one rack slot. Settings and coefficients are
// shared; each layer keeps its own filter memory.
class Processor {
public:
    static constexpr int numLayers = 2;
    static constexpr int tickSamples = 32;

    void prepare (double sampleRate) noexcept {
        rate = std::max (1000.0, sampleRate);
        const auto ticksFor = [this] (double seconds) { return std::max (1, (int) std::lround (seconds * rate / tickSamples)); };
        gainTicks = ticksFor (0.02); modeTicks = ticksFor (0.05); freqTicks = ticksFor (0.03); wetTicks = ticksFor (0.02);
        reset();
    }

    // Clears all memory; the next setTargets() snaps instead of gliding.
    void reset() noexcept {
        for (int l = 0; l < numLayers; ++l) resetLayer (l);
        active.fill (false);
        phase = 0; needsSnap = true;
    }
    void resetLayer (int layer) noexcept { layers[(size_t) layer] = {}; }

    void setTargets (const Settings& s, bool snap = false) noexcept {
        snap = snap || needsSnap; needsSnap = false;
        for (int b = 0; b < numBands; ++b) gain[(size_t) b].set (amountForBand (s, (Band) b), gainTicks, snap);
        mode.set (s.vintage ? 1.0 : 0.0, modeTicks, snap);
        setCut (lowCut, s.lowCutHz > lowCutOffHz, s.lowCutHz, snap);
        setCut (highCut, s.highCutHz < highCutOffHz, s.highCutHz, snap);
        if (snap) { phase = 0; updateAll(); }
    }

    // channels: L/R of layer 0, then L/R of layer 1. Disabled layers are untouched.
    void process (float* const* channels, const bool* enabled, int numSamples) noexcept {
        int done = 0;
        while (done < numSamples) {
            if (phase == 0) { advance(); phase = tickSamples; }
            const int len = std::min (phase, numSamples - done);
            const int offset = tickSamples - phase;   // position inside the current tick
            for (int l = 0; l < numLayers; ++l)
                if (enabled[l]) processChunk (layers[(size_t) l], channels[2 * l] + done, channels[2 * l + 1] + done, len, offset);
            phase -= len; done += len;
        }
    }

    // True when no section is running (the slot is a bit-exact bypass).
    bool isTransparent() const noexcept {
        for (bool a : active) if (a) return false;
        return lowCut.wet.current == 0.0 && ! lowCut.wet.moving() && highCut.wet.current == 0.0 && ! highCut.wet.moving();
    }

private:
    struct Ramp {
        double current = 0.0, target = 0.0, step = 0.0; int left = 0;
        void set (double t, int ticks, bool snap) noexcept {
            if (snap) { current = target = t; left = 0; return; }
            if (t == target) return;
            target = t; left = ticks; step = (target - current) / ticks;
        }
        bool advance() noexcept {
            if (left <= 0) return false;
            if (--left == 0) current = target; else current += step;
            return true;
        }
        bool moving() const noexcept { return left > 0; }
    };
    struct Cut {
        Shape shape; Ramp logHz, wet; Coeffs c; double wetFrom = 0.0;
        explicit Cut (Shape s) : shape (s) {}
    };
    struct Section { double z1 = 0.0, z2 = 0.0; };
    struct LayerState {
        std::array<std::array<Section, numComponents>, 2> bands {};
        std::array<Section, 2> lowCut {}, highCut {};
    };

    void setCut (Cut& cut, bool on, float hz, bool snap) noexcept {
        const double logHz = std::log ((double) hz);
        if (on && (snap || (cut.wet.current == 0.0 && cut.wet.target == 0.0)))
            cut.logHz.set (logHz, freqTicks, true);           // engage at the knob frequency
        else if (on)
            cut.logHz.set (logHz, freqTicks, false);
        cut.wet.set (on ? 1.0 : 0.0, wetTicks, snap);
        if (snap) cut.wetFrom = cut.wet.current;
    }

    void designCut (Cut& cut) noexcept { cut.c = design (cut.shape, std::exp (cut.logHz.current), butterworthQ, 0.0, rate); }

    void updateComponent (int i) noexcept {
        const auto st = componentState (i, gain[(size_t) components[(size_t) i].band].current, mode.current);
        db[(size_t) i] = st.db;
        if (st.db != 0.0 || active[(size_t) i]) {
            coeffs[(size_t) i] = design (components[(size_t) i].shape, st.hz, st.q, st.db, rate);
            if (st.db != 0.0) active[(size_t) i] = true;
        }
    }
    void updateAll() noexcept {
        for (int i = 0; i < numComponents; ++i) updateComponent (i);
        designCut (lowCut); designCut (highCut);
    }

    void advance() noexcept {
        const bool modeMoved = mode.advance();
        std::array<bool, numBands> moved {};
        for (int b = 0; b < numBands; ++b) moved[(size_t) b] = gain[(size_t) b].advance() || modeMoved;
        for (int i = 0; i < numComponents; ++i)
            if (moved[(size_t) components[(size_t) i].band]) updateComponent (i);
        // A 0 dB section is an exact identity; drop it once its memory has decayed.
        for (int i = 0; i < numComponents; ++i) {
            if (! active[(size_t) i] || db[(size_t) i] != 0.0) continue;
            double energy = 0.0;
            for (auto& layer : layers) for (auto& ch : layer.bands) energy += std::abs (ch[(size_t) i].z1) + std::abs (ch[(size_t) i].z2);
            if (energy < 1.0e-9) {
                active[(size_t) i] = false;
                for (auto& layer : layers) for (auto& ch : layer.bands) ch[(size_t) i] = {};
            }
        }
        for (auto* cut : { &lowCut, &highCut }) {
            cut->wetFrom = cut->wet.current;
            const bool freqMoved = cut->logHz.advance();
            const bool wasOff = cut->wetFrom == 0.0;
            cut->wet.advance();
            if (freqMoved || (wasOff && cut->wet.current > 0.0)) designCut (*cut);
            if (cut->wet.current == 0.0 && ! cut->wet.moving()) {
                const bool isLow = cut == &lowCut;
                for (auto& layer : layers) (isLow ? layer.lowCut : layer.highCut) = {};
            }
        }
    }

    static void run (const Coeffs& c, Section& s, double* x, int n) noexcept {
        double z1 = s.z1, z2 = s.z2;
        const double b0 = c.b0, b1 = c.b1, b2 = c.b2, a1 = c.a1, a2 = c.a2;
        for (int i = 0; i < n; ++i) {
            const double in = x[i], y = b0 * in + z1;
            z1 = b1 * in - a1 * y + z2;
            z2 = b2 * in - a2 * y;
            x[i] = y;
        }
        s.z1 = z1; s.z2 = z2;
    }
    void runCut (const Cut& cut, Section& s, double* x, int n, int offset) const noexcept {
        const double to = cut.wet.current, from = cut.wetFrom;
        if (to == 0.0 && from == 0.0) return;
        if (to == 1.0 && from == 1.0) { run (cut.c, s, x, n); return; }
        double dry[tickSamples];
        std::copy (x, x + n, dry);
        run (cut.c, s, x, n);
        for (int i = 0; i < n; ++i) {
            const double w = from + (to - from) * (double) (offset + i + 1) / tickSamples;
            x[i] = dry[i] + w * (x[i] - dry[i]);
        }
    }

    void processChunk (LayerState& st, float* left, float* right, int n, int offset) noexcept {
        float* io[2] { left, right };
        for (int ch = 0; ch < 2; ++ch) {
            double x[tickSamples];
            for (int i = 0; i < n; ++i) x[i] = io[ch][i];
            runCut (lowCut, st.lowCut[(size_t) ch], x, n, offset);
            for (int i = 0; i < numComponents; ++i)
                if (active[(size_t) i]) run (coeffs[(size_t) i], st.bands[(size_t) ch][(size_t) i], x, n);
            runCut (highCut, st.highCut[(size_t) ch], x, n, offset);
            for (int i = 0; i < n; ++i) io[ch][i] = (float) x[i];
        }
    }

    double rate = 44100.0;
    int gainTicks = 1, modeTicks = 1, freqTicks = 1, wetTicks = 1, phase = 0;
    bool needsSnap = true;
    std::array<Ramp, numBands> gain;
    Ramp mode;
    Cut lowCut { Shape::highPass }, highCut { Shape::lowPass };
    std::array<Coeffs, numComponents> coeffs {};
    std::array<double, numComponents> db {};
    std::array<bool, numComponents> active {};
    std::array<LayerState, numLayers> layers {};
};

} // namespace rs::eq
