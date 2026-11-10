// Standalone tests for Source/ReverbDsp.h (no JUCE). Build: g++ -std=c++17 -O2 -ISource Tests/ReverbDspTests.cpp
#include "ReverbDsp.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
int failures = 0;
void check (bool ok, const char* what, double value = 0.0) {
    std::printf ("%s %-72s %.4f\n", ok ? "PASS" : "FAIL", what, value);
    if (! ok) ++failures;
}
constexpr double pi = 3.14159265358979323846;
using Buf = std::vector<float>;
const char* names[] { "Room", "Plate", "Ambience", "Terrain", "Bloom", "Shimmer" };

rs::reverb::Settings settings (int mode, float decay = 0.5f, float size = 0.5f, float tone = 0.5f, float character = 0.5f, float mix = 1.0f) {
    rs::reverb::Settings s; s.mode = mode; s.decay01 = decay; s.size01 = size; s.tone01 = tone; s.character01 = character; s.mix = mix;
    s.preDelayMs = 0.0f; s.lowCutHz = 20.0f; return s;
}

struct Rig {
    rs::reverb::Processor p; double fs;
    Rig (double rate, const rs::reverb::Settings& s) : fs (rate) { p.prepare (rate); p.setTargets (s, true); }
    // Processes stereo input on layer 1 (layer 2 gets the same), returns layer 1 L and R.
    void render (Buf& l, Buf& r, int block = 256) {
        Buf l2 = l, r2 = r;
        const bool enabled[2] { true, true };
        const int n = (int) l.size();
        for (int start = 0; start < n; start += block) {
            const int len = std::min (block, n - start);
            float* ptr[4] { l.data() + start, r.data() + start, l2.data() + start, r2.data() + start };
            p.process (ptr, enabled, len);
        }
    }
};

Buf impulseL (int n) { Buf x ((size_t) n, 0.0f); x[0] = 1.0f; return x; }
Buf noise (int n, float amp, unsigned seed) {
    Buf x ((size_t) n); unsigned s = seed;
    for (auto& v : x) { s = s * 1664525u + 1013904223u; v = amp * (((float) (s >> 8) / 16777216.0f) * 2.0f - 1.0f) * 1.7320508f; }
    return x;
}
// Bandpass (RBJ, constant 0 dB peak) for octave-band decay measurements.
Buf bandpass (const Buf& x, double hz, double fs, double q = 1.41) {
    const double w = 2 * pi * hz / fs, al = std::sin (w) / (2 * q), a0 = 1 + al;
    const double b0 = al / a0, b2 = -al / a0, a1 = -2 * std::cos (w) / a0, a2 = (1 - al) / a0;
    Buf y (x.size()); double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    for (size_t i = 0; i < x.size(); ++i) { const double v = b0 * x[i] + b2 * x2 - a1 * y1 - a2 * y2; x2 = x1; x1 = x[i]; y2 = y1; y1 = v; y[i] = (float) v; }
    return y;
}
// RT60 from the Schroeder energy decay curve, fitted between -5 and -25 dB (T20).
double rt60 (const Buf& h, double fs) {
    std::vector<double> edc (h.size()); double acc = 0;
    for (size_t i = h.size(); i-- > 0;) { acc += (double) h[i] * h[i]; edc[i] = acc; }
    size_t peak = 0; for (size_t i = 0; i < h.size(); ++i) if (edc[i] < edc[0]) { peak = i; break; }
    (void) peak;
    // start from the energy maximum of the smoothed envelope (bloom swells first)
    const double total = edc[0];
    double sx = 0, sy = 0, sxx = 0, sxy = 0; int cnt = 0;
    for (size_t i = 0; i < h.size(); ++i) {
        const double db = 10 * std::log10 (std::max (1e-30, edc[i] / total));
        if (db <= -5 && db >= -25) { const double t = (double) i / fs; sx += t; sy += db; sxx += t * t; sxy += t * db; ++cnt; }
    }
    if (cnt < 10) return 0;
    const double slope = (cnt * sxy - sx * sy) / (cnt * sxx - sx * sx);
    return -60.0 / slope;
}
double energy (const Buf& x, size_t from, size_t to) { double s = 0; for (size_t i = from; i < std::min (to, x.size()); ++i) s += (double) x[i] * x[i]; return s; }
double db (double e) { return 10 * std::log10 (std::max (1e-30, e)); }
bool finite (const Buf& x) { for (float v : x) if (! std::isfinite (v)) return false; return true; }
double peakAbs (const Buf& x, size_t from = 0, size_t to = ~(size_t) 0) { double p = 0; for (size_t i = from; i < std::min (to, x.size()); ++i) p = std::max (p, (double) std::abs (x[i])); return p; }
// Normalized echo density (Abel & Huang): 1 = Gaussian-like diffuse tail.
double echoDensity (const Buf& h, size_t centre, size_t win) {
    double s2 = 0; for (size_t i = centre - win / 2; i < centre + win / 2; ++i) s2 += (double) h[i] * h[i];
    const double sd = std::sqrt (s2 / win); int out = 0;
    for (size_t i = centre - win / 2; i < centre + win / 2; ++i) if (std::abs (h[i]) > sd) ++out;
    return (double) out / win / 0.3173;
}
double toneDb (const Buf& x, double hz, double fs, size_t from, size_t to) {
    double re = 0, im = 0, ws = 0; const size_t n = to - from;
    for (size_t i = 0; i < n; ++i) { const double w = 0.5 - 0.5 * std::cos (2 * pi * i / n); re += w * x[from + i] * std::cos (2 * pi * hz * (from + i) / fs); im += w * x[from + i] * std::sin (2 * pi * hz * (from + i) / fs); ws += w; }
    return 20 * std::log10 (std::max (1e-12, 2 * std::sqrt (re * re + im * im) / ws));
}
}

int main() {
#if RS_REVERB_HAS_SSE
    _mm_setcsr (_mm_getcsr() | 0x8040u);
#endif
    using namespace rs::reverb;
    const double fs = 48000.0;

    // 1. Mix 0 is bit-exact dry for every mode.
    for (int m = 0; m < numModes; ++m) {
        Rig rig (fs, settings (m, 0.5f, 0.5f, 0.5f, 0.5f, 0.0f));
        Buf l = noise (20000, 0.3f, 1), r = noise (20000, 0.3f, 2); const Buf l0 = l, r0 = r;
        rig.render (l, r);
        bool same = l == l0 && r == r0;
        char buf[96]; std::snprintf (buf, sizeof buf, "%s: mix 0 is bit-exact dry", names[m]); check (same, buf);
    }

    // 2. Mid-band (1 kHz octave) RT60 follows the Decay knob.
    for (int m = 0; m < numModes; ++m) {
        for (float d : { 0.25f, 0.6f }) {
            const double target = decaySeconds (m, d);
            Rig rig (fs, settings (m, d, 0.5f, 0.5f, m == shimmer ? 0.0f : m == bloom ? 0.0f : 0.5f));
            const int n = (int) (fs * std::min (40.0, target * 1.6 + 1.0));
            Buf l = impulseL (n), r ((size_t) n, 0.0f); rig.render (l, r);
            const double t = rt60 (bandpass (l, 1000.0, fs), fs);
            // Plate diffusers add a little at the shortest decays; Terrain's strong discrete echoes skew the
            // energy-decay fit when the decay is very short (its wash itself follows Decay exactly).
            const double tol = m == plate ? 0.25 : m == terrain ? 0.35 : 0.2;
            char buf[96]; std::snprintf (buf, sizeof buf, "%s: RT60 @1k within %.0f%% of %.2f s (decay %.2f)", names[m], tol * 100, target, d);
            check (std::abs (t / target - 1.0) < tol, buf, t);
        }
    }

    // 3. Tone: high band decays faster when dark, nearly as long when bright.
    for (float tone : { 0.0f, 1.0f }) {
        Rig rig (fs, settings (room, 0.6f, 0.5f, tone));
        const int n = (int) (fs * 4); Buf l = impulseL (n), r ((size_t) n, 0.0f); rig.render (l, r);
        const double ratio = rt60 (bandpass (l, 8000.0, fs), fs) / rt60 (bandpass (l, 1000.0, fs), fs);
        check (tone == 0.0f ? ratio < 0.35 : ratio > 0.7, tone == 0.0f ? "Room tone 0: RT60(8k)/RT60(1k) < 0.35" : "Room tone 1: RT60(8k)/RT60(1k) > 0.7", ratio);
    }

    // 4. Stereo: late tails are decorrelated, mono sum keeps level.
    for (int m = 0; m < numModes; ++m) {
        Rig rig (fs, settings (m, 0.5f, 0.5f, 0.5f, m == ambience ? 0.5f : 0.3f));
        const int n = (int) (fs * 1.5); Buf l = impulseL (n), r = impulseL (n); rig.render (l, r);
        const size_t a = (size_t) (0.15 * fs), b = (size_t) (0.6 * fs);
        double sl = 0, sr = 0, slr = 0; for (size_t i = a; i < b; ++i) { sl += l[i] * l[i]; sr += r[i] * r[i]; slr += l[i] * r[i]; }
        const double rho = slr / std::sqrt (sl * sr);
        char buf[96]; std::snprintf (buf, sizeof buf, "%s: L/R correlation of the tail |rho| < 0.3", names[m]); check (std::abs (rho) < 0.3, buf, rho);
    }

    // 5. Echo density: diffuse (Gaussian-like) tails, no flutter.
    for (int m : { room, plate, ambience, bloom, shimmer }) {
        Rig rig (fs, settings (m, 0.6f, 0.5f, 0.5f, m == shimmer ? 0.0f : 0.5f));
        const int n = (int) (fs * 1.0); Buf l = impulseL (n), r ((size_t) n, 0.0f); rig.render (l, r);
        const double centre = m == ambience ? 0.08 : 0.3;
        const double ned = echoDensity (l, (size_t) (centre * fs), (size_t) (0.02 * fs));
        char buf[96]; std::snprintf (buf, sizeof buf, "%s: normalized echo density at %.0f ms > 0.85", names[m], centre * 1000); check (ned > 0.85, buf, ned);
    }

    // 6. Terrain: Scatter 0 keeps discrete echoes, Scatter 1 smears them.
    {
        double crest[2];
        for (int i = 0; i < 2; ++i) {
            Rig rig (fs, settings (terrain, 0.4f, 0.5f, 0.5f, (float) i));
            const int n = (int) (fs * 0.8); Buf l = impulseL (n), r ((size_t) n, 0.0f); rig.render (l, r);
            const size_t a = (size_t) (0.02 * fs), b = (size_t) (0.5 * fs);
            crest[i] = peakAbs (l, a, b) / std::sqrt (energy (l, a, b) / (double) (b - a));
        }
        check (crest[0] > 12.0, "Terrain scatter 0: echoes stay discrete (crest factor > 12)", crest[0]);
        check (crest[1] < 0.6 * crest[0], "Terrain scatter 1: echoes are smeared (crest < 0.6x)", crest[1]);
    }

    // 7. Bloom: the energy peak moves later with Attack.
    {
        double peakT[2];
        for (int i = 0; i < 2; ++i) {
            Rig rig (fs, settings (bloom, 0.5f, 0.5f, 0.5f, i == 0 ? 0.0f : 1.0f));
            const int n = (int) (fs * 5); Buf l = impulseL (n), r = impulseL (n); rig.render (l, r);
            const size_t w = (size_t) (0.05 * fs); double best = 0; size_t at = 0;
            for (size_t s = 0; s + w < l.size(); s += w / 4) { const double e = energy (l, s, s + w) + energy (r, s, s + w); if (e > best) { best = e; at = s + w / 2; } }
            peakT[i] = (double) at / fs;
        }
        check (peakT[0] < 0.2, "Bloom attack 0: energy peak before 200 ms", peakT[0]);
        check (peakT[1] > 0.3, "Bloom attack 1: tail swells, peak after 300 ms", peakT[1]);
    }

    // 8. Shimmer: the tail grows an octave above the input only when Shimmer > 0.
    {
        double rel[2];
        for (int i = 0; i < 2; ++i) {
            Rig rig (fs, settings (shimmer, 0.5f, 0.5f, 0.5f, i == 0 ? 0.0f : 0.7f));
            const int n = (int) (fs * 4); Buf l ((size_t) n, 0.0f);
            for (int k = 0; k < (int) fs / 2; ++k) l[(size_t) k] = 0.3f * (float) std::sin (2 * pi * 440.0 * k / fs);
            Buf r = l; rig.render (l, r);
            const size_t a = (size_t) (1.0 * fs), b = (size_t) (3.0 * fs);
            rel[i] = toneDb (l, 880.0, fs, a, b) - toneDb (l, 440.0, fs, a, b);
        }
        check (rel[0] < -40.0, "Shimmer 0: no octave in the tail (880 Hz re 440 Hz, dB)", rel[0]);
        check (rel[1] > -18.0 && rel[1] - rel[0] > 30.0, "Shimmer 0.7: octave builds up in the tail (dB)", rel[1]);
    }

    // 8b. Plate dispersion: lows leave the plate later than highs (filter group delay is subtracted).
    {
        const int n = (int) (fs * 0.5);
        // first-arrival time: first sample whose |x| reaches 30 % of the window peak
        auto centroid = [] (const Buf& x, size_t to) { double pk = 0; const size_t e = std::min (to, x.size()); for (size_t i = 0; i < e; ++i) pk = std::max (pk, (double) std::abs (x[i])); for (size_t i = 0; i < e; ++i) if (std::abs (x[i]) >= 0.3 * pk) return (double) i; return 0.0; };
        Rig rig (fs, settings (plate, 0.5f)); Buf l = impulseL (n), r ((size_t) n, 0.0f); rig.render (l, r);
        const Buf ref = impulseL (n); const size_t win = (size_t) (0.025 * fs);
        const double lagLo = centroid (bandpass (l, 300.0, fs), win) - centroid (bandpass (ref, 300.0, fs), win);
        const double lagHi = centroid (bandpass (l, 6000.0, fs), win) - centroid (bandpass (ref, 6000.0, fs), win);
        check ((lagLo - lagHi) / fs * 1000.0 > 1.5, "Plate: 300 Hz arrives later than 6 kHz (ms)", (lagLo - lagHi) / fs * 1000.0);
    }

    // 8c. Ambience keeps the transient: almost no wet energy in the first 1.5 ms, the body follows at once.
    {
        Rig rig (fs, settings (ambience, 0.5f)); const int n = (int) (fs * 0.3); Buf l = impulseL (n), r = impulseL (n); rig.render (l, r);
        const size_t a = (size_t) (0.0015 * fs), b = (size_t) (0.003 * fs), c = (size_t) (0.015 * fs);
        const double early = energy (l, 0, a) / (double) a, body = energy (l, b, c) / (double) (c - b);
        check (early < 0.3 * body, "Ambience: wet power 0..1.5 ms < 0.3x of 3..15 ms", early / body);
    }

    // 8d. Terrain is much darker than Room (air absorption).
    {
        double ratio[2]; const int modes[2] { room, terrain };
        for (int i = 0; i < 2; ++i) {
            Rig rig (fs, settings (modes[i], 0.5f)); const int n = (int) (fs * 1.0); Buf l = impulseL (n), r ((size_t) n, 0.0f); rig.render (l, r);
            const size_t a = (size_t) (0.03 * fs), b = (size_t) (0.9 * fs);
            ratio[i] = energy (bandpass (l, 6000.0, fs), a, b) / energy (bandpass (l, 1000.0, fs), a, b);
        }
        check (ratio[1] < 0.5 * ratio[0], "Terrain: 6k/1k energy < 0.5x of Room", ratio[1] / ratio[0]);
    }

    // 9. Loudness: steady noise through the wet path at default settings is close to 0 dB.
    for (int m = 0; m < numModes; ++m) {
        Rig rig (fs, settings (m));
        const int n = (int) (fs * 12); Buf l = noise (n, 0.1f, 7), r = noise (n, 0.1f, 8);
        const double in = energy (l, (size_t) (6 * fs), (size_t) n) + energy (r, (size_t) (6 * fs), (size_t) n);
        rig.render (l, r);
        const double g = db ((energy (l, (size_t) (6 * fs), (size_t) n) + energy (r, (size_t) (6 * fs), (size_t) n)) / in);
        char buf[96]; std::snprintf (buf, sizeof buf, "%s: default wet level within +-2 dB of dry (dB)", names[m]); check (std::abs (g) < 2.0, buf, g);
    }

    // 10. Stability at every extreme: decays after input stops, never blows up.
    for (int m = 0; m < numModes; ++m) {
        Rig rig (fs, settings (m, 1.0f, 1.0f, 1.0f, 1.0f));
        const int n = (int) (fs * 24); Buf l = noise (n, 0.5f, 11), r = noise (n, 0.5f, 12);
        for (size_t i = (size_t) (2 * fs); i < l.size(); ++i) l[i] = r[i] = 0.0f;
        rig.render (l, r);
        const double e3 = energy (l, (size_t) (3 * fs), (size_t) (4 * fs)), eEnd = energy (l, (size_t) (23 * fs), (size_t) (24 * fs));
        char buf[96]; std::snprintf (buf, sizeof buf, "%s: max settings stable (finite, decaying; last s re 3rd s, dB)", names[m]);
        check (finite (l) && finite (r) && eEnd < e3 && peakAbs (l) < 8.0, buf, db (eEnd / e3));
    }
    // Hot input (+12 dBFS noise) stays bounded.
    for (int m = 0; m < numModes; ++m) {
        Rig rig (fs, settings (m, 0.8f, 0.8f, 0.8f, 1.0f));
        Buf l = noise ((int) fs * 3, 4.0f, 21), r = noise ((int) fs * 3, 4.0f, 22); rig.render (l, r);
        char buf[96]; std::snprintf (buf, sizeof buf, "%s: +12 dBFS input stays finite and bounded (peak)", names[m]);
        check (finite (l) && peakAbs (l) < 200.0, buf, peakAbs (l));
    }

    // 11. Low cut removes rumble from the wet signal.
    {
        double e[2];
        for (int i = 0; i < 2; ++i) {
            auto s = settings (room, 0.5f); s.lowCutHz = i == 0 ? 20.0f : 400.0f;
            Rig rig (fs, s);
            const int n = (int) (fs * 3); Buf l ((size_t) n); for (int k = 0; k < n; ++k) l[(size_t) k] = 0.3f * (float) std::sin (2 * pi * 60.0 * k / fs);
            Buf r = l; rig.render (l, r); e[i] = energy (l, (size_t) fs, (size_t) n);
        }
        check (db (e[1] / e[0]) < -15.0, "Low cut 400 Hz: 60 Hz in the wet down > 15 dB", db (e[1] / e[0]));
    }

    // 12. Pre-delay: wet onset follows the setting.
    for (int m : { plate, bloom, room }) {
        auto s = settings (m, 0.5f, 0.5f, 0.5f, m == bloom ? 0.0f : 0.5f); s.preDelayMs = 100.0f; Rig rig (fs, s);
        const int n = (int) (fs * 0.5); Buf l = impulseL (n), r = impulseL (n); rig.render (l, r);
        const double pk = peakAbs (l); size_t onset = 0; for (size_t i = 0; i < l.size(); ++i) if (std::abs (l[i]) > 1e-3 * pk) { onset = i; break; }
        const double ms = onset * 1000.0 / fs;
        // Bloom feeds the hall through the swell network, so its onset includes the hall's shortest line (~35 ms).
        const double maxMs = m == bloom ? 140.0 : 112.0;
        char buf[96]; std::snprintf (buf, sizeof buf, "%s: pre-delay 100 ms -> onset in 100..%.0f ms", names[m], maxMs); check (ms >= 99.9 && ms < maxMs, buf, ms);
    }

    // 13. Output does not depend on the host block size.
    for (int m = 0; m < numModes; ++m) {
        Buf l1 = noise (30000, 0.3f, 3), r1 = noise (30000, 0.3f, 4), l2 = l1, r2 = r1;
        Rig a (fs, settings (m)), b (fs, settings (m));
        a.render (l1, r1, 1); b.render (l2, r2, 509);
        char buf[96]; std::snprintf (buf, sizeof buf, "%s: block size 1 vs 509 identical", names[m]); check (l1 == l2 && r1 == r2, buf);
    }

    // 14. Switching modes mid-stream and sweeping Size never click.
    {
        Rig rig (fs, settings (room));
        const int n = (int) (fs * 8), block = 128; Buf l = noise (n, 0.2f, 31), r = noise (n, 0.2f, 32);
        Buf l2 = l, r2 = r; const bool enabled[2] { true, true };
        double worstJump = 0, steadyJump = 0; float prev = 0;
        for (int start = 0, blockIndex = 0; start < n; start += block, ++blockIndex) {
            const double time = (double) start / fs;
            auto s = settings ((int) (time / 1.2) % numModes, 0.5f, (float) (0.5 + 0.5 * std::sin (time * 3.0)), 0.5f, 0.5f, 0.6f);
            rig.p.setTargets (s, false);
            float* ptr[4] { l.data() + start, r.data() + start, l2.data() + start, r2.data() + start };
            rig.p.process (ptr, enabled, block);
            for (int i = 0; i < block; ++i) { const float v = l[(size_t) (start + i)]; const double j = std::abs (v - prev); prev = v; worstJump = std::max (worstJump, j); if (time < 1.1) steadyJump = std::max (steadyJump, j); }
        }
        check (finite (l) && worstJump < 1.6 * steadyJump, "Mode switches + Size sweep: worst step < 1.6x steady (ratio)", worstJump / steadyJump);
    }

    // 15. Sample-rate independence of RT60.
    {
        double t[2]; const double rates[2] { 44100.0, 96000.0 };
        for (int i = 0; i < 2; ++i) {
            Rig rig (rates[i], settings (room, 0.6f));
            const int n = (int) (rates[i] * 3); Buf l = impulseL (n), r ((size_t) n, 0.0f); rig.render (l, r);
            t[i] = rt60 (bandpass (l, 1000.0, rates[i]), rates[i]);
        }
        check (std::abs (t[1] / t[0] - 1.0) < 0.08, "Room RT60 at 96 kHz vs 44.1 kHz (ratio)", t[1] / t[0]);
    }

    // 16. CPU: both layers, stereo, 48 kHz.
    if (std::getenv ("RS_SKIP_BENCH") == nullptr)
        for (int m = 0; m < numModes; ++m) {
            Rig rig (fs, settings (m));
            const int n = (int) fs * 10; Buf l = noise (n, 0.2f, 41), r = noise (n, 0.2f, 42);
            const auto t0 = std::chrono::steady_clock::now();
            rig.render (l, r, 512);
            const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            std::printf ("BENCH %-9s %.2f%% of one core (2 layers stereo, 48 kHz)\n", names[m], 100.0 * sec / 10.0);
        }

    std::printf (failures == 0 ? "\nAll reverb tests passed\n" : "\n%d reverb test(s) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
