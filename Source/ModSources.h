#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace rs::mod {

inline constexpr int numMacros = 4, numLfos = 2, numRows = 16;

enum Source : int { none = 0, macro1, macro2, macro3, macro4, lfo1, lfo2, envelope, build, velocity, note, modWheel, aftertouch, pitchBend, random, numSources };
inline constexpr const char* sourceNames[numSources] { "None", "Macro 1", "Macro 2", "Macro 3", "Macro 4", "LFO 1", "LFO 2", "Envelope", "Build",
                                                       "Velocity", "Note", "Mod Wheel", "Aftertouch", "Pitch Bend", "Random" };
inline bool isBipolarByNature (int source) noexcept { return source == lfo1 || source == lfo2 || source == pitchBend; }

enum Shape : int { sine = 0, triangle, sawUp, sawDown, square, sampleHold, smoothRandom, numShapes };
inline constexpr const char* shapeNames[numShapes] { "Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H", "Random" };

enum Trigger : int { freeRun = 0, retrigger, oneShot, numTriggers };
inline constexpr const char* triggerNames[numTriggers] { "Free", "Retrig", "One Shot" };

inline constexpr int numDivisions = 16;
inline constexpr const char* divisionNames[numDivisions] { "8 bars", "4 bars", "2 bars", "1 bar", "1/2", "1/2T", "1/4.", "1/4", "1/4T", "1/8.", "1/8", "1/8T", "1/16.", "1/16", "1/16T", "1/32" };
inline constexpr double divisionBeats[numDivisions] { 32.0, 16.0, 8.0, 4.0, 2.0, 4.0 / 3.0, 1.5, 1.0, 2.0 / 3.0, 0.75, 0.5, 1.0 / 3.0, 0.375, 0.25, 1.0 / 6.0, 0.125 };

inline constexpr float minRateHz = 0.02f, maxRateHz = 30.0f;

inline double wrap (double phase) noexcept { return phase - std::floor (phase); }

inline float shapeCurve (float x, float curve) noexcept {
    x = std::clamp (x, 0.0f, 1.0f);
    const float k = std::clamp (curve, -1.0f, 1.0f) * 6.0f;
    if (std::abs (k) < 1.0e-4f) return x;
    return (std::exp (k * x) - 1.0f) / (std::exp (k) - 1.0f);
}

struct Random {
    std::uint32_t state = 0x2545F491u;
    float next() noexcept { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return (float) (state >> 8) * (1.0f / 16777216.0f); }
};

struct LfoSettings {
    int shape = sine;
    float rateHz = 1.0f;
    bool sync = false;
    int division = 7;
    int trigger = freeRun;
    float phase = 0.0f;
    float smooth = 0.0f;
};

class Lfo {
public:
    void prepare (double sampleRate) noexcept { fs = sampleRate > 0.0 ? sampleRate : 48000.0; reset(); }
    void reset() noexcept {
        cycle = 0.0; held = false; output = shapeAt (0.0); smoothed = output;
        random = Random {}; current = random.next(); nextValue = random.next();
    }
    void noteOn (const LfoSettings& s) noexcept {
        if (s.trigger == freeRun) return;
        cycle = 0.0; held = false;
        current = random.next(); nextValue = random.next();
    }
    float advance (const LfoSettings& s, int samples, double bpm, bool transportRunning, double ppqAtEnd) noexcept {
        settings = s;
        const double beats = divisionBeats[std::clamp (s.division, 0, numDivisions - 1)];
        const double hz = s.sync ? std::max (1.0, bpm) / 60.0 / beats : (double) std::clamp (s.rateHz, minRateHz, maxRateHz);
        const double before = cycle;
        if (s.sync && s.trigger == freeRun && transportRunning)
            cycle = ppqAtEnd / beats;
        else if (! held)
            cycle += hz * (double) samples / fs;
        if (s.trigger == oneShot && cycle >= 1.0) { cycle = 1.0; held = true; }
        const auto wholeBefore = (long long) std::floor (before), wholeAfter = (long long) std::floor (cycle);
        if (wholeAfter != wholeBefore && ! held) { current = nextValue; nextValue = random.next(); }
        const double position = held ? 1.0 - 1.0e-9 : wrap (cycle + (double) s.phase);
        output = shapeAt (position);
        const float smoothSeconds = std::clamp (s.smooth, 0.0f, 1.0f) * 0.25f / (float) hz;
        if (smoothSeconds <= 1.0e-5f) smoothed = output;
        else smoothed += (1.0f - std::exp (-(float) samples / (smoothSeconds * (float) fs))) * (output - smoothed);
        return smoothed;
    }
    float value() const noexcept { return smoothed; }
    double displayPhase() const noexcept { return held ? 1.0 : wrap (cycle + (double) settings.phase); }
    float shapeAt (double position) const noexcept {
        const auto p = (float) wrap (position);
        switch (settings.shape) {
            case triangle: return 1.0f - std::abs (2.0f * p - 1.0f);
            case sawUp: return p;
            case sawDown: return 1.0f - p;
            case square: return p < 0.5f ? 1.0f : 0.0f;
            case sampleHold: return current;
            case smoothRandom: { const float w = 0.5f - 0.5f * std::cos (3.14159265f * p); return current + w * (nextValue - current); }
            case sine:
            default: return 0.5f - 0.5f * std::cos (6.28318531f * p);
        }
    }
    static float shapePreview (int shape, double position) noexcept {
        Lfo l; l.settings.shape = shape; l.current = 0.2f; l.nextValue = 0.85f;
        if (shape == sampleHold || shape == smoothRandom) {
            const double cycles = position * 4.0;
            Random r; for (int i = 0; i < (int) cycles; ++i) r.next();
            l.current = r.next(); l.nextValue = r.next();
            return l.shapeAt (wrap (cycles));
        }
        return l.shapeAt (position);
    }
private:
    double fs = 48000.0, cycle = 0.0;
    bool held = false;
    float output = 0.0f, smoothed = 0.0f, current = 0.0f, nextValue = 0.0f;
    Random random;
    LfoSettings settings;
};

struct EnvelopeSettings { float attackMs = 10.0f, decayMs = 300.0f, sustain = 0.7f, releaseMs = 500.0f; };

class Envelope {
public:
    enum Stage { idle, attack, decay, sustain, release };
    void prepare (double sampleRate) noexcept { fs = sampleRate > 0.0 ? sampleRate : 48000.0; reset(); }
    void reset() noexcept { stage = idle; level = 0.0f; }
    void noteOn() noexcept { stage = attack; attackFrom = level; attackPosition = 0.0f; }
    void noteOff() noexcept { if (stage != idle) stage = release; }
    float advance (const EnvelopeSettings& s, int samples) noexcept {
        const float dt = (float) samples / (float) fs;
        const float sustainLevel = std::clamp (s.sustain, 0.0f, 1.0f);
        const auto settle = [dt] (float ms) { return 1.0f - std::exp (-dt * 5.0f / std::max (1.0e-4f, ms * 0.001f)); };
        switch (stage) {
            case attack: {
                const float length = std::max (1.0e-4f, s.attackMs * 0.001f);
                attackPosition = std::min (1.0f, attackPosition + dt / length);
                level = attackFrom + (1.0f - attackFrom) * attackPosition;
                if (attackPosition >= 1.0f) stage = decay;
                break;
            }
            case decay:
                level += settle (s.decayMs) * (sustainLevel - level);
                if (std::abs (level - sustainLevel) < 1.0e-4f) { level = sustainLevel; stage = sustain; }
                break;
            case sustain: level = sustainLevel; break;
            case release:
                level -= settle (s.releaseMs) * level;
                if (level < 1.0e-4f) { level = 0.0f; stage = idle; }
                break;
            case idle:
            default: level = 0.0f; break;
        }
        return level;
    }
    float value() const noexcept { return level; }
    Stage getStage() const noexcept { return stage; }
private:
    double fs = 48000.0;
    Stage stage = idle;
    float level = 0.0f, attackFrom = 0.0f, attackPosition = 0.0f;
};

struct RowSettings { int source = none, aux = none, destination = 0; float amount = 0.0f, curve = 0.0f; bool bipolar = false; };

inline float rowOutput (const RowSettings& row, const float* sources) noexcept {
    if (row.source <= none || row.source >= numSources || row.destination <= 0 || std::abs (row.amount) < 1.0e-6f) return 0.0f;
    float s = shapeCurve (sources[row.source], row.curve);
    if (row.bipolar) s = 2.0f * s - 1.0f;
    const float aux = row.aux > none && row.aux < numSources ? std::clamp (sources[row.aux], 0.0f, 1.0f) : 1.0f;
    return row.amount * s * aux;
}

}
