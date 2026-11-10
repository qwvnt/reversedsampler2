#include "LayerEditor.h"
#include "LayerText.h"

namespace
{
    constexpr int groupTitleHeight = 20;
    constexpr int groupPadding = 6;
}

LayerEditor::LayerEditor (ReverseSamplerProcessor& p, int layerIndex)
    : samplerProcessor (p),
      layer (layerIndex),
      colour (Theme::layerColour (layerIndex)),
      onButton (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::on), "ON", Theme::layerColour (layerIndex),
                "Switch the layer on or off"),
      reverseButton (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::reverse), "R", Theme::reversed,
                     "Reverse: play the layer backwards"),
      rootKnob      (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::root),    "Root",     Theme::layerColour (layerIndex)),
      startKnob     (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::start),   "Start",    Theme::layerColour (layerIndex)),
      endKnob       (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::end),     "End",      Theme::layerColour (layerIndex)),
      timeModeBox   (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::timeMode),   "Mode"),
      lengthModeBox (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::lengthMode), "Length"),
      secondsKnob   (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::seconds), "Seconds",  Theme::layerColour (layerIndex)),
      beatsKnob     (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::beats),   "Beats",    Theme::layerColour (layerIndex)),
      offsetKnob    (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::peakOffset), "Peak offset", Theme::layerColour (layerIndex)),
      pitchKnob     (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::pitch),   "Pitch",    Theme::layerColour (layerIndex)),
      levelKnob     (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::level),   "Level",    Theme::layerColour (layerIndex)),
      panKnob       (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::pan),     "Pan",      Theme::layerColour (layerIndex)),
      widthKnob     (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::width),   "Width",    Theme::layerColour (layerIndex)),
      lowCutKnob    (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::lowCut),  "Low Cut",  Theme::layerColour (layerIndex)),
      highCutKnob   (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::highCut), "High Cut", Theme::layerColour (layerIndex))
{
    for (int i = 0; i < rs::numLayers; ++i)
    {
        auto& tab = layerTabs[(size_t) i];
        tab.setButtonText ("LAYER " + juce::String (i + 1));
        tab.setToggleState (i == layer, juce::dontSendNotification);
        tab.setColour (juce::TextButton::buttonOnColourId, Theme::layerColour (i));
        tab.setColour (juce::TextButton::textColourOnId, Theme::background);
        tab.setConnectedEdges (i == 0 ? juce::Button::ConnectedOnRight : juce::Button::ConnectedOnLeft);
        tab.setTooltip ("Show the settings of layer " + juce::String (i + 1));
        tab.onClick = [this, i]
        {
            layerTabs[(size_t) i].setToggleState (i == layer, juce::dontSendNotification);

            if (i != layer && onShowLayer != nullptr)
                onShowLayer (i);
        };
        addAndMakeVisible (tab);
    }

    for (auto* button : { &onButton, &reverseButton })
        addAndMakeVisible (*button);

    loadButton.setTooltip ("Load an audio file into this layer");
    loadButton.onClick = [this] { if (onBrowse != nullptr) onBrowse (layer); };
    clearButton.setTooltip ("Remove the sample and reset every setting of this layer, and its envelope, to the default");
    clearButton.onClick = [this] { samplerProcessor.resetLayer (layer); };
    doneButton.setTooltip ("Back to both layers");
    doneButton.onClick = [this] { if (onClose != nullptr) onClose(); };

    for (auto* button : { &loadButton, &clearButton, &doneButton })
        addAndMakeVisible (*button);

    infoLabel.setFont (juce::FontOptions (13.5f));
    infoLabel.setColour (juce::Label::textColourId, Theme::dimText);
    timingLabel.setFont (juce::FontOptions (13.5f));
    timingLabel.setColour (juce::Label::textColourId, Theme::text);
    warningLabel.setFont (juce::FontOptions (13.5f, juce::Font::bold));
    warningLabel.setColour (juce::Label::textColourId, Theme::warning);
    warningLabel.setJustificationType (juce::Justification::centredRight);

    for (auto* label : { &infoLabel, &timingLabel, &warningLabel })
    {
        label->setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (*label);
    }

    // Waveform with draggable Start/End markers.
    auto& state = p.getValueTreeState();
    auto makeMarkerAttachment = [&] (const char* name, float& value)
    {
        return std::make_unique<juce::ParameterAttachment> (*state.getParameter (ParamIDs::layerID (layer, name)),
                                                            [this, &value] (float newValue)
                                                            {
                                                                value = newValue;
                                                                waveform.setRegion (startPercent * 0.01f, endPercent * 0.01f);
                                                            });
    };

    startAttachment = makeMarkerAttachment (ParamIDs::start, startPercent);
    endAttachment   = makeMarkerAttachment (ParamIDs::end, endPercent);

    waveform.setLayerColour (colour);
    waveform.setEditable (true);
    waveform.setEmptyText ("Layer " + juce::String (layer + 1) + " is empty\nDrop an audio file here or click to browse");
    waveform.onClick = [this] { if (sample == nullptr && onBrowse != nullptr) onBrowse (layer); };
    waveform.onPressed  = [this] { samplerProcessor.startAudition (layer); };   // hold to listen to this layer alone
    waveform.onReleased = [this] { samplerProcessor.stopAudition(); };
    waveform.setHint ("Click and hold the waveform: this layer alone, as it sounds in the full sound");
    waveform.onMarkerDragStarted = [this] (WaveformView::Marker m)          { getMarkerAttachment (m).beginGesture(); };
    waveform.onMarkerDragged     = [this] (WaveformView::Marker m, float f) { getMarkerAttachment (m).setValueAsPartOfGesture (f * 100.0f); };
    waveform.onMarkerDragEnded   = [this] (WaveformView::Marker m)          { getMarkerAttachment (m).endGesture(); };
    addAndMakeVisible (waveform);

    startAttachment->sendInitialUpdate();
    endAttachment->sendInitialUpdate();

    // The layer's volume envelope: start of the build -> its peak.
    envelopeLabel.setText ("VOLUME ENVELOPE (start -> peak): click to add a point, drag to move, double-click to delete, "
                               + juce::String (KeyNames::command) + "-click to reset a point",
                           juce::dontSendNotification);
    envelopeLabel.setFont (juce::FontOptions (11.5f, juce::Font::bold));
    envelopeLabel.setColour (juce::Label::textColourId, Theme::dimText);
    addAndMakeVisible (envelopeLabel);

    flatButton.setTooltip ("Reset the envelope to flat");
    flatButton.onClick = [this] { envelopeEditor.resetToFlat(); };
    addAndMakeVisible (flatButton);

    envelopeEditor.setCurveColour (colour);
    envelopeEditor.setEnvelope (samplerProcessor.getEnvelope (layer));
    envelopeEditor.onChange = [this] (const rs::Envelope& e) { samplerProcessor.setEnvelope (layer, e); };
    envelopeEditor.paintBackground = [this] (juce::Graphics& g, juce::Rectangle<float> plotArea) { paintEnvelopeBackground (g, plotArea); };
    addAndMakeVisible (envelopeEditor);

    // Knobs.
    rootKnob.setTooltip ("The note that plays the sample at its original pitch (type a note name, e.g. C3)");
    startKnob.setTooltip ("Region start in the file (drag S on the waveform)");
    endKnob.setTooltip ("Region end in the file (drag E on the waveform)");
    timeModeBox.setTooltip ("Stretch: pitch and length are independent.  Resample: like tape, the pitch changes the length");
    lengthModeBox.setTooltip ("How long the layer plays in Stretch mode: as the sample, in seconds or in beats of the host tempo");
    secondsKnob.setTooltip ("Length in seconds, the same on every note");
    beatsKnob.setTooltip ("Length in beats of the host tempo, the same on every note");
    offsetKnob.setTooltip ("Moves this layer: + later, - earlier (0 = it peaks at the PEAK). Moved before the note start, its start is trimmed");
    pitchKnob.setTooltip ("Transpose the layer in semitones");
    levelKnob.setTooltip ("Layer volume");
    panKnob.setTooltip ("Layer balance");
    widthKnob.setTooltip ("Stereo width: 0 % = mono, 100 % = as recorded, 200 % = extra wide");
    lowCutKnob.setTooltip ("High-pass filter (Off at 20 Hz)");
    highCutKnob.setTooltip ("Low-pass filter (Off at 20 kHz)");

    for (auto* knob : { &rootKnob, &startKnob, &endKnob, &secondsKnob, &beatsKnob, &offsetKnob, &pitchKnob,
                        &levelKnob, &panKnob, &widthKnob, &lowCutKnob, &highCutKnob })
        addAndMakeVisible (*knob);

    modeColumn.addAndMakeVisible (timeModeBox);
    modeColumn.addAndMakeVisible (lengthModeBox);
    addAndMakeVisible (modeColumn);

    sync();
}

LayerEditor::~LayerEditor()
{
    if (waveform.isPressed())
        samplerProcessor.stopAudition();
}

juce::ParameterAttachment& LayerEditor::getMarkerAttachment (WaveformView::Marker marker)
{
    return marker == WaveformView::startMarker ? *startAttachment : *endAttachment;
}

void LayerEditor::paintEnvelopeBackground (juce::Graphics& g, juce::Rectangle<float> plotArea)
{
    g.setColour (Theme::background.withAlpha (0.7f));
    g.fillRect (plotArea);

    if (sample == nullptr || sample->overviewMax.empty())
        return;

    // The region in play order (a reversed layer plays it backwards), dim, under the curve.
    const auto numBins = (int) sample->overviewMax.size();
    const auto peakLevel = juce::jmax (1.0e-6f, sample->overviewPeak);
    const auto s0 = juce::jlimit (0.0, 1.0, shownSettings.start), s1 = juce::jlimit (s0, 1.0, shownSettings.end);
    const auto centreY = plotArea.getCentreY(), halfHeight = plotArea.getHeight() * 0.5f;
    const auto waveColour = ! shownSettings.enabled ? Theme::disabled : (shownSettings.reversed ? Theme::reversed : colour);
    const auto x0 = (int) plotArea.getX(), width = juce::jmax (1, (int) plotArea.getWidth());

    auto binAt = [&] (double fraction)
    {
        const auto position = shownSettings.reversed ? s1 - (s1 - s0) * fraction : s0 + (s1 - s0) * fraction;
        return juce::jlimit (0, numBins - 1, (int) (position * numBins));
    };

    g.setColour (waveColour.withAlpha (0.25f));

    for (int px = 0; px < width; ++px)
    {
        auto b0 = binAt ((double) px / width), b1 = binAt ((double) (px + 1) / width);

        if (b0 > b1)
            std::swap (b0, b1);

        float low = 0.0f, high = 0.0f;

        for (int b = b0; b <= b1; ++b)
        {
            low  = juce::jmin (low,  sample->overviewMin[(size_t) b]);
            high = juce::jmax (high, sample->overviewMax[(size_t) b]);
        }

        g.drawVerticalLine (x0 + px, centreY - high / peakLevel * halfHeight, centreY - low / peakLevel * halfHeight);
    }

    // The start trimmed at the note start (the layer is moved earlier than the key): not played.
    if (trimmedFraction > 1.0e-4)
    {
        const auto trimX = plotArea.getX() + (float) juce::jmin (1.0, trimmedFraction) * plotArea.getWidth();
        g.setColour (Theme::background.withAlpha (0.75f));
        g.fillRect (plotArea.withRight (trimX));
        g.setColour (Theme::text.withAlpha (0.8f));
        g.drawVerticalLine (juce::roundToInt (trimX), plotArea.getY(), plotArea.getBottom());
        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("trimmed", plotArea.withRight (trimX - 4.0f).removeFromTop (16.0f), juce::Justification::centredRight, true);
    }

    // The part after the PEAK: cut (Cut), held (Freeze), restarted (Loop) or faded out (Release).
    if (playedFraction < 0.9999)
    {
        const auto cutX = plotArea.getX() + (float) playedFraction * plotArea.getWidth();
        g.setColour (Theme::background.withAlpha (0.75f));
        g.fillRect (plotArea.withLeft (cutX));
        g.setColour (Theme::text.withAlpha (0.8f));
        g.drawVerticalLine (juce::roundToInt (cutX), plotArea.getY(), plotArea.getBottom());
        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText ("after the PEAK", plotArea.withLeft (cutX + 4.0f).removeFromTop (16.0f), juce::Justification::centredLeft, true);
    }
}

//==============================================================================
void LayerEditor::sync()
{
    if (const auto version = samplerProcessor.getSampleVersion(); version != shownSampleVersion)
    {
        shownSampleVersion = version;
        sample = samplerProcessor.getSample (layer);
        waveform.setSample (sample);
        envelopeEditor.repaint();

        // A loading error is shown here too, as the main page's status line is covered.
        const auto status = samplerProcessor.getStatusMessage();
        const auto loading = samplerProcessor.getLoadingFile (layer);
        infoLabel.setColour (juce::Label::textColourId, status.isNotEmpty() && loading == juce::File() ? Theme::error : Theme::dimText);
        const auto missing = samplerProcessor.getMissingSamplePath (layer);
        infoLabel.setText (loading != juce::File() ? "Loading " + loading.getFullPathName() + "..."
                           : status.isNotEmpty()   ? status
                           : sample != nullptr     ? LayerText::describeFile (*sample)
                           : missing.isNotEmpty()  ? "Sample not found: " + missing
                                                   : juce::String ("No sample loaded"),
                           juce::dontSendNotification);
    }

    clearButton.setEnabled (! samplerProcessor.isLayerDefault (layer));

    const auto settings = samplerProcessor.getLayerSettings (layer);
    onButton.setButtonText (settings.enabled ? "ON" : "OFF");

    envelopeEditor.setEnvelope (samplerProcessor.getEnvelope (layer));

    juce::String cutText;   // where the PEAK (SAMPLE) and the note start fall in this layer

    {
        const auto lastNote = samplerProcessor.getLastNote();
        const auto noteLayout = samplerProcessor.getNoteLayout (lastNote >= 0 ? lastNote : settings.rootNote);
        const auto& l = noteLayout.layers[(size_t) layer];
        const auto fraction = l.fullLength > 0.0 ? juce::jlimit (0.0, 1.0, l.length / l.fullLength) : 1.0;

        const auto rate = samplerProcessor.getPlaybackRate();

        if (l.cutAway && l.getEnd() <= 0.0)
            cutText = " Moved before the note start: silent.";
        else if (l.cutAway)
            cutText = " Lies after the PEAK: silent.";
        else if (l.active && ! l.reachesPeak)
            cutText = " Ends " + LayerText::seconds ((noteLayout.peak - l.getEnd()) / rate) + " before the PEAK.";
        else if (l.active && l.fullLength - l.length > 0.5)
            cutText = " The PEAK comes " + LayerText::seconds ((l.fullLength - l.length) / rate) + " before its end.";

        if (l.active && l.getTrim() > 0.5)
            cutText = " Start trimmed " + LayerText::seconds (l.getTrim() / rate) + "." + cutText;

        const auto trimmed = l.fullLength > 0.0 ? juce::jlimit (0.0, 1.0, l.getTrim() / l.fullLength) : 0.0;

        if (std::abs (fraction - playedFraction) > 1.0e-4 || std::abs (trimmed - trimmedFraction) > 1.0e-4)
        {
            playedFraction = fraction;
            trimmedFraction = trimmed;
            envelopeEditor.repaint();
        }
    }

    if (! juce::exactlyEqual (settings.start, shownSettings.start) || ! juce::exactlyEqual (settings.end, shownSettings.end)
        || settings.reversed != shownSettings.reversed || settings.enabled != shownSettings.enabled)
    {
        shownSettings = settings;
        envelopeEditor.repaint();   // its background shows the region
    }

    waveform.setReversed (settings.reversed);
    waveform.setLayerEnabled (settings.enabled);
    waveform.setPlayheadPosition (samplerProcessor.getPlayheadPosition (layer));

    // Length controls: only Stretch mode has a length of its own.
    const auto stretch = settings.timeMode == rs::TimeMode::stretch;
    const auto beats = settings.lengthMode == rs::LengthMode::beats;
    lengthModeBox.setEnabled (stretch);
    secondsKnob.setVisible (! beats);
    beatsKnob.setVisible (beats);
    secondsKnob.setEnabled (stretch && settings.lengthMode == rs::LengthMode::seconds);
    beatsKnob.setEnabled (stretch);

    juce::String details ("Load a sample to hear this layer."), warning;

    if (sample != nullptr)
    {
        const auto timing = LayerText::describeTiming (*sample, settings, samplerProcessor.getPlaybackRate(), samplerProcessor.getCurrentBpm(),
                                                       samplerProcessor.getLastNote());
        details = timing.details + cutText;
        warning = timing.warning;
    }

    if (timingLabel.getText() != details)
    {
        timingLabel.setText (details, juce::dontSendNotification);
        timingLabel.setTooltip (details);   // the whole text, when it does not fit
    }

    if (warningLabel.getText() != warning)
    {
        const auto layoutChanges = warningLabel.getText().isEmpty() != warning.isEmpty();
        warningLabel.setText (warning, juce::dontSendNotification);

        if (layoutChanges)
            resized();
    }
}

void LayerEditor::setDragHighlight (bool shouldHighlight)
{
    if (dragHighlight != shouldHighlight)
    {
        dragHighlight = shouldHighlight;
        waveform.setDragHighlight (shouldHighlight);
        repaint();
    }
}

//==============================================================================
void LayerEditor::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.75f);

    g.setColour (Theme::panel.darker (0.15f));
    g.fillRoundedRectangle (bounds, 8.0f);
    g.setColour (colour.withAlpha (dragHighlight ? 1.0f : 0.6f));
    g.drawRoundedRectangle (bounds, 8.0f, 1.5f);

    const std::pair<juce::Rectangle<int>, const char*> groups[] = { { sourceGroup, "SOURCE" }, { timeGroup, "TIME" }, { soundGroup, "SOUND" } };

    for (const auto& [area, title] : groups)
    {
        g.setColour (Theme::panel);
        g.fillRoundedRectangle (area.toFloat(), 6.0f);
        g.setColour (Theme::border);
        g.drawRoundedRectangle (area.toFloat().reduced (0.5f), 6.0f, 1.0f);

        g.setColour (Theme::dimText);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (title, area.withHeight (groupTitleHeight).reduced (10, 0), juce::Justification::centredLeft);
    }
}

void LayerEditor::layoutGroup (juce::Rectangle<int> area, std::initializer_list<juce::Component*> columns)
{
    area = area.reduced (groupPadding).withTrimmedTop (groupTitleHeight - groupPadding);
    const auto columnWidth = area.getWidth() / juce::jmax (1, (int) columns.size());

    for (auto* column : columns)
        column->setBounds (area.removeFromLeft (columnWidth));
}

void LayerEditor::resized()
{
    auto area = getLocalBounds().reduced (12, 10);

    // Header: layer tabs, ON, R, file info, Load / Clear / Done.
    auto header = area.removeFromTop (28);

    for (auto& tab : layerTabs)
        tab.setBounds (header.removeFromLeft (78));

    header.removeFromLeft (12);
    onButton.setBounds (header.removeFromLeft (48).reduced (0, 2));
    header.removeFromLeft (6);
    reverseButton.setBounds (header.removeFromLeft (34).reduced (0, 2));
    header.removeFromLeft (12);

    doneButton.setBounds (header.removeFromRight (70).reduced (0, 2));
    header.removeFromRight (6);
    clearButton.setBounds (header.removeFromRight (64).reduced (0, 2));
    header.removeFromRight (6);
    loadButton.setBounds (header.removeFromRight (80).reduced (0, 2));
    header.removeFromRight (10);
    infoLabel.setBounds (header);

    // Bottom: the three groups of controls.
    auto groups = area.removeFromBottom (126);
    area.removeFromBottom (6);

    auto timingRow = area.removeFromBottom (22);
    warningLabel.setBounds (timingRow.removeFromRight (warningLabel.getText().isEmpty() ? 0 : timingRow.getWidth() * 2 / 5));
    timingLabel.setBounds (timingRow);
    area.removeFromBottom (4);

    area.removeFromTop (6);

    // The layer envelope under the waveform.
    auto envelopeArea = area.removeFromBottom (area.getHeight() * 2 / 5);
    auto envelopeHeader = envelopeArea.removeFromTop (20);
    flatButton.setBounds (envelopeHeader.removeFromRight (50).reduced (0, 1));
    envelopeLabel.setBounds (envelopeHeader);
    envelopeEditor.setBounds (envelopeArea);
    area.removeFromBottom (6);
    waveform.setBounds (area);

    // Widths in "knob units": SOURCE 3, TIME 3.5 (combo column 1.5 + length + offset knobs), SOUND 6.
    constexpr int gap = 10;
    const auto unit = (groups.getWidth() - 2 * gap - 6 * groupPadding) / 12.5f;
    const auto sourceWidth = juce::roundToInt (3.0f * unit) + 2 * groupPadding;
    const auto timeWidth   = juce::roundToInt (3.5f * unit) + 2 * groupPadding;

    sourceGroup = groups.removeFromLeft (sourceWidth);
    groups.removeFromLeft (gap);
    timeGroup = groups.removeFromLeft (timeWidth);
    groups.removeFromLeft (gap);
    soundGroup = groups;

    layoutGroup (sourceGroup, { &rootKnob, &startKnob, &endKnob });
    layoutGroup (soundGroup,  { &pitchKnob, &levelKnob, &panKnob, &widthKnob, &lowCutKnob, &highCutKnob });

    auto time = timeGroup.reduced (groupPadding).withTrimmedTop (groupTitleHeight - groupPadding);
    offsetKnob.setBounds (time.removeFromRight (juce::roundToInt (unit)));
    auto lengthKnobArea = time.removeFromRight (juce::roundToInt (unit));
    secondsKnob.setBounds (lengthKnobArea);
    beatsKnob.setBounds (lengthKnobArea);

    time.removeFromRight (4);
    modeColumn.setBounds (time);
    auto column = modeColumn.getLocalBounds();
    timeModeBox.setBounds (column.removeFromTop (44));
    column.removeFromTop (8);
    lengthModeBox.setBounds (column.removeFromTop (44));
}
