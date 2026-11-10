// JUCE-free checks of the compressor.
// g++ -std=c++17 -O2 -ISource Tests/CompressorDspTests.cpp -o CompressorDspTests && ./CompressorDspTests
#include "CompressorDsp.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

using namespace rs::comp;
static int failures = 0;
static void check (bool ok, const char* what) { std::printf ("%s %s\n", ok ? "PASS" : "FAIL", what); if (! ok) ++failures; }
constexpr double fs = 48000.0, pi = 3.14159265358979323846;

struct Run { std::vector<float> in, out; std::vector<float> reduction; };

static Run render (Processor& p, double seconds, int block, const std::function<float (double)>& signal) {
    Run r;
    const int total = (int) (seconds * fs);
    std::vector<float> buf[4]; for (auto& b : buf) b.assign ((size_t) block, 0.0f);
    float* ch[4] { buf[0].data(), buf[1].data(), buf[2].data(), buf[3].data() };
    bool en[2] { true, false };
    for (int done = 0; done < total; done += block) {
        const int n = std::min (block, total - done);
        for (int k = 0; k < n; ++k) { buf[0][(size_t) k] = buf[1][(size_t) k] = signal ((double) (done + k) / fs); r.in.push_back (buf[0][(size_t) k]); }
        p.process (ch, en, n);
        r.reduction.push_back (p.meters().take().reductionDb);
        for (int k = 0; k < n; ++k) r.out.push_back (buf[0][(size_t) k]);
    }
    return r;
}
static double rmsDb (const std::vector<float>& x, double t0, double t1) {
    double s = 0.0; const size_t a = (size_t) (t0 * fs), b = std::min (x.size(), (size_t) (t1 * fs));
    for (size_t i = a; i < b; ++i) s += (double) x[i] * x[i];
    return 10.0 * std::log10 (s / (double) (b - a) + 1e-30);
}
static double harmonic (const std::vector<float>& x, double t0, double t1, double hz) {
    double re = 0.0, im = 0.0; const size_t a = (size_t) (t0 * fs), b = (size_t) (t1 * fs);
    for (size_t i = a; i < b; ++i) { const double w = 0.5 - 0.5 * std::cos (2.0 * pi * (double) (i - a) / (double) (b - a)); re += w * x[i] * std::cos (2.0 * pi * hz * i / fs); im += w * x[i] * std::sin (2.0 * pi * hz * i / fs); }
    return std::sqrt (re * re + im * im);
}
static std::function<float (double)> sine (double hz, double db) { const double a = std::pow (10.0, db / 20.0); return [=] (double t) { return (float) (a * std::sin (2.0 * pi * hz * t)); }; }

int main() {
    char msg[220];
    {
        bool smooth = true; float worst = 0.0f;
        for (float knee : { 0.0f, 6.0f, 18.0f })
            for (float l = -70.0f; l < 10.0f; l += 0.01f) {
                const float a = gainReductionDb (l, -20.0f, 4.0f, knee), b = gainReductionDb (l + 0.01f, -20.0f, 4.0f, knee);
                worst = std::max (worst, std::abs (b - a));
                if (b < a - 1e-6f) smooth = false;
            }
        check (smooth && worst < 0.0081f, "the gain computer is continuous and never decreases with level, at every knee");
        check (std::abs (gainReductionDb (-8.0f, -20.0f, 4.0f, 0.0f) - 9.0f) < 1e-4f && gainReductionDb (-21.0f, -20.0f, 4.0f, 0.0f) == 0.0f, "hard knee: 12 dB over at 4:1 = 9 dB of reduction");
        std::snprintf (msg, sizeof msg, "6 dB knee: %.4f dB of reduction at the threshold (exact 0.5625)", gainReductionDb (-20.0f, -20.0f, 4.0f, 6.0f));
        check (std::abs (gainReductionDb (-20.0f, -20.0f, 4.0f, 6.0f) - 0.5625f) < 1e-4f, msg);
    }
    {
        Settings s; s.mode = modern; s.thresholdDb = -20.0f; s.ratio = 4.0f; s.kneeDb = 0.0f; s.attackMs = 5.0f; s.releaseMs = 300.0f;
        Processor p; p.prepare (fs); p.setTargets (s, true);
        const auto r = render (p, 2.0, 512, sine (1000.0, -8.0));
        const double gain = rmsDb (r.out, 1.0, 2.0) - rmsDb (r.in, 1.0, 2.0);
        std::snprintf (msg, sizeof msg, "Modern 4:1 at -20 dB, sine 12 dB over: %.2f dB of gain (static -9)", gain);
        check (std::abs (gain + 9.0) < 0.5, msg);
        const double thd = (harmonic (r.out, 1.0, 2.0, 2000.0) + harmonic (r.out, 1.0, 2.0, 3000.0)) / harmonic (r.out, 1.0, 2.0, 1000.0);
        std::snprintf (msg, sizeof msg, "Modern is transparent: harmonics %.4f %% of the fundamental", 100.0 * thd);
        check (thd < 3.0e-4, msg);
    }
    {
        Settings s; s.mode = modern; s.thresholdDb = -10.0f;
        Processor p; p.prepare (fs); p.setTargets (s, true);
        const auto r = render (p, 0.5, 256, sine (440.0, -20.0));
        check (r.out == r.in, "Modern below the threshold is bit-exact");
        Settings m = s; m.mix = 0.0f; m.thresholdDb = -40.0f; Processor q; q.prepare (fs); q.setTargets (m, true);
        const auto z = render (q, 0.5, 256, sine (440.0, -6.0));
        check (z.out == z.in, "Mix 0 is bit-exact dry");
    }
    {
        Settings s; s.mode = vintage; s.thresholdDb = -20.0f; s.ratio = 4.0f; s.time = time2;
        Processor p; p.prepare (fs); p.setTargets (s, true);
        const auto r = render (p, 3.0, 512, sine (1000.0, -2.0));
        const double gain = rmsDb (r.out, 2.0, 3.0) - rmsDb (r.in, 2.0, 3.0);
        const double expected = -gainReductionDb (s, -2.0f);
        std::snprintf (msg, sizeof msg, "Vintage 4:1 at -20 dB, sine 18 dB over: %.2f dB of gain (curve %.2f)", gain, expected);
        check (std::abs (gain - expected) < 1.5, msg);
        const double h2 = harmonic (r.out, 2.0, 3.0, 2000.0) / harmonic (r.out, 2.0, 3.0, 1000.0);
        const double h3 = harmonic (r.out, 2.0, 3.0, 3000.0) / harmonic (r.out, 2.0, 3.0, 1000.0);
        std::snprintf (msg, sizeof msg, "Vintage tube colour: 2nd harmonic %.2f %%, 3rd %.2f %% (2nd dominates, gentle)", 100.0 * h2, 100.0 * h3);
        check (h2 > 0.002 && h2 < 0.06 && h2 > h3, msg);
    }
    {
        const auto releaseTime = [] (int time, double burst) {
            Settings s; s.mode = vintage; s.thresholdDb = -30.0f; s.ratio = 6.0f; s.time = time;
            Processor p; p.prepare (fs); p.setTargets (s, true);
            const auto on = sine (200.0, -6.0);
            const auto r = render (p, burst + 30.0, 480, [&] (double t) { return t < burst ? on (t) : 0.0f; });
            const size_t start = (size_t) (burst * fs / 480);
            const float peak = r.reduction[start - 1];
            for (size_t i = start; i < r.reduction.size(); ++i) if (r.reduction[i] < 0.25f * peak) return (double) (i - start) * 480.0 / fs;
            return 99.0;
        };
        const double shortOpto = releaseTime (opto, 0.3), longOpto = releaseTime (opto, 8.0);
        std::snprintf (msg, sizeof msg, "Opto releases to 25 %%: %.2f s after a short burst, %.2f s after 8 s of compression", shortOpto, longOpto);
        check (longOpto > 1.5 * shortOpto, msg);
        const double short6 = releaseTime (time6, 0.3), long6 = releaseTime (time6, 8.0);
        std::snprintf (msg, sizeof msg, "Fairchild time 6 (program dependent): %.2f s after a burst, %.2f s after sustained compression", short6, long6);
        check (long6 > 3.0 * short6, msg);
        const double t1 = releaseTime (time1, 2.0), t4 = releaseTime (time4, 2.0);
        std::snprintf (msg, sizeof msg, "Fairchild time 1 releases in %.2f s, time 4 in %.2f s", t1, t4);
        check (t1 < 0.6 && t4 > 4.0 && t4 < 12.0, msg);
    }
    for (int mode = 0; mode < numModes; ++mode) {
        Settings s; s.mode = mode; s.thresholdDb = -30.0f; s.ratio = 8.0f; s.attackMs = 1.0f; s.releaseMs = 50.0f; s.time = time1; s.makeupDb = 6.0f; s.mix = 0.7f;
        const auto signal = [] (double t) { return (float) (0.8 * std::sin (2.0 * pi * 110.0 * t) * (std::fmod (t, 0.25) < 0.05 ? 1.0 : 0.1)); };
        Processor a; a.prepare (fs); a.setTargets (s, true); const auto x = render (a, 2.0, 512, signal);
        Processor b; b.prepare (fs); b.setTargets (s, true); const auto y = render (b, 2.0, 37, signal);
        double diff = 0.0; bool finite = true;
        for (size_t i = 0; i < x.out.size(); ++i) { diff = std::max (diff, (double) std::abs (x.out[i] - y.out[i])); finite = finite && std::isfinite (x.out[i]) && std::abs (x.out[i]) < 4.0f; }
        std::snprintf (msg, sizeof msg, "%s: finite, independent of the block size (%.1e)", mode == vintage ? "Vintage" : "Modern", diff);
        check (finite && diff < 1e-5, msg);
        Processor fresh; fresh.prepare (fs); fresh.setTargets (s, true);
        a.resetLayer (0); a.reset();
        const auto u = render (a, 0.5, 256, signal), v = render (fresh, 0.5, 256, signal);
        std::snprintf (msg, sizeof msg, "%s: reset matches a fresh processor; silence in gives exact silence out", mode == vintage ? "Vintage" : "Modern");
        const auto quiet = render (fresh, 0.5, 256, [] (double) { return 0.0f; });
        bool silent = true; for (size_t i = (size_t) (0.25 * fs); i < quiet.out.size(); ++i) silent = silent && std::abs (quiet.out[i]) < 1e-6f;
        check (u.out == v.out && silent, msg);
    }
    {
        Settings s; s.mode = vintage;
        Processor p; p.prepare (fs); p.setTargets (s, true);
        std::vector<float> buf[4]; for (auto& b : buf) b.assign (512, 0.3f);
        float* ch[4] { buf[0].data(), buf[1].data(), buf[2].data(), buf[3].data() };
        bool en[2] { true, true };
        const auto start = std::chrono::steady_clock::now();
        for (int b = 0; b < (int) (4.0 * fs / 512); ++b) p.process (ch, en, 512);
        const double seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
        std::printf ("BENCH Vintage compressor, 2 layers @48k: %.2f %% of one core\n", seconds / 4.0 * 100.0);
        check (seconds < 0.2, "compressor costs under 5 % of one core");
    }
    std::printf (failures == 0 ? "ALL COMPRESSOR TESTS PASSED\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
