#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include "SamplerEngine.h"

#include <cmath>

/** Texts that describe a layer, shared by the layer panels and the layer editor. */
namespace LayerText
{
    inline juce::String seconds (double s)
    {
        return juce::String (s, s < 10.0 ? 2 : 1) + " s";
    }

    inline juce::String describeFile (const rs::SampleData& sample)
    {
        auto text = sample.file.getFileName()
                  + "   |   " + seconds (sample.getLengthInSeconds())
                  + "   |   " + juce::String (sample.sampleRate / 1000.0, 1) + " kHz"
                  + "   |   " + (sample.audio.getNumChannels() > 1 ? "stereo" : "mono");

        if (sample.wasTruncated)
            text += "   |   truncated";

        return text;
    }

    struct Timing
    {
        rs::LayerTiming timing;
        double regionSeconds = 0.0, outputSeconds = 0.0;
        juce::String lengthLine; // "1.50 s -> 4.00 s", for the layer panels
        juce::String modeLine;   // "Stretch x2.67   |   +5 st"
        juce::String details;    // longer, for the layer editor
        juce::String warning;    // empty when everything is fine
    };

    /** A note far from the root changes the pitch a lot: warn about it. Empty when the note is close. */
    inline juce::String farNoteWarning (const rs::LayerSettings& settings, int playedNote)
    {
        if (playedNote < 0)
            return {};

        const auto shift = playedNote - settings.rootNote + settings.pitch;

        if (std::abs (shift) <= 12)
            return {};

        return "Last note " + juce::MidiMessage::getMidiNoteName (playedNote, true, true, 3) + " is " + (shift > 0 ? "+" : "")
               + juce::String (shift) + " st from the root: artefacts are likely.";
    }

    /** How the layer plays at its root note. playedNote (the last played note, or -1) adds a warning
        when it is far from the root. */
    inline Timing describeTiming (const rs::SampleData& sample, const rs::LayerSettings& settings, double hostRate, double bpm,
                                  int playedNote = -1)
    {
        Timing t;
        t.timing = rs::computeLayerTiming (sample, settings, settings.rootNote, hostRate, bpm);
        t.regionSeconds = (t.timing.regionEnd - t.timing.regionStart) / sample.sampleRate;
        t.outputSeconds = t.timing.outputLength / hostRate;

        const auto factor = t.timing.getStretchFactor();
        const auto pitchText = (settings.pitch > 0 ? "+" : "") + juce::String (settings.pitch) + " st";
        const auto region = seconds (t.regionSeconds);
        const auto output = seconds (t.outputSeconds);

        if (settings.timeMode == rs::TimeMode::resample)
        {
            t.lengthLine = region + "  ->  " + output;
            t.modeLine = "Resample   |   " + pitchText;
            t.details = "Region " + region + "  ->  plays " + output + " at the root note. "
                        "Resample: higher notes play shorter, lower notes longer (like tape).";
            t.warning = farNoteWarning (settings, playedNote);
            return t;
        }

        const auto factorText = "x" + juce::String (factor, factor < 10.0 ? 2 : 1);
        t.lengthLine = region + "  ->  " + output;
        t.modeLine = "Stretch " + factorText + "   |   " + pitchText;

        juce::String lengthSource ("the sample length");

        if (settings.lengthMode == rs::LengthMode::seconds)
            lengthSource = "a fixed time";
        else if (settings.lengthMode == rs::LengthMode::beats)
            lengthSource = juce::String (settings.lengthBeats, 2) + " beats at " + juce::String (bpm, 1) + " BPM";

        t.details = "Region " + region + "  ->  plays " + output + " (" + factorText + ", set by " + lengthSource + "), on every note.";

        if (! t.timing.useStretcher)
            t.details << " The root note plays the region untouched.";

        if (t.timing.stretchLimited)
            t.warning = "Limit reached: a region can play at most 16x longer or 8x shorter.";
        else if (factor >= 3.0 || factor <= 1.0 / 3.0)
            t.warning = "Strong time-stretch (" + factorText + "): artefacts are likely.";
        else
            t.warning = farNoteWarning (settings, playedNote);

        return t;
    }
}
