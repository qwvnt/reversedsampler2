#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "TimelineView.h"

/** The SAMPLE box in the centre of the main page: both layers on one time line,
    the peak, what comes after it and the final envelope. Click it to open the SAMPLE editor.
*/
class SampleView final : public juce::Component
{
public:
    explicit SampleView (ReverseSamplerProcessor&);

    /** Reads the current samples, settings and playhead. Called by the editor's timer. */
    void sync();

    /** The layer lane at this point (local coordinates). */
    int getLayerAt (juce::Point<int> position) const;
    void setDragHighlight (int layer, bool shouldHighlight);

    TimelineView& getTimeline() noexcept   { return timeline; }

    std::function<void()> onOpenEditor;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    TimelineView timeline;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleView)
};
