#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "SamplerEngine.h"

/** Draws a volume envelope and lets you edit it with the mouse.
    Click: add a point. Drag: move it. Double-click or right-click a point: delete it.
    Cmd-click (Ctrl-click on Windows/Linux) a point: back to its default level; Cmd/Ctrl-drag
    resets every point the mouse passes over.
    The first and last points stay at the start and at the peak; they only move up and down.
*/
class EnvelopeEditor final : public juce::Component
{
public:
    EnvelopeEditor();

    static constexpr float pointRadius = 5.0f;
    static constexpr int inset = 6;   // between the bounds and the curve (room for the points)

    void setEnvelope (const rs::Envelope&);   // does not call onChange
    const rs::Envelope& getEnvelope() const noexcept   { return envelope; }

    void setCurveColour (juce::Colour);
    void setEditable (bool shouldBeEditable);
    void setFilled (bool shouldFill)                     { filled = shouldFill; repaint(); }

    /** The envelope that Cmd/Ctrl-click and resetToDefault() go back to (flat by default). */
    void setDefaultEnvelope (const rs::Envelope& e)      { defaultEnvelope = e; }
    float getDefaultValueAt (float x) const noexcept     { return defaultEnvelope.valueAt (x); }

    /** Shift- and Alt-clicks go to the parent component (the time line uses them for its own gestures). */
    void setPassesModifiedClicksToParent (bool shouldPass)   { passModifiedClicks = shouldPass; }

    /** Called after every edit. */
    std::function<void (const rs::Envelope&)> onChange;

    /** Paints under the curve, e.g. the waveform of the build. */
    std::function<void (juce::Graphics&, juce::Rectangle<float> plotArea)> paintBackground;

    // Editing, in envelope coordinates (0..1). Used by the mouse and by tests.
    int addPoint (float x, float y);
    void movePoint (int index, float x, float y);
    void removePoint (int index);
    void resetToFlat();
    void resetToDefault();
    void resetPointToDefault (int index);   // its level goes back to the default curve at its position

    juce::Rectangle<float> getPlotArea() const noexcept;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    bool hitTest (int x, int y) override;

private:
    juce::Point<float> toScreen (int index) const noexcept;
    juce::Point<float> toEnvelope (juce::Point<float> screen) const noexcept;
    int findPoint (juce::Point<float> screen) const noexcept;
    void changed();
    void resetPointsNear (juce::Point<float> screen);
    bool forwardToParent (const juce::MouseEvent&, int kind);

    rs::Envelope envelope, defaultEnvelope;
    juce::Colour colour { juce::Colours::white };
    bool editable = true, filled = true, passModifiedClicks = false, forwarding = false, resetting = false;
    int dragging = -1, hovered = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (EnvelopeEditor)
};
