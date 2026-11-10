#include "PluginProcessor.h"
#include "PluginEditor.h"

namespace
{
    constexpr int overviewBins = 4096;
    constexpr auto legacySamplePathProperty = "samplePath";   // v0.1 (one layer)

    juce::String samplePathProperty (int layer)   { return "layer" + juce::String (layer + 1) + "Path"; }
    juce::String layerName (int layer)            { return "Layer " + juce::String (layer + 1); }
    juce::String envelopeProperty (int index)
    {
        return index < rs::numLayers ? "envelope" + juce::String (index + 1)
                                     : juce::String (index == rs::numLayers ? "envelopeMain" : "envelopeDissolve");
    }

    //==============================================================================
    // On/off parameter that always holds exactly 0 or 1.
    // juce::AudioParameterBool keeps whatever normalised value the host sends (e.g. 0.37) while the
    // saved state only has 0/1, so restoring the same state could report different values depending
    // on the value before the restore (found by pluginval "Discrete parameter state restoration").
    // Same IDs, same saved values (0/1) and same texts, so old projects load unchanged.
    class SwitchParameter final : public juce::RangedAudioParameter
    {
    public:
        SwitchParameter (const juce::ParameterID& parameterID, const juce::String& parameterName, bool defaultOn)
            : juce::RangedAudioParameter (parameterID, parameterName, juce::AudioProcessorParameterWithIDAttributes{}),
              defaultValue (defaultOn ? 1.0f : 0.0f),
              value (defaultValue)
        {
        }

        float getValue() const override                   { return value.load (std::memory_order_relaxed); }
        void setValue (float newValue) override           { value.store (newValue >= 0.5f ? 1.0f : 0.0f, std::memory_order_relaxed); }
        float getDefaultValue() const override            { return defaultValue; }
        int getNumSteps() const override                  { return 2; }
        bool isDiscrete() const override                  { return true; }
        bool isBoolean() const override                   { return true; }
        const juce::NormalisableRange<float>& getNormalisableRange() const override { return range; }

        juce::String getText (float v, int maximumLength) const override
        {
            return juce::String (v >= 0.5f ? "On" : "Off").substring (0, juce::jmax (0, maximumLength));
        }

        float getValueForText (const juce::String& text) const override
        {
            const auto t = text.trim().toLowerCase();

            if (t == "on" || t == "yes" || t == "true")    return 1.0f;
            if (t == "off" || t == "no" || t == "false")   return 0.0f;

            return t.getFloatValue() >= 0.5f ? 1.0f : 0.0f;
        }

    private:
        const juce::NormalisableRange<float> range { 0.0f, 1.0f, 1.0f };
        const float defaultValue;
        std::atomic<float> value;
    };

    // Keep the old parameter's ID, index and normalisation range for DAW sessions,
    // but ignore obsolete automation and saved counts. Only one generation is supported.
    class SingleReflectionParameter final : public juce::AudioParameterInt
    {
    public:
        SingleReflectionParameter (const juce::ParameterID& id, const juce::String& name)
            : juce::AudioParameterInt (id, name, 1, 8, 1,
                                      juce::AudioParameterIntAttributes().withAutomatable (false)) {}
    private:
        void setValue (float) override {}
    };

    //==============================================================================
    // Value <-> text helpers. Units are part of the text, so hosts and knobs show the same thing.
    juce::String formatSeconds (float seconds)
    {
        return seconds < 10.0f ? juce::String (seconds, 2) + " s" : juce::String (seconds, 1) + " s";
    }

    juce::String formatFrequency (float hz)
    {
        return hz < 1000.0f ? juce::String (juce::roundToInt (hz)) + " Hz" : juce::String (hz / 1000.0f, 2) + " kHz";
    }

    float parseFrequency (const juce::String& text, float offValue)
    {
        if (text.trim().equalsIgnoreCase ("off"))
            return offValue;

        const auto value = text.getFloatValue();
        return text.containsIgnoreCase ("k") ? value * 1000.0f : value;
    }

    int parseNoteName (const juce::String& text)
    {
        const auto t = text.trim().toUpperCase();

        if (t.isNotEmpty() && t.containsOnly ("0123456789"))
            return juce::jlimit (0, 127, t.getIntValue());

        if (t.isEmpty() || t[0] < 'A' || t[0] > 'G')
            return 60;

        static constexpr int offsets[] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
        int note = offsets[t[0] - 'A'];
        int index = 1;

        if (t[index] == '#')       { ++note; ++index; }
        else if (t[index] == 'B')  { --note; ++index; }   // flat, e.g. "Bb2"

        // Octave numbers follow the "middle C = C3" convention, so MIDI 0 is C-2.
        return juce::jlimit (0, 127, (t.substring (index).getIntValue() + 2) * 12 + note);
    }

    juce::NormalisableRange<float> skewedRange (float min, float max, float centre, float interval = 0.0f)
    {
        juce::NormalisableRange<float> range (min, max, interval);
        range.setSkewForCentre (centre);
        return range;
    }

    juce::NormalisableRange<float> logRange (float min, float max)
    {
        return { min, max,
                 [] (float start, float end, float normalised) { return start * std::pow (end / start, normalised); },
                 [] (float start, float end, float value)      { return std::log (value / start) / std::log (end / start); },
                 [] (float start, float end, float value)      { return juce::jlimit (start, end, value); } };
    }

    juce::NormalisableRange<float> frequencyRange()
    {
        return { 20.0f, 20000.0f,
                 [] (float start, float end, float normalised) { return start * std::pow (end / start, normalised); },
                 [] (float start, float end, float value)      { return std::log (value / start) / std::log (end / start); },
                 [] (float start, float end, float value)      { return juce::jlimit (start, end, value); } };
    }

    using FloatAttributes = juce::AudioParameterFloatAttributes;
    using IntAttributes   = juce::AudioParameterIntAttributes;

    /** Counts up while it exists: changes made meanwhile are not recorded for undo. */
    struct ScopedRestore
    {
        explicit ScopedRestore (std::atomic<int>& c) : counter (c)   { ++counter; }
        ~ScopedRestore()                                             { --counter; }
        std::atomic<int>& counter;
    };

    constexpr juce::uint32 undoQuietMs = 400;                       // edits closer than this make one step
    constexpr size_t undoSampleMemory = (size_t) 256 * 1024 * 1024;   // audio kept alive by the undo history

    bool sameValue (float a, float b) noexcept   { return std::abs (a - b) <= 1.0e-6f; }
}

//==============================================================================
ReverseSamplerProcessor::ReverseSamplerProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "ReverseSampler", createParameterLayout())
{
    formatManager.registerBasicFormats();
    modMatrix.connect (apvts);
    fxRack.connect (apvts, [this] (const juce::String& id) { return modMatrix.resolve (id); });

    gainParam = modMatrix.resolve (ParamIDs::gain);

    rs::GlobalParameters globals;
    globals.attackMs  = apvts.getRawParameterValue (ParamIDs::attack);
    globals.releaseMs = apvts.getRawParameterValue (ParamIDs::release);
    globals.releaseCurve    = apvts.getRawParameterValue (ParamIDs::releaseCurve);
    globals.playMode        = playModeParam  = apvts.getRawParameterValue (ParamIDs::playMode);
    globals.afterPeak       = afterPeakParam = apvts.getRawParameterValue (ParamIDs::afterPeak);
    globals.dissolveSeconds = dissolveParam  = apvts.getRawParameterValue (ParamIDs::dissolve);
    globals.peakCutSeconds  = peakCutParam   = apvts.getRawParameterValue (ParamIDs::peakCut);
    globals.arriveLatency   = &arriveLatency;
    globals.mainEnvelope    = &envelopes[(size_t) mainEnvelope];
    globals.dissolveEnvelope = &envelopes[(size_t) dissolveEnvelope];
    envelopes[(size_t) dissolveEnvelope].set (getDefaultEnvelope (dissolveEnvelope));

    for (int layer = 0; layer < numLayers; ++layer)
    {
        const auto raw = [this, layer] (const char* name) { return apvts.getRawParameterValue (ParamIDs::layerID (layer, name)); };

        auto& p = layerParams[(size_t) layer];
        p.enabled       = raw (ParamIDs::on);
        p.reverse       = raw (ParamIDs::reverse);
        p.timeMode      = raw (ParamIDs::timeMode);
        p.lengthMode    = raw (ParamIDs::lengthMode);
        p.lengthSeconds = raw (ParamIDs::seconds);
        p.lengthBeats   = raw (ParamIDs::beats);
        p.pitch         = raw (ParamIDs::pitch);
        p.rootNote      = raw (ParamIDs::root);
        p.start         = raw (ParamIDs::start);
        p.end           = raw (ParamIDs::end);
        p.peakOffset    = raw (ParamIDs::peakOffset);
        globals.layerEnvelopes[(size_t) layer] = &envelopes[(size_t) layer];

        const auto modulated = [this, layer] (const char* name) { return modMatrix.resolve (ParamIDs::layerID (layer, name)); };
        rs::LayerBusParameters bus;
        bus.levelDb   = modulated (ParamIDs::level);
        bus.pan       = modulated (ParamIDs::pan);
        bus.width     = modulated (ParamIDs::width);
        bus.lowCutHz  = modulated (ParamIDs::lowCut);
        bus.highCutHz = modulated (ParamIDs::highCut);
        layerBuses[(size_t) layer].setParameters (bus);
    }

    std::array<rs::StretchStartCache*, numLayers> caches {};
    for (int layer = 0; layer < numLayers; ++layer)
        caches[(size_t) layer] = &startCaches[(size_t) layer];

    for (int i = 0; i < numVoices; ++i)
    {
        auto* voice = new rs::SamplerVoice (globals, layerParams, bpm);
        voice->setStartCaches (caches);
        synth.addSamplerVoice (voice);
    }

    auditionVoice = new rs::SamplerVoice (globals, layerParams, bpm);
    auditionVoice->setStartCaches (caches);
    auditionSynth.addSamplerVoice (auditionVoice);

    installSamples ({});

    // Undo follows the gestures of every parameter.
    allParameters = getParameters();
    parameterBefore.assign ((size_t) allParameters.size(), 0.0f);
    parameterTouched.assign ((size_t) allParameters.size(), 0);
    parameterGestures.assign ((size_t) allParameters.size(), 0);

    parameterMovesNotes.assign ((size_t) allParameters.size(), 0);

    for (int i = 0; i < allParameters.size(); ++i)
    {
        const auto id = dynamic_cast<juce::AudioProcessorParameterWithID*> (allParameters[i]) != nullptr
                      ? dynamic_cast<juce::AudioProcessorParameterWithID*> (allParameters[i])->paramID : juce::String();
        // FX and modulation never move a note; everything else (layers, play mode, peak cut...) can.
        parameterMovesNotes[(size_t) i] = id.startsWith ("fx") || id.startsWith ("mod") || id.startsWith ("lfo") || id.startsWith ("macro") ? 0 : 1;
    }

    for (auto* param : allParameters)
        param->addListener (this);

    startTimerHz (10);   // keeps the Arrive latency up to date and closes undo steps
}

ReverseSamplerProcessor::~ReverseSamplerProcessor()
{
    stopTimer();
    warmer.stopThread (5000);

    // Cancels the background loads (they stop between two chunks) and waits for the worker.
    for (auto& serial : loadSerials)
        ++serial;

    loader.removeAllJobs (true, 10000);

    for (auto* param : allParameters)
        param->removeListener (this);
}

void ReverseSamplerProcessor::timerCallback()
{
    updateLatency();

    bool ready = false;

    {
        const juce::ScopedLock sl (undoLock);

        if (editPending)
        {
            const auto quiet = juce::Time::getMillisecondCounter() - lastEditTime;
            const auto mouseDown = juce::ModifierKeys::currentModifiers.isAnyMouseButtonDown();

            // A gesture that was begun but never ended must not block undo for good.
            if (openGestures > 0 && ! mouseDown && quiet > 3000)
            {
                std::fill (parameterGestures.begin(), parameterGestures.end(), 0);
                openGestures = 0;
            }

            ready = openGestures == 0 && ! mouseDown && quiet >= undoQuietMs && ! anyUserLoadPending();
        }
    }

    if (ready)
        commitPendingEdit();
}

juce::AudioProcessorValueTreeState::ParameterLayout ReverseSamplerProcessor::createParameterLayout()
{
    using namespace juce;
    AudioProcessorValueTreeState::ParameterLayout layout;

    const auto msText = [] (float ms, int) { return (ms < 100.0f ? String (ms, 1) : String (roundToInt (ms))) + " ms"; };
    const auto dbText = [] (float db, int) { return db <= -60.0f ? String ("-inf dB") : String (db, 1) + " dB"; };
    const auto dbValue = [] (const String& text) { return text.containsIgnoreCase ("inf") ? -60.0f : text.getFloatValue(); };

    // Global (IDs and version 1 kept from v0.1, so old projects still load).
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::attack, 1 }, "Attack",
                                                       skewedRange (0.0f, 10000.0f, 500.0f), 0.0f,
                                                       FloatAttributes().withStringFromValueFunction (msText)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::release, 1 }, "Release",
                                                       skewedRange (1.0f, 10000.0f, 500.0f), 300.0f,
                                                       FloatAttributes().withStringFromValueFunction (msText)));
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::gain, 1 }, "Volume",
                                                       NormalisableRange<float> (-48.0f, 12.0f, 0.1f), 0.0f,
                                                       FloatAttributes().withStringFromValueFunction ([] (float db, int) { return String (db, 1) + " dB"; })));

    // Added in 0.3.
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::releaseCurve, 3 }, "Release Curve",
                                                       NormalisableRange<float> (-100.0f, 100.0f, 1.0f), 0.0f,
                                                       FloatAttributes().withStringFromValueFunction ([] (float v, int)
                                                       {
                                                           const auto amount = roundToInt (v);
                                                           return amount == 0 ? String ("Linear") : (amount > 0 ? "+" : "") + String (amount);
                                                       })));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::playMode, 3 }, "Mode", StringArray { "Swell", "Arrive" }, 0));
    layout.add (std::make_unique<AudioParameterChoice> (ParameterID { ParamIDs::afterPeak, 3 }, "After Peak",
                                                        StringArray { "Cut", "Release", "Freeze", "Loop" }, 0));   // "Release" was "Dissolve" before 0.4.1
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::dissolve, 3 }, "After Peak Fade",
                                                       skewedRange (0.1f, 20.0f, 3.0f), 3.0f,
                                                       FloatAttributes().withStringFromValueFunction ([] (float v, int) { return formatSeconds (v); })));

    // Added in 0.4: the peak moved earlier (every build ends that much sooner).
    layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::peakCut, 4 }, "Peak Move",
                                                       skewedRange (0.0f, 30.0f, 2.0f), 0.0f,
                                                       FloatAttributes().withStringFromValueFunction ([] (float v, int)
                                                                         {
                                                                             return v < 0.0005f ? String ("0.000 s") : "-" + String (v, v < 10.0f ? 3 : 2) + " s";
                                                                         })
                                                                        .withValueFromStringFunction ([] (const String& t) { return std::abs (t.getFloatValue()); })));

    for (int layer = 0; layer < numLayers; ++layer)
    {
        const auto id   = [layer] (const char* name) { return ParameterID { ParamIDs::layerID (layer, name), 2 }; };
        const auto name = [layer] (const char* text) { return layerName (layer) + " " + text; };

        layout.add (std::make_unique<SwitchParameter> (id (ParamIDs::on), name ("On"), true));
        layout.add (std::make_unique<SwitchParameter> (id (ParamIDs::reverse), name ("Reverse"), false));

        layout.add (std::make_unique<AudioParameterChoice> (id (ParamIDs::timeMode), name ("Time Mode"),
                                                            StringArray { "Stretch", "Resample" }, 0));
        layout.add (std::make_unique<AudioParameterChoice> (id (ParamIDs::lengthMode), name ("Length Mode"),
                                                            StringArray { "Sample", "Seconds", "Beats" }, 0));
        layout.add (std::make_unique<AudioParameterFloat> (id (ParamIDs::seconds), name ("Length Seconds"),
                                                           skewedRange (0.1f, 60.0f, 4.0f), 4.0f,
                                                           FloatAttributes().withStringFromValueFunction ([] (float v, int) { return formatSeconds (v); })));
        layout.add (std::make_unique<AudioParameterFloat> (id (ParamIDs::beats), name ("Length Beats"),
                                                           skewedRange (0.25f, 64.0f, 8.0f, 0.25f), 4.0f,
                                                           FloatAttributes().withStringFromValueFunction ([] (float v, int)
                                                           {
                                                               return String (v, std::abs (v - std::round (v)) < 0.01f ? 0 : 2) + (std::abs (v - 1.0f) < 0.01f ? " beat" : " beats");
                                                           })));

        layout.add (std::make_unique<AudioParameterInt> (id (ParamIDs::pitch), name ("Pitch"), -24, 24, 0,
                                                         IntAttributes().withStringFromValueFunction ([] (int v, int) { return (v > 0 ? "+" : "") + String (v) + " st"; })
                                                                        .withValueFromStringFunction ([] (const String& t) { return t.getIntValue(); })));
        layout.add (std::make_unique<AudioParameterInt> (id (ParamIDs::root), name ("Root Note"), 0, 127, 60,
                                                         IntAttributes().withStringFromValueFunction ([] (int v, int) { return MidiMessage::getMidiNoteName (v, true, true, 3); })
                                                                        .withValueFromStringFunction ([] (const String& t) { return parseNoteName (t); })));

        const auto percentText = [] (float v, int) { return String (v, 1) + " %"; };
        layout.add (std::make_unique<AudioParameterFloat> (id (ParamIDs::start), name ("Start"), NormalisableRange<float> (0.0f, 100.0f), 0.0f,
                                                           FloatAttributes().withStringFromValueFunction (percentText)));
        layout.add (std::make_unique<AudioParameterFloat> (id (ParamIDs::end), name ("End"), NormalisableRange<float> (0.0f, 100.0f), 100.0f,
                                                           FloatAttributes().withStringFromValueFunction (percentText)));

        layout.add (std::make_unique<AudioParameterFloat> (id (ParamIDs::level), name ("Level"), NormalisableRange<float> (-60.0f, 12.0f, 0.1f), 0.0f,
                                                           FloatAttributes().withStringFromValueFunction (dbText).withValueFromStringFunction (dbValue)));
        layout.add (std::make_unique<AudioParameterFloat> (id (ParamIDs::pan), name ("Pan"), NormalisableRange<float> (-100.0f, 100.0f, 1.0f), 0.0f,
                                                           FloatAttributes().withStringFromValueFunction ([] (float v, int)
                                                           {
                                                               const auto amount = roundToInt (std::abs (v));
                                                               return amount == 0 ? String ("C") : (v < 0.0f ? "L" : "R") + String (amount);
                                                           })
                                                           .withValueFromStringFunction ([] (const String& t)
                                                           {
                                                               const auto value = std::abs (t.retainCharacters ("0123456789.").getFloatValue());
                                                               return t.trim().startsWithIgnoreCase ("l") || t.trim().startsWith ("-") ? -value : value;
                                                           })));
        layout.add (std::make_unique<AudioParameterFloat> (id (ParamIDs::width), name ("Width"), NormalisableRange<float> (0.0f, 200.0f, 1.0f), 100.0f,
                                                           FloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (roundToInt (v)) + " %"; })));
        layout.add (std::make_unique<AudioParameterFloat> (id (ParamIDs::lowCut), name ("Low Cut"), frequencyRange(), 20.0f,
                                                           FloatAttributes().withStringFromValueFunction ([] (float v, int) { return v <= 20.5f ? String ("Off") : formatFrequency (v); })
                                                                            .withValueFromStringFunction ([] (const String& t) { return parseFrequency (t, 20.0f); })));
        layout.add (std::make_unique<AudioParameterFloat> (id (ParamIDs::highCut), name ("High Cut"), frequencyRange(), 20000.0f,
                                                           FloatAttributes().withStringFromValueFunction ([] (float v, int) { return v >= 19950.0f ? String ("Off") : formatFrequency (v); })
                                                                            .withValueFromStringFunction ([] (const String& t) { return parseFrequency (t, 20000.0f); })));
        layout.add (std::make_unique<AudioParameterFloat> (ParameterID { ParamIDs::layerID (layer, ParamIDs::peakOffset), 3 }, name ("Peak Offset"),
                                                           NormalisableRange<float> (-8.0f, 8.0f), 0.0f,
                                                           FloatAttributes().withStringFromValueFunction ([] (float v, int)
                                                           {
                                                               return std::abs (v) < 0.0005f ? String ("0.000 s") : (v > 0.0f ? "+" : "") + String (v, 3) + " s";
                                                           })));
    }

    // Stable slot-based parameters: reordering swaps slot contents, not host automation lanes.
    for (int slot = 0; slot < rs::fxSlots; ++slot)
    {
        const auto id = [slot] (const char* n) { return ParameterID { rs::fxID (slot, n), 7 }; };
        const auto name = [slot] (const char* n) { return "FX " + String (slot + 1) + " " + n; };
        layout.add (std::make_unique<AudioParameterChoice> (id("type"), name("Type"), StringArray { "Empty", "Delay", "EQ", "Drive", "Reverb", "Chorus", "Compressor" }, 0));
        layout.add (std::make_unique<SwitchParameter> (id("a"), name("Layer A"), true));
        layout.add (std::make_unique<SwitchParameter> (id("b"), name("Layer B"), true));
        for (const auto* n : { "p1", "p2", "p3", "mix" })
            layout.add (std::make_unique<AudioParameterFloat> (id(n), name(n), NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f));
    }
    // New version-hint: previous parameter IDs and their host order are unchanged.
    for (int slot = 0; slot < rs::fxSlots; ++slot)
    {
        const auto id = [slot] (const char* n) { return ParameterID { rs::fxID (slot, n), 8 }; };
        const auto name = [slot] (const char* n) { return "FX " + String (slot + 1) + " " + n; };
        layout.add (std::make_unique<AudioParameterChoice> (id("delayStyle"), name("Delay Style"), StringArray { "Reverse", "Tape", "Replica", "Amethyst" }, 2));
        layout.add (std::make_unique<SwitchParameter> (id("sync"), name("Delay Sync"), false));
        layout.add (std::make_unique<AudioParameterChoice> (id("division"), name("Delay Division"), rs::delayDivisions(), 10));
        layout.add (std::make_unique<AudioParameterFloat> (id("diffusion"), name("Reverse Diffusion"), NormalisableRange<float> (0.0f, 100.0f, 0.1f), 0.0f));
        layout.add (std::make_unique<AudioParameterFloat> (id("bias"), name("Tape Bias"), NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f));
        layout.add (std::make_unique<AudioParameterChoice> (id("replica"), name("Replica Character"), StringArray { "Vintage", "Modern" }, 1));
        layout.add (std::make_unique<SingleReflectionParameter> (id("reflections"), name("Amethyst Reflections")));
    }
    // Append after every 0.8 parameter, preserving host indices and IDs.
    for (int slot = 0; slot < rs::fxSlots; ++slot)
    {
        const auto id = [slot] (const char* n) { return ParameterID { rs::fxID (slot, n), 9 }; };
        const auto name = [slot] (const char* n) { return "FX " + String (slot + 1) + " " + n; };
        layout.add (std::make_unique<AudioParameterFloat> (id("amSplice"), name("Amethyst Splice"), NormalisableRange<float> (150.0f, 700.0f, 1.0f), 350.0f));
        layout.add (std::make_unique<SwitchParameter> (id("amReverse"), name("Amethyst Reverse"), true));
        layout.add (std::make_unique<SwitchParameter> (id("replicaBrown"), name("Replica Hi Cut"), false));
    }
    // v0.10 EQ: Low/Mid/Treble stay on p1..p3, so existing automation keeps working.
    for (int slot = 0; slot < rs::fxSlots; ++slot)
    {
        const auto id = [slot] (const char* n) { return ParameterID { rs::fxID (slot, n), 10 }; };
        const auto name = [slot] (const char* n) { return "FX " + String (slot + 1) + " " + n; };
        layout.add (std::make_unique<AudioParameterFloat> (id("eqAir"), name("EQ Air"), NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f));
        layout.add (std::make_unique<AudioParameterFloat> (id("eqLowCut"), name("EQ Low Cut"), logRange (20.0f, 2000.0f), 20.0f,
                                                           FloatAttributes().withStringFromValueFunction ([] (float v, int) { return v <= rs::eq::lowCutOffHz ? String ("Off") : formatFrequency (v); })
                                                                            .withValueFromStringFunction ([] (const String& t) { return parseFrequency (t, 20.0f); })));
        layout.add (std::make_unique<AudioParameterFloat> (id("eqHighCut"), name("EQ High Cut"), logRange (1000.0f, 20000.0f), 20000.0f,
                                                           FloatAttributes().withStringFromValueFunction ([] (float v, int) { return v >= rs::eq::highCutOffHz ? String ("Off") : formatFrequency (v); })
                                                                            .withValueFromStringFunction ([] (const String& t) { return parseFrequency (t, 20000.0f); })));
        layout.add (std::make_unique<AudioParameterChoice> (id("eqMode"), name("EQ Mode"), StringArray { "Vintage", "Modern" }, 0));
    }
    // v0.11 Drive: full-band, 4 types. Drive/Tone/Output/Mix stay on p1..p3/mix.
    for (int slot = 0; slot < rs::fxSlots; ++slot)
        layout.add (std::make_unique<AudioParameterChoice> (ParameterID { rs::fxID (slot, "driveType"), 11 }, "FX " + String (slot + 1) + " Drive Type",
                                                            StringArray { "Tube", "Soft Clip", "Tape", "Triode" }, 0));
    // v0.13 Reverb: Size/Decay/Tone/Mix stay on p1..p3/mix; mode, pre-delay, low cut and Character are new.
    for (int slot = 0; slot < rs::fxSlots; ++slot)
    {
        const auto id = [slot] (const char* n) { return ParameterID { rs::fxID (slot, n), 12 }; };
        const auto name = [slot] (const char* n) { return "FX " + String (slot + 1) + " " + n; };
        layout.add (std::make_unique<AudioParameterChoice> (id("rvMode"), name("Reverb Mode"),
                                                            StringArray { "Room", "Plate", "Ambience", "Terrain", "Bloom", "Shimmer" }, 0));
        layout.add (std::make_unique<AudioParameterFloat> (id("rvPreDelay"), name("Reverb Pre-Delay"), NormalisableRange<float> (0.0f, (float) rs::reverb::maxPreDelayMs, 0.1f, 0.5f), 10.0f,
                                                           FloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, v < 10.0f ? 1 : 0) + " ms"; })
                                                                            .withValueFromStringFunction ([] (const String& t) { return t.retainCharacters ("0123456789.,").replaceCharacter (',', '.').getFloatValue(); })));
        layout.add (std::make_unique<AudioParameterFloat> (id("rvLowCut"), name("Reverb Low Cut"), logRange (20.0f, 1000.0f), 80.0f,
                                                           FloatAttributes().withStringFromValueFunction ([] (float v, int) { return formatFrequency (v); })
                                                                            .withValueFromStringFunction ([] (const String& t) { return parseFrequency (t, 20.0f); })));
        layout.add (std::make_unique<AudioParameterFloat> (id("rvChar"), name("Reverb Character"), NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f));
    }
    for (int slot = 0; slot < rs::fxSlots; ++slot)
    {
        const auto id = [slot] (const char* n) { return ParameterID { rs::fxID (slot, n), 13 }; };
        const auto name = [slot] (const char* n) { return "FX " + String (slot + 1) + " " + n; };
        const auto decibels = FloatAttributes().withStringFromValueFunction ([] (float v, int) { return (v > 0.05f ? "+" : "") + String (v, 1) + " dB"; })
                                               .withValueFromStringFunction ([] (const String& t) { return t.retainCharacters ("0123456789.,-+").replaceCharacter (',', '.').getFloatValue(); });
        const auto percent = FloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " %"; });
        StringArray models;
        for (const auto* model : rs::chorus::modelNames) models.add (model);
        layout.add (std::make_unique<AudioParameterChoice> (id("chModel"), name("Chorus Model"), models, 0));
        layout.add (std::make_unique<AudioParameterInt> (id("chMode"), name("Chorus Mode"), 0, 6, 0));
        layout.add (std::make_unique<SwitchParameter> (id("chVariant"), name("Chorus High/Dark"), false));
        layout.add (std::make_unique<SwitchParameter> (id("chStereo"), name("Chorus Dual"), false));
        layout.add (std::make_unique<AudioParameterFloat> (id("chDelay"), name("Chorus Delay"), NormalisableRange<float> (rs::chorus::minDelayMs, rs::chorus::maxDelayMs, 0.1f, 0.5f), 15.0f,
                                                           FloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " ms"; })
                                                                            .withValueFromStringFunction ([] (const String& t) { return t.retainCharacters ("0123456789.,").replaceCharacter (',', '.').getFloatValue(); })));
        layout.add (std::make_unique<AudioParameterFloat> (id("chFeedback"), name("Chorus Feedback"), NormalisableRange<float> (-95.0f, 95.0f, 0.1f), 0.0f, percent));
        layout.add (std::make_unique<AudioParameterInt> (id("chVoices"), name("Chorus Voices"), rs::chorus::minVoices, rs::chorus::maxVoices, 4));
        layout.add (std::make_unique<AudioParameterFloat> (id("chOutput"), name("Chorus Output"), NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f, decibels));
        layout.add (std::make_unique<SwitchParameter> (id("chHQ"), name("Chorus HQ"), true));
        layout.add (std::make_unique<AudioParameterFloat> (id("chVibRate"), name("Chorus Vibrato Rate"), NormalisableRange<float> (0.0f, 100.0f, 0.1f), 50.0f, percent));
        layout.add (std::make_unique<AudioParameterFloat> (id("chVibDepth"), name("Chorus Vibrato Depth"), NormalisableRange<float> (0.0f, 100.0f, 0.1f), 30.0f, percent));
    }
    for (int slot = 0; slot < rs::fxSlots; ++slot)
    {
        const auto id = [slot] (const char* n) { return ParameterID { rs::fxID (slot, n), 14 }; };
        const auto name = [slot] (const char* n) { return "FX " + String (slot + 1) + " " + n; };
        const auto milliseconds = FloatAttributes().withStringFromValueFunction ([] (float v, int) { return v < 1.0f ? String (v, 2) + " ms" : v < 100.0f ? String (v, 1) + " ms" : String (v, 0) + " ms"; })
                                                   .withValueFromStringFunction ([] (const String& t) { return t.retainCharacters ("0123456789.,").replaceCharacter (',', '.').getFloatValue() * (t.containsIgnoreCase ("ms") || ! t.containsIgnoreCase ("s") ? 1.0f : 1000.0f); });
        layout.add (std::make_unique<AudioParameterChoice> (id("cpMode"), name("Compressor Mode"), StringArray { "Vintage", "Modern" }, 0));
        layout.add (std::make_unique<AudioParameterFloat> (id("cpKnee"), name("Compressor Knee"), NormalisableRange<float> (0.0f, rs::comp::maxKneeDb, 0.1f), 6.0f,
                                                           FloatAttributes().withStringFromValueFunction ([] (float v, int) { return String (v, 1) + " dB"; })
                                                                            .withValueFromStringFunction ([] (const String& t) { return t.retainCharacters ("0123456789.,").replaceCharacter (',', '.').getFloatValue(); })));
        layout.add (std::make_unique<AudioParameterFloat> (id("cpAttack"), name("Compressor Attack"), logRange (rs::comp::minAttackMs, rs::comp::maxAttackMs), 10.0f, milliseconds));
        layout.add (std::make_unique<AudioParameterFloat> (id("cpRelease"), name("Compressor Release"), logRange (rs::comp::minReleaseMs, rs::comp::maxReleaseMs), 150.0f, milliseconds));
        StringArray times;
        for (const auto* t : rs::comp::timeNames) times.add (t);
        layout.add (std::make_unique<AudioParameterChoice> (id("cpTime"), name("Compressor Time Constant"), times, 1));
    }
    rs::ModMatrix::addParameters (layout);
    return layout;
}

//==============================================================================
const juce::String ReverseSamplerProcessor::getName() const
{
    return JucePlugin_Name;
}

void ReverseSamplerProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // The worker's players are prepared here, so it must be idle; it starts again below.
    warmer.stopThread (5000);
    for (auto& cache : startCaches)
        cache.clear();
    for (auto& player : warmPlayers)
        player.prepare (sampleRate);

    synth.setCurrentPlaybackSampleRate (sampleRate);
    auditionSynth.setCurrentPlaybackSampleRate (sampleRate);
    auditionHeld = false;
    keyboardState.reset();
    synth.silenceAll();           // a new start: nothing from before keeps playing
    auditionSynth.silenceAll();
    bypassed = false;

    layerBuffer.setSize (2 * numLayers, juce::jmax (1, samplesPerBlock));

    modMatrix.prepare (sampleRate);

    for (auto& bus : layerBuses)
        bus.prepare (sampleRate, samplesPerBlock);

    fxRack.prepare (sampleRate, samplesPerBlock);
    lastGainDb = gainParam->load();
    lastGain = lastGainTarget = juce::Decibels::decibelsToGain (lastGainDb);
    updateLatency();
    invalidateWarmStarts();
    warmer.startThread (juce::Thread::Priority::normal);
}

//==============================================================================
// Warm starts. One pass warms the stretched layers of the notes nearest to centreNote, nearest
// first, as many as the caches hold. A note's layout is computed exactly as SamplerVoice::startNote
// does it, so the worker's players start with the same key the voices build, and the stored state
// is what the voice would have computed itself.
int ReverseSamplerProcessor::warmStretchStarts (int centreNote)
{
    const std::lock_guard<std::mutex> guard (warmMutex);   // the worker and a direct call never share the players
    std::array<rs::SampleDataPtr, numLayers> held;
    std::array<const rs::SampleData*, numLayers> pointers {};
    std::array<rs::LayerSettings, numLayers> settings;
    bool anyStretch = false;

    for (int layer = 0; layer < numLayers; ++layer)
    {
        held[(size_t) layer] = getSample (layer);
        pointers[(size_t) layer] = held[(size_t) layer].get();
        settings[(size_t) layer] = getLayerSettings (layer);
        anyStretch = anyStretch || (pointers[(size_t) layer] != nullptr && settings[(size_t) layer].enabled && settings[(size_t) layer].timeMode == rs::TimeMode::stretch);
    }

    if (! anyStretch)
        return 0;

    const auto hostRate = getPlaybackRate();
    const auto tempo = bpm.load();
    const auto mode = getPlayMode();
    const auto latency = mode == rs::PlayMode::arrive ? (double) arriveLatency.load() : 0.0;
    const auto cutSeconds = getPeakCutSeconds();
    const auto afterPeak = getAfterPeak();
    const int centre = juce::jlimit (0, 127, centreNote < 0 ? 60 : centreNote);
    int warmed = 0;

    for (int step = 0; step < 2 * rs::StretchStartCache::capacity; ++step)
    {
        // 0, +1, -1, +2, -2 ...: nearest first
        const int note = centre + ((step + 1) / 2) * ((step % 2) != 0 ? 1 : -1);

        if (! juce::isPositiveAndBelow (note, 128) || warmer.threadShouldExit())
            continue;

        const auto layout = rs::computeNoteLayout (pointers, settings, note, mode, latency, hostRate, tempo, cutSeconds);

        if (! layout.anyLayer)
            continue;

        for (int layer = 0; layer < numLayers; ++layer)
        {
            const auto& l = layout.layers[(size_t) layer];

            if (pointers[(size_t) layer] == nullptr || ! l.active || ! l.timing.useStretcher)
                continue;

            auto& player = warmPlayers[(size_t) layer];
            const auto skip = l.start < 0.0 ? (juce::int64) std::llround (-l.start) : (juce::int64) 0;
            player.start (*pointers[(size_t) layer], l.timing, settings[(size_t) layer].reversed, rs::Envelope {},
                          rs::LayerPlayer::PeakHold { afterPeak, l.length, l.reachesPeak }, skip);
            player.stop();

            if (startCaches[(size_t) layer].store (player, note, centre))
                ++warmed;
        }
    }

    return warmed;
}

void ReverseSamplerProcessor::StretchWarmer::run()
{
    unsigned done = 0;
    int centre = -1;

    while (! threadShouldExit())
    {
        const auto wanted = owner.warmGeneration.load (std::memory_order_acquire);
        const int played = owner.lastNote.load (std::memory_order_relaxed);
        const bool moved = played >= 0 && (centre < 0 || std::abs (played - centre) > 12);   // the player went elsewhere: follow

        if (wanted == done && ! moved)
        {
            wait (50);
            continue;
        }

        if (wanted != done)
        {
            wait (60);   // a knob being turned settles first

            if (threadShouldExit())
                break;
        }

        const auto generation = owner.warmGeneration.load (std::memory_order_acquire);

        if (generation != done)
            for (auto& cache : owner.startCaches)
                cache.clear();

        centre = owner.lastNote.load (std::memory_order_relaxed);
        owner.warmStretchStarts (centre);
        done = generation;
    }
}

bool ReverseSamplerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto output = layouts.getMainOutputChannelSet();
    return output == juce::AudioChannelSet::mono() || output == juce::AudioChannelSet::stereo();
}

void ReverseSamplerProcessor::updateBpm()
{
    hostPlaying = false;

    if (auto* hostPlayHead = getPlayHead())
        if (const auto position = hostPlayHead->getPosition())
        {
            if (const auto hostBpm = position->getBpm())
                if (*hostBpm > 1.0)
                {
                    if (! juce::exactlyEqual (bpm.load(), *hostBpm))
                        invalidateWarmStarts();
                    bpm.store (*hostBpm);
                }

            if (const auto ppq = position->getPpqPosition())
            {
                hostPpq = *ppq;
                hostPlaying = position->getIsPlaying();
            }
        }
}

void ReverseSamplerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ScopedNoDenormals noDenormals;
    const int numSamples = buffer.getNumSamples();
    bypassed = false;

    // An offline render (export, freeze) right after a project load must not miss the samples still loading.
    if (pendingLoads.load (std::memory_order_relaxed) > 0 && isNonRealtime())
    {
        if (! offlineWaitExpired)
            offlineWaitExpired = ! waitForPendingLoads (60000);
    }
    else
    {
        offlineWaitExpired = false;
    }

    updateBpm();

    if (layerBuffer.getNumSamples() < numSamples)   // only if the host sends more than it announced
        layerBuffer.setSize (2 * numLayers, numSamples, false, false, true);

    juce::AudioBuffer<float> layers (layerBuffer.getArrayOfWritePointers(), 2 * numLayers, numSamples);
    layers.clear();

    // Adds the notes played on the on-screen keyboard to the incoming MIDI.
    keyboardState.processNextMidiBuffer (midiMessages, 0, numSamples, true);

    for (const auto metadata : midiMessages)
        if (const auto message = metadata.getMessage(); message.isNoteOn())
            lastNote.store (message.getNoteNumber(), std::memory_order_relaxed);

    synth.renderNextBlock (layers, midiMessages, 0, numSamples);

    // The solo audition plays into the same layer buses, so it sounds exactly like the full sound.
    handleAudition();
    auditionSynth.renderNextBlock (layers, noMidi, 0, numSamples);

    rs::ModMatrix::BlockInfo info;
    info.numSamples = numSamples;
    info.bpm = bpm.load();
    info.playing = hostPlaying;
    info.ppqStart = hostPpq;
    info.buildEnd = synth.getTimelinePosition();
    info.buildPeak = synth.getBuildPeak();
    const bool modulated = modMatrix.refreshActive();
    modMatrix.beginBlock (midiMessages, info);

    for (int layer = 0; layer < numLayers; ++layer)
        layerBuses[(size_t) layer].setFastRamps (modulated && modMatrix.layerIsModulated (layer));

    buffer.clear();
    const int step = modulated ? modulationStep : juce::jmax (1, numSamples);
    const bool interleaveFx = modulated && (modMatrix.fxIsModulated() || modMatrix.gainIsModulated());

    const auto mixAndGain = [&] (int start, int count)
    {
        float* part[2 * numLayers];
        for (int channel = 0; channel < 2 * numLayers; ++channel)
            part[channel] = layers.getWritePointer (channel) + start;
        juce::AudioBuffer<float> chunk (part, 2 * numLayers, count);
        fxRack.process (chunk, count, info.bpm);

        // Mix the layers into the output.
        if (buffer.getNumChannels() >= 2)
        {
            for (int layer = 0; layer < numLayers; ++layer)
            {
                buffer.addFrom (0, start, chunk, 2 * layer,     0, count);
                buffer.addFrom (1, start, chunk, 2 * layer + 1, 0, count);
            }
        }
        else if (buffer.getNumChannels() == 1)
        {
            for (int channel = 0; channel < 2 * numLayers; ++channel)
                buffer.addFrom (0, start, chunk, channel, 0, count, 0.5f);
        }

        const auto gainDb = gainParam->load();
        if (! juce::exactlyEqual (gainDb, lastGainDb))
        {
            lastGainDb = gainDb;
            lastGainTarget = juce::Decibels::decibelsToGain (gainDb);
        }
        buffer.applyGainRamp (start, count, lastGain, lastGainTarget);
        lastGain = lastGainTarget;
    };

    for (int start = 0; start < numSamples; start += step)
    {
        const int count = juce::jmin (step, numSamples - start);
        modMatrix.processChunk (start, count);

        for (int layer = 0; layer < numLayers; ++layer)
            layerBuses[(size_t) layer].process (layers.getWritePointer (2 * layer) + start, layers.getWritePointer (2 * layer + 1) + start, count);

        // The FX rack and the master gain only need the 32-sample grid when they are modulated themselves.
        if (interleaveFx)
            mixAndGain (start, count);
    }

    if (! interleaveFx && numSamples > 0)
        mixAndGain (0, numSamples);

    if (numSamples <= 0)
    {
        lastGainDb = gainParam->load();
        lastGain = lastGainTarget = juce::Decibels::decibelsToGain (lastGainDb);
    }
}

// Host bypass. The default would leave the voices frozen mid-note and drop the MIDI, so a note
// released during the bypass would hang afterwards and the effect tails would carry on where
// they stopped (and, with Arrive latency, a debug build would assert). Instead the first bypassed
// block stops every note and clears every tail; while bypassed the instrument stays silent and
// the on-screen keyboard and the audition buttons are kept in step.
void ReverseSamplerProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    keyboardState.processNextMidiBuffer (midiMessages, 0, buffer.getNumSamples(), true);

    // An audition asked for while bypassed is dropped, not played late when the bypass ends.
    auditionHandledSerial = auditionStartSerial.load();
    auditionHeld = false;

    if (! bypassed)
    {
        bypassed = true;
        synth.silenceAll();
        auditionSynth.silenceAll();

        for (auto& bus : layerBuses)
            bus.reset();

        modMatrix.passThrough();
        modMatrix.resetSources();
        fxRack.reset();
    }

    buffer.clear();
}

void ReverseSamplerProcessor::swapFxSlots (int first, int second)
{
    if (! juce::isPositiveAndBelow (first, rs::fxSlots) || ! juce::isPositiveAndBelow (second, rs::fxSlots) || first == second)
        return;
    commitPendingEdit();
    // Requested before and after: while the values are swapped one by one, both slots keep running
    // their old effect with its own knob values; after the fade they restart with the swapped ones.
    fxRack.requestReset (first); fxRack.requestReset (second);
    for (auto* name : rs::fxParameterNames)
    {
        auto* a = apvts.getParameter (rs::fxID (first, name));
        auto* b = apvts.getParameter (rs::fxID (second, name));
        const auto av = a->getValue(), bv = b->getValue();
        a->beginChangeGesture(); b->beginChangeGesture();
        a->setValueNotifyingHost (bv); b->setValueNotifyingHost (av);
        a->endChangeGesture(); b->endChangeGesture();
    }
    for (int row = 0; row < rs::mod::numRows; ++row)
    {
        const auto destination = modMatrix.readRow (row).destination;
        if (destination <= 0)
            continue;
        const auto& d = rs::ModMatrix::destinations()[(size_t) destination];
        if (d.fxSlot != first && d.fxSlot != second)
            continue;
        const auto moved = rs::ModMatrix::destinationIndex (rs::fxID (d.fxSlot == first ? second : first, d.fxParameter.toRawUTF8()));
        setParameterAsGesture (rs::ModMatrix::rowID (row, "Dst"), (float) moved);
    }
    fxRack.requestReset (first); fxRack.requestReset (second);
    commitPendingEdit();
}

void ReverseSamplerProcessor::setFxType (int slot, int newType)
{
    if (! juce::isPositiveAndBelow (slot, rs::fxSlots))
        return;
    newType = juce::jlimit (0, rs::numFxTypes - 1, newType);
    auto* typeParam = apvts.getParameter (rs::fxID (slot, "type"));
    if (typeParam == nullptr || juce::roundToInt (typeParam->convertFrom0to1 (typeParam->getValue())) == newType)
        return;
    commitPendingEdit();
    const auto set = [this, slot] (const char* name, float value) {
        if (auto* param = apvts.getParameter (rs::fxID (slot, name)))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 (value));
            param->endChangeGesture();
        }
    };
    // Type first, knobs after. The rack reads a slot's knobs before its Type, so whenever it sees one
    // of these new knob values it also sees the new Type: the old effect fades out with its own knob
    // values, and the new one starts with these (the switch comes 20 ms later).
    set ("type", (float) newType);
    if (newType == 0)
    {
        for (const auto* name : rs::fxParameterNames)
        {
            if (juce::String (name) == "type")
                continue;
            if (auto* param = apvts.getParameter (rs::fxID (slot, name)))
                set (name, param->convertFrom0to1 (param->getDefaultValue()));
        }
    }
    else
    {
        const char* shared[] { "p1", "p2", "p3", "mix" };
        for (int k = 0; k < 4; ++k)
            set (shared[k], rs::fxTypeDefaults[newType][k]);
    }
    for (int row = 0; row < rs::mod::numRows; ++row)
    {
        const auto destination = modMatrix.readRow (row).destination;
        if (destination > 0 && rs::ModMatrix::destinations()[(size_t) destination].fxSlot == slot)
            resetModulationRow (row);
    }
    commitPendingEdit();
}

void ReverseSamplerProcessor::applyFxPreset (int slot, const rs::FxPreset& preset)
{
    if (! juce::isPositiveAndBelow (slot, rs::fxSlots) || ! juce::isPositiveAndBelow (preset.type, rs::numFxTypes))
        return;
    commitPendingEdit();
    const auto set = [this, slot] (const char* name, float value) {
        if (auto* param = apvts.getParameter (rs::fxID (slot, name)))
        {
            param->beginChangeGesture();
            param->setValueNotifyingHost (param->convertTo0to1 (value));
            param->endChangeGesture();
        }
    };
    // Requested before and after (see FxRackDSP::requestReset): the running effect fades out with
    // the values it had, then the preset starts clean, even when only the knobs differ.
    const bool typeChanges = juce::roundToInt (apvts.getRawParameterValue (rs::fxID (slot, "type"))->load()) != preset.type;
    fxRack.requestReset (slot);
    set ("type", (float) preset.type);
    for (const auto* name : rs::fxParameterNames)
    {
        const juce::String id (name);
        if (id == "type" || id == "a" || id == "b")
            continue;
        if (auto* param = apvts.getParameter (rs::fxID (slot, name)))
            set (name, param->convertFrom0to1 (param->getDefaultValue()));
    }
    const char* shared[] { "p1", "p2", "p3", "mix" };
    for (int k = 0; k < 4; ++k)
        set (shared[k], rs::fxTypeDefaults[preset.type][k]);
    for (const auto& value : preset)
        set (value.name, value.value);
    fxRack.requestReset (slot);
    if (typeChanges)
        for (int row = 0; row < rs::mod::numRows; ++row)
        {
            const auto destination = modMatrix.readRow (row).destination;
            if (destination > 0 && rs::ModMatrix::destinations()[(size_t) destination].fxSlot == slot)
                resetModulationRow (row);
        }
    commitPendingEdit();
}

void ReverseSamplerProcessor::setParameterAsGesture (const juce::String& id, float value)
{
    if (auto* param = apvts.getParameter (id))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (value));
        param->endChangeGesture();
    }
}

void ReverseSamplerProcessor::resetModulationRow (int row)
{
    for (const auto* field : rs::ModMatrix::rowFields)
        if (auto* param = apvts.getParameter (rs::ModMatrix::rowID (row, field)))
            setParameterAsGesture (param->paramID, param->convertFrom0to1 (param->getDefaultValue()));
}

void ReverseSamplerProcessor::clearModulationRow (int row)
{
    if (! juce::isPositiveAndBelow (row, rs::mod::numRows))
        return;
    commitPendingEdit();
    resetModulationRow (row);
    commitPendingEdit();
}

int ReverseSamplerProcessor::addModulation (int source, const juce::String& paramID)
{
    const int destination = rs::ModMatrix::destinationIndex (paramID);
    if (destination <= 0 || ! juce::isPositiveAndBelow (source, (int) rs::mod::numSources) || source == rs::mod::none)
        return -1;

    for (int row = 0; row < rs::mod::numRows; ++row)
    {
        const auto r = modMatrix.readRow (row);
        if (r.source != rs::mod::none || r.destination != 0 || std::abs (r.amount) >= 1.0e-6f)
            continue;

        const bool bipolar = rs::mod::isBipolarByNature (source);
        commitPendingEdit();
        setParameterAsGesture (rs::ModMatrix::rowID (row, "Src"), (float) source);
        setParameterAsGesture (rs::ModMatrix::rowID (row, "Dst"), (float) destination);
        setParameterAsGesture (rs::ModMatrix::rowID (row, "Bip"), bipolar ? 1.0f : 0.0f);
        setParameterAsGesture (rs::ModMatrix::rowID (row, "Amt"), bipolar ? 25.0f : 50.0f);
        commitPendingEdit();
        return row;
    }

    return -1;
}

juce::String ReverseSamplerProcessor::getMacroName (int macro) const
{
    const auto name = apvts.state.getProperty ("macroName" + juce::String (macro + 1)).toString();
    return name.isNotEmpty() ? name : "MACRO " + juce::String (macro + 1);
}

void ReverseSamplerProcessor::setMacroName (int macro, const juce::String& name)
{
    if (! juce::isPositiveAndBelow (macro, rs::mod::numMacros))
        return;
    const auto key = "macroName" + juce::String (macro + 1);
    const auto trimmed = name.trim().substring (0, 24);
    if (trimmed.isEmpty() || trimmed == "MACRO " + juce::String (macro + 1))
        apvts.state.removeProperty (key, nullptr);
    else
        apvts.state.setProperty (key, trimmed, nullptr);
}

void ReverseSamplerProcessor::handleAudition()
{
    if (const auto serial = auditionStartSerial.load(); serial != auditionHandledSerial)
    {
        auditionHandledSerial = serial;
        const auto layer = juce::jlimit (0, numLayers - 1, auditionLayer.load());
        auditionNote = juce::jlimit (0, 127, juce::roundToInt (layerParams[(size_t) layer].rootNote->load()));
        auditionSynth.allNotesOff (0, false);
        auditionVoice->setSoloLayer (layer);
        auditionSynth.noteOn (1, auditionNote, 1.0f);
        lastNote.store (auditionNote, std::memory_order_relaxed);   // the time line shows this note
        auditionHeld = true;
    }

    if (auditionHeld && auditionStopSerial.load() == auditionHandledSerial)
    {
        auditionSynth.noteOff (1, auditionNote, 0.0f, true);
        auditionHeld = false;
    }
}

void ReverseSamplerProcessor::startAudition (int layer)
{
    if (! juce::isPositiveAndBelow (layer, numLayers))
        return;

    auditionLayer.store (layer);
    auditionHeldLayer.store (layer);
    ++auditionStartSerial;
}

void ReverseSamplerProcessor::stopAudition()
{
    auditionHeldLayer.store (-1);
    auditionStopSerial.store (auditionStartSerial.load());
}

double ReverseSamplerProcessor::getTimelinePosition() const noexcept
{
    const auto solo = auditionSynth.getTimelinePosition();
    return solo >= 0.0 ? solo : synth.getTimelinePosition();
}

float ReverseSamplerProcessor::getPlayheadPosition (int layer) const noexcept
{
    const auto solo = auditionSynth.getPlayheadPosition (layer);
    return solo >= 0.0f ? solo : synth.getPlayheadPosition (layer);
}

juce::AudioProcessorEditor* ReverseSamplerProcessor::createEditor()
{
    return new ReverseSamplerEditor (*this);
}

//==============================================================================
void ReverseSamplerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto tree = apvts.copyState();

    for (int layer = 0; layer < numLayers; ++layer)
    {
        tree.removeProperty (samplePathProperty (layer), nullptr);

        // A sample that was not found, or is still loading with the project, keeps its place in it.
        const auto sample = getSample (layer);
        auto path = sample != nullptr ? sample->file.getFullPathName() : getMissingSamplePath (layer);

        {
            const std::lock_guard<std::mutex> guard (infoMutex);

            if (loadingFromProject[(size_t) layer] && loadingFiles[(size_t) layer] != juce::File())
                path = loadingFiles[(size_t) layer].getFullPathName();
        }

        if (path.isNotEmpty())
            tree.setProperty (samplePathProperty (layer), path, nullptr);
    }

    for (int index = 0; index < numEnvelopes; ++index)
    {
        const auto envelope = getEnvelope (index);
        const auto isDefault = index == dissolveEnvelope ? envelope == getDefaultEnvelope (index) : envelope.isFlat();
        tree.removeProperty (envelopeProperty (index), nullptr);

        if (! isDefault)
            tree.setProperty (envelopeProperty (index), envelope.toString(), nullptr);
    }

    if (const auto xml = tree.createXml())
        copyXmlToBinary (*xml, destData);
}

void ReverseSamplerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto tree = juce::ValueTree::fromXml (*xml);
    const ScopedRestore notForUndo (restoring);   // a project or preset load is not an undo step

    std::array<juce::String, numLayers> paths;

    for (int layer = 0; layer < numLayers; ++layer)
    {
        paths[(size_t) layer] = tree.getProperty (samplePathProperty (layer)).toString();
        tree.removeProperty (samplePathProperty (layer), nullptr);
    }

    for (int index = 0; index < numEnvelopes; ++index)
    {
        const auto text = tree.getProperty (envelopeProperty (index)).toString();
        tree.removeProperty (envelopeProperty (index), nullptr);
        setEnvelope (index, text.isEmpty() ? getDefaultEnvelope (index) : rs::Envelope::fromString (text));
    }

    // Projects saved with v0.1 had one sample and one Reverse switch: they become layer 1.
    const auto legacyPath = tree.getProperty (legacySamplePathProperty).toString();
    const auto legacyReverse = tree.getChildWithProperty ("id", "reverse");
    tree.removeProperty (legacySamplePathProperty, nullptr);

    if (paths[0].isEmpty())
        paths[0] = legacyPath;

    // Older projects lack the parameters added since. JUCE otherwise keeps the current
    // values for missing parameters when loading into an existing instance.
    for (auto* parameter : getParameters())
        if (auto* param = dynamic_cast<juce::RangedAudioParameter*> (parameter))
        {
            const auto& id = param->paramID;
            if (! tree.getChildWithProperty ("id", id).isValid())
            {
                juce::ValueTree child ("PARAM");
                child.setProperty ("id", id, nullptr);
                child.setProperty ("value", param->convertFrom0to1 (param->getDefaultValue()), nullptr);
                tree.appendChild (child, nullptr);
            }
        }
    fxRack.requestResetAll();   // before and after (see requestReset): old effects fade out with their own values
    apvts.replaceState (tree);
    fxRack.requestResetAll();

    if (legacyReverse.isValid())
        if (auto* param = apvts.getParameter (ParamIDs::layerID (0, ParamIDs::reverse)))
            param->setValueNotifyingHost ((float) legacyReverse.getProperty ("value") >= 0.5f ? 1.0f : 0.0f);

    // The samples load in the background, so the host does not freeze on big files. Meanwhile the
    // layer is empty (never the sample of the previous project). A sample that cannot be loaded
    // leaves its layer empty, and its path is kept for the next save.
    {
        const std::lock_guard<std::mutex> guard (infoMutex);
        projectProblems.clear();
    }

    for (int layer = 0; layer < numLayers; ++layer)
    {
        const auto& path = paths[(size_t) layer];
        const juce::ScopedLock sl (loadLock);
        cancelLoad (layer);

        if (path.isEmpty())
        {
            setSampleAndMissingPath (layer, nullptr, {});
        }
        else if (juce::File::isAbsolutePath (path) && juce::File (path).existsAsFile())
        {
            // The same, unchanged file stays as it is: no reload, and the notes keep playing
            // (some hosts load the state again for their own undo or for A/B comparisons).
            const juce::File file (path);

            if (const auto current = getSample (layer); current != nullptr && current->file == file
                                                         && current->fileTime == file.getLastModificationTime())
            {
                setSampleAndMissingPath (layer, current, {});
            }
            else
            {
                setSampleAndMissingPath (layer, nullptr, {});
                startLoad (layer, file, true);
            }
        }
        else
        {
            setSampleAndMissingPath (layer, nullptr, path);
            const std::lock_guard<std::mutex> guard (infoMutex);
            projectProblems.add (layerName (layer) + ": saved sample not found: " + path);
        }
    }

    if (const auto problems = [this] { const std::lock_guard<std::mutex> guard (infoMutex); return projectProblems.joinIntoString ("   "); }();
        problems.isNotEmpty())
        setStatusMessage (problems);

    clearUndoHistory();
    updateLatency();
}

//==============================================================================
rs::SampleDataPtr ReverseSamplerProcessor::readSampleFile (int layer, const juce::File& file, juce::String& error,
                                                           const std::function<bool()>& cancelled)
{
    const auto prefix = layerName (layer) + ": ";

    if (file.isDirectory())
    {
        error = prefix + "folders are not supported yet, drop a single audio file";
        return nullptr;
    }

    if (! file.existsAsFile())
    {
        error = prefix + "file not found: " + file.getFullPathName();
        return nullptr;
    }

    const std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));

    if (reader == nullptr)
    {
        error = prefix + "unsupported or damaged audio file: " + file.getFileName();
        return nullptr;
    }

    if (reader->lengthInSamples <= 0 || reader->numChannels == 0 || reader->sampleRate <= 0.0)
    {
        error = prefix + "no audio found in " + file.getFileName();
        return nullptr;
    }

    // At most maxSampleLengthSeconds, and never more than 2^26 frames per channel.
    const auto maxLength   = juce::jmin ((juce::int64) (maxSampleLengthSeconds * reader->sampleRate), (juce::int64) 1 << 26);
    const auto numSamples  = (int) juce::jmin (reader->lengthInSamples, maxLength);
    const auto numChannels = reader->numChannels > 1 ? 2 : 1;

    auto sample = std::make_shared<rs::SampleData>();
    sample->file         = file;
    sample->fileTime     = file.getLastModificationTime();
    sample->sampleRate   = reader->sampleRate;
    sample->wasTruncated = reader->lengthInSamples > maxLength;
    sample->audio.setSize (numChannels, numSamples);

    // In chunks, so a newer request can cancel a long load.
    constexpr int chunk = 1 << 16;

    for (int done = 0; done < numSamples; done += chunk)
    {
        if (cancelled != nullptr && cancelled())
            return nullptr;

        if (! reader->read (&sample->audio, done, juce::jmin (chunk, numSamples - done), done, true, numChannels > 1))
        {
            error = prefix + "could not read audio from " + file.getFileName();
            return nullptr;
        }
    }

    sample->buildOverview (overviewBins);
    sample->detectOnsets();
    return sample;
}

bool ReverseSamplerProcessor::loadSample (int layer, const juce::File& file)
{
    jassert (juce::isPositiveAndBelow (layer, numLayers));
    const juce::ScopedLock sl (loadLock);
    cancelLoad (layer);

    juce::String error;
    auto sample = readSampleFile (layer, file, error, {});

    if (sample == nullptr)
    {
        setStatusMessage (error);
        return false;
    }

    noteSampleEdit (layer);
    setSampleAndMissingPath (layer, std::move (sample), {});
    return true;
}

void ReverseSamplerProcessor::loadSampleAsync (int layer, const juce::File& file)
{
    if (! juce::isPositiveAndBelow (layer, numLayers))
        return;

    const juce::ScopedLock sl (loadLock);
    startLoad (layer, file, false);
}

void ReverseSamplerProcessor::startLoad (int layer, const juce::File& file, bool fromProject)
{
    const juce::ScopedLock sl (loadLock);
    LoadRequest request { layer, file, ++loadSerials[(size_t) layer], fromProject };

    {
        const std::lock_guard<std::mutex> guard (infoMutex);
        loadingFiles[(size_t) layer] = file;
        loadingFromProject[(size_t) layer] = fromProject;
        pendingLoads.store ((int) std::count_if (loadingFiles.begin(), loadingFiles.end(), [] (const juce::File& f) { return f != juce::File(); }));
    }

    ++sampleVersion;
    loader.addJob ([this, request] { runLoad (request); });
}

void ReverseSamplerProcessor::cancelLoad (int layer)
{
    const juce::ScopedLock sl (loadLock);
    ++loadSerials[(size_t) layer];   // the worker drops the request

    if (getLoadingFile (layer) != juce::File())
        finishLoad (layer);
}

void ReverseSamplerProcessor::finishLoad (int layer)
{
    {
        const std::lock_guard<std::mutex> guard (infoMutex);
        loadingFiles[(size_t) layer] = juce::File();
        loadingFromProject[(size_t) layer] = false;
        pendingLoads.store ((int) std::count_if (loadingFiles.begin(), loadingFiles.end(), [] (const juce::File& f) { return f != juce::File(); }));
    }

    ++sampleVersion;
    loadFinished.signal();
}

bool ReverseSamplerProcessor::cancelUserLoads()
{
    const juce::ScopedLock sl (loadLock);
    bool cancelled = false;

    for (int layer = 0; layer < numLayers; ++layer)
    {
        const auto userLoad = [&] { const std::lock_guard<std::mutex> guard (infoMutex);
                                    return loadingFiles[(size_t) layer] != juce::File() && ! loadingFromProject[(size_t) layer]; }();

        if (userLoad)
        {
            cancelLoad (layer);
            cancelled = true;
        }
    }

    return cancelled;
}

bool ReverseSamplerProcessor::anyUserLoadPending() const
{
    const std::lock_guard<std::mutex> guard (infoMutex);

    for (int layer = 0; layer < numLayers; ++layer)
        if (loadingFiles[(size_t) layer] != juce::File() && ! loadingFromProject[(size_t) layer])
            return true;

    return false;
}

juce::File ReverseSamplerProcessor::getLoadingFile (int layer) const
{
    const std::lock_guard<std::mutex> guard (infoMutex);
    return loadingFiles[(size_t) juce::jlimit (0, numLayers - 1, layer)];
}

void ReverseSamplerProcessor::runLoad (const LoadRequest& request)
{
    const auto layer = request.layer;
    const auto isCurrent = [this, &request] { return loadSerials[(size_t) request.layer].load() == request.serial; };

    if (! isCurrent())
        return;

    juce::String error;
    rs::SampleDataPtr sample;

    try
    {
        sample = readSampleFile (layer, request.file, error, [&] { return ! isCurrent(); });
    }
    catch (const std::bad_alloc&)
    {
        sample = nullptr;
        error = layerName (layer) + ": not enough memory for " + request.file.getFileName();
    }

    // Installed by the worker itself (not via the message thread), so an offline render that waits for it cannot block.
    const juce::ScopedLock sl (loadLock);

    if (! isCurrent())
        return;

    if (sample != nullptr)
    {
        if (! request.fromProject)
            recordSampleEdit (layer);

        setSampleAndMissingPath (layer, std::move (sample), {});
    }
    else if (request.fromProject)
    {
        setSampleAndMissingPath (layer, nullptr, request.file.getFullPathName());
        const std::lock_guard<std::mutex> guard (infoMutex);
        projectProblems.add (error);
    }

    // Errors of this project load stay on show until they are dealt with; a user load reports its own.
    // (Before finishLoad(), so whoever waits for the load sees the message too.)
    if (sample == nullptr && ! request.fromProject)
        setStatusMessage (error);
    else if (const auto problems = [this] { const std::lock_guard<std::mutex> guard (infoMutex); return projectProblems.joinIntoString ("   "); }();
             problems.isNotEmpty())
        setStatusMessage (problems);

    finishLoad (layer);
}

bool ReverseSamplerProcessor::waitForPendingLoads (int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) juce::jmax (0, timeoutMs);

    while (pendingLoads.load() > 0)
    {
        const auto now = juce::Time::getMillisecondCounter();

        if (now >= end)
            return false;

        loadFinished.wait ((int) juce::jmin ((juce::uint32) 20, end - now));
    }

    return true;
}

void ReverseSamplerProcessor::clearSample (int layer)
{
    if (! juce::isPositiveAndBelow (layer, numLayers))
        return;

    const juce::ScopedLock sl (loadLock);
    cancelLoad (layer);
    noteSampleEdit (layer);
    setSampleAndMissingPath (layer, nullptr, {});
}

void ReverseSamplerProcessor::setSampleAndMissingPath (int layer, rs::SampleDataPtr sample, const juce::String& missingPath)
{
    const juce::ScopedLock sl (loadLock);

    auto newSamples = [this] { const std::lock_guard<std::mutex> guard (infoMutex); return samples; }();
    const auto changed = newSamples[(size_t) layer] != sample;
    newSamples[(size_t) layer] = std::move (sample);

    {
        const std::lock_guard<std::mutex> guard (infoMutex);
        missingPaths[(size_t) layer] = missingPath;
    }

    if (changed)
        installSamples (newSamples);   // also clears the status message
    else
        setStatusMessage ({});

    // The host is told about a new latency from the message thread (the timer does it for the loader).
    if (juce::MessageManager::existsAndIsCurrentThread())
        updateLatency();
}

juce::String ReverseSamplerProcessor::getMissingSamplePath (int layer) const
{
    const std::lock_guard<std::mutex> guard (infoMutex);
    return missingPaths[(size_t) juce::jlimit (0, numLayers - 1, layer)];
}

void ReverseSamplerProcessor::resetLayer (int layer)
{
    if (! juce::isPositiveAndBelow (layer, numLayers))
        return;

    clearSample (layer);

    for (const auto* name : ParamIDs::layerParameters)
    {
        if (auto* param = apvts.getParameter (ParamIDs::layerID (layer, name)))
        {
            if (! sameValue (param->getValue(), param->getDefaultValue()))
            {
                param->beginChangeGesture();
                param->setValueNotifyingHost (param->getDefaultValue());
                param->endChangeGesture();
            }
        }
    }

    if (! (getEnvelope (layer) == getDefaultEnvelope (layer)))
        setEnvelope (layer, getDefaultEnvelope (layer));

    updateLatency();
}

bool ReverseSamplerProcessor::isLayerDefault (int layer) const
{
    if (! juce::isPositiveAndBelow (layer, numLayers) || getSample (layer) != nullptr || getMissingSamplePath (layer).isNotEmpty()
        || isLoading (layer))
        return false;

    for (const auto* name : ParamIDs::layerParameters)
        if (const auto* param = apvts.getParameter (ParamIDs::layerID (layer, name)))
            if (! sameValue (param->getValue(), param->getDefaultValue()))
                return false;

    return getEnvelope (layer) == getDefaultEnvelope (layer);
}

void ReverseSamplerProcessor::installSamples (const std::array<rs::SampleDataPtr, rs::numLayers>& newSamples)
{
    const juce::SynthesiserSound::Ptr sound (new rs::InstrumentSound (newSamples));
    synth.replaceSound (sound);
    auditionSynth.replaceSound (sound);

    {
        const std::lock_guard<std::mutex> guard (infoMutex);
        samples = newSamples;
        statusMessage.clear();
    }

    ++sampleVersion;
    invalidateWarmStarts();
}

bool ReverseSamplerProcessor::canReadFile (const juce::File& file) const
{
    return formatManager.findFormatForFileExtension (file.getFileExtension()) != nullptr;
}

juce::String ReverseSamplerProcessor::getSupportedFileWildcard() const
{
    return formatManager.getWildcardForAllFormats();
}

rs::SampleDataPtr ReverseSamplerProcessor::getSample (int layer) const
{
    const std::lock_guard<std::mutex> guard (infoMutex);
    return samples[(size_t) layer];
}

juce::String ReverseSamplerProcessor::getStatusMessage() const
{
    const std::lock_guard<std::mutex> guard (infoMutex);
    return statusMessage;
}

void ReverseSamplerProcessor::setStatusMessage (const juce::String& message)
{
    {
        const std::lock_guard<std::mutex> guard (infoMutex);
        statusMessage = message;
    }

    ++sampleVersion;
}

//==============================================================================
rs::Envelope ReverseSamplerProcessor::getEnvelope (int index) const noexcept
{
    return envelopes[(size_t) juce::jlimit (0, numEnvelopes - 1, index)].get();
}

void ReverseSamplerProcessor::setEnvelope (int index, const rs::Envelope& envelope)
{
    index = juce::jlimit (0, numEnvelopes - 1, index);

    if (restoring.load() == 0)
    {
        const juce::ScopedLock sl (undoLock);

        if (! envelopeTouched[(size_t) index])
        {
            envelopeTouched[(size_t) index] = true;
            envelopeBefore[(size_t) index] = envelopes[(size_t) index].get();
        }

        noteEdit();
    }

    envelopes[(size_t) index].set (envelope);
}

rs::Envelope ReverseSamplerProcessor::getDefaultEnvelope (int index) noexcept
{
    return index == dissolveEnvelope ? rs::Envelope::dissolveDefault() : rs::Envelope {};
}

rs::PlayMode ReverseSamplerProcessor::getPlayMode() const noexcept
{
    return playModeParam->load() >= 0.5f ? rs::PlayMode::arrive : rs::PlayMode::swell;
}

rs::AfterPeak ReverseSamplerProcessor::getAfterPeak() const noexcept
{
    return (rs::AfterPeak) juce::jlimit (0, 3, juce::roundToInt (afterPeakParam->load()));
}

double ReverseSamplerProcessor::getDissolveSeconds() const noexcept
{
    return dissolveParam->load();
}

double ReverseSamplerProcessor::getPeakCutSeconds() const noexcept
{
    return juce::jmax (0.0f, peakCutParam->load());
}

rs::NoteLayout ReverseSamplerProcessor::getNoteLayout (int midiNote) const
{
    std::array<rs::SampleDataPtr, numLayers> held;
    std::array<const rs::SampleData*, numLayers> pointers {};
    std::array<rs::LayerSettings, numLayers> settings;

    for (int layer = 0; layer < numLayers; ++layer)
    {
        held[(size_t) layer] = getSample (layer);
        pointers[(size_t) layer] = held[(size_t) layer].get();
        settings[(size_t) layer] = getLayerSettings (layer);
    }

    return rs::computeNoteLayout (pointers, settings, midiNote, getPlayMode(), arriveLatency.load(), getPlaybackRate(), bpm.load(),
                                  getPeakCutSeconds());
}

void ReverseSamplerProcessor::updateLatency()
{
    int latency = 0;

    if (getPlayMode() == rs::PlayMode::arrive)
    {
        std::array<rs::SampleDataPtr, numLayers> held;
        std::array<const rs::SampleData*, numLayers> pointers {};
        std::array<rs::LayerSettings, numLayers> settings;

        for (int layer = 0; layer < numLayers; ++layer)
        {
            held[(size_t) layer] = getSample (layer);
            pointers[(size_t) layer] = held[(size_t) layer].get();
            settings[(size_t) layer] = getLayerSettings (layer);
        }

        latency = rs::computeArriveLatency (pointers, settings, getPlaybackRate(), bpm.load(), getPeakCutSeconds());
    }

    if (arriveLatency.load() != latency)
        invalidateWarmStarts();
    arriveLatency.store (latency);

    if (getLatencySamples() != latency)
        setLatencySamples (latency);
}

//==============================================================================
// Undo. Each step records, for what an edit changed, the values before and after it.
// Parameters are followed through their gestures (what the editor does), so host automation
// never becomes an undo step. A step is closed when the edit has been quiet for a moment and
// no mouse button is held.
void ReverseSamplerProcessor::parameterGestureChanged (int index, bool gestureIsStarting)
{
    if (restoring.load() > 0 || ! juce::isPositiveAndBelow (index, allParameters.size()))
        return;

    const juce::ScopedLock sl (undoLock);
    const auto i = (size_t) index;

    if (gestureIsStarting)
    {
        if (! parameterTouched[i])
        {
            parameterTouched[i] = 1;
            parameterBefore[i] = allParameters.getUnchecked (index)->getValue();
        }

        ++parameterGestures[i];
        ++openGestures;
    }
    else if (parameterGestures[i] > 0)
    {
        --parameterGestures[i];
        --openGestures;
        noteEdit();
    }
}

void ReverseSamplerProcessor::noteEdit()
{
    const juce::ScopedLock sl (undoLock);
    editPending = true;
    lastEditTime = juce::Time::getMillisecondCounter();
}

void ReverseSamplerProcessor::noteSampleEdit (int layer)
{
    if (restoring.load() == 0)
        recordSampleEdit (layer);
}

void ReverseSamplerProcessor::recordSampleEdit (int layer)
{
    const juce::ScopedLock sl (undoLock);

    // Loading or clearing a sample is a step of its own; samples changed together (two files dropped) stay one.
    if (editPending && std::none_of (sampleTouched.begin(), sampleTouched.end(), [] (bool t) { return t; }))
        commitPendingEdit();

    if (! sampleTouched[(size_t) layer])
    {
        sampleTouched[(size_t) layer] = true;
        sampleBefore[(size_t) layer] = getSample (layer);
        missingBefore[(size_t) layer] = getMissingSamplePath (layer);
    }

    noteEdit();
}

void ReverseSamplerProcessor::commitPendingEdit()
{
    const juce::ScopedLock sl (undoLock);
    EditStep step;

    for (int index = 0; index < allParameters.size(); ++index)
    {
        const auto i = (size_t) index;

        if (parameterTouched[i] && parameterGestures[i] == 0)   // a knob still held stays in the next step
        {
            parameterTouched[i] = 0;
            const auto after = allParameters.getUnchecked (index)->getValue();

            if (! sameValue (after, parameterBefore[i]))
                step.parameters.push_back ({ index, parameterBefore[i], after });
        }
    }

    for (int index = 0; index < numEnvelopes; ++index)
    {
        const auto i = (size_t) index;

        if (envelopeTouched[i])
        {
            envelopeTouched[i] = false;
            const auto after = envelopes[i].get();

            if (! (after == envelopeBefore[i]))
                step.envelopes.push_back ({ index, envelopeBefore[i], after });
        }
    }

    for (int layer = 0; layer < numLayers; ++layer)
    {
        const auto i = (size_t) layer;

        if (sampleTouched[i])
        {
            sampleTouched[i] = false;
            auto after = getSample (layer);
            auto missingAfter = getMissingSamplePath (layer);

            if (after != sampleBefore[i] || missingAfter != missingBefore[i])
                step.samples.push_back ({ layer, sampleBefore[i], std::move (after), missingBefore[i], std::move (missingAfter) });

            sampleBefore[i] = nullptr;
        }
    }

    editPending = openGestures > 0;

    if (step.isEmpty())
        return;

    undoSteps.push_back (std::move (step));

    if ((int) undoSteps.size() > maxUndoSteps)
        undoSteps.erase (undoSteps.begin());

    redoSteps.clear();
    limitUndoMemory();
}

void ReverseSamplerProcessor::limitUndoMemory()
{
    // Samples replaced or cleared stay in memory while a step can bring them back. Very long
    // samples are capped: the oldest steps go first.
    std::vector<const rs::SampleData*> counted;

    for (int layer = 0; layer < numLayers; ++layer)
        if (const auto sample = getSample (layer))
            counted.push_back (sample.get());

    size_t bytes = 0;

    const auto count = [&] (const rs::SampleDataPtr& sample)
    {
        if (sample != nullptr && std::find (counted.begin(), counted.end(), sample.get()) == counted.end())
        {
            counted.push_back (sample.get());
            bytes += (size_t) sample->audio.getNumChannels() * (size_t) sample->audio.getNumSamples() * sizeof (float);
        }
    };

    for (int index = (int) undoSteps.size() - 1; index >= 0; --index)
    {
        for (const auto& change : undoSteps[(size_t) index].samples)
        {
            count (change.before);
            count (change.after);
        }

        if (bytes > undoSampleMemory)
        {
            undoSteps.erase (undoSteps.begin(), undoSteps.begin() + index + 1);
            break;
        }
    }
}

bool ReverseSamplerProcessor::undo()
{
    // A file still loading is the newest action: undo cancels it.
    if (cancelUserLoads())
    {
        setStatusMessage ("Loading cancelled");
        return true;
    }

    commitPendingEdit();
    EditStep step;

    {
        const juce::ScopedLock sl (undoLock);

        if (undoSteps.empty())
            return false;

        step = std::move (undoSteps.back());
        undoSteps.pop_back();
    }

    applyStep (step, false);

    const juce::ScopedLock sl (undoLock);
    redoSteps.push_back (std::move (step));
    return true;
}

bool ReverseSamplerProcessor::redo()
{
    commitPendingEdit();
    EditStep step;

    {
        const juce::ScopedLock sl (undoLock);

        if (redoSteps.empty())
            return false;

        step = std::move (redoSteps.back());
        redoSteps.pop_back();
    }

    applyStep (step, true);

    const juce::ScopedLock sl (undoLock);
    undoSteps.push_back (std::move (step));
    return true;
}

bool ReverseSamplerProcessor::canUndo() const
{
    if (anyUserLoadPending())
        return true;

    const juce::ScopedLock sl (undoLock);
    return ! undoSteps.empty() || editPending;
}

bool ReverseSamplerProcessor::canRedo() const
{
    const juce::ScopedLock sl (undoLock);
    return ! redoSteps.empty() && ! editPending;   // a new edit will clear them
}

void ReverseSamplerProcessor::applyStep (const EditStep& step, bool redoIt)
{
    const ScopedRestore notForUndo (restoring);

    // FX settings that restart an effect go first, then everything else: the rack reads a slot's
    // knobs before these, so the old effect never runs with knob values meant for the new setting.
    const auto restartsEffect = [this] (const juce::AudioProcessorParameter* param)
    {
        for (int slot = 0; slot < rs::fxSlots; ++slot)
            for (const char* name : { "type", "delayStyle", "replica", "amReverse" })
                if (apvts.getParameter (rs::fxID (slot, name)) == param)
                    return true;
        return false;
    };

    for (int pass = 0; pass < 2; ++pass)
    for (const auto& change : step.parameters)
    {
        auto* param = allParameters[change.index];
        const auto value = redoIt ? change.after : change.before;

        if (param != nullptr && restartsEffect (param) == (pass == 0) && ! sameValue (param->getValue(), value))
        {
            param->beginChangeGesture();   // the host sees it like an edit in the editor
            param->setValueNotifyingHost (value);
            param->endChangeGesture();
        }
    }

    for (const auto& change : step.envelopes)
        setEnvelope (change.index, redoIt ? change.after : change.before);

    for (const auto& change : step.samples)
    {
        const juce::ScopedLock sl (loadLock);
        cancelLoad (change.layer);
        setSampleAndMissingPath (change.layer, redoIt ? change.after : change.before, redoIt ? change.missingAfter : change.missingBefore);
    }

    updateLatency();
}

void ReverseSamplerProcessor::clearUndoHistory()
{
    const juce::ScopedLock sl (undoLock);
    undoSteps.clear();
    redoSteps.clear();
    std::fill (parameterTouched.begin(), parameterTouched.end(), 0);
    envelopeTouched.fill (false);
    sampleTouched.fill (false);
    sampleBefore.fill (nullptr);
    editPending = false;
}

//==============================================================================
// Creates new instances of the plug-in.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new ReverseSamplerProcessor();
}
