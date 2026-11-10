#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Controls.h"
#include "PluginProcessor.h"
#include "TimelineView.h"

/** The SAMPLE editor. It opens over the main page when you click the SAMPLE box.

    The common time line of both layers with the final envelope (on the sum) to draw,
    MODE (Swell / Arrive), PEAK (moved earlier), AFTER PEAK (Cut / Release / Freeze /
    Loop, with the Release fade drawn on the time line) and the NOTE envelope (attack,
    release and its curve, volume).
*/
class SampleEditor final : public juce::Component
{
public:
    explicit SampleEditor (ReverseSamplerProcessor&);

    void sync();

    int getLayerAt (juce::Point<int> position) const;   // local coordinates
    void setDragHighlight (int layer, bool shouldHighlight);

    TimelineView& getTimeline() noexcept   { return timeline; }

    std::function<void()> onClose;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void layoutGroup (juce::Rectangle<int> area, std::initializer_list<juce::Component*> columns);

    ReverseSamplerProcessor& samplerProcessor;

    juce::TextButton flatButton { "Flat" }, doneButton { "Done" };
    juce::Label infoLabel, hintLabel;
    TimelineView timeline;

    ChoiceButtons modeButtons;
    LabelledCombo afterPeakBox;
    LabelledKnob peakKnob, dissolveKnob, attackKnob, releaseKnob, curveKnob, volumeKnob;

    juce::Rectangle<int> modeGroup, afterGroup, noteGroup;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SampleEditor)
};
