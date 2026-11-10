#include "SampleEditor.h"
#include "Theme.h"

namespace
{
    constexpr int headerHeight = 34, groupTitle = 18, controlsHeight = 112, hintHeight = 30;

    void styleLabel (juce::Label& label, juce::Colour colour, float size)
    {
        label.setColour (juce::Label::textColourId, colour);
        label.setFont (juce::FontOptions (size));
        label.setInterceptsMouseClicks (false, false);
    }
}

SampleEditor::SampleEditor (ReverseSamplerProcessor& p)
    : samplerProcessor (p),
      timeline (p, true),
      modeButtons (p.getValueTreeState(), ParamIDs::playMode, Theme::text),
      afterPeakBox (p.getValueTreeState(), ParamIDs::afterPeak, "After peak"),
      peakKnob (p.getValueTreeState(), ParamIDs::peakCut, "Peak", Theme::warning),
      dissolveKnob (p.getValueTreeState(), ParamIDs::dissolve, "Fade", Theme::text),
      attackKnob (p.getValueTreeState(), ParamIDs::attack, "Attack", Theme::text),
      releaseKnob (p.getValueTreeState(), ParamIDs::release, "Release", Theme::text),
      curveKnob (p.getValueTreeState(), ParamIDs::releaseCurve, "Curve", Theme::text),
      volumeKnob (p.getValueTreeState(), ParamIDs::gain, "Volume", Theme::text)
{
    styleLabel (infoLabel, Theme::dimText, 13.0f);
    styleLabel (hintLabel, Theme::dimText, 11.5f);
    const juce::String cmd (KeyNames::command), alt (KeyNames::alt);
    hintLabel.setText ("Final envelope (sum of both layers): click to add a point, drag to move, double-click to delete, " + cmd
                       + "-click to reset.  Release: the curve after the PEAK is the fade.\n"
                       + "Shift-drag a layer: move it, a start before the note is trimmed (snaps, Shift-" + alt + ": free; Shift-double-click: back to 0).  "
                       + "Drag the PEAK flag: move the peak (for all layers).  " + alt + "-click and hold a layer: solo.  " + cmd + "-wheel: zoom.",
                       juce::dontSendNotification);
    hintLabel.setMinimumHorizontalScale (0.8f);

    flatButton.setTooltip ("Reset the final envelope to flat and the Release fade to its default");
    flatButton.onClick = [this]
    {
        timeline.getEnvelopeEditor().resetToFlat();
        timeline.getDissolveEditor().resetToDefault();
    };
    doneButton.onClick = [this] { if (onClose != nullptr) onClose(); };

    modeButtons.buttons[0]->setTooltip ("Swell: the build starts with the key, the peak comes later");
    modeButtons.buttons[1]->setTooltip ("Arrive: the peak lands on the note (the plugin reports latency, the host plays earlier)");
    afterPeakBox.setTooltip ("What happens at the PEAK, for all layers: Cut (stop), Release (the sample plays on and fades out), "
                             "Freeze (holds the sound of the PEAK while the key is held), Loop (the build repeats while the key is held)");
    dissolveKnob.setTooltip ("Release: how long the sound takes to fade out after the PEAK (the shape is the curve after the PEAK)");
    peakKnob.setTooltip ("Moves the PEAK earlier: every layer plays until it, then After peak starts for all layers (drag the PEAK flag on the time line, double-click to reset)");
    curveKnob.setTooltip ("Release curve: positive = fast drop and a long tail, negative = holds longer, then falls");

    for (auto* c : std::initializer_list<juce::Component*> { &infoLabel, &hintLabel, &flatButton, &doneButton, &timeline, &modeButtons,
                                                            &afterPeakBox, &peakKnob, &dissolveKnob, &attackKnob, &releaseKnob, &curveKnob, &volumeKnob })
        addAndMakeVisible (c);

    sync();
}

void SampleEditor::sync()
{
    timeline.sync();
    infoLabel.setText (timeline.describe(), juce::dontSendNotification);
    dissolveKnob.setEnabled (samplerProcessor.getAfterPeak() == rs::AfterPeak::release);
}

int SampleEditor::getLayerAt (juce::Point<int> position) const
{
    return timeline.getLayerAt (position - timeline.getPosition());
}

void SampleEditor::setDragHighlight (int layer, bool shouldHighlight)
{
    timeline.setDragHighlight (layer, shouldHighlight);
}

//==============================================================================
void SampleEditor::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.75f);
    g.setColour (Theme::panel);
    g.fillRoundedRectangle (bounds, 10.0f);
    g.setColour (Theme::text.withAlpha (0.5f));
    g.drawRoundedRectangle (bounds, 10.0f, 1.5f);

    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
    g.drawText ("SAMPLE", getLocalBounds().reduced (14, 0).removeFromTop (headerHeight), juce::Justification::centredLeft);

    const std::pair<juce::Rectangle<int>, const char*> groups[] = { { modeGroup, "MODE" }, { afterGroup, "PEAK / AFTER PEAK" }, { noteGroup, "NOTE" } };

    for (const auto& [area, title] : groups)
    {
        g.setColour (Theme::panelLight);
        g.fillRoundedRectangle (area.toFloat(), 6.0f);
        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (title, area.withHeight (groupTitle).reduced (8, 0), juce::Justification::centredLeft);
    }
}

void SampleEditor::layoutGroup (juce::Rectangle<int> area, std::initializer_list<juce::Component*> columns)
{
    area.removeFromTop (groupTitle);
    area = area.reduced (6, 2);
    const auto width = area.getWidth() / juce::jmax (1, (int) columns.size());

    for (auto* c : columns)
        c->setBounds (area.removeFromLeft (width).reduced (2, 0));
}

void SampleEditor::resized()
{
    auto area = getLocalBounds().reduced (10, 0);

    auto header = area.removeFromTop (headerHeight).reduced (0, 5);
    header.removeFromLeft (80);
    doneButton.setBounds (header.removeFromRight (64));
    header.removeFromRight (6);
    flatButton.setBounds (header.removeFromRight (56));
    header.removeFromRight (10);
    infoLabel.setBounds (header);

    area.removeFromBottom (8);
    auto controls = area.removeFromBottom (controlsHeight);
    area.removeFromBottom (6);
    hintLabel.setBounds (area.removeFromBottom (hintHeight));
    timeline.setBounds (area);

    const auto unit = (controls.getWidth() - 16) / 10;
    modeGroup = controls.removeFromLeft (unit * 2);
    controls.removeFromLeft (8);
    afterGroup = controls.removeFromLeft (unit * 4);
    controls.removeFromLeft (8);
    noteGroup = controls;

    auto modeArea = modeGroup.withTrimmedTop (groupTitle).reduced (10, 0);
    modeButtons.setBounds (modeArea.withSizeKeepingCentre (modeArea.getWidth(), 28));

    auto afterArea = afterGroup.withTrimmedTop (groupTitle).reduced (8, 2);
    const auto knobWidth = juce::jmin (90, afterArea.getWidth() / 3);
    peakKnob.setBounds (afterArea.removeFromLeft (knobWidth));
    afterArea.removeFromLeft (6);
    dissolveKnob.setBounds (afterArea.removeFromRight (knobWidth));
    afterPeakBox.setBounds (afterArea.withSizeKeepingCentre (afterArea.getWidth(), 44));

    layoutGroup (noteGroup, { &attackKnob, &releaseKnob, &curveKnob, &volumeKnob });
}
