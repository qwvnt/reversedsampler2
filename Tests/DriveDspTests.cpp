// Standalone tests for Source/DriveDsp.h (no JUCE). Build: g++ -std=c++17 -O2 -ISource Tests/DriveDspTests.cpp
#include "DriveDsp.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
int failures = 0;
void check (bool ok, const char* what, double value = 0.0) {
    std::printf ("%s %-70s %.4f\n", ok ? "PASS" : "FAIL", what, value);
    if (! ok) ++failures;
}
constexpr double pi = 3.14159265358979323846;

struct Rig {
    rs::drive::Processor p; double fs;
    explicit Rig (double rate, rs::drive::Settings s) : fs (rate) { p.prepare (rate); p.setTargets (s, true); }
    // Renders a sine on all 4 channels, returns layer 1 left.
    std::vector<float> sine (double hz, float amp, int n, int block = 256) {
        std::vector<float> ch[4];
        for (auto& c : ch) { c.resize ((size_t) n); for (int i = 0; i < n; ++i) c[(size_t) i] = amp * (float) std::sin (2 * pi * hz * i / fs); }
        const bool enabled[2] { true, true };
        for (int start = 0; start < n; start += block) {
            const int len = std::min (block, n - start);
            float* ptr[4] { ch[0].data() + start, ch[1].data() + start, ch[2].data() + start, ch[3].data() + start };
            p.process (ptr, enabled, len);
        }
        return ch[0];
    }
};

// Amplitude of one frequency over [from, end) via a windowed DFT (Hann), in dB re. full-scale sine.
double toneDb (const std::vector<float>& x, double hz, double fs, size_t from) {
    double re = 0, im = 0, wsum = 0; const size_t n = x.size() - from;
    for (size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos (2 * pi * (double) i / (double) n);
        re += w * x[from + i] * std::cos (2 * pi * hz * (double) (from + i) / fs);
        im += w * x[from + i] * std::sin (2 * pi * hz * (double) (from + i) / fs);
        wsum += w;
    }
    return 20.0 * std::log10 (std::max (1e-12, 2.0 * std::sqrt (re * re + im * im) / wsum));
}
double rmsDb (const std::vector<float>& x, size_t from) {
    double s = 0; for (size_t i = from; i < x.size(); ++i) s += (double) x[i] * x[i];
    return 10.0 * std::log10 (std::max (1e-24, s / (double) (x.size() - from)));
}
rs::drive::Settings settings (int type, float drive, float tone = 1.0f, float mix = 1.0f) {
    rs::drive::Settings s; s.type = type; s.drive01 = drive; s.tone01 = tone; s.mix = mix; s.output = 1.0f; return s;
}
}

int main() {
    using namespace rs::drive;
    const double fs = 44100.0; const int n = 32768; const size_t settle = 8192;

    // 1. Oversampling chain alone (Mix 0) is flat across the audio band.
    {
        double worst = 0;
        for (double hz : { 30.0, 100.0, 1000.0, 5000.0, 10000.0, 15000.0, 18000.0 }) {
            Rig r (fs, settings (tube, 1.0f, 1.0f, 0.0f));
            worst = std::max (worst, std::abs (toneDb (r.sine (hz, 0.5f, n), hz, fs, settle) - 20 * std::log10 (0.5)));
        }
        check (worst < 0.01, "Mix 0: 4x up/down chain is flat to 18 kHz (dB error)", worst);
    }
    // 2. Drive 0 on a quiet signal is clean unity for every type (curves have slope 1, auto-gain ~1).
    for (int t = 0; t < numTypes; ++t) {
        Rig r (fs, settings (t, 0.0f));
        const double g = toneDb (r.sine (1000.0, 0.01f, n), 1000.0, fs, settle) + 40.0;
        char what[96]; std::snprintf (what, sizeof what, "%s: drive 0 at -40 dBFS is unity (dB)", typeNames[t]);
        check (std::abs (g) < 1.0, what, g);
    }
    // 3. Auto-gain: a -12 dBFS sine keeps its loudness at any drive.
    for (int t = 0; t < numTypes; ++t) {
        double worst = 0;
        for (float d : { 0.25f, 0.5f, 0.75f, 1.0f }) {
            Rig r (fs, settings (t, d));
            worst = std::max (worst, std::abs (rmsDb (r.sine (220.0, 0.25f, n), settle) - 20 * std::log10 (0.25 / std::sqrt (2.0))));
        }
        char what[96]; std::snprintf (what, sizeof what, "%s: loudness matched across Drive (worst dB)", typeNames[t]);
        check (worst < 2.0, what, worst);
    }
    // 4. Harmonic character at medium drive.
    double h2[numTypes], h3[numTypes];
    for (int t = 0; t < numTypes; ++t) {
        Rig r (fs, settings (t, 0.5f));
        const auto y = r.sine (500.0, 0.25f, n);
        const double f = toneDb (y, 500.0, fs, settle);
        h2[t] = toneDb (y, 1000.0, fs, settle) - f; h3[t] = toneDb (y, 1500.0, fs, settle) - f;
        std::printf ("     %-9s H2 %6.1f dB  H3 %6.1f dB\n", typeNames[t], h2[t], h3[t]);
    }
    check (h2[tube] > -25.0 && h2[tube] > h3[tube], "Tube: warm, 2nd harmonic dominates (H2 dB)", h2[tube]);
    check (h3[softClip] > h2[softClip] + 10.0 && h2[softClip] > -70.0, "Soft Clip: odd harmonics, trace of 2nd from diode mismatch (H3 dB)", h3[softClip]);
    check (h3[tape] > h2[tape] + 20.0 && h3[tape] > -45.0, "Tape: symmetric hysteresis, 3rd harmonic (H3 dB)", h3[tape]);
    check (h2[triode] > -30.0, "Triode: asymmetric 12AX7 stage, strong 2nd (H2 dB)", h2[triode]);
    // 5. Top end at small signal: Tube darkest, Soft Clip open, Tape gap loss, Triode Miller roll-off.
    {
        double hf[numTypes];
        for (int t = 0; t < numTypes; ++t) {   // 15 kHz relative to 1 kHz, small signal
            Rig r (fs, settings (t, 0.0f)), r1 (fs, settings (t, 0.0f));
            hf[t] = toneDb (r.sine (15000.0, 0.01f, n), 15000.0, fs, settle) - toneDb (r1.sine (1000.0, 0.01f, n), 1000.0, fs, settle);
        }
        check (hf[tube] < -1.5, "Tube: soft vintage top (15 kHz re 1 kHz, dB)", hf[tube]);
        check (std::abs (hf[softClip]) < 0.6, "Soft Clip: full band, open top (dB)", hf[softClip]);
        check (hf[tape] < -0.5 && hf[tape] > -3.0, "Tape: gentle gap loss (dB)", hf[tape]);
        check (hf[triode] < -0.5 && hf[triode] > -3.5, "Triode: Miller roll-off (dB)", hf[triode]);
    }
    // 5b. Tape: head bump, and highs saturate earlier (pre-emphasis), like a real machine.
    {
        Rig lo (fs, settings (tape, 0.0f)), ref (fs, settings (tape, 0.0f));
        const double bump = toneDb (lo.sine (60.0, 0.01f, n), 60.0, fs, settle) - toneDb (ref.sine (1000.0, 0.01f, n), 1000.0, fs, settle);
        check (bump > 1.0 && bump < 2.2, "Tape: head bump at 60 Hz (dB)", bump);
        const auto comp = [&] (double hz) {
            Rig q (fs, settings (tape, 0.6f)), l (fs, settings (tape, 0.6f));
            return (toneDb (l.sine (hz, 0.5f, n), hz, fs, settle) - 20 * std::log10 (0.5)) - (toneDb (q.sine (hz, 0.01f, n), hz, fs, settle) + 40.0);
        };
        const double c1 = comp (1000.0), c8 = comp (8000.0);
        check (c8 < c1 - 1.0, "Tape: 8 kHz compresses more than 1 kHz (dB difference)", c8 - c1);
    }
    // 6. Aliasing (4x IIR + ADAA): worst inharmonic component below 18 kHz of a -6 dBFS sine.
    const auto alias = [&] (int t, float drive, double hz) {
        Rig r (fs, settings (t, drive));
        const auto y = r.sine (hz, 0.5f, n);
        const double f = toneDb (y, hz, fs, settle);
        double worst = -200;
        for (int k = 2; k < 200; ++k) {   // fold each harmonic above Nyquist back into the band
            if (hz * k < fs / 2) continue;
            double a = std::fmod (hz * k, fs); if (a > fs / 2) a = fs - a;
            const double r0 = std::fmod (a, hz);
            if (r0 < 30.0 || hz - r0 < 30.0 || a < 40.0 || a > 18000.0) continue;
            worst = std::max (worst, toneDb (y, a, fs, settle) - f);
        }
        return worst;
    };
    for (int t = 0; t < numTypes; ++t) {
        const double hard = alias (t, 1.0f, 2507.0), medium = alias (t, 0.5f, 4999.0);
        char what[96]; std::snprintf (what, sizeof what, "%s: aliasing, 2.5 kHz @ max drive < -45 dB", typeNames[t]);
        check (hard < -45.0, what, hard);
        std::snprintf (what, sizeof what, "%s: aliasing, 5 kHz @ 50 %% drive < -75 dB", typeNames[t]);
        check (medium < -75.0, what, medium);
    }
    // 7. Hot input stays finite and bounded.
    {
        bool ok = true; float peak = 0;
        for (int t = 0; t < numTypes; ++t) {
            Rig r (fs, settings (t, 1.0f));
            for (float v : r.sine (80.0, 4.0f, 8192)) { ok &= std::isfinite (v); peak = std::max (peak, std::abs (v)); }
        }
        check (ok && peak < 4.0f, "+12 dBFS input at max drive: finite, bounded (peak)", peak);
    }
    // 8. Type switching is a crossfade, not a click.
    {
        Rig r (fs, settings (tube, 0.7f));
        std::vector<float> ch[4]; for (auto& c : ch) c.resize (8192);
        for (int i = 0; i < 8192; ++i) for (auto& c : ch) c[(size_t) i] = 0.3f * (float) std::sin (2 * pi * 110.0 * i / fs);
        const bool en[2] { true, false };
        float maxStepBefore = 0, maxStepAfter = 0;
        for (int start = 0, blk = 0; start < 8192; start += 128, ++blk) {
            if (start == 4096) r.p.setTargets (settings (tape, 0.7f), false);
            float* ptr[4] { ch[0].data() + start, ch[1].data() + start, ch[2].data() + start, ch[3].data() + start };
            r.p.process (ptr, en, 128);
        }
        for (int i = 2001; i < 4096; ++i) maxStepBefore = std::max (maxStepBefore, std::abs (ch[0][(size_t) i] - ch[0][(size_t) i - 1]));
        for (int i = 4096; i < 6000; ++i) maxStepAfter = std::max (maxStepAfter, std::abs (ch[0][(size_t) i] - ch[0][(size_t) i - 1]));
        check (maxStepAfter < 2.5f * maxStepBefore + 0.02f, "Tube -> Tape switch has no click (step ratio)", maxStepAfter / std::max (1e-6f, maxStepBefore));
    }
    // 9. Block size never changes the result.
    {
        Rig a (fs, settings (tape, 0.8f, 0.6f, 0.7f)), b (fs, settings (tape, 0.8f, 0.6f, 0.7f));
        const auto ya = a.sine (300.0, 0.4f, 10000, 512), yb = b.sine (300.0, 0.4f, 10000, 37);
        float diff = 0; for (size_t i = 0; i < ya.size(); ++i) diff = std::max (diff, std::abs (ya[i] - yb[i]));
        check (diff == 0.0f, "bit-identical for block sizes 512 and 37", diff);
    }
    // 10. Tone 0 is dark, Tone 100 is open.
    {
        Rig dark (fs, settings (softClip, 0.0f, 0.0f)), open (fs, settings (softClip, 0.0f, 1.0f));
        const double d = toneDb (dark.sine (8000.0, 0.01f, n), 8000.0, fs, settle) + 40.0;
        const double o = toneDb (open.sine (8000.0, 0.01f, n), 8000.0, fs, settle) + 40.0;
        Rig ref (fs, settings (softClip, 0.0f, 1.0f));
        const double r = toneDb (ref.sine (1000.0, 0.01f, n), 1000.0, fs, settle) + 40.0;
        check (d - r < -20.0 && std::abs (o - r) < 0.3, "Tone: 0 % darkens 8 kHz, 100 % is open (dark dB)", d - r);
    }
    // 11. CPU: one slot, both layers, stereo.
    if (std::getenv ("RS_SKIP_BENCH") == nullptr) {
        for (int t = 0; t < numTypes; ++t) {
            Processor p; p.prepare (48000.0); p.setTargets (settings (t, 0.8f, 0.7f, 0.8f), true);
            std::vector<float> ch[4]; for (auto& c : ch) c.resize (512);
            const bool en[2] { true, true }; const int blocks = 48000 * 10 / 512;
            const auto t0 = std::chrono::steady_clock::now();
            for (int b = 0; b < blocks; ++b) {
                for (int i = 0; i < 512; ++i) for (auto& c : ch) c[(size_t) i] = 0.3f * (float) std::sin (2 * pi * 220.0 * (b * 512 + i) / 48000.0);
                float* ptr[4] { ch[0].data(), ch[1].data(), ch[2].data(), ch[3].data() };
                p.process (ptr, en, 512);
            }
            const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
            std::printf ("BENCH %-9s 2 layers stereo @48k: %.2f %% of one core\n", typeNames[t], sec / 10.0 * 100.0);
        }
    }
    if (failures == 0) std::printf ("ALL DRIVE TESTS PASSED\n"); else std::printf ("%d DRIVE TEST(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
