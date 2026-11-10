#include "LayerPanel.h"
#include "LayerText.h"

LayerPanel::LayerPanel (ReverseSamplerProcessor& p, int layerIndex)
    : samplerProcessor (p),
      layer (layerIndex),
      colour (Theme::layerColour (layerIndex)),
      onButton (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::on), "ON", Theme::layerColour (layerIndex),
                "Switch the layer on or off"),
      reverseButton (p.getValueTreeState(), ParamIDs::layerID (layerIndex, ParamIDs::reverse), "R", Theme::reversed,
                     "Reverse: play the layer backwards")
{
    titleLabel.setText ("LAYER " + juce::String (layer + 1), juce::dontSendNotification);
    titleLabel.setFont (juce::FontOptions (26.0f, juce::Font::bold));
    titleLabel.setColour (juce::Label::textColourId, colour);

    fileLabel.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    lengthLabel.setFont (juce::FontOptions (13.5f));
    modeLabel.setFont (juce::FontOptions (13.5f));
    regionLabel.setFont (juce::FontOptions (13.5f));
    lengthLabel.setColour (juce::Label::textColourId, Theme::dimText);
    modeLabel.setColour (juce::Label::textColourId, Theme::dimText);
    regionLabel.setColour (juce::Label::textColourId, Theme::dimText);
    warningLabel.setFont (juce::FontOptions (13.0f, juce::Font::bold));
    warningLabel.setColour (juce::Label::textColourId, Theme::warning);

    for (auto* label : { &titleLabel, &fileLabel, &lengthLabel, &modeLabel, &regionLabel, &warningLabel })
    {
        label->setJustificationType (juce::Justification::centred);
        label->setMinimumHorizontalScale (1.0f);          // shorten long text with "..." instead of squashing it
        label->setInterceptsMouseClicks (false, false);   // clicks open the settings
        addAndMakeVisible (*label);
    }

    warningLabel.setJustificationType (juce::Justification::centredTop);

    for (auto* button : { &onButton, &reverseButton })
        addAndMakeVisible (*button);

    loadButton.setTooltip ("Load an audio file into this layer");
    loadButton.onClick = [this] { if (onBrowse != nullptr) onBrowse (layer); };
    addAndMakeVisible (loadButton);

    settingsButton.setTooltip ("Pitch, length, region, level, pan, filters...");
    settingsButton.onClick = [this] { if (onOpenSettings != nullptr) onOpenSettings (layer); };
    addAndMakeVisible (settingsButton);

    setMouseCursor (juce::MouseCursor::PointingHandCursor);
    sync();
}

void LayerPanel::setTextIfChanged (juce::Label& label, const juce::String& text)
{
    if (label.getText() != text)
        label.setText (text, juce::dontSendNotification);
}

//==============================================================================
void LayerPanel::sync()
{
    if (const auto version = samplerProcessor.getSampleVersion(); version != shownSampleVersion)
    {
        shownSampleVersion = version;
        sample = samplerProcessor.getSample (layer);
    }

    const auto settings = samplerProcessor.getLayerSettings (layer);

    if (layerEnabled != settings.enabled)
    {
        layerEnabled = settings.enabled;
        titleLabel.setColour (juce::Label::textColourId, layerEnabled ? colour : Theme::disabled);
        repaint();
    }

    onButton.setButtonText (settings.enabled ? "ON" : "OFF");

    const auto hadWarning = warningLabel.getText().isNotEmpty();
    const auto loading = samplerProcessor.getLoadingFile (layer);

    if (sample == nullptr && loading != juce::File())
    {
        setTextIfChanged (fileLabel, "Loading: " + loading.getFileName());
        setTextIfChanged (lengthLabel, "Please wait...");
        setTextIfChanged (modeLabel, {});
        setTextIfChanged (regionLabel, {});
        setTextIfChanged (warningLabel, {});

        if (hadWarning)
            layoutInfo();

        return;
    }

    if (sample == nullptr)
    {
        const auto missing = samplerProcessor.getMissingSamplePath (layer);
        setTextIfChanged (fileLabel, missing.isNotEmpty() ? "Not found: " + juce::File::createFileWithoutCheckingPath (missing).getFileName()
                                                          : juce::String ("Empty"));
        setTextIfChanged (lengthLabel, missing.isNotEmpty() ? juce::String ("Drop the file here again") : juce::String ("Drop an audio file here"));
        setTextIfChanged (modeLabel, "or click Load...");
        setTextIfChanged (regionLabel, {});
        setTextIfChanged (warningLabel, {});

        if (hadWarning)
            layoutInfo();

        return;
    }

    const auto timing = LayerText::describeTiming (*sample, settings, samplerProcessor.getPlaybackRate(), samplerProcessor.getCurrentBpm(),
                                                       samplerProcessor.getLastNote());
    // A new file loading in the background: the current one plays until it is ready.
    setTextIfChanged (fileLabel, loading != juce::File() ? "Loading: " + loading.getFileName() : sample->file.getFileName());
    setTextIfChanged (lengthLabel, timing.lengthLine);
    setTextIfChanged (modeLabel, timing.modeLine);
    setTextIfChanged (regionLabel, "Region " + juce::String (juce::roundToInt (settings.start * 100.0)) + " - "
                                       + juce::String (juce::roundToInt (settings.end * 100.0)) + " %   |   Root "
                                       + juce::MidiMessage::getMidiNoteName (settings.rootNote, true, true, 3));
    setTextIfChanged (warningLabel, timing.warning);

    if (hadWarning != timing.warning.isNotEmpty())
        layoutInfo();
}

void LayerPanel::setDragHighlight (bool shouldHighlight)
{
    if (dragHighlight != shouldHighlight)
    {
        dragHighlight = shouldHighlight;
        repaint();
    }
}

void LayerPanel::mouseUp (const juce::MouseEvent& event)
{
    if (! event.mouseWasClicked())
        return;

    if (sample == nullptr)
    {
        if (onBrowse != nullptr)
            onBrowse (layer);
    }
    else if (onOpenSettings != nullptr)
    {
        onOpenSettings (layer);
    }
}

//==============================================================================
void LayerPanel::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat().reduced (0.75f);
    const auto accent = layerEnabled ? colour : Theme::disabled;

    g.setColour (Theme::panel);
    g.fillRoundedRectangle (bounds, 10.0f);

    g.setColour (dragHighlight ? accent : Theme::border);
    g.drawRoundedRectangle (bounds, 10.0f, dragHighlight ? 2.0f : 1.5f);

    // Accent bar under the title.
    const auto bar = titleLabel.getBounds().toFloat().withSizeKeepingCentre (48.0f, 3.0f)
                                                     .withY ((float) titleLabel.getBottom() + 2.0f);
    g.setColour (accent);
    g.fillRoundedRectangle (bar, 1.5f);
}

void LayerPanel::resized()
{
    auto area = getLocalBounds().reduced (12, 12);
    const auto compact = getHeight() < 280;

    auto buttons = area.removeFromBottom (28);
    const auto buttonWidth = (buttons.getWidth() - 8) / 2;
    loadButton.setBounds (buttons.removeFromLeft (buttonWidth));
    settingsButton.setBounds (buttons.removeFromRight (buttonWidth));
    area.removeFromBottom (compact ? 6 : 8);

    titleLabel.setBounds (area.removeFromTop (compact ? 28 : 36));
    area.removeFromTop (compact ? 10 : 12);

    auto toggles = area.removeFromTop (28).withSizeKeepingCentre (56 + 8 + 40, 28);
    onButton.setBounds (toggles.removeFromLeft (56));
    reverseButton.setBounds (toggles.removeFromRight (40));
    area.removeFromTop (compact ? 2 : 10);

    infoArea = area;
    layoutInfo();
}

void LayerPanel::layoutInfo()
{
    constexpr int fileHeight = 22, lineHeight = 20, warningGap = 4, warningHeight = 36;
    constexpr int fullHeight = fileHeight + 3 * lineHeight + warningGap + warningHeight;

    const auto hasWarning = warningLabel.getText().isNotEmpty();
    auto info = infoArea;
    auto showRegion = true;

    if (info.getHeight() >= fullHeight)
    {
        // Everything fits: a fixed block in the middle, so nothing jumps when a warning appears.
        info = info.withSizeKeepingCentre (info.getWidth(), fullHeight);
    }
    else
    {
        // Small window: the warning is more important than the region line.
        showRegion = ! hasWarning && info.getHeight() >= fileHeight + 3 * lineHeight;
    }

    fileLabel.setBounds (info.removeFromTop (fileHeight));
    lengthLabel.setBounds (info.removeFromTop (lineHeight));
    modeLabel.setBounds (info.removeFromTop (lineHeight));
    regionLabel.setVisible (showRegion);

    if (showRegion)
        regionLabel.setBounds (info.removeFromTop (lineHeight));

    info.removeFromTop (warningGap);
    warningLabel.setBounds (info);
}
