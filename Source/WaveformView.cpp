#include "WaveformView.h"
#include "Theme.h"

#include <cmath>

namespace
{
    constexpr float markerHitDistance = 7.0f;
}

WaveformView::WaveformView()
    : colour (Theme::layer1)
{
    updateCursor (-1);
}

//==============================================================================
void WaveformView::setSample (rs::SampleDataPtr newSample)
{
    if (sample != newSample)
    {
        sample = std::move (newSample);
        updateCursor (-1);
        repaint();
    }
}

void WaveformView::setReversed (bool shouldBeReversed)
{
    if (reversed != shouldBeReversed)
    {
        reversed = shouldBeReversed;
        repaint();
    }
}

void WaveformView::setLayerEnabled (bool shouldBeEnabled)
{
    if (enabled != shouldBeEnabled)
    {
        enabled = shouldBeEnabled;
        repaint();
    }
}

void WaveformView::setLayerColour (juce::Colour newColour)
{
    if (colour != newColour)
    {
        colour = newColour;
        repaint();
    }
}

void WaveformView::setRegion (float startFraction, float endFraction)
{
    startFraction = juce::jlimit (0.0f, 1.0f, startFraction);
    endFraction   = juce::jlimit (0.0f, 1.0f, endFraction);

    if (std::abs (startFraction - regionStart) > 1.0e-6f || std::abs (endFraction - regionEnd) > 1.0e-6f)
    {
        regionStart = startFraction;
        regionEnd = endFraction;
        repaint();
    }
}

void WaveformView::setPlayheadPosition (float forwardPosition)
{
    if (std::abs (forwardPosition - playhead) > 1.0e-4f)
    {
        playhead = forwardPosition;
        repaint();
    }
}

void WaveformView::setDragHighlight (bool shouldHighlight)
{
    if (dragHighlight != shouldHighlight)
    {
        dragHighlight = shouldHighlight;
        repaint();
    }
}

void WaveformView::setEmptyText (const juce::String& newText)
{
    if (emptyText != newText)
    {
        emptyText = newText;
        repaint();
    }
}

void WaveformView::setEditable (bool canDragMarkers)
{
    editable = canDragMarkers;
    updateCursor (-1);
    repaint();
}

void WaveformView::setHint (const juce::String& text)
{
    if (hint != text)
    {
        hint = text;
        repaint();
    }
}

void WaveformView::setCaption (const juce::String& text, juce::Colour textColour)
{
    if (caption != text || captionColour != textColour)
    {
        caption = text;
        captionColour = textColour;
        repaint();
    }
}

//==============================================================================
juce::Rectangle<float> WaveformView::getWaveArea() const
{
    auto area = getLocalBounds().toFloat().reduced (10.0f, 6.0f);
    area.removeFromTop (16.0f);   // badge and marker handles
    return area.reduced (0.0f, 2.0f);
}

float WaveformView::forwardToX (float forwardPosition) const
{
    const auto area = getWaveArea();
    return area.getX() + (reversed ? 1.0f - forwardPosition : forwardPosition) * area.getWidth();
}

float WaveformView::xToForward (float x) const
{
    const auto area = getWaveArea();
    const auto t = juce::jlimit (0.0f, 1.0f, (x - area.getX()) / juce::jmax (1.0f, area.getWidth()));
    return reversed ? 1.0f - t : t;
}

int WaveformView::findMarkerAt (float x) const
{
    if (! editable || sample == nullptr)
        return -1;

    const auto startDistance = std::abs (x - forwardToX (regionStart));
    const auto endDistance   = std::abs (x - forwardToX (regionEnd));

    if (juce::jmin (startDistance, endDistance) > markerHitDistance)
        return -1;

    if (std::abs (startDistance - endDistance) < 0.5f)
    {
        // Both markers are at the same spot: pick the one that can move towards the mouse.
        const auto towardsHigherPosition = xToForward (x) >= regionStart;
        return (regionStart <= regionEnd) == towardsHigherPosition ? endMarker : startMarker;
    }

    return startDistance < endDistance ? startMarker : endMarker;
}

void WaveformView::updateCursor (int hoveredMarker)
{
    hoverMarker = hoveredMarker;

    if (hoveredMarker >= 0 || draggedMarker >= 0)
        setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
    else if (sample == nullptr || ! editable)
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
    else
        setMouseCursor (juce::MouseCursor::NormalCursor);
}

//==============================================================================
void WaveformView::resized()
{
    repaint();
}

void WaveformView::mouseMove (const juce::MouseEvent& event)
{
    const auto marker = findMarkerAt ((float) event.x);

    if (marker != hoverMarker)
    {
        updateCursor (marker);
        repaint();
    }
}

void WaveformView::mouseExit (const juce::MouseEvent&)
{
    if (hoverMarker >= 0 && draggedMarker < 0)
    {
        updateCursor (-1);
        repaint();
    }
}

void WaveformView::mouseDown (const juce::MouseEvent& event)
{
    draggedMarker = findMarkerAt ((float) event.x);

    if (draggedMarker >= 0 && onMarkerDragStarted != nullptr)
        onMarkerDragStarted ((Marker) draggedMarker);

    if (draggedMarker < 0 && sample != nullptr && onPressed != nullptr && ! event.mods.isPopupMenu())
    {
        pressed = true;
        onPressed();
        repaint();
    }
}

void WaveformView::mouseDrag (const juce::MouseEvent& event)
{
    if (draggedMarker >= 0 && onMarkerDragged != nullptr)
        onMarkerDragged ((Marker) draggedMarker, xToForward ((float) event.x));
}

void WaveformView::mouseUp (const juce::MouseEvent& event)
{
    if (draggedMarker >= 0)
    {
        const auto marker = (Marker) draggedMarker;
        draggedMarker = -1;
        updateCursor (findMarkerAt ((float) event.x));

        if (onMarkerDragEnded != nullptr)
            onMarkerDragEnded (marker);

        repaint();
        return;
    }

    if (pressed)
    {
        pressed = false;

        if (onReleased != nullptr)
            onReleased();

        repaint();
    }

    if (event.mouseWasClicked() && onClick != nullptr)
        onClick();
}

//==============================================================================
void WaveformView::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.5f);

    g.setColour (Theme::panel);
    g.fillRoundedRectangle (bounds, 6.0f);

    g.setColour (dragHighlight ? colour : Theme::border);
    g.drawRoundedRectangle (bounds, 6.0f, dragHighlight ? 2.0f : 1.0f);

    if (sample == nullptr || sample->overviewMax.empty())
    {
        g.setColour (dragHighlight ? Theme::text : Theme::dimText);
        g.setFont (juce::FontOptions (15.0f));
        g.drawFittedText (emptyText, getLocalBounds().reduced (8), juce::Justification::centred, 3);
        return;
    }

    const auto area       = getWaveArea();
    const auto centreY    = area.getCentreY();
    const auto halfHeight = area.getHeight() * 0.5f;
    const auto numBins    = (int) sample->overviewMax.size();
    const auto numColumns = juce::jmax (1, (int) area.getWidth());
    const auto scale      = sample->overviewPeak > 1.0e-6f ? 1.0f / sample->overviewPeak : 1.0f;   // fit to height

    const auto accent   = ! enabled ? Theme::disabled : (reversed ? Theme::reversed : colour);
    const auto outside  = accent.withMultipliedAlpha (0.3f);

    const auto low  = juce::jmin (regionStart, regionEnd);
    const auto high = juce::jmax (regionStart, regionEnd);
    const auto regionLeft  = juce::jmin (forwardToX (low), forwardToX (high));
    const auto regionRight = juce::jmax (forwardToX (low), forwardToX (high));

    g.setColour (Theme::border);
    g.drawHorizontalLine (juce::roundToInt (centreY), area.getX(), area.getRight());

    for (int column = 0; column < numColumns; ++column)
    {
        // Screen column -> range of the forward-ordered overview.
        const auto screenStart = (float) column / (float) numColumns;
        const auto screenEnd   = (float) (column + 1) / (float) numColumns;
        const auto from = reversed ? 1.0f - screenEnd   : screenStart;
        const auto to   = reversed ? 1.0f - screenStart : screenEnd;

        const auto firstBin = juce::jlimit (0, numBins - 1, (int) (from * (float) numBins));
        const auto endBin   = juce::jlimit (firstBin + 1, numBins, (int) std::ceil (to * (float) numBins));

        auto minValue = 0.0f, maxValue = 0.0f;

        for (auto bin = firstBin; bin < endBin; ++bin)
        {
            minValue = juce::jmin (minValue, sample->overviewMin[(size_t) bin]);
            maxValue = juce::jmax (maxValue, sample->overviewMax[(size_t) bin]);
        }

        const auto x      = area.getX() + (float) column;
        const auto top    = centreY - juce::jmin (1.0f, maxValue * scale) * halfHeight;
        const auto bottom = centreY - juce::jmax (-1.0f, minValue * scale) * halfHeight;
        const auto inside = x + 0.5f >= regionLeft && x + 0.5f <= regionRight;

        g.setColour (inside ? accent : outside);
        g.fillRect (x, top, 1.0f, juce::jmax (1.0f, bottom - top));
    }

    // Badge: what the layer does right now.
    const auto badgeArea = getLocalBounds().toFloat().reduced (10.0f, 4.0f).removeFromTop (16.0f);
    g.setFont (juce::FontOptions (11.5f, juce::Font::bold));
    g.setColour (accent);
    g.drawText (! enabled ? "OFF" : (reversed ? "REVERSED" : "FORWARD"), badgeArea,
                editable ? juce::Justification::centred : juce::Justification::centredRight);   // marker handles sit at the edges

    if (hint.isNotEmpty())
    {
        g.setColour (pressed ? accent : Theme::dimText);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (pressed ? juce::String ("PLAYING SOLO (release to stop)") : hint,
                    getLocalBounds().toFloat().reduced (10.0f, 4.0f).removeFromBottom (16.0f), juce::Justification::centredLeft, true);
    }

    if (caption.isNotEmpty() && ! editable)
    {
        g.setColour (captionColour);
        g.drawText (caption, badgeArea.withTrimmedRight (90.0f), juce::Justification::centredLeft, true);
    }

    // Region markers.
    const auto regionIsTrimmed = low > 0.0005f || high < 0.9995f;

    if (editable || regionIsTrimmed)
    {
        for (int marker = startMarker; marker <= endMarker; ++marker)
        {
            const auto x = forwardToX (marker == startMarker ? regionStart : regionEnd);
            const auto active = marker == hoverMarker || marker == draggedMarker;

            g.setColour (active ? Theme::text : Theme::text.withAlpha (0.75f));
            g.fillRect (x - 0.75f, area.getY() - 2.0f, 1.5f, area.getHeight() + 4.0f);

            if (editable)
            {
                const juce::Rectangle<float> handle (x - 9.0f, area.getY() - 17.0f, 18.0f, 15.0f);
                g.fillRoundedRectangle (handle, 3.0f);
                g.setColour (Theme::background);
                g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
                g.drawText (marker == startMarker ? "S" : "E", handle, juce::Justification::centred);
            }
        }
    }

    if (playhead >= 0.0f)
    {
        const auto playheadX = forwardToX (playhead);
        g.setColour (Theme::text);
        g.fillRect (playheadX - 0.75f, area.getY(), 1.5f, area.getHeight());
    }
}
