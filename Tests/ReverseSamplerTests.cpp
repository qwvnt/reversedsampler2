// Reverse Sampler tests: the PEAK as one moment for all layers, After peak =
// Cut / Release / Freeze / Loop, Clear = reset, undo/redo, missing samples,
// background loading, de-clicked voice stops, sinc interpolation, a UI smoke
// test and a CPU benchmark (printed, not checked).
//
// Build: cmake -DRS_BUILD_TESTS=ON ..., target ReverseSamplerTests.
// Environment: RS_BENCH_ONLY=1 runs only the benchmark, RS_SKIP_BENCH=1 skips it,
// RS_SNAPSHOT_DIR=<dir> saves PNG snapshots of the UI, RS_DUMP=1 saves a freeze render.
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "SampleEditor.h"
#include "TimelineView.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <random>

static int passes = 0, failures = 0;
static constexpr double rate = 48000.0;
static constexpr int blockSize = 512;

#define CHECK(cond, msg) do { if (cond) ++passes; else { ++failures; std::cout << "  FAIL: " << msg << "  [" #cond "]\n"; } } while (false)

static juce::File tmp (const juce::String& name)
{
    return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ReverseSamplerTests").getChildFile (name);
}

static void writeTone (const juce::File& file, double freq, double seconds, float amp, double fileRate = rate, int channels = 1)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    const auto n = (int) std::llround (seconds * fileRate);
    juce::AudioBuffer<float> b (channels, n);

    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < n; ++i)
            b.setSample (c, i, amp * (float) std::sin (juce::MathConstants<double>::twoPi * freq * i / fileRate));

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (file.createOutputStream().release());
    auto writer = wav.createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (fileRate).withNumChannels (channels).withBitsPerSample (24));
    jassert (writer != nullptr);
    writer->writeFromAudioSampleBuffer (b, 0, n);
}

static void setParam (ReverseSamplerProcessor& p, const juce::String& id, float value)
{
    auto* param = p.getValueTreeState().getParameter (id);
    jassert (param != nullptr);
    param->setValueNotifyingHost (param->convertTo0to1 (value));
}

static juce::String L (int layer, const char* name)   { return ParamIDs::layerID (layer, name); }

struct Scene
{
    double offset1 = 0.0, offset2 = 0.0, cut = 0.0;
    int afterPeak = 0;      // 0 Cut, 1 Release, 2 Freeze, 3 Loop
    int mode = 0;           // 0 Swell, 1 Arrive
    float fade = 1.5f;
    int timeMode = 1;       // 1 Resample (exact), 0 Stretch
    int pitch2 = 0;
};

static std::unique_ptr<ReverseSamplerProcessor> makeProcessor (const Scene& s, const juce::File& f1, const juce::File& f2)
{
    auto p = std::make_unique<ReverseSamplerProcessor>();
    p->setPlayConfigDetails (0, 2, rate, blockSize);
    p->prepareToPlay (rate, blockSize);
    p->loadSample (0, f1);
    p->loadSample (1, f2);
    setParam (*p, L (0, ParamIDs::peakOffset), (float) s.offset1);
    setParam (*p, L (1, ParamIDs::peakOffset), (float) s.offset2);
    setParam (*p, L (0, ParamIDs::timeMode), (float) s.timeMode);
    setParam (*p, L (1, ParamIDs::timeMode), (float) s.timeMode);
    setParam (*p, L (1, ParamIDs::pitch), (float) s.pitch2);
    setParam (*p, ParamIDs::peakCut, (float) s.cut);
    setParam (*p, ParamIDs::afterPeak, (float) s.afterPeak);
    setParam (*p, ParamIDs::playMode, (float) s.mode);
    setParam (*p, ParamIDs::dissolve, s.fade);
    setParam (*p, ParamIDs::attack, 0.0f);
    setParam (*p, ParamIDs::release, 5.0f);
    p->updateLatency();
    return p;
}

/** Renders the left channel; the key goes down at 0 and up at noteOff (seconds). */
static std::vector<float> render (ReverseSamplerProcessor& p, double seconds, double noteOff, int note = 60)
{
    const auto total = (int) std::llround (seconds * rate);
    const auto offAt = (juce::int64) std::llround (noteOff * rate);
    std::vector<float> out;
    out.reserve ((size_t) total);
    juce::AudioBuffer<float> buffer (2, blockSize);

    for (juce::int64 pos = 0; pos < total; pos += blockSize)
    {
        juce::MidiBuffer midi;

        if (pos == 0)
            midi.addEvent (juce::MidiMessage::noteOn (1, note, 1.0f), 0);

        if (offAt >= pos && offAt < pos + blockSize)
            midi.addEvent (juce::MidiMessage::noteOff (1, note), (int) (offAt - pos));

        buffer.clear();
        p.processBlock (buffer, midi);

        for (int i = 0; i < blockSize && pos + i < total; ++i)
            out.push_back (buffer.getSample (0, i));
    }

    return out;
}

/** Hann-windowed Goertzel amplitude of one frequency, in a window centred at t (seconds). */
static double level (const std::vector<float>& x, double freq, double t, double window = 0.04)
{
    const auto n = (int) (window * rate);
    const auto start = (int) std::llround (t * rate) - n / 2;
    const auto w = juce::MathConstants<double>::twoPi * freq / rate;
    double re = 0.0, im = 0.0, sumW = 0.0;

    for (int i = 0; i < n; ++i)
    {
        const auto k = start + i;
        const auto v = k >= 0 && k < (int) x.size() ? (double) x[(size_t) k] : 0.0;
        const auto hann = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / n);
        re += v * hann * std::cos (w * k);
        im -= v * hann * std::sin (w * k);
        sumW += hann;
    }

    return 2.0 * std::sqrt (re * re + im * im) / sumW;
}

/** The largest second difference between t0 and t1: a click shows up as a spike. */
static double roughness (const std::vector<float>& x, double t0, double t1)
{
    double m = 0.0;
    const auto a = juce::jmax ((size_t) 2, (size_t) (t0 * rate)), b = juce::jmin (x.size(), (size_t) (t1 * rate));

    for (auto i = a; i < b; ++i)
        m = juce::jmax (m, std::abs ((double) x[i] - 2.0 * x[i - 1] + x[i - 2]));

    return m;
}

static double rms (const std::vector<float>& x, double t0, double t1)
{
    double s = 0.0;
    const auto a = (size_t) juce::jmax (0.0, t0 * rate), b = juce::jmin (x.size(), (size_t) (t1 * rate));

    for (auto i = a; i < b; ++i)
        s += (double) x[i] * x[i];

    return b > a ? std::sqrt (s / (double) (b - a)) : 0.0;
}

static bool allFinite (const std::vector<float>& x)
{
    for (auto v : x)
        if (! std::isfinite (v) || std::abs (v) > 8.0f)
            return false;

    return true;
}

static constexpr double f1 = 440.0, f2 = 1000.0;
static constexpr double on = 0.05, off = 0.004;   // "sounds" / "silent" thresholds (the tones are 0.5)

//==============================================================================
static void testLayout (const juce::File& a, const juce::File& b)
{
    std::cout << "layout\n";
    auto near = [] (double samples, double seconds) { return std::abs (samples / rate - seconds) < 0.002; };

    {
        auto p = makeProcessor ({}, a, b);
        const auto n = p->getNoteLayout (60);
        CHECK (near (n.peak, 8.0), "offsets 0: PEAK at 8 s");
        CHECK (near (n.layers[0].start, 4.0) && n.layers[0].reachesPeak, "offsets 0: L1 starts at 4 s, reaches the PEAK");
        CHECK (near (n.layers[1].start, 0.0) && n.layers[1].reachesPeak, "offsets 0: L2 starts at 0");
    }
    {
        Scene s; s.offset1 = -3.0;
        auto p = makeProcessor (s, a, b);
        const auto n = p->getNoteLayout (60);
        CHECK (near (n.peak, 8.0), "L1 -3 s: PEAK stays at 8 s");
        CHECK (near (n.layers[0].start, 1.0) && near (n.layers[0].getEnd(), 5.0), "L1 -3 s: L1 from 1 to 5 s");
        CHECK (n.layers[0].active && ! n.layers[0].reachesPeak, "L1 -3 s: L1 active, ends before the PEAK");
        CHECK (near (n.layers[0].length, 4.0), "L1 -3 s: L1 plays whole");
    }
    {
        Scene s; s.offset1 = -3.0; s.cut = 2.0;
        auto p = makeProcessor (s, a, b);
        const auto n = p->getNoteLayout (60);
        CHECK (near (n.peak, 6.0), "cut 2 s: PEAK at 6 s");
        CHECK (near (n.layers[0].start, 1.0), "cut 2 s: L1 start unchanged");
        CHECK (near (n.layers[1].length, 6.0) && n.layers[1].reachesPeak, "cut 2 s: L2 plays 6 s");
        CHECK (near (n.end, 8.0), "cut 2 s: layout end 8 s");
    }
    {
        Scene s; s.offset1 = -3.0; s.cut = 4.5;
        auto p = makeProcessor (s, a, b);
        const auto n = p->getNoteLayout (60);
        CHECK (near (n.peak, 3.5) && near (n.layers[0].length, 2.5) && n.layers[0].reachesPeak, "cut 4.5 s: L1 cut at the PEAK 3.5 s");
    }
    {
        Scene s; s.offset1 = 1.0;
        auto p = makeProcessor (s, a, b);
        const auto n = p->getNoteLayout (60);
        CHECK (near (n.peak, 8.0) && near (n.layers[0].start, 5.0) && near (n.layers[0].length, 3.0), "L1 +1 s: plays 5..8 s");
        CHECK (n.layers[0].reachesPeak && near (n.end, 9.0), "L1 +1 s: build ends at 9 s");
    }
    {
        Scene s; s.offset1 = -3.0; s.cut = 7.5;
        auto p = makeProcessor (s, a, b);
        const auto n = p->getNoteLayout (60);
        CHECK (near (n.peak, 0.5) && n.layers[0].cutAway && ! n.layers[0].active && n.layers[1].active, "cut 7.5 s: L1 lies after the PEAK");
    }
    {
        Scene s; s.offset1 = -3.0; s.cut = 2.0; s.mode = 1;
        auto p = makeProcessor (s, a, b);
        const auto n = p->getNoteLayout (60);
        CHECK (std::abs (p->getArriveLatency() - 1.0 * rate) < 2.0, "Arrive: latency capped at 1 s");
        CHECK (near (n.peak, 1.0) && near (n.layers[0].start, -4.0) && near (n.layers[1].start, -5.0), "Arrive: long build is trimmed to stay within the latency cap");
    }
}

//==============================================================================
static void testCut (const juce::File& a, const juce::File& b)
{
    std::cout << "cut\n";
    {
        Scene s; s.offset1 = -3.0;   // L1 1..5 s, L2 0..8 s, PEAK 8 s
        auto p = makeProcessor (s, a, b);
        const auto x = render (*p, 9.5, 9.0);
        CHECK (allFinite (x), "finite");
        CHECK (level (x, f1, 0.5) < off && level (x, f1, 3.0) > on && level (x, f1, 5.5) < off, "L1 sounds 1..5 s only");
        CHECK (level (x, f2, 7.9) > on && level (x, f2, 8.2) < off, "L2 stops at the PEAK");
    }
    {
        Scene s; s.offset1 = 1.0;    // L1 5..9 s, cut at the PEAK 8 s
        auto p = makeProcessor (s, a, b);
        const auto x = render (*p, 9.5, 9.4);
        CHECK (level (x, f1, 7.9) > on && level (x, f1, 8.2) < off && level (x, f1, 8.8) < off, "positive offset: L1 cut at the PEAK");
    }
    {
        Scene s; s.offset1 = 1.0; s.cut = 0.5;   // PEAK 7.5 s: both layers cut in the middle of the sound
        auto p = makeProcessor (s, a, b);
        const auto x = render (*p, 9.0, 8.5);
        const auto edge = roughness (x, 7.4, 7.6), steady = roughness (x, 7.0, 7.3);
        std::cout << "    cut edge roughness " << edge << " / steady " << steady << "\n";
        CHECK (edge < 3.0 * steady + 1.0e-4, "cut at the PEAK without a click");
    }
    {
        Scene s; s.cut = 2.0;        // PEAK 6 s, both cut there
        auto p = makeProcessor (s, a, b);
        const auto x = render (*p, 8.0, 7.5);
        CHECK (level (x, f1, 5.9) > on && level (x, f2, 5.9) > on, "moved peak: both sound before it");
        CHECK (level (x, f1, 6.2) < off && level (x, f2, 6.2) < off, "moved peak: both stop at it");
    }
}

//==============================================================================
static void testFreeze (const juce::File& a, const juce::File& b, int timeMode)
{
    std::cout << "freeze (" << (timeMode == 0 ? "stretch" : "resample") << ")\n";
    {
        Scene s; s.offset1 = -3.0; s.afterPeak = 2; s.timeMode = timeMode;   // L1 ends at 5 s, PEAK 8 s
        auto p = makeProcessor (s, a, b);
        const auto x = render (*p, 11.0, 10.5);
        CHECK (allFinite (x), "finite");
        CHECK (level (x, f1, 6.0) < off && level (x, f1, 9.0) < off, "layer that ended before the PEAK stays silent");
        CHECK (level (x, f2, 7.0) > on, "L2 before the PEAK");
        CHECK (level (x, f2, 8.5) > on && level (x, f2, 10.0) > on, "L2 frozen after the PEAK while held");
        if (std::getenv ("RS_DUMP") != nullptr)
        {
            juce::AudioBuffer<float> dump (1, (int) x.size());
            dump.copyFrom (0, 0, x.data(), (int) x.size());
            auto file = tmp ("freeze_dump.wav");
            file.deleteFile();
            std::unique_ptr<juce::OutputStream> os (file.createOutputStream().release());
            auto writer = juce::WavAudioFormat().createWriterFor (os, juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (1).withBitsPerSample (32));
            writer->writeFromAudioSampleBuffer (dump, 0, dump.getNumSamples());
        }

        const auto held = rms (x, 8.2, 10.4), before = rms (x, 7.0, 7.9);
        const auto edge = roughness (x, 7.9, 8.2), steady = roughness (x, 7.0, 7.5);
        std::cout << "    freeze rms " << held << " / before " << before << ", edge roughness " << edge << " / steady " << steady
                  << ", levels 8.5 s " << level (x, f2, 8.5) << " 9.5 s " << level (x, f2, 9.5) << " 7.8 s " << level (x, f2, 7.8) << "\n";
        CHECK (held > 0.5 * before && held < 1.6 * before, "freeze level close to the PEAK level");
        CHECK (edge < 3.0 * steady + 1.0e-4, "freeze starts without a click");
        CHECK (rms (x, 10.8, 11.0) < 1.0e-4, "released: silent");
    }
    {
        Scene s; s.offset1 = 1.0; s.cut = 1.0; s.afterPeak = 2; s.timeMode = timeMode;   // PEAK 7 s; L1 5..9, L2 0..8
        auto p = makeProcessor (s, a, b);
        const auto x = render (*p, 10.0, 9.5);
        CHECK (level (x, f1, 6.9) > on && level (x, f2, 6.9) > on, "both sound before the PEAK");
        CHECK (level (x, f1, 7.5) > on && level (x, f1, 9.2) > on, "L1 frozen at the PEAK (not at its own end)");
        CHECK (level (x, f2, 7.5) > on && level (x, f2, 9.2) > on, "L2 frozen at the PEAK");
        const auto edge = roughness (x, 6.9, 7.2), steady = roughness (x, 6.0, 6.5);
        std::cout << "    freeze both: rms " << rms (x, 7.2, 9.4) << " / before " << rms (x, 6.0, 6.9) << ", edge " << edge << " / steady " << steady << "\n";
        CHECK (rms (x, 7.2, 9.4) > 0.5 * rms (x, 6.0, 6.9) && rms (x, 7.2, 9.4) < 1.6 * rms (x, 6.0, 6.9), "both: freeze level close to the PEAK level");
        CHECK (edge < 3.0 * steady + 1.0e-4, "both freeze without a click");
    }
}

//==============================================================================
static void testLoop (const juce::File& a, const juce::File& b)
{
    std::cout << "loop\n";
    {
        Scene s; s.offset1 = -3.0; s.cut = 2.0; s.afterPeak = 3;   // cycle 6 s: L1 1..5, L2 0..6
        auto p = makeProcessor (s, a, b);
        const auto x = render (*p, 16.0, 15.0);
        CHECK (allFinite (x), "finite");
        CHECK (level (x, f2, 5.9) > on && level (x, f2, 6.2) > on && level (x, f2, 11.0) > on, "L2 restarts at the PEAK");
        CHECK (level (x, f1, 5.5) < off && level (x, f1, 6.5) < off && level (x, f1, 7.5) > on && level (x, f1, 11.5) < off
               && level (x, f1, 13.5) > on, "L1 restarts with the cycle (1 s after each PEAK)");
    }
    {
        Scene s; s.offset1 = 1.0; s.afterPeak = 3;   // cycle 8 s: L1 5..8, L2 0..8
        auto p = makeProcessor (s, a, b);
        const auto x = render (*p, 15.0, 14.5);
        CHECK (level (x, f1, 7.9) > on && level (x, f1, 8.5) < off && level (x, f1, 12.0) < off && level (x, f1, 13.5) > on,
               "L1 stops at the PEAK and comes back 5 s into the next cycle");
        CHECK (level (x, f2, 8.5) > on, "L2 restarts at the PEAK");
    }
}

//==============================================================================
static void testRelease (const juce::File& a, const juce::File& b)
{
    std::cout << "release\n";
    const auto curve = rs::Envelope::dissolveDefault();
    {
        Scene s; s.cut = 2.0; s.afterPeak = 1; s.fade = 1.5f;   // PEAK 6 s, both builds go on to 8 s
        auto p = makeProcessor (s, a, b);
        const auto x = render (*p, 9.0, 8.8);
        CHECK (allFinite (x), "finite");
        const auto before = level (x, f1, 5.95, 0.02), mid = level (x, f1, 6.75, 0.02);
        CHECK (std::abs (mid / before - curve.valueAt (0.5f)) < 0.06, "L1 follows the fade curve (half-way: " + juce::String (mid / before, 3) + ")");
        CHECK (level (x, f2, 6.4) > on, "L2 plays on after the PEAK");
        CHECK (rms (x, 7.55, 9.0) < 1.0e-4, "silent after PEAK + fade");
        CHECK (roughness (x, 5.9, 6.2) < 1.5 * roughness (x, 5.0, 5.5) + 1.0e-4, "no click at the PEAK");
    }
    {
        Scene s; s.offset1 = 1.0; s.afterPeak = 1; s.fade = 3.0f;   // PEAK 8 s; L1 goes on to 9 s, L2 ends at 8 s
        auto p = makeProcessor (s, a, b);
        const auto x = render (*p, 10.0, 9.8);
        CHECK (level (x, f1, 8.5) > 0.3 * level (x, f1, 7.9) && level (x, f1, 8.5) < level (x, f1, 7.9), "L1 fades after the PEAK");
        CHECK (rms (x, 9.05, 10.0) < 1.0e-4, "the tail ends with the material");
    }
    {
        Scene s; s.cut = 2.0; s.afterPeak = 1; s.fade = 1.0f;   // flat curve: plays on unchanged to the end (8 s)
        auto p = makeProcessor (s, a, b);
        p->setEnvelope (ReverseSamplerProcessor::dissolveEnvelope, rs::Envelope());
        const auto x = render (*p, 9.0, 8.8);
        CHECK (std::abs (level (x, f1, 7.5) / level (x, f1, 5.5) - 1.0) < 0.05, "flat curve: full level after the PEAK");
        CHECK (rms (x, 8.05, 9.0) < 1.0e-4, "flat curve: ends with the material");
    }
    {
        Scene s; s.cut = 2.0; s.afterPeak = 1; s.fade = 1.5f;   // the key let go before the PEAK: the normal release
        auto p = makeProcessor (s, a, b);
        const auto x = render (*p, 7.0, 3.0);
        CHECK (rms (x, 3.1, 7.0) < 1.0e-4, "early note-off still stops the note");
    }
}

//==============================================================================
// A layer moved earlier than the note start: its start is trimmed, the PEAK and the other layer stay.
static void testTrim (const juce::File& a, const juce::File& b)
{
    std::cout << "trim\n";
    auto near = [] (double samples, double seconds) { return std::abs (samples / rate - seconds) < 0.002; };
    {
        Scene s; s.offset2 = -2.0;
        auto p = makeProcessor (s, a, b);
        const auto n = p->getNoteLayout (60);
        CHECK (near (n.peak, 8.0), "L2 -2 s: PEAK stays at 8 s");
        CHECK (near (n.layers[1].start, -2.0) && near (n.layers[1].getTrim(), 2.0) && near (n.layers[1].getEnd(), 6.0), "L2 -2 s: start trimmed 2 s, ends at 6 s");
        CHECK (n.layers[1].active && ! n.layers[1].reachesPeak, "L2 -2 s: active, ends before the PEAK");
        CHECK (near (n.layers[0].start, 4.0), "L2 -2 s: L1 stays at 4 s");
        CHECK (near (n.buildStart, 0.0), "L2 -2 s: the final envelope starts at the note start");

        const auto x = render (*p, 9.0, 8.5);
        CHECK (allFinite (x), "finite");
        CHECK (level (x, f2, 0.1) > on && level (x, f2, 5.9) > on && level (x, f2, 6.3) < off, "L2 plays from the key to 6 s");
        CHECK (level (x, f1, 3.5) < off && level (x, f1, 4.5) > on && level (x, f1, 7.9) > on && level (x, f1, 8.2) < off, "L1 unchanged: 4..8 s");
        CHECK (roughness (x, 0.0, 0.03) < 3.0 * roughness (x, 1.0, 1.5) + 1.0e-4, "the trimmed start does not click");
    }
    {
        Scene s; s.offset2 = -2.0; s.timeMode = 0; s.pitch2 = 3;   // through the stretcher
        auto p = makeProcessor (s, a, b);
        const auto x = render (*p, 9.0, 8.5);
        const auto f = f2 * std::pow (2.0, 3.0 / 12.0);
        CHECK (allFinite (x), "stretch: finite");
        CHECK (level (x, f, 0.2) > on && level (x, f, 5.8) > on && level (x, f, 6.4) < off, "stretch: L2 trimmed, plays to 6 s");
    }
    {
        Scene s; s.offset2 = -8.0;   // the whole build before the note start
        auto p = makeProcessor (s, a, b);
        const auto n = p->getNoteLayout (60);
        CHECK (! n.layers[1].active && n.layers[1].cutAway && near (n.peak, 8.0) && near (n.layers[0].start, 4.0), "L2 -8 s: silent, the rest stays");
        const auto x = render (*p, 9.0, 8.5);
        CHECK (level (x, f2, 0.5) < off && level (x, f1, 5.0) > on, "L2 -8 s: only L1 plays");
    }
    {
        Scene s; s.offset2 = 1.0;    // moved later: as before, the earliest build still starts with the key
        auto p = makeProcessor (s, a, b);
        const auto n = p->getNoteLayout (60);
        CHECK (near (n.layers[1].start, 0.0) && near (n.peak, 7.0) && near (n.layers[0].start, 3.0), "L2 +1 s: unchanged behaviour");
    }
    {
        Scene s; s.offset2 = -2.0; s.mode = 1;
        auto p = makeProcessor (s, a, b);
        CHECK (std::abs (p->getArriveLatency() - 1.0 * rate) < 2.0, "Arrive: earlier layer offset still respects the 1 s cap");
        CHECK (near (p->getNoteLayout (60).layers[1].start, -9.0), "Arrive: long shifted layer is trimmed at the start");
    }
    {
        Scene s; s.offset2 = -2.0; s.afterPeak = 3;   // Loop: every cycle starts trimmed
        auto p = makeProcessor (s, a, b);
        const auto x = render (*p, 14.0, 13.5);
        CHECK (level (x, f2, 8.1) > on && level (x, f2, 14.3 - 0.5) < off, "Loop: L2 restarts at the PEAK, trimmed");
        CHECK (level (x, f1, 11.0) < off && level (x, f1, 12.5) > on, "Loop: L1 4 s into the next cycle");
    }
}

//==============================================================================
static void testState (const juce::File& a, const juce::File& b)
{
    std::cout << "state\n";
    Scene s; s.afterPeak = 1; s.fade = 4.0f; s.offset1 = -1.25; s.cut = 0.5;
    auto p = makeProcessor (s, a, b);
    auto* choice = dynamic_cast<juce::AudioParameterChoice*> (p->getValueTreeState().getParameter (ParamIDs::afterPeak));
    CHECK (choice != nullptr && choice->choices.joinIntoString (",") == "Cut,Release,Freeze,Loop", "After Peak choices");
    CHECK (p->getValueTreeState().getParameter (ParamIDs::dissolve)->getName (64) == "After Peak Fade", "fade parameter name");
    CHECK (p->getAfterPeak() == rs::AfterPeak::release, "Release selected");

    juce::MemoryBlock state;
    p->getStateInformation (state);
    auto q = std::make_unique<ReverseSamplerProcessor>();
    q->setPlayConfigDetails (0, 2, rate, blockSize);
    q->prepareToPlay (rate, blockSize);
    q->setStateInformation (state.getData(), (int) state.getSize());
    CHECK (q->getAfterPeak() == rs::AfterPeak::release && std::abs (q->getDissolveSeconds() - 4.0) < 0.01, "state restores Release and Fade");
    CHECK (std::abs (q->getLayerSettings (0).peakOffset + 1.25) < 0.001 && std::abs (q->getPeakCutSeconds() - 0.5) < 0.001, "state restores offset and Peak Move");

    // On/Reverse switches: an odd host value (0.37) is stored as a clean 0/1, so restoring the
    // same state gives the same value whatever the value was before (pluginval test).
    for (auto* id : { ParamIDs::on, ParamIDs::reverse })
    {
        auto* sw = q->getValueTreeState().getParameter (L (0, id));
        const auto hostSet = [sw] (float v) { sw->setValue (v); sw->sendValueChangedMessageToListeners (v); };   // like a plug-in wrapper
        const auto is = [sw] (float v) { return std::abs (sw->getValue() - v) < 1.0e-6f; };
        CHECK (sw != nullptr && sw->isBoolean() && sw->isDiscrete(), "layer switch is boolean");
        hostSet (0.37f);
        CHECK (is (0.0f), "switch snaps 0.37 to Off");
        juce::MemoryBlock saved;
        q->getStateInformation (saved);
        hostSet (0.77f);
        CHECK (is (1.0f), "switch snaps 0.77 to On");
        q->setStateInformation (saved.getData(), (int) saved.getSize());
        CHECK (is (0.0f), "switch restores Off from state");
        CHECK (sw->getText (1.0f, 16) == "On" && sw->getValueForText ("off") < 0.5f && sw->getValueForText ("On") > 0.5f, "switch texts");
        sw->setValueNotifyingHost (sw->getDefaultValue());
    }
}

//==============================================================================
static void testRandom (const juce::File& a, const juce::File& b)
{
    std::cout << "random\n";
    std::mt19937 rng (41);
    std::uniform_real_distribution<double> u (0.0, 1.0);
    int bad = 0;

    for (int run = 0; run < 24; ++run)
    {
        Scene s;
        s.offset1 = (u (rng) - 0.5) * 8.0;
        s.offset2 = (u (rng) - 0.5) * 4.0;
        s.cut = u (rng) < 0.3 ? 0.0 : u (rng) * 6.0;
        s.afterPeak = run % 4;
        s.mode = (run / 4) % 2;
        s.timeMode = (run / 8) % 2;
        s.fade = (float) (0.2 + u (rng) * 3.0);
        auto p = makeProcessor (s, a, b);
        const auto x = render (*p, 6.0, 1.0 + u (rng) * 4.0, 55 + run % 10);

        if (! allFinite (x))
            ++bad;
    }

    CHECK (bad == 0, "random scenes stay finite");
}

//==============================================================================
static void snapshot (juce::Component& c, const juce::String& name)
{
    const auto image = c.createComponentSnapshot (c.getLocalBounds());   // paints the component in any case
    const auto* dir = std::getenv ("RS_SNAPSHOT_DIR");

    if (dir == nullptr)
        return;

    auto file = juce::File::getCurrentWorkingDirectory().getChildFile (dir).getChildFile (name + ".png");
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    juce::FileOutputStream os (file);
    juce::PNGImageFormat().writeImageToStream (image, os);
}

static void testUiTrim (const juce::File& a, const juce::File& b)
{
    Scene s; s.offset2 = -2.0;
    auto p = makeProcessor (s, a, b);
    std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditor());
    auto* e = dynamic_cast<ReverseSamplerEditor*> (editor.get());

    if (e == nullptr)
        return;

    juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
    snapshot (*e, "main_trimmed");
    e->showSampleEditor();
    juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
    snapshot (*e, "sample_editor_trimmed");

    for (auto* child : e->getChildren())
        if (auto* se = dynamic_cast<SampleEditor*> (child))
        {
            const auto text = se->getTimeline().describe();
            std::cout << "    trimmed: " << text << "\n";
            CHECK (text.contains ("start trimmed 2.00 s"), "describe shows the trimmed start");
        }

    e->showLayerEditor (1);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
    snapshot (*e, "layer_editor_trimmed");
}

static void testUi (const juce::File& a, const juce::File& b)
{
    std::cout << "ui\n";
    for (int mode = 0; mode < 4; ++mode)
    {
        Scene s; s.offset1 = mode == 2 ? -3.0 : 1.0; s.cut = mode == 1 ? 2.0 : 1.0; s.afterPeak = mode; s.fade = 1.5f;
        auto p = makeProcessor (s, a, b);
        std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditor());
        auto* e = dynamic_cast<ReverseSamplerEditor*> (editor.get());
        CHECK (e != nullptr, "editor");

        if (e == nullptr)
            return;

        e->showSampleEditor();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
        static const char* names[] = { "cut", "release", "freeze", "loop" };
        snapshot (*e, juce::String ("sample_editor_") + names[mode]);

        // The timeline's text.
        SampleEditor* se = nullptr;

        for (auto* child : e->getChildren())
            if (auto* found = dynamic_cast<SampleEditor*> (child))
                se = found;

        CHECK (se != nullptr, "sample editor open");

        if (se != nullptr)
        {
            const auto text = se->getTimeline().describe();
            std::cout << "    " << names[mode] << ": " << text << "\n";

            if (mode == 2)
                CHECK (text.contains ("ends before the PEAK"), "describe mentions a layer ending before the PEAK");
        }

        if (mode == 2)
        {
            e->showLayerEditor (0);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (400);
            snapshot (*e, "layer_editor_freeze");
        }

        editor.reset();
    }
}

//==============================================================================

//==============================================================================
static void gesture (ReverseSamplerProcessor& p, const juce::String& id, float value)
{
    auto* param = p.getValueTreeState().getParameter (id);
    param->beginChangeGesture();
    param->setValueNotifyingHost (param->convertTo0to1 (value));
    param->endChangeGesture();
}

static float value (ReverseSamplerProcessor& p, const juce::String& id)
{
    auto* param = p.getValueTreeState().getParameter (id);
    return param->convertFrom0to1 (param->getValue());
}

static void testResetLayer (const juce::File& a, const juce::File& b)
{
    std::cout << "reset layer\n";
    Scene s; s.offset1 = -1.25; s.offset2 = 0.5;
    auto p = makeProcessor (s, a, b);
    setParam (*p, L (0, ParamIDs::pitch), 5.0f);
    setParam (*p, L (0, ParamIDs::level), -6.0f);
    setParam (*p, L (0, ParamIDs::pan), -30.0f);
    setParam (*p, L (0, ParamIDs::reverse), 1.0f);
    setParam (*p, L (0, ParamIDs::root), 48.0f);
    setParam (*p, L (0, ParamIDs::start), 10.0f);
    setParam (*p, L (0, ParamIDs::lowCut), 200.0f);
    setParam (*p, L (0, ParamIDs::on), 0.0f);
    rs::Envelope e; e.y[0] = 0.2f;
    p->setEnvelope (0, e);
    p->commitPendingEdit();

    const auto sample0 = p->getSample (0);
    CHECK (! p->isLayerDefault (0) && ! p->isLayerDefault (1), "layers are not default before Clear");

    p->resetLayer (0);
    p->commitPendingEdit();

    bool allDefault = true;

    for (const auto* name : ParamIDs::layerParameters)
    {
        auto* param = p->getValueTreeState().getParameter (L (0, name));
        allDefault = allDefault && std::abs (param->getValue() - param->getDefaultValue()) < 1.0e-6f;
    }

    CHECK (p->getSample (0) == nullptr, "Clear removes the sample");
    CHECK (allDefault, "Clear resets every setting of the layer");
    CHECK (p->getEnvelope (0).isFlat(), "Clear resets the layer envelope");
    CHECK (p->isLayerDefault (0), "the layer is default after Clear");
    CHECK (p->getSample (1) != nullptr && std::abs (value (*p, L (1, ParamIDs::peakOffset)) - 0.5f) < 1.0e-4f, "the other layer is untouched");

    CHECK (p->undo(), "Clear can be undone");
    CHECK (p->getSample (0) == sample0, "undo brings the same sample back");
    CHECK (std::abs (value (*p, L (0, ParamIDs::pitch)) - 5.0f) < 1.0e-4f && std::abs (value (*p, L (0, ParamIDs::peakOffset)) + 1.25f) < 1.0e-3f
           && value (*p, L (0, ParamIDs::reverse)) > 0.5f && std::abs (value (*p, L (0, ParamIDs::root)) - 48.0f) < 0.01f,
           "undo brings the settings back");
    CHECK (std::abs (p->getEnvelope (0).y[0] - 0.2f) < 1.0e-6f, "undo brings the envelope back");

    CHECK (p->redo(), "Clear can be redone");
    CHECK (p->isLayerDefault (0), "redo clears the layer again");
    CHECK (p->undo(), "undo again");

    setParam (*p, L (0, ParamIDs::on), 1.0f);
    setParam (*p, L (0, ParamIDs::reverse), 0.0f);
    setParam (*p, L (0, ParamIDs::pitch), 0.0f);
    setParam (*p, L (0, ParamIDs::root), 60.0f);
    setParam (*p, L (0, ParamIDs::start), 0.0f);
    const auto layer0 = p->getNoteLayout (60).layers[0];
    const auto t0 = juce::jmax (0.0, layer0.start / rate) + 0.5;
    const auto x = render (*p, t0 + 0.5, t0 + 0.4);
    CHECK (level (x, f1, t0) > on, "the restored sample plays");
}

static void testUndo (const juce::File& a, const juce::File& b)
{
    std::cout << "undo\n";
    Scene s;
    auto p = makeProcessor (s, a, b);
    const auto before = p->getSample (0);
    juce::MemoryBlock state;
    p->getStateInformation (state);
    p->setStateInformation (state.getData(), (int) state.getSize());
    CHECK (! p->canUndo() && ! p->canRedo(), "loading a state clears the history");
    CHECK (p->getSample (0) == before, "an unchanged file is not loaded again");

    // Host automation (no gesture) is not an undo step.
    setParam (*p, L (1, ParamIDs::level), -12.0f);
    p->commitPendingEdit();
    CHECK (! p->canUndo(), "automation is not recorded");

    for (int i = 1; i <= 10; ++i)
    {
        gesture (*p, L (1, ParamIDs::pitch), (float) i);
        p->commitPendingEdit();
    }

    int undone = 0;

    while (p->undo())
        ++undone;

    CHECK (undone == ReverseSamplerProcessor::maxUndoSteps, "at most 8 steps: " + juce::String (undone));
    CHECK (std::abs (value (*p, L (1, ParamIDs::pitch)) - 2.0f) < 1.0e-4f, "8 undos go back to the 3rd value");
    CHECK (std::abs (value (*p, L (1, ParamIDs::level)) + 12.0f) < 0.05f, "undo leaves automated values alone");

    p->redo(); p->redo(); p->redo();
    CHECK (std::abs (value (*p, L (1, ParamIDs::pitch)) - 5.0f) < 1.0e-4f, "redo x3");
    CHECK (p->canRedo(), "more to redo");
    gesture (*p, L (1, ParamIDs::pitch), -3.0f);
    p->commitPendingEdit();
    CHECK (! p->canRedo(), "a new edit clears redo");
    p->undo();
    CHECK (std::abs (value (*p, L (1, ParamIDs::pitch)) - 5.0f) < 1.0e-4f, "undo the new edit");

    // Edits before the step closes make one step.
    gesture (*p, L (0, ParamIDs::level), -3.0f);
    gesture (*p, L (0, ParamIDs::pan), 40.0f);
    p->commitPendingEdit();
    p->undo();
    CHECK (std::abs (value (*p, L (0, ParamIDs::level))) < 0.05f && std::abs (value (*p, L (0, ParamIDs::pan))) < 0.5f, "grouped edits undo together");

    // An envelope drag is one step.
    rs::Envelope e;
    e.y[1] = 0.5f; p->setEnvelope (ReverseSamplerProcessor::mainEnvelope, e);
    e.y[1] = 0.3f; p->setEnvelope (ReverseSamplerProcessor::mainEnvelope, e);
    p->commitPendingEdit();
    p->undo();
    CHECK (p->getEnvelope (ReverseSamplerProcessor::mainEnvelope).isFlat(), "envelope drag undone in one step");
    p->redo();
    CHECK (std::abs (p->getEnvelope (ReverseSamplerProcessor::mainEnvelope).y[1] - 0.3f) < 1.0e-6f, "envelope redo");

    // Samples.
    const auto old = p->getSample (1);
    const auto c = tmp ("tone700_2s.wav");
    writeTone (c, 700.0, 2.0, 0.5f);
    p->loadSample (1, c);
    p->commitPendingEdit();
    const auto loaded = p->getSample (1);
    CHECK (loaded != nullptr && loaded != old, "new sample loaded");
    p->undo();
    CHECK (p->getSample (1) == old, "undo brings the old sample back");
    p->redo();
    CHECK (p->getSample (1) == loaded, "redo the load");

    // Two files dropped together: one step.
    p->loadSample (0, c);
    p->loadSample (1, a);
    p->commitPendingEdit();
    p->undo();
    CHECK (p->getSample (0) != nullptr && p->getSample (0)->file == a && p->getSample (1) == loaded, "two loads undo together");

    // The timer closes steps by itself.
    gesture (*p, L (0, ParamIDs::width), 50.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (700);
    gesture (*p, L (0, ParamIDs::width), 150.0f);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (700);
    p->undo();
    CHECK (std::abs (value (*p, L (0, ParamIDs::width)) - 50.0f) < 0.5f, "timer closes steps: undo goes back one edit");

    p->getStateInformation (state);
    p->setStateInformation (state.getData(), (int) state.getSize());
    CHECK (! p->canUndo() && ! p->canRedo(), "a state load clears undo and redo");
}

static void testMissing (const juce::File& a, const juce::File& b)
{
    std::cout << "missing sample\n";
    const auto c = tmp ("will_be_missing.wav");
    writeTone (c, 300.0, 1.0, 0.5f);
    Scene s;
    auto p = makeProcessor (s, a, c);
    juce::MemoryBlock state;
    p->getStateInformation (state);
    c.deleteFile();

    auto q = makeProcessor (s, a, b);   // layer 2 holds another sample before the project loads
    q->setStateInformation (state.getData(), (int) state.getSize());
    CHECK (q->getSample (0) != nullptr, "the found sample loads");
    CHECK (q->getSample (1) == nullptr, "a missing sample leaves its layer empty");
    CHECK (q->getMissingSamplePath (1) == c.getFullPathName(), "the missing path is kept");
    CHECK (q->getStatusMessage().contains ("not found"), "the status says so");

    juce::MemoryBlock again;
    q->getStateInformation (again);
    auto r = makeProcessor (s, a, b);
    r->setStateInformation (again.getData(), (int) again.getSize());
    CHECK (r->getMissingSamplePath (1) == c.getFullPathName(), "saving keeps the missing path");

    r->loadSample (1, b);
    CHECK (r->getMissingSamplePath (1).isEmpty(), "loading a sample forgets the missing one");
    r->undo();
    CHECK (r->getSample (1) == nullptr && r->getMissingSamplePath (1) == c.getFullPathName(), "undo brings the missing path back");
    r->resetLayer (1);
    CHECK (r->getMissingSamplePath (1).isEmpty() && r->isLayerDefault (1), "Clear forgets the missing path");
}

//==============================================================================
/** Renders with MIDI events at exact sample positions: (sample, message). */
static std::vector<float> renderEvents (ReverseSamplerProcessor& p, double seconds, const std::vector<std::pair<int, juce::MidiMessage>>& events,
                                        std::function<void (juce::int64)> beforeBlock = {})
{
    const auto total = (int) std::llround (seconds * rate);
    std::vector<float> out;
    juce::AudioBuffer<float> buffer (2, blockSize);

    for (juce::int64 pos = 0; pos < total; pos += blockSize)
    {
        if (beforeBlock)
            beforeBlock (pos);

        juce::MidiBuffer midi;

        for (const auto& [at, message] : events)
            if (at >= pos && at < pos + blockSize)
                midi.addEvent (message, (int) (at - pos));

        buffer.clear();
        p.processBlock (buffer, midi);

        for (int i = 0; i < blockSize && pos + i < total; ++i)
            out.push_back (buffer.getSample (0, i));
    }

    return out;
}

static double roughnessAt (const std::vector<float>& x, int at, int before, int after)
{
    double m = 0.0;

    for (int i = juce::jmax (2, at - before); i < juce::jmin ((int) x.size(), at + after); ++i)
        m = juce::jmax (m, std::abs ((double) x[(size_t) i] - 2.0 * x[(size_t) i - 1] + x[(size_t) i - 2]));

    return m;
}

static void testDeclick (const juce::File& a, const juce::File& b)
{
    std::cout << "declick\n";

    const auto makeOne = [&]
    {
        auto p = std::make_unique<ReverseSamplerProcessor>();
        p->setPlayConfigDetails (0, 2, rate, blockSize);
        p->prepareToPlay (rate, blockSize);
        p->loadSample (0, a);   // 440 Hz, 0.5, 4 s
        setParam (*p, L (0, ParamIDs::timeMode), 1.0f);
        setParam (*p, ParamIDs::attack, 5.0f);   // the new note itself starts smoothly
        setParam (*p, ParamIDs::release, 5.0f);
        return p;
    };

    // Voice stealing: 16 identical voices (note 60 on 16 channels), then a 17th note.
    {
        std::vector<std::pair<int, juce::MidiMessage>> events;

        for (int ch = 1; ch <= 16; ++ch)
            events.push_back ({ 0, juce::MidiMessage::noteOn (ch, 60, 1.0f) });

        auto p = makeOne();
        const auto without = renderEvents (*p, 1.5, events);

        int at = (int) rate;   // the steal falls where one voice is near its peak

        while (std::abs (without[(size_t) at]) / 16.0 < 0.45)
            ++at;

        events.push_back ({ at, juce::MidiMessage::noteOn (1, 72, 1.0f) });
        auto q = makeOne();
        const auto with = renderEvents (*q, 1.5, events);

        std::vector<float> diff (with.size());

        for (size_t i = 0; i < with.size(); ++i)
            diff[i] = with[i] - without[i];

        const auto rough = roughnessAt (diff, at, 64, 480);
        std::cout << "    steal: voice value " << without[(size_t) at] / 16.0 << ", roughness " << rough << "\n";
        CHECK (rough < 0.03, "a stolen voice fades out instead of clicking");
        CHECK (std::abs (diff[(size_t) at + 1200]) < 2.0, "after the steal the rest plays on");
    }

    // A sample change stops the notes: they fade out.
    {
        auto p = makeOne();
        const auto plain = render (*p, 1.5, 1.4);
        juce::int64 cutBlock = (juce::int64) rate / blockSize;

        while (std::abs (plain[(size_t) (cutBlock * blockSize)]) < 0.4)
            ++cutBlock;

        auto q = makeOne();
        const auto cutAt = cutBlock * blockSize;
        const auto x = renderEvents (*q, 1.5, { { 0, juce::MidiMessage::noteOn (1, 60, 1.0f) } },
                                     [&] (juce::int64 pos) { if (pos == cutAt) q->loadSample (1, b); });
        const auto rough = roughnessAt (x, (int) cutAt, 64, 480);
        std::cout << "    sample change: value " << plain[(size_t) cutAt] << ", roughness " << rough << "\n";
        CHECK (rough < 0.01, "a sample change fades the notes out");
        CHECK (rms (x, (double) cutAt / rate + 0.01, (double) cutAt / rate + 0.1) < 1.0e-4, "and they stop");
    }
}

//==============================================================================
static void testInterpolation()
{
    std::cout << "interpolation\n";

    const auto playTone = [] (double freq, double fileRate, int note) -> std::vector<float>
    {
        const auto f = tmp ("interp_" + juce::String (freq) + "_" + juce::String (fileRate) + ".wav");
        writeTone (f, freq, 2.0, 0.5f, fileRate);
        auto p = std::make_unique<ReverseSamplerProcessor>();
        p->setPlayConfigDetails (0, 2, rate, blockSize);
        p->prepareToPlay (rate, blockSize);
        p->loadSample (0, f);
        setParam (*p, L (0, ParamIDs::timeMode), 1.0f);   // Resample
        setParam (*p, ParamIDs::attack, 0.0f);
        return render (*p, 1.0, 0.95, note);
    };

    // 44.1 kHz file at 48 kHz: the image of 15 kHz lands on 18.9 kHz.
    {
        const auto x = playTone (15000.0, 44100.0, 60);
        const auto tone = level (x, 15000.0, 0.5), image = level (x, 48000.0 - (44100.0 - 15000.0), 0.5);
        std::cout << "    44.1k -> 48k: 15 kHz " << tone << ", image " << image << "\n";
        CHECK (tone > 0.48 && tone < 0.52, "15 kHz passes (44.1 kHz file)");
        CHECK (image < 0.5e-3, "its image is at least 60 dB down");
    }

    // An octave up: 15 kHz becomes 30 kHz, above Nyquist; it must not fold back to 18 kHz.
    {
        const auto x = playTone (15000.0, 48000.0, 72);
        const auto alias = level (x, 18000.0, 0.25);
        std::cout << "    +12 st: alias " << alias << "\n";
        CHECK (alias < 0.5e-3, "no alias an octave up");

        const auto y = playTone (5000.0, 48000.0, 72);
        CHECK (std::abs (level (y, 10000.0, 0.25) - 0.5) < 0.01, "5 kHz an octave up is 10 kHz at full level");
    }

    // The root note of a file at the host rate plays it untouched.
    {
        const auto x = playTone (440.0, 48000.0, 60);
        double maxError = 0.0;

        for (int i = 1000; i < 20000; ++i)
            maxError = juce::jmax (maxError, std::abs ((double) x[(size_t) i] - 0.5 * std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / rate)));

        std::cout << "    root note error " << maxError << "\n";
        CHECK (maxError < 1.0e-5, "the root note is the file itself");
    }

    // An octave down (upsampling): no images.
    {
        const auto x = playTone (8000.0, 48000.0, 48);
        const auto tone = level (x, 4000.0, 0.5), image = level (x, 20000.0, 0.5);   // 24k - 4k
        std::cout << "    -12 st: 4 kHz " << tone << ", image " << image << "\n";
        CHECK (std::abs (tone - 0.5) < 0.01 && image < 0.5e-3, "octave down is clean");
    }
}

static void testUiUndo (const juce::File& a, const juce::File& b)
{
    std::cout << "ui undo\n";
    Scene s;
    auto p = makeProcessor (s, a, b);
    std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditor());
    auto* e = dynamic_cast<ReverseSamplerEditor*> (editor.get());

    if (e == nullptr)
        return;

    gesture (*p, ParamIDs::gain, -6.0f);
    CHECK (e->keyPressed (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0)), "Ctrl+Z is handled");
    CHECK (std::abs (value (*p, ParamIDs::gain)) < 0.05f, "Ctrl+Z undoes");
    CHECK (e->keyPressed (juce::KeyPress ('Z', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier, 0)), "Ctrl+Shift+Z is handled");
    CHECK (std::abs (value (*p, ParamIDs::gain) + 6.0f) < 0.05f, "Ctrl+Shift+Z redoes");
    e->keyPressed (juce::KeyPress ('z', juce::ModifierKeys::commandModifier, 0));
    CHECK (e->keyPressed (juce::KeyPress ('y', juce::ModifierKeys::commandModifier, 0)), "Ctrl+Y is handled");
    CHECK (std::abs (value (*p, ParamIDs::gain) + 6.0f) < 0.05f, "Ctrl+Y redoes");
    CHECK (! e->keyPressed (juce::KeyPress ('z', 0, 0)), "a plain Z is left alone");

    e->showLayerEditor (0);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
    snapshot (*e, "layer_editor_undo");
    p->resetLayer (0);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
    snapshot (*e, "layer_editor_cleared");
    CHECK (e->getOpenLayer() == 0, "the layer stays open after Clear");
}

static void testBenchmark (const juce::File& a, const juce::File& b)
{
    // Not a check: how long 8 notes x 2 layers take to render, with mono files at the host rate
    // and with stereo 44.1 kHz files (the reader resamples them).
    const auto sa = tmp ("bench_stereo_441_a.wav"), sb = tmp ("bench_stereo_441_b.wav");

    if (! sa.existsAsFile())  writeTone (sa, 440.0, 4.0, 0.5f, 44100.0, 2);
    if (! sb.existsAsFile())  writeTone (sb, 1000.0, 8.0, 0.5f, 44100.0, 2);

    for (int run = 0; run < 4; ++run)
    {
        const auto timeMode = run % 2;
        const auto stereo = run >= 2;
        Scene s; s.timeMode = timeMode; s.pitch2 = 7;
        auto p = makeProcessor (s, stereo ? sa : a, stereo ? sb : b);
        std::vector<std::pair<int, juce::MidiMessage>> events;

        for (int k = 0; k < 8; ++k)
            events.push_back ({ k * 100, juce::MidiMessage::noteOn (1, 55 + k, 1.0f) });

        const auto start = juce::Time::getMillisecondCounterHiRes();
        renderEvents (*p, 4.0, events);
        const auto ms = juce::Time::getMillisecondCounterHiRes() - start;
        std::cout << "    bench " << (timeMode == 0 ? "Stretch " : "Resample") << (stereo ? " stereo 44.1k" : " mono 48k    ")
                  << ": 4 s of 8 notes x 2 layers in " << juce::roundToInt (ms) << " ms (" << juce::String (ms / 40.0, 1) << " % of one core)\n";
    }
}


//==============================================================================
static juce::File bigFile()
{
    const auto big = tmp ("big_stereo_60s.wav");   // big enough that loading it takes a while

    if (! big.existsAsFile())
        writeTone (big, 330.0, 60.0, 0.5f, rate, 2);

    return big;
}

static void waitForLoader()
{
    juce::Thread::sleep (400);   // a cancelled load notices it within one chunk
}

static void testAsyncLoading (const juce::File& a, const juce::File& b)
{
    std::cout << "background loading\n";
    const auto big = bigFile();
    const auto bad = tmp ("not_audio.wav");
    bad.replaceWithText ("this is not audio");

    Scene s;
    auto p = makeProcessor (s, a, b);
    p->commitPendingEdit();
    const auto old = p->getSample (0);

    // A user load: the old sample plays until the new one is ready; then it is one undo step.
    p->loadSampleAsync (0, big);
    CHECK (p->isLoading (0) && p->getLoadingFile (0) == big, "the load runs in the background");
    CHECK (p->getSample (0) == old, "the old sample stays until the new one is ready");
    CHECK (p->canUndo(), "a load in progress can be undone (cancelled)");
    CHECK (p->waitForPendingLoads (30000), "the load finishes");
    const auto bigSample = p->getSample (0);
    CHECK (! p->isLoading (0) && bigSample != nullptr && bigSample->file == big, "the new sample is installed");
    CHECK (bigSample != nullptr && bigSample->audio.getNumChannels() == 2 && bigSample->audio.getNumSamples() == (int) (60 * rate)
           && std::abs (bigSample->audio.getSample (1, (int) (59 * rate) + 37) - 0.5f * (float) std::sin (juce::MathConstants<double>::twoPi * 330.0 * ((int) (59 * rate) + 37) / rate)) < 1.0e-4f,
           "it is read completely, in chunks");
    p->commitPendingEdit();
    p->undo();
    CHECK (p->getSample (0) == old, "undo brings the old sample back");
    p->redo();
    CHECK (p->getSample (0) == bigSample, "redo");

    // Undo while loading cancels the load (it is the newest action).
    p->loadSampleAsync (1, big);
    CHECK (p->undo(), "undo during a load");
    CHECK (! p->isLoading (1), "undo cancels the load");
    waitForLoader();
    CHECK (p->getSample (1) != nullptr && p->getSample (1)->file == b, "the cancelled file never arrives");
    CHECK (p->getSample (0) == bigSample, "and the step before stays");

    // A newer load wins.
    p->loadSampleAsync (1, big);
    p->loadSampleAsync (1, a);
    p->waitForPendingLoads (30000);
    waitForLoader();
    CHECK (p->getSample (1) != nullptr && p->getSample (1)->file == a, "the newest load wins");

    // Clear cancels a load.
    p->loadSampleAsync (1, big);
    CHECK (! p->isLayerDefault (1), "a layer that is loading is not default");
    p->resetLayer (1);
    waitForLoader();
    CHECK (p->getSample (1) == nullptr && ! p->isLoading (1) && p->isLayerDefault (1), "Clear cancels the load");

    // Two files dropped together: one undo step, even when they finish at different times.
    p->commitPendingEdit();
    p->loadSampleAsync (0, a);
    p->loadSampleAsync (1, big);
    p->waitForPendingLoads (30000);
    juce::MessageManager::getInstance()->runDispatchLoopUntil (700);   // the timer closes the step
    CHECK (p->getSample (0) != nullptr && p->getSample (0)->file == a && p->getSample (1) != nullptr && p->getSample (1)->file == big, "both loaded");
    p->undo();
    CHECK (p->getSample (0) == bigSample && p->getSample (1) == nullptr, "two loads undo together");

    // A file that cannot be read: the layer keeps its sample and the status says why.
    p->loadSampleAsync (0, bad);
    p->waitForPendingLoads (30000);
    CHECK (p->getSample (0) == bigSample && p->getStatusMessage().contains ("unsupported"), "a bad file leaves the layer alone");

    // Arrive: the latency follows a sample loaded in the background (the timer reports it).
    {
        Scene arrive; arrive.mode = 1;
        auto w = makeProcessor (arrive, a, b);   // the 8 s sample sets the latency
        const auto before = w->getLatencySamples();
        w->loadSampleAsync (1, a);               // now 4 s
        w->waitForPendingLoads (30000);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
        std::cout << "    Arrive latency " << before << " -> " << w->getLatencySamples() << "\n";
        CHECK (before == (int) rate && w->getLatencySamples() == (int) rate && w->getLatencySamples() == w->getArriveLatency(),
               "Arrive latency remains bounded at 1 s after long background loads");
    }

    // Destroying the plug-in while it loads is safe.
    {
        auto t = makeProcessor (s, a, b);
        t->loadSampleAsync (0, big);
        t->loadSampleAsync (1, big);
    }

    CHECK (true, "destroyed while loading");
}

static void testAsyncProject (const juce::File& a, const juce::File& b)
{
    std::cout << "background project load\n";
    const auto big = bigFile();
    const auto damaged = tmp ("damaged.wav");
    damaged.replaceWithText ("RIFF....WAVEjunk");

    Scene s;
    auto p = makeProcessor (s, big, a);
    juce::MemoryBlock state;
    p->getStateInformation (state);

    auto q = makeProcessor (s, b, b);   // other samples before the project loads
    q->setStateInformation (state.getData(), (int) state.getSize());
    CHECK (q->isLoading (0) && q->getSample (0) == nullptr, "a project sample loads in the background; meanwhile the layer is empty");

    juce::MemoryBlock during;
    q->getStateInformation (during);   // saved while still loading
    CHECK (q->waitForPendingLoads (30000) && q->getSample (0) != nullptr && q->getSample (0)->file == big
           && q->getSample (1) != nullptr && q->getSample (1)->file == a, "the project samples arrive");
    CHECK (! q->canUndo(), "a project load is not an undo step");
    CHECK (q->getStatusMessage().isEmpty(), "no error");

    // An offline render (export) right after the project load waits for the samples.
    auto r = std::make_unique<ReverseSamplerProcessor>();
    r->setPlayConfigDetails (0, 2, rate, blockSize);
    r->setNonRealtime (true);
    r->prepareToPlay (rate, blockSize);
    r->setStateInformation (during.getData(), (int) during.getSize());
    const auto x = render (*r, 1.0, 0.9);
    CHECK (r->getSample (0) != nullptr && r->getSample (0)->file == big, "a state saved during the load keeps the sample");
    CHECK (rms (x, 0.05, 0.5) > 0.05, "the offline render has the sound from the first block: " + juce::String (rms (x, 0.05, 0.5)));

    // A sample that exists but cannot be read: the layer stays empty, the path is kept, the status says why.
    auto t = makeProcessor (s, a, damaged);
    juce::MemoryBlock withDamaged;
    {
        auto u = makeProcessor (s, a, b);
        u->getStateInformation (withDamaged);
    }
    auto xml = juce::AudioProcessor::getXmlFromBinary (withDamaged.getData(), (int) withDamaged.getSize());
    xml->setAttribute ("layer2Path", damaged.getFullPathName());
    juce::MemoryBlock patched;
    juce::AudioProcessor::copyXmlToBinary (*xml, patched);
    auto v = makeProcessor (s, b, b);
    v->setStateInformation (patched.getData(), (int) patched.getSize());
    v->waitForPendingLoads (30000);
    CHECK (v->getSample (0) != nullptr && v->getSample (0)->file == a, "the good sample loads");
    CHECK (v->getSample (1) == nullptr && v->getMissingSamplePath (1) == damaged.getFullPathName(), "a damaged sample leaves the layer empty and keeps its path");
    CHECK (v->getStatusMessage().contains ("unsupported or damaged"), "the status says why: " + v->getStatusMessage());
}

static void testModSources()
{
    std::cout << "mod sources\n";
    using namespace rs::mod;
    {
        Lfo lfo; lfo.prepare (rate);
        LfoSettings s; s.rateHz = 1.0f;
        lfo.advance (s, (int) rate / 4, 120.0, false, 0.0);
        const float quarter = lfo.value();
        lfo.advance (s, (int) rate / 4, 120.0, false, 0.0);
        CHECK (std::abs (quarter - 0.5f) < 0.01f && std::abs (lfo.value() - 1.0f) < 0.01f, "1 Hz sine LFO: half at a quarter cycle, top at half a cycle");
    }
    {
        Lfo lfo; lfo.prepare (rate);
        LfoSettings s; s.shape = sawUp; s.sync = true; s.division = 7;
        lfo.advance (s, 64, 120.0, true, 10.25);
        CHECK (std::abs (lfo.value() - 0.25f) < 0.01f, "synced free-running LFO follows the host position (1/4 at ppq 10.25)");
        lfo.advance (s, 64, 120.0, true, 10.75);
        CHECK (std::abs (lfo.value() - 0.75f) < 0.01f, "synced LFO jumps with the transport");
    }
    {
        Lfo lfo; lfo.prepare (rate);
        LfoSettings s; s.shape = sawUp; s.rateHz = 2.0f; s.trigger = oneShot;
        lfo.noteOn (s);
        lfo.advance (s, (int) rate, 120.0, false, 0.0);
        const float end = lfo.value();
        lfo.advance (s, (int) rate, 120.0, false, 0.0);
        CHECK (end > 0.99f && lfo.value() > 0.99f, "one-shot LFO stops at the end of its cycle");
        lfo.noteOn (s);
        lfo.advance (s, (int) rate / 8, 120.0, false, 0.0);
        CHECK (std::abs (lfo.value() - 0.25f) < 0.01f, "a new note restarts a one-shot LFO");
    }
    {
        Envelope env; env.prepare (rate);
        EnvelopeSettings s; s.attackMs = 10.0f; s.decayMs = 50.0f; s.sustain = 0.5f; s.releaseMs = 50.0f;
        env.noteOn();
        env.advance (s, (int) (rate * 0.005));
        const float mid = env.value();
        env.advance (s, (int) (rate * 0.005));
        const float top = env.value();
        for (int i = 0; i < 20; ++i) env.advance (s, 480);
        const float held = env.value();
        env.noteOff();
        for (int i = 0; i < 20; ++i) env.advance (s, 480);
        CHECK (std::abs (mid - 0.5f) < 0.02f && top > 0.99f, "envelope attack is linear and reaches the top in its time");
        CHECK (std::abs (held - 0.5f) < 0.01f, "envelope decays to the sustain level");
        CHECK (env.value() < 0.001f && env.getStage() == Envelope::idle, "envelope releases to zero");
    }
    {
        RowSettings row; row.destination = 1; row.source = macro1; row.amount = 0.5f;
        float sources[numSources] {};
        sources[macro1] = 1.0f; sources[macro2] = 0.5f;
        CHECK (std::abs (rowOutput (row, sources) - 0.5f) < 1.0e-6f, "row output is source times amount");
        row.bipolar = true;
        sources[macro1] = 0.0f;
        CHECK (std::abs (rowOutput (row, sources) + 0.5f) < 1.0e-6f, "bipolar row maps 0 to -amount");
        row.bipolar = false; sources[macro1] = 1.0f; row.aux = macro2;
        CHECK (std::abs (rowOutput (row, sources) - 0.25f) < 1.0e-6f, "aux source scales the row");
        row.aux = none; row.curve = 0.5f; sources[macro1] = 0.5f;
        CHECK (rowOutput (row, sources) < 0.25f, "positive curve bends the source down in the middle");
    }
}

static void testModMatrix (const juce::File& a, const juce::File& b)
{
    std::cout << "mod matrix\n";
    using rs::ModMatrix;
    Scene scene;
    const auto amp = [] (const std::vector<float>& x) { return rms (x, 0.5, 1.5); };

    auto plain = makeProcessor (scene, a, b);
    const auto reference = render (*plain, 2.0, 1.8);

    auto routed = makeProcessor (scene, a, b);
    CHECK (routed->addModulation (rs::mod::macro1, ParamIDs::gain) == 0, "a modulation goes to the first free row");
    gesture (*routed, ModMatrix::rowID (0, "Amt"), -100.0f);
    const auto chunked = render (*routed, 2.0, 1.8);
    double maxDiff = 0.0;
    for (size_t i = 0; i < std::min (reference.size(), chunked.size()); ++i)
        maxDiff = std::max (maxDiff, (double) std::abs (reference[i] - chunked[i]));
    std::cout << "    macro at 0: max difference to the unmodulated render " << maxDiff << "\n";
    CHECK (reference.size() == chunked.size() && maxDiff < 1.0e-5, "an active row at zero output renders the same as no modulation");

    auto layerRouted = makeProcessor (scene, a, b);
    layerRouted->addModulation (rs::mod::macro1, L (0, ParamIDs::level));
    gesture (*layerRouted, ModMatrix::rowID (0, "Amt"), -100.0f);
    const auto layerChunked = render (*layerRouted, 2.0, 1.8);
    double layerDiff = 0.0;
    for (size_t i = 0; i < std::min (reference.size(), layerChunked.size()); ++i)
        layerDiff = std::max (layerDiff, (double) std::abs (reference[i] - layerChunked[i]));
    std::cout << "    layer row at 0 (FX rack per block): max difference to the unmodulated render " << layerDiff << "\n";
    CHECK (reference.size() == layerChunked.size() && layerDiff < 1.0e-5, "a layer row at zero output renders the same as no modulation");

    auto down = makeProcessor (scene, a, b);
    down->addModulation (rs::mod::macro1, ParamIDs::gain);
    gesture (*down, ModMatrix::rowID (0, "Amt"), -100.0f);
    setParam (*down, ModMatrix::macroID (0), 100.0f);
    const auto quiet = render (*down, 2.0, 1.8);
    std::cout << "    macro 1 -> Volume -100 %: " << amp (quiet) << " / " << amp (reference) << "\n";
    CHECK (amp (reference) > on && amp (quiet) < amp (reference) * 0.01, "macro 1 turns the volume down through the matrix");
    CHECK (std::abs (value (*down, ParamIDs::gain)) < 0.01f, "modulation never writes the host parameter");

    auto layer = makeProcessor (scene, a, b);
    layer->addModulation (rs::mod::velocity, L (0, ParamIDs::level));
    gesture (*layer, ModMatrix::rowID (0, "Amt"), -100.0f);
    gesture (*layer, ModMatrix::rowID (0, "Bip"), 0.0f);
    setParam (*layer, L (1, ParamIDs::on), 0.0f);
    const auto layerQuiet = render (*layer, 2.0, 1.8);
    CHECK (amp (layerQuiet) < amp (reference) * 0.05, "velocity of a note turns its layer level down through the matrix");

    auto env = makeProcessor (scene, a, b);
    env->addModulation (rs::mod::envelope, ParamIDs::gain);
    renderEvents (*env, 0.02, { { 256, juce::MidiMessage::noteOn (1, 60, 1.0f) } });
    CHECK (env->getModMatrix().sourceValue (rs::mod::envelope) > 0.0f, "a note in the middle of a block starts the envelope");
    CHECK (env->getModMatrix().rowValue (0) > 0.0f, "the row shows the envelope output");
    renderEvents (*env, 1.5, { { 0, juce::MidiMessage::noteOff (1, 60) } });
    CHECK (env->getModMatrix().sourceValue (rs::mod::envelope) < 0.001f, "note off releases the envelope");

    auto& p = *routed;
    const int row = p.addModulation (rs::mod::lfo1, rs::fxID (1, "p1"));
    CHECK (row == 1 && value (p, ModMatrix::rowID (1, "Bip")) > 0.5f && std::abs (value (p, ModMatrix::rowID (1, "Amt")) - 25.0f) < 0.01f,
           "an LFO gets a bipolar row at 25 %");
    CHECK (p.undo() && (int) value (p, ModMatrix::rowID (1, "Src")) == 0 && (int) value (p, ModMatrix::rowID (1, "Dst")) == 0, "one undo removes a new modulation");
    CHECK (p.redo() && (int) value (p, ModMatrix::rowID (1, "Src")) == rs::mod::lfo1, "redo brings it back");
    p.setMacroName (0, "Brightness");
    juce::MemoryBlock state; p.getStateInformation (state);
    ReverseSamplerProcessor copy; copy.setStateInformation (state.getData(), (int) state.getSize());
    CHECK ((int) value (copy, ModMatrix::rowID (1, "Dst")) == ModMatrix::destinationIndex (rs::fxID (1, "p1"))
           && std::abs (value (copy, ModMatrix::rowID (0, "Amt")) + 100.0f) < 0.01f && copy.getMacroName (0) == "Brightness",
           "rows and macro names round trip with the project");

    gesture (p, rs::fxID (1, "type"), 2.0f);
    p.swapFxSlots (1, 2);
    CHECK ((int) value (p, ModMatrix::rowID (1, "Dst")) == ModMatrix::destinationIndex (rs::fxID (2, "p1")), "a slot swap moves its modulation with it");
    p.setFxType (2, 0);
    CHECK ((int) value (p, ModMatrix::rowID (1, "Dst")) == 0 && (int) value (p, ModMatrix::rowID (1, "Src")) == 0, "changing the effect clears the rows that pointed at it");
    CHECK (p.undo() && (int) value (p, ModMatrix::rowID (1, "Dst")) == ModMatrix::destinationIndex (rs::fxID (2, "p1")), "undo brings those rows back with the effect");

    for (int r = 2; r < rs::mod::numRows; ++r) p.addModulation (rs::mod::macro2, ParamIDs::gain);
    CHECK (p.addModulation (rs::mod::macro3, ParamIDs::gain) == -1, "no row is used once all rows are taken");
    p.clearModulationRow (5);
    CHECK ((int) value (p, ModMatrix::rowID (5, "Src")) == 0 && p.addModulation (rs::mod::macro3, ParamIDs::gain) == 5, "a cleared row is free again");
}

static void testUiModulation (const juce::File& a, const juce::File& b)
{
    std::cout << "ui modulation\n";
    auto p = makeProcessor ({}, a, b);
    p->setFxType (0, 2);
    setParam (*p, rs::fxID (0, "p1"), 75.0f);
    setParam (*p, rs::fxID (0, "p3"), 30.0f);
    p->addModulation (rs::mod::lfo1, rs::fxID (0, "p2"));
    gesture (*p, rs::ModMatrix::rowID (0, "Amt"), 40.0f);
    p->addModulation (rs::mod::macro1, ParamIDs::gain);
    p->addModulation (rs::mod::envelope, L (0, ParamIDs::lowCut));
    gesture (*p, rs::ModMatrix::rowID (2, "Aux"), (float) rs::mod::velocity);
    setParam (*p, rs::ModMatrix::macroID (0), 60.0f);
    setParam (*p, rs::ModMatrix::lfoID (1, "Shape"), (float) rs::mod::smoothRandom);
    setParam (*p, rs::ModMatrix::lfoID (1, "Sync"), 1.0f);
    p->setMacroName (0, "Brightness");

    std::unique_ptr<juce::AudioProcessorEditor> editor (p->createEditor());
    juce::AudioBuffer<float> audio (2, blockSize);
    const auto play = [&] (int blocks) {
        for (int k = 0; k < blocks; ++k) {
            juce::MidiBuffer midi;
            if (k == 0) midi.addEvent (juce::MidiMessage::noteOn (1, 60, 0.8f), 0);
            audio.clear();
            p->processBlock (audio, midi);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (5);
        }
    };
    const auto findButton = [] (juce::Component& parent, const juce::String& text) -> juce::TextButton* {
        for (auto* child : parent.getChildren())
            if (auto* button = dynamic_cast<juce::TextButton*> (child); button && button->getButtonText() == text) return button;
        return nullptr;
    };
    play (40);
    snapshot (*editor, "main_modulation");
    CHECK (p->getModMatrix().isDestinationRouted (rs::ModMatrix::destinationIndex (ParamIDs::gain)), "volume shows as modulated");

    auto* fxButton = findButton (*editor, "FX RACK");
    CHECK (fxButton != nullptr, "FX button available for the spectrum");
    if (fxButton != nullptr) {
        fxButton->triggerClick();
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        play (60);
        CHECK (p->getSpectrumTap (0).wanted.load() && p->getSpectrumTap (0).written.load() > 0, "an open EQ asks for and receives its spectrum");
        snapshot (*editor, "fx_eq_spectrum");
        FxRackEditor* rack = nullptr;
        for (auto* child : editor->getChildren()) if (auto* r = dynamic_cast<FxRackEditor*> (child)) rack = r;
        if (auto* back = rack != nullptr ? findButton (*rack, "Back") : nullptr) { back->triggerClick(); juce::MessageManager::getInstance()->runDispatchLoopUntil (100); }
        juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
        CHECK (! p->getSpectrumTap (0).wanted.load(), "a closed EQ stops the spectrum capture");
    }

    auto* modButton = findButton (*editor, "MOD MATRIX");
    CHECK (modButton != nullptr && modButton->isVisible(), "MOD MATRIX button on the main page");
    if (modButton != nullptr) {
        modButton->triggerClick();
        play (30);
        ModMatrixEditor* matrix = nullptr;
        for (auto* child : editor->getChildren()) if (auto* m = dynamic_cast<ModMatrixEditor*> (child)) matrix = m;
        CHECK (matrix != nullptr && matrix->isVisible() && ! modButton->isVisible(), "the Mod Matrix opens over the main page");
        snapshot (*editor, "mod_matrix");
        if (matrix != nullptr) {
            juce::ComboBox* destinationBox = nullptr;
            for (auto* row : matrix->getChildren())
                for (auto* child : row->getChildren())
                    if (auto* box = dynamic_cast<juce::ComboBox*> (child); box != nullptr && destinationBox == nullptr
                        && box->getSelectedId() - 1 == rs::ModMatrix::destinationIndex (rs::fxID (0, "p2")))
                        destinationBox = box;
            CHECK (destinationBox != nullptr, "row 1 shows its FX destination");
            const bool hasDisplay = ! juce::Desktop::getInstance().getDisplays().displays.isEmpty();
            if (destinationBox != nullptr && hasDisplay) {
                editor->setVisible (true);
                editor->addToDesktop (0);
                editor->toFront (true);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
                destinationBox->grabKeyboardFocus();
                juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
                destinationBox->showPopup();
                juce::MessageManager::getInstance()->runDispatchLoopUntil (150);
                int menus = 0;
                for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i)
                    if (juce::Desktop::getInstance().getComponent (i) != editor.get()) ++menus;
                CHECK (menus > 0, "the destination box opens the grouped popup menu");
                if (std::getenv ("RS_SNAPSHOT_DIR") != nullptr) {
                    const auto bounds = editor->getScreenBounds();
                    juce::Image whole (juce::Image::ARGB, bounds.getWidth(), bounds.getHeight(), true);
                    juce::Graphics g (whole);
                    for (int i = 0; i < juce::Desktop::getInstance().getNumComponents(); ++i) {
                        auto* c = juce::Desktop::getInstance().getComponent (i);
                        const auto image = c->createComponentSnapshot (c->getLocalBounds());
                        g.drawImageAt (image, c->getScreenX() - bounds.getX(), c->getScreenY() - bounds.getY());
                    }
                    auto file = juce::File::getCurrentWorkingDirectory().getChildFile (std::getenv ("RS_SNAPSHOT_DIR")).getChildFile ("mod_matrix_destination_menu.png");
                    file.deleteFile();
                    juce::FileOutputStream os (file);
                    juce::PNGImageFormat().writeImageToStream (whole, os);
                }
                juce::PopupMenu::dismissAllActiveMenus();
                juce::MessageManager::getInstance()->runDispatchLoopUntil (60);
                editor->removeFromDesktop();
            }
        }
        editor->setSize (960, 640);
        juce::MessageManager::getInstance()->runDispatchLoopUntil (60);
        snapshot (*editor, "mod_matrix_small");
        if (auto* back = matrix != nullptr ? findButton (*matrix, "Back") : nullptr) { back->triggerClick(); juce::MessageManager::getInstance()->runDispatchLoopUntil (100); }
        bool open = false;
        for (auto* child : editor->getChildren()) if (dynamic_cast<ModMatrixEditor*> (child)) open = true;
        CHECK (! open && modButton->isVisible(), "Back closes the Mod Matrix");
    }
}

static void testFxPresets()
{
    ReverseSamplerProcessor p;
    auto& state = p.getValueTreeState();
    const auto value = [&] (int slot, const char* name) { return state.getRawParameterValue (rs::fxID (slot, name))->load(); };
    int perType[rs::numFxTypes] {};
    bool allValid = true, allLand = true, keepRouting = true, restoreDefaults = true;
    for (size_t k = 0; k < rs::numFxPresets; ++k)
    {
        const auto& preset = rs::fxPresets[k];
        if (! juce::isPositiveAndBelow (preset.type, rs::numFxTypes) || preset.type == 0) { allValid = false; continue; }
        ++perType[preset.type];
        const int slot = (int) (k % rs::fxSlots);
        setParam (p, rs::fxID (slot, "b"), 0.0f);
        setParam (p, rs::fxID (slot, "diffusion"), 77.0f);   // not part of any preset: must return to its default
        p.applyFxPreset (slot, preset);
        if ((int) std::round (value (slot, "type")) != preset.type) allLand = false;
        for (const auto& v : preset.values)
        {
            auto* param = state.getParameter (rs::fxID (slot, v.name));
            if (param == nullptr) { allValid = false; continue; }
            const float got = param->convertFrom0to1 (param->getValue());
            if (std::abs (got - v.value) > 0.051f + 0.001f * std::abs (v.value)) { allLand = false; std::cout << "    preset " << preset.name << ": " << v.name << " = " << got << ", wanted " << v.value << "\n"; }
        }
        if (value (slot, "b") >= 0.5f) keepRouting = false;
        bool diffusionInPreset = false;
        for (const auto& v : preset.values) if (juce::String (v.name) == "diffusion") diffusionInPreset = true;
        if (! diffusionInPreset && std::abs (value (slot, "diffusion")) > 1.0e-3f) restoreDefaults = false;
        setParam (p, rs::fxID (slot, "b"), 1.0f);
    }
    bool enough = true;
    for (int t = 1; t < rs::numFxTypes; ++t) if (perType[t] < 5) enough = false;
    CHECK (allValid, "every FX preset names an effect type and existing parameters");
    CHECK (enough, "at least 5 presets for every effect type");
    CHECK (allLand, "every preset value lands on its parameter (within the parameter's step)");
    CHECK (keepRouting, "a preset keeps the slot's A/B routing");
    CHECK (restoreDefaults, "a preset restores the settings it does not mention to their defaults");
    CHECK (p.canUndo(), "a preset is an undo step");
}

static void testFxRack (const juce::File& a, const juce::File& b)
{
    std::cout << "FX rack\n";
    ReverseSamplerProcessor p;
    rs::FxRackDSP rack; rack.connect (p.getValueTreeState()); rack.prepare (rate);
    juce::AudioBuffer<float> audio (4, 4000); audio.clear();
    audio.setSample (0, 0, 1.0f); audio.setSample (1, 0, 1.0f);
    audio.setSample (2, 0, 1.0f); audio.setSample (3, 0, 1.0f);
    setParam (p, rs::fxID (0, "type"), 1); setParam (p, rs::fxID (0, "a"), 1); setParam (p, rs::fxID (0, "b"), 0);
    setParam (p, rs::fxID (0, "p1"), 0); setParam (p, rs::fxID (0, "p2"), 0); setParam (p, rs::fxID (0, "p3"), 100); setParam (p, rs::fxID (0, "mix"), 100);
    rack.process (audio, 4000);
    CHECK (std::abs (audio.getSample (0, 0)) < 1.0e-6f, "wet delay removes initial impulse on A");
    CHECK (std::abs (audio.getSample (0, 1920)) > 0.5f, "delay appears on A at 40 ms");
    CHECK (std::abs (audio.getSample (2, 0) - 1.0f) < 1.0e-6f && std::abs (audio.getSample (2, 1920)) < 1.0e-6f, "B bypasses A-only effect");
    // Both routed, only A receives an impulse: their delay histories must be independent.
    setParam (p, rs::fxID (0, "b"), 1); rack.prepare (rate); audio.clear();
    audio.setSample (0, 0, 1.0f); audio.setSample (0, 3999, 1.0f);
    rack.process (audio, 4000);
    CHECK (audio.getMagnitude (0, 1920, 1) > 0.5f && audio.getMagnitude (2, 0, 4000) < 1.0e-6f, "both enabled, A delay does not leak into B");
    audio.clear(); setParam (p, rs::fxID (0, "a"), 0); rack.process (audio, 4000);
    setParam (p, rs::fxID (0, "a"), 1); rack.process (audio, 4000);
    CHECK (audio.getMagnitude (0, 0, 4000) < 1.0e-6f, "disabled and reenabled delay has no stale history");
    setParam (p, rs::fxID (0, "type"), 2); setParam (p, rs::fxID (0, "p1"), 50); setParam (p, rs::fxID (0, "p2"), 50); setParam (p, rs::fxID (0, "p3"), 50);
    audio.clear(); audio.setSample (0, 0, 0.7f); rack.process (audio, 4000);
    CHECK (std::abs (audio.getSample (0, 0) - 0.7f) < 1.0e-5f && audio.getMagnitude (0, 1, 3999) < 1.0e-5f, "EQ centre is unity");
    {
        // v0.10 EQ: Modern is exact, Vintage follows the Pultec-style reference, cuts are clean in both.
        const auto sineGain = [&] (double freq) {
            juce::AudioBuffer<float> sine (4, 4000);
            float peak = 0.0f;
            for (int block = 0; block < 6; ++block) {
                sine.clear();
                for (int i = 0; i < 4000; ++i) sine.setSample (0, i, 0.1f * (float) std::sin (juce::MathConstants<double>::twoPi * freq * (block * 4000 + i) / rate));
                rack.process (sine, 4000);
                if (block == 5) peak = sine.getMagnitude (0, 0, 4000);
            }
            return juce::Decibels::gainToDecibels (peak / 0.1f);
        };
        setParam (p, rs::fxID (0, "eqMode"), 1); setParam (p, rs::fxID (0, "p2"), 100);
        CHECK (std::abs (sineGain (1000.0) - 12.0f) < 0.15f, "Modern EQ: Mid +12 dB at 1 kHz");
        setParam (p, rs::fxID (0, "eqMode"), 0);
        CHECK (std::abs (sineGain (1000.0) - 6.1f) < 0.4f, "Vintage EQ: Mid max is a broad +6 dB Pultec-style bump");
        setParam (p, rs::fxID (0, "p2"), 50); setParam (p, rs::fxID (0, "eqLowCut"), 1000.0f);
        const float cutVintage = sineGain (100.0);
        setParam (p, rs::fxID (0, "eqMode"), 1);
        const float cutModern = sineGain (100.0);
        CHECK (cutVintage < -38.0f && std::abs (cutVintage - cutModern) < 0.05f, "EQ low cut is the same clean 12 dB/oct filter in both modes");
        setParam (p, rs::fxID (0, "eqLowCut"), 20.0f); setParam (p, rs::fxID (0, "eqMode"), 0);
    }
    setParam (p, rs::fxID (0, "type"), 3); setParam (p, rs::fxID (0, "p1"), 100); setParam (p, rs::fxID (0, "p2"), 100); setParam (p, rs::fxID (0, "p3"), 50);
    setParam (p, rs::fxID (0, "mix"), 100);
    CHECK (p.getValueTreeState().getParameter (rs::fxID (0, "driveType")) != nullptr, "drive type parameter exists");
    // The 4x oversampled Drive has a few samples of latency, so judge a steady sine, not sample 0.
    for (int driveType = 0; driveType < 4; ++driveType) {
        setParam (p, rs::fxID (0, "driveType"), (float) driveType);
        const auto input = [] (int n) { return 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * n / rate); };
        audio.clear();
        for (int n = 0; n < 4000; ++n) { audio.setSample (0, n, input (n)); audio.setSample (1, n, input (n)); }
        rack.process (audio, 4000);
        bool finite = true; float difference = 0.0f; double energy = 0.0;
        for (int n = 2000; n < 4000; ++n) {
            const float y = audio.getSample (0, n);
            finite = finite && std::isfinite (y); difference = juce::jmax (difference, std::abs (y - input (n))); energy += (double) y * y;
        }
        const double levelDb = 10.0 * std::log10 (juce::jmax (1.0e-12, energy / 2000.0) / (0.25 * 0.25 / 2.0));
        CHECK (finite && difference > 0.05f, "drive type " << driveType << " processes routed audio");
        CHECK (std::abs (levelDb) < 3.0, "drive type " << driveType << " is loudness-matched at full drive");
    }
    setParam (p, rs::fxID (0, "driveType"), 0);
    // Reverb: every mode is reachable from the host, leaves a tail after the input stops and stays finite.
    setParam (p, rs::fxID (0, "type"), 4); setParam (p, rs::fxID (0, "p1"), 50); setParam (p, rs::fxID (0, "p2"), 50);
    setParam (p, rs::fxID (0, "p3"), 50); setParam (p, rs::fxID (0, "mix"), 50);
    CHECK (p.getValueTreeState().getParameter (rs::fxID (0, "rvMode")) != nullptr && p.getValueTreeState().getParameter (rs::fxID (0, "rvChar")) != nullptr, "reverb parameters exist");
    for (int reverbMode = 0; reverbMode < rs::reverb::numModes; ++reverbMode) {
        setParam (p, rs::fxID (0, "rvMode"), (float) reverbMode);
        // A Type or mode change restarts the reverb: the old effect fades out, the new one starts
        // clean and fades in (about 60 ms). Let that finish, so the burst below reaches the new mode.
        audio.clear(); rack.process (audio, 4000);
        audio.clear();
        for (int n = 0; n < 1000; ++n) { const float x = 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 330.0 * n / rate); audio.setSample (0, n, x); audio.setSample (1, n, x); }
        rack.process (audio, 4000);
        bool finite = true; double tail = 0.0;
        for (int n = 0; n < 4000; ++n) for (int c = 0; c < 2; ++c) { const float y = audio.getSample (c, n); finite = finite && std::isfinite (y) && std::abs (y) < 4.0f; if (n >= 3000) tail += (double) y * y; }
        CHECK (finite && tail > 1.0e-7, "reverb mode " << reverbMode << " leaves a finite tail");
    }
    setParam (p, rs::fxID (0, "rvMode"), 0);
    setParam (p, rs::fxID (0, "type"), 3); setParam (p, rs::fxID (0, "p1"), 100); setParam (p, rs::fxID (0, "p2"), 100);
    setParam (p, rs::fxID (0, "p3"), 50); setParam (p, rs::fxID (0, "mix"), 100);
    setParam (p, rs::fxID (1, "type"), 1); setParam (p, rs::fxID (1, "p1"), 17);
    p.swapFxSlots (0, 1);
    CHECK ((int)p.getValueTreeState().getRawParameterValue(rs::fxID(0,"type"))->load() == 1 && std::abs(p.getValueTreeState().getRawParameterValue(rs::fxID(0,"p1"))->load()-17) < 0.01f, "swap moves effect settings");
    CHECK ((int)p.getValueTreeState().getRawParameterValue(rs::fxID(1,"type"))->load() == 3, "swap moves other effect");
    CHECK (p.undo() && (int)p.getValueTreeState().getRawParameterValue(rs::fxID(0,"type"))->load() == 3, "one undo reverts swap");
    CHECK (p.redo() && (int)p.getValueTreeState().getRawParameterValue(rs::fxID(0,"type"))->load() == 1, "redo restores swap");
    juce::MemoryBlock state; p.getStateInformation (state); ReverseSamplerProcessor q; q.setStateInformation (state.getData(), (int)state.getSize());
    CHECK ((int)q.getValueTreeState().getRawParameterValue(rs::fxID(0,"type"))->load() == 1 && std::abs(q.getValueTreeState().getRawParameterValue(rs::fxID(0,"p1"))->load()-17) < 0.01f, "FX state round trips");
    // v0.15: choosing a type in the editor loads that effect's neutral shared knobs as one undo step,
    // so a former Delay Time never becomes a hidden EQ cut. Host automation of Type alone keeps them.
    setParam (p, rs::fxID (2, "type"), 1); setParam (p, rs::fxID (2, "p1"), 17); setParam (p, rs::fxID (2, "p2"), 90);
    p.setFxType (2, 2);
    {
        auto& st = p.getValueTreeState();
        bool flat = (int) st.getRawParameterValue (rs::fxID (2, "type"))->load() == 2;
        for (const char* k : { "p1", "p2", "p3" }) flat = flat && std::abs (st.getRawParameterValue (rs::fxID (2, k))->load() - 50.0f) < 0.01f;
        CHECK (flat, "new EQ starts flat");
        CHECK (p.undo() && (int) st.getRawParameterValue (rs::fxID (2, "type"))->load() == 1
               && std::abs (st.getRawParameterValue (rs::fxID (2, "p1"))->load() - 17.0f) < 0.01f, "one undo restores the delay and its knobs");
        p.setFxType (2, 4);
        CHECK (std::abs (st.getRawParameterValue (rs::fxID (2, "mix"))->load() - rs::fxTypeDefaults[4][3]) < 0.01f, "new reverb starts at its default mix");
        p.setFxType (2, 5);
        CHECK ((int) st.getRawParameterValue (rs::fxID (2, "type"))->load() == 5 && std::abs (st.getRawParameterValue (rs::fxID (2, "p3"))->load() - 100.0f) < 0.01f
               && std::abs (st.getRawParameterValue (rs::fxID (2, "mix"))->load() - 50.0f) < 0.01f, "new chorus starts at full width and an equal mix");
        setParam (p, rs::fxID (2, "chModel"), 1); setParam (p, rs::fxID (2, "chMode"), 5); setParam (p, rs::fxID (2, "chVoices"), 7);
        juce::MemoryBlock chorusState; p.getStateInformation (chorusState);
        ReverseSamplerProcessor c; c.setStateInformation (chorusState.getData(), (int) chorusState.getSize());
        auto& cs = c.getValueTreeState();
        CHECK ((int) cs.getRawParameterValue (rs::fxID (2, "type"))->load() == 5 && (int) cs.getRawParameterValue (rs::fxID (2, "chModel"))->load() == 1
               && (int) cs.getRawParameterValue (rs::fxID (2, "chMode"))->load() == 5 && (int) cs.getRawParameterValue (rs::fxID (2, "chVoices"))->load() == 7,
               "chorus model, mode and voices round trip");
        setParam (p, rs::fxID (2, "chFeedback"), 40.0f); setParam (p, rs::fxID (2, "p1"), 90.0f);
        setParam (p, rs::fxID (2, "a"), 0.0f); setParam (p, rs::fxID (2, "b"), 0.0f);
        p.setFxType (2, 0);
        const auto at = [&] (const char* name) { return st.getRawParameterValue (rs::fxID (2, name))->load(); };
        CHECK ((int) at ("chModel") == 0 && (int) at ("chMode") == 0 && std::abs (at ("chFeedback")) < 0.01f && (int) at ("chVoices") == 4
               && std::abs (at ("p1") - 50.0f) < 0.01f && std::abs (at ("rvPreDelay") - 10.0f) < 0.01f,
               "choosing Empty resets every setting of the slot to its default");
        CHECK (at ("a") >= 0.5f && at ("b") >= 0.5f, "choosing Empty also resets the A/B routing to both layers");
        p.setFxType (2, 5);
        CHECK ((int) at ("chModel") == 0 && std::abs (at ("chFeedback")) < 0.01f && std::abs (at ("p3") - 100.0f) < 0.01f, "a chorus chosen after Empty starts from the defaults");
        CHECK (p.undo() && (int) at ("type") == 0 && p.undo() && (int) at ("type") == 5 && (int) at ("chModel") == 1 && std::abs (at ("chFeedback") - 40.0f) < 0.01f
               && at ("a") < 0.5f && at ("b") < 0.5f,
               "undo after Empty brings back the chorus with its settings and its A/B routing");
        CHECK (p.redo() && (int) at ("type") == 0 && at ("a") >= 0.5f && at ("b") >= 0.5f, "redo of Empty resets A/B again");
        p.undo();
        setParam (p, rs::fxID (2, "b"), 1.0f);
        p.setFxType (2, 0);
    }
    // Simulate a state created before FX existed, loaded over an active rack.
    auto legacyTree = p.getValueTreeState().copyState();
    for (int i = legacyTree.getNumChildren() - 1; i >= 0; --i)
        if (legacyTree.getChild (i).getProperty ("id").toString().startsWith ("fx")) legacyTree.removeChild (i, nullptr);
    juce::MemoryBlock legacy; juce::AudioProcessor::copyXmlToBinary (*legacyTree.createXml(), legacy);
    q.setStateInformation (legacy.getData(), (int)legacy.getSize());
    CHECK (q.getValueTreeState().getRawParameterValue(rs::fxID(0,"type"))->load() < 0.5f, "old project clears previous rack");
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (p.createEditor());
        juce::TextButton* fxButton = nullptr;
        for (auto* child : editor->getChildren())
            if (auto* button = dynamic_cast<juce::TextButton*> (child); button && button->getButtonText() == "FX RACK") fxButton = button;
        CHECK (fxButton != nullptr, "FX button available");
        if (fxButton) { fxButton->triggerClick(); juce::MessageManager::getInstance()->runDispatchLoopUntil (250); }
        FxRackEditor* rackEditor = nullptr;
        for (auto* child : editor->getChildren()) if (auto* rackView = dynamic_cast<FxRackEditor*> (child)) rackEditor = rackView;
        CHECK (rackEditor != nullptr && rackEditor->isVisible(), "FX window opens"); snapshot (*editor, "fx_rack");
        for (int mode=0; mode<4; ++mode) {
            setParam (p, rs::fxID (0, "delayStyle"), (float) mode);
            setParam (p, rs::fxID (0, "sync"), mode == 3 ? 1.0f : 0.0f);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (150);
            snapshot (*editor, "fx_delay_" + juce::String (mode));
            if (mode == 3 && rackEditor != nullptr) {
                std::function<bool(juce::Component&)> hasReflections = [&] (juce::Component& component) {
                    if (auto* label=dynamic_cast<juce::Label*>(&component); label && label->isVisible() && label->getText()=="Reflections") return true;
                    for(auto* child:component.getChildren()) if(hasReflections(*child)) return true;
                    return false;
                };
                CHECK (!hasReflections(*rackEditor), "Amethyst editor contains no Reflections control");
            }
        }
        juce::TextButton* back = nullptr;
        if (rackEditor != nullptr) {
            std::vector<juce::ComboBox*> typeMenus;
            std::function<void (juce::Component&)> collect = [&] (juce::Component& component) {
                if (auto* box = dynamic_cast<juce::ComboBox*> (&component); box && box->getNumItems() > 0 && box->getItemText (0) == "Empty") typeMenus.push_back (box);
                for (auto* child : component.getChildren()) collect (*child);
            };
            collect (*rackEditor);
            CHECK (typeMenus.size() == (size_t) rs::fxSlots, "every slot has a Type menu");
            if (typeMenus.size() == (size_t) rs::fxSlots) {
                auto* menu = typeMenus[3];
                juce::Component* slotView = menu->getParentComponent();
                menu->setSelectedId (6, juce::sendNotificationSync);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
                bool modelShown = false;
                for (auto* child : slotView->getChildren())
                    if (auto* box = dynamic_cast<juce::ComboBox*> (child); box && box != menu && box->isVisible() && box->getItemText (0) == rs::chorus::modelNames[0]) modelShown = true;
                CHECK ((int) p.getValueTreeState().getRawParameterValue (rs::fxID (3, "type"))->load() == 5 && modelShown, "choosing Chorus in the Type menu switches the slot and shows its controls at once");
                snapshot (*editor, "fx_menu_chorus");
                menu->setSelectedId (1, juce::sendNotificationSync);
                juce::MessageManager::getInstance()->runDispatchLoopUntil (100);
            }
        }
        {
            const int models[2][rs::fxSlots] { { 0, 1, 2, 3, 4 }, { 4, 5, 6, 3, 1 } };
            const int modes[2][rs::fxSlots] { { 0, 6, 1, 2, 2 }, { 0, 0, 1, 0, 2 } };
            std::function<bool (juce::Component&, const juce::String&)> showsLabel = [&] (juce::Component& component, const juce::String& text) {
                if (auto* label = dynamic_cast<juce::Label*> (&component); label && label->isVisible() && label->getText() == text) return true;
                for (auto* child : component.getChildren()) if (showsLabel (*child, text)) return true;
                return false;
            };
            for (int page = 0; page < 2; ++page) {
                for (int slot = 0; slot < rs::fxSlots; ++slot) {
                    setParam (p, rs::fxID (slot, "type"), 5);
                    setParam (p, rs::fxID (slot, "chModel"), (float) models[page][slot]);
                    setParam (p, rs::fxID (slot, "chMode"), (float) modes[page][slot]);
                }
                juce::MessageManager::getInstance()->runDispatchLoopUntil (300);
                snapshot (*editor, "fx_chorus_" + juce::String (page + 1));
                if (rackEditor != nullptr && page == 0)
                    CHECK (showsLabel (*rackEditor, "Intensity") && showsLabel (*rackEditor, "Voices") && ! showsLabel (*rackEditor, "Noise"), "chorus slots show their model's controls");
                if (rackEditor != nullptr && page == 1)
                    CHECK (showsLabel (*rackEditor, "Vibrato Rate") && showsLabel (*rackEditor, "Detune"), "Ensemble and MicroPitch controls are shown");
            }
            for (int slot = 0; slot < rs::fxSlots; ++slot) setParam (p, rs::fxID (slot, "type"), 0);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        }
        if (rackEditor != nullptr) {
            std::function<bool (juce::Component&, const juce::String&)> showsLabel = [&] (juce::Component& component, const juce::String& text) {
                if (auto* label = dynamic_cast<juce::Label*> (&component); label && label->isVisible() && label->getText() == text) return true;
                for (auto* child : component.getChildren()) if (showsLabel (*child, text)) return true;
                return false;
            };
            p.setFxType (1, 6); p.setFxType (2, 6);
            setParam (p, rs::fxID (2, "cpMode"), 1); setParam (p, rs::fxID (2, "cpKnee"), 12.0f);
            setParam (p, rs::fxID (1, "p1"), 40); setParam (p, rs::fxID (2, "p1"), 40);
            juce::MidiBuffer midi; midi.addEvent (juce::MidiMessage::noteOn (1, 60, 1.0f), 0);
            juce::AudioBuffer<float> audio (2, 512);
            for (int b = 0; b < 40; ++b) {
                audio.clear(); p.processBlock (audio, midi); midi.clear();
                const float wave = 0.5f + 0.45f * std::sin ((float) b * 0.4f);
                for (int slot : { 1, 2 }) p.getCompressorMeters (slot).push (wave, wave * 0.5f, 6.0f * wave, 20.0f * std::log10 (wave));
                juce::MessageManager::getInstance()->runDispatchLoopUntil (35);
            }
            snapshot (*editor, "fx_compressor");
            CHECK (showsLabel (*rackEditor, "Threshold") && showsLabel (*rackEditor, "Attack") && showsLabel (*rackEditor, "Knee"), "the compressor shows its controls");
            CHECK ((int) p.getValueTreeState().getRawParameterValue (rs::fxID (1, "type"))->load() == 6
                   && std::abs (p.getValueTreeState().getRawParameterValue (rs::fxID (1, "mix"))->load() - 100.0f) < 0.01f, "a new compressor starts fully wet");
            midi.addEvent (juce::MidiMessage::allNotesOff (1), 0); audio.clear(); p.processBlock (audio, midi);
            p.setFxType (1, 0); p.setFxType (2, 0);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (200);
        }
        if (rackEditor != nullptr)
            for (auto* child : rackEditor->getChildren())
                if (auto* button = dynamic_cast<juce::TextButton*> (child); button && button->getButtonText() == "Back") back = button;
        CHECK (back != nullptr, "FX back button available");
        if (back) { back->triggerClick(); juce::MessageManager::getInstance()->runDispatchLoopUntil (250); }
        bool stillOpen = false;
        for (auto* child : editor->getChildren()) if (dynamic_cast<FxRackEditor*> (child)) stillOpen = true;
        CHECK (! stillOpen && fxButton != nullptr && fxButton->isVisible(), "FX closes and main page returns");
    }
    auto dry = makeProcessor ({}, a, b), wet = makeProcessor ({}, a, b);
    setParam (*wet, rs::fxID (0, "type"), 3); setParam (*wet, rs::fxID (0, "p1"), 90); setParam (*wet, rs::fxID (0, "mix"), 100);
    auto dryAudio = render (*dry, 0.3, 0.2), wetAudio = render (*wet, 0.3, 0.2); float difference = 0;
    for (size_t n = 0; n < dryAudio.size(); ++n) difference = juce::jmax (difference, std::abs (dryAudio[n] - wetAudio[n]));
    CHECK (difference > 0.01f, "rack is connected to processor signal path");
}

static void testDelayModes (const juce::File& sample)
{
    std::cout << "delay modes and MIDI velocity\n";
    ReverseSamplerProcessor p;
    rs::FxRackDSP rack; rack.connect (p.getValueTreeState()); rack.prepare (rate);
    auto set = [&] (const char* id, float v) { setParam (p, rs::fxID (0, id), v); };
    set ("type", 1); set ("a", 1); set ("b", 0); set ("p1", 0); set ("p2", 50); set ("mix", 100);
    set ("delayStyle", 0); set ("diffusion", 0);
    juce::AudioBuffer<float> audio (4, 8000); audio.clear(); audio.setSample (0, 400, 0.5f);
    rack.process (audio, 8000);
    const int reverseAt = 1920 + (1919 - 400);
    CHECK (std::abs (audio.getSample (0, reverseAt) - 0.5f) < 0.01f, "reverse reverses the first grain");
    CHECK (std::abs (audio.getSample (0, reverseAt + 1920) - 0.1625f) < 0.01f, "reverse feedback keeps echoes reversed");
    set ("diffusion", 100); rack.prepare (rate); audio.clear(); audio.setSample (0, 400, 0.5f); rack.process (audio, 8000);
    CHECK (std::abs (audio.getSample (0, reverseAt) - 0.5f) > 0.1f && audio.getMagnitude (0, reverseAt + 1, 1000) > 0.01f, "diffusion smears the reverse echo");
    set ("delayStyle", 2); set ("replica", 1); set ("p2", 0); set ("sync", 1); set ("division", 10);
    rack.prepare (rate); juce::AudioBuffer<float> syncAudio (4, 30000); syncAudio.clear(); syncAudio.setSample (0, 1000, 0.5f);
    rack.process (syncAudio, 30000, 120.0);
    CHECK (std::abs (syncAudio.getSample (0, 25000) - 0.5f) < 1.0e-5f, "quarter note at 120 BPM = 500 ms");
    set ("division", 9); rack.prepare (rate); syncAudio.clear(); syncAudio.setSample (0, 1000, 0.5f); rack.process (syncAudio, 30000, 120.0);
    CHECK (std::abs (syncAudio.getSample (0, 17000) - 0.5f) < 1.0e-5f, "quarter triplet at 120 BPM");
    set ("sync", 0); set ("delayStyle", 3); set ("reflections", 1); set ("amSplice", 60); set ("amReverse", 0); rack.prepare (rate);
    juce::AudioBuffer<float> tone (4, 24000); tone.clear();
    for (int n=0; n<24000; ++n) tone.setSample (0, n, 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * n / rate));
    rack.process (tone, 24000); std::vector<float> out (tone.getReadPointer (0), tone.getReadPointer (0) + 24000);
    CHECK (level (out, 880.0, 0.25, 0.1) > level (out, 440.0, 0.25, 0.1) * 3.0, "Amethyst shifts a 440 Hz tone up one octave");
    // Legacy Reflections automation is ignored: always one shift generation.
    auto renderReflections = [&] (int count) {
        set ("reflections", (float) count); set ("p2", 100); rack.prepare (rate);
        juce::AudioBuffer<float> b (4, 24000); b.clear();
        for (int n=3840; n<4800; ++n) b.setSample (0, n, 0.25f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * n / rate));
        rack.process (b, 24000);
        return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + 24000);
    };
    const auto one = renderReflections (1), three = renderReflections (3);
    CHECK (one == three, "legacy Reflections values cannot add shift generations");
    CHECK (p.getValueTreeState().getRawParameterValue (rs::fxID (0, "reflections"))->load() == 1.0f,
           "Reflections remains 1 even after old host automation");
    // A sustained tone exposes whether the second generation is actually shifted
    // again (+24 semitones), instead of repeating a previously pitched signal.
    auto measureSpiral = [&] (int count, bool reverse) {
        set ("reflections", (float) count); set ("p2", 100); set ("amReverse", reverse ? 1.0f : 0.0f); rack.prepare (rate);
        juce::AudioBuffer<float> b (4, 48000); b.clear();
        for (int n=0; n<48000; ++n) b.setSample (0, n, 0.1f * (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * n / rate));
        rack.process (b, 48000);
        return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + 48000);
    };
    for (int reverse=0; reverse<2; ++reverse) {
        const auto single = measureSpiral (1, reverse != 0), multi = measureSpiral (2, reverse != 0);
        const auto octave = level (multi, 880.0, 0.7, 0.2), second = level (multi, 1760.0, 0.7, 0.2), singleSecond = level (single, 1760.0, 0.7, 0.2);
        std::cout << "    direction " << reverse << " spectral 880=" << octave << " 1760=" << second << " single1760=" << singleSecond << "\n";
        CHECK (single == multi && octave > 0.001 && second < octave * 0.05 && singleSecond < octave * 0.05,
               "Amethyst keeps one octave generation in either direction");
        CHECK (std::all_of (multi.begin(), multi.end(), [] (float x) { return std::isfinite (x) && std::abs (x) < 2.0f; }), "Amethyst output stays finite in both directions");
    }
    // Replica Hi-Cut reduces a high-frequency repeat; Modern remains linear.
    set ("delayStyle", 2); set ("replica", 1); set ("p2", 0); set ("replicaBrown", 0);
    auto renderReplicaTone = [&] (bool brown, float amplitude = 0.1f) {
        set ("replicaBrown", brown ? 1.0f : 0.0f); rack.prepare (rate);
        juce::AudioBuffer<float> b (4, 24000); b.clear();
        for (int n=0; n<24000; ++n) b.setSample (0,n, amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * 8000.0 * n / rate));
        rack.process (b, 24000); return std::vector<float> (b.getReadPointer (0), b.getReadPointer (0) + 24000);
    };
    const auto bright = renderReplicaTone (false), brown = renderReplicaTone (true);
    CHECK (level (brown, 8000.0, 0.25, 0.1) < level (bright, 8000.0, 0.25, 0.1) * 0.6, "Replica Hi-Cut removes highs from repeats");
    set ("replica", 0);
    const auto vintageQuiet = renderReplicaTone (false, 0.03f), vintageLoud = renderReplicaTone (false, 0.3f);
    const auto qLevel = level (vintageQuiet, 8000.0, 0.25, 0.1), lLevel = level (vintageLoud, 8000.0, 0.25, 0.1);
    std::cout << "    Replica vintage quiet/loud " << qLevel / lLevel << "\n";
    CHECK (lLevel > 0.01 && qLevel / lLevel > 0.03 && qLevel / lLevel < 0.3, "Replica Vintage responds to input dynamics");
    // Feed a real MIDI note through the processor, not just the effect in isolation.
    for (int style = 0; style < 4; ++style) {
        auto renderVelocity = [&] (float velocity) {
            auto proc = std::make_unique<ReverseSamplerProcessor>();
            proc->setPlayConfigDetails (0, 2, rate, blockSize); proc->prepareToPlay (rate, blockSize); proc->loadSample (0, sample);
            setParam (*proc, L (0, ParamIDs::timeMode), 1); setParam (*proc, ParamIDs::attack, 0); setParam (*proc, ParamIDs::release, 5);
            setParam (*proc, rs::fxID (0, "type"), 1); setParam (*proc, rs::fxID (0, "delayStyle"), (float)style);
            setParam (*proc, rs::fxID (0, "p1"), 0); setParam (*proc, rs::fxID (0, "p2"), 0); setParam (*proc, rs::fxID (0, "mix"), 100);
            const auto v = renderEvents (*proc, 0.35, { {0, juce::MidiMessage::noteOn (1, 60, velocity)}, {(juce::int64)(rate*0.2), juce::MidiMessage::noteOff (1, 60)} });
            double e = 0; for (float x : v) e += (double)x*x; return std::sqrt (e / (double)v.size());
        };
        const auto loud = renderVelocity (1.0f), quiet = renderVelocity (0.3f);
        std::cout << "    mode " << style << " velocity ratio " << (loud > 0 ? quiet / loud : 0) << "\n";
        CHECK (loud > 0.001 && quiet / loud > 0.25 && quiet / loud < 0.4, "MIDI velocity is retained at 100% wet, mode " + juce::String (style));
    }
    set ("amSplice", 175); set ("amReverse", 0); set ("replicaBrown", 1);
    set ("delayStyle", 1); set ("bias", 23); set ("sync", 1); set ("division", 11); set ("diffusion", 71); set ("reflections", 7); set ("replica", 0);
    p.swapFxSlots (0, 1);
    CHECK (p.getValueTreeState().getRawParameterValue (rs::fxID(1,"bias"))->load() > 22.9f && p.getValueTreeState().getRawParameterValue (rs::fxID(1,"reflections"))->load() == 1.0f && p.getValueTreeState().getRawParameterValue (rs::fxID(1,"amSplice"))->load() > 174.9f && p.getValueTreeState().getRawParameterValue (rs::fxID(1,"replicaBrown"))->load() > 0.5f, "swap moves all delay controls");
    juce::MemoryBlock state; p.getStateInformation (state); ReverseSamplerProcessor q; q.setStateInformation (state.getData(), (int)state.getSize());
    CHECK ((int) q.getValueTreeState().getRawParameterValue (rs::fxID(1,"delayStyle"))->load() == 1 && (int) q.getValueTreeState().getRawParameterValue (rs::fxID(1,"division"))->load() == 11 && q.getValueTreeState().getRawParameterValue (rs::fxID(1,"sync"))->load() > 0.5f, "delay mode, sync, division round trip");
    CHECK (q.getValueTreeState().getRawParameterValue (rs::fxID(1,"amSplice"))->load() > 174.9f && q.getValueTreeState().getRawParameterValue (rs::fxID(1,"amReverse"))->load() < 0.5f && q.getValueTreeState().getRawParameterValue (rs::fxID(1,"replicaBrown"))->load() > 0.5f, "new delay controls round trip");
    auto old08 = p.getValueTreeState().copyState();
    for (int j=old08.getNumChildren()-1; j>=0; --j) {
        const auto id = old08.getChild(j).getProperty ("id").toString();
        if (id.contains ("_amSplice") || id.contains ("_amReverse") || id.contains ("_replicaBrown")) old08.removeChild (j, nullptr);
    }
    juce::MemoryBlock old08State; juce::AudioProcessor::copyXmlToBinary (*old08.createXml(), old08State); q.setStateInformation (old08State.getData(), (int) old08State.getSize());
    CHECK (std::abs (q.getValueTreeState().getRawParameterValue (rs::fxID(1,"amSplice"))->load() - 350.0f) < 0.1f && q.getValueTreeState().getRawParameterValue (rs::fxID(1,"amReverse"))->load() > 0.5f && q.getValueTreeState().getRawParameterValue (rs::fxID(1,"replicaBrown"))->load() < 0.5f, "0.8 state gets defaults for new delay controls");
    {
        auto noPeakMove = p.getValueTreeState().copyState();
        noPeakMove.removeChild (noPeakMove.getChildWithProperty ("id", ParamIDs::peakCut), nullptr);
        setParam (q, ParamIDs::peakCut, 0.5f);
        juce::MemoryBlock noPeakMoveState; juce::AudioProcessor::copyXmlToBinary (*noPeakMove.createXml(), noPeakMoveState);
        q.setStateInformation (noPeakMoveState.getData(), (int) noPeakMoveState.getSize());
        CHECK (std::abs (q.getValueTreeState().getRawParameterValue (ParamIDs::peakCut)->load()) < 1.0e-6f, "a state without a layer parameter loads its default, not the current value");
    }
    auto old = p.getValueTreeState().copyState();
    for (int j=old.getNumChildren()-1; j>=0; --j) {
        const auto id = old.getChild(j).getProperty("id").toString();
        if (id.contains("_delayStyle") || id.contains("_sync") || id.contains("_division") || id.contains("_diffusion") || id.contains("_bias") || id.contains("_replica") || id.contains("_reflections")) old.removeChild(j, nullptr);
    }
    juce::MemoryBlock oldState; juce::AudioProcessor::copyXmlToBinary (*old.createXml(), oldState); q.setStateInformation (oldState.getData(), (int) oldState.getSize());
    CHECK ((int)q.getValueTreeState().getRawParameterValue(rs::fxID(1,"type"))->load() == 1 && (int)q.getValueTreeState().getRawParameterValue(rs::fxID(1,"delayStyle"))->load() == 2 && q.getValueTreeState().getRawParameterValue(rs::fxID(1,"sync"))->load() < 0.5f, "0.7 FX states preserve existing slots and initialize new controls");
}

static void benchmarkScene (int timeMode, bool withFx, size_t maxHeld = 3, bool heavyFx = false)
{
    const auto la = tmp ("scene_flute_12s.wav"), lb = tmp ("scene_fragment_3s.wav");
    if (! la.existsAsFile()) writeTone (la, 523.0, 12.0, 0.4f, 44100.0, 2);
    if (! lb.existsAsFile()) writeTone (lb, 196.0, 3.43, 0.4f, 44100.0, 2);
    Scene s; s.timeMode = timeMode; s.afterPeak = 1;
    auto p = makeProcessor (s, la, lb);
    setParam (*p, L (0, ParamIDs::reverse), 1.0f);
    setParam (*p, ParamIDs::attack, 5.0f); setParam (*p, ParamIDs::release, 1.0f);
    const auto set = [&] (int slot, const char* name, float v) { setParam (*p, rs::fxID (slot, name), v); };
    if (withFx && heavyFx) {
    // The most expensive setting of every effect, both layers: Tape drive, Classic chorus, Shimmer, Tape delay, Vintage compressor.
    p->setFxType (0, 3); set (0, "driveType", (float) rs::drive::tape); set (0, "p1", 60);
    p->setFxType (1, 5); set (1, "chModel", (float) rs::chorus::classic); set (1, "chHQ", 1);
    p->setFxType (2, 4); set (2, "rvMode", (float) rs::reverb::shimmer); set (2, "p1", 60); set (2, "p2", 70); set (2, "rvChar", 70);
    p->setFxType (3, 1); set (3, "delayStyle", 1); set (3, "p2", 50); set (3, "mix", 40);
    p->setFxType (4, 6); set (4, "cpMode", (float) rs::comp::vintage); set (4, "cpTime", (float) rs::comp::opto); set (4, "p1", 40);
    } else if (withFx) {
    p->setFxType (0, 3); set (0, "a", 0); set (0, "p1", 26); set (0, "p2", 100);
    p->setFxType (1, 5); set (1, "a", 0); set (1, "chModel", (float) rs::chorus::sine); set (1, "p2", 100); set (1, "chFeedback", -28); set (1, "p3", 100); set (1, "mix", 26);
    p->setFxType (2, 4); set (2, "rvMode", 4); set (2, "p1", 28); set (2, "p2", 60);
    p->setFxType (3, 1); set (3, "a", 0); set (3, "delayStyle", 3); set (3, "sync", 1); set (3, "amSplice", 313);
    p->setFxType (4, 2); set (4, "eqAir", 91.7f); set (4, "p3", 91.7f);
    }
    for (const int bs : { 128, 256, 512 }) {
        p->releaseResources(); p->setPlayConfigDetails (0, 2, rate, bs); p->prepareToPlay (rate, bs);
        juce::AudioBuffer<float> buffer (2, bs);
        std::vector<double> times;
        const int blocks = (int) (12.0 * rate / bs);
        std::mt19937 rng (3);
        juce::int64 pos = 0, nextNote = 0;
        std::vector<int> held;
        for (int b = 0; b < blocks; ++b, pos += bs) {
            juce::MidiBuffer midi;
            while (nextNote < pos + bs && pos < (juce::int64) (10.0 * rate)) {
                const int offset = (int) juce::jmax ((juce::int64) 0, nextNote - pos);
                if (held.size() > maxHeld) { midi.addEvent (juce::MidiMessage::noteOff (1, held.front()), offset); held.erase (held.begin()); }
                const int note = 48 + (int) (rng() % 24);
                midi.addEvent (juce::MidiMessage::noteOn (1, note, 0.9f), offset); held.push_back (note);
                nextNote += (juce::int64) (rate * (0.05 + 0.1 * (double) (rng() % 100) / 100.0));
            }
            buffer.clear();
            const auto start = std::chrono::steady_clock::now();
            p->processBlock (buffer, midi);
            times.push_back (std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - start).count());
        }
        double total = 0.0; for (auto t : times) total += t;
        auto sorted = times; std::sort (sorted.begin(), sorted.end());
        const double budget = 1000.0 * bs / rate;
        int over = 0; for (auto t : times) if (t > 0.5 * budget) ++over;
        std::cout << "    scene " << (timeMode == 0 ? "Stretch " : "Resample") << (withFx ? (heavyFx ? " +heavyFX" : " +FX") : " dry") << " held " << maxHeld << " block " << bs << ": avg " << juce::String (100.0 * total / (blocks * budget), 1) << " % of one core, p99 "
                  << juce::String (100.0 * sorted[(size_t) (0.99 * (double) sorted.size())] / budget, 0) << " %, worst " << juce::String (100.0 * sorted.back() / budget, 0)
                  << " % of the block, blocks over 50 %: " << over << "\n";
    }
}

static void benchmarkFx()
{
    ReverseSamplerProcessor p; rs::FxRackDSP rack; rack.connect (p.getValueTreeState());
    constexpr int bs = 128;
    for (int slots : { 1, 5 }) {
        for (int i=0; i<5; ++i) {
            const auto set = [&] (const char* name, float v) { setParam (p, rs::fxID (i, name), v); };
            set ("type", i < slots ? 1.0f : 0.0f); set ("a", 1); set ("b", 1);
            set ("delayStyle", 3); set ("p1", 0); set ("p2", 65); set ("mix", 50);
            set ("amSplice", 30); set ("amReverse", 1); set ("reflections", 8);
        }
        rack.prepare (rate); juce::AudioBuffer<float> audio (4, bs);
        std::vector<double> durations; durations.reserve (1000);
        for (int block=0; block<1000; ++block) {
            for (int n=0; n<bs; ++n) for (int ch=0; ch<4; ++ch)
                audio.setSample (ch, n, 0.03f * (float) std::sin (juce::MathConstants<double>::twoPi * (440.0 + ch * 20.0) * (block * bs + n) / rate));
            const auto start = std::chrono::steady_clock::now(); rack.process (audio, bs);
            const auto end = std::chrono::steady_clock::now();
            durations.push_back (std::chrono::duration<double, std::milli> (end-start).count());
        }
        double avg=0; for (auto v : durations) avg += v; avg /= (double) durations.size();
        std::sort (durations.begin(), durations.end());
        std::cout << "FX bench slots=" << slots << " 2 layers 1 reflection 30ms, block 128: avg=" << avg << " ms p99=" << durations[990] << " ms max=" << durations.back() << " ms; budget=" << 1000.0*bs/rate << " ms\n";
    }
}

#include "AudioQualityTests.h"

int main()
{
    juce::ScopedJuceInitialiser_GUI gui;
    if (std::getenv ("RS_FX_BENCH_ONLY") != nullptr) { benchmarkFx(); return 0; }
    if (std::getenv ("RS_SCENE_BENCH_ONLY") != nullptr) { benchmarkScene (0, true, 3); benchmarkScene (0, true, 12); benchmarkScene (1, true, 12); benchmarkScene (1, true, 12, true); return 0; }

    const auto a = tmp ("tone440_4s.wav"), b = tmp ("tone1000_8s.wav");
    writeTone (a, f1, 4.0, 0.5f);
    writeTone (b, f2, 8.0, 0.5f);

    if (std::getenv ("RS_BENCH_ONLY") != nullptr)
    {
        testBenchmark (a, b);
        return 0;
    }

    testLayout (a, b);
    testCut (a, b);
    testFreeze (a, b, 1);
    testFreeze (a, b, 0);
    testLoop (a, b);
    testRelease (a, b);
    testTrim (a, b);
    testState (a, b);
    testFxRack (a, b);
    testFxPresets();
    testModSources();
    testModMatrix (a, b);
    testDelayModes (a);
    testRandom (a, b);
    testResetLayer (a, b);
    testUndo (a, b);
    testMissing (a, b);
    testAsyncLoading (a, b);
    testAsyncProject (a, b);
    testDeclick (a, b);
    testInterpolation();
    testNativeAttack();
    testAudioQuality();
    testUi (a, b);
    testUiTrim (a, b);
    testUiUndo (a, b);
    testUiModulation (a, b);
    if (std::getenv ("RS_SKIP_BENCH") == nullptr)
        testBenchmark (a, b);

    std::cout << "\n" << passes << " passed, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
