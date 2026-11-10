// JUCE-free checks of the chorus models.
// g++ -std=c++17 -O2 -ISource Tests/ChorusDspTests.cpp -o ChorusDspTests && ./ChorusDspTests
#include "ChorusDsp.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <vector>

static int failures = 0;
static void check (bool ok, const char* what) { std::printf ("%s %s\n", ok ? "PASS" : "FAIL", what); if (! ok) ++failures; }

using namespace rs::chorus;
constexpr double fs = 48000.0;
constexpr double pi = 3.14159265358979323846;

struct Render { std::vector<float> l, r, inL, inR; };

static Render render (rs::chorus::Processor& p, double seconds, int block, double hz = 440.0, float amp = 0.5f, bool chord = false) {
    Render out;
    const int total = (int) (seconds * fs);
    std::vector<float> buf[4];
    for (auto& b : buf) b.assign ((size_t) block, 0.0f);
    float* ch[4] { buf[0].data(), buf[1].data(), buf[2].data(), buf[3].data() };
    bool en[2] { true, false };
    for (int done = 0; done < total; done += block) {
        const int n = std::min (block, total - done);
        for (int k = 0; k < n; ++k) {
            const double t = (double) (done + k) / fs;
            buf[0][(size_t) k] = chord ? amp / 3.0f * (float) (std::sin (2.0 * pi * hz * t) + std::sin (2.0 * pi * hz * 1.26 * t) + std::sin (2.0 * pi * hz * 1.5 * t)) : amp * (float) std::sin (2.0 * pi * hz * t);
            buf[1][(size_t) k] = buf[0][(size_t) k];
            out.inL.push_back (buf[0][(size_t) k]); out.inR.push_back (buf[1][(size_t) k]);
        }
        p.process (ch, en, n);
        for (int k = 0; k < n; ++k) { out.l.push_back (buf[0][(size_t) k]); out.r.push_back (buf[1][(size_t) k]); }
    }
    return out;
}

static double rms (const std::vector<float>& x, size_t from) {
    double s = 0.0; for (size_t i = from; i < x.size(); ++i) s += (double) x[i] * x[i];
    return x.size() > from ? std::sqrt (s / (double) (x.size() - from)) : 0.0;
}
static double rmsDiff (const std::vector<float>& a, const std::vector<float>& b, size_t from) {
    double s = 0.0; for (size_t i = from; i < a.size(); ++i) { const double d = (double) a[i] - b[i]; s += d * d; }
    return std::sqrt (s / (double) (a.size() - from));
}
static bool finite (const Render& r) {
    for (size_t i = 0; i < r.l.size(); ++i) if (! std::isfinite (r.l[i]) || ! std::isfinite (r.r[i]) || std::abs (r.l[i]) > 4.0f || std::abs (r.r[i]) > 4.0f) return false;
    return true;
}
static double maxStep (const std::vector<float>& x, size_t from) {
    double m = 0.0; for (size_t i = from + 2; i < x.size(); ++i) m = std::max (m, std::abs ((double) x[i] - 2.0 * x[i - 1] + x[i - 2]));
    return m;
}

static Settings base (int model, int mode) {
    Settings s; s.model = model; s.mode = mode; s.mix01 = 0.5f;
    if (model == digital || model == sine) s.delayMs = 15.0f;
    return s;
}

static double timeToSleep (rs::chorus::Processor& p) {
    std::vector<float> buf[4]; for (auto& b : buf) b.assign (512, 0.0f);
    float* ch[4] { buf[0].data(), buf[1].data(), buf[2].data(), buf[3].data() };
    bool en[2] { true, false };
    for (int b = 0; b < 40; ++b) {
        for (int k = 0; k < 512; ++k) buf[0][(size_t) k] = buf[1][(size_t) k] = 0.4f * (float) std::sin (0.07 * (b * 512 + k));
        p.process (ch, en, 512);
    }
    int silent = 0; const int hold = (int) (holdSeconds * fs);
    for (int b = 0; b < (int) (20.0 * fs / 512); ++b) {
        for (auto& v : buf) std::fill (v.begin(), v.end(), 0.0f);
        p.process (ch, en, 512);
        float m = 0.0f; for (int k = 0; k < 512; ++k) m = std::max ({ m, std::abs (buf[0][(size_t) k]), std::abs (buf[1][(size_t) k]) });
        if (! std::isfinite (m)) return -2.0;
        silent = m < 1.0e-6f ? silent + 512 : 0;
        if (silent >= hold) return (b + 1) * 512.0 / fs;
    }
    return -1.0;
}

int main() {
    char msg[200];
    const size_t settle = (size_t) (0.3 * fs);
    for (int model = 0; model < numModels; ++model)
        for (int mode = 0; mode < modeCounts[model]; ++mode) {
            Settings s = base (model, mode);
            s.voices = 6;
            rs::chorus::Processor p; p.prepare (fs); p.setTargets (s, true);
            const auto a = render (p, 2.0, 512, 440.0, 0.5f, true);
            std::snprintf (msg, sizeof msg, "%s mode %d: finite and bounded", modelNames[model], mode); check (finite (a), msg);
            const double level = rms (a.l, settle) / rms (a.inL, settle);
            const double effect = std::max (rmsDiff (a.l, a.inL, settle), rmsDiff (a.r, a.inR, settle)) / rms (a.inL, settle);
            std::snprintf (msg, sizeof msg, "%s mode %d: level %.2f of dry, differs from dry by %.2f", modelNames[model], mode, level, effect);
            check (level > 0.45 && level < 1.6 && effect > 0.05, msg);
            rs::chorus::Processor q; q.prepare (fs); q.setTargets (s, true);
            const auto b = render (q, 2.0, 37, 440.0, 0.5f, true);
            double diff = 0.0; for (size_t i = 0; i < a.l.size(); ++i) diff = std::max (diff, (double) std::abs (a.l[i] - b.l[i]));
            std::snprintf (msg, sizeof msg, "%s mode %d: block size independent (max diff %.2e)", modelNames[model], mode, diff); check (diff < 1.0e-5, msg);
            std::snprintf (msg, sizeof msg, "%s mode %d: no clicks (max 2nd difference %.4f)", modelNames[model], mode, maxStep (a.l, settle)); check (maxStep (a.l, settle) < 0.08, msg);
            rs::chorus::Processor g; g.prepare (fs); g.setTargets (s, true);
            const double t = timeToSleep (g);
            std::snprintf (msg, sizeof msg, "%s mode %d: silent and asleep %.2f s after the input stops", modelNames[model], mode, t); check (t > 0.0 && t < 1.0, msg);
            rs::chorus::Processor fresh; fresh.prepare (fs); fresh.setTargets (s, true);
            g.resetLayer (0); g.reset();
            const auto c1 = render (g, 0.3, 256), c2 = render (fresh, 0.3, 256);
            double rd = 0.0; for (size_t i = 0; i < c1.l.size(); ++i) rd = std::max (rd, (double) std::abs (c1.l[i] - c2.l[i]));
            std::snprintf (msg, sizeof msg, "%s mode %d: reset matches a fresh processor", modelNames[model], mode); check (rd < 1.0e-6, msg);
        }
    {
        Settings s = base (classic, classicChorus); s.dualStereo = false; s.width01 = 1.0f;
        rs::chorus::Processor p; p.prepare (fs); p.setTargets (s, true);
        const auto a = render (p, 1.0, 512, 440.0, 0.05f);
        const double leftVsDry = rmsDiff (a.l, a.inL, settle) / rms (a.inL, settle);
        check (leftVsDry < 0.05, "Classic, Classic output: left output is the (pre-amped) dry signal");
        check (rmsDiff (a.r, a.inR, settle) / rms (a.inR, settle) > 0.1, "Classic, Classic output: right output carries the chorus");
    }
    {
        Settings s = base (classic, classicChorus); s.mix01 = 0.0f; s.variant = true;
        rs::chorus::Processor p; p.prepare (fs); p.setTargets (s, true);
        const auto hot = render (p, 0.5, 512, 220.0, 0.9f);
        check (rmsDiff (hot.l, hot.inL, settle / 2) / rms (hot.inL, settle / 2) > 0.02, "Classic High: the preamp colours the signal even with the effect at 0 %");
    }
    {
        const auto pitchWobble = [&] (int model, int mode) {
            Settings s = base (model, mode); s.mix01 = 1.0f;
            rs::chorus::Processor p; p.prepare (fs); p.setTargets (s, true);
            const auto a = render (p, 4.0, 512, 1000.0, 0.3f);
            const std::vector<float>& mono = a.l;
            std::vector<double> crossings;
            for (size_t i = settle + 1; i < mono.size(); ++i) if (mono[i - 1] < 0.0f && mono[i] >= 0.0f) crossings.push_back ((double) i - mono[i] / (mono[i] - mono[i - 1]));
            double lo = 1e9, hi = 0.0;
            for (size_t i = 10; i < crossings.size(); i += 10) { const double f = 10.0 * fs / (crossings[i] - crossings[i - 10]); lo = std::min (lo, f); hi = std::max (hi, f); }
            return 1200.0 * std::log2 (hi / lo);
        };
        const double steady = pitchWobble (spatial, spatial1), deep = pitchWobble (liquid, liquidDeep);
        std::snprintf (msg, sizeof msg, "Spatial pitch drift %.1f cents, far below Liquid on Deep (%.1f cents)", steady, deep);
        check (steady < 0.5 * deep, msg);
    }
    {
        Settings s = base (digital, pitchShift); s.mix01 = 1.0f; s.depth01 = 0.5f;
        rs::chorus::Processor p; p.prepare (fs); p.setTargets (s, true);
        const auto a = render (p, 3.0, 512, 1000.0, 0.3f);
        const auto measure = [&] (const std::vector<float>& x) {
            double first = -1.0, last = 0.0; int count = 0;
            for (size_t i = (size_t) fs + 1; i < x.size(); ++i) if (x[i - 1] < 0.0f && x[i] >= 0.0f) { const double c = (double) i - x[i] / (x[i] - x[i - 1]); if (first < 0) first = c; last = c; ++count; }
            return 1200.0 * std::log2 ((count - 1) * fs / (last - first) / 1000.0);
        };
        const double left = measure (a.l), right = measure (a.r);
        std::snprintf (msg, sizeof msg, "Digital Pitch: left %.1f cents, right %+.1f cents (-9 / +11 expected)", left, right);
        check (std::abs (left + 9.0) < 1.5 && std::abs (right - 11.0) < 1.5, msg);
    }
    for (int model = 0; model < numModels; ++model) {
        Settings s = base (model, 0); s.mix01 = 1.0f;
        rs::chorus::Processor p; p.prepare (fs); p.setTargets (s, true);
        const auto quiet = render (p, 1.0, 480, 440.0, 0.0f);
        bool silent = true; for (size_t i = 0; i < quiet.l.size(); ++i) silent = silent && quiet.l[i] == 0.0f && quiet.r[i] == 0.0f;
        std::snprintf (msg, sizeof msg, "%s: no hiss, silence in gives exact silence out", modelNames[model]);
        check (silent, msg);
    }
    {
        Settings s = base (ensemble, ensembleSix);
        rs::chorus::Processor p; p.prepare (fs); p.setTargets (s, true);
        const auto start = std::chrono::steady_clock::now();
        std::vector<float> buf[4]; for (auto& b : buf) b.assign (512, 0.1f);
        float* ch[4] { buf[0].data(), buf[1].data(), buf[2].data(), buf[3].data() };
        bool en[2] { true, true };
        for (int b = 0; b < (int) (4.0 * fs / 512); ++b) p.process (ch, en, 512);
        const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        std::printf ("BENCH Ensemble 6 lines, 2 layers @48k: %.2f %% of one core\n", seconds / 4.0 * 100.0);
        Settings c = base (classic, classicChorus); c.rate01 = 1.0f; c.dualStereo = true;
        rs::chorus::Processor q; q.prepare (fs); q.setTargets (c, true);
        const auto start2 = std::chrono::steady_clock::now();
        for (int b = 0; b < (int) (4.0 * fs / 512); ++b) q.process (ch, en, 512);
        const double seconds2 = std::chrono::duration<double> (std::chrono::steady_clock::now() - start2).count();
        std::printf ("BENCH Classic Dual (clock up to 197 kHz), 2 layers @48k: %.2f %% of one core\n", seconds2 / 4.0 * 100.0);
        check (seconds < 0.4 && seconds2 < 0.4, "chorus costs under 10 % of one core");
    }
    std::printf (failures == 0 ? "ALL CHORUS TESTS PASSED\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
