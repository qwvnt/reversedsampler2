#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "SamplerEngine.h"

/** Draws a layer's sample. A reversed layer is drawn mirrored, so the view always
    shows what you hear from left to right. The part outside the region (Start/End)
    is dimmed; in editable mode the two region markers can be dragged.
*/
class WaveformView final : public juce::Component
{
public:
    enum Marker { startMarker = 0, endMarker = 1 };

    WaveformView();

    void setSample (rs::SampleDataPtr newSample);
    void setReversed (bool shouldBeReversed);
    void setLayerEnabled (bool shouldBeEnabled);
    void setLayerColour (juce::Colour newColour);
    void setRegion (float startFraction, float endFraction);   // forward order, 0..1
    void setPlayheadPosition (float forwardPosition);          // negative hides the playhead
    void setDragHighlight (bool shouldHighlight);
    void setEmptyText (const juce::String& newText);
    void setEditable (bool canDragMarkers);
    void setCaption (const juce::String& text, juce::Colour textColour);   // small note at the top left

    bool hasSample() const noexcept   { return sample != nullptr; }

    /** A click that was not a marker drag. */
    std::function<void()> onClick;

    /** Press and release on the waveform (not on a marker), when a sample is loaded: e.g. to listen while held. */
    std::function<void()> onPressed, onReleased;
    bool isPressed() const noexcept   { return pressed; }

    /** A small dim text at the bottom left, shown when a sample is loaded. */
    void setHint (const juce::String& text);

    /** Marker drags in editable mode (fraction of the forward-ordered sample). */
    std::function<void (Marker)> onMarkerDragStarted;
    std::function<void (Marker, float)> onMarkerDragged;
    std::function<void (Marker)> onMarkerDragEnded;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::Rectangle<float> getWaveArea() const;
    float forwardToX (float forwardPosition) const;
    float xToForward (float x) const;
    int findMarkerAt (float x) const;
    void updateCursor (int hoveredMarker);

    rs::SampleDataPtr sample;
    juce::Colour colour;
    juce::String emptyText { "Drop an audio file here\nor click to browse" };
    juce::String caption, hint;
    juce::Colour captionColour;
    float regionStart = 0.0f, regionEnd = 1.0f;
    float playhead = -1.0f;
    bool reversed = false, enabled = true, dragHighlight = false, editable = false, pressed = false;
    int hoverMarker = -1, draggedMarker = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformView)
};
