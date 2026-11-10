#include "FxRackEditor.h"
#include "Theme.h"
#include "Controls.h"
#include <cmath>

namespace {
std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachSlider (juce::AudioProcessorValueTreeState& state, const juce::String& id, juce::Slider& slider) {
    tagParameter (slider, id);
    return std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, id, slider);
}
class EqCurve final : public juce::Component, private juce::Timer {
public:
    EqCurve (ReverseSamplerProcessor& p, int slot) : processor (p), tap (p.getSpectrumTap (slot)), fft (fftOrder) {
        auto& state = p.getValueTreeState();
        const char* ids[] { "p1", "p2", "p3", "eqAir", "eqLowCut", "eqHighCut", "eqMode" };
        for (size_t i = 0; i < values.size(); ++i) {
            paramIDs[i] = rs::fxID (slot, ids[i]);
            values[i] = state.getRawParameterValue (paramIDs[i]);
        }
        for (int k = 0; k < points; ++k) freqs[(size_t) k] = (float) (20.0 * std::pow (1000.0, (double) k / (points - 1)));
        for (int k = 0; k < fftSize; ++k) window[(size_t) k] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) k / (float) fftSize);
        float sum = 0.0f;
        for (auto w : window) sum += w;
        windowGain = 2.0f / sum;
        spectrumPre.fill (floorDb - 1.0f); spectrumPost.fill (floorDb - 1.0f);
        setInterceptsMouseClicks (false, false);
        startTimerHz (30);
    }
    ~EqCurve() override { tap.wanted.store (false); }
    void paint (juce::Graphics& g) override {
        const auto r = getLocalBounds().toFloat();
        g.setColour (Theme::background); g.fillRoundedRectangle (r, 5.0f);
        paintSpectrum (g, r);
        g.setColour (Theme::border.withAlpha (0.6f));
        for (float f : { 100.0f, 1000.0f, 10000.0f }) g.drawVerticalLine ((int) xFor (f), r.getY(), r.getBottom());
        for (float db : { -12.0f, -6.0f, 6.0f, 12.0f }) g.drawHorizontalLine ((int) yFor (db), r.getX(), r.getRight());
        g.setColour (Theme::dimText.withAlpha (0.8f));
        g.drawHorizontalLine ((int) yFor (0.0f), r.getX(), r.getRight());
        g.setFont (juce::FontOptions (10.0f));
        g.drawText ("100", juce::Rectangle<float> (xFor (100.0f) + 2, r.getBottom() - 13, 30, 12), juce::Justification::centredLeft);
        g.drawText ("1k", juce::Rectangle<float> (xFor (1000.0f) + 2, r.getBottom() - 13, 30, 12), juce::Justification::centredLeft);
        g.drawText ("10k", juce::Rectangle<float> (xFor (10000.0f) + 2, r.getBottom() - 13, 30, 12), juce::Justification::centredLeft);
        auto fill = curve; fill.lineTo (r.getRight(), yFor (0.0f)); fill.lineTo (r.getX(), yFor (0.0f)); fill.closeSubPath();
        g.setColour (Theme::layer1.withAlpha (0.18f)); g.fillPath (fill);
        g.setColour (modulated ? Theme::modulation : Theme::layer1); g.strokePath (curve, juce::PathStrokeType (2.0f));
        g.setColour (Theme::border); g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.0f);
    }
    void resized() override { rebuild(); }
private:
    static constexpr int points = 160;
    static constexpr float rangeDb = 15.0f;
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder, columns = 200;
    static constexpr float floorDb = -90.0f, topDb = 0.0f, tiltDbPerOctave = 4.5f, fallPerFrame = 40.0f / 30.0f;
    float xFor (float hz) const { return (float) getWidth() * std::log (hz / 20.0f) / std::log (1000.0f); }
    float yFor (float db) const { const float h = (float) getHeight(); return h * 0.5f - juce::jlimit (-rangeDb, rangeDb, db) / rangeDb * (h * 0.5f - 4.0f); }
    float ySpectrum (float db) const { const float h = (float) getHeight(); return h - 2.0f - (juce::jlimit (floorDb, topDb, db) - floorDb) / (topDb - floorDb) * (h - 6.0f); }
    void paintSpectrum (juce::Graphics& g, juce::Rectangle<float> r) {
        if (! hasSpectrum) return;
        const auto path = [&] (const std::array<float, columns>& db, bool closed) {
            juce::Path p;
            for (int c = 0; c < columns; ++c) {
                const float x = r.getX() + r.getWidth() * (float) c / (columns - 1), y = ySpectrum (db[(size_t) c]);
                if (c == 0) {
                    if (closed) { p.startNewSubPath (x, r.getBottom()); p.lineTo (x, y); }
                    else p.startNewSubPath (x, y);
                } else p.lineTo (x, y);
            }
            if (closed) { p.lineTo (r.getRight(), r.getBottom()); p.closeSubPath(); }
            return p;
        };
        g.setColour (Theme::layer2.withAlpha (0.2f)); g.fillPath (path (spectrumPost, true));
        g.setColour (Theme::layer2.withAlpha (0.5f)); g.strokePath (path (spectrumPost, false), juce::PathStrokeType (1.0f));
        g.setColour (Theme::dimText.withAlpha (0.45f)); g.strokePath (path (spectrumPre, false), juce::PathStrokeType (1.0f));
    }
    void analyse (const float* samples, std::array<float, columns>& shown) {
        for (int k = 0; k < fftSize; ++k) fftData[(size_t) k] = samples[k] * window[(size_t) k];
        std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
        fft.performFrequencyOnlyForwardTransform (fftData.data(), true);
        const double binHz = lastRate / fftSize;
        for (int c = 0; c < columns; ++c) {
            const double lo = 20.0 * std::pow (1000.0, (c - 0.5) / (columns - 1)), hi = 20.0 * std::pow (1000.0, (c + 0.5) / (columns - 1));
            const int first = juce::jlimit (1, fftSize / 2 - 1, (int) std::floor (lo / binHz));
            const int lastBin = juce::jlimit (first, fftSize / 2 - 1, (int) std::ceil (hi / binHz));
            float peak = 0.0f;
            for (int b = first; b <= lastBin; ++b) peak = std::max (peak, fftData[(size_t) b]);
            const float db = juce::Decibels::gainToDecibels (peak * windowGain, floorDb - 20.0f)
                           + tiltDbPerOctave * (float) std::log2 (std::sqrt (lo * hi) / 1000.0);
            auto& s = shown[(size_t) c];
            s = db > s ? db : std::max (db, s - fallPerFrame);
        }
    }
    void timerCallback() override {
        bool showing = true;
        for (const juce::Component* c = this; c->getParentComponent() != nullptr && showing; c = c->getParentComponent()) showing = c->isVisible();
        tap.wanted.store (showing);
        if (! showing) return;
        auto& matrix = processor.getModMatrix();
        bool changed = false, anyModulated = false;
        for (size_t i = 0; i < values.size(); ++i) {
            const float base = values[i]->load();
            const float v = i == 6 ? base : matrix.displayValue (paramIDs[i], base);
            anyModulated = anyModulated || ! juce::exactlyEqual (v, base);
            if (! juce::exactlyEqual (v, last[i])) { last[i] = v; changed = true; }
        }
        if (anyModulated != modulated) { modulated = anyModulated; changed = true; }
        const double sr = processor.getSampleRate() > 0.0 ? processor.getSampleRate() : 48000.0;
        if (! juce::exactlyEqual (sr, lastRate)) { lastRate = sr; changed = true; }
        if (changed) rebuild();
        const unsigned written = tap.written.load (std::memory_order_acquire);
        if (written != lastWritten) {
            lastWritten = written;
            tap.read (pre.data(), post.data(), fftSize);
            analyse (pre.data(), spectrumPre);
            analyse (post.data(), spectrumPost);
            hasSpectrum = true;
            repaint();
        } else if (hasSpectrum) {
            bool visible = false;
            for (auto* spectrum : { &spectrumPre, &spectrumPost })
                for (auto& s : *spectrum) { s = std::max (floorDb - 1.0f, s - fallPerFrame); visible = visible || s > floorDb; }
            hasSpectrum = visible;
            repaint();
        }
    }
    void rebuild() {
        const auto settings = rs::eqSettings (last[0], last[1], last[2], last[3], last[4], last[5], last[6]);
        rs::eq::response (settings, lastRate, freqs.data(), dbs.data(), points);
        curve.clear();
        for (int k = 0; k < points; ++k) {
            const float x = (float) getWidth() * (float) k / (points - 1), y = yFor (dbs[(size_t) k]);
            if (k == 0) curve.startNewSubPath (x, y); else curve.lineTo (x, y);
        }
        repaint();
    }
    ReverseSamplerProcessor& processor;
    rs::SpectrumTap& tap;
    juce::dsp::FFT fft;
    std::array<juce::String, 7> paramIDs;
    std::array<std::atomic<float>*, 7> values {};
    std::array<float, 7> last { 50.0f, 50.0f, 50.0f, 50.0f, 20.0f, 20000.0f, 0.0f };
    double lastRate = 48000.0;
    std::array<float, points> freqs {}, dbs {};
    std::array<float, fftSize> window {}, pre {}, post {};
    std::array<float, 2 * fftSize> fftData {};
    std::array<float, columns> spectrumPre {}, spectrumPost {};
    float windowGain = 1.0f;
    unsigned lastWritten = 0;
    bool hasSpectrum = false, modulated = false;
    juce::Path curve;
};

class CompressorView final : public juce::Component, private juce::Timer {
public:
    CompressorView (ReverseSamplerProcessor& p, int slot) : processor (p), index (slot) {
        auto& state = p.getValueTreeState();
        const char* ids[] { "p1", "p2", "p3", "mix", "cpMode", "cpKnee", "type" };
        for (size_t i = 0; i < values.size(); ++i) values[i] = state.getRawParameterValue (rs::fxID (slot, ids[i]));
        history.fill ({ floorDb, floorDb, 0.0f });
        setInterceptsMouseClicks (false, false);
    }
    void visibilityChanged() override { if (isVisible()) startTimerHz (30); else stopTimer(); }
    void paint (juce::Graphics& g) override {
        const auto s = settings();
        auto area = getLocalBounds().toFloat();
        g.setColour (Theme::background); g.fillRoundedRectangle (area, 5.0f);
        g.setColour (Theme::border); g.drawRoundedRectangle (area.reduced (0.5f), 5.0f, 1.0f);
        area = area.reduced (6.0f);
        auto historyArea = area.removeFromBottom (juce::jmax (34.0f, area.getHeight() * 0.28f));
        area.removeFromBottom (6.0f);
        auto meterArea = area.removeFromRight (42.0f);
        area.removeFromRight (4.0f);
        const float side = juce::jmin (area.getWidth(), area.getHeight());
        const auto plot = area.withSizeKeepingCentre (side, side);
        paintCurve (g, plot, s);
        paintMeters (g, meterArea);
        paintHistory (g, historyArea);
    }
private:
    static constexpr float floorDb = -60.0f;
    static constexpr int historySize = 120;
    struct Frame { float inDb, outDb, reductionDb; };
    rs::comp::Settings settings() const {
        rs::comp::Settings s;
        s.mode = values[4] != nullptr ? juce::jlimit (0, 1, (int) std::round (values[4]->load())) : 0;
        s.thresholdDb = rs::comp::thresholdFromKnob (values[0]->load());
        s.ratio = rs::comp::ratioFromKnob (values[1]->load());
        s.makeupDb = rs::comp::makeupFromKnob (values[2]->load());
        s.mix = values[3]->load() * 0.01f;
        s.kneeDb = values[5] != nullptr ? values[5]->load() : 6.0f;
        return s;
    }
    static float toDb (float gain) { return gain > 1.0e-6f ? juce::jmax (floorDb, 20.0f * std::log10 (gain)) : floorDb; }
    static float mapDb (float db, float top, float bottom, float lo, float hi) { return bottom + (juce::jlimit (lo, hi, db) - lo) / (hi - lo) * (top - bottom); }
    void paintCurve (juce::Graphics& g, juce::Rectangle<float> r, const rs::comp::Settings& s) {
        const auto x = [&] (float db) { return mapDb (db, r.getRight(), r.getX(), floorDb, 0.0f); };
        const auto y = [&] (float db) { return mapDb (db, r.getY(), r.getBottom(), floorDb, 0.0f); };
        g.setColour (Theme::border.withAlpha (0.5f));
        for (float db = -48.0f; db < 0.0f; db += 12.0f) { g.drawVerticalLine ((int) x (db), r.getY(), r.getBottom()); g.drawHorizontalLine ((int) y (db), r.getX(), r.getRight()); }
        g.drawRect (r, 1.0f);
        const float knee = rs::comp::kneeOf (s);
        g.setColour (Theme::layer1.withAlpha (0.12f));
        g.fillRect (juce::Rectangle<float>::leftTopRightBottom (x (s.thresholdDb - 0.5f * knee), r.getY(), juce::jmax (x (s.thresholdDb - 0.5f * knee) + 1.0f, x (s.thresholdDb + 0.5f * knee)), r.getBottom()));
        g.setColour (Theme::dimText.withAlpha (0.5f));
        g.drawLine (x (floorDb), y (floorDb), x (0.0f), y (0.0f), 1.0f);
        const float dashes[] { 4.0f, 3.0f };
        g.drawDashedLine ({ x (s.thresholdDb), r.getY(), x (s.thresholdDb), r.getBottom() }, dashes, 2, 1.0f);
        juce::Path curve;
        for (int k = 0; k <= 120; ++k) {
            const float in = floorDb + (0.0f - floorDb) * (float) k / 120.0f;
            const float wet = rs::comp::outputDb (s, in);
            const float out = 20.0f * std::log10 (juce::jmax (1.0e-6f, (1.0f - s.mix) * std::pow (10.0f, in / 20.0f) + s.mix * std::pow (10.0f, wet / 20.0f)));
            if (k == 0) curve.startNewSubPath (x (in), y (out)); else curve.lineTo (x (in), y (out));
        }
        g.setColour (Theme::layer1); g.strokePath (curve, juce::PathStrokeType (2.0f));
        if (displayLevel > floorDb + 1.0f) {
            const float wet = displayLevel - displayReduction + s.makeupDb;
            const float out = 20.0f * std::log10 (juce::jmax (1.0e-6f, (1.0f - s.mix) * std::pow (10.0f, displayLevel / 20.0f) + s.mix * std::pow (10.0f, wet / 20.0f)));
            g.setColour (juce::Colours::white);
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre ({ x (displayLevel), y (out) }));
        }
        g.setColour (Theme::dimText); g.setFont (juce::FontOptions (10.0f));
        g.drawText ("IN", r.withTrimmedTop (r.getHeight() - 13.0f).withTrimmedRight (3.0f), juce::Justification::centredRight);
        g.drawText ("OUT", r.withHeight (13.0f).withTrimmedLeft (3.0f), juce::Justification::centredLeft);
        g.drawText ("KNEE " + juce::String (knee, 1) + " dB", r.withHeight (13.0f).withTrimmedRight (3.0f), juce::Justification::centredRight);
    }
    void paintMeters (juce::Graphics& g, juce::Rectangle<float> r) {
        auto label = r.removeFromBottom (12.0f);
        const float w = r.getWidth() / 3.0f;
        const auto bar = [&] (juce::Rectangle<float> column, float fraction, bool fromTop, juce::Colour colour) {
            column = column.reduced (2.0f, 0.0f);
            g.setColour (Theme::panelLight); g.fillRect (column);
            const float h = column.getHeight() * juce::jlimit (0.0f, 1.0f, fraction);
            g.setColour (colour); g.fillRect (fromTop ? column.withHeight (h) : column.withTrimmedTop (column.getHeight() - h));
        };
        bar (r.withWidth (w), (displayIn - floorDb) / -floorDb, false, Theme::dimText);
        bar (r.withX (r.getX() + w).withWidth (w), displayReduction / 24.0f, true, Theme::error);
        bar (r.withX (r.getX() + 2.0f * w).withWidth (w), (displayOut - floorDb) / -floorDb, false, Theme::layer1);
        g.setColour (Theme::dimText); g.setFont (juce::FontOptions (9.0f));
        const char* names[] { "I", "GR", "O" };
        for (int i = 0; i < 3; ++i) g.drawText (names[i], label.withX (label.getX() + (float) i * w).withWidth (w), juce::Justification::centred);
    }
    void paintHistory (juce::Graphics& g, juce::Rectangle<float> r) {
        g.setColour (Theme::panelLight.withAlpha (0.6f)); g.fillRect (r);
        constexpr float lo = -48.0f;
        const auto y = [&] (float db) { return mapDb (db, r.getY(), r.getBottom(), lo, 0.0f); };
        const auto x = [&] (int i) { return r.getX() + r.getWidth() * (float) i / (float) (historySize - 1); };
        juce::Path in, out, reduction;
        in.startNewSubPath (r.getX(), r.getBottom()); out.startNewSubPath (r.getX(), r.getBottom());
        for (int i = 0; i < historySize; ++i) {
            const auto& f = history[(size_t) ((head + i) % historySize)];
            in.lineTo (x (i), y (f.inDb)); out.lineTo (x (i), y (f.outDb));
            const float gr = r.getY() + r.getHeight() * juce::jlimit (0.0f, 1.0f, f.reductionDb / 24.0f);
            if (i == 0) reduction.startNewSubPath (x (i), gr); else reduction.lineTo (x (i), gr);
        }
        in.lineTo (r.getRight(), r.getBottom()); in.closeSubPath();
        out.lineTo (r.getRight(), r.getBottom()); out.closeSubPath();
        g.setColour (Theme::dimText.withAlpha (0.35f)); g.fillPath (in);
        g.setColour (Theme::layer1.withAlpha (0.55f)); g.fillPath (out);
        g.setColour (Theme::error); g.strokePath (reduction, juce::PathStrokeType (1.5f));
        g.setColour (Theme::text); g.setFont (juce::FontOptions (10.0f));
        g.drawText ("GR " + juce::String (-displayReduction, 1) + " dB", r.reduced (3.0f, 1.0f), juce::Justification::bottomLeft);
    }
    void timerCallback() override {
        const auto reading = processor.getCompressorMeters (index).take();
        const bool active = values[6] != nullptr && (int) std::round (values[6]->load()) == 6;
        const float fall = 26.0f / 30.0f;
        const auto ballistic = [fall] (float& shown, float now) { shown = now > shown ? now : juce::jmax (now, shown - fall); };
        const float in = active ? toDb (reading.inputPeak) : floorDb, out = active ? toDb (reading.outputPeak) : floorDb;
        ballistic (displayIn, in); ballistic (displayOut, out);
        ballistic (displayLevel, active ? juce::jmax (floorDb, reading.levelDb) : floorDb);
        displayReduction = active ? (reading.reductionDb > displayReduction ? reading.reductionDb : juce::jmax (reading.reductionDb, displayReduction - fall)) : 0.0f;
        history[(size_t) head] = { in, out, active ? reading.reductionDb : 0.0f };
        head = (head + 1) % historySize;
        repaint();
    }
    ReverseSamplerProcessor& processor;
    int index = 0;
    std::array<std::atomic<float>*, 7> values {};
    std::array<Frame, historySize> history {};
    int head = 0;
    float displayIn = floorDb, displayOut = floorDb, displayLevel = floorDb, displayReduction = 0.0f;
};
}

class FxRackEditor::Slot final : public juce::Component, public juce::DragAndDropTarget, private juce::Timer
{
    struct Handle final : juce::Component, juce::SettableTooltipClient {
        int index = 0;
        void paint (juce::Graphics& g) override {
            g.setColour (Theme::panelLight); g.fillRoundedRectangle (getLocalBounds().toFloat(), 5.0f);
            g.setColour (Theme::text); g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText ("SLOT " + juce::String(index + 1) + "  :: drag", getLocalBounds(), juce::Justification::centred);
        }
        void mouseDrag (const juce::MouseEvent& e) override {
            if (e.getDistanceFromDragStart() > 5)
                if (auto* c = juce::DragAndDropContainer::findParentDragContainerFor (this))
                    if (! c->isDragAndDropActive()) c->startDragging (index, this);
        }
    } handle;
    using ComboAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
public:
    Slot (ReverseSamplerProcessor& p, int n) : processor (p), index (n), curve (p, n), compressorView (p, n) {
        auto& state = p.getValueTreeState();
        handle.index = n; handle.setTooltip ("Drag this header onto another slot to swap effects"); addAndMakeVisible (handle);
        type.addItemList ({ "Empty", "Delay", "EQ", "Drive", "Reverb", "Chorus", "Compressor" }, 1); addAndMakeVisible (type);
        type.setTooltip ("Select an effect, or Empty to remove it");
        typeValue = state.getRawParameterValue (rs::fxID (n, "type"));
        presets.setTooltip ("Factory presets for this slot's effect (or for any effect while the slot is Empty); one Undo step");
        addAndMakeVisible (presets);
        presets.onClick = [this] { showPresetMenu(); };
        syncType();
        type.onChange = [this] {
            shownTypeId = type.getSelectedId();
            const int t = shownTypeId - 1;
            if (t >= 0) processor.setFxType (index, t);
            updateLabels();
        };
        for (int l = 0; l < 2; ++l) {
            routes[(size_t)l].setButtonText (l == 0 ? "A" : "B"); routes[(size_t)l].setClickingTogglesState (true);
            routes[(size_t)l].setTooltip (l == 0 ? "Process layer 1 (A)" : "Process layer 2 (B)"); addAndMakeVisible (routes[(size_t)l]);
            routeAttachments[(size_t)l] = std::make_unique<ButtonAttachment> (state, rs::fxID(n, l == 0 ? "a" : "b"), routes[(size_t)l]);
        }
        style.addItemList ({ "Reverse", "Tape", "Replica", "Amethyst" }, 1); addAndMakeVisible (style);
        styleAttachment = std::make_unique<ComboAttachment> (state, rs::fxID(n, "delayStyle"), style);
        sync.setButtonText ("SYNC"); sync.setClickingTogglesState (true); sync.setTooltip ("Use the host tempo and a note division; otherwise use milliseconds"); addAndMakeVisible (sync);
        syncAttachment = std::make_unique<ButtonAttachment> (state, rs::fxID(n, "sync"), sync);
        division.addItemList (rs::delayDivisions(), 1); division.setTooltip ("T = triplet, . = dotted; delays over 2 seconds are limited to 2 seconds"); addAndMakeVisible (division);
        divisionAttachment = std::make_unique<ComboAttachment> (state, rs::fxID(n, "division"), division);
        setupKnob (replica); replica.setTextValueSuffix ("");
        replica.textFromValueFunction = [] (double v) { return v < 0.5 ? juce::String ("Vintage") : juce::String ("Modern"); };
        replica.valueFromTextFunction = [] (const juce::String& v) { return v.trim().startsWithIgnoreCase ("v") ? 0.0 : 1.0; };
        replicaAttachment = attachSlider (state, rs::fxID(n, "replica"), replica);
        const char* ids[] { "p1", "p2", "p3", "mix" };
        for (int i = 0; i < 4; ++i) {
            setupKnob (knobs[(size_t)i]);
            auto& label = labels[(size_t)i]; label.setJustificationType (juce::Justification::centred); label.setFont (juce::FontOptions (13.0f)); addAndMakeVisible (label);
            knobAttachments[(size_t)i] = attachSlider (state, rs::fxID(n, ids[i]), knobs[(size_t)i]);
        }
        const char* extras[] { "diffusion", "bias" };
        for (int i=0; i<2; ++i) {
            setupKnob (special[(size_t)i]);
            specialAttachments[(size_t)i] = attachSlider (state, rs::fxID(n, extras[i]), special[(size_t)i]);
        }
        setupKnob (splice); splice.setTextValueSuffix ("");
        spliceAttachment = attachSlider (state, rs::fxID (n, "amSplice"), splice);
        spliceLabel.setText ("Splice", juce::dontSendNotification); spliceLabel.setJustificationType (juce::Justification::centred);
        spliceLabel.setFont (juce::FontOptions (13.0f)); addAndMakeVisible (spliceLabel);
        direction.setClickingTogglesState (true); direction.setTooltip ("Switch between reverse and forward grains"); addAndMakeVisible (direction);
        directionAttachment = std::make_unique<ButtonAttachment> (state, rs::fxID (n, "amReverse"), direction);
        brown.setButtonText ("HI-CUT"); brown.setClickingTogglesState (true); brown.setTooltip ("Roll off the highs on Replica repeats"); addAndMakeVisible (brown);
        brownAttachment = std::make_unique<ButtonAttachment> (state, rs::fxID (n, "replicaBrown"), brown);
        // EQ: Low/Mid/Treble reuse knobs 0..2 (p1..p3); Air and the two cuts are EQ-only.
        eqModeValue = state.getRawParameterValue (rs::fxID (n, "eqMode"));
        for (auto* b : { &vintageButton, &modernButton }) { b->setClickingTogglesState (false); addAndMakeVisible (*b); }
        vintageButton.setTooltip ("Passive-style analog curves: broad boosts with gentle interacting dips");
        modernButton.setTooltip ("Clean, precise EQ: the knob shows the exact gain");
        vintageButton.onClick = [this] { setEqMode (0); };
        modernButton.onClick = [this] { setEqMode (1); };
        addAndMakeVisible (curve);
        const char* eqIds[] { "eqAir", "eqLowCut", "eqHighCut" };
        const char* eqNames[] { "Air", "Low Cut", "High Cut" };
        for (size_t i = 0; i < 3; ++i) {
            setupKnob (eqKnobs[i]); eqKnobs[i].setTextValueSuffix ("");
            eqLabels[i].setText (eqNames[i], juce::dontSendNotification); eqLabels[i].setJustificationType (juce::Justification::centred);
            eqLabels[i].setFont (juce::FontOptions (13.0f)); addAndMakeVisible (eqLabels[i]);
            eqAttachments[i] = attachSlider (state, rs::fxID (n, eqIds[i]), eqKnobs[i]);
        }
        eqKnobs[0].textFromValueFunction = [this] (double v) { return bandText (v); };
        eqKnobs[0].valueFromTextFunction = [this] (const juce::String& t) { return bandValue (t); };
        // Drive: four types on one row.
        driveTypeValue = state.getRawParameterValue (rs::fxID (n, "driveType"));
        const char* driveText[] { "TUBE", "SOFT CLIP", "TAPE", "TRIODE" };
        const char* driveTips[] { "Vintage tube: warm 2nd harmonic, sags and softens the top when pushed",
                                  "Overdrive pedal: silicon diodes in the op-amp feedback, clean signal leaks through",
                                  "Tape machine: magnetic hysteresis, 50 us emphasis, head bump and gap loss",
                                  "Triode preamp stage: asymmetric plate curve, grid conduction and bias shift" };
        for (size_t i = 0; i < driveButtons.size(); ++i) {
            driveButtons[i].setButtonText (driveText[i]); driveButtons[i].setTooltip (driveTips[i]);
            driveButtons[i].setClickingTogglesState (false);
            driveButtons[i].onClick = [this, i] { setDriveType ((int) i); };
            addAndMakeVisible (driveButtons[i]);
        }
        // Reverb: six modes on two rows; Pre-Delay, Low Cut and the per-mode Character knob are reverb-only.
        reverbModeValue = state.getRawParameterValue (rs::fxID (n, "rvMode"));
        const char* reverbText[] { "ROOM", "PLATE", "AMBIENCE", "TERRAIN", "BLOOM", "SHIMMER" };
        const char* reverbTips[] { "Room: dense early reflections of a real room, then a short smooth tail. Character = Early (reflection level)",
                                   "Plate: bright, instantly dense metal plate with a fast build-up. Character = Modulation (shimmer of the plate)",
                                   "Ambience: very short, transparent space that adds depth without an audible tail. Character = Width",
                                   "Terrain: open air - discrete echoes from distant surfaces, darker with distance, over a soft wash. Character = Scatter",
                                   "Bloom: the tail swells up like a pad, then decays slowly. Character = Attack (swell time)",
                                   "Shimmer: a large hall whose tail climbs in octaves. Character = Shimmer (octave amount)" };
        for (size_t i = 0; i < reverbButtons.size(); ++i) {
            reverbButtons[i].setButtonText (reverbText[i]); reverbButtons[i].setTooltip (reverbTips[i]);
            reverbButtons[i].setClickingTogglesState (false);
            reverbButtons[i].onClick = [this, i] { setReverbMode ((int) i); };
            addAndMakeVisible (reverbButtons[i]);
        }
        const char* rvIds[] { "rvPreDelay", "rvLowCut", "rvChar" };
        const char* rvNames[] { "Pre-Delay", "Low Cut", "Character" };
        for (size_t i = 0; i < 3; ++i) {
            setupKnob (rvKnobs[i]);
            rvLabels[i].setText (rvNames[i], juce::dontSendNotification); rvLabels[i].setJustificationType (juce::Justification::centred);
            rvLabels[i].setFont (juce::FontOptions (13.0f)); addAndMakeVisible (rvLabels[i]);
            rvAttachments[i] = attachSlider (state, rs::fxID (n, rvIds[i]), rvKnobs[i]);
        }
        rvKnobs[0].setTextValueSuffix (""); rvKnobs[1].setTextValueSuffix ("");
        rvKnobs[0].setTooltip ("Gap between the dry sound and the reverb");
        rvKnobs[1].setTooltip ("Clean high-pass on the reverb input: keeps the low end out of the tail");
        rvKnobs[0].updateText(); rvKnobs[1].updateText();
        for (int m = 0; m < rs::chorus::numModels; ++m) chorusModel.addItem (rs::chorus::modelNames[m], m + 1);
        chorusModel.setTooltip ("Chorus model: each one has the controls and factory behaviour of its hardware"); addAndMakeVisible (chorusModel);
        chorusModelAttachment = std::make_unique<ComboAttachment> (state, rs::fxID (n, "chModel"), chorusModel);
        chorusModelValue = state.getRawParameterValue (rs::fxID (n, "chModel"));
        chorusModeValue = state.getRawParameterValue (rs::fxID (n, "chMode"));
        for (size_t i = 0; i < chorusModeButtons.size(); ++i) {
            chorusModeButtons[i].setClickingTogglesState (false);
            chorusModeButtons[i].onClick = [this, i] { setChorusMode ((int) i); };
            addAndMakeVisible (chorusModeButtons[i]);
        }
        const char* chorusSwitchIds[] { "chVariant", "chStereo", "chHQ" };
        for (size_t i = 0; i < chorusSwitches.size(); ++i) {
            chorusSwitches[i].setClickingTogglesState (true); addAndMakeVisible (chorusSwitches[i]);
            chorusSwitchAttachments[i] = std::make_unique<ButtonAttachment> (state, rs::fxID (n, chorusSwitchIds[i]), chorusSwitches[i]);
        }
        chorusSwitches[2].setButtonText ("HQ");
        chorusSwitches[2].setTooltip ("Band-limited bucket-brigade clock steps: cleaner analog models with no extra aliasing");
        const char* chorusIds[] { "chDelay", "chFeedback", "chVoices", "chOutput", "chVibRate", "chVibDepth" };
        for (size_t i = 0; i < chorusKnobs.size(); ++i) {
            setupKnob (chorusKnobs[i]); chorusKnobs[i].setTextValueSuffix ("");
            chorusLabels[i].setJustificationType (juce::Justification::centred); chorusLabels[i].setFont (juce::FontOptions (13.0f)); addAndMakeVisible (chorusLabels[i]);
            chorusAttachments[i] = attachSlider (state, rs::fxID (n, chorusIds[i]), chorusKnobs[i]);
            chorusKnobs[i].updateText();
        }
        addChildComponent (compressorView);
        cpModeValue = state.getRawParameterValue (rs::fxID (n, "cpMode"));
        cpTimeValue = state.getRawParameterValue (rs::fxID (n, "cpTime"));
        cpVintage.setTooltip ("Vintage: warm variable-mu and opto glue. RMS detector with a 60 Hz sidechain filter, wide knee, program-dependent release, tube and transformer colour");
        cpModern.setTooltip ("Modern: clean, transparent feed-forward compressor. Peak detector, adjustable knee, exact attack and release, no colour");
        for (auto* b : { &cpVintage, &cpModern }) { b->setClickingTogglesState (false); addAndMakeVisible (*b); }
        cpVintage.onClick = [this] { setCompressorParameter ("cpMode", (float) rs::comp::vintage); };
        cpModern.onClick = [this] { setCompressorParameter ("cpMode", (float) rs::comp::modern); };
        const char* timeTips[] { "Time 1: attack 0.2 ms, release 0.3 s", "Time 2: attack 0.2 ms, release 0.8 s", "Time 3: attack 0.4 ms, release 2 s",
                                 "Time 4: attack 0.4 ms, release 5 s", "Time 5: attack 0.4 ms, release 2 s, up to 10 s after long compression",
                                 "Time 6: attack 0.2 ms, release 0.3 s, up to 10 and 25 s after long compression",
                                 "Opto: attack 10 ms, half the release in 60 ms, the rest in 1-15 s depending on how long it compressed" };
        for (size_t i = 0; i < cpTimeButtons.size(); ++i) {
            cpTimeButtons[i].setButtonText (rs::comp::timeNames[i]); cpTimeButtons[i].setTooltip (timeTips[i]);
            cpTimeButtons[i].setClickingTogglesState (false);
            cpTimeButtons[i].onClick = [this, i] { setCompressorParameter ("cpTime", (float) i); };
            addAndMakeVisible (cpTimeButtons[i]);
        }
        const char* cpIds[] { "cpKnee", "cpAttack", "cpRelease" };
        const char* cpNames[] { "Knee", "Attack", "Release" };
        for (size_t i = 0; i < cpKnobs.size(); ++i) {
            setupKnob (cpKnobs[i]); cpKnobs[i].setTextValueSuffix ("");
            cpLabels[i].setText (cpNames[i], juce::dontSendNotification); cpLabels[i].setJustificationType (juce::Justification::centred);
            cpLabels[i].setFont (juce::FontOptions (13.0f)); addAndMakeVisible (cpLabels[i]);
            cpAttachments[i] = attachSlider (state, rs::fxID (n, cpIds[i]), cpKnobs[i]);
            cpKnobs[i].updateText();
        }
        for (size_t i = 0; i < 3; ++i) { defaultText[i] = knobs[i].textFromValueFunction; defaultParse[i] = knobs[i].valueFromTextFunction; }
        startTimerHz (10); updateLabels();
    }
    void setupKnob (juce::Slider& s) {
        s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag); s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 18);
        s.setTextValueSuffix (" %"); addAndMakeVisible (s);
    }
    void paint (juce::Graphics& g) override {
        g.setColour (Theme::panel); g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (2), 7);
        g.setColour (highlight ? Theme::layer1 : Theme::border); g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (2), 7, highlight ? 3.0f : 1.0f);
        if (type.getSelectedId() == 1) { g.setColour (Theme::dimText.withAlpha (0.7f)); g.setFont (juce::FontOptions (22.0f, juce::Font::bold)); g.drawText ("EMPTY", getLocalBounds().withTrimmedTop (120), juce::Justification::centred); }
    }
    void resized() override {
        auto area = getLocalBounds().reduced (9); handle.setBounds (area.removeFromTop (27)); area.removeFromTop (6);
        auto header = area.removeFromTop (28); presets.setBounds (header.removeFromRight (juce::jmin (84, header.getWidth() / 3))); header.removeFromRight (4); type.setBounds (header); area.removeFromTop (5);
        auto row = area.removeFromTop (26); routes[0].setBounds (row.removeFromLeft (row.getWidth()/2).reduced (2, 0)); routes[1].setBounds (row.reduced (2, 0));
        area.removeFromTop (4);
        if (type.getSelectedId() == 3) {
            auto modeRow = area.removeFromTop (25);
            vintageButton.setBounds (modeRow.removeFromLeft (modeRow.getWidth() / 2).reduced (2, 0)); modernButton.setBounds (modeRow.reduced (2, 0));
            area.removeFromTop (6);
            curve.setBounds (area.removeFromTop (juce::jlimit (60, 120, area.getHeight() / 4)));
            area.removeFromTop (6);
            // Staggered pairs as in the sketch: AIR | TREBLE, MID | LOW, LOW CUT | HIGH CUT.
            juce::Slider* grid[3][2] { { &eqKnobs[0], &knobs[2] }, { &knobs[1], &knobs[0] }, { &eqKnobs[1], &eqKnobs[2] } };
            juce::Label* captions[3][2] { { &eqLabels[0], &labels[2] }, { &labels[1], &labels[0] }, { &eqLabels[1], &eqLabels[2] } };
            const int rowHeight = area.getHeight() / 3, stagger = rowHeight / 5;
            for (int r = 0; r < 3; ++r) {
                auto rowArea = r == 2 ? area : area.removeFromTop (rowHeight);
                for (int c = 0; c < 2; ++c) {
                    auto cell = c == 0 ? rowArea.removeFromLeft (rowArea.getWidth() / 2) : rowArea;
                    if (r < 2) cell = c == 0 ? cell.withTrimmedBottom (stagger) : cell.withTrimmedTop (stagger);
                    captions[r][c]->setBounds (cell.removeFromTop (18));
                    grid[r][c]->setBounds (cell.withSizeKeepingCentre (juce::jmin (90, cell.getWidth()), cell.getHeight()));
                }
            }
            return;
        }
        if (type.getSelectedId() == 5) {
            for (int r = 0; r < 2; ++r) {
                auto modeRow = area.removeFromTop (24);
                const int w = modeRow.getWidth() / 3;
                for (int c = 0; c < 3; ++c)
                    reverbButtons[(size_t) (r * 3 + c)].setBounds ((c == 2 ? modeRow : modeRow.removeFromLeft (w)).reduced (2, 1));
            }
            area.removeFromTop (6);
            // SIZE | DECAY, TONE | CHARACTER, PRE-DELAY | LOW CUT, MIX.
            juce::Slider* grid[4][2] { { &knobs[0], &knobs[1] }, { &knobs[2], &rvKnobs[2] }, { &rvKnobs[0], &rvKnobs[1] }, { &knobs[3], nullptr } };
            juce::Label* captions[4][2] { { &labels[0], &labels[1] }, { &labels[2], &rvLabels[2] }, { &rvLabels[0], &rvLabels[1] }, { &labels[3], nullptr } };
            const int rowHeight = area.getHeight() / 4;
            for (int r = 0; r < 4; ++r) {
                auto rowArea = r == 3 ? area : area.removeFromTop (rowHeight);
                for (int c = 0; c < 2; ++c) {
                    if (grid[r][c] == nullptr) continue;
                    auto cell = grid[r][1] == nullptr ? rowArea : c == 0 ? rowArea.removeFromLeft (rowArea.getWidth() / 2) : rowArea;
                    captions[r][c]->setBounds (cell.removeFromTop (18));
                    grid[r][c]->setBounds (cell.withSizeKeepingCentre (juce::jmin (90, cell.getWidth()), cell.getHeight()));
                }
            }
            return;
        }
        if (type.getSelectedId() == 6) { layoutChorus (area); return; }
        if (type.getSelectedId() == 7) { layoutCompressor (area); return; }
        if (type.getSelectedId() == 2) { style.setBounds (area.removeFromTop (25)); area.removeFromTop (3); sync.setBounds (area.removeFromTop (23)); area.removeFromTop (3); }
        const bool amethyst = type.getSelectedId() == 2 && style.getSelectedId() == 4;
        if (amethyst) {
            direction.setBounds (area.removeFromTop (25)); area.removeFromTop (5);
            const int rowHeight = area.getHeight() / 4;
            const int rows[4] { 0, 1, 3, 4 };
            for (int r = 0; r < 4; ++r) {
                auto cell = area.removeFromTop (rowHeight);
                if (rows[r] == 4) {
                    spliceLabel.setBounds (cell.removeFromTop (18));
                    splice.setBounds (cell);
                } else {
                    const int i = rows[r];
                    labels[(size_t)i].setBounds (cell.removeFromTop (18));
                    knobs[(size_t)i].setBounds (cell);
                }
            }
            return;
        }
        if (type.getSelectedId() == 2 && style.getSelectedId() == 3) { brown.setBounds (area.removeFromTop (23)); area.removeFromTop (3); }
        if (type.getSelectedId() == 4) {
            auto typeRow = area.removeFromTop (25);
            const int w = typeRow.getWidth() / (int) driveButtons.size();
            for (size_t i = 0; i < driveButtons.size(); ++i)
                driveButtons[i].setBounds ((i + 1 == driveButtons.size() ? typeRow : typeRow.removeFromLeft (w)).reduced (2, 0));
            area.removeFromTop (4);
        }
        const int height = area.getHeight() / 4;
        for (int i = 0; i < 4; ++i) {
            auto cell = i == 3 ? area : area.removeFromTop (height);
            labels[(size_t)i].setBounds (cell.removeFromTop (18));
            knobs[(size_t)i].setBounds (cell.withSizeKeepingCentre (juce::jmin (90, cell.getWidth()), cell.getHeight()));
            if (i == 0) division.setBounds (cell.withSizeKeepingCentre (juce::jmin (130, cell.getWidth()), 26));
            if (i == 2) {
                for (auto& slider : special) slider.setBounds (cell.withSizeKeepingCentre (juce::jmin (90, cell.getWidth()), cell.getHeight()));
                replica.setBounds (cell.withSizeKeepingCentre (juce::jmin (90, cell.getWidth()), cell.getHeight()));
            }
        }
    }
    bool isInterestedInDragSource (const SourceDetails& d) override { return d.description.isInt() && (int)d.description != index && juce::isPositiveAndBelow ((int)d.description, rs::fxSlots); }
    void itemDragEnter (const SourceDetails&) override { highlight = true; repaint(); }
    void itemDragExit (const SourceDetails&) override { highlight = false; repaint(); }
    void itemDropped (const SourceDetails& d) override { highlight = false; repaint(); const int from = (int)d.description; if (juce::isPositiveAndBelow (from, rs::fxSlots) && from != index) processor.swapFxSlots (from, index); }
private:
    void timerCallback() override { syncType(); updateLabels(); }
    // Presets of the slot's effect, grouped by genre. An Empty slot lists every effect's presets.
    void showPresetMenu() {
        const int current = typeValue == nullptr ? 0 : juce::jlimit (0, rs::numFxTypes - 1, (int) std::round (typeValue->load()));
        const auto genreMenu = [] (int fxType) {
            juce::PopupMenu byGenre;
            juce::StringArray genres;
            for (const auto& preset : rs::fxPresets)
                if (preset.type == fxType) genres.addIfNotAlreadyThere (preset.genre);
            for (const auto& genre : genres) {
                juce::PopupMenu group;
                for (size_t k = 0; k < rs::numFxPresets; ++k)
                    if (rs::fxPresets[k].type == fxType && genre == rs::fxPresets[k].genre) group.addItem ((int) k + 1, rs::fxPresets[k].name);
                byGenre.addSubMenu (genre, group);
            }
            return byGenre;
        };
        juce::PopupMenu menu;
        if (current == 0)
            for (int t = 1; t < rs::numFxTypes; ++t) menu.addSubMenu (rs::fxTypeNames[t], genreMenu (t));
        else menu = genreMenu (current);
        juce::Component::SafePointer<Slot> safe (this);
        menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&presets).withMinimumWidth (presets.getWidth()), [safe] (int result) {
            if (safe == nullptr || result <= 0 || (size_t) result > rs::numFxPresets) return;
            safe->processor.applyFxPreset (safe->index, rs::fxPresets[(size_t) result - 1]);
            safe->shownTypeId = 1 + rs::fxPresets[(size_t) result - 1].type;
            safe->type.setSelectedId (safe->shownTypeId, juce::dontSendNotification);
            safe->updateLabels();
        });
    }
    void syncType() {
        const int id = 1 + (typeValue == nullptr ? 0 : juce::jlimit (0, rs::numFxTypes - 1, (int) std::round (typeValue->load())));
        if (type.isPopupActive() || type.getSelectedId() != shownTypeId) return;
        if (type.getSelectedId() != id) type.setSelectedId (id, juce::dontSendNotification);
        shownTypeId = id;
    }
    bool eqVintage() const { return eqModeValue == nullptr || eqModeValue->load() < 0.5f; }
    // Vintage shows a Pultec-like 0..10 scale (signed, since the knobs also cut); Modern shows exact dB.
    juce::String bandText (double v) const {
        const double db = rs::eqKnobToDb ((float) v);
        if (std::abs (db) < 0.005) return lastEqVintage ? "0" : "0.0 dB";
        if (lastEqVintage) return (db > 0 ? "+" : "") + juce::String (db / 1.2, 1);
        return (db > 0 ? "+" : "") + juce::String (db, 1) + " dB";
    }
    double bandValue (const juce::String& t) const {
        const double x = t.retainCharacters ("0123456789.,-+").replaceCharacter (',', '.').getDoubleValue();
        return juce::jlimit (0.0, 100.0, 50.0 + (lastEqVintage ? x * 1.2 : x) / 0.24);
    }
    int reverbMode() const { return reverbModeValue == nullptr ? 0 : juce::jlimit (0, rs::reverb::numModes - 1, (int) std::round (reverbModeValue->load())); }
    void setReverbMode (int m) {
        if (auto* param = processor.getValueTreeState().getParameter (rs::fxID (index, "rvMode"))) {
            param->beginChangeGesture(); param->setValueNotifyingHost (param->convertTo0to1 ((float) m)); param->endChangeGesture();
        }
        updateLabels();
    }
    static juce::String secondsText (double s) { return s < 1.0 ? juce::String (s * 1000.0, 0) + " ms" : juce::String (s, s < 10.0 ? 2 : 1) + " s"; }
    // Decay knob shows the mid-band RT60 of the current mode; typed seconds are inverted numerically.
    double decayValue (const juce::String& text) const {
        double x = text.retainCharacters ("0123456789.,").replaceCharacter (',', '.').getDoubleValue();
        if (text.containsIgnoreCase ("ms")) x *= 0.001;
        const int m = reverbMode(); double lo = 0.0, hi = 1.0;
        for (int k = 0; k < 40; ++k) { const double mid = 0.5 * (lo + hi); (rs::reverb::decaySeconds (m, mid) < x ? lo : hi) = mid; }
        return juce::jlimit (0.0, 100.0, 50.0 * (lo + hi));
    }
    int driveType() const { return driveTypeValue == nullptr ? 0 : juce::jlimit (0, 3, (int) std::round (driveTypeValue->load())); }
    void setDriveType (int t) {
        if (auto* param = processor.getValueTreeState().getParameter (rs::fxID (index, "driveType"))) {
            param->beginChangeGesture(); param->setValueNotifyingHost (param->convertTo0to1 ((float) t)); param->endChangeGesture();
        }
        updateLabels();
    }
    int chorusModelIndex() const { return chorusModelValue == nullptr ? 0 : juce::jlimit (0, rs::chorus::numModels - 1, (int) std::round (chorusModelValue->load())); }
    int chorusModeIndex() const {
        const int raw = chorusModeValue == nullptr ? 0 : (int) std::round (chorusModeValue->load());
        return juce::jlimit (0, rs::chorus::modeCounts[chorusModelIndex()] - 1, raw);
    }
    void setChorusMode (int m) {
        if (auto* param = processor.getValueTreeState().getParameter (rs::fxID (index, "chMode"))) {
            param->beginChangeGesture(); param->setValueNotifyingHost (param->convertTo0to1 ((float) m)); param->endChangeGesture();
        }
        updateLabels();
    }
    static juce::String hertzText (double hz) { return hz < 10.0 ? juce::String (hz, 2) + " Hz" : juce::String (hz, 1) + " Hz"; }
    void updateChorus (bool visible) {
        using namespace rs::chorus;
        const int model = chorusModelIndex(), m = chorusModeIndex();
        static const char* const modeText[numModels][7] {
            { "CHORUS", "VIBRATO" }, { "1", "2", "3", "4", "1+4", "2+4", "3+4" }, { "NORMAL", "DEEP" },
            { "PITCH", "MOD", "MULTI" }, { "I", "II", "I+II" }, { "" }, { "3 LINES", "6 LINES" } };
        static const char* const modeTips[numModels] {
            "Chorus: triangle LFO, Intensity sets speed and depth together. Vibrato: sine LFO, wet only (Mix 100 %)",
            "Fixed presets: wide, steady stereo space with almost no pitch wobble. 4 alone sounds like 3+4",
            "Depth switch: Normal is subtle, Deep is a thick, watery sweep",
            "Pitch: steady detune, -9 / +11 cents at Detune 50 %, no wobble. Mod: modulated delay with pan and level motion. Multi: 2-8 independent voices",
            "Synth chorus buttons: I 0.51 Hz, II 0.86 Hz, I+II 9.75 Hz mono vibrato",
            "",
            "String ensemble: three delay lines on a three-phase LFO, or six lines for stereo" };
        const int modes = modeCounts[model];
        for (int i = 0; i < (int) chorusModeButtons.size(); ++i) {
            auto& b = chorusModeButtons[(size_t) i];
            b.setVisible (visible && modes > 1 && i < modes);
            b.setButtonText (i < modes ? modeText[model][i] : "");
            b.setTooltip (modeTips[model]);
        }
        chorusModel.setVisible (visible);
        chorusSwitches[0].setVisible (visible && (model == classic || model == liquid));
        chorusSwitches[0].setButtonText (model == classic ? "HIGH" : "DARK");
        chorusSwitches[0].setTooltip (model == classic ? "Input switch: High adds a transistor gain stage, the preamp colours the sound even at Mix 0"
                                                   : "Dark: longer delays and a darker tone");
        chorusSwitches[1].setVisible (visible && model == classic);
        chorusSwitches[1].setTooltip ("Classic: left dry, right wet. Dual: two lines with LFOs 90 degrees apart");
        chorusSwitches[2].setVisible (visible && model != digital && model != sine);
        for (auto& k : chorusKnobs) k.setVisible (false);
        for (auto& l : chorusLabels) l.setVisible (false);
        chorusGrid.clear();
        if (! visible) { if (type.getSelectedId() != 7) knobs[1].setTooltip ({}); return; }
        const auto knob = [this] (juce::Slider& s, juce::Label& l, const juce::String& caption) {
            l.setText (caption, juce::dontSendNotification); s.setVisible (true); l.setVisible (true); chorusGrid.push_back ({ &s, &l });
        };
        const auto rate = [this, &knob] (const juce::String& caption, std::function<double (double)> hz) {
            knobs[0].setTextValueSuffix ("");
            knobs[0].textFromValueFunction = [hz] (double v) { return hertzText (hz (v * 0.01)); };
            knobs[0].valueFromTextFunction = {};
            knob (knobs[0], labels[0], caption);
        };
        const auto depth = [this, &knob] (const juce::String& caption) {
            knobs[1].textFromValueFunction = defaultText[1]; knobs[1].valueFromTextFunction = defaultParse[1]; knobs[1].setTextValueSuffix (" %");
            knobs[1].setTooltip ({});
            knob (knobs[1], labels[1], caption);
        };
        switch (model) {
            case classic:
                if (m == classicVibrato) { rate ("Rate", [] (double v) { return classicVibratoHz ((float) v); }); depth ("Depth"); }
                else rate ("Intensity", [] (double v) { return classicChorusHz ((float) v); });
                break;
            case liquid: rate ("Rate", [] (double v) { return liquidHz ((float) v); }); break;
            case digital:
                if (m == pitchShift) {
                    knobs[1].setTextValueSuffix ("");
                    knobs[1].textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (detuneCents ((float) (v * 0.01), 0))) + "/+" + juce::String (juce::roundToInt (detuneCents ((float) (v * 0.01), 1))) + " ct"; };
                    knobs[1].valueFromTextFunction = {};
                    knob (knobs[1], labels[1], "Detune");
                } else { rate ("Rate", [] (double v) { return digitalHz ((float) v); }); depth ("Depth"); }
                knob (chorusKnobs[0], chorusLabels[0], "Delay");
                knob (chorusKnobs[1], chorusLabels[1], "Feedback");
                if (m == multiVoice) knob (chorusKnobs[2], chorusLabels[2], "Voices");
                break;
            case sine:
                rate ("Rate", [] (double v) { return digitalHz ((float) v); }); depth ("Depth");
                knob (chorusKnobs[0], chorusLabels[0], "Delay");
                knob (chorusKnobs[1], chorusLabels[1], "Feedback");
                break;
            case ensemble:
                rate ("Chorus Rate", [] (double v) { return ensembleChorusHz ((float) v); }); depth ("Chorus Depth");
                knob (chorusKnobs[4], chorusLabels[4], "Vibrato Rate");
                chorusKnobs[4].textFromValueFunction = [] (double v) { return hertzText (ensembleVibratoHz ((float) (v * 0.01))); };
                knob (chorusKnobs[5], chorusLabels[5], "Vibrato Depth");
                break;
            default: break;
        }
        knob (knobs[3], labels[3], "Mix");
        knob (knobs[2], labels[2], "Width");
        knob (chorusKnobs[3], chorusLabels[3], "Output");
        for (auto& k : knobs) k.updateText();
        for (auto& k : chorusKnobs) k.updateText();
        for (int i = 0; i < 4; ++i) {
            bool used = false;
            for (const auto& cell : chorusGrid) used = used || cell.first == &knobs[(size_t) i];
            knobs[(size_t) i].setVisible (used); labels[(size_t) i].setVisible (used);
        }
    }
    void layoutChorus (juce::Rectangle<int> area) {
        chorusModel.setBounds (area.removeFromTop (25)); area.removeFromTop (4);
        int modes = 0;
        for (auto& b : chorusModeButtons) if (b.isVisible()) ++modes;
        for (int start = 0; start < modes; start += 4) {
            auto row = area.removeFromTop (24);
            const int count = std::min (4, modes - start), w = row.getWidth() / count;
            for (int c = 0; c < count; ++c) chorusModeButtons[(size_t) (start + c)].setBounds ((c == count - 1 ? row : row.removeFromLeft (w)).reduced (2, 1));
        }
        if (modes > 0) area.removeFromTop (4);
        std::vector<juce::TextButton*> switches;
        for (auto& b : chorusSwitches) if (b.isVisible()) switches.push_back (&b);
        if (! switches.empty()) {
            auto row = area.removeFromTop (23);
            const int w = row.getWidth() / (int) switches.size();
            for (size_t c = 0; c < switches.size(); ++c) switches[c]->setBounds ((c + 1 == switches.size() ? row : row.removeFromLeft (w)).reduced (2, 0));
            area.removeFromTop (6);
        }
        const int rows = juce::jmax (1, ((int) chorusGrid.size() + 1) / 2), rowHeight = area.getHeight() / rows;
        for (int r = 0; r < rows; ++r) {
            auto rowArea = r == rows - 1 ? area : area.removeFromTop (rowHeight);
            const auto first = (size_t) (2 * r);
            const bool pair = first + 1 < chorusGrid.size();
            for (size_t c = 0; c < (pair ? 2u : 1u); ++c) {
                auto cell = ! pair ? rowArea : c == 0 ? rowArea.removeFromLeft (rowArea.getWidth() / 2) : rowArea;
                chorusGrid[first + c].second->setBounds (cell.removeFromTop (18));
                chorusGrid[first + c].first->setBounds (cell.withSizeKeepingCentre (juce::jmin (90, cell.getWidth()), cell.getHeight()));
            }
        }
    }
    int compressorMode() const { return cpModeValue == nullptr ? 0 : juce::jlimit (0, rs::comp::numModes - 1, (int) std::round (cpModeValue->load())); }
    int compressorTime() const { return cpTimeValue == nullptr ? 1 : juce::jlimit (0, rs::comp::numTimes - 1, (int) std::round (cpTimeValue->load())); }
    void setCompressorParameter (const char* id, float value) {
        if (auto* param = processor.getValueTreeState().getParameter (rs::fxID (index, id))) {
            param->beginChangeGesture(); param->setValueNotifyingHost (param->convertTo0to1 (value)); param->endChangeGesture();
        }
        updateLabels();
    }
    static double parseNumber (const juce::String& s) { return s.retainCharacters ("0123456789.,-+").replaceCharacter (',', '.').getDoubleValue(); }
    void updateCompressor (bool visible) {
        const bool vintageMode = compressorMode() == rs::comp::vintage;
        compressorView.setVisible (visible);
        cpVintage.setVisible (visible); cpModern.setVisible (visible);
        for (auto& b : cpTimeButtons) b.setVisible (visible && vintageMode);
        for (size_t i = 0; i < cpKnobs.size(); ++i) { cpKnobs[i].setVisible (visible && ! vintageMode); cpLabels[i].setVisible (visible && ! vintageMode); }
        if (! visible) return;
        knobs[0].setTextValueSuffix ("");
        knobs[0].textFromValueFunction = [] (double v) { return juce::String (rs::comp::thresholdFromKnob ((float) v), 1) + " dB"; };
        knobs[0].valueFromTextFunction = [] (const juce::String& s) { return juce::jlimit (0.0, 100.0, (parseNumber (s) - rs::comp::minThresholdDb) / (rs::comp::maxThresholdDb - rs::comp::minThresholdDb) * 100.0); };
        knobs[1].setTextValueSuffix ("");
        knobs[1].textFromValueFunction = [] (double v) { const float r = rs::comp::ratioFromKnob ((float) v); return juce::String (r, r < 10.0f ? 1 : 0) + ":1"; };
        knobs[1].valueFromTextFunction = [] (const juce::String& s) { return (double) rs::comp::ratioToKnob ((float) parseNumber (s.upToFirstOccurrenceOf (":", false, false))); };
        knobs[2].setTextValueSuffix ("");
        knobs[2].textFromValueFunction = [] (double v) { return "+" + juce::String (rs::comp::makeupFromKnob ((float) v), 1) + " dB"; };
        knobs[2].valueFromTextFunction = [] (const juce::String& s) { return juce::jlimit (0.0, 100.0, parseNumber (s) / rs::comp::maxMakeupDb * 100.0); };
        knobs[3].setTextValueSuffix (" %");
        knobs[1].setTooltip (vintageMode ? "Ratio reached above the wide vintage knee: gentle at the threshold, firmer as the level rises" : "");
        for (int i = 0; i < 4; ++i) { knobs[(size_t) i].setVisible (true); labels[(size_t) i].setVisible (true); knobs[(size_t) i].updateText(); }
        for (auto& k : cpKnobs) k.updateText();
    }
    void layoutCompressor (juce::Rectangle<int> area) {
        auto modeRow = area.removeFromTop (25);
        cpVintage.setBounds (modeRow.removeFromLeft (modeRow.getWidth() / 2).reduced (2, 0)); cpModern.setBounds (modeRow.reduced (2, 0));
        area.removeFromTop (6);
        compressorView.setBounds (area.removeFromTop (juce::jlimit (150, 260, area.getHeight() * 2 / 5)));
        area.removeFromTop (6);
        std::vector<std::pair<juce::Slider*, juce::Label*>> grid { { &knobs[0], &labels[0] }, { &knobs[1], &labels[1] } };
        if (compressorMode() == rs::comp::vintage) {
            for (int start = 0; start < (int) cpTimeButtons.size(); start += 4) {
                auto row = area.removeFromTop (24);
                const int count = juce::jmin (4, (int) cpTimeButtons.size() - start), w = row.getWidth() / 4;
                for (int c = 0; c < count; ++c) cpTimeButtons[(size_t) (start + c)].setBounds ((count == 4 && c == 3 ? row : row.removeFromLeft (count == 3 && c == 2 ? 2 * w : w)).reduced (2, 1));
            }
            area.removeFromTop (6);
        } else {
            grid.push_back ({ &cpKnobs[0], &cpLabels[0] }); grid.push_back ({ &cpKnobs[1], &cpLabels[1] }); grid.push_back ({ &cpKnobs[2], &cpLabels[2] });
        }
        grid.push_back ({ &knobs[2], &labels[2] }); grid.push_back ({ &knobs[3], &labels[3] });
        const int columns = compressorMode() == rs::comp::vintage ? 2 : 3;
        const int rows = ((int) grid.size() + columns - 1) / columns, rowHeight = area.getHeight() / rows;
        for (int r = 0; r < rows; ++r) {
            auto rowArea = r == rows - 1 ? area : area.removeFromTop (rowHeight);
            const int first = columns * r, count = juce::jmin (columns, (int) grid.size() - first);
            const int width = rowArea.getWidth() / columns;
            rowArea = rowArea.withSizeKeepingCentre (width * count, rowArea.getHeight());
            for (int c = 0; c < count; ++c) {
                auto cell = c == count - 1 ? rowArea : rowArea.removeFromLeft (width);
                grid[(size_t) (first + c)].second->setBounds (cell.removeFromTop (18));
                grid[(size_t) (first + c)].first->setBounds (cell.withSizeKeepingCentre (juce::jmin (90, cell.getWidth()), cell.getHeight()));
            }
        }
    }
    void setEqMode (int mode) {
        if (auto* param = processor.getValueTreeState().getParameter (rs::fxID (index, "eqMode"))) {
            param->beginChangeGesture(); param->setValueNotifyingHost (param->convertTo0to1 ((float) mode)); param->endChangeGesture();
        }
        updateLabels();
    }
    void updateLabels() {
        const int t = juce::jlimit (0, rs::numFxTypes - 1, type.getSelectedId()-1), mode = juce::jlimit (0, 3, style.getSelectedId()-1);
        const bool syncOn = sync.getToggleState();
        const int div = juce::jlimit (0, (int) std::size (rs::divisionBeats) - 1, division.getSelectedId() - 1);
        const double tempo = processor.getCurrentBpm();
        const bool limited = t == 1 && syncOn && tempo > 1.0 && 60.0 / tempo * rs::divisionBeats[div] > rs::maxDelaySeconds;
        direction.setButtonText (direction.getToggleState() ? "REVERSE" : "FORWARD");
        const bool vintageEq = eqVintage();
        vintageButton.setToggleState (vintageEq, juce::dontSendNotification); modernButton.setToggleState (! vintageEq, juce::dontSendNotification);
        const int currentDrive = driveType();
        for (size_t i = 0; i < driveButtons.size(); ++i) driveButtons[i].setToggleState ((int) i == currentDrive, juce::dontSendNotification);
        const int rvMode = reverbMode();
        for (size_t i = 0; i < reverbButtons.size(); ++i) reverbButtons[i].setToggleState ((int) i == rvMode, juce::dontSendNotification);
        const int chModelNow = chorusModelIndex(), chModeNow = chorusModeIndex();
        for (size_t i = 0; i < chorusModeButtons.size(); ++i) chorusModeButtons[i].setToggleState ((int) i == chModeNow, juce::dontSendNotification);
        chorusSwitches[1].setButtonText (chorusSwitches[1].getToggleState() ? "DUAL" : "CLASSIC");
        const int cpModeNow = compressorMode(), cpTimeNow = compressorTime();
        cpVintage.setToggleState (cpModeNow == rs::comp::vintage, juce::dontSendNotification); cpModern.setToggleState (cpModeNow == rs::comp::modern, juce::dontSendNotification);
        for (size_t i = 0; i < cpTimeButtons.size(); ++i) cpTimeButtons[i].setToggleState ((int) i == cpTimeNow, juce::dontSendNotification);
        if (t == lastType && mode == lastStyle && syncOn == lastSync && limited == lastLimited && vintageEq == lastEqVintage && rvMode == lastReverbMode && chModelNow == lastChorusModel && chModeNow == lastChorusMode && cpModeNow == lastCompMode && cpTimeNow == lastCompTime) return;
        lastChorusModel = chModelNow; lastChorusMode = chModeNow; lastCompMode = cpModeNow; lastCompTime = cpTimeNow;
        lastType = t; lastStyle = mode; lastSync = syncOn; lastLimited = limited; lastEqVintage = vintageEq; lastReverbMode = rvMode;
        const auto& names = rs::fxKnobNames;
        const char* extraNames[] { "Diffusion", "Bias", "Character", "" };
        for (int i=0; i<4; ++i) {
            const char* caption = t == 1 && mode == 3 ? (i == 0 ? "Delay Time" : i == 1 ? "Feedback" : i == 3 ? "Mix" : "")
                                : t == 1 && i == 2 ? extraNames[mode] : names[t][i];
            labels[(size_t)i].setText (t == 1 && i == 0 && limited ? "Time (limited: 2 s)" : caption, juce::dontSendNotification);
            labels[(size_t)i].setColour (juce::Label::textColourId, t == 1 && i == 0 && limited ? Theme::error : Theme::text);
            labels[(size_t)i].setVisible (t != 0 && !(t == 1 && mode == 3 && i == 2));
            knobs[(size_t)i].setVisible (t != 0 && !(t == 1 && (i == 2 || (i == 0 && syncOn))) && !(t == 2 && i == 3));
            if (t == 2 && i == 3) labels[(size_t)i].setVisible (false);   // EQ is always fully wet
        }
        spliceLabel.setText ("Splice / Grain (ms)", juce::dontSendNotification);
        splice.setTextValueSuffix ("");
        splice.textFromValueFunction = [] (double v) { return juce::String (v, 0) + " ms"; };
        splice.valueFromTextFunction = [] (const juce::String& s) { return juce::jlimit (150.0, 700.0, s.getDoubleValue()); };
        splice.updateText();
        // p1 keeps its original 0..100 host automation values but shows ms in Delay.
        if (t == 1) {
            knobs[0].setTextValueSuffix ("");
            knobs[0].textFromValueFunction = [] (double v) { return juce::String (40.0 + v * 9.6, 0) + " ms"; };
            knobs[0].valueFromTextFunction = [] (const juce::String& s) { return juce::jlimit (0.0, 100.0, (s.getDoubleValue()-40.0)/9.6); };
        } else if (t == 3) {   // Drive 0..100 % = 0..+36 dB of gain into the curve
            knobs[0].setTextValueSuffix ("");
            knobs[0].textFromValueFunction = [] (double v) { return "+" + juce::String (v * rs::drive::maxDriveDb / 100.0, 1) + " dB"; };
            knobs[0].valueFromTextFunction = [] (const juce::String& s) { return juce::jlimit (0.0, 100.0, s.retainCharacters ("0123456789.,").replaceCharacter (',', '.').getDoubleValue() * 100.0 / rs::drive::maxDriveDb); };
        } else { knobs[0].textFromValueFunction = {}; knobs[0].valueFromTextFunction = {}; knobs[0].setTextValueSuffix (" %"); }
        for (size_t i = 0; i < 3; ++i) {
            if (t == 2) {
                knobs[i].setTextValueSuffix ("");
                knobs[i].textFromValueFunction = [this] (double v) { return bandText (v); };
                knobs[i].valueFromTextFunction = [this] (const juce::String& s) { return bandValue (s); };
            } else if (t == 3 && i == 2) {   // Output: 50 = 0 dB, 100 = +6 dB
                knobs[i].setTextValueSuffix ("");
                knobs[i].textFromValueFunction = [] (double v) {
                    if (v <= 0.0) return juce::String ("-inf dB");
                    const double db = 20.0 * std::log10 (v / 50.0);
                    return (db > 0.005 ? "+" : "") + juce::String (db, 1) + " dB";
                };
                knobs[i].valueFromTextFunction = [] (const juce::String& s) {
                    if (s.containsIgnoreCase ("inf")) return 0.0;
                    const double db = s.retainCharacters ("0123456789.,-+").replaceCharacter (',', '.').getDoubleValue();
                    return juce::jlimit (0.0, 100.0, 50.0 * std::pow (10.0, db / 20.0));
                };
            } else if (t == 4 && i == 1) {   // Decay in seconds of the current mode
                knobs[i].setTextValueSuffix ("");
                knobs[i].textFromValueFunction = [this] (double v) { return secondsText (rs::reverb::decaySeconds (reverbMode(), v * 0.01)); };
                knobs[i].valueFromTextFunction = [this] (const juce::String& s) { return decayValue (s); };
            } else if (i > 0) {
                knobs[i].textFromValueFunction = defaultText[i]; knobs[i].valueFromTextFunction = defaultParse[i]; knobs[i].setTextValueSuffix (" %");
            }
            knobs[i].updateText();
        }
        for (auto& k : eqKnobs) { k.setVisible (t == 2); k.updateText(); }
        for (auto& l : eqLabels) l.setVisible (t == 2);
        vintageButton.setVisible (t == 2); modernButton.setVisible (t == 2); curve.setVisible (t == 2);
        for (auto& b : driveButtons) b.setVisible (t == 3);
        for (auto& b : reverbButtons) b.setVisible (t == 4);
        rvLabels[2].setText (rs::reverb::characterName (rvMode), juce::dontSendNotification);
        for (auto& k : rvKnobs) { k.setVisible (t == 4); k.updateText(); }
        for (auto& l : rvLabels) l.setVisible (t == 4);
        style.setVisible (t == 1); sync.setVisible (t == 1); division.setVisible (t == 1 && syncOn);
        replica.setVisible (t == 1 && mode == 2); brown.setVisible (t == 1 && mode == 2);
        splice.setVisible (t == 1 && mode == 3); spliceLabel.setVisible (t == 1 && mode == 3); direction.setVisible (t == 1 && mode == 3);
        special[0].setVisible (t == 1 && mode == 0); special[1].setVisible (t == 1 && mode == 1);
        updateChorus (t == 5);
        updateCompressor (t == 6);
        for (auto& button : routes) button.setEnabled (t != 0);
        resized(); repaint();
    }
    ReverseSamplerProcessor& processor; int index, lastType = -1, lastStyle = -1; bool lastSync = false, lastLimited = false, highlight = false;
    juce::ComboBox type, style, division; juce::Slider replica, splice; juce::Label spliceLabel; juce::TextButton sync, direction, brown; std::array<juce::TextButton,2> routes;
    juce::TextButton presets { "PRESETS" };
    std::array<juce::Slider,4> knobs; std::array<juce::Slider,2> special; std::array<juce::Label,4> labels;
    std::unique_ptr<ComboAttachment> typeAttachment, styleAttachment, divisionAttachment;
    std::unique_ptr<SliderAttachment> replicaAttachment, spliceAttachment;
    std::unique_ptr<ButtonAttachment> syncAttachment, directionAttachment, brownAttachment;
    std::array<std::unique_ptr<ButtonAttachment>,2> routeAttachments;
    std::array<std::unique_ptr<SliderAttachment>,4> knobAttachments;
    std::array<std::unique_ptr<SliderAttachment>,2> specialAttachments;
    EqCurve curve;
    juce::TextButton vintageButton { "VINTAGE" }, modernButton { "MODERN" };
    std::array<juce::Slider,3> eqKnobs; std::array<juce::Label,3> eqLabels;
    std::array<std::unique_ptr<SliderAttachment>,3> eqAttachments;
    std::atomic<float>* eqModeValue = nullptr; bool lastEqVintage = true;
    std::atomic<float>* typeValue = nullptr;
    std::array<juce::TextButton, 4> driveButtons; std::atomic<float>* driveTypeValue = nullptr;
    std::array<juce::TextButton, 6> reverbButtons; std::atomic<float>* reverbModeValue = nullptr; int lastReverbMode = -1;
    std::array<juce::Slider,3> rvKnobs; std::array<juce::Label,3> rvLabels;
    std::array<std::unique_ptr<SliderAttachment>,3> rvAttachments;
    std::array<std::function<juce::String (double)>,3> defaultText;
    std::array<std::function<double (const juce::String&)>,3> defaultParse;
    juce::ComboBox chorusModel; std::unique_ptr<ComboAttachment> chorusModelAttachment;
    std::atomic<float>* chorusModelValue = nullptr; std::atomic<float>* chorusModeValue = nullptr;
    std::array<juce::TextButton, 7> chorusModeButtons;
    std::array<juce::TextButton, 3> chorusSwitches;
    std::array<std::unique_ptr<ButtonAttachment>, 3> chorusSwitchAttachments;
    std::array<juce::Slider, 6> chorusKnobs; std::array<juce::Label, 6> chorusLabels;
    std::array<std::unique_ptr<SliderAttachment>, 6> chorusAttachments;
    std::vector<std::pair<juce::Slider*, juce::Label*>> chorusGrid;
    int lastChorusModel = -1, lastChorusMode = -1;
    int shownTypeId = 0;
    CompressorView compressorView;
    juce::TextButton cpVintage { "VINTAGE" }, cpModern { "MODERN" };
    std::array<juce::TextButton, rs::comp::numTimes> cpTimeButtons;
    std::array<juce::Slider, 3> cpKnobs; std::array<juce::Label, 3> cpLabels;
    std::array<std::unique_ptr<SliderAttachment>, 3> cpAttachments;
    std::atomic<float>* cpModeValue = nullptr; std::atomic<float>* cpTimeValue = nullptr;
    int lastCompMode = -1, lastCompTime = -1;
};

FxRackEditor::FxRackEditor (ReverseSamplerProcessor& p) : processor(p) {
    close.onClick = [this] { if (onClose) onClose(); }; addAndMakeVisible (close);
    for (int i=0; i<rs::fxSlots; ++i) { slots[(size_t)i] = std::make_unique<Slot> (processor, i); addAndMakeVisible (*slots[(size_t)i]); }
}
FxRackEditor::~FxRackEditor() = default;
void FxRackEditor::paint (juce::Graphics& g) {
    g.fillAll (Theme::background); g.setColour (Theme::text); g.setFont (juce::FontOptions (22.0f, juce::Font::bold));
    g.drawText ("FX RACK", 15, 6, 130, 30, juce::Justification::centredLeft);
    g.setColour (Theme::dimText); g.setFont (juce::FontOptions (12.0f));
    g.drawText ("Serial chain 1 > 5  |  A = layer 1, B = layer 2  |  Drag slot headers to swap", 150, 6, getWidth()-250, 30, juce::Justification::centredLeft);
}
void FxRackEditor::resized() {
    auto area = getLocalBounds().reduced (8); auto header = area.removeFromTop (32); close.setBounds (header.removeFromRight (70)); area.removeFromTop (4);
    for (int i=0; i<rs::fxSlots; ++i) { const int width = area.getWidth() / (rs::fxSlots - i); slots[(size_t)i]->setBounds (area.removeFromLeft (width)); }
}
