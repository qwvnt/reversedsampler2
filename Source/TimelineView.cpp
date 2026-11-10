#include "TimelineView.h"
#include "LayerText.h"
#include "Theme.h"

namespace
{
    constexpr int topMargin = 22, axisHeight = 16, laneGap = 6;
    constexpr float snapPixels = 8.0f, handleHitPixels = 12.0f;
    constexpr int noAnchor = -2, peakAnchor = -1;

    juce::String signedSeconds (double s)
    {
        if (std::abs (s) < 0.0005)
            return "0.000 s";

        return (s > 0.0 ? "+" : "-") + juce::String (std::abs (s), 3) + " s";
    }

    void drawDashedVertical (juce::Graphics& g, float x, float top, float bottom)
    {
        for (auto y = top; y < bottom; y += 7.0f)
            g.drawLine (x, y, x, juce::jmin (bottom, y + 4.0f), 1.0f);
    }
}

TimelineView::TimelineView (ReverseSamplerProcessor& p, bool canEditEnvelope)
    : samplerProcessor (p), editable (canEditEnvelope)
{
    dissolveEditor.setCurveColour (Theme::text.withAlpha (0.85f));
    dissolveEditor.setFilled (false);
    dissolveEditor.setEditable (editable);
    dissolveEditor.setDefaultEnvelope (rs::Envelope::dissolveDefault());
    dissolveEditor.setPassesModifiedClicksToParent (true);
    dissolveEditor.onChange = [this] (const rs::Envelope& e) { samplerProcessor.setEnvelope (ReverseSamplerProcessor::dissolveEnvelope, e); };
    addChildComponent (dissolveEditor);

    // On top of the fade curve (Release): the point at the peak belongs to the final envelope.
    envelopeEditor.setCurveColour (Theme::text);
    envelopeEditor.setFilled (false);
    envelopeEditor.setEditable (editable);
    envelopeEditor.setPassesModifiedClicksToParent (true);
    envelopeEditor.onChange = [this] (const rs::Envelope& e) { samplerProcessor.setEnvelope (ReverseSamplerProcessor::mainEnvelope, e); };
    addChildComponent (envelopeEditor);

    sync();
}

TimelineView::~TimelineView()
{
    // A gesture still in progress when the view closes.
    if (drag == Drag::audition)
        samplerProcessor.stopAudition();
    else if (drag == Drag::layer && juce::isPositiveAndBelow (dragLayerIndex, rs::numLayers))
        if (auto* param = getParameter (ParamIDs::layerID (dragLayerIndex, ParamIDs::peakOffset)))
            param->endChangeGesture();

    if (drag == Drag::peak)
        if (auto* param = getParameter (ParamIDs::peakCut))
            param->endChangeGesture();
}

//==============================================================================
void TimelineView::sync()
{
    if (const auto version = samplerProcessor.getSampleVersion(); version != shownSampleVersion)
    {
        shownSampleVersion = version;

        for (int layer = 0; layer < rs::numLayers; ++layer)
            samples[(size_t) layer] = samplerProcessor.getSample (layer);
    }

    for (int layer = 0; layer < rs::numLayers; ++layer)
    {
        settings[(size_t) layer] = samplerProcessor.getLayerSettings (layer);
        layerEnvelopes[(size_t) layer] = samplerProcessor.getEnvelope (layer);
    }

    dissolveEnvelope = samplerProcessor.getEnvelope (ReverseSamplerProcessor::dissolveEnvelope);
    const auto lastNote = samplerProcessor.getLastNote();
    note = lastNote >= 0 ? lastNote : 60;
    rate = samplerProcessor.getPlaybackRate();
    mode = samplerProcessor.getPlayMode();
    afterPeak = samplerProcessor.getAfterPeak();
    dissolveSamples = samplerProcessor.getDissolveSeconds() * rate;
    layout = samplerProcessor.getNoteLayout (note);
    playhead = samplerProcessor.getTimelinePosition();
    auditionLayer = samplerProcessor.getAuditionLayer();

    updateOnsets();

    // While dragging, the view stays put and the anchor point stays still.
    if (drag == Drag::layer || drag == Drag::peak)
        displayShift = anchorTime (anchorIndex) - anchorTime0;
    else
        updateViewRange();

    const auto mainEnvelope = samplerProcessor.getEnvelope (ReverseSamplerProcessor::mainEnvelope);
    envelopeEditor.setEnvelope (mainEnvelope);
    envelopeEditor.setVisible (layout.anyLayer && (editable || ! mainEnvelope.isFlat()));

    dissolveEditor.setEnvelope (dissolveEnvelope);
    dissolveEditor.setVisible (layout.anyLayer && afterPeak == rs::AfterPeak::release
                               && (editable || dissolveEnvelope != rs::Envelope::dissolveDefault()));

    // Repaint only when something visible changed.
    juce::String state;
    state << note << " " << (int) mode << " " << (int) afterPeak << " " << viewStart << " " << viewEnd << " " << displayShift << " "
          << layout.peak << " " << layout.cut << " " << dissolveSamples << " " << juce::roundToInt (playhead / 64.0) << " "
          << shownSampleVersion << " " << auditionLayer << " " << (int) drag << " " << (int) snapped << " " << dissolveEnvelope.toString();

    for (int layer = 0; layer < rs::numLayers; ++layer)
    {
        const auto& l = layout.layers[(size_t) layer];
        const auto& s = settings[(size_t) layer];
        state << " " << (int) l.active << (int) l.cutAway << " " << l.start << " " << l.length << " " << l.fullLength << " "
              << (int) s.enabled << (int) s.reversed << " " << s.start << " " << s.end << " " << s.peakOffset << " "
              << layerEnvelopes[(size_t) layer].toString();
    }

    if (state != shownState)
    {
        shownState = state;
        updateEnvelopeBounds();
        repaint();
    }
}

void TimelineView::updateOnsets()
{
    for (int layer = 0; layer < rs::numLayers; ++layer)
    {
        const auto* sample = samples[(size_t) layer].get();
        const auto& t = layout.layers[(size_t) layer].timing;
        const auto reversed = settings[(size_t) layer].reversed;

        juce::String key;
        key << shownSampleVersion << " " << (sample != nullptr ? 1 : 0) << " " << t.regionStart << " " << t.regionEnd << " " << (int) reversed;

        if (key == onsetKeys[(size_t) layer])
            continue;

        onsetKeys[(size_t) layer] = key;
        auto& fractions = onsetFractions[(size_t) layer];
        fractions.clear();

        if (sample == nullptr || t.regionEnd <= t.regionStart)
            continue;

        // A reversed layer plays its region backwards: an attack in the file is where its swell lands.
        const auto regionLength = (double) (t.regionEnd - t.regionStart);

        for (const auto position : sample->onsets)
        {
            if (position <= t.regionStart || position >= t.regionEnd)
                continue;

            const auto fraction = (position - t.regionStart) / regionLength;
            fractions.push_back (reversed ? 1.0 - fraction : fraction);
        }

        std::sort (fractions.begin(), fractions.end());
    }
}

void TimelineView::updateViewRange()
{
    // From the note start (a trimmed start is not shown) over the builds (also the parts after
    // the PEAK), plus what comes after the peak.
    fullStart = 0.0;
    auto contentEnd = juce::jmax (layout.peak, layout.end);

    for (const auto& l : layout.layers)
        if (l.active || l.cutAway)
            contentEnd = juce::jmax (contentEnd, l.start + l.fullLength);

    const auto span = juce::jmax (0.05 * rate, contentEnd - fullStart);

    if (afterPeak == rs::AfterPeak::release)
        fullEnd = juce::jmax (fullStart + span * 1.06, layout.peak + dissolveSamples * 1.02);
    else
        fullEnd = fullStart + span * (afterPeak == rs::AfterPeak::cut ? 1.06 : 1.3);

    if (isZoomed())
    {
        const auto fullSpan = fullEnd - fullStart;

        if (zoomRange.getLength() >= fullSpan)
            zoomRange = {};
        else
            zoomRange = zoomRange.movedToStartAt (juce::jlimit (fullStart, fullEnd - zoomRange.getLength(), zoomRange.getStart()));
    }

    viewStart = isZoomed() ? zoomRange.getStart() : fullStart;
    viewEnd   = isZoomed() ? zoomRange.getEnd()   : fullEnd;
}

juce::String TimelineView::describe() const
{
    if (! layout.anyLayer)
    {
        for (const auto& l : layout.layers)
            if (l.cutAway)
                return "The PEAK lies before every build (Peak Move or a large peak offset), nothing plays. Double-click the PEAK flag to reset it.";

        return "Load a sample into a layer (or switch a layer on).";
    }

    const auto noteName = juce::MidiMessage::getMidiNoteName (note, true, true, 3);
    juce::String text;

    if (mode == rs::PlayMode::arrive)
        text << "Arrive: the peak lands on the note. Latency " << LayerText::seconds (layout.peak / rate)
             << " (the host plays the notes that much earlier).";
    else
        text << "Swell: the build starts with the key. Peak " << LayerText::seconds (layout.peak / rate) << " after the key.";

    if (layout.cut > 0.5)
        text << " Peak moved " << signedSeconds (-layout.cut / rate) << ".";

    for (int layer = 0; layer < rs::numLayers; ++layer)
    {
        const auto& l = layout.layers[(size_t) layer];
        const auto offset = settings[(size_t) layer].peakOffset;

        if (l.active)
            text << "   L" << (layer + 1) << ": " << (l.start > -0.5 ? "starts " + LayerText::seconds (juce::jmax (0.0, l.start) / rate)
                                                                 : "start trimmed " + LayerText::seconds (-l.start / rate))
                 << (std::abs (offset) >= 0.0005 ? " (peak " + signedSeconds (offset) + ")" : juce::String())
                 << (l.reachesPeak ? juce::String() : juce::String (" (ends before the PEAK)"));
        else if (l.cutAway)
            text << "   L" << (layer + 1) << (l.getEnd() <= 0.0 ? ": before the note start, silent" : ": after the PEAK, silent");
    }

    return text + "   (" + noteName + ")";
}

//==============================================================================
float TimelineView::timeToX (double samplesTime) const noexcept
{
    const auto proportion = (samplesTime - displayShift - viewStart) / juce::jmax (1.0, viewEnd - viewStart);
    return (float) plot.getX() + (float) proportion * (float) plot.getWidth();
}

double TimelineView::xToTime (float x) const noexcept
{
    const auto proportion = (double) (x - (float) plot.getX()) / juce::jmax (1.0, (double) plot.getWidth());
    return viewStart + proportion * (viewEnd - viewStart) + displayShift;
}

int TimelineView::getLayerAt (juce::Point<int> position) const
{
    for (int layer = 0; layer < rs::numLayers; ++layer)
        if (lanes[(size_t) layer].expanded (0, laneGap / 2).contains (position))
            return layer;

    return position.y < getHeight() / 2 ? 0 : 1;
}

int TimelineView::getLaneAt (juce::Point<int> position) const
{
    for (int layer = 0; layer < rs::numLayers; ++layer)
        if (lanes[(size_t) layer].contains (position))
            return layer;

    return -1;
}

void TimelineView::setDragHighlight (int layer, bool shouldHighlight)
{
    if (juce::isPositiveAndBelow (layer, rs::numLayers) && dragHighlight[(size_t) layer] != shouldHighlight)
    {
        dragHighlight[(size_t) layer] = shouldHighlight;
        repaint();
    }
}

juce::Rectangle<float> TimelineView::getPeakHandleBounds() const noexcept
{
    const auto top = 1.0f, bottom = (float) (plot.getY() - EnvelopeEditor::inset);
    return { timeToX (layout.peak) - 7.0f, top, 14.0f, juce::jmax (6.0f, bottom - top) };
}

bool TimelineView::isOnPeakHandle (juce::Point<float> position) const
{
    return layout.anyLayer && position.y < (float) plot.getY() && std::abs (position.x - timeToX (layout.peak)) <= handleHitPixels;
}

void TimelineView::resized()
{
    auto area = getLocalBounds();
    area.removeFromTop (topMargin);
    axis = area.removeFromBottom (axisHeight);
    plot = area;

    auto lanesArea = plot;
    const auto laneHeight = (lanesArea.getHeight() - laneGap) / rs::numLayers;

    for (auto& lane : lanes)
    {
        lane = lanesArea.removeFromTop (laneHeight);
        lanesArea.removeFromTop (laneGap);
    }

    updateEnvelopeBounds();
}

void TimelineView::updateEnvelopeBounds()
{
    if (plot.isEmpty() || ! layout.anyLayer)
        return;

    const auto margin = EnvelopeEditor::inset;   // the editor's own inset around its curve

    auto placeEditor = [&] (EnvelopeEditor& editor, double from, double to)
    {
        // Kept within a sane size when zoomed far in.
        const auto x0 = juce::jlimit (-100000, 100000, juce::roundToInt (timeToX (from)));
        const auto x1 = juce::jlimit (-100000, 100000, juce::roundToInt (timeToX (to)));
        editor.setBounds (juce::Rectangle<int>::leftTopRightBottom (x0 - margin, plot.getY() - margin,
                                                                     juce::jmax (x0 + 8, x1) + margin, plot.getBottom() + margin));
    };

    // The final envelope runs from the start of the earliest build to the peak; the Release fade from the peak on.
    placeEditor (envelopeEditor, layout.buildStart, layout.peak);
    placeEditor (dissolveEditor, layout.peak, layout.peak + dissolveSamples);
}

//==============================================================================
juce::RangedAudioParameter* TimelineView::getParameter (const juce::String& id) const
{
    return samplerProcessor.getValueTreeState().getParameter (id);
}

void TimelineView::setParameter (const juce::String& id, float value)
{
    if (auto* param = getParameter (id))
        param->setValueNotifyingHost (param->convertTo0to1 (value));
}

double TimelineView::anchorTime (int anchor) const noexcept
{
    if (anchor == peakAnchor)
        return layout.peak;

    if (juce::isPositiveAndBelow (anchor, rs::numLayers))
        return layout.layers[(size_t) anchor].start;

    return anchorTime0;
}

void TimelineView::beginAnchor (int anchor)
{
    anchorIndex = anchor;
    displayShift = 0.0;
    anchorTime0 = anchorTime (anchor);
}

std::vector<double> TimelineView::layerPoints (int layer) const
{
    // The start and the end of the whole build, and its transients (also those after the PEAK).
    std::vector<double> points;
    const auto& l = layout.layers[(size_t) layer];

    if (! l.active && ! l.cutAway)
        return points;

    points.push_back (l.start);
    points.push_back (l.getEnd());

    for (const auto fraction : onsetFractions[(size_t) layer])
        points.push_back (l.start + fraction * l.fullLength);

    return points;
}

double TimelineView::findSnap (double desired, const std::vector<double>& sources, const std::vector<double>& targets)
{
    const auto samplesPerPixel = (viewEnd - viewStart) / juce::jmax (1, plot.getWidth());
    auto best = desired, bestError = (double) snapPixels * samplesPerPixel;

    for (const auto source : sources)
    {
        for (const auto target : targets)
        {
            const auto candidate = target - source;

            if (const auto error = std::abs (candidate - desired); error <= bestError)
            {
                best = candidate;
                bestError = error;
                snapped = true;
                snapGuide = target;   // a display position: the targets stay still during the drag
            }
        }
    }

    return best;
}

void TimelineView::beginLayerDrag (int layer)
{
    if (drag != Drag::none || ! juce::isPositiveAndBelow (layer, rs::numLayers) || ! layout.layers[(size_t) layer].active)
        return;

    drag = Drag::layer;
    dragLayerIndex = layer;
    snapped = false;
    gestureMade = true;
    dragStartValue = settings[(size_t) layer].peakOffset;

    // The other layer stays still (with one layer: the common peak), the dragged one moves.
    int anchor = peakAnchor;

    for (int other = 0; other < rs::numLayers; ++other)
        if (other != layer && layout.layers[(size_t) other].active)
            anchor = other;

    beginAnchor (anchor);

    // Snap: the dragged layer's start, peak and transients onto the other layers' and the common peak.
    snapSources = layerPoints (layer);
    snapTargets.clear();

    for (int other = 0; other < rs::numLayers; ++other)
        if (other != layer)
            for (const auto point : layerPoints (other))
                snapTargets.push_back (point);

    snapTargets.push_back (layout.peak);
    snapTargets.push_back (0.0);   // the note start: a trimmed layer snaps back to start with the key

    // Moved earlier, the layer's start is trimmed at the note start; at least 50 ms of it stays.
    // (Moving it earlier does not move the PEAK: the natural peak stays as without this offset.)
    auto naturalPeak = layout.layers[(size_t) layer].fullLength;

    if (mode == rs::PlayMode::arrive)
        naturalPeak = layout.peak + layout.cut;
    else
        for (int other = 0; other < rs::numLayers; ++other)
            if (const auto& o = layout.layers[(size_t) other]; other != layer && (o.active || o.cutAway))
                naturalPeak = juce::jmax (naturalPeak, o.fullLength - juce::jmax (0.0, std::round (settings[(size_t) other].peakOffset * rate)));

    const auto keep = juce::jmin (layout.layers[(size_t) layer].fullLength, 0.05 * rate);
    minDragOffset = juce::jlimit (-8.0, 0.0, (keep - naturalPeak) / rate);

    if (auto* param = getParameter (ParamIDs::layerID (layer, ParamIDs::peakOffset)))
        param->beginChangeGesture();

    repaint();
}

void TimelineView::dragLayer (float deltaPixels, bool snap)
{
    if (drag != Drag::layer)
        return;

    const auto samplesPerPixel = (viewEnd - viewStart) / juce::jmax (1, plot.getWidth());
    auto delta = (double) deltaPixels * samplesPerPixel;
    snapped = false;

    if (snap)
        delta = findSnap (delta, snapSources, snapTargets);

    const auto offset = juce::jlimit (juce::jmin (minDragOffset, dragStartValue), 8.0, dragStartValue + delta / rate);
    setParameter (ParamIDs::layerID (dragLayerIndex, ParamIDs::peakOffset), (float) offset);
    sync();
}

void TimelineView::endLayerDrag()
{
    if (drag != Drag::layer)
        return;

    if (auto* param = getParameter (ParamIDs::layerID (dragLayerIndex, ParamIDs::peakOffset)))
        param->endChangeGesture();

    // Zoomed: the view follows the anchor, so nothing jumps when the drag ends.
    if (isZoomed())
        zoomRange += displayShift;

    drag = Drag::none;
    dragLayerIndex = -1;
    anchorIndex = noAnchor;
    displayShift = 0.0;
    snapped = false;
    samplerProcessor.updateLatency();   // Arrive: the latency follows at once
    shownState.clear();
    sync();
}

void TimelineView::resetLayerOffset (int layer)
{
    if (! juce::isPositiveAndBelow (layer, rs::numLayers))
        return;

    if (auto* param = getParameter (ParamIDs::layerID (layer, ParamIDs::peakOffset)))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        param->endChangeGesture();
    }

    samplerProcessor.updateLatency();
    sync();
}

void TimelineView::beginPeakDrag()
{
    if (drag != Drag::none || ! layout.anyLayer)
        return;

    drag = Drag::peak;
    snapped = false;
    gestureMade = true;
    dragStartValue = samplerProcessor.getPeakCutSeconds();
    dragPeak0 = layout.peak;

    // The layers stay still (anchored on one of them), the peak line moves.
    int anchor = peakAnchor;
    double earliestStart = layout.peak;

    for (int layer = rs::numLayers; --layer >= 0;)
    {
        if (const auto& l = layout.layers[(size_t) layer]; l.active || l.cutAway)
        {
            anchor = layer;
            earliestStart = juce::jmin (earliestStart, juce::jmax (0.0, l.start));
        }
    }

    beginAnchor (anchor);

    // At least 10 ms of the earliest build (from the note start) stays before the PEAK.
    const auto naturalPeak = layout.peak + layout.cut;
    maxCut = juce::jlimit (0.0, (double) rs::maxArriveSeconds, (naturalPeak - earliestStart - 0.01 * rate) / rate);

    // Snap: onto every transient (also after the PEAK), the end of each build and the natural peak.
    snapSources = { layout.peak };
    snapTargets = { naturalPeak };

    for (int layer = 0; layer < rs::numLayers; ++layer)
    {
        const auto points = layerPoints (layer);

        for (size_t i = 1; i < points.size(); ++i)   // not the start of a build
            snapTargets.push_back (points[i]);
    }

    if (auto* param = getParameter (ParamIDs::peakCut))
        param->beginChangeGesture();

    repaint();
}

void TimelineView::dragPeak (float deltaPixels, bool snap)
{
    if (drag != Drag::peak)
        return;

    const auto samplesPerPixel = (viewEnd - viewStart) / juce::jmax (1, plot.getWidth());
    auto delta = (double) deltaPixels * samplesPerPixel;
    snapped = false;

    if (snap)
        delta = findSnap (delta, snapSources, snapTargets);

    // Moving the peak to the left cuts the builds sooner.
    const auto cut = juce::jlimit (0.0, maxCut, dragStartValue - delta / rate);
    setParameter (ParamIDs::peakCut, (float) cut);
    sync();
}

void TimelineView::endPeakDrag()
{
    if (drag != Drag::peak)
        return;

    if (auto* param = getParameter (ParamIDs::peakCut))
        param->endChangeGesture();

    if (isZoomed())
        zoomRange += displayShift;

    drag = Drag::none;
    anchorIndex = noAnchor;
    displayShift = 0.0;
    snapped = false;
    samplerProcessor.updateLatency();
    shownState.clear();
    sync();
}

void TimelineView::resetPeak()
{
    if (auto* param = getParameter (ParamIDs::peakCut))
    {
        param->beginChangeGesture();
        param->setValueNotifyingHost (param->convertTo0to1 (0.0f));
        param->endChangeGesture();
    }

    samplerProcessor.updateLatency();
    sync();
}

//==============================================================================
void TimelineView::zoomAround (float x, float factor)
{
    const auto fullSpan = fullEnd - fullStart;
    const auto span = viewEnd - viewStart;
    const auto minSpan = juce::jmin (fullSpan, 0.005 * rate);
    const auto newSpan = juce::jlimit (minSpan, fullSpan, span * (double) factor);

    if (newSpan >= fullSpan * 0.999)
    {
        resetZoom();
        return;
    }

    const auto proportion = juce::jlimit (0.0, 1.0, (double) (x - (float) plot.getX()) / juce::jmax (1.0, (double) plot.getWidth()));
    const auto pivot = viewStart + proportion * span;
    const auto start = juce::jlimit (fullStart, fullEnd - newSpan, pivot - proportion * newSpan);

    zoomRange = { start, start + newSpan };
    viewStart = zoomRange.getStart();
    viewEnd = zoomRange.getEnd();
    updateEnvelopeBounds();
    repaint();
}

void TimelineView::scrollBy (double fractionOfView)
{
    if (! isZoomed())
        return;

    const auto span = zoomRange.getLength();
    zoomRange = zoomRange.movedToStartAt (juce::jlimit (fullStart, fullEnd - span, zoomRange.getStart() + fractionOfView * span));
    viewStart = zoomRange.getStart();
    viewEnd = zoomRange.getEnd();
    updateEnvelopeBounds();
    repaint();
}

void TimelineView::resetZoom()
{
    zoomRange = {};

    if (drag == Drag::none)
        updateViewRange();
    else
    {
        viewStart = fullStart;
        viewEnd = fullEnd;
    }

    updateEnvelopeBounds();
    repaint();
}

//==============================================================================
void TimelineView::mouseDown (const juce::MouseEvent& e)
{
    gestureMade = false;
    dragMouseX = e.position.x;
    dragMouseY = e.position.y;

    if (! layout.anyLayer || drag != Drag::none)
        return;

    if (isOnPeakHandle (e.position))
    {
        beginPeakDrag();
        return;
    }

    const auto layer = getLaneAt (e.getPosition());

    if (layer < 0)
        return;

    if (e.mods.isAltDown() && ! e.mods.isShiftDown())
    {
        // Alt-click and hold: this layer alone, as it sounds in the full sound.
        drag = Drag::audition;
        dragLayerIndex = layer;
        gestureMade = true;
        samplerProcessor.startAudition (layer);
        repaint();
        return;
    }

    if (e.mods.isShiftDown())
        beginLayerDrag (layer);
}

void TimelineView::mouseDrag (const juce::MouseEvent& e)
{
    dragMouseX = e.position.x;
    dragMouseY = e.position.y;
    const auto dx = (float) e.getDistanceFromDragStartX();

    if (drag == Drag::layer)
        dragLayer (dx, ! e.mods.isAltDown());
    else if (drag == Drag::peak)
        dragPeak (dx, ! e.mods.isAltDown());
}

void TimelineView::mouseUp (const juce::MouseEvent& e)
{
    switch (drag)
    {
        case Drag::layer:  endLayerDrag(); break;
        case Drag::peak:   endPeakDrag(); break;
        case Drag::audition:
            samplerProcessor.stopAudition();
            drag = Drag::none;
            dragLayerIndex = -1;
            repaint();
            break;
        case Drag::none:
        default:
            break;
    }

    if (! editable && onClick != nullptr && e.mouseWasClicked() && ! gestureMade && ! e.mods.isShiftDown() && ! e.mods.isAltDown())
        onClick();
}

void TimelineView::mouseDoubleClick (const juce::MouseEvent& e)
{
    if (isOnPeakHandle (e.position))
    {
        endPeakDrag();
        resetPeak();
        return;
    }

    if (axis.contains (e.getPosition()))
    {
        resetZoom();
        return;
    }

    if (const auto layer = getLaneAt (e.getPosition()); layer >= 0 && e.mods.isShiftDown())
    {
        endLayerDrag();
        resetLayerOffset (layer);
    }
}

void TimelineView::mouseMove (const juce::MouseEvent& e)
{
    setMouseCursor (isOnPeakHandle (e.position) ? juce::MouseCursor::LeftRightResizeCursor
                                                : (editable ? juce::MouseCursor::NormalCursor : juce::MouseCursor::PointingHandCursor));
}

void TimelineView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const auto amount = std::abs (wheel.deltaY) >= std::abs (wheel.deltaX) ? wheel.deltaY : wheel.deltaX;

    if (e.mods.isCommandDown())
        zoomAround (e.position.x, (float) std::exp (-1.5 * (double) amount));
    else if (isZoomed())
        scrollBy (-0.5 * (double) amount);
    else
        juce::Component::mouseWheelMove (e, wheel);
}

//==============================================================================
void TimelineView::paint (juce::Graphics& g)
{
    for (int layer = 0; layer < rs::numLayers; ++layer)
        paintLane (g, layer);

    if (! layout.anyLayer)
        return;

    // Arrive: what lies before the note-on cannot be played.
    if (const auto zeroX = timeToX (0.0); zeroX > (float) plot.getX())
    {
        const auto x = juce::jmin (zeroX, (float) plot.getRight());
        g.setColour (Theme::background.withAlpha (0.65f));
        g.fillRect (juce::Rectangle<float> ((float) plot.getX(), (float) plot.getY(), x - (float) plot.getX(), (float) plot.getHeight()));
        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("cut: longer than the latency", juce::Rectangle<float> ((float) plot.getX(), 0.0f, x - (float) plot.getX(), (float) topMargin),
                    juce::Justification::centred, true);
    }

    // Where the peak would be without the cut.
    if (layout.cut > 0.5)
    {
        g.setColour (Theme::dimText.withAlpha (0.7f));
        drawDashedVertical (g, timeToX (layout.peak + layout.cut), (float) plot.getY(), (float) plot.getBottom());
    }

    // The peak, with its flag to drag.
    const auto peakX = timeToX (layout.peak);
    const auto handle = getPeakHandleBounds();
    g.setColour (Theme::text.withAlpha (0.8f));
    g.drawLine (peakX, handle.getBottom(), peakX, (float) plot.getBottom(), 1.5f);
    g.setColour (drag == Drag::peak ? Theme::warning : Theme::text);
    g.fillRoundedRectangle (handle, 3.0f);
    g.setColour (Theme::background);

    for (auto dx : { -2.5f, 0.0f, 2.5f })
        g.drawVerticalLine (juce::roundToInt (handle.getCentreX() + dx), handle.getY() + 3.0f, handle.getBottom() - 3.0f);

    juce::String peakText (mode == rs::PlayMode::arrive ? "NOTE / PEAK" : "PEAK");

    if (layout.cut > 0.5)
        peakText << " " << signedSeconds (-layout.cut / rate);

    g.setColour (Theme::text.withAlpha (0.85f));
    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.drawText (peakText, juce::Rectangle<float> (handle.getX() - 164.0f, 0.0f, 160.0f, (float) topMargin), juce::Justification::centredRight);

    const char* afterText = afterPeak == rs::AfterPeak::cut      ? "CUT"
                          : afterPeak == rs::AfterPeak::release  ? "RELEASE (fades out)"
                          : afterPeak == rs::AfterPeak::freeze   ? "FREEZE (while held)"
                                                                 : "LOOP (while held)";
    g.setColour (Theme::dimText);
    g.setFont (juce::FontOptions (11.0f));
    g.drawText (afterText, juce::Rectangle<float> (handle.getRight() + 4.0f, 0.0f, juce::jmax (0.0f, (float) plot.getRight() - handle.getRight() - 4.0f),
                                                   (float) topMargin),
                juce::Justification::centredLeft, true);

    paintAxis (g);
    paintDragInfo (g);

    if (playhead >= 0.0)
    {
        const auto x = timeToX (playhead);

        if (x >= (float) plot.getX() && x <= (float) plot.getRight())
        {
            g.setColour (juce::Colours::white);
            g.drawLine (x, (float) plot.getY(), x, (float) plot.getBottom(), 1.5f);
        }
    }
}

void TimelineView::paintLane (juce::Graphics& g, int layer)
{
    const auto lane = lanes[(size_t) layer].toFloat();
    const auto& s = settings[(size_t) layer];
    const auto& l = layout.layers[(size_t) layer];
    const auto* sample = samples[(size_t) layer].get();
    const auto layerColour = Theme::layerColour (layer);
    const auto waveColour = ! s.enabled ? Theme::disabled : (s.reversed ? Theme::reversed : layerColour);
    const auto soloing = auditionLayer == layer;
    const auto highlighted = dragHighlight[(size_t) layer] || soloing || (drag == Drag::layer && dragLayerIndex == layer);

    g.setColour (Theme::background.withAlpha (0.6f));
    g.fillRoundedRectangle (lane, 6.0f);
    g.setColour (highlighted ? layerColour : Theme::border);
    g.drawRoundedRectangle (lane.reduced (0.5f), 6.0f, highlighted ? 2.0f : 1.0f);

    juce::String caption ("LAYER " + juce::String (layer + 1));

    if (std::abs (s.peakOffset) >= 0.0005)
        caption << "   peak " << signedSeconds (s.peakOffset);

    if (l.active && l.start < -0.5)
        caption << "   start trimmed " << LayerText::seconds (-l.start / rate);

    if (soloing)
        caption << "   SOLO";

    g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
    g.setColour (s.enabled || soloing ? layerColour : Theme::disabled);
    g.drawText (caption, lane.reduced (8.0f, 4.0f), juce::Justification::topLeft);

    if (layer == 0 && isZoomed())
    {
        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (10.5f));
        g.drawText ("ZOOM x" + juce::String ((fullEnd - fullStart) / juce::jmax (1.0, viewEnd - viewStart), 1) + "  (double-click the time axis to see all)",
                    lane.reduced (8.0f, 4.0f), juce::Justification::topRight);
    }

    if (sample == nullptr || (! l.active && ! l.cutAway))
    {
        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText (sample == nullptr ? "Layer " + juce::String (layer + 1) + " is empty. Drop an audio file here."
                                      : "Layer " + juce::String (layer + 1) + " is off.",
                    lane, juce::Justification::centred);
        return;
    }

    const auto numSamples = sample->audio.getNumSamples();
    const auto numChannels = sample->audio.getNumChannels();
    const auto numBins = (int) sample->overviewMax.size();
    const auto samplesPerBin = (double) numSamples / juce::jmax (1, numBins);
    const auto peakLevel = juce::jmax (1.0e-6f, sample->overviewPeak);
    const auto centreY = lane.getCentreY() + 4.0f;
    const auto halfHeight = lane.getHeight() * 0.5f - 10.0f;
    const auto regionStart = (double) l.timing.regionStart, regionLength = (double) (l.timing.regionEnd - l.timing.regionStart);
    const auto& layerEnvelope = layerEnvelopes[(size_t) layer];

    // The whole build runs from x0 to xFull; the PEAK (x1) is where every layer stops (Cut), holds
    // (Freeze), restarts (Loop) or starts to fade (Release).
    const auto x0 = timeToX (l.start), x1 = timeToX (layout.peak), xFull = timeToX (l.start + l.fullLength);
    const auto width = juce::jmax (1.0f, xFull - x0);
    const auto playedFraction = l.fullLength > 0.0 ? l.length / l.fullLength : 0.0;
    const auto releasing = afterPeak == rs::AfterPeak::release;
    const auto fadeEndsSilent = dissolveEnvelope.y[(size_t) (dissolveEnvelope.numPoints - 1)] <= 1.0e-4f;

    // Source position (forward samples) at a fraction of the whole build.
    auto sourceAt = [&] (double fraction) { return s.reversed ? regionStart + regionLength * (1.0 - fraction) : regionStart + regionLength * fraction; };

    // Min and max of the source between two positions: from the overview, or from the samples themselves when zoomed in.
    auto rangeBetween = [&] (double a, double b, float& low, float& high)
    {
        if (a > b)
            std::swap (a, b);

        if (b - a < 2.0 * samplesPerBin)
        {
            const auto i0 = juce::jlimit (0, numSamples - 1, (int) std::floor (a));
            const auto i1 = juce::jlimit (i0, numSamples - 1, (int) std::ceil (b));
            low = high = sample->audio.getSample (0, i0);

            for (int channel = 0; channel < numChannels; ++channel)
            {
                for (int i = i0; i <= i1; ++i)
                {
                    const auto v = sample->audio.getSample (channel, i);
                    low = juce::jmin (low, v);
                    high = juce::jmax (high, v);
                }
            }

            return;
        }

        const auto b0 = juce::jlimit (0, numBins - 1, (int) (a / samplesPerBin));
        const auto b1 = juce::jlimit (b0, numBins - 1, (int) (b / samplesPerBin));
        low = high = 0.0f;

        for (int bin = b0; bin <= b1; ++bin)
        {
            low  = juce::jmin (low,  sample->overviewMin[(size_t) bin]);
            high = juce::jmax (high, sample->overviewMax[(size_t) bin]);
        }
    };

    if (numBins > 0 && numSamples > 0)
    {
        const auto first = juce::jmax ((int) std::floor (x0), plot.getX());
        const auto last  = juce::jmin ((int) std::ceil (xFull), plot.getRight());

        for (int px = first; px < last; ++px)
        {
            const auto f0 = juce::jlimit (0.0, 1.0, ((double) px - x0) / width);
            const auto f1 = juce::jlimit (0.0, 1.0, ((double) px + 1.0 - x0) / width);
            float low = 0.0f, high = 0.0f;
            rangeBetween (sourceAt (f0), sourceAt (f1), low, high);

            const auto top = centreY - high / peakLevel * halfHeight, bottom = centreY - low / peakLevel * halfHeight;
            auto gain = l.active ? layerEnvelope.valueAt ((float) (0.5 * (f0 + f1))) : 0.0f;

            if ((float) px + 0.5f >= x1)
            {
                // After the PEAK only Release plays on, fading along the drawn curve.
                const auto progress = (xToTime ((float) px + 0.5f) - layout.peak) / juce::jmax (1.0, dissolveSamples);
                gain = releasing && ! (progress >= 1.0 && fadeEndsSilent)
                           ? gain * dissolveEnvelope.valueAt ((float) juce::jlimit (0.0, 1.0, progress)) : 0.0f;
            }

            if (gain <= 0.0f)
            {
                // Not heard: very dim.
                g.setColour (waveColour.withAlpha (0.1f));
                g.drawVerticalLine (px, top, juce::jmax (top + 1.0f, bottom));
                continue;
            }

            g.setColour (waveColour.withAlpha (0.18f));
            g.drawVerticalLine (px, top, juce::jmax (top + 1.0f, bottom));
            g.setColour (waveColour.withAlpha (0.9f));
            const auto gainedTop = centreY + (top - centreY) * gain, gainedBottom = centreY + (bottom - centreY) * gain;
            g.drawVerticalLine (px, gainedTop, juce::jmax (gainedTop + 1.0f, gainedBottom));
        }
    }

    // A trimmed start: a jagged edge where the layer starts to sound, at the note start.
    if (l.active && l.start < -0.5)
    {
        const auto x = timeToX (0.0);

        if (x >= (float) plot.getX() - 1.0f && x <= (float) plot.getRight())
        {
            juce::Path edge;
            const auto top = centreY - halfHeight, bottom = centreY + halfHeight;
            constexpr int teeth = 8;
            edge.startNewSubPath (x, top);

            for (int i = 1; i <= teeth; ++i)
                edge.lineTo (x + ((i % 2) != 0 ? 4.0f : 0.0f), top + (bottom - top) * (float) i / (float) teeth);

            g.setColour (Theme::text.withAlpha (0.8f));
            g.strokePath (edge, juce::PathStrokeType (1.5f));
        }
    }

    if (! l.active)
    {
        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (13.0f));
        g.drawText ("Layer " + juce::String (layer + 1) + (l.getEnd() <= 0.0 ? " is moved before the note start: silent" : " lies after the PEAK: silent"),
                    lane, juce::Justification::centred);
        return;
    }

    // Freeze holds the layers that still sound at the PEAK, at about the level they have there.
    if (afterPeak == rs::AfterPeak::freeze && l.reachesPeak)
    {
        float holdLevel = 2.0f;

        if (numBins > 0 && numSamples > 0)
        {
            float low = 0.0f, high = 0.0f;
            const auto window = juce::jmin (playedFraction, 0.01);
            rangeBetween (sourceAt (playedFraction - window), sourceAt (playedFraction), low, high);
            const auto level = 0.5f * (high - low) / peakLevel * halfHeight * layerEnvelope.valueAt ((float) playedFraction);
            holdLevel = juce::jmax (2.0f, level * 0.7f);
        }

        g.saveState();
        g.reduceClipRegion (plot);
        g.setColour (waveColour.withAlpha (0.3f));
        g.fillRect (juce::Rectangle<float> (x1, centreY - holdLevel, juce::jmax (0.0f, (float) plot.getRight() - x1), 2.0f * holdLevel));
        g.restoreState();
    }

    // The end of this layer's build (its own natural peak), when its peak offset moves it.
    if (std::abs (s.peakOffset) >= 0.0005)
    {
        const auto xEnd = timeToX (l.getEnd());
        g.setColour (waveColour);
        juce::Path marker;
        marker.addTriangle (xEnd - 5.0f, lane.getY() + 2.0f, xEnd + 5.0f, lane.getY() + 2.0f, xEnd, lane.getY() + 10.0f);
        g.fillPath (marker);
    }
}

void TimelineView::paintDragInfo (juce::Graphics& g)
{
    const auto dragging = drag == Drag::layer || drag == Drag::peak;

    // Transients: faint ticks in the editor, bright while dragging.
    if (editable || dragging)
    {
        for (int layer = 0; layer < rs::numLayers; ++layer)
        {
            const auto& l = layout.layers[(size_t) layer];

            if (! l.active && ! l.cutAway)
                continue;

            const auto lane = lanes[(size_t) layer].toFloat();
            g.setColour (Theme::layerColour (layer).withAlpha (dragging ? 0.95f : 0.4f));

            for (const auto fraction : onsetFractions[(size_t) layer])
            {
                const auto x = timeToX (l.start + fraction * l.fullLength);

                if (x >= (float) plot.getX() && x <= (float) plot.getRight())
                    g.fillRect (x - 0.75f, lane.getBottom() - (dragging ? 12.0f : 7.0f), 1.5f, dragging ? 10.0f : 5.0f);
            }
        }
    }

    if (! dragging)
        return;

    if (snapped)
    {
        const auto x = (float) plot.getX() + (float) ((snapGuide - viewStart) / juce::jmax (1.0, viewEnd - viewStart)) * (float) plot.getWidth();
        g.setColour (Theme::warning.withAlpha (0.9f));
        drawDashedVertical (g, x, (float) plot.getY(), (float) plot.getBottom());
    }

    juce::String text;

    if (drag == Drag::layer)
        text << "LAYER " << (dragLayerIndex + 1) << " peak " << signedSeconds (settings[(size_t) juce::jlimit (0, rs::numLayers - 1, dragLayerIndex)].peakOffset);
    else
        text << "PEAK " << (layout.cut > 0.5 ? signedSeconds (-layout.cut / rate) : juce::String ("natural"));

    if (snapped)
        text << "  (snap)";

    g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
    const auto box = juce::Rectangle<float> (juce::GlyphArrangement::getStringWidth (g.getCurrentFont(), text) + 16.0f, 20.0f)
                         .withPosition (dragMouseX + 12.0f, dragMouseY - 28.0f)
                         .constrainedWithin (plot.toFloat());
    g.setColour (Theme::panelLight.withAlpha (0.95f));
    g.fillRoundedRectangle (box, 4.0f);
    g.setColour (snapped ? Theme::warning : Theme::text);
    g.drawText (text, box, juce::Justification::centred);
}

void TimelineView::paintAxis (juce::Graphics& g)
{
    const auto origin = mode == rs::PlayMode::arrive ? layout.peak : 0.0;   // Arrive: seconds to the note
    const auto visibleStart = viewStart + displayShift, visibleEnd = viewEnd + displayShift;
    const auto secondsPerPixel = (visibleEnd - visibleStart) / rate / juce::jmax (1, plot.getWidth());
    double step = 0.001;

    for (auto candidate : { 0.001, 0.002, 0.005, 0.01, 0.02, 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 20.0, 30.0, 60.0 })
    {
        step = candidate;

        if (candidate / secondsPerPixel >= 70.0)
            break;
    }

    const auto decimals = step < 0.01 ? 3 : (step < 1.0 ? 2 : 0);
    g.setFont (juce::FontOptions (10.5f));

    const auto firstTick = std::ceil ((visibleStart - origin) / rate / step) * step;

    for (auto t = firstTick; t * rate + origin <= visibleEnd; t += step)
    {
        const auto x = timeToX (t * rate + origin);
        g.setColour (Theme::border);
        g.drawVerticalLine (juce::roundToInt (x), (float) axis.getY(), (float) axis.getY() + 4.0f);
        g.setColour (Theme::dimText);
        const auto label = (std::abs (t) < 1.0e-9 ? juce::String ("0") : juce::String (t, decimals)) + " s";
        auto labelArea = juce::Rectangle<float> (60.0f, (float) axis.getHeight()).withCentre ({ x, (float) axis.getCentreY() + 2.0f });
        labelArea.setX (juce::jlimit ((float) axis.getX() - 10.0f, (float) axis.getRight() - 50.0f, labelArea.getX()));
        g.drawText (label, labelArea, juce::Justification::centred);
    }
}
