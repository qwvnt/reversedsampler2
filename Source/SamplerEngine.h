#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace rs
{

constexpr int numLayers = 2;

//==============================================================================
/** A volume curve drawn with the mouse. Points go from x = 0 (start of the build)
    to x = 1 (the peak); y is the gain, 0..1. Straight lines join the points.
    The curve multiplies the natural loudness of the sample, so its shape is kept.
*/
struct Envelope
{
    static constexpr int maxPoints = 32;

    int numPoints = 2;
    std::array<float, maxPoints> x {}, y {};

    Envelope() noexcept;

    float valueAt (float position) const noexcept;
    bool isFlat() const noexcept;      // every point at 100 %
    void sanitise() noexcept;          // sorted, inside 0..1, first point at x = 0, last at x = 1

    juce::String toString() const;     // "0:1 0.5:0.25 1:1"
    static Envelope fromString (const juce::String&);

    /** The default fade after the peak (After peak = Release): close to (1 - x)^2. */
    static Envelope dissolveDefault() noexcept;

    bool operator== (const Envelope&) const noexcept;
    bool operator!= (const Envelope& other) const noexcept   { return ! operator== (other); }
};

/** Reads an envelope sample by sample, for positions that only move forwards. */
class EnvelopeCursor
{
public:
    void reset() noexcept   { segment = 1; }
    float valueAt (const Envelope&, float position) noexcept;

private:
    int segment = 1;
};

/** An envelope shared by the UI (one writer) and the audio thread (readers), without locks. */
class SharedEnvelope
{
public:
    SharedEnvelope() noexcept;

    void set (const Envelope&) noexcept;
    Envelope get() const noexcept;

private:
    std::atomic<juce::uint32> sequence { 0 };
    std::atomic<int> numPoints { 2 };
    std::array<std::atomic<float>, Envelope::maxPoints> xs, ys;

    JUCE_DECLARE_NON_COPYABLE (SharedEnvelope)
};

//==============================================================================
/** A decoded audio file. It is created once and never modified afterwards, so the
    audio thread and the UI can share it safely through a shared_ptr.
*/
struct SampleData
{
    juce::AudioBuffer<float> audio;    // 1 or 2 channels in the original (forward) order
    double sampleRate = 44100.0;
    juce::File file;
    juce::Time fileTime;               // modification time when it was read
    bool wasTruncated = false;

    // Waveform overview in forward order: min/max over all channels per bin.
    std::vector<float> overviewMin, overviewMax;
    float overviewPeak = 0.0f;

    // Transients (onsets) in forward order, in samples: where the level jumps up.
    std::vector<int> onsets;

    double getLengthInSeconds() const noexcept   { return audio.getNumSamples() / sampleRate; }
    void buildOverview (int maxNumBins);
    void detectOnsets();
};

using SampleDataPtr = std::shared_ptr<const SampleData>;

//==============================================================================
/** The instrument: one sample per layer (either may be empty). Immutable. */
class InstrumentSound final : public juce::SynthesiserSound
{
public:
    explicit InstrumentSound (std::array<SampleDataPtr, numLayers> layerSamples)
        : samples (std::move (layerSamples)) {}

    bool appliesToNote (int) override      { return true; }
    bool appliesToChannel (int) override   { return true; }

    const SampleData* getSample (int layer) const noexcept   { return samples[(size_t) layer].get(); }

private:
    std::array<SampleDataPtr, numLayers> samples;
};

//==============================================================================
enum class TimeMode   { stretch = 0, resample = 1 };
enum class LengthMode { sample = 0, seconds = 1, beats = 2 };
enum class PlayMode   { swell = 0, arrive = 1 };          // build from the key press / peak on the note
enum class AfterPeak  { cut = 0, release = 1, freeze = 2, loop = 3 };   // 1 was Dissolve before 0.4.1

/** One layer's settings, read from the parameters when a note starts. */
struct LayerSettings
{
    bool enabled = true;
    bool reversed = false;
    TimeMode timeMode = TimeMode::stretch;
    LengthMode lengthMode = LengthMode::sample;
    double lengthSeconds = 4.0;
    double lengthBeats = 4.0;
    int pitch = 0;                 // semitones
    int rootNote = 60;
    double start = 0.0, end = 1.0; // region, as fractions of the sample length
    double peakOffset = 0.0;       // seconds; > 0 peaks after the other layer, < 0 before
};

/** Pointers to the parameter values a layer reads. */
struct LayerParameters
{
    std::atomic<float>* enabled = nullptr;
    std::atomic<float>* reverse = nullptr;
    std::atomic<float>* timeMode = nullptr;
    std::atomic<float>* lengthMode = nullptr;
    std::atomic<float>* lengthSeconds = nullptr;
    std::atomic<float>* lengthBeats = nullptr;
    std::atomic<float>* pitch = nullptr;
    std::atomic<float>* rootNote = nullptr;
    std::atomic<float>* start = nullptr;    // 0..100 %
    std::atomic<float>* end = nullptr;      // 0..100 %
    std::atomic<float>* peakOffset = nullptr;   // seconds
};

LayerSettings readLayerSettings (const LayerParameters&) noexcept;

//==============================================================================
/** How one layer plays one note. Used by the audio thread and by the UI. */
struct LayerTiming
{
    int regionStart = 0, regionEnd = 0;   // source samples, forward order, end exclusive
    double readIncrement = 1.0;           // source samples per reader sample
    double readLength = 0.0;              // reader length in host samples
    double outputLength = 0.0;            // what you hear, in host samples
    double inputPerOutput = 1.0;          // stretcher speed: reader samples per output sample
    double transpose = 1.0;               // stretcher frequency factor
    bool useStretcher = false;
    bool stretchLimited = false;          // the requested length was outside the supported range

    /** How many times longer than the original region the note lasts. */
    double getStretchFactor() const noexcept   { return readLength > 0.0 ? outputLength / readLength : 1.0; }
};

constexpr double maxStretch = 16.0;   // a layer can play up to 16 times longer...
constexpr double maxSqueeze = 8.0;    // ...or up to 8 times shorter than its region

LayerTiming computeLayerTiming (const SampleData&, const LayerSettings&, int midiNote, double hostRate, double bpm) noexcept;

/** Where the layers of one note sit in time. Each layer is its whole build (the region,
    stretched), placed so that the builds end together at the natural peak; a layer's peak
    offset moves it (a layer moved earlier than the key press has its start trimmed; the other
    layer and the PEAK stay where they are). The PEAK (the natural peak, moved earlier by the peak cut) is one moment
    for the whole note: every layer plays until it, and what comes after the peak (Cut,
    Release, Freeze, Loop) starts there for all layers at once.
    Swell: the earliest build starts with the key press. Arrive: the PEAK lies exactly one
    latency after the note-on, so with the host's latency compensation it lands on the note.
*/
struct NoteLayout
{
    struct Layer
    {
        bool active = false;       // the build starts before the PEAK and ends after the note start
        LayerTiming timing;
        double start = 0.0;        // host samples after the note-on; negative = the start of the build is trimmed
        double length = 0.0;       // what plays up to the PEAK (or less, when the build ends sooner), host samples
        double fullLength = 0.0;   // the whole build; the layer envelope spans this
        bool reachesPeak = false;  // the build still sounds at the PEAK (Freeze holds it, Release lets it play on)
        bool cutAway = false;      // the build starts after the PEAK (or ends before the note start): silent

        double getPeak() const noexcept   { return start + length; }       // where the played part ends
        double getEnd() const noexcept    { return start + fullLength; }   // where the whole build ends (its natural peak)
        double getTrim() const noexcept   { return juce::jmax (0.0, -start); }   // what is trimmed at the start (before the note)
    };

    std::array<Layer, numLayers> layers;
    bool anyLayer = false;
    double peak = 0.0;         // the PEAK, host samples after the note-on; one loop cycle ends here
    double buildStart = 0.0;   // where the first layer starts to sound (>= 0): the final envelope runs from here to the peak
    double end = 0.0;          // where the last build ends, also after the PEAK (what Release can still play)
    double cut = 0.0;          // the PEAK lies this much before the natural peak (host samples)
};

constexpr double maxArriveSeconds = 1.0;    // cap host latency; longer builds are trimmed at the start

NoteLayout computeNoteLayout (const std::array<const SampleData*, numLayers>&, const std::array<LayerSettings, numLayers>&,
                              int midiNote, PlayMode, double arriveLatency, double hostRate, double bpm,
                              double peakCutSeconds = 0.0) noexcept;

/** The latency Arrive needs: the longest build minus its positive peak offset (at each layer's root note), minus the peak cut. */
int computeArriveLatency (const std::array<const SampleData*, numLayers>&, const std::array<LayerSettings, numLayers>&,
                          double hostRate, double bpm, double peakCutSeconds = 0.0) noexcept;

//==============================================================================
/** Warm note starts for one layer in Stretch mode.

    Starting a stretched layer pre-rolls the time-stretcher (about 2 ms of FFT work per stereo layer):
    a chord of five notes on two layers is 20 ms inside one audio block, far over the budget of any
    small buffer. The pre-roll depends only on the layer's settings, the sample and the note, so a
    background thread computes it ahead of time for the notes around the one last played, and a note
    start copies the finished state in (a few hundred kilobytes, tens of microseconds). A note whose
    key is not here starts the slow way, exactly as before; the output is identical either way.

    Entries are claimed with a state machine, so the audio thread never waits for the worker and the
    worker never rewrites an entry while it is being read.
*/
class LayerPlayer;

class StretchStartCache
{
public:
    static constexpr int capacity = 24;   // the last note +/- an octave; about 1 MB per stereo entry, allocated only in Stretch mode

    /** Everything the pre-roll reads. Compared bytewise, so build it the same way on both threads. */
    struct Key
    {
        double hostRate = 0.0, readIncrement = 0.0, inputPerOutput = 0.0, transpose = 0.0;
        juce::int64 inputStart = 0;
        const float* channels[2] {};
        int regionStart = 0, regionEnd = 0, sampleLength = 0, seekLength = 0;
        bool reversed = false, mono = false, holdAtPeak = false;
    };

    StretchStartCache();
    ~StretchStartCache();

    /** Worker: warms `note` of this layer from the player's last start (see LayerPlayer::warmedStart).
        May allocate. Returns false when the start was not stretched, already cached, or every
        candidate entry was busy; `centre` is the note the cache is kept around. */
    bool store (const LayerPlayer& warmed, int note, int centre);
    /** Worker (or any thread while the audio thread cannot run): forgets everything. */
    void clear() noexcept;
    int countReady() const noexcept;

private:
    friend class LayerPlayer;

    struct Impl;
    std::unique_ptr<Impl> impl;
};

//==============================================================================
/** Plays one layer of one note. The region is read forwards or backwards and
    resampled to the host rate. In Stretch mode, a time-stretcher then changes the
    pitch and the length independently; it is skipped when neither changes, so the
    root note plays the file untouched.
*/
class LayerPlayer
{
public:
    LayerPlayer();
    ~LayerPlayer();

    /** Allocates the stretcher. Call before playback (not on the audio thread). */
    void prepare (double hostSampleRate);

    /** What the layer does at the PEAK. Cut and Loop end it there (the voice restarts it for Loop),
        Release plays on to the end of the build (the voice fades it), Freeze holds the sound of
        the PEAK, cross-fading into it, when the build still sounds there. */
    struct PeakHold
    {
        AfterPeak mode = AfterPeak::cut;
        double peakSamples = 0.0;   // output samples from the start of the build to the PEAK
        bool reachesPeak = false;   // the build still sounds at the PEAK
    };

    /** Starts the build. skipSamples > 0 starts that far into it (Arrive, when the build is longer than the latency). */
    void start (const SampleData&, const LayerTiming&, bool shouldBeReversed, const Envelope& layerEnvelope,
                PeakHold, juce::int64 skipSamples = 0) noexcept;
    void stop() noexcept                     { active = false; holding = false; }
    void startFadeOut() noexcept;            // short fade, then stop

    bool isActive() const noexcept           { return active; }
    bool isFadingOut() const noexcept        { return fadeStep < 0.0f; }
    bool isHoldingPeak() const noexcept      { return active && holding; }

    /** Adds numSamples of output to left/right, multiplied sample by sample by gains. */
    void render (float* left, float* right, const float* gains, int numSamples) noexcept;

    /** Position (0..1) in the forward-ordered sample, or -1 when inactive. */
    float getForwardPosition() const noexcept;

    /** One sample of the reader (the region in playback order, at the host rate). The source is
        interpolated with a windowed sinc; when the reader moves through the source faster than
        one sample per sample, the kernel widens so that nothing above the new Nyquist folds back.
        readerStep: how far the reader index advances per output sample (1 = the reader itself). */
    float readSample (int channel, double readerIndex, double readerStep = 1.0) const noexcept;
    void readFrame (double readerIndex, double readerStep, float& left, float& right) const noexcept;

    static constexpr int stretchChunk = 128;

    /** Warm starts: with a cache, a stretched start whose key is warm copies the state in instead of
        pre-rolling. Null (the default) always pre-rolls. */
    void setStartCache (StretchStartCache* cache) noexcept   { startCache = cache; }
    /** Whether the last start() pre-rolled the stretcher itself (so its state can be stored), and its key. */
    bool lastStartWasWarmable() const noexcept               { return lastStartSeeked; }
    const StretchStartCache::Key& lastStartKey() const noexcept { return startKey; }

private:
    friend class StretchStartCache;
    struct Stretcher;

    struct Grain
    {
        double position = 0.0;   // reader index where the grain starts
        int age = 0;             // output samples since the grain started
    };

    static constexpr int numGrains = 4;

    struct Kernel;
    bool makeKernel (double readerIndex, double readerStep, Kernel&) const noexcept;
    float applyKernel (const Kernel&, int channel) const noexcept;
    void prepareWideKernel (double ratio) noexcept;

    // Kernels for one read speed above 1 (the note's), tabulated at the note start.
    std::vector<float> wideTable;
    double wideRatio = 0.0;
    int wideReach = 0;

    float nextLayerGain() noexcept;
    void startHold (juce::int64 alreadyHeld, int fadeLength) noexcept;
    void respawnGrain (Grain&) noexcept;
    void renderHold (float* left, float* right, const float* gains, int numSamples) noexcept;

    void fillInput (std::array<std::vector<float>, 2>& into, juce::int64 readerStart, int numFrames) noexcept;

    std::unique_ptr<Stretcher> stretcher;
    StretchStartCache* startCache = nullptr;
    StretchStartCache::Key startKey;
    bool lastStartSeeked = false;
    std::array<std::vector<float>, 2> stretchInput, stretchOutput, seekInput;
    bool stretchMono = false;                 // a mono sample goes through a one-channel stretcher (half the work)

    const float* channels[2] {};
    int sampleLength = 0;
    LayerTiming timing;
    bool reversed = false, active = false, stretching = false;
    double hostRate = 0.0;

    juce::int64 outputLength = 0, outputPosition = 0;   // what the stream plays: to the PEAK, or the whole build (Release)
    double fullOutputLength = 1.0;            // the whole build: the layer envelope spans this
    juce::int64 inputFed = 0;                 // reader samples handed to the stretcher
    juce::int64 fadeInStart = 0;              // the de-click fade-in starts here (0, or the skip in Arrive)
    int fadeInLength = 0, fadeOutLength = 0;  // de-click fades when the region starts/ends inside the sample
    float fadeGain = 1.0f, fadeStep = 0.0f;   // fade-out when the layer is switched off

    Envelope envelope;                        // the layer's drawn volume curve
    EnvelopeCursor envelopeCursor;
    float envelopeAtPeak = 1.0f;
    bool envelopeIsFlat = true;

    // Freeze: a cloud of short grains from the 300 ms before the PEAK, cross-faded in just before it.
    PeakHold peakHold;
    bool holding = false, holdAtPeak = false;
    juce::int64 peakPosition = 0;             // stream position of the PEAK
    juce::int64 holdStart = 0;                // stream position where the cross-fade into the grains starts
    int holdFadeLength = 1;                   // the grains' fade-in (the cross-fade)
    std::array<Grain, numGrains> grains;
    std::vector<float> grainWindow;           // Hann window, allocated in prepare()
    int grainLength = 0;
    double grainRate = 1.0, windowStart = 0.0, windowEnd = 0.0;
    juce::int64 holdPosition = 0;
    juce::uint32 randomState = 1;

    JUCE_DECLARE_NON_COPYABLE (LayerPlayer)
};

//==============================================================================
struct GlobalParameters
{
    std::atomic<float>* attackMs = nullptr;
    std::atomic<float>* releaseMs = nullptr;
    std::atomic<float>* releaseCurve = nullptr;   // -100..100
    std::atomic<float>* playMode = nullptr;       // PlayMode
    std::atomic<float>* afterPeak = nullptr;      // AfterPeak
    std::atomic<float>* dissolveSeconds = nullptr;   // Release after the peak: the fade time (the parameter was Dissolve)
    std::atomic<float>* peakCutSeconds = nullptr;  // the peak moved earlier by this much

    const std::atomic<int>* arriveLatency = nullptr;              // samples, as reported to the host
    std::array<const SharedEnvelope*, numLayers> layerEnvelopes {};
    const SharedEnvelope* mainEnvelope = nullptr;                 // on the sum of the layers
    const SharedEnvelope* dissolveEnvelope = nullptr;             // Release after the peak: the shape of the fade
};

/** Attack and release of a note. The release follows a curve: 0 = straight line,
    > 0 drops fast and leaves a long tail, < 0 holds longer and falls off at the end.
*/
class AmpEnvelope
{
public:
    void setSampleRate (double newRate) noexcept   { rate = newRate; }
    void noteOn (float attackSeconds) noexcept;
    void noteOff (float releaseSeconds, float curve) noexcept;
    void reset() noexcept                          { state = State::idle; level = 0.0f; }

    float getNextSample() noexcept;
    bool isActive() const noexcept                 { return state != State::idle; }
    bool isReleasing() const noexcept              { return state == State::release; }

private:
    enum class State { idle, attack, sustain, release };

    double rate = 44100.0;
    State state = State::idle;
    float level = 0.0f, attackStep = 0.0f;
    float releaseStart = 0.0f, releaseProgress = 0.0f, releaseStep = 0.0f, releasePower = 1.0f;
};

/** One note. Renders layer N into channels 2N and 2N+1 of the buffer. */
class SamplerVoice final : public juce::SynthesiserVoice
{
public:
    SamplerVoice (const GlobalParameters&, const std::array<LayerParameters, numLayers>&, const std::atomic<double>& bpm);

    bool canPlaySound (juce::SynthesiserSound*) override;
    void startNote (int midiNoteNumber, float velocity, juce::SynthesiserSound*, int currentPitchWheelPosition) override;
    void stopNote (float velocity, bool allowTailOff) override;
    /** Ends the note at once, without the short fade (host bypass, prepareToPlay). */
    void stopSilently();
    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}
    void setCurrentPlaybackSampleRate (double newRate) override;
    /** Warm starts for the layers (see StretchStartCache). Call before playback starts. */
    void setStartCaches (const std::array<StretchStartCache*, numLayers>& caches) noexcept   { for (int l = 0; l < numLayers; ++l) players[(size_t) l].setStartCache (caches[(size_t) l]); }

    void renderNextBlock (juce::AudioBuffer<float>&, int startSample, int numSamples) override;
    using juce::SynthesiserVoice::renderNextBlock;

    bool isLayerActive (int layer) const noexcept          { return players[(size_t) layer].isActive(); }
    float getForwardPosition (int layer) const noexcept    { return players[(size_t) layer].getForwardPosition(); }

    /** Time on the note's layout (as shown in the SAMPLE view), in host samples; loops map onto the first cycle. */
    double getTimelinePosition() const noexcept;
    double getBuildPeak() const noexcept   { return layout.peak; }

    /** Solo: the next notes play only this layer (even if it is switched off), placed exactly as in the
        full sound, but starting right away instead of after the other layers' lead-in. -1 = all layers. */
    void setSoloLayer (int layer) noexcept   { soloLayer = layer; }

    /** A note stopped at once (a stolen voice, a sample change) fades out over a few milliseconds
        instead of breaking off with a click. The fade is rendered when the note stops; this adds
        what is left of it to the layer channels. Called by SamplerSynth after the voices. */
    void renderTail (juce::AudioBuffer<float>&, int startSample, int numSamples) noexcept;

    static constexpr double tailSeconds = 0.005;

private:
    void captureTail() noexcept;
    void finishNote();
    void startCycle (juce::int64 cycleStartTime);
    void startLayer (int layer);
    juce::int64 nextEventTime() const noexcept;
    juce::int64 getCycleEnd() const noexcept;
    void handleEvents();
    float nextReleaseFadeGain (juce::int64 time) noexcept;
    bool isLooping() const noexcept   { return afterPeak == AfterPeak::loop; }
    const NoteLayout& currentLayout() const noexcept   { return cycleIndex == 0 ? layout : loopLayout; }

    GlobalParameters globals;
    std::array<LayerParameters, numLayers> layerParams;
    const std::atomic<double>& bpm;

    std::array<LayerPlayer, numLayers> players;
    AmpEnvelope envelope;
    float velocityGain = 0.0f;
    std::array<float, LayerPlayer::stretchChunk> gains {};

    // The note: where the layers sit, and what happens after the peak.
    const InstrumentSound* instrument = nullptr;
    std::array<LayerSettings, numLayers> settings;
    NoteLayout layout, loopLayout;       // the first cycle, and the next ones (Loop)
    std::array<Envelope, numLayers> layerEnvelopes;
    Envelope mainEnvelope;
    EnvelopeCursor mainCursor;
    bool mainEnvelopeIsFlat = true;
    bool noteIsArrive = false;
    juce::int64 noteLatency = 0;
    AfterPeak afterPeak = AfterPeak::cut;
    double fadeSamples = 0.0;            // Release: the fade after the PEAK
    Envelope fadeEnvelope;
    EnvelopeCursor fadeCursor;
    bool fadeEndsSilent = true;          // the drawn fade ends at zero: the note ends with it
    int soloLayer = -1;
    bool layerIsSolo (int layer) const noexcept   { return soloLayer == layer; }
    bool layerSwitchedOff (int layer) const noexcept;

    juce::int64 noteTime = 0;            // host samples since the note-on
    juce::int64 cycleStart = 0;          // note time where the current cycle started
    int cycleIndex = 0;
    std::array<juce::int64, numLayers> layerStartTime {};   // note time; -1 = nothing to start
    std::array<bool, numLayers> layerStarted {};
    juce::int64 releaseTime = -1;        // Arrive: the note-off is delayed by the latency

    juce::AudioBuffer<float> tailBuffer, tailScratch;   // the fade of a note stopped at once
    std::vector<float> tailFade;
    int tailLength = 0, tailPosition = 0;
};

//==============================================================================
class SamplerSynth final : public juce::Synthesiser
{
public:
    SamplerSynth();

    /** Takes ownership of the voice. Call before playback starts. */
    void addSamplerVoice (SamplerVoice* voice);

    /** Swaps in a new sound and stops all voices. Safe to call while audio is running. */
    void replaceSound (juce::SynthesiserSound::Ptr newSound);

    /** Stops every note at once, without the short fade, and forgets held keys and the sustain
        pedal. Safe to call while audio is running. */
    void silenceAll();

    /** Forward position (0..1) of the newest note on this layer, or -1 when silent. */
    float getPlayheadPosition (int layer) const noexcept   { return playheads[(size_t) layer].load (std::memory_order_relaxed); }

    /** Position of the newest note on its layout (host samples), or -1 when silent. */
    double getTimelinePosition() const noexcept           { return timeline.load (std::memory_order_relaxed); }
    double getBuildPeak() const noexcept                  { return buildPeak.load (std::memory_order_relaxed); }

private:
    void renderVoices (juce::AudioBuffer<float>&, int startSample, int numSamples) override;
    using juce::Synthesiser::renderVoices;

    std::vector<SamplerVoice*> samplerVoices;   // owned by the base class
    std::array<std::atomic<float>, numLayers> playheads;
    std::atomic<double> timeline { -1.0 };
    std::atomic<double> buildPeak { 0.0 };
};

} // namespace rs
