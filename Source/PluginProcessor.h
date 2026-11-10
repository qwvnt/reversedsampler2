#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include "LayerBus.h"
#include "FxRack.h"
#include "FxPresets.h"
#include "SamplerEngine.h"
#include "ModMatrix.h"

#include <functional>
#include <mutex>
#include <vector>

namespace ParamIDs
{
    // Global
    inline constexpr auto attack  = "attack";
    inline constexpr auto release = "release";
    inline constexpr auto gain    = "gain";      // master volume
    inline constexpr auto releaseCurve = "releaseCurve";
    inline constexpr auto playMode     = "playMode";    // Swell / Arrive
    inline constexpr auto afterPeak    = "afterPeak";   // Cut / Release / Freeze / Loop
    inline constexpr auto dissolve     = "dissolve";    // Release after the peak: the fade time, seconds (the ID is from Dissolve)
    inline constexpr auto peakCut      = "peakCut";     // the peak moved earlier, seconds (0 = the natural peak)

    // Per layer; the full ID is made with layerID(), e.g. "l2_pitch".
    inline constexpr auto on         = "on";
    inline constexpr auto reverse    = "reverse";
    inline constexpr auto timeMode   = "timeMode";
    inline constexpr auto lengthMode = "lengthMode";
    inline constexpr auto seconds    = "seconds";
    inline constexpr auto beats      = "beats";
    inline constexpr auto pitch      = "pitch";
    inline constexpr auto root       = "root";
    inline constexpr auto start      = "start";
    inline constexpr auto end        = "end";
    inline constexpr auto level      = "level";
    inline constexpr auto pan        = "pan";
    inline constexpr auto width      = "width";
    inline constexpr auto lowCut     = "lowCut";
    inline constexpr auto highCut    = "highCut";
    inline constexpr auto peakOffset = "peakOffset";

    /** Every per-layer parameter (Clear puts them all back to their defaults). */
    inline constexpr const char* layerParameters[] = { on, reverse, timeMode, lengthMode, seconds, beats, pitch, root,
                                                       start, end, level, pan, width, lowCut, highCut, peakOffset };

    inline juce::String layerID (int layer, const char* name)   { return "l" + juce::String (layer + 1) + "_" + name; }
}

//==============================================================================
class ReverseSamplerProcessor final : public juce::AudioProcessor,
                                      private juce::AudioProcessorParameter::Listener,
                                      private juce::Timer
{
public:
    static constexpr int numLayers = rs::numLayers;
    static constexpr int mainEnvelope = rs::numLayers;          // envelope index of the final envelope (on the sum)
    static constexpr int dissolveEnvelope = rs::numLayers + 1;  // the fade after the peak (After peak = Release)
    static constexpr int numEnvelopes = rs::numLayers + 2;
    static constexpr int numVoices = 8;
    static constexpr double maxSampleLengthSeconds = 120.0;
    static constexpr int maxUndoSteps = 8;
    static constexpr int modulationStep = 32;

    ReverseSamplerProcessor();
    ~ReverseSamplerProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;
    /** Host bypass: stops every note and clears the effect tails, then stays silent (see the .cpp). */
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlockBypassed;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                            { return true; }

    const juce::String getName() const override;
    bool acceptsMidi() const override                          { return true; }
    bool producesMidi() const override                         { return false; }
    bool isMidiEffect() const override                         { return false; }
    double getTailLengthSeconds() const override               { return 180.0; }

    int getNumPrograms() override                              { return 1; }
    int getCurrentProgram() override                           { return 0; }
    void setCurrentProgram (int) override                      {}
    const juce::String getProgramName (int) override           { return "Default"; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    /** Loads an audio file into a layer in the background (what the editor uses): the layer keeps
        playing its current sample until the new one is ready, which then becomes one undo step.
        A newer load, Clear, Undo or a project load cancels it. Errors go to getStatusMessage(). */
    void loadSampleAsync (int layer, const juce::File& file);

    /** Loads an audio file into a layer right away (blocking). On failure the reason is in getStatusMessage(). */
    bool loadSample (int layer, const juce::File& file);
    void clearSample (int layer);

    /** The file being loaded into the layer in the background, or an empty File. */
    juce::File getLoadingFile (int layer) const;
    bool isLoading (int layer) const                           { return getLoadingFile (layer) != juce::File(); }

    /** Waits until the background loads have finished (offline rendering waits for them too). False on timeout. */
    bool waitForPendingLoads (int timeoutMs);

    /** Clear: removes the sample and puts every setting of the layer, and its envelope, back to the default. */
    void resetLayer (int layer);

    /** True when the layer has no sample and all its settings are the defaults (Clear has nothing to do). */
    bool isLayerDefault (int layer) const;

    /** A sample the project uses but that was not found when it was loaded (empty if none).
        The path stays in the project until another sample is loaded or the layer is cleared. */
    juce::String getMissingSamplePath (int layer) const;

    bool canReadFile (const juce::File& file) const;
    juce::String getSupportedFileWildcard() const;

    rs::SampleDataPtr getSample (int layer) const;
    juce::String getStatusMessage() const;

    /** Changes whenever a sample or the status message changes. */
    int getSampleVersion() const noexcept                      { return sampleVersion.load(); }

    rs::LayerSettings getLayerSettings (int layer) const noexcept   { return rs::readLayerSettings (layerParams[(size_t) layer]); }

    /** Volume envelopes: 0 and 1 are the layers, mainEnvelope is the final one on the sum,
        dissolveEnvelope the fade after the peak (After peak = Release). */
    rs::Envelope getEnvelope (int index) const noexcept;
    void setEnvelope (int index, const rs::Envelope&);
    static rs::Envelope getDefaultEnvelope (int index) noexcept;

    rs::PlayMode getPlayMode() const noexcept;
    rs::AfterPeak getAfterPeak() const noexcept;
    double getDissolveSeconds() const noexcept;
    double getPeakCutSeconds() const noexcept;

    /** Solo audition (press and hold in the editors): plays one layer at its root note, velocity 1,
        exactly as in the full sound, until stopAudition(). Safe to call from the message thread. */
    void startAudition (int layer);
    void stopAudition();
    int getAuditionLayer() const noexcept                      { return auditionHeldLayer.load(); }

    /** Where the layers of this note sit in time, as the voices will play it. */
    rs::NoteLayout getNoteLayout (int midiNote) const;

    /** Arrive reports the longest build as latency, so the host can play the notes early. */
    void updateLatency();
    int getArriveLatency() const noexcept                      { return arriveLatency.load(); }

    /** The last note played (MIDI or on-screen keyboard), or -1. */
    int getLastNote() const noexcept                           { return lastNote.load(); }
    double getTimelinePosition() const noexcept;
    float getPlayheadPosition (int layer) const noexcept;
    double getCurrentBpm() const noexcept                      { return bpm.load(); }
    double getPlaybackRate() const noexcept                    { return getSampleRate() > 0.0 ? getSampleRate() : 48000.0; }

    //==============================================================================
    /** Undo and redo of what is edited in the plug-in: settings changed with the mouse or the
        keyboard (not host automation), envelopes, samples loaded or cleared. Edits close together
        (e.g. one knob drag, or the mouse wheel) make one step; up to maxUndoSteps are kept.
        Loading a project or preset (setStateInformation) clears the history. Message thread. */
    bool undo();
    bool redo();
    bool canUndo() const;
    bool canRedo() const;

    /** Closes the edit in progress as an undo step now, instead of when the edit has finished. */
    void commitPendingEdit();

    void swapFxSlots (int first, int second);
    // Editor type change: sets the type, then loads the neutral p1..p3/mix of the new effect, as one undo step.
    void setFxType (int slot, int newType);
    // Loads a factory preset into a slot as one undo step: the preset's type, every other setting of
    // the slot at its default, then the preset's values. The A/B routing is kept.
    void applyFxPreset (int slot, const rs::FxPreset& preset);
    rs::comp::Meters& getCompressorMeters (int slot) noexcept { return fxRack.compressorMeters (slot); }
    rs::SpectrumTap& getSpectrumTap (int slot) noexcept { return fxRack.spectrumTap (slot); }

    rs::ModMatrix& getModMatrix() noexcept                          { return modMatrix; }
    int addModulation (int source, const juce::String& paramID);
    void clearModulationRow (int row);
    juce::String getMacroName (int macro) const;
    void setMacroName (int macro, const juce::String& name);
    juce::AudioProcessorValueTreeState& getValueTreeState() noexcept   { return apvts; }
    juce::MidiKeyboardState& getKeyboardState() noexcept               { return keyboardState; }

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
    void setStatusMessage (const juce::String& message);
    void installSamples (const std::array<rs::SampleDataPtr, rs::numLayers>& newSamples);
    void updateBpm();
    void timerCallback() override;
    void handleAudition();
    void setParameterAsGesture (const juce::String& id, float value);
    void resetModulationRow (int row);

    // Undo: each step holds the values before and after, for what it changed.
    struct EditStep
    {
        struct ParameterChange { int index; float before, after; };
        struct EnvelopeChange  { int index; rs::Envelope before, after; };
        struct SampleChange    { int layer; rs::SampleDataPtr before, after; juce::String missingBefore, missingAfter; };

        std::vector<ParameterChange> parameters;
        std::vector<EnvelopeChange> envelopes;
        std::vector<SampleChange> samples;

        bool isEmpty() const noexcept   { return parameters.empty() && envelopes.empty() && samples.empty(); }
    };

    void parameterValueChanged (int, float) override {}
    void parameterGestureChanged (int parameterIndex, bool gestureIsStarting) override;
    void noteSampleEdit (int layer);   // call before a user edit changes the sample of a layer
    void noteEdit();
    void applyStep (const EditStep&, bool redoIt);
    void clearUndoHistory();
    void limitUndoMemory();
    void setSampleAndMissingPath (int layer, rs::SampleDataPtr, const juce::String& missingPath);
    void recordSampleEdit (int layer);

    // Background loading: one worker thread; each layer's newest request wins.
    struct LoadRequest
    {
        int layer = 0;
        juce::File file;
        int serial = 0;
        bool fromProject = false;   // a project load: not an undo step; a failure leaves the layer empty
    };

    void startLoad (int layer, const juce::File& file, bool fromProject);
    void cancelLoad (int layer);
    bool cancelUserLoads();
    bool anyUserLoadPending() const;
    void runLoad (const LoadRequest&);
    void finishLoad (int layer);
    rs::SampleDataPtr readSampleFile (int layer, const juce::File&, juce::String& error, const std::function<bool()>& cancelled);

    juce::AudioProcessorValueTreeState apvts;
    juce::AudioFormatManager formatManager;
    juce::MidiKeyboardState keyboardState;
    rs::SamplerSynth synth;
    rs::SamplerSynth auditionSynth;              // one voice for the solo audition
    rs::SamplerVoice* auditionVoice = nullptr;   // owned by auditionSynth
    juce::MidiBuffer noMidi;
    std::atomic<int> auditionLayer { 0 }, auditionStartSerial { 0 }, auditionStopSerial { 0 }, auditionHeldLayer { -1 };
    int auditionHandledSerial = 0, auditionNote = 60;   // audio thread
    bool auditionHeld = false;                          // audio thread

    std::array<rs::LayerParameters, rs::numLayers> layerParams;
    std::array<rs::LayerBus, rs::numLayers> layerBuses;
    rs::ModMatrix modMatrix;
    rs::FxRackDSP fxRack;
    juce::AudioBuffer<float> layerBuffer;   // channels 2N, 2N+1 = layer N
    std::atomic<float>* gainParam = nullptr;
    float lastGain = 1.0f;
    float lastGainDb = 0.0f, lastGainTarget = 1.0f;
    bool bypassed = false;   // audio thread: the previous block was a host bypass
    std::atomic<double> bpm { 120.0 };
    double hostPpq = 0.0;
    bool hostPlaying = false;

    std::array<rs::SharedEnvelope, numEnvelopes> envelopes;   // layer 1, layer 2, final, Release fade
    std::atomic<int> arriveLatency { 0 };
    std::atomic<int> lastNote { -1 };
    std::atomic<float>* playModeParam = nullptr;
    std::atomic<float>* afterPeakParam = nullptr;
    std::atomic<float>* dissolveParam = nullptr;
    std::atomic<float>* peakCutParam = nullptr;

    juce::CriticalSection loadLock;    // serialises sample changes
    mutable std::mutex infoMutex;      // guards samples and statusMessage
    std::array<rs::SampleDataPtr, rs::numLayers> samples;
    std::array<juce::String, rs::numLayers> missingPaths;   // samples of the project that were not found
    juce::String statusMessage;
    std::atomic<int> sampleVersion { 0 };

    // Undo history (message thread; the lock covers hosts that load state from another thread).
    juce::CriticalSection undoLock;
    std::vector<EditStep> undoSteps, redoSteps;   // oldest first
    juce::Array<juce::AudioProcessorParameter*> allParameters;
    std::vector<float> parameterBefore;           // value when the pending edit first touched the parameter
    std::vector<char> parameterTouched;
    std::vector<int> parameterGestures;           // gestures in progress, per parameter
    std::array<rs::Envelope, numEnvelopes> envelopeBefore;
    std::array<bool, numEnvelopes> envelopeTouched {};
    std::array<rs::SampleDataPtr, rs::numLayers> sampleBefore;
    std::array<juce::String, rs::numLayers> missingBefore;
    std::array<bool, rs::numLayers> sampleTouched {};
    int openGestures = 0;
    bool editPending = false;
    juce::uint32 lastEditTime = 0;
    std::atomic<int> restoring { 0 };             // > 0 while undo, redo or a project load change things

    // Background loads (loadingFiles, loadingFromProject and projectProblems are guarded by infoMutex).
    std::array<std::atomic<int>, rs::numLayers> loadSerials {};   // a request is current while its serial matches
    std::array<juce::File, rs::numLayers> loadingFiles;
    std::array<bool, rs::numLayers> loadingFromProject {};
    juce::StringArray projectProblems;            // samples of the last project load that could not be loaded
    std::atomic<int> pendingLoads { 0 };
    bool offlineWaitExpired = false;              // audio thread: an offline render gave up waiting for a load
    juce::WaitableEvent loadFinished;
    juce::ThreadPool loader { juce::ThreadPoolOptions{}.withThreadName ("Reverse Sampler loader").withNumberOfThreads (1) };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverseSamplerProcessor)
};
