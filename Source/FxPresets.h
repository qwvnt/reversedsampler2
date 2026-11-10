#pragma once
// Factory presets for the FX rack, by effect type. Values are in the parameters' own units (the
// same numbers the knobs show): p1..p3/mix are 0..100, choices are their index, Hz and ms are plain.
// Anything a preset does not mention is left at the parameter's default, so every preset starts
// from the same clean state no matter what the slot held before. The A/B routing is kept.
#include <cstddef>

namespace rs {

struct FxPresetValue { const char* name = nullptr; float value = 0.0f; };

constexpr int maxFxPresetValues = 12;

// values is a fixed array ending at the first null name (never an initializer_list: in a constexpr
// aggregate MSVC lets its backing array die, and reading the preset crashes).
struct FxPreset {
    const char* genre;   // the sub-menu it sits in
    const char* name;
    int type;
    FxPresetValue values[maxFxPresetValues];

    struct Iterator {
        const FxPresetValue* v;
        bool operator!= (const Iterator& o) const noexcept { return v != o.v; }
        void operator++() noexcept { ++v; }
        const FxPresetValue& operator*() const noexcept { return *v; }
    };
    Iterator begin() const noexcept { return { values }; }
    Iterator end() const noexcept {
        int n = 0;
        while (n < maxFxPresetValues && values[n].name != nullptr) ++n;
        return { values + n };
    }
};

// Compressor knobs: Threshold -60..0 dB over 0..100 (70 = -18 dB), Ratio 1:1..20:1 exponential
// (46.3 = 4:1), Makeup 0..24 dB over 0..100. Delay Time 0..100 = 40 ms..1 s. Drive 0..100 = 0..36 dB.
inline constexpr FxPreset fxPresets[] = {
    // ---------------------------------------------------------------- Delay (type 1)
    { "Ambient", "Glacial Tape Wash", 1, { { "delayStyle", 1 }, { "p1", 78 }, { "p2", 82 }, { "mix", 48 }, { "bias", 22 } } },
    { "Ambient", "Dotted Eighth Haze", 1, { { "delayStyle", 2 }, { "sync", 1 }, { "division", 8 }, { "p2", 64 }, { "mix", 36 }, { "replica", 0 }, { "replicaBrown", 1 } } },
    { "Ambient", "Reverse Bloom", 1, { { "delayStyle", 0 }, { "p1", 56 }, { "p2", 48 }, { "mix", 58 }, { "diffusion", 70 } } },
    { "Ambient", "Shimmering Octaves", 1, { { "delayStyle", 3 }, { "sync", 1 }, { "division", 13 }, { "p2", 72 }, { "mix", 42 }, { "amSplice", 520 }, { "amReverse", 0 } } },
    { "Experimental", "Backwards Crystal", 1, { { "delayStyle", 3 }, { "p1", 34 }, { "p2", 80 }, { "mix", 65 }, { "amSplice", 180 }, { "amReverse", 1 } } },
    { "Experimental", "Smeared Reverse Triplets", 1, { { "delayStyle", 0 }, { "sync", 1 }, { "division", 9 }, { "p2", 62 }, { "mix", 50 }, { "diffusion", 85 } } },
    { "Experimental", "Wobbly Short Tape", 1, { { "delayStyle", 1 }, { "p1", 10 }, { "p2", 66 }, { "mix", 45 }, { "bias", 72 } } },
    { "Electronic", "Ping Pong Sixteenths", 1, { { "delayStyle", 2 }, { "sync", 1 }, { "division", 4 }, { "p2", 38 }, { "mix", 30 }, { "replica", 1 } } },
    { "Cinematic", "Slapback Whisper", 1, { { "delayStyle", 2 }, { "p1", 9 }, { "p2", 6 }, { "mix", 24 }, { "replica", 0 } } },
    { "Cinematic", "Trailer Pulse", 1, { { "delayStyle", 2 }, { "sync", 1 }, { "division", 7 }, { "p2", 42 }, { "mix", 32 }, { "replica", 1 } } },
    { "Cinematic", "Distant Reverse Echo", 1, { { "delayStyle", 0 }, { "sync", 1 }, { "division", 13 }, { "p2", 30 }, { "mix", 34 }, { "diffusion", 55 } } },
    { "Cinematic", "Dark Tape Room", 1, { { "delayStyle", 1 }, { "p1", 22 }, { "p2", 40 }, { "mix", 28 }, { "bias", 20 } } },
    { "Ambient", "Quarter Note Drift", 1, { { "delayStyle", 1 }, { "sync", 1 }, { "division", 10 }, { "p2", 55 }, { "mix", 38 }, { "bias", 35 } } },
    { "Ambient", "Half Note Memory", 1, { { "delayStyle", 2 }, { "sync", 1 }, { "division", 13 }, { "p2", 58 }, { "mix", 32 }, { "replica", 0 }, { "replicaBrown", 1 } } },
    { "Experimental", "Octave Up Whisper", 1, { { "delayStyle", 3 }, { "sync", 1 }, { "division", 10 }, { "p2", 36 }, { "mix", 30 }, { "amSplice", 420 }, { "amReverse", 0 } } },
    { "Electronic", "Eighth Note Clean", 1, { { "delayStyle", 2 }, { "sync", 1 }, { "division", 7 }, { "p2", 45 }, { "mix", 30 }, { "replica", 1 } } },
    { "Lo-Fi", "Worn Tape Echo", 1, { { "delayStyle", 1 }, { "p1", 38 }, { "p2", 50 }, { "mix", 36 }, { "bias", 15 } } },
    { "Rock", "Rockabilly Slap", 1, { { "delayStyle", 1 }, { "p1", 8 }, { "p2", 12 }, { "mix", 30 }, { "bias", 50 } } },

    // ---------------------------------------------------------------- EQ (type 2)
    { "Ambient", "Air and Space", 2, { { "p1", 44 }, { "p2", 42 }, { "p3", 58 }, { "eqAir", 74 }, { "eqLowCut", 60 }, { "eqMode", 0 } } },
    { "Ambient", "Distant Pad", 2, { { "p1", 46 }, { "p2", 36 }, { "p3", 32 }, { "eqAir", 28 }, { "eqLowCut", 90 }, { "eqHighCut", 7000 }, { "eqMode", 1 } } },
    { "Ambient", "Sub Bed", 2, { { "p1", 66 }, { "p2", 40 }, { "p3", 30 }, { "eqAir", 20 }, { "eqHighCut", 3200 }, { "eqMode", 0 } } },
    { "Experimental", "Telephone Ghost", 2, { { "p1", 20 }, { "p2", 70 }, { "p3", 30 }, { "eqAir", 10 }, { "eqLowCut", 500 }, { "eqHighCut", 3000 }, { "eqMode", 1 } } },
    { "Experimental", "Hollow Mids", 2, { { "p1", 58 }, { "p2", 12 }, { "p3", 60 }, { "eqAir", 62 }, { "eqMode", 1 } } },
    { "Experimental", "Radio Static Tilt", 2, { { "p1", 30 }, { "p2", 56 }, { "p3", 78 }, { "eqAir", 90 }, { "eqLowCut", 180 }, { "eqMode", 0 } } },
    { "Lo-Fi", "Cassette Dull", 2, { { "p1", 54 }, { "p2", 50 }, { "p3", 38 }, { "eqAir", 24 }, { "eqLowCut", 40 }, { "eqHighCut", 9000 }, { "eqMode", 0 } } },
    { "Electronic", "Mix Bus Smile", 2, { { "p1", 56 }, { "p2", 46 }, { "p3", 54 }, { "eqAir", 56 }, { "eqLowCut", 30 }, { "eqMode", 1 } } },
    { "Cinematic", "Orchestral Depth", 2, { { "p1", 54 }, { "p2", 46 }, { "p3", 52 }, { "eqAir", 60 }, { "eqLowCut", 35 }, { "eqMode", 0 } } },
    { "Cinematic", "Far Away", 2, { { "p1", 48 }, { "p2", 44 }, { "p3", 36 }, { "eqAir", 30 }, { "eqLowCut", 120 }, { "eqHighCut", 6000 }, { "eqMode", 1 } } },
    { "Cinematic", "Low Rumble Focus", 2, { { "p1", 62 }, { "p2", 44 }, { "p3", 40 }, { "eqAir", 34 }, { "eqHighCut", 5000 }, { "eqMode", 0 } } },
    { "Cinematic", "Clear Dialogue Space", 2, { { "p1", 42 }, { "p2", 54 }, { "p3", 50 }, { "eqAir", 52 }, { "eqLowCut", 150 }, { "eqMode", 1 } } },
    { "Ambient", "Gentle Tilt Up", 2, { { "p1", 46 }, { "p2", 48 }, { "p3", 54 }, { "eqAir", 60 }, { "eqLowCut", 45 }, { "eqMode", 0 } } },
    { "Ambient", "Warm Blanket", 2, { { "p1", 56 }, { "p2", 48 }, { "p3", 42 }, { "eqAir", 36 }, { "eqHighCut", 12000 }, { "eqMode", 0 } } },
    { "Experimental", "Nasal Band", 2, { { "p1", 36 }, { "p2", 62 }, { "p3", 38 }, { "eqAir", 30 }, { "eqLowCut", 300 }, { "eqHighCut", 5000 }, { "eqMode", 1 } } },
    { "Lo-Fi", "AM Radio", 2, { { "p1", 30 }, { "p2", 58 }, { "p3", 36 }, { "eqAir", 20 }, { "eqLowCut", 250 }, { "eqHighCut", 4500 }, { "eqMode", 0 } } },
    { "Electronic", "Sub Tight", 2, { { "p1", 56 }, { "p2", 48 }, { "p3", 52 }, { "eqAir", 50 }, { "eqLowCut", 32 }, { "eqMode", 1 } } },
    { "Rock", "Guitar Cab Shape", 2, { { "p1", 52 }, { "p2", 56 }, { "p3", 46 }, { "eqAir", 30 }, { "eqLowCut", 80 }, { "eqHighCut", 7000 }, { "eqMode", 0 } } },

    // ---------------------------------------------------------------- Drive (type 3)
    { "Ambient", "Warm Tube Glow", 3, { { "driveType", 0 }, { "p1", 22 }, { "p2", 42 }, { "p3", 50 }, { "mix", 70 } } },
    { "Ambient", "Tape Bloom", 3, { { "driveType", 2 }, { "p1", 36 }, { "p2", 38 }, { "p3", 52 }, { "mix", 100 } } },
    { "Ambient", "Parallel Triode Haze", 3, { { "driveType", 3 }, { "p1", 48 }, { "p2", 30 }, { "p3", 46 }, { "mix", 35 } } },
    { "Experimental", "Crushed Diodes", 3, { { "driveType", 1 }, { "p1", 68 }, { "p2", 58 }, { "p3", 42 }, { "mix", 80 } } },
    { "Experimental", "Molten Tape", 3, { { "driveType", 2 }, { "p1", 70 }, { "p2", 30 }, { "p3", 46 }, { "mix", 90 } } },
    { "Experimental", "Broken Transmission", 3, { { "driveType", 3 }, { "p1", 76 }, { "p2", 84 }, { "p3", 40 }, { "mix", 70 } } },
    { "Lo-Fi", "Dusty Preamp", 3, { { "driveType", 0 }, { "p1", 40 }, { "p2", 26 }, { "p3", 50 }, { "mix", 100 } } },
    { "Rock", "Fuzz Pedal", 3, { { "driveType", 1 }, { "p1", 72 }, { "p2", 48 }, { "p3", 42 }, { "mix", 100 } } },
    { "Cinematic", "Console Warmth", 3, { { "driveType", 0 }, { "p1", 16 }, { "p2", 48 }, { "p3", 50 }, { "mix", 100 } } },
    { "Cinematic", "Tape Print", 3, { { "driveType", 2 }, { "p1", 26 }, { "p2", 44 }, { "p3", 52 }, { "mix", 100 } } },
    { "Cinematic", "Grit Underneath", 3, { { "driveType", 3 }, { "p1", 42 }, { "p2", 36 }, { "p3", 48 }, { "mix", 30 } } },
    { "Cinematic", "Tension Edge", 3, { { "driveType", 1 }, { "p1", 46 }, { "p2", 62 }, { "p3", 46 }, { "mix", 45 } } },
    { "Ambient", "Soft Tube Shimmer", 3, { { "driveType", 0 }, { "p1", 30 }, { "p2", 62 }, { "p3", 50 }, { "mix", 50 } } },
    { "Ambient", "Tape Air", 3, { { "driveType", 2 }, { "p1", 20 }, { "p2", 70 }, { "p3", 52 }, { "mix", 60 } } },
    { "Experimental", "Half Broken Triode", 3, { { "driveType", 3 }, { "p1", 60 }, { "p2", 70 }, { "p3", 44 }, { "mix", 55 } } },
    { "Lo-Fi", "Dusty Tape", 3, { { "driveType", 2 }, { "p1", 48 }, { "p2", 22 }, { "p3", 48 }, { "mix", 100 } } },
    { "Electronic", "Clean Boost", 3, { { "driveType", 0 }, { "p1", 10 }, { "p2", 50 }, { "p3", 56 }, { "mix", 100 } } },
    { "Rock", "Crunch Amp", 3, { { "driveType", 3 }, { "p1", 58 }, { "p2", 46 }, { "p3", 44 }, { "mix", 100 } } },

    // ---------------------------------------------------------------- Reverb (type 4)
    { "Ambient", "Soft Shimmer", 4, { { "rvMode", 5 }, { "p1", 60 }, { "p2", 38 }, { "p3", 58 }, { "mix", 42 }, { "rvPreDelay", 40 }, { "rvLowCut", 140 }, { "rvChar", 55 } } },
    { "Ambient", "Cathedral Bloom", 4, { { "rvMode", 4 }, { "p1", 80 }, { "p2", 72 }, { "p3", 48 }, { "mix", 50 }, { "rvPreDelay", 20 }, { "rvLowCut", 90 }, { "rvChar", 70 } } },
    { "Ambient", "Open Field", 4, { { "rvMode", 3 }, { "p1", 64 }, { "p2", 58 }, { "p3", 52 }, { "mix", 42 }, { "rvPreDelay", 0 }, { "rvLowCut", 60 }, { "rvChar", 55 } } },
    { "Ambient", "Breathing Plate", 4, { { "rvMode", 1 }, { "p1", 58 }, { "p2", 62 }, { "p3", 56 }, { "mix", 38 }, { "rvPreDelay", 30 }, { "rvLowCut", 100 }, { "rvChar", 60 } } },
    { "Experimental", "Glass Shimmer", 4, { { "rvMode", 5 }, { "p1", 76 }, { "p2", 44 }, { "p3", 68 }, { "mix", 55 }, { "rvPreDelay", 0 }, { "rvLowCut", 200 }, { "rvChar", 80 } } },
    { "Experimental", "Slow Swell", 4, { { "rvMode", 4 }, { "p1", 50 }, { "p2", 60 }, { "p3", 42 }, { "mix", 70 }, { "rvPreDelay", 0 }, { "rvLowCut", 80 }, { "rvChar", 85 } } },
    { "Experimental", "Canyon Echoes", 4, { { "rvMode", 3 }, { "p1", 84 }, { "p2", 56 }, { "p3", 34 }, { "mix", 48 }, { "rvPreDelay", 120 }, { "rvLowCut", 150 }, { "rvChar", 85 } } },
    { "Cinematic", "Small Dark Room", 4, { { "rvMode", 0 }, { "p1", 30 }, { "p2", 25 }, { "p3", 35 }, { "mix", 22 }, { "rvPreDelay", 8 }, { "rvLowCut", 80 }, { "rvChar", 40 } } },
    { "Electronic", "Tight Ambience", 4, { { "rvMode", 2 }, { "p1", 40 }, { "p2", 30 }, { "p3", 55 }, { "mix", 28 }, { "rvPreDelay", 5 }, { "rvLowCut", 120 }, { "rvChar", 70 } } },
    { "Cinematic", "Concert Hall", 4, { { "rvMode", 4 }, { "p1", 66 }, { "p2", 48 }, { "p3", 50 }, { "mix", 36 }, { "rvPreDelay", 25 }, { "rvLowCut", 90 }, { "rvChar", 30 } } },
    { "Cinematic", "Scoring Stage", 4, { { "rvMode", 0 }, { "p1", 72 }, { "p2", 50 }, { "p3", 48 }, { "mix", 32 }, { "rvPreDelay", 18 }, { "rvLowCut", 70 }, { "rvChar", 55 } } },
    { "Cinematic", "Valley at Dusk", 4, { { "rvMode", 3 }, { "p1", 70 }, { "p2", 50 }, { "p3", 44 }, { "mix", 38 }, { "rvPreDelay", 30 }, { "rvLowCut", 100 }, { "rvChar", 45 } } },
    { "Cinematic", "Dark Plate", 4, { { "rvMode", 1 }, { "p1", 62 }, { "p2", 46 }, { "p3", 32 }, { "mix", 34 }, { "rvPreDelay", 40 }, { "rvLowCut", 120 }, { "rvChar", 40 } } },
    { "Cinematic", "Whisper of Shimmer", 4, { { "rvMode", 5 }, { "p1", 64 }, { "p2", 34 }, { "p3", 50 }, { "mix", 30 }, { "rvPreDelay", 60 }, { "rvLowCut", 160 }, { "rvChar", 35 } } },
    { "Ambient", "Morning Ambience", 4, { { "rvMode", 2 }, { "p1", 60 }, { "p2", 55 }, { "p3", 60 }, { "mix", 40 }, { "rvPreDelay", 10 }, { "rvLowCut", 90 }, { "rvChar", 60 } } },
    { "Ambient", "Warm Hall", 4, { { "rvMode", 4 }, { "p1", 70 }, { "p2", 55 }, { "p3", 40 }, { "mix", 42 }, { "rvPreDelay", 30 }, { "rvLowCut", 70 }, { "rvChar", 50 } } },
    { "Experimental", "Metal Plate Flutter", 4, { { "rvMode", 1 }, { "p1", 45 }, { "p2", 55 }, { "p3", 72 }, { "mix", 45 }, { "rvPreDelay", 0 }, { "rvLowCut", 200 }, { "rvChar", 90 } } },
    { "Lo-Fi", "Spring Box", 4, { { "rvMode", 1 }, { "p1", 36 }, { "p2", 40 }, { "p3", 38 }, { "mix", 30 }, { "rvPreDelay", 12 }, { "rvLowCut", 150 }, { "rvChar", 75 } } },
    { "Electronic", "Club Room", 4, { { "rvMode", 0 }, { "p1", 42 }, { "p2", 32 }, { "p3", 46 }, { "mix", 24 }, { "rvPreDelay", 10 }, { "rvLowCut", 110 }, { "rvChar", 50 } } },
    { "Rock", "Live Room", 4, { { "rvMode", 0 }, { "p1", 50 }, { "p2", 36 }, { "p3", 52 }, { "mix", 26 }, { "rvPreDelay", 12 }, { "rvLowCut", 90 }, { "rvChar", 60 } } },

    // ---------------------------------------------------------------- Chorus (type 5)
    { "Ambient", "Slow Ensemble Drift", 5, { { "chModel", 6 }, { "chMode", 1 }, { "p1", 18 }, { "p2", 60 }, { "p3", 100 }, { "mix", 50 }, { "chHQ", 1 } } },
    { "Ambient", "Liquid Deep", 5, { { "chModel", 2 }, { "chMode", 1 }, { "chVariant", 1 }, { "p1", 24 }, { "p2", 70 }, { "p3", 90 }, { "mix", 55 }, { "chHQ", 1 } } },
    { "Ambient", "Spatial Wide", 5, { { "chModel", 1 }, { "chMode", 6 }, { "chStereo", 1 }, { "p1", 30 }, { "p2", 50 }, { "p3", 100 }, { "mix", 45 }, { "chHQ", 1 } } },
    { "Ambient", "Sine Halo", 5, { { "chModel", 5 }, { "p1", 12 }, { "p2", 80 }, { "p3", 100 }, { "mix", 40 }, { "chDelay", 28 }, { "chFeedback", -20 }, { "chHQ", 1 } } },
    { "Experimental", "Seasick Vibrato", 5, { { "chModel", 0 }, { "chMode", 1 }, { "p1", 55 }, { "p2", 70 }, { "p3", 100 }, { "mix", 100 }, { "chVibRate", 30 }, { "chVibDepth", 60 } } },
    { "Experimental", "Pitch Smear", 5, { { "chModel", 3 }, { "chMode", 0 }, { "p1", 40 }, { "p2", 85 }, { "p3", 100 }, { "mix", 65 }, { "chFeedback", 60 }, { "chHQ", 1 } } },
    { "Experimental", "Eight Voice Cloud", 5, { { "chModel", 3 }, { "chMode", 2 }, { "chVoices", 8 }, { "p1", 22 }, { "p2", 75 }, { "p3", 100 }, { "mix", 60 }, { "chDelay", 40 }, { "chHQ", 1 } } },
    { "Electronic", "Poly Both", 5, { { "chModel", 4 }, { "chMode", 2 }, { "p1", 50 }, { "p2", 50 }, { "p3", 100 }, { "mix", 50 } } },
    { "Rock", "Classic Chorus", 5, { { "chModel", 0 }, { "chMode", 0 }, { "p1", 45 }, { "p2", 50 }, { "p3", 100 }, { "mix", 50 } } },
    { "Cinematic", "String Ensemble", 5, { { "chModel", 6 }, { "chMode", 1 }, { "p1", 30 }, { "p2", 45 }, { "p3", 100 }, { "mix", 45 }, { "chHQ", 1 } } },
    { "Cinematic", "Wide Pad Spread", 5, { { "chModel", 1 }, { "chMode", 4 }, { "chStereo", 1 }, { "p1", 25 }, { "p2", 40 }, { "p3", 100 }, { "mix", 40 }, { "chHQ", 1 } } },
    { "Cinematic", "Subtle Motion", 5, { { "chModel", 5 }, { "p1", 15 }, { "p2", 35 }, { "p3", 80 }, { "mix", 30 }, { "chDelay", 18 }, { "chHQ", 1 } } },
    { "Cinematic", "Detuned Layers", 5, { { "chModel", 3 }, { "chMode", 0 }, { "p1", 30 }, { "p2", 40 }, { "p3", 100 }, { "mix", 40 }, { "chHQ", 1 } } },
    { "Ambient", "Poly Breath", 5, { { "chModel", 4 }, { "chMode", 0 }, { "p1", 22 }, { "p2", 55 }, { "p3", 100 }, { "mix", 45 } } },
    { "Ambient", "Six Voice Haze", 5, { { "chModel", 3 }, { "chMode", 2 }, { "chVoices", 6 }, { "p1", 20 }, { "p2", 60 }, { "p3", 100 }, { "mix", 50 }, { "chDelay", 30 }, { "chHQ", 1 } } },
    { "Experimental", "Slow Liquid Swirl", 5, { { "chModel", 2 }, { "chMode", 0 }, { "p1", 12 }, { "p2", 85 }, { "p3", 100 }, { "mix", 60 }, { "chHQ", 1 } } },
    { "Lo-Fi", "Warbly Cassette", 5, { { "chModel", 0 }, { "chMode", 1 }, { "p1", 35 }, { "p2", 40 }, { "p3", 60 }, { "mix", 100 }, { "chVibRate", 45 }, { "chVibDepth", 25 } } },
    { "Electronic", "Dual Spatial", 5, { { "chModel", 1 }, { "chMode", 5 }, { "chStereo", 1 }, { "p1", 45 }, { "p2", 50 }, { "p3", 100 }, { "mix", 50 } } },
    { "Rock", "Juicy Stereo", 5, { { "chModel", 1 }, { "chMode", 3 }, { "p1", 50 }, { "p2", 55 }, { "p3", 100 }, { "mix", 50 } } },

    // ---------------------------------------------------------------- Compressor (type 6)
    { "Ambient", "Opto Glue", 6, { { "cpMode", 0 }, { "cpTime", 6 }, { "p1", 60 }, { "p2", 30 }, { "p3", 18 }, { "mix", 100 } } },
    { "Ambient", "Slow Swell Leveller", 6, { { "cpMode", 0 }, { "cpTime", 5 }, { "p1", 50 }, { "p2", 46 }, { "p3", 30 }, { "mix", 100 } } },
    { "Ambient", "Parallel Warmth", 6, { { "cpMode", 0 }, { "cpTime", 2 }, { "p1", 35 }, { "p2", 70 }, { "p3", 45 }, { "mix", 40 } } },
    { "Experimental", "Transient Softener", 6, { { "cpMode", 1 }, { "p1", 50 }, { "p2", 70 }, { "p3", 30 }, { "mix", 100 }, { "cpKnee", 3 }, { "cpAttack", 0.1f }, { "cpRelease", 40 } } },
    { "Electronic", "Transparent Peak Control", 6, { { "cpMode", 1 }, { "p1", 75 }, { "p2", 46 }, { "p3", 10 }, { "mix", 100 }, { "cpKnee", 6 }, { "cpAttack", 5 }, { "cpRelease", 120 } } },
    { "Cinematic", "Vari-Mu Mix Bus", 6, { { "cpMode", 0 }, { "cpTime", 3 }, { "p1", 70 }, { "p2", 24 }, { "p3", 8 }, { "mix", 100 } } },
    { "Cinematic", "Gentle Opto", 6, { { "cpMode", 0 }, { "cpTime", 6 }, { "p1", 66 }, { "p2", 28 }, { "p3", 12 }, { "mix", 100 } } },
    { "Cinematic", "Swell Control", 6, { { "cpMode", 0 }, { "cpTime", 4 }, { "p1", 55 }, { "p2", 40 }, { "p3", 22 }, { "mix", 100 } } },
    { "Cinematic", "Parallel Body", 6, { { "cpMode", 0 }, { "cpTime", 2 }, { "p1", 45 }, { "p2", 60 }, { "p3", 36 }, { "mix", 35 } } },
    { "Cinematic", "Transparent Rider", 6, { { "cpMode", 1 }, { "p1", 68 }, { "p2", 36 }, { "p3", 10 }, { "mix", 100 }, { "cpKnee", 12 }, { "cpAttack", 20 }, { "cpRelease", 400 } } },
    { "Ambient", "Breathing Room", 6, { { "cpMode", 1 }, { "p1", 55 }, { "p2", 50 }, { "p3", 22 }, { "mix", 100 }, { "cpKnee", 18 }, { "cpAttack", 60 }, { "cpRelease", 1200 } } },
    { "Ambient", "Tube Hug", 6, { { "cpMode", 0 }, { "cpTime", 1 }, { "p1", 58 }, { "p2", 42 }, { "p3", 20 }, { "mix", 70 } } },
    { "Experimental", "Slow Pump", 6, { { "cpMode", 1 }, { "p1", 42 }, { "p2", 72 }, { "p3", 36 }, { "mix", 100 }, { "cpKnee", 4 }, { "cpAttack", 2 }, { "cpRelease", 900 } } },
    { "Lo-Fi", "Squashed Cassette", 6, { { "cpMode", 0 }, { "cpTime", 2 }, { "p1", 40 }, { "p2", 66 }, { "p3", 40 }, { "mix", 100 } } },
    { "Electronic", "Punchy Modern", 6, { { "cpMode", 1 }, { "p1", 62 }, { "p2", 56 }, { "p3", 20 }, { "mix", 100 }, { "cpKnee", 4 }, { "cpAttack", 12 }, { "cpRelease", 90 } } },
    { "Rock", "Drum Bus Glue", 6, { { "cpMode", 0 }, { "cpTime", 2 }, { "p1", 60 }, { "p2", 46 }, { "p3", 24 }, { "mix", 100 } } },
};

inline constexpr std::size_t numFxPresets = sizeof (fxPresets) / sizeof (fxPresets[0]);

} // namespace rs
