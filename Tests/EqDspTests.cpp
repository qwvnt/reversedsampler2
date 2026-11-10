// Standalone tests for Source/EqDsp.h (no JUCE needed):
//   g++ -std=c++17 -O2 -Wall -Wextra -ISource Tests/EqDspTests.cpp -o EqDspTests && ./EqDspTests
#include "../Source/EqDsp.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <vector>

using namespace rs::eq;
static int failures = 0;
static void check (bool ok, const char* what) { std::printf ("%s  %s\n", ok ? "PASS" : "FAIL", what); if (! ok) ++failures; }

static double analogDb (const Settings& s, double f) {
    double m = 1.0;
    for (int i = 0; i < numComponents; ++i) {
        const auto st = componentState (i, amountForBand (s, components[(size_t) i].band), s.vintage ? 1.0 : 0.0);
        if (st.db == 0.0) continue;
        const auto p = detail::prototype (components[(size_t) i].shape, std::pow (10.0, st.db / 40.0), st.q);
        m *= detail::magnitudeSquared (p, f / st.hz);
    }
    if (s.lowCutHz > lowCutOffHz) m *= detail::magnitudeSquared (detail::prototype (Shape::highPass, 1.0, butterworthQ), f / s.lowCutHz);
    if (s.highCutHz < highCutOffHz) m *= detail::magnitudeSquared (detail::prototype (Shape::lowPass, 1.0, butterworthQ), f / s.highCutHz);
    return 10.0 * std::log10 (m);
}
static float digitalDb (const Settings& s, double rate, float f) { float d; response (s, rate, &f, &d, 1); return d; }

// Steady-state gain of the real processor for a sine.
static double measuredDb (const Settings& s, double rate, double f, int blockSize = 512) {
    Processor p; p.prepare (rate); p.setTargets (s, true);
    const int warm = (int) (rate * 1.5), len = (int) rate / 2;
    std::vector<float> l (warm + len), r (warm + len);
    for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = (float) (0.25 * std::sin (2.0 * detail::pi * f * (double) i / rate));
    std::vector<float> d0 (blockSize), d1 (blockSize);
    const bool en[2] { true, false };
    for (int pos = 0; pos < (int) l.size(); pos += blockSize) {
        const int n = std::min (blockSize, (int) l.size() - pos);
        float* ch[4] { l.data() + pos, r.data() + pos, d0.data(), d1.data() };
        p.process (ch, en, n);
    }
    double e = 0.0; for (int i = warm; i < warm + len; ++i) e += (double) l[(size_t) i] * l[(size_t) i];
    return 10.0 * std::log10 (e / len / (0.25 * 0.25 * 0.5));
}

int main() {
    Settings vmax; vmax.lowDb = vmax.midDb = vmax.trebleDb = vmax.airDb = 12.0f;
    const auto only = [] (Band b, float db, bool vintage) {
        Settings s; s.vintage = vintage;
        (b == low ? s.lowDb : b == mid ? s.midDb : b == treble ? s.trebleDb : s.airDb) = db; return s; };

    // 1. Matched sections follow the analog prototypes.
    for (double rate : { 44100.0, 48000.0, 96000.0, 192000.0 }) {
        double worst = 0.0, worstF = 0.0;
        for (int b = 0; b < numBands; ++b)
            for (bool v : { true, false })
                for (float g : { -12.0f, -6.0f, 6.0f, 12.0f }) {
                    const auto s = only ((Band) b, g, v);
                    for (double f = 10.0; f < std::min (20000.0, 0.45 * rate); f *= 1.02) {
                        const double e = std::abs (digitalDb (s, rate, (float) f) - analogDb (s, f));
                        if (e > worst) { worst = e; worstF = f; }
                    }
                }
        std::printf ("   %6.0f Hz: worst digital-vs-analog error %.3f dB at %.0f Hz\n", rate, worst, worstF);
        char msg[128]; std::snprintf (msg, sizeof msg, "bands track analog up to 20 kHz at %.0f Hz", rate);
        check (worst < (rate < 90000.0 ? 0.8 : 0.3), msg);
    }
    // 2. Vintage maxima reproduce the reference screenshots.
    struct Ref { Band b; double f, db; const char* name; };
    const Ref refs[] { { low, 15, 6.23, "LOW 15 Hz" }, { low, 50, 3.62, "LOW 50 Hz" }, { low, 300, -0.65, "LOW dip 300 Hz" },
                       { mid, 1000, 6.23, "MID 1 kHz" }, { mid, 500, 3.62, "MID 500 Hz" },
                       { treble, 3500, 5.69, "TREBLE 3.5 kHz" }, { treble, 800, -1.46, "TREBLE dip 800 Hz" }, { treble, 15000, -0.73, "TREBLE 15 kHz" },
                       { air, 3500, -1.79, "AIR dip 3.5 kHz" }, { air, 10000, 2.79, "AIR 10 kHz" }, { air, 15000, 5.99, "AIR 15 kHz" } };
    for (const auto& r : refs) {
        const double got = digitalDb (only (r.b, 12.0f, true), 96000.0, (float) r.f);
        const double got48 = digitalDb (only (r.b, 12.0f, true), 48000.0, (float) r.f);
        char msg[160]; std::snprintf (msg, sizeof msg, "vintage %s = %.2f dB @96k, %.2f dB @48k (ref %.2f)", r.name, got, got48, r.db);
        check (std::abs (got - r.db) < 0.35 && std::abs (got48 - r.db) < 0.8, msg);
    }
    // 3. Modern is a plain, exact EQ; negative vintage mirrors the boost.
    check (std::abs (digitalDb (only (mid, 12.0f, false), 48000.0, 1000.0f) - 12.0) < 0.05, "modern MID +12 dB at 1 kHz");
    check (std::abs (digitalDb (only (treble, -7.0f, false), 48000.0, 3500.0f) + 7.0) < 0.05, "modern TREBLE -7 dB at 3.5 kHz");
    check (std::abs (digitalDb (only (low, 12.0f, false), 48000.0, 20.0f) - 12.0) < 0.3, "modern LOW shelf +12 dB");
    check (std::abs (digitalDb (only (air, 12.0f, false), 44100.0, 20000.0f) - analogDb (only (air, 12.0f, false), 20000.0)) < 0.5, "modern AIR keeps its shape at 20 kHz / 44.1 kHz");
    {
        double worst = 0.0;
        for (double f = 10; f < 20000; f *= 1.05) worst = std::max (worst, (double) std::abs (digitalDb (vmax, 96000, (float) f) + digitalDb ([&] { Settings s = vmax; s.lowDb = s.midDb = s.trebleDb = s.airDb = -12.0f; return s; }(), 96000, (float) f)));
        check (worst < 0.3, "vintage cut mirrors the boost");
    }
    // 4. Clean cuts, identical in both modes.
    Settings cut; cut.lowCutHz = 200.0f; cut.highCutHz = 5000.0f;
    Settings cutM = cut; cutM.vintage = false;
    check (std::abs (digitalDb (cut, 48000, 200) + 3.01) < 0.05 && std::abs (digitalDb (cut, 48000, 5000) + 3.01) < 0.1, "cuts are Butterworth (-3 dB at cutoff)");
    check (std::abs (digitalDb (cut, 48000, 50) + 24.1) < 0.3, "low cut is 12 dB/oct");
    check (digitalDb (cut, 48000, 1000) == digitalDb (cutM, 48000, 1000), "cuts do not depend on mode");
    // 5. The real-time path equals the drawn curve.
    for (double f : { 15.0, 100.0, 1000.0, 3500.0, 12000.0, 18000.0 }) {
        const double got = measuredDb (vmax, 48000, f), want = digitalDb (vmax, 48000, (float) f);
        char msg[128]; std::snprintf (msg, sizeof msg, "processor at %.0f Hz: %.2f dB (curve %.2f)", f, got, want);
        check (std::abs (got - want) < 0.05, msg);
    }
    check (std::abs (measuredDb (cut, 44100, 60) - digitalDb (cut, 44100, 60)) < 0.1, "processor applies the low cut");
    // 6. Neutral is a bit-exact bypass; host block size does not change the output.
    {
        Processor p; p.prepare (48000); p.setTargets (Settings {}, true);
        std::vector<float> a (4096), b (4096), c (4096), d (4096);
        for (size_t i = 0; i < a.size(); ++i) a[i] = b[i] = (float) std::sin (0.01 * (double) i * i);
        const auto orig = a; float* ch[4] { a.data(), b.data(), c.data(), d.data() }; const bool en[2] { true, true };
        p.process (ch, en, 4096);
        check (a == orig && p.isTransparent(), "neutral settings are a bit-exact bypass");
    }
    {
        std::vector<std::vector<float>> outs;
        for (int bs : { 1, 7, 64, 333, 4096 }) {
            Processor p; p.prepare (48000); p.setTargets (Settings {}, true);
            std::vector<float> l (48000), r (48000), z0 (4096), z1 (4096);
            for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = (float) ((std::rand() % 2001) - 1000) / 1000.0f;
            std::srand (1);
            for (size_t i = 0; i < l.size(); ++i) l[i] = r[i] = (float) ((std::rand() % 2001) - 1000) / 1000.0f;
            const bool en[2] { true, false };
            for (int pos = 0; pos < 48000;) {
                int n = std::min (bs, 48000 - pos);
                if (pos < 4800 && pos + n > 4800) n = 4800 - pos;   // change lands on the same sample for every block size
                if (pos == 4800) { Settings s = vmax; s.vintage = false; s.lowCutHz = 80; p.setTargets (s); }
                float* ch[4] { l.data() + pos, r.data() + pos, z0.data(), z1.data() };
                p.process (ch, en, n);
                pos += n;
            }
            outs.push_back (l);
        }
        double diff = 0.0;
        for (auto& o : outs) for (size_t i = 0; i < o.size(); ++i) diff = std::max (diff, (double) std::abs (o[i] - outs[0][i]));
        // Parameter changes are seen at block starts, so allow a tiny difference around the change.
        check (diff == 0.0, "output is bit-identical for any host block size");
    }
    // 7. Wild automation (mode flips, cuts on/off, full sweeps) stays finite and click-free.
    {
        Processor p; p.prepare (44100); p.setTargets (Settings {}, true);
        std::vector<float> l (64), r (64), z (64);
        double phase = 0.0, maxStep = 0.0, prev = 0.0; bool finite = true;
        for (int block = 0; block < 4000; ++block) {
            Settings s; const double t = block / 4000.0;
            s.lowDb = (float) (12 * std::sin (t * 37)); s.midDb = (float) (12 * std::sin (t * 23)); s.trebleDb = (float) (12 * std::sin (t * 29)); s.airDb = (float) (12 * std::sin (t * 41));
            s.vintage = (block / 300) % 2 == 0;
            s.lowCutHz = (block / 450) % 2 ? 20.0f : (float) (20 * std::pow (50.0, 0.5 + 0.5 * std::sin (t * 13)));
            s.highCutHz = (block / 520) % 2 ? 20000.0f : (float) (1000 * std::pow (20.0, 0.5 + 0.5 * std::sin (t * 17)));
            p.setTargets (s);
            for (int i = 0; i < 64; ++i) { l[(size_t) i] = r[(size_t) i] = (float) (0.3 * std::sin (phase)); phase += 2 * detail::pi * 110 / 44100; }
            float* ch[4] { l.data(), r.data(), z.data(), z.data() }; const bool en[2] { true, false };
            p.process (ch, en, 64);
            for (float v : l) { finite = finite && std::isfinite (v); maxStep = std::max (maxStep, std::abs (v - prev)); prev = v; }
        }
        std::printf ("   max sample step under automation: %.4f (110 Hz sine step %.4f)\n", maxStep, 0.3 * 2 * detail::pi * 110 / 44100);
        check (finite, "automation keeps output finite");
        check (maxStep < 0.06, "automation produces no clicks");
    }
    // 8. Cost.
    if (! std::getenv ("RS_SKIP_BENCH")) {
        Processor p; p.prepare (48000); Settings s = vmax; s.lowCutHz = 40; s.highCutHz = 16000; p.setTargets (s, true);
        std::vector<float> buf (4 * 512, 0.1f); float* ch[4] { buf.data(), buf.data() + 512, buf.data() + 1024, buf.data() + 1536 }; const bool en[2] { true, true };
        const int blocks = 20000;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < blocks; ++i) { std::fill (buf.begin(), buf.end(), 0.1f * (float) (i % 7 - 3)); p.process (ch, en, 512); }
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        const double audio = blocks * 512.0 / 48000.0;
        std::printf ("   all bands + cuts, 2 stereo layers: %.3f%% of one core (%.0fx real time)\n", 100 * sec / audio, audio / sec);
        const auto t1 = std::chrono::steady_clock::now();
        int moves = 0;
        for (int i = 0; i < blocks; ++i) { s.midDb = (float) (12 * std::sin (i * 0.01)); s.vintage = (i / 50) % 2; p.setTargets (s); std::fill (buf.begin(), buf.end(), 0.1f); p.process (ch, en, 512); ++moves; }
        const double sec2 = std::chrono::duration<double> (std::chrono::steady_clock::now() - t1).count();
        std::printf ("   same while automating every block:  %.3f%% of one core\n", 100 * sec2 / audio);
        check (std::isfinite (buf[0]) && moves == blocks, "benchmark ran");
    }
    std::printf (failures ? "\n%d FAILURE(S)\n" : "\nALL EQ TESTS PASSED\n", failures);
    return failures ? 1 : 0;
}
