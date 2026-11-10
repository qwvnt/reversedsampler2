// JUCE-free check of the FX rack idle gate (v0.15): every DSP module must decay below
// -120 dB in finite time after the input stops (no self-noise, DC or denormal hum),
// otherwise the gate could never put a silent slot to sleep. After resetLayer the
// module must restart bit-exact like a freshly prepared one.
// g++ -std=c++17 -O2 -ISource Tests/IdleGateTests.cpp -o IdleGateTests
#include "ReverbDsp.h"
#include "DriveDsp.h"
#include "EqDsp.h"
#include <cstdio>
#include <cmath>
#include <random>
#include <vector>
#include <algorithm>

static int failures = 0;
static void check (bool ok, const char* what) { std::printf ("%s %s\n", ok ? "PASS" : "FAIL", what); if (! ok) ++failures; }

constexpr double fs = 48000.0;
constexpr int block = 512;
constexpr float silence = 1.0e-6f;

// Returns seconds of silent input until input+output stayed below -120 dB for `hold` seconds (or -1).
template <typename Proc>
static double timeToSleep (Proc& p, double holdSeconds, double maxSeconds) {
    std::vector<float> buf[4]; for (auto& b : buf) b.assign (block, 0.0f);
    float* ch[4] { buf[0].data(), buf[1].data(), buf[2].data(), buf[3].data() };
    bool en[2] { true, false };
    std::mt19937 rng (7); std::uniform_real_distribution<float> u (-0.5f, 0.5f);
    for (int b = 0; b < (int) (0.5 * fs / block); ++b) {
        for (int k = 0; k < block; ++k) { buf[0][(size_t) k] = u (rng); buf[1][(size_t) k] = u (rng); buf[2][(size_t) k] = buf[3][(size_t) k] = 0.0f; }
        p.process (ch, en, block);
    }
    const int hold = (int) (holdSeconds * fs); int silent = 0;
    for (int b = 0; b < (int) (maxSeconds * fs / block); ++b) {
        for (auto& v : buf) std::fill (v.begin(), v.end(), 0.0f);
        p.process (ch, en, block);
        float m = 0.0f; for (int k = 0; k < block; ++k) m = std::max ({ m, std::abs (buf[0][(size_t) k]), std::abs (buf[1][(size_t) k]) });
        if (! std::isfinite (m)) return -2.0;
        silent = m < silence ? silent + block : 0;
        if (silent >= hold) return (double) (b + 1) * block / fs;
    }
    return -1.0;
}

template <typename Proc>
static bool restartsClean (Proc& used, Proc& fresh) {
    used.resetLayer (0);
    std::vector<float> a[4], b[4]; for (int c = 0; c < 4; ++c) { a[c].assign (block, 0.0f); b[c].assign (block, 0.0f); }
    a[0][3] = a[1][3] = b[0][3] = b[1][3] = 0.8f;
    float* ca[4] { a[0].data(), a[1].data(), a[2].data(), a[3].data() };
    float* cb[4] { b[0].data(), b[1].data(), b[2].data(), b[3].data() };
    bool en[2] { true, false };
    double diff = 0.0;
    for (int r = 0; r < 40; ++r) {
        used.process (ca, en, block); fresh.process (cb, en, block);
        for (int k = 0; k < block; ++k) diff = std::max (diff, (double) std::abs (a[0][(size_t) k] - b[0][(size_t) k]));
        for (int c = 0; c < 4; ++c) { std::fill (a[c].begin(), a[c].end(), 0.0f); std::fill (b[c].begin(), b[c].end(), 0.0f); }
    }
    return diff < 1.0e-4;   // shared smoothers may differ by the tick phase only
}

int main() {
    const char* names[] { "Room", "Plate", "Ambience", "Terrain", "Bloom", "Shimmer" };
    for (int m = 0; m < rs::reverb::numModes; ++m)
        for (double decay : { 0.4, 1.0 }) {
            rs::reverb::Settings s; s.mode = m; s.size01 = 0.5f; s.decay01 = (float) decay; s.tone01 = 0.5f; s.mix = 0.3f; s.character01 = 1.0f;
            rs::reverb::Processor p; p.prepare (fs); p.setTargets (s, true);
            const double t = timeToSleep (p, 0.5, 240.0);
            const double rt = rs::reverb::decaySeconds (m, decay);
            char msg[160]; std::snprintf (msg, sizeof msg, "Reverb %-8s decay %.1f (RT60 %.1f s): asleep after %.1f s of silence", names[m], decay, rt, t);
            check (t > 0.0 && t < 3.0 * rt + 3.0, msg);
            rs::reverb::Processor fresh; fresh.prepare (fs); fresh.setTargets (s, true);
            if (decay == 0.4) { std::snprintf (msg, sizeof msg, "Reverb %-8s restarts clean after the gate reset", names[m]); check (restartsClean (p, fresh), msg); }
        }
    for (int type = 0; type < rs::drive::numTypes; ++type) {
        rs::drive::Settings d; d.drive01 = 1.0f; d.tone01 = 0.5f; d.output = 1.0f; d.mix = 1.0f; d.type = type;
        rs::drive::Processor p; p.prepare (fs); p.setTargets (d, true);
        const double t = timeToSleep (p, 0.1, 10.0);
        char msg[120]; std::snprintf (msg, sizeof msg, "Drive type %d at full drive: asleep after %.2f s of silence", type, t);
        check (t > 0.0 && t < 3.0, msg);   // Triode: the slow bias-shift recovery settles in ~1.2 s
        rs::drive::Processor fresh; fresh.prepare (fs); fresh.setTargets (d, true);
        std::snprintf (msg, sizeof msg, "Drive type %d restarts clean after the gate reset", type); check (restartsClean (p, fresh), msg);
    }
    {
        rs::eq::Settings e; rs::eq::Processor p; p.prepare (fs); p.setTargets (e, true);
        const double t = timeToSleep (p, 0.3, 10.0);
        char msg[120]; std::snprintf (msg, sizeof msg, "EQ: asleep after %.2f s of silence", t);
        check (t > 0.0 && t < 1.5, msg);
    }
    std::printf (failures == 0 ? "ALL IDLE GATE TESTS PASSED\n" : "%d FAILURES\n", failures);
    return failures == 0 ? 0 : 1;
}
