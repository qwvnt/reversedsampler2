// FX rack tests that need no JUCE: Source/FxRack.cpp is built against the stand-ins in Tests/MockJuce.
//   g++ -std=c++17 -O2 -ISource -ITests/MockJuce Tests/FxRackTests.cpp Source/FxRack.cpp -o FxRackTests && ./FxRackTests
// What they check (v0.16):
//  - switching, re-routing or resetting an effect while audio flows never clicks (20 ms cross-fades);
//  - the outgoing effect fades out with its own knob values, not with the new type's defaults;
//  - switching a transparent effect on never dips the level (its input fades in, then its output);
//  - Tape and Amethyst glide to a new Time like a tape motor: never backwards, no kinks, and they arrive;
//  - a slot that is silent switches at once, and settings made before the first block apply at once;
//  - the result does not depend on the host block size (larger blocks are processed in pieces);
//  - a silent rack still goes to sleep and outputs exact silence;
//  - reset() (host bypass) clears every tail at once;
//  - nothing reads memory that was never written (the stand-in buffers start as NaN).
#include "FxRack.h"
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

static int passes = 0, failures = 0;
#define CHECK(cond, msg) do { if (cond) ++passes; else { ++failures; std::cout << "  FAIL: " << msg << "\n"; } } while (false)

namespace {
constexpr double rate = 48000.0;

struct Rig {
    juce::AudioProcessorValueTreeState state;
    rs::FxRackDSP rack;
    std::vector<float> inA, outA, outB, outAR;
    long long t = 0;
    double ampA = 0.3, ampB = 0.0, level = 1.0;   // level: the input fades in and out over 10 ms
    double processSeconds = 0.0;                   // time spent inside FxRackDSP::process
    bool input = true;

    explicit Rig (int maxBlock = 512) {
        for (int slot = 0; slot < rs::fxSlots; ++slot) defaults (slot);
        rack.connect (state);
        rack.prepare (rate, maxBlock);
    }
    void set (int slot, const char* name, float v) { state.getRawParameterValue (rs::fxID (slot, name))->store (v); }
    void defaults (int slot) {
        // The plugin's defaults (PluginProcessor.cpp, createParameterLayout).
        const std::pair<const char*, float> values[] {
            { "type", 0.0f }, { "a", 1.0f }, { "b", 1.0f }, { "p1", 50.0f }, { "p2", 50.0f }, { "p3", 50.0f }, { "mix", 50.0f },
            { "delayStyle", 2.0f }, { "sync", 0.0f }, { "division", 10.0f }, { "diffusion", 0.0f }, { "bias", 50.0f },
            { "replica", 1.0f }, { "reflections", 1.0f }, { "amSplice", 350.0f }, { "amReverse", 1.0f }, { "replicaBrown", 0.0f },
            { "eqAir", 50.0f }, { "eqLowCut", 20.0f }, { "eqHighCut", 20000.0f }, { "eqMode", 0.0f }, { "driveType", 0.0f },
            { "rvMode", 0.0f }, { "rvPreDelay", 10.0f }, { "rvLowCut", 80.0f }, { "rvChar", 50.0f },
            { "chModel", 0.0f }, { "chMode", 0.0f }, { "chVariant", 0.0f }, { "chStereo", 0.0f }, { "chDelay", 15.0f },
            { "chFeedback", 0.0f }, { "chVoices", 4.0f }, { "chOutput", 0.0f }, { "chHQ", 1.0f }, { "chVibRate", 50.0f }, { "chVibDepth", 30.0f },
            { "cpMode", 0.0f }, { "cpKnee", 6.0f }, { "cpAttack", 10.0f }, { "cpRelease", 150.0f }, { "cpTime", 1.0f } };
        for (const auto& v : values) set (slot, v.first, v.second);
    }
    // Like the editor (PluginProcessor::setFxType): the type, then the shared knobs get its defaults.
    void setType (int slot, int type) {
        set (slot, "type", (float) type);
        if (type > 0) {
            const char* shared[] { "p1", "p2", "p3", "mix" };
            for (int k = 0; k < 4; ++k) set (slot, shared[k], rs::fxTypeDefaults[type][k]);
        }
    }
    void runBlocks (int count, int block) {
        juce::AudioBuffer<float> audio (4, block);
        const double step = 1.0 / (0.01 * rate), twoPi = juce::MathConstants<double>::twoPi;
        for (int b = 0; b < count; ++b) {
            for (int n = 0; n < block; ++n) {
                level = input ? std::min (1.0, level + step) : std::max (0.0, level - step);
                const double time = (double) (t + n) / rate;
                const auto a = (float) (level * ampA * std::sin (twoPi * 220.0 * time));
                const auto bb = (float) (level * ampB * std::sin (twoPi * 330.0 * time + 1.0));
                audio.setSample (0, n, a); audio.setSample (1, n, a);
                audio.setSample (2, n, bb); audio.setSample (3, n, bb);
                inA.push_back (a);
            }
            const auto start = std::chrono::steady_clock::now();
            rack.process (audio, block, 120.0);
            processSeconds += std::chrono::duration<double> (std::chrono::steady_clock::now() - start).count();
            for (int n = 0; n < block; ++n) { outA.push_back (audio.getSample (0, n)); outB.push_back (audio.getSample (2, n)); outAR.push_back (audio.getSample (1, n)); }
            t += block;
        }
    }
    void run (double seconds, int block = 256) { runBlocks ((int) std::ceil (seconds * rate / block), block); }
    double now() const { return (double) outA.size() / rate; }
};

size_t index (double seconds, size_t size) { return std::min (size, (size_t) std::max (0.0, std::round (seconds * rate))); }
double roughness (const std::vector<float>& x, double t0, double t1) {
    double m = 0.0;
    for (size_t i = std::max<size_t> (2, index (t0, x.size())); i < index (t1, x.size()); ++i)
        m = std::max (m, std::abs ((double) x[i] - 2.0 * x[i - 1] + x[i - 2]));
    return m;
}
double maxDiff (const std::vector<float>& x, const std::vector<float>& y, double t0, double t1) {
    double m = 0.0;
    for (size_t i = index (t0, x.size()); i < index (t1, std::min (x.size(), y.size())); ++i) m = std::max (m, std::abs ((double) x[i] - y[i]));
    return m;
}
double peak (const std::vector<float>& x, double t0, double t1) {
    double m = 0.0;
    for (size_t i = index (t0, x.size()); i < index (t1, x.size()); ++i) m = std::max (m, (double) std::abs (x[i]));
    return m;
}
bool finite (const std::vector<float>& x) {
    for (const float v : x) if (! std::isfinite (v)) return false;
    return true;
}
double rms (const std::vector<float>& x, double t0, double t1) {
    double sum = 0.0; size_t count = 0;
    for (size_t i = index (t0, x.size()); i < index (t1, x.size()); ++i, ++count) sum += (double) x[i] * x[i];
    return std::sqrt (sum / (double) std::max<size_t> (1, count));
}

// Applies a change at a block boundary. A one-sample switch between two different signals is a step,
// i.e. a click: the 0.23 s after the change may not be rougher than the steady state around it.
void changeOver (Rig& r, const std::string& what, const std::function<void()>& apply) {
    const double at = r.now();
    apply();
    r.run (0.35);
    const double steady = std::max (roughness (r.outA, at - 0.15, at - 0.005), roughness (r.outA, at + 0.23, at + 0.35));
    const double edge = roughness (r.outA, at - 0.002, at + 0.23);
    std::cout << "    " << what << ": " << edge << " (steady " << steady << ")\n";
    CHECK (edge < 2.0 * steady + 2.0e-4, "no click: " << what);
}

// Every type at once on both layers; slot 5 (a Reverse delay) only on layer A.
void scene (Rig& r) {
    r.ampB = 0.25;
    r.setType (0, 1); r.set (0, "delayStyle", 3); r.set (0, "p1", 10);
    r.setType (1, 3); r.set (1, "driveType", 2);
    r.setType (2, 4); r.set (2, "rvMode", 4);
    r.setType (3, 2); r.set (3, "p1", 80); r.set (3, "eqMode", 1);
    r.setType (4, 1); r.set (4, "delayStyle", 0); r.set (4, "p1", 20); r.set (4, "b", 0);
}

void testRoutes() {
    std::cout << "A/B route changes cross-fade\n";
    const std::pair<const char*, std::function<void (Rig&)>> cases[] {
        { "Delay", [] (Rig& r) { r.setType (0, 1); r.set (0, "p1", 0); r.set (0, "p2", 40); r.set (0, "mix", 100); } },
        { "EQ", [] (Rig& r) { r.setType (0, 2); r.set (0, "p1", 100); r.set (0, "p2", 0); r.set (0, "p3", 100); } },
        { "Drive", [] (Rig& r) { r.setType (0, 3); } },
        { "Reverb", [] (Rig& r) { r.setType (0, 4); r.set (0, "mix", 60); } },
        { "Chorus", [] (Rig& r) { r.setType (0, 5); r.set (0, "chModel", 1); } },
        { "Compressor", [] (Rig& r) { r.setType (0, 6); r.set (0, "p1", 20); r.set (0, "p3", 50); } },
    };
    for (const auto& c : cases) {
        Rig r; c.second (r); r.run (0.5);
        const double at = r.now();
        changeOver (r, std::string (c.first) + ", layer A off", [&] { r.set (0, "a", 0); });
        CHECK (maxDiff (r.outA, r.inA, at + 0.0201, r.now()) == 0.0, c.first << ": exactly dry 20 ms after layer A is switched off");
        changeOver (r, std::string (c.first) + ", layer A on", [&] { r.set (0, "a", 1); });
        CHECK (maxDiff (r.outA, r.inA, r.now() - 0.1, r.now()) > 1.0e-3, c.first << ": layer A is processed again");
        CHECK (finite (r.outA) && finite (r.outB), c.first << ": finite output");
    }
}

void testTypes() {
    std::cout << "Type changes cross-fade\n";
    const auto eq = [] (Rig& r) { r.setType (0, 2); r.set (0, "p1", 100); r.set (0, "p3", 0); };
    Rig r; r.setType (0, 1); r.set (0, "p1", 0); r.set (0, "mix", 100); r.run (0.5);
    changeOver (r, "Delay -> Reverb", [&] { r.setType (0, 4); });
    changeOver (r, "Reverb -> Drive", [&] { r.setType (0, 3); r.set (0, "p1", 60); });
    const double at = r.now();
    changeOver (r, "Drive -> EQ", [&] { eq (r); });
    // 20 ms out + 20 ms in: soon after, the new effect plays exactly as if it had always been there.
    Rig ref; eq (ref); ref.run (r.now());
    const double settled = maxDiff (r.outA, ref.outA, at + 0.1, r.now());
    std::cout << "    EQ 100 ms after the change vs. an EQ that was always there: " << settled << "\n";
    CHECK (settled < 1.0e-4, "after a type change the new effect runs on its own");
    changeOver (r, "EQ -> Empty", [&] { r.setType (0, 0); });
    CHECK (maxDiff (r.outA, r.inA, r.now() - 0.3, r.now()) == 0.0, "an Empty slot is exactly dry");
    changeOver (r, "Empty -> Delay", [&] { r.setType (0, 1); r.set (0, "p1", 0); r.set (0, "mix", 100); });
    CHECK (finite (r.outA) && finite (r.outB), "finite output");
}

void testDelayCharacter() {
    std::cout << "Delay character changes cross-fade\n";
    Rig r; r.setType (0, 1); r.set (0, "p1", 0); r.set (0, "p2", 40); r.set (0, "mix", 100); r.run (0.5);
    changeOver (r, "Replica Modern -> Vintage", [&] { r.set (0, "replica", 0); });
    changeOver (r, "Replica -> Tape", [&] { r.set (0, "delayStyle", 1); });
    changeOver (r, "Tape -> Reverse", [&] { r.set (0, "delayStyle", 0); });
    changeOver (r, "Reverse -> Amethyst", [&] { r.set (0, "delayStyle", 3); });
    changeOver (r, "Amethyst reverse -> forward", [&] { r.set (0, "amReverse", 0); });
    changeOver (r, "Amethyst -> Replica", [&] { r.set (0, "delayStyle", 2); });
    CHECK (finite (r.outA), "finite output");
}

void testResets() {
    std::cout << "reset requests (slot swap, project load) cross-fade\n";
    Rig r; r.setType (0, 1); r.set (0, "p1", 0); r.set (0, "p2", 60); r.set (0, "mix", 100); r.run (0.5);
    changeOver (r, "reset while audio flows", [&] { r.rack.requestReset (0); });
    r.input = false; r.run (0.1);   // the input fades out, the echoes ring on
    const double ringing = peak (r.outA, r.now() - 0.05, r.now());
    changeOver (r, "reset during a delay tail", [&] { r.rack.requestReset (0); });
    CHECK (ringing > 0.01, "the tail was ringing before the reset");
    CHECK (peak (r.outA, r.now() - 0.3, r.now()) == 0.0, "the tail is gone after the reset");
    r.rack.requestResetAll(); r.run (0.1);
    CHECK (peak (r.outA, r.now() - 0.1, r.now()) == 0.0, "resetting a silent rack changes nothing");
}

void testSilent() {
    std::cout << "a silent slot switches at once\n";
    const auto eq = [] (Rig& r) { r.setType (0, 2); r.set (0, "p1", 100); r.set (0, "p3", 0); };
    Rig fresh; eq (fresh); fresh.run (0.3);   // set before the first block: active from the first sample
    Rig later; later.setType (0, 1); later.set (0, "mix", 100); later.input = false; later.level = 0.0; later.run (1.0);
    eq (later);                               // switched while the slot is silent
    later.t = 0; later.inA.clear(); later.outA.clear(); later.outB.clear(); later.input = true; later.level = 1.0;
    later.run (0.3);
    CHECK (maxDiff (fresh.outA, fresh.inA, 0.0, 0.3) > 0.01, "the EQ is active from the first sample");
    CHECK (maxDiff (fresh.outA, later.outA, 0.0, 0.3) == 0.0, "a silent slot switches at once: same output as a fresh EQ");
}

void testBlockSizes() {
    std::cout << "the result does not depend on the host block size\n";
    Rig x (256), y (4096);   // x gets 1024-sample blocks and splits them, y gets 256-sample blocks
    scene (x); scene (y);
    const auto both = [&] (int blocks1024, const std::function<void (Rig&)>& change) {
        change (x); change (y);
        x.runBlocks (blocks1024, 1024); y.runBlocks (4 * blocks1024, 256);
    };
    both (40, [] (Rig&) {});
    both (20, [] (Rig& r) { r.set (0, "a", 0); });
    both (20, [] (Rig& r) { r.setType (2, 1); r.set (3, "eqMode", 0); });
    both (20, [] (Rig& r) { r.rack.requestReset (1); r.set (0, "a", 1); });
    both (40, [] (Rig& r) { r.input = false; });
    CHECK (x.outA == y.outA && x.outB == y.outB, "1024-sample blocks in 256-sample pieces = 256-sample blocks");
    CHECK (finite (x.outA) && finite (x.outB), "finite output");
}

void testIdle() {
    std::cout << "a silent rack still goes to sleep\n";
    Rig r; scene (r); r.set (2, "rvMode", 0); r.set (2, "p2", 20);   // a short Room instead of Bloom
    r.run (2.0);
    const double busy = r.processSeconds;
    r.input = false; r.run (15.0);
    r.processSeconds = 0.0; r.run (2.0);
    const double idle = r.processSeconds;
    std::cout << "    2 s busy: " << busy * 1000.0 << " ms, 2 s asleep: " << idle * 1000.0 << " ms\n";
    CHECK (peak (r.outA, r.now() - 2.0, r.now()) == 0.0 && peak (r.outB, r.now() - 2.0, r.now()) == 0.0, "exact silence once asleep");
    CHECK (idle < 0.1 * busy, "an idle rack costs less than 10 % of a busy one");
}

void testReset() {
    std::cout << "reset() (host bypass) clears every tail at once\n";
    Rig r; scene (r); r.run (1.0);
    r.input = false; r.level = 0.0;   // a bypassed instrument plays nothing
    r.rack.reset();
    r.run (0.5);
    CHECK (peak (r.outA, r.now() - 0.5, r.now()) == 0.0 && peak (r.outB, r.now() - 0.5, r.now()) == 0.0, "no tail comes back after reset()");
    Rig s, ref; scene (s); scene (ref);
    s.run (1.0); s.rack.reset();
    s.t = 0; s.inA.clear(); s.outA.clear(); s.outB.clear();
    s.run (0.5); ref.run (0.5);
    const double d = std::max (maxDiff (s.outA, ref.outA, 0.0, 0.5), maxDiff (s.outB, ref.outB, 0.0, 0.5));
    std::cout << "    after reset() vs. a freshly prepared rack: " << d << "\n";
    CHECK (d == 0.0, "after reset() the rack plays exactly like a freshly prepared one");
    CHECK (finite (r.outA) && finite (r.outB) && finite (s.outA) && finite (s.outB), "finite output");
}
void testHeldKnobs() {
    std::cout << "the outgoing effect keeps its own knob values\n";
    // The new type's defaults arrive together with the Type: a Tape or Amethyst at 40 ms fading out with
    // Reverb's Time 50 would jump to 520 ms and zip backwards through the line.
    for (const int style : { 1, 3 }) {
        Rig r; r.setType (0, 1); r.set (0, "delayStyle", (float) style);
        r.set (0, "p1", 0); r.set (0, "p2", 40); r.set (0, "mix", 100); r.run (0.5);
        changeOver (r, std::string (style == 1 ? "Tape" : "Amethyst") + " -> Reverb with its knobs", [&] { r.setType (0, 4); });
        CHECK (finite (r.outA), "finite output");
    }
    // A flat EQ is transparent, so switching it on may not dip the level: its input fades in while its
    // output is still dry, then its output fades in (both at once dipped by 2.5 dB halfway through).
    Rig r; r.setType (0, 2); r.set (0, "a", 0); r.run (0.5);
    const double at = r.now();
    r.set (0, "a", 1); r.run (0.2);
    const size_t width = (size_t) (0.005 * rate);
    double lowest = 2.0, highest = 0.0;
    for (size_t i = index (at - 0.01, r.outA.size()); i + width <= index (at + 0.15, r.outA.size()); i += width / 4) {
        double out = 0.0, in = 0.0;
        for (size_t k = i; k < i + width; ++k) { out += (double) r.outA[k] * r.outA[k]; in += (double) r.inA[k] * r.inA[k]; }
        lowest = std::min (lowest, std::sqrt (out / in)); highest = std::max (highest, std::sqrt (out / in));
    }
    std::cout << "    flat EQ, layer A on: level " << lowest << " .. " << highest << " of the input\n";
    CHECK (lowest > 0.97 && highest < 1.03, "switching a flat EQ on does not dip the level");
}

void testGlides() {
    std::cout << "Tape and Amethyst glide to a new Time like a tape motor\n";
    // The input stops, the line empties, the input starts again: when does the first echo come?
    const auto firstEcho = [] (Rig& r) {
        r.input = false; r.run (1.5);
        const double from = r.now();
        r.input = true; r.run (1.2);
        for (size_t i = index (from, r.outA.size()); i < r.outA.size(); ++i)
            if (std::abs (r.outA[i]) > 0.01f) return (double) i / rate - from;
        return -1.0;
    };
    for (const int style : { 1, 3 }) {
        const std::string name = style == 1 ? "Tape" : "Amethyst";
        const auto delay = [style] (Rig& r, float time) {
            r.setType (0, 1); r.set (0, "delayStyle", (float) style); r.set (0, "p1", time); r.set (0, "p2", 0); r.set (0, "mix", 100);
        };
        Rig r; delay (r, 0); r.run (0.5);
        changeOver (r, name + " Time 40 -> 520 ms", [&] { r.set (0, "p1", 50); });
        r.run (1.0);
        changeOver (r, name + " Time 520 -> 539 ms", [&] { r.set (0, "p1", 52); });
        r.run (1.0);
        const double at = r.now();
        r.set (0, "p1", 0); r.run (1.0);
        CHECK (finite (r.outA) && peak (r.outA, at, r.now()) < 0.6, name << ": a shortening glide stays bounded");
        r.set (0, "p1", 50); r.run (1.5);
        // It arrives: soon after the glide it plays like a delay that was always at 520 ms.
        Rig ref; delay (ref, 50); ref.run (r.now());
        const double level = 20.0 * std::log10 (rms (r.outA, r.now() - 0.5, r.now()) / rms (ref.outA, r.now() - 0.5, r.now()));
        const double difference = maxDiff (r.outA, ref.outA, r.now() - 0.2, r.now());
        const double heard = firstEcho (r), expected = firstEcho (ref);
        std::cout << "    " << name << " after the glides vs. always at 520 ms: first echo " << heard * 1000.0 << " ms vs. "
                  << expected * 1000.0 << " ms, level " << level << " dB, largest difference " << difference << "\n";
        CHECK (expected > 0.5 && std::abs (heard - expected) < 0.003, name << ": the glide arrives at the new Time");
        CHECK (std::abs (level) < 1.0, name << ": as loud as a delay that was always there");
        // Tape plays sample for sample the same; Amethyst aligns each grain to the one before, so after
        // a glide it may settle on another, equally good alignment.
        if (style == 1) CHECK (difference < 1.0e-4, "Tape: exactly the repeats of a delay that was always there");
    }
}
} // namespace

void testChorusFromEmpty() {
    std::cout << "Empty -> Chorus while playing\n";
    for (int model = 0; model < rs::chorus::numModels; ++model) {
        Rig r; r.run (0.5);
        r.setType (0, 5); r.set (0, "chModel", (float) model);
        r.run (0.6);
        const double left = maxDiff (r.outA, r.inA, r.now() - 0.3, r.now()), right = maxDiff (r.outAR, r.inA, r.now() - 0.3, r.now());
        std::cout << "    " << rs::chorus::modelNames[model] << ": L differs by " << left << ", R by " << right << "\n";
        CHECK (std::max (left, right) > 0.02, std::string (rs::chorus::modelNames[model]) + " is heard right after Empty -> Chorus");
    }
}

void testChorus() {
    std::cout << "Chorus models and modes cross-fade\n";
    Rig r; r.setType (0, 4); r.run (0.5);
    changeOver (r, "Reverb -> Chorus Classic", [&] { r.setType (0, 5); });
    changeOver (r, "Classic -> Spatial", [&] { r.set (0, "chModel", 1); });
    changeOver (r, "Spatial button 1 -> 3+4", [&] { r.set (0, "chMode", 6); });
    changeOver (r, "Spatial -> Digital Mod", [&] { r.set (0, "chModel", 3); r.set (0, "chMode", 1); r.set (0, "chFeedback", 70); });
    changeOver (r, "Digital Mod -> multi-voice", [&] { r.set (0, "chMode", 2); r.set (0, "chVoices", 8); });
    changeOver (r, "Liquid normal -> dark", [&] { r.set (0, "chModel", 2); r.set (0, "chVariant", 1); });
    changeOver (r, "Chorus -> Compressor (vintage)", [&] { r.setType (0, 6); });
    changeOver (r, "Compressor vintage -> modern", [&] { r.set (0, "cpMode", 1); });
    changeOver (r, "Compressor -> Empty", [&] { r.setType (0, 0); });
    CHECK (maxDiff (r.outA, r.inA, r.now() - 0.3, r.now()) == 0.0, "an Empty slot after a chorus is exactly dry");
    CHECK (finite (r.outA) && finite (r.outB), "finite output");
}

int main() {
    testChorusFromEmpty();
    testChorus();
    testRoutes();
    testTypes();
    testDelayCharacter();
    testHeldKnobs();
    testGlides();
    testResets();
    testSilent();
    testBlockSizes();
    testIdle();
    testReset();
    std::cout << "\n" << passes << " passed, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
