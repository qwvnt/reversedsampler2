#include "EnvelopeEditor.h"
#include "Theme.h"

namespace
{
    constexpr float minGap = 0.002f;   // points keep at least this distance in x
}

EnvelopeEditor::EnvelopeEditor()
{
    setRepaintsOnMouseActivity (false);
}

void EnvelopeEditor::setEnvelope (const rs::Envelope& newEnvelope)
{
    if (dragging >= 0 || newEnvelope == envelope)
        return;

    envelope = newEnvelope;
    envelope.sanitise();
    repaint();
}

void EnvelopeEditor::setCurveColour (juce::Colour newColour)
{
    colour = newColour;
    repaint();
}

void EnvelopeEditor::setEditable (bool shouldBeEditable)
{
    editable = shouldBeEditable;
    setInterceptsMouseClicks (editable, false);
    repaint();
}

//==============================================================================
int EnvelopeEditor::addPoint (float x, float y)
{
    if (envelope.numPoints >= rs::Envelope::maxPoints)
        return -1;

    x = juce::jlimit (minGap, 1.0f - minGap, x);
    y = juce::jlimit (0.0f, 1.0f, y);

    int index = 1;

    while (index < envelope.numPoints - 1 && envelope.x[(size_t) index] < x)
        ++index;

    for (int i = envelope.numPoints; i > index; --i)
    {
        envelope.x[(size_t) i] = envelope.x[(size_t) i - 1];
        envelope.y[(size_t) i] = envelope.y[(size_t) i - 1];
    }

    envelope.x[(size_t) index] = x;
    envelope.y[(size_t) index] = y;
    ++envelope.numPoints;
    changed();
    return index;
}

void EnvelopeEditor::movePoint (int index, float x, float y)
{
    if (! juce::isPositiveAndBelow (index, envelope.numPoints))
        return;

    const auto last = envelope.numPoints - 1;

    if (index == 0)
        x = 0.0f;
    else if (index == last)
        x = 1.0f;
    else
        x = juce::jlimit (envelope.x[(size_t) index - 1] + minGap, envelope.x[(size_t) index + 1] - minGap, x);

    envelope.x[(size_t) index] = x;
    envelope.y[(size_t) index] = juce::jlimit (0.0f, 1.0f, y);
    changed();
}

void EnvelopeEditor::removePoint (int index)
{
    if (index <= 0 || index >= envelope.numPoints - 1)
        return;   // the first and the last point stay

    for (int i = index; i < envelope.numPoints - 1; ++i)
    {
        envelope.x[(size_t) i] = envelope.x[(size_t) i + 1];
        envelope.y[(size_t) i] = envelope.y[(size_t) i + 1];
    }

    --envelope.numPoints;
    changed();
}

void EnvelopeEditor::resetToFlat()
{
    envelope = {};
    changed();
}

void EnvelopeEditor::resetToDefault()
{
    envelope = defaultEnvelope;
    envelope.sanitise();
    changed();
}

void EnvelopeEditor::resetPointToDefault (int index)
{
    if (! juce::isPositiveAndBelow (index, envelope.numPoints))
        return;

    const auto x = envelope.x[(size_t) index];
    movePoint (index, x, getDefaultValueAt (x));
}

void EnvelopeEditor::resetPointsNear (juce::Point<float> screen)
{
    const auto index = findPoint (screen);

    if (index >= 0 && std::abs (envelope.y[(size_t) index] - getDefaultValueAt (envelope.x[(size_t) index])) > 1.0e-6f)
        resetPointToDefault (index);
}

void EnvelopeEditor::changed()
{
    repaint();

    if (onChange != nullptr)
        onChange (envelope);
}

//==============================================================================
juce::Rectangle<float> EnvelopeEditor::getPlotArea() const noexcept
{
    return getLocalBounds().toFloat().reduced ((float) inset);
}

juce::Point<float> EnvelopeEditor::toScreen (int index) const noexcept
{
    const auto area = getPlotArea();
    return { area.getX() + envelope.x[(size_t) index] * area.getWidth(),
             area.getBottom() - envelope.y[(size_t) index] * area.getHeight() };
}

juce::Point<float> EnvelopeEditor::toEnvelope (juce::Point<float> screen) const noexcept
{
    const auto area = getPlotArea();
    return { (screen.x - area.getX()) / juce::jmax (1.0f, area.getWidth()),
             (area.getBottom() - screen.y) / juce::jmax (1.0f, area.getHeight()) };
}

int EnvelopeEditor::findPoint (juce::Point<float> screen) const noexcept
{
    int best = -1;
    auto bestDistance = pointRadius * 2.0f;

    for (int i = 0; i < envelope.numPoints; ++i)
    {
        const auto distance = toScreen (i).getDistanceFrom (screen);

        if (distance <= bestDistance)
        {
            best = i;
            bestDistance = distance;
        }
    }

    return best;
}

void EnvelopeEditor::paint (juce::Graphics& g)
{
    const auto area = getPlotArea();

    if (paintBackground != nullptr)
        paintBackground (g, area);

    if (editable)
    {
        g.setColour (Theme::border.withAlpha (0.6f));

        for (auto level : { 0.25f, 0.5f, 0.75f })
            g.drawHorizontalLine (juce::roundToInt (area.getBottom() - level * area.getHeight()), area.getX(), area.getRight());
    }

    juce::Path curve;

    for (int i = 0; i < envelope.numPoints; ++i)
    {
        const auto p = toScreen (i);

        if (i == 0)
            curve.startNewSubPath (p);
        else
            curve.lineTo (p);
    }

    if (filled)
    {
        auto fill = curve;
        fill.lineTo (area.getRight(), area.getBottom());
        fill.lineTo (area.getX(), area.getBottom());
        fill.closeSubPath();
        g.setColour (colour.withAlpha (0.12f));
        g.fillPath (fill);
    }

    g.setColour (colour);
    g.strokePath (curve, juce::PathStrokeType (2.0f));

    if (! editable)
        return;

    for (int i = 0; i < envelope.numPoints; ++i)
    {
        const auto p = toScreen (i);
        const auto radius = (i == hovered || i == dragging) ? pointRadius + 1.5f : pointRadius;
        g.setColour (Theme::background);
        g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (p));
        g.setColour (colour);
        g.drawEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (p), 2.0f);
    }

    if (dragging >= 0)
    {
        const auto p = toScreen (dragging);
        const auto text = juce::String (juce::roundToInt (envelope.y[(size_t) dragging] * 100.0f)) + " %";
        g.setColour (Theme::text);
        g.setFont (juce::FontOptions (12.0f));
        auto box = juce::Rectangle<float> (48.0f, 16.0f).withCentre (p.translated (0.0f, -16.0f));
        g.drawText (text, box.constrainedWithin (getLocalBounds().toFloat()), juce::Justification::centred);
    }
}

// Mouse event kinds handed to the parent.
namespace { enum { downEvent, dragEvent, upEvent, doubleClickEvent }; }

bool EnvelopeEditor::forwardToParent (const juce::MouseEvent& e, int kind)
{
    if (kind == downEvent || kind == doubleClickEvent)
        forwarding = passModifiedClicks && (e.mods.isShiftDown() || e.mods.isAltDown()) && getParentComponent() != nullptr;

    if (! forwarding)
        return false;

    auto* parent = getParentComponent();
    const auto relative = e.getEventRelativeTo (parent);

    switch (kind)
    {
        case downEvent:        parent->mouseDown (relative); break;
        case dragEvent:        parent->mouseDrag (relative); break;
        case upEvent:          parent->mouseUp (relative); forwarding = false; break;
        case doubleClickEvent: parent->mouseDoubleClick (relative); break;
        default: break;
    }

    return true;
}

void EnvelopeEditor::mouseDown (const juce::MouseEvent& e)
{
    if (! editable || forwardToParent (e, downEvent))
        return;

    const auto index = findPoint (e.position);

    // Cmd-click (Ctrl-click on Windows and Linux): back to the default level.
    if (e.mods.isCommandDown())
    {
        resetting = true;
        resetPointsNear (e.position);
        return;
    }

    if (e.mods.isPopupMenu())
    {
        removePoint (index);
        return;
    }

    if (index >= 0)
    {
        dragging = index;
    }
    else
    {
        const auto p = toEnvelope (e.position);
        dragging = addPoint (p.x, p.y);
    }

    repaint();
}

void EnvelopeEditor::mouseDrag (const juce::MouseEvent& e)
{
    if (forwardToParent (e, dragEvent))
        return;

    if (resetting)
    {
        resetPointsNear (e.position);
        return;
    }

    if (dragging >= 0)
    {
        const auto p = toEnvelope (e.position);
        movePoint (dragging, p.x, p.y);
    }
}

void EnvelopeEditor::mouseUp (const juce::MouseEvent& e)
{
    if (forwardToParent (e, upEvent))
        return;

    dragging = -1;
    resetting = false;
    repaint();
}

void EnvelopeEditor::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (forwardToParent (e, doubleClickEvent))
        return;   // the mouse-up that follows ends the forwarding

    if (editable && ! e.mods.isCommandDown())
        removePoint (findPoint (e.position));
}

bool EnvelopeEditor::hitTest (int x, int y)
{
    // The margins beside the curve belong to whatever lies underneath, unless a point is there.
    if (! editable)
        return false;

    return findPoint ({ (float) x, (float) y }) >= 0 || (x >= inset && x < getWidth() - inset);
}

void EnvelopeEditor::mouseMove (const juce::MouseEvent& e)
{
    const auto index = editable ? findPoint (e.position) : -1;

    if (index != hovered)
    {
        hovered = index;
        setMouseCursor (index >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::CrosshairCursor);
        repaint();
    }
}
