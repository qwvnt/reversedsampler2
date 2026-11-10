#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "EnvelopeEditor.h"
#include "PluginProcessor.h"

/** One note on a common time line: each layer where it plays (a shorter build starts
    later so the peaks meet), the peak line, what happens after the peak, the final
    envelope on top and the playhead. Shown for the last played note (C3 at first).

    Mouse (Cmd = Ctrl on Windows and Linux, Alt = Option on the Mac):
      Shift-drag a layer           moves its peak (Peak offset); snaps to transients, Shift-Alt: no snap
      Shift-double-click a layer   peak offset back to 0
      drag the PEAK flag           moves the common peak earlier (the builds end sooner); Alt: no snap
      double-click the PEAK flag   back to the natural peak
      Alt-click and hold a layer   plays that layer alone
      Cmd-wheel                    zoom; the wheel scrolls when zoomed; double-click the time axis to see all
*/
class TimelineView final : public juce::Component
{
public:
    TimelineView (ReverseSamplerProcessor&, bool canEditEnvelope);
    ~TimelineView() override;

    /** Reads samples, settings and the playhead. Called by the editor's timer. */
    void sync();

    /** The layer lane at this point (local coordinates). */
    int getLayerAt (juce::Point<int> position) const;
    void setDragHighlight (int layer, bool shouldHighlight);

    EnvelopeEditor& getEnvelopeEditor() noexcept   { return envelopeEditor; }
    EnvelopeEditor& getDissolveEditor() noexcept   { return dissolveEditor; }

    /** Text about the timing of the shown note, e.g. "Peak 2.50 s after the key (C3)". */
    juce::String describe() const;

    std::function<void()> onClick;   // the read-only view on the main page

    //==============================================================================
    // The mouse gestures, also used by tests. Distances are in pixels.
    void beginLayerDrag (int layer);
    void dragLayer (float deltaPixels, bool snap);
    void endLayerDrag();
    void resetLayerOffset (int layer);

    void beginPeakDrag();
    void dragPeak (float deltaPixels, bool snap);
    void endPeakDrag();
    void resetPeak();

    bool isSnapped() const noexcept                 { return snapped; }
    void zoomAround (float x, float factor);        // factor < 1 zooms in
    void scrollBy (double fractionOfView);
    void resetZoom();
    bool isZoomed() const noexcept                  { return ! zoomRange.isEmpty(); }

    float timeToX (double samples) const noexcept;  // host samples on the layout -> x
    double xToTime (float x) const noexcept;
    juce::Range<double> getVisibleRange() const noexcept   { return { viewStart, viewEnd }; }
    const rs::NoteLayout& getLayout() const noexcept       { return layout; }

    /** Transients of a layer as fractions of its whole build, in play order. */
    const std::vector<double>& getOnsets (int layer) const noexcept   { return onsetFractions[(size_t) layer]; }

    juce::Rectangle<int> getLaneBounds (int layer) const noexcept    { return lanes[(size_t) layer]; }
    juce::Rectangle<float> getPeakHandleBounds() const noexcept;

    //==============================================================================
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    enum class Drag { none, layer, peak, audition };

    void paintLane (juce::Graphics&, int layer);
    void paintAxis (juce::Graphics&);
    void paintDragInfo (juce::Graphics&);
    void updateEnvelopeBounds();
    void updateOnsets();
    void updateViewRange();
    void setParameter (const juce::String& id, float value);
    juce::RangedAudioParameter* getParameter (const juce::String& id) const;
    int getLaneAt (juce::Point<int> position) const;   // -1 outside the lanes
    bool isOnPeakHandle (juce::Point<float> position) const;
    void beginAnchor (int anchor);
    double anchorTime (int anchor) const noexcept;
    double findSnap (double desired, const std::vector<double>& sources, const std::vector<double>& targets);
    std::vector<double> layerPoints (int layer) const;   // start, end and transients of a build

    ReverseSamplerProcessor& samplerProcessor;
    const bool editable;
    EnvelopeEditor dissolveEditor, envelopeEditor;

    std::array<rs::SampleDataPtr, rs::numLayers> samples;
    std::array<rs::LayerSettings, rs::numLayers> settings;
    std::array<rs::Envelope, rs::numLayers> layerEnvelopes;
    rs::Envelope dissolveEnvelope;
    rs::NoteLayout layout;
    rs::PlayMode mode = rs::PlayMode::swell;
    rs::AfterPeak afterPeak = rs::AfterPeak::cut;
    double rate = 48000.0, dissolveSamples = 0.0, viewStart = 0.0, viewEnd = 1.0, playhead = -1.0;
    double fullStart = 0.0, fullEnd = 1.0;
    juce::Range<double> zoomRange;   // empty = everything visible
    int note = 60, shownSampleVersion = -1, auditionLayer = -1;
    juce::String shownState;
    std::array<bool, rs::numLayers> dragHighlight {};

    // Transients, as fractions of each layer's whole build (play order).
    std::array<std::vector<double>, rs::numLayers> onsetFractions;
    std::array<juce::String, rs::numLayers> onsetKeys;

    // The drag in progress. While dragging, the view range stays put and one point
    // (a layer start or the peak) is kept still, so the picture does not jump.
    Drag drag = Drag::none;
    int dragLayerIndex = -1, anchorIndex = -2;   // anchor: a layer, -1 = the common peak, -2 = none
    double anchorTime0 = 0.0, displayShift = 0.0;
    double dragStartValue = 0.0, dragPeak0 = 0.0, maxCut = 0.0;
    double minDragOffset = -8.0;   // a layer dragged earlier keeps at least 50 ms after the note start
    std::vector<double> snapSources, snapTargets;
    bool snapped = false, gestureMade = false;
    double snapGuide = 0.0;
    float dragMouseX = 0.0f, dragMouseY = 0.0f;

    juce::Rectangle<int> plot, axis;
    std::array<juce::Rectangle<int>, rs::numLayers> lanes;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TimelineView)
};
