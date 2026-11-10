#include "SampleView.h"
#include "Theme.h"

namespace
{
    constexpr int titleHeight = 22;
}

SampleView::SampleView (ReverseSamplerProcessor& p)
    : timeline (p, false)
{
    timeline.onClick = [this] { if (onOpenEditor != nullptr) onOpenEditor(); };
    addAndMakeVisible (timeline);
    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    timeline.setMouseCursor (juce::MouseCursor::PointingHandCursor);
}

void SampleView::sync()
{
    timeline.sync();
}

int SampleView::getLayerAt (juce::Point<int> position) const
{
    return timeline.getLayerAt (position - timeline.getPosition());
}

void SampleView::setDragHighlight (int layer, bool shouldHighlight)
{
    timeline.setDragHighlight (layer, shouldHighlight);
}

void SampleView::mouseUp (const juce::MouseEvent& e)
{
    if (e.mouseWasClicked() && onOpenEditor != nullptr)
        onOpenEditor();
}

void SampleView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.75f);

    g.setColour (Theme::panel);
    g.fillRoundedRectangle (bounds, 10.0f);
    g.setColour (Theme::border);
    g.drawRoundedRectangle (bounds, 10.0f, 1.5f);

    auto title = getLocalBounds().reduced (14, 6).removeFromTop (titleHeight);
    g.setColour (Theme::dimText);
    g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    g.drawText ("SAMPLE", title, juce::Justification::centred);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText ("click to edit", title, juce::Justification::centredRight);
}

void SampleView::resized()
{
    auto area = getLocalBounds().reduced (10, 6);
    area.removeFromTop (titleHeight);
    area.removeFromBottom (2);
    timeline.setBounds (area);
}
