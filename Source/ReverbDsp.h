#pragma once
// Reverb for the FX rack: six algorithms on one engine. Header-only, no JUCE.
//
//  Room      image-source early reflections of a shoebox room + 16-line FDN tail
//  Plate     Dattorro figure-8 plate tank (modulated), stereo taps
//  Ambience  dense diffused early cluster + very short FDN tail (adds space, not echo)
//  Terrain   open air: sparse distant reflections with air absorption + scattered wash
//  Bloom     two FDNs in series: the tail swells for "Attack" seconds, then decays
//  Shimmer   long FDN hall with an octave-up pitch shifter inside the feedback loop
//
// Every tail has three-band (low / mid / high) frequency-dependent decay, so Decay is
// the true mid-band RT60 at any Size. Lengths, pre-delay and taps glide per sample,
// so automation never clicks. No allocation or locking in process().
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
 #include <xmmintrin.h>
 #define RS_REVERB_HAS_SSE 1
#endif

namespace rs::reverb {

enum Mode : int { room = 0, plate = 1, ambience = 2, terrain = 3, bloom = 4, shimmer = 5, numModes = 6 };

constexpr float maxPreDelayMs = 250.0f;
constexpr float minLowCutHz = 20.0f, maxLowCutHz = 1000.0f;

struct Settings {
    int mode = room;
    float size01 = 0.5f, decay01 = 0.5f, tone01 = 0.5f, mix = 0.5f;   // mix: 0 dry .. 1 wet (equal power)
    float preDelayMs = 10.0f, lowCutHz = 80.0f, character01 = 0.5f;
};

// Mid-band RT60 range per mode (the Decay knob is exponential between these).
inline void decayRange (int mode, double& lo, double& hi) noexcept {
    static constexpr double r[numModes][2] { { 0.3, 3.0 }, { 0.5, 6.0 }, { 0.2, 1.0 }, { 0.8, 10.0 }, { 2.0, 25.0 }, { 2.0, 25.0 } };
    const int m = std::clamp (mode, 0, numModes - 1);
    lo = r[m][0]; hi = r[m][1];
}
inline double decaySeconds (int mode, double v01) noexcept {
    double lo = 1.0, hi = 1.0; decayRange (mode, lo, hi);
    return lo * std::pow (hi / lo, std::clamp (v01, 0.0, 1.0));
}
// Size knob -> geometric scale of every delay (0.4x .. 1.6x).
inline double sizeScale (double v01) noexcept { return 0.4 * std::pow (4.0, std::clamp (v01, 0.0, 1.0)); }
inline const char* modeName (int mode) noexcept {
    static const char* names[numModes] { "Room", "Plate", "Ambience", "Terrain", "Bloom", "Shimmer" };
    return names[std::clamp (mode, 0, numModes - 1)];
}
inline const char* characterName (int mode) noexcept {
    static const char* names[numModes] { "Early", "Modulation", "Width", "Scatter", "Attack", "Shimmer" };
    return names[std::clamp (mode, 0, numModes - 1)];
}
// Bloom: how long the tail takes to swell to its peak (50 .. 500 ms).
inline double bloomAttackSeconds (double c01) noexcept { return 0.05 * std::pow (10.0, std::clamp (c01, 0.0, 1.0)); }

namespace detail {
constexpr double pi = 3.14159265358979323846;
constexpr double ln1000 = 6.907755278982137;   // RT60 is a 60 dB (1/1000) decay
constexpr double soundSpeed = 343.0;
constexpr int maxTaps = 32;
constexpr int terrainTaps = 20, terrainPrimary = 14, terrainNear = 7;   // 14 surfaces + 6 canyon flutter echoes
constexpr double plateRate = 29761.0;         // Dattorro's reference rate

// Flush denormals while the reverb runs (tails decay into the denormal range).
struct FtzGuard {
#if RS_REVERB_HAS_SSE
    unsigned int saved;
    FtzGuard() noexcept : saved (_mm_getcsr()) { _mm_setcsr (saved | 0x8040u); }
    ~FtzGuard() { _mm_setcsr (saved); }
#endif
};

struct Rng {
    std::uint32_t s;
    explicit Rng (std::uint32_t seed) noexcept : s (seed) {}
    double next() noexcept { s = s * 1664525u + 1013904223u; return (double) (s >> 8) / 16777216.0; }
};

inline float onePole (double hz, double fs) noexcept { return (float) (1.0 - std::exp (-2.0 * pi * std::min (hz, 0.45 * fs) / fs)); }

// Power-of-two circular delay line with 4-point Hermite fractional reads.
// Read before push: at(1) is the previous input sample.
class Delay {
public:
    void allocate (double maxSamples) {
        int size = 16;
        while ((double) size < maxSamples + 8.0) size <<= 1;
        buf.assign ((size_t) size, 0.0f); mask = size - 1; pos = 0; used = false;
    }
    void clear() noexcept {
        if (used) std::fill (buf.begin(), buf.end(), 0.0f);
        used = false; pos = 0;
    }
    void push (float x) noexcept { buf[(size_t) pos] = x; pos = (pos + 1) & mask; used = true; }
    float at (int d) const noexcept { return buf[(size_t) ((pos - d) & mask)]; }
    float read (float d) const noexcept {
        d = std::clamp (d, 2.0f, (float) (mask - 4));
        const int i = (int) d; const float t = d - (float) i;
        const float xm1 = at (i - 1), x0 = at (i), x1 = at (i + 1), x2 = at (i + 2);
        const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + x0;
    }
    int capacity() const noexcept { return mask - 4; }
private:
    std::vector<float> buf { std::vector<float> (16, 0.0f) };
    int mask = 15, pos = 0;
    bool used = false;
};

// Schroeder allpass (g + z^-M) / (1 + g z^-M), fractional M.
inline float allpass (Delay& d, float x, float m, float g) noexcept {
    const float z = d.read (m); const float w = x - g * z; d.push (w); return z + g * w;
}

// Topology-preserving state-variable filter (Simper); coefficients can move per tick.
struct Svf {
    float a1 = 1, a2 = 0, a3 = 0, k = 1.41421356f;
    void set (double hz, double fs, double q) noexcept {
        const double g = std::tan (pi * std::min (hz, 0.45 * fs) / fs), kk = 1.0 / q, a = 1.0 / (1.0 + g * (g + kk));
        a1 = (float) a; a2 = (float) (g * a); a3 = (float) (g * g * a); k = (float) kk;
    }
};
struct SvfState { float ic1 = 0, ic2 = 0; };
inline void svf (const Svf& f, SvfState& s, float x, float& lp, float& hp) noexcept {
    const float v3 = x - s.ic2, v1 = f.a1 * s.ic1 + f.a2 * v3, v2 = s.ic2 + f.a2 * s.ic1 + f.a3 * v3;
    s.ic1 = 2.0f * v1 - s.ic1; s.ic2 = 2.0f * v2 - s.ic2;
    lp = v2; hp = x - f.k * v1 - v2;
}

// Smooth bounded saturator (only used as a safety net inside the shimmer loop).
inline float softClip (float x) noexcept {
    x = std::clamp (x, -3.0f, 3.0f);
    return x * (27.0f + x * x) / (27.0f + 9.0f * x * x);
}

// Sign patterns for FDN input / output vectors. Left and right output rows are
// orthogonal (pattern has as many +1 as -1 in the first 8 and in all 16 entries).
inline constexpr float signIn[16]  { 1, -1, 1, 1, -1, 1, -1, -1, 1, 1, -1, 1, -1, -1, 1, -1 };
inline constexpr float signOut[16] { 1, 1, -1, 1, 1, -1, -1, 1, -1, 1, 1, -1, 1, -1, -1, -1 };
inline constexpr float pattern[16] { 1, 1, -1, -1, 1, -1, 1, -1, -1, 1, -1, 1, 1, -1, -1, 1 };

template <int N> struct FdnParams {
    std::array<float, N> len {}, gl {}, gm {}, gh {};   // length (samples) and low / mid / high loop gains
    float aLo = 0, aHi = 0, depth = 0;                  // band split one-poles, modulation depth (samples)
};

// N-line feedback delay network, Hadamard (lossless) feedback matrix,
// a slow sine LFO per line, three-band absorption per line.
template <int N> class Fdn {
    static_assert (N == 8 || N == 16, "FDN size");
public:
    void allocate (const std::array<double, N>& maxSamples, double fs) {
        for (int i = 0; i < N; ++i) {
            line[(size_t) i].allocate (maxSamples[(size_t) i]);
            const double rate = 0.13 * std::pow (6.0, (double) i / (double) (N - 1)), w = 2.0 * pi * rate / fs;
            rc[(size_t) i] = (float) std::cos (w); rsn[(size_t) i] = (float) std::sin (w);
        }
        clear();
    }
    void clear() noexcept {
        for (auto& d : line) d.clear();
        lo.fill (0.0f); hi.fill (0.0f);
        for (int i = 0; i < N; ++i) { oc[(size_t) i] = (float) std::cos (2.39996 * i); os[(size_t) i] = (float) std::sin (2.39996 * i); }
    }
    void renormalise() noexcept {
        for (int i = 0; i < N; ++i) {
            const float g = 1.5f - 0.5f * (oc[(size_t) i] * oc[(size_t) i] + os[(size_t) i] * os[(size_t) i]);
            oc[(size_t) i] *= g; os[(size_t) i] *= g;
        }
    }
    template <bool Settled = false>
    void process (const FdnParams<N>& a, const FdnParams<N>& b, float t, float inL, float inR, float& outL, float& outR) noexcept {
        float v[N];
        const float depth = b.depth, aLo = b.aLo, aHi = b.aHi;
        for (int i = 0; i < N; ++i) {
            const size_t j = (size_t) i;
            const float c = oc[j], s = os[j];
            oc[j] = c * rc[j] - s * rsn[j]; os[j] = s * rc[j] + c * rsn[j];
            const float x = line[j].read ((Settled ? a.len[j] : a.len[j] + (b.len[j] - a.len[j]) * t) + depth * s);
            lo[j] += aLo * (x - lo[j]); hi[j] += aHi * (x - hi[j]);
            v[i] = b.gl[j] * lo[j] + b.gm[j] * (hi[j] - lo[j]) + b.gh[j] * (x - hi[j]);
        }
        float l = 0.0f, r = 0.0f;
        for (int i = 0; i < N; ++i) { l += signOut[i] * v[i]; r += signOut[i] * pattern[i] * v[i]; }
        const float outScale = N == 16 ? 0.25f : 0.35355339f, inScale = N == 16 ? 0.35355339f : 0.5f;
        outL = l * outScale; outR = r * outScale;
        for (int h = 1; h < N; h <<= 1)                       // fast Walsh-Hadamard transform
            for (int i = 0; i < N; i += 2 * h)
                for (int k = i; k < i + h; ++k) { const float p = v[k], q = v[k + h]; v[k] = p + q; v[k + h] = p - q; }
        for (int i = 0; i < N; ++i)
            line[(size_t) i].push (v[i] * outScale + signIn[i] * inScale * ((i & 1) != 0 ? inR : inL));
    }
private:
    std::array<Delay, N> line;
    std::array<float, N> lo {}, hi {}, oc {}, os {}, rc {}, rsn {};
};

struct PlateParams { float k = 1, g1 = 0.5f, g2 = 0.5f, bl = 1, bh = 1, aLo = 0, aHi = 0, exc = 0, inLp = 1; };

// Dattorro, "Effect Design Part 1" (JAES 1997): input diffusers + figure-8 tank.
// All lengths are in samples at 29761 Hz and are multiplied by k (rate * size).
class Plate {
public:
    void allocate (double maxK) {
        const double extra = 64.0 * maxK + 8.0;
        const double inL[4] { 142, 107, 379, 277 };
        for (int i = 0; i < 4; ++i) in[(size_t) i].allocate (inL[i] * maxK + extra);
        ap1L.allocate (672 * maxK + extra); d1L.allocate (4453 * maxK + extra); ap2L.allocate (1800 * maxK + extra); d2L.allocate (3720 * maxK + extra);
        ap1R.allocate (908 * maxK + extra); d1R.allocate (4217 * maxK + extra); ap2R.allocate (2656 * maxK + extra); d2R.allocate (3163 * maxK + extra);
    }
    void setLfo (double fs) noexcept {
        const double w = 2.0 * pi * 0.93 / fs; rc = (float) std::cos (w); rsn = (float) std::sin (w);
        // 16 first-order allpasses, each delaying DC by 0.22 ms and the top octave by almost nothing:
        // ~3.5 ms of low-frequency lag at any sample rate -> the plate's falling "chirp" on transients.
        const double d = std::max (1.05, 0.00022 * fs);
        disp = (float) (-(d - 1.0) / (d + 1.0));
    }
    void clear() noexcept {
        for (auto& d : in) d.clear();
        ap1L.clear(); d1L.clear(); ap2L.clear(); d2L.clear(); ap1R.clear(); d1R.clear(); ap2R.clear(); d2R.clear();
        bw = loL = hiL = loR = hiR = endL = endR = 0.0f; oc = 1.0f; os = 0.0f; dz.fill (0.0f);
    }
    void renormalise() noexcept { const float g = 1.5f - 0.5f * (oc * oc + os * os); oc *= g; os *= g; }
    template <bool Settled = false>
    void process (const PlateParams& a, const PlateParams& b, float t, float x, float& outL, float& outR) noexcept {
        const float k = Settled ? a.k : a.k + (b.k - a.k) * t;
        bw += b.inLp * (x - bw);
        float v = bw;
        for (auto& z : dz) { const float y = disp * v + z; z = v - disp * y; v = y; }
        v = allpass (in[0], v, 142.0f * k, 0.75f);
        v = allpass (in[1], v, 107.0f * k, 0.75f);
        v = allpass (in[2], v, 379.0f * k, 0.625f);
        v = allpass (in[3], v, 277.0f * k, 0.625f);
        const float c = oc, s = os;
        oc = c * rc - s * rsn; os = s * rc + c * rsn;
        const float lIn = v + endR, rIn = v + endL;
        // left half
        const float l1 = allpass (ap1L, lIn, 672.0f * k + b.exc * s, -0.7f);
        const float dl = d1L.read (4453.0f * k); d1L.push (l1);
        loL += b.aLo * (dl - loL); hiL += b.aHi * (dl - hiL);
        const float l2 = allpass (ap2L, (b.bl * loL + (hiL - loL) + b.bh * (dl - hiL)) * b.g1, 1800.0f * k, 0.5f);
        const float el = d2L.read (3720.0f * k); d2L.push (l2);
        // right half
        const float r1 = allpass (ap1R, rIn, 908.0f * k + b.exc * c, -0.7f);
        const float dr = d1R.read (4217.0f * k); d1R.push (r1);
        loR += b.aLo * (dr - loR); hiR += b.aHi * (dr - hiR);
        const float r2 = allpass (ap2R, (b.bl * loR + (hiR - loR) + b.bh * (dr - hiR)) * b.g1, 2656.0f * k, 0.5f);
        const float er = d2R.read (3163.0f * k); d2R.push (r2);
        endL = el * b.g2; endR = er * b.g2;
        outL = d1R.read (266.0f * k) + d1R.read (2974.0f * k) - ap2R.read (1913.0f * k) + d2R.read (1996.0f * k)
             - d1L.read (1990.0f * k) - ap2L.read (187.0f * k) - d2L.read (1066.0f * k);
        outR = d1L.read (353.0f * k) + d1L.read (3627.0f * k) - ap2L.read (1228.0f * k) + d2L.read (2673.0f * k)
             - d1R.read (2111.0f * k) - ap2R.read (335.0f * k) - d2R.read (121.0f * k);
    }
private:
    std::array<Delay, 4> in;
    Delay ap1L, d1L, ap2L, d2L, ap1R, d1R, ap2R, d2R;
    float bw = 0, loL = 0, hiL = 0, loR = 0, hiR = 0, endL = 0, endR = 0;
    float oc = 1, os = 0, rc = 1, rsn = 0, disp = 0;
    std::array<float, 16> dz {};
};

// Octave-up granular shifter: two read heads, power-complementary |sin| / |cos| windows.
class Shifter {
public:
    void prepare (double fs, double grainSeconds) {
        grain = (float) (grainSeconds * fs); buf.allocate (grain + 8.0);
        step = 1.0f / grain;
        const double w = pi / grain; rc = (float) std::cos (w); rsn = (float) std::sin (w);
        clear();
    }
    void clear() noexcept { buf.clear(); phase = 0.0f; c = 1.0f; s = 0.0f; }
    float process (float x) noexcept {
        float p2 = phase + 0.5f; if (p2 >= 1.0f) p2 -= 1.0f;
        const float y = std::abs (s) * buf.read (2.0f + grain * (1.0f - phase)) + std::abs (c) * buf.read (2.0f + grain * (1.0f - p2));
        buf.push (x);
        phase += step;
        const float nc = c * rc - s * rsn; s = s * rc + c * rsn; c = nc;
        if (phase >= 1.0f) { phase -= 1.0f; c = (float) std::cos (pi * phase); s = (float) std::sin (pi * phase); }   // re-sync once per grain
        return y;
    }
private:
    Delay buf;
    float grain = 1, step = 1, phase = 0, c = 1, s = 0, rc = 1, rsn = 0;
};
} // namespace detail

class Processor {
public:
    static constexpr int tick = 32;   // parameters are recomputed every 32 samples, glides are per sample

    void prepare (double sampleRate) {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        buildTables();
        setShimmerFilters();
        for (auto& z : layers) allocateLayer (z);
        const double t = (double) tick;
        smoothFast = (float) (1.0 - std::exp (-t / (0.05 * fs)));
        smoothSize = (float) (1.0 - std::exp (-t / (0.3 * fs)));
        smoothPre = (float) (1.0 - std::exp (-t / (0.12 * fs)));
        smoothMix = (float) (1.0 - std::exp (-t / (0.03 * fs)));
        duckPerTick = (float) (t / (0.025 * fs));
        reset();
    }
    void reset() noexcept {
        current = target; mode = target.mode;
        for (int l = 0; l < 2; ++l) resetLayer (l);
        computeFrame (current, mode, to); from = to; frameFor = current;
        duckFrom = duckTo = 1.0f; remaining = 0; silent = false; framesEqual = false;
    }
    void resetLayer (int layer) noexcept { clearLayer (layers[(size_t) std::clamp (layer, 0, 1)]); }
    void setTargets (const Settings& s, bool snap) noexcept {
        const auto finite = [] (float v, float fallback) { return std::isfinite (v) ? v : fallback; };   // NaN would never leave the tails
        target = s;
        target.mode = std::clamp (s.mode, 0, numModes - 1);
        target.size01 = std::clamp (finite (s.size01, 0.5f), 0.0f, 1.0f); target.decay01 = std::clamp (finite (s.decay01, 0.5f), 0.0f, 1.0f);
        target.tone01 = std::clamp (finite (s.tone01, 0.5f), 0.0f, 1.0f); target.mix = std::clamp (finite (s.mix, 0.5f), 0.0f, 1.0f);
        target.character01 = std::clamp (finite (s.character01, 0.5f), 0.0f, 1.0f);
        target.preDelayMs = std::clamp (finite (s.preDelayMs, 10.0f), 0.0f, maxPreDelayMs);
        target.lowCutHz = std::clamp (finite (s.lowCutHz, 80.0f), minLowCutHz, maxLowCutHz);
        if (snap) reset();
    }
    int currentMode() const noexcept { return mode; }
    // ch: 4 channel pointers (layer 1 L/R, layer 2 L/R), processed in place.
    void process (float* const* ch, const bool* enabled, int n) noexcept {
        detail::FtzGuard guard;
        int done = 0;
        while (done < n) {
            if (remaining == 0) nextTick();
            const int len = std::min (remaining, n - done);
            const int offset = tick - remaining;   // position inside the tick: output never depends on block size
            for (int l = 0; l < 2; ++l)
                if (enabled[l]) run (layers[(size_t) l], ch[2 * l] + done, ch[2 * l + 1] + done, len, offset);
            remaining -= len; done += len;
        }
    }

private:
    using Taps = std::array<std::array<float, detail::maxTaps>, 2>;
    struct Frame {
        float pre = 2, tailOffset = 0, dry = 1, wet = 0, er = 0, tail = 0, width = 1, inject = 0, shimNorm = 0, attackGain = 0, tailSend = 0;
        detail::Svf hp, shLow;
        int tapCount = 0;
        Taps tap {}, tapGain {}, tapLp {};
        std::array<std::array<float, 8>, 2> erLen {}; std::array<float, 8> erG {};
        std::array<std::array<float, 4>, 2> inLen {}; float inG = 0;
        detail::FdnParams<16> main; detail::FdnParams<8> attack;
        detail::PlateParams plate;
    };
    struct Layer {
        std::array<detail::SvfState, 2> hp {};
        std::array<detail::Delay, 2> in;                       // pre-delay / early-reflection source lines
        Taps tapState {};
        std::array<std::array<detail::Delay, 8>, 2> erAp;    // early diffusion (Ambience, Terrain)
        std::array<std::array<detail::Delay, 4>, 2> inAp;    // tail input diffusion
        detail::Fdn<16> main; detail::Fdn<8> attack;
        detail::Plate plateTank;
        std::array<detail::Shifter, 2> shift;
        std::array<detail::SvfState, 2> shLp {}, shHp {};
        float fbL = 0, fbR = 0;
        bool stale = false;
    };

    static constexpr double erDiff[4] { 0.0013, 0.0021, 0.0034, 0.0052 };            // Ambience
    static constexpr double nearDiff[3] { 0.0031, 0.0053, 0.0089 };                 // Terrain, near echoes
    static constexpr double farDiff[4] { 0.0077, 0.0131, 0.0213, 0.0310 };          // Terrain, far echoes
    static constexpr double roomDiff[4] { 0.0011, 0.0017, 0.0026, 0.0039 };
    static constexpr double hallDiff[4] { 0.0047, 0.0069, 0.0103, 0.0151 };
    static constexpr double rightSpread = 1.127;                                      // right-ear diffusers are longer
    static constexpr double maxScale = 1.6, maxModSeconds = 0.0007;

    void buildTables() {
        static constexpr int perm16[16] { 0, 9, 4, 13, 2, 11, 6, 15, 1, 8, 5, 12, 3, 10, 7, 14 };
        static constexpr int perm8[8] { 0, 5, 2, 7, 4, 1, 6, 3 };
        static constexpr double range[numModes][2] { { 0.0062, 0.027 }, { 0, 0 }, { 0.0031, 0.0129 }, { 0.019, 0.067 }, { 0.031, 0.103 }, { 0.029, 0.097 } };
        for (int m = 0; m < numModes; ++m)
            for (int i = 0; i < 16; ++i) {
                const int j = perm16[i];
                mainBase[(size_t) m][(size_t) i] = range[m][0] <= 0.0 ? 0.0
                    : range[m][0] * std::pow (range[m][1] / range[m][0], j / 15.0) * (1.0 + 0.021 * std::sin (7.3 * j + m));
            }
        for (int i = 0; i < 8; ++i) attackBase[(size_t) i] = 0.011 * std::pow (0.037 / 0.011, perm8[i] / 7.0) * (1.0 + 0.019 * std::sin (5.1 * perm8[i]));
        // Ambience: 32 taps per ear, 2..57 ms, exponentially decaying, random signs.
        detail::Rng rng (0x5eed1234u);
        for (int e = 0; e < 2; ++e)
            for (int k = 0; k < detail::maxTaps; ++k) {
                const double u = ((double) k + 0.5 + 0.6 * (rng.next() - 0.5)) / detail::maxTaps;
                const double t = 0.0018 + 0.055 * std::pow (u, 1.5);
                ambSec[(size_t) e][(size_t) k] = t;
                ambGain[(size_t) e][(size_t) k] = (rng.next() < 0.5 ? -1.0 : 1.0) * (1.0 - std::exp (-t / 0.003)) * std::exp (-t / 0.022) * (0.7 + 0.6 * rng.next());
                ambFc[(size_t) e][(size_t) k] = std::max (2500.0, 12000.0 * std::exp (-t / 0.04));   // wall absorption: 4..8 kHz at Tone 50 %
            }
        // Terrain: 14 distant surfaces (cliffs, buildings, tree lines), seconds at scale 1.
        static constexpr double times[detail::terrainPrimary] { 0.031, 0.052, 0.079, 0.108, 0.147, 0.190, 0.236, 0.287, 0.345, 0.410, 0.485, 0.570, 0.660, 0.760 };
        detail::Rng rt (0x7e2a11u);
        for (int k = 0; k < detail::terrainPrimary; ++k) {
            terSec[(size_t) k] = times[k] * (1.0 + 0.06 * (rt.next() - 0.5));
            terAz[(size_t) k] = (rt.next() * 2.0 - 1.0) * 1.45;
            terRefl[(size_t) k] = 0.45 + 0.45 * rt.next();
        }
        for (int k = detail::terrainPrimary; k < detail::terrainTaps; ++k) {
            const int n = k - detail::terrainPrimary;
            terSec[(size_t) k] = 0.29 + 0.083 * n;
            terAz[(size_t) k] = (n & 1) != 0 ? -1.2 : 1.2;
            terRefl[(size_t) k] = 0.75 * std::pow (0.72, n);
        }
        detail::Rng rb (0xb100Au);
        for (int e = 0; e < 2; ++e)
            for (int k = 0; k < bloomTaps; ++k) {
                bloomJitter[(size_t) e][(size_t) k] = k == 0 ? 0.0 : 0.6 * rb.next();
                bloomSign[(size_t) e][(size_t) k] = rb.next() < 0.5 ? -1.0 : 1.0;
            }
    }

    void allocateLayer (Layer& z) {
        for (auto& d : z.in) d.allocate ((0.76 * 1.03 * maxScale + 0.001 + maxPreDelayMs * 0.001) * fs + 16.0);
        for (auto& ear : z.erAp) for (auto& d : ear) d.allocate (0.031 * maxScale * rightSpread * fs + 8.0);
        for (auto& ear : z.inAp) for (auto& d : ear) d.allocate (0.0151 * std::sqrt (maxScale) * rightSpread * fs + 8.0);
        std::array<double, 16> maxMain {}; std::array<double, 8> maxAttack {};
        for (int i = 0; i < 16; ++i) {
            double m = 0.0;
            for (int k = 0; k < numModes; ++k) m = std::max (m, mainBase[(size_t) k][(size_t) i]);
            maxMain[(size_t) i] = m * maxScale * fs + maxModSeconds * fs + 8.0;
        }
        for (int i = 0; i < 8; ++i) maxAttack[(size_t) i] = attackBase[(size_t) i] * maxScale * fs + maxModSeconds * fs + 8.0;
        z.main.allocate (maxMain, fs); z.attack.allocate (maxAttack, fs);
        z.plateTank.allocate (fs / detail::plateRate * 1.3); z.plateTank.setLfo (fs);
        z.shift[0].prepare (fs, 0.071); z.shift[1].prepare (fs, 0.083);
        clearLayer (z);
    }
    static void clearLayer (Layer& z) noexcept {
        z.hp = {}; for (auto& d : z.in) d.clear();
        z.tapState = {};
        for (auto& ear : z.erAp) for (auto& d : ear) d.clear();
        for (auto& ear : z.inAp) for (auto& d : ear) d.clear();
        z.main.clear(); z.attack.clear(); z.plateTank.clear();
        for (auto& s : z.shift) s.clear();
        z.shLp = {}; z.shHp = {}; z.fbL = z.fbR = 0.0f; z.stale = false;
    }

    template <int N>
    double fdnParams (detail::FdnParams<N>& p, const double* base, double scale, double t60, double tLo, double tHi, double fHi, double depthSec, double fLo = 200.0) const noexcept {
        double sum = 0.0;
        p.depth = (float) (depthSec * fs);
        for (int i = 0; i < N; ++i) {
            const double len = std::max (base[i] * scale * fs, (double) p.depth + 8.0), e = -detail::ln1000 * len / fs;
            p.len[(size_t) i] = (float) len; sum += len;
            p.gl[(size_t) i] = (float) std::exp (e / tLo); p.gm[(size_t) i] = (float) std::exp (e / t60); p.gh[(size_t) i] = (float) std::exp (e / tHi);
        }
        p.aLo = detail::onePole (fLo, fs); p.aHi = detail::onePole (fHi, fs);
        // Output gain that makes the impulse response energy 1: E ~ fs*T60 / (13.8 * N * meanLength).
        return std::sqrt (13.8 * N * (sum / N) / (fs * t60));
    }

    void roomTaps (double scale, double fHi, Frame& f) const noexcept {
        const double Lx = 8.4 * scale, Ly = 6.2 * scale, Lz = 2.6 + 1.6 * scale;
        const double sx = 0.3 * Lx, sy = 0.62 * Ly, sz = std::min (1.5, 0.45 * Lz);
        const double lx = 0.7 * Lx, ly = 0.42 * Ly, lz = std::min (1.65, 0.5 * Lz);
        const double direct = std::sqrt ((sx - lx) * (sx - lx) + (sy - ly) * (sy - ly) + (sz - lz) * (sz - lz));
        const float lp1 = detail::onePole (std::min (16000.0, 2.2 * fHi), fs), lp2 = detail::onePole (std::min (12000.0, 1.5 * fHi), fs);
        double energy[2] { 0.0, 0.0 };
        int k = 0;
        for (int nx = -2; nx <= 2; ++nx)
            for (int ny = -2; ny <= 2; ++ny)
                for (int nz = -2; nz <= 2; ++nz) {
                    const int order = std::abs (nx) + std::abs (ny) + std::abs (nz);
                    if (order < 1 || order > 2 || k >= detail::maxTaps) continue;
                    const double ix = nx * Lx + ((nx & 1) != 0 ? Lx - sx : sx);   // image source position
                    const double iy = ny * Ly + ((ny & 1) != 0 ? Ly - sy : sy);
                    const double iz = nz * Lz + ((nz & 1) != 0 ? Lz - sz : sz);
                    const double dCentre = std::sqrt ((ix - lx) * (ix - lx) + (iy - ly) * (iy - ly) + (iz - lz) * (iz - lz));
                    const double side = (iy - ly) / std::max (1e-6, dCentre);
                    for (int e = 0; e < 2; ++e) {
                        const double ey = ly + (e == 0 ? 0.09 : -0.09);   // ears, listener faces +x
                        const double d = std::sqrt ((ix - lx) * (ix - lx) + (iy - ey) * (iy - ey) + (iz - lz) * (iz - lz));
                        const double shadow = 0.72 + 0.28 * (e == 0 ? side : -side);
                        const double g = direct / d * std::pow (0.8, order) * shadow;
                        f.tap[(size_t) e][(size_t) k] = (float) (std::max (0.0002, (d - direct) / detail::soundSpeed) * fs);
                        f.tapGain[(size_t) e][(size_t) k] = (float) g;
                        f.tapLp[(size_t) e][(size_t) k] = order == 1 ? lp1 : lp2;
                        energy[e] += g * g;
                    }
                    ++k;
                }
        f.tapCount = k;
        for (int e = 0; e < 2; ++e) {
            const float norm = (float) (1.0 / std::sqrt (std::max (1e-9, energy[e])));
            for (int i = 0; i < k; ++i) f.tapGain[(size_t) e][(size_t) i] *= norm;
        }
    }
    void ambienceTaps (double scale, double toneFactor, Frame& f) const noexcept {
        double energy[2] { 0.0, 0.0 };
        for (int e = 0; e < 2; ++e)
            for (int k = 0; k < detail::maxTaps; ++k) {
                const size_t ue = (size_t) e, uk = (size_t) k;
                f.tap[ue][uk] = (float) (ambSec[ue][uk] * scale * fs);
                f.tapGain[ue][uk] = (float) ambGain[ue][uk];
                f.tapLp[ue][uk] = detail::onePole (ambFc[ue][uk] * toneFactor, fs);
                energy[e] += ambGain[ue][uk] * ambGain[ue][uk];
            }
        f.tapCount = detail::maxTaps;
        for (int e = 0; e < 2; ++e)
            for (auto& g : f.tapGain[(size_t) e]) g *= (float) (1.0 / std::sqrt (energy[e]));
    }
    void terrainTaps (double scale, double toneFactor, double t60, Frame& f) const noexcept {
        double energy[2] { 0.0, 0.0 };
        for (int k = 0; k < detail::terrainTaps; ++k) {
            const size_t uk = (size_t) k;
            const double t = terSec[uk] * scale, path = detail::soundSpeed * t, side = std::sin (terAz[uk]);
            // Distance spreading, surface reflectivity, and Decay: a short decay means only
            // the nearest surfaces still answer, a long one lets the far horizon come back.
            const double spread = std::pow (12.0 / (12.0 + path), 0.6) * terRefl[uk] * std::exp (-detail::ln1000 * t / t60);
            // Air absorption: the further the surface, the darker the echo.
            // Air and vegetation: near surfaces ~4 kHz, the far horizon rolls off around 1.5..2 kHz.
            const double fc = std::clamp (4500.0 * std::pow (35.0 / std::max (1.0, path), 0.6), 600.0, 4500.0) * toneFactor;
            for (int e = 0; e < 2; ++e) {
                const double itd = 0.00033 * side * (e == 0 ? -1.0 : 1.0), ild = 1.0 + 0.3 * side * (e == 0 ? 1.0 : -1.0);
                f.tap[(size_t) e][uk] = (float) ((t + itd + 0.001) * fs);
                f.tapGain[(size_t) e][uk] = (float) (spread * ild);
                f.tapLp[(size_t) e][uk] = detail::onePole (fc, fs);
                energy[e] += spread * spread * ild * ild;
            }
        }
        f.tapCount = detail::terrainTaps;
        for (int e = 0; e < 2; ++e)
            for (int k = 0; k < detail::terrainTaps; ++k) f.tapGain[(size_t) e][(size_t) k] *= (float) (1.0 / std::sqrt (energy[e]));
    }
    void diffuserLengths (std::array<std::array<float, 4>, 2>& out, const double* base, double scale) const noexcept {
        for (int i = 0; i < 4; ++i) {
            out[0][(size_t) i] = (float) (base[i] * scale * fs);
            out[1][(size_t) i] = (float) (base[i] * scale * rightSpread * fs);
        }
    }

    void computeFrame (const Settings& s, int m, Frame& f) const noexcept {
        static constexpr double toneOffset[numModes] { 0.0, 0.15, 0.0, -0.1, -0.05, 0.0 };
        static constexpr double bassRatio[numModes] { 1.3, 0.8, 1.0, 0.8, 1.25, 1.0 };   // Room: warm low-mids; Plate/Terrain: no boom
        static constexpr double modDepth[numModes] { 0.00012, 0.0, 0.00008, 0.0002, 0.0006, 0.0004 };
        // Calibrated so that steady noise at default settings comes out at ~0 dB (see ReverbDspTests).
        static constexpr double trim[numModes] { 1.08, 2.32, 1.49, 2.97, 0.91, 0.81 };
        const double scale = sizeScale (s.size01), t60 = decaySeconds (m, s.decay01), c = s.character01;
        const double tone = std::clamp (s.tone01 + toneOffset[m], 0.0, 1.0);
        const double fHi = 1200.0 * std::pow (10.0, 1.05 * tone), hfRatio = 0.1 + 0.8 * tone;
        const double tLo = t60 * bassRatio[m], tHi = t60 * hfRatio, level = std::pow (t60, 0.25) * trim[m];
        const double angle = s.mix * detail::pi * 0.5;
        f.pre = (float) std::max (2.0, s.preDelayMs * 0.001 * fs);
        f.hp.set (s.lowCutHz, fs, 0.7071);
        f.dry = s.mix <= 0.0f ? 1.0f : (float) std::cos (angle);
        f.wet = s.mix <= 0.0f ? 0.0f : (float) std::sin (angle);
        f.width = m == ambience ? (float) (2.0 * c) : 1.0f;
        f.er = f.tail = f.inject = f.shimNorm = f.attackGain = f.tailSend = f.tailOffset = 0.0f;
        f.tapCount = 0;
        switch (m) {
            case room: {
                const double norm = fdnParams<16> (f.main, mainBase[room].data(), scale, t60, tLo, tHi, fHi, modDepth[m], 350.0);
                diffuserLengths (f.inLen, roomDiff, std::sqrt (scale)); f.inG = 0.6f;
                roomTaps (scale, fHi, f);
                f.tailOffset = (float) (0.006 * scale * fs);
                f.er = (float) ((0.3 + 1.4 * c) * trim[m]);
                f.tail = (float) ((1.2 - 0.6 * c) * norm * level);
                break;
            }
            case plate: {
                const double k = fs / detail::plateRate * (0.55 + 0.75 * s.size01);
                const double seg1 = (672.0 + 4453.0) * k, seg2 = (1800.0 + 3720.0) * k, half = seg1 + seg2;
                f.plate.k = (float) k;
                f.plate.g1 = (float) std::exp (-detail::ln1000 * seg1 / (fs * t60));
                f.plate.g2 = (float) std::exp (-detail::ln1000 * seg2 / (fs * t60));
                f.plate.bl = (float) std::exp (-detail::ln1000 * half / fs * (1.0 / tLo - 1.0 / t60));
                f.plate.bh = (float) std::exp (-detail::ln1000 * half / fs * (1.0 / tHi - 1.0 / t60));
                f.plate.aLo = detail::onePole (200.0, fs); f.plate.aHi = detail::onePole (fHi, fs);
                f.plate.exc = (float) ((4.0 + 24.0 * c) * fs / detail::plateRate);
                f.plate.inLp = detail::onePole (std::min (18000.0, 1.6 * fHi), fs);
                f.tail = (float) (std::sqrt (half / (fs * t60)) * level);
                break;
            }
            case ambience: {
                const double norm = fdnParams<16> (f.main, mainBase[ambience].data(), scale, t60, tLo, tHi, fHi, modDepth[m]);
                ambienceTaps (scale, 0.35 + 0.75 * tone, f);
                for (int i = 0; i < 4; ++i) {
                    f.erLen[0][(size_t) i] = (float) (erDiff[i] * std::sqrt (scale) * fs);
                    f.erLen[1][(size_t) i] = (float) (erDiff[i] * std::sqrt (scale) * rightSpread * fs);
                    f.erG[(size_t) i] = 0.72f;   // high diffusion: a smooth field, no flutter
                }
                f.er = (float) trim[m];
                f.tail = (float) (0.7 * norm * level);
                break;
            }
            case terrain: {
                // Open air: the wash loses its top much faster than the mids (roll-off from 2..4 kHz).
                const double fAir = std::min (fHi, 3000.0 + 1000.0 * tone), tAir = t60 * (0.1 + 0.5 * tone);
                const double norm = fdnParams<16> (f.main, mainBase[terrain].data(), scale, t60, tLo, tAir, fAir, modDepth[m]);
                terrainTaps (scale, 0.5 + tone, t60, f);
                for (int i = 0; i < 7; ++i) {
                    const double base = i < 3 ? nearDiff[i] : farDiff[i - 3];
                    f.erLen[0][(size_t) i] = (float) (base * scale * fs);
                    f.erLen[1][(size_t) i] = (float) (base * scale * rightSpread * fs);
                    f.erG[(size_t) i] = (float) ((i < 3 ? 0.7 : 0.75) * c);
                }
                f.tailSend = (float) (0.25 + 0.75 * c);
                // Air absorption on everything the listener hears: 12 dB/oct above ~2.5..5.5 kHz (Tone).
                f.shLow.set (std::min (2500.0 + 3000.0 * tone, 0.45 * fs), fs, 0.6);
                f.er = (float) trim[m];
                f.tail = (float) (0.6 * norm * level);
                break;
            }
            case bloom: {
                // A rising cloud of 16 taps per ear spread over the attack time feeds a short dense
                // network (smear), which feeds the long hall: the tail swells, then decays at RT60.
                const double attack = bloomAttackSeconds (c);
                const double smear = 0.08 + 0.22 * c;   // short attacks stay tight, long ones get a softer cloud
                const double normA = fdnParams<8> (f.attack, attackBase.data(), scale, smear, smear, smear * 0.6, fHi, modDepth[m] * 0.5);
                double energyB[2] { 0.0, 0.0 };
                for (int e = 0; e < 2; ++e)
                    for (int k = 0; k < bloomTaps; ++k) {
                        const size_t ue = (size_t) e, uk = (size_t) k;
                        const double g = bloomSign[ue][uk] * std::pow ((k + 1.0) / bloomTaps, 1.5);
                        f.tap[ue][uk] = (float) (attack * (k + bloomJitter[ue][uk]) / bloomTaps * fs);
                        f.tapGain[ue][uk] = (float) g; f.tapLp[ue][uk] = 1.0f;
                        energyB[e] += g * g;
                    }
                for (int e = 0; e < 2; ++e)
                    for (int k = 0; k < bloomTaps; ++k) f.tapGain[(size_t) e][(size_t) k] *= (float) (1.0 / std::sqrt (energyB[e]));
                f.tapCount = bloomTaps;
                const double norm = fdnParams<16> (f.main, mainBase[bloom].data(), scale, t60, tLo, tHi, fHi, modDepth[m]);
                diffuserLengths (f.inLen, hallDiff, std::sqrt (scale)); f.inG = 0.7f;
                f.attackGain = (float) normA;
                f.tail = (float) (norm * level);
                break;
            }
            default: {   // shimmer
                const double norm = fdnParams<16> (f.main, mainBase[shimmer].data(), scale, t60, tLo, tHi, fHi, modDepth[m]);
                diffuserLengths (f.inLen, hallDiff, std::sqrt (scale)); f.inG = 0.7f;
                f.shimNorm = (float) norm;
                f.shLow.set (std::min (3500.0 * std::pow (8000.0 / 3500.0, tone), 0.21 * fs), fs, 0.7071);   // no aliasing after the octave
                f.inject = (float) (0.92 * std::sqrt (c));   // loop energy per octave generation <= 0.85 (unit-energy tank)
                // The octave generations add energy; keep loudness steady while Shimmer is turned up.
                f.tail = (float) (level * std::sqrt (1.0 - 0.75 * (double) f.inject * f.inject));
                break;
            }
        }
    }

    void nextTick() noexcept {
        from = to;
        duckFrom = duckTo;
        bool switched = false;
        if (target.mode != mode && duckFrom <= 0.0f) {   // wet is silent: switch algorithm and start clean
            mode = target.mode;
            for (auto& z : layers) clearLayer (z);
            computeFrame (current, mode, from);
            switched = true;
        }
        const auto step = [] (float& v, float t, float k) { v += (t - v) * k; if (std::abs (t - v) < 1.0e-5f) v = t; };
        step (current.size01, target.size01, smoothSize);
        step (current.decay01, target.decay01, smoothFast);
        step (current.tone01, target.tone01, smoothFast);
        step (current.character01, target.character01, smoothFast);
        step (current.lowCutHz, target.lowCutHz, smoothFast);
        step (current.preDelayMs, target.preDelayMs, smoothPre);
        step (current.mix, target.mix, smoothMix);
        current.mode = mode;
        bool recomputed = false;
        if (! sameFrame (current, frameFor)) { computeFrame (current, mode, to); frameFor = current; recomputed = true; }
        const float duckTarget = target.mode != mode ? 0.0f : 1.0f;
        duckTo = duckTarget > duckFrom ? std::min (duckTarget, duckFrom + duckPerTick) : std::max (duckTarget, duckFrom - duckPerTick);
        framesEqual = ! recomputed && ! switched && std::memcmp (&duckFrom, &duckTo, sizeof (float)) == 0;
        silent = from.wet == 0.0f && to.wet == 0.0f;
        for (auto& z : layers) { z.main.renormalise(); z.attack.renormalise(); z.plateTank.renormalise(); }
        remaining = tick;
    }

    void run (Layer& z, float* l, float* r, int len, int offset) noexcept {
        if (silent) { z.stale = true; return; }          // Mix 0: dry is bit-exact, skip all work
        if (z.stale) clearLayer (z);                      // do not replay old audio when the wet comes back
        if (framesEqual) runSettled<true> (z, l, r, len, offset); else runSettled<false> (z, l, r, len, offset);
    }
    // Settled: `from` and `to` are the same frame, so every per-sample glide is skipped (lerp (a, a, t) is a).
    template <bool Settled>
    void runSettled (Layer& z, float* l, float* r, int len, int offset) noexcept {
        switch (mode) {
            case room:     runMode<room, Settled> (z, l, r, len, offset); break;
            case plate:    runMode<plate, Settled> (z, l, r, len, offset); break;
            case ambience: runMode<ambience, Settled> (z, l, r, len, offset); break;
            case terrain:  runMode<terrain, Settled> (z, l, r, len, offset); break;
            case bloom:    runMode<bloom, Settled> (z, l, r, len, offset); break;
            default:       runMode<shimmer, Settled> (z, l, r, len, offset); break;
        }
    }

    // A settled reverb recomputes nothing: the frame only changes when a setting does.
    static bool sameFrame (const Settings& a, const Settings& b) noexcept {
        const auto same = [] (float x, float y) { return std::memcmp (&x, &y, sizeof (float)) == 0; };
        return a.mode == b.mode && same (a.size01, b.size01) && same (a.decay01, b.decay01) && same (a.tone01, b.tone01) && same (a.mix, b.mix)
            && same (a.preDelayMs, b.preDelayMs) && same (a.lowCutHz, b.lowCutHz) && same (a.character01, b.character01);
    }

    template <int M, bool Settled>
    void runMode (Layer& z, float* lch, float* rch, int len, int offset) noexcept {
        const auto lerp = [] (float a, float b, float t) noexcept { if constexpr (Settled) { (void) b; (void) t; return a; } else return a + (b - a) * t; };
        const Frame& a = from; const Frame& b = to;
        const float invTick = 1.0f / (float) tick;
        for (int i = 0; i < len; ++i) {
            const float t = (float) (offset + i + 1) * invTick;
            const float xl = lch[i], xr = rch[i];
            float lpTmp = 0.0f, hl = 0.0f, hr = 0.0f;
            detail::svf (b.hp, z.hp[0], xl, lpTmp, hl);
            detail::svf (b.hp, z.hp[1], xr, lpTmp, hr);
            const float pre = lerp (a.pre, b.pre, t);
            float wl = 0.0f, wr = 0.0f;

            if constexpr (M == room || M == ambience || M == terrain) {
                float nl = 0.0f, nr = 0.0f, fl = 0.0f, fr = 0.0f;   // near / far buses (only Terrain splits them)
                for (int k = 0; k < b.tapCount; ++k) {
                    const size_t uk = (size_t) k;
                    const detail::Delay& src = z.in[(size_t) (k & 1)];
                    const float x0 = src.read (pre + lerp (a.tap[0][uk], b.tap[0][uk], t));
                    const float x1 = src.read (pre + lerp (a.tap[1][uk], b.tap[1][uk], t));
                    float& s0 = z.tapState[0][uk]; float& s1 = z.tapState[1][uk];
                    s0 += b.tapLp[0][uk] * (x0 - s0); s1 += b.tapLp[1][uk] * (x1 - s1);
                    if (M == terrain && k >= detail::terrainNear) { fl += b.tapGain[0][uk] * s0; fr += b.tapGain[1][uk] * s1; }
                    else { nl += b.tapGain[0][uk] * s0; nr += b.tapGain[1][uk] * s1; }
                }
                if constexpr (M == room) {
                    const float off = lerp (a.tailOffset, b.tailOffset, t);
                    float tl = z.in[0].read (pre + off), tr = z.in[1].read (pre + off);
                    for (int j = 0; j < 4; ++j) {
                        const size_t uj = (size_t) j;
                        tl = detail::allpass (z.inAp[0][uj], tl, lerp (a.inLen[0][uj], b.inLen[0][uj], t), b.inG);
                        tr = detail::allpass (z.inAp[1][uj], tr, lerp (a.inLen[1][uj], b.inLen[1][uj], t), b.inG);
                    }
                    float ol = 0.0f, orr = 0.0f;
                    z.main.template process<Settled> (a.main, b.main, t, tl, tr, ol, orr);
                    const float er = lerp (a.er, b.er, t), tg = lerp (a.tail, b.tail, t);
                    wl = er * nl + tg * ol; wr = er * nr + tg * orr;
                } else if constexpr (M == ambience) {
                    for (int j = 0; j < 4; ++j) {
                        const size_t uj = (size_t) j;
                        nl = detail::allpass (z.erAp[0][uj], nl, lerp (a.erLen[0][uj], b.erLen[0][uj], t), b.erG[uj]);
                        nr = detail::allpass (z.erAp[1][uj], nr, lerp (a.erLen[1][uj], b.erLen[1][uj], t), b.erG[uj]);
                    }
                    float ol = 0.0f, orr = 0.0f;
                    z.main.template process<Settled> (a.main, b.main, t, nl, nr, ol, orr);
                    const float er = lerp (a.er, b.er, t), tg = lerp (a.tail, b.tail, t);
                    wl = er * nl + tg * ol; wr = er * nr + tg * orr;
                } else {   // terrain: later echoes are scattered more
                    for (int j = 0; j < 7; ++j) {
                        const size_t uj = (size_t) j;
                        float& vl = j < 3 ? nl : fl; float& vr = j < 3 ? nr : fr;
                        vl = detail::allpass (z.erAp[0][uj], vl, lerp (a.erLen[0][uj], b.erLen[0][uj], t), b.erG[uj]);
                        vr = detail::allpass (z.erAp[1][uj], vr, lerp (a.erLen[1][uj], b.erLen[1][uj], t), b.erG[uj]);
                    }
                    float ol = 0.0f, orr = 0.0f;
                    // The wash is fed by every reflection (near ones less), so it carries the decay past the last echo.
                    z.main.template process<Settled> (a.main, b.main, t, (0.5f * nl + fl) * b.tailSend, (0.5f * nr + fr) * b.tailSend, ol, orr);
                    const float er = lerp (a.er, b.er, t), tg = lerp (a.tail, b.tail, t);
                    float lo = 0.0f, hi = 0.0f;
                    detail::svf (b.shLow, z.shLp[0], er * (nl + fl) + tg * ol, wl, hi);
                    detail::svf (b.shLow, z.shLp[1], er * (nr + fr) + tg * orr, wr, lo);
                }
            } else if constexpr (M == plate) {
                const float x = 0.5f * (z.in[0].read (pre) + z.in[1].read (pre));
                float ol = 0.0f, orr = 0.0f;
                z.plateTank.template process<Settled> (a.plate, b.plate, t, x, ol, orr);
                const float tg = lerp (a.tail, b.tail, t);
                wl = tg * ol; wr = tg * orr;
            } else {   // bloom, shimmer
                float tl = 0.0f, tr = 0.0f;
                if constexpr (M == bloom) {
                    for (int k = 0; k < b.tapCount; ++k) {
                        const size_t uk = (size_t) k;
                        const detail::Delay& src = z.in[(size_t) (k & 1)];
                        tl += b.tapGain[0][uk] * src.read (pre + lerp (a.tap[0][uk], b.tap[0][uk], t));
                        tr += b.tapGain[1][uk] * src.read (pre + lerp (a.tap[1][uk], b.tap[1][uk], t));
                    }
                } else {
                    tl = z.in[0].read (pre) + z.fbL; tr = z.in[1].read (pre) + z.fbR;
                }
                for (int j = 0; j < 4; ++j) {
                    const size_t uj = (size_t) j;
                    tl = detail::allpass (z.inAp[0][uj], tl, lerp (a.inLen[0][uj], b.inLen[0][uj], t), b.inG);
                    tr = detail::allpass (z.inAp[1][uj], tr, lerp (a.inLen[1][uj], b.inLen[1][uj], t), b.inG);
                }
                float ol = 0.0f, orr = 0.0f;
                if constexpr (M == bloom) {
                    float al = 0.0f, ar = 0.0f;
                    z.attack.template process<Settled> (a.attack, b.attack, t, tl, tr, al, ar);
                    z.main.template process<Settled> (a.main, b.main, t, al * b.attackGain, ar * b.attackGain, ol, orr);
                    const float tg = lerp (a.tail, b.tail, t);
                    wl = tg * ol; wr = tg * orr;
                } else {
                    z.main.template process<Settled> (a.main, b.main, t, tl, tr, ol, orr);
                    const float nl = ol * b.shimNorm, nr = orr * b.shimNorm;   // unit-energy tank output
                    float lo = 0.0f, hi = 0.0f, bl = 0.0f, br = 0.0f;
                    detail::svf (b.shLow, z.shLp[0], nl, lo, hi); detail::svf (shimHigh, z.shHp[0], lo, lpTmp, bl);
                    detail::svf (b.shLow, z.shLp[1], nr, lo, hi); detail::svf (shimHigh, z.shHp[1], lo, lpTmp, br);
                    z.fbL = detail::softClip (b.inject * z.shift[0].process (bl));
                    z.fbR = detail::softClip (b.inject * z.shift[1].process (br));
                    const float tg = lerp (a.tail, b.tail, t);
                    wl = tg * nl; wr = tg * nr;
                }
            }

            if constexpr (M == ambience) {
                // M/S width with constant power (the ambience tail is decorrelated, so mid and side carry equal energy).
                const float w = b.width, norm = 1.4142136f / std::sqrt (1.0f + w * w);
                const float mid = 0.5f * (wl + wr) * norm, side = 0.5f * (wl - wr) * w * norm;
                wl = mid + side; wr = mid - side;
            }
            z.in[0].push (hl); z.in[1].push (hr);
            const float wet = lerp (a.wet, b.wet, t) * lerp (duckFrom, duckTo, t), dry = lerp (a.dry, b.dry, t);
            lch[i] = dry * xl + wet * wl;
            rch[i] = dry * xr + wet * wr;
        }
    }

public:
    // Shimmer loop high-pass: below 160 Hz the octave generations only add mud (the low-pass is per frame).
    void setShimmerFilters() noexcept { shimHigh.set (160.0, fs, 0.7071); }

private:
    double fs = 48000.0;
    std::array<std::array<double, 16>, numModes> mainBase {};
    std::array<double, 8> attackBase {};
    std::array<std::array<double, detail::maxTaps>, 2> ambSec {}, ambGain {}, ambFc {};
    std::array<double, detail::terrainTaps> terSec {}, terAz {}, terRefl {};
    static constexpr int bloomTaps = 16;
    std::array<std::array<double, bloomTaps>, 2> bloomJitter {}, bloomSign {};
    detail::Svf shimHigh;
    Settings target, current, frameFor;   // frameFor: the settings `to` was last computed from
    int mode = room, remaining = 0;
    bool silent = false, framesEqual = false;   // framesEqual: from == to this tick, no glide to compute
    float smoothFast = 0.1f, smoothSize = 0.01f, smoothPre = 0.02f, smoothMix = 0.1f, duckPerTick = 0.05f, duckFrom = 1.0f, duckTo = 1.0f;
    Frame from, to;
    std::array<Layer, 2> layers;
};

} // namespace rs::reverb
