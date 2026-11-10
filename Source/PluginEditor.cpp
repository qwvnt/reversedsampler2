#include "PluginEditor.h"
#include "Theme.h"

namespace
{
    constexpr auto soonText = "coming soon";
}

//==============================================================================
ReverseSamplerEditor::ReverseSamplerEditor (ReverseSamplerProcessor& p)
    : AudioProcessorEditor (p),
      samplerProcessor (p),
      sampleView (p),
      modeButtons (p.getValueTreeState(), ParamIDs::playMode, Theme::text),
      afterPeakBox (p.getValueTreeState(), ParamIDs::afterPeak, "After peak"),
      attackKnob  (p.getValueTreeState(), ParamIDs::attack,  "Attack",  Theme::text),
      releaseKnob (p.getValueTreeState(), ParamIDs::release, "Release", Theme::text),
      volumeKnob  (p.getValueTreeState(), ParamIDs::gain,    "Volume",  Theme::text),
      keyboard (p.getKeyboardState(), juce::MidiKeyboardComponent::horizontalKeyboard)
{
    auto& lf = editorLookAndFeel;
    lf.setColour (juce::ResizableWindow::backgroundColourId, Theme::background);
    lf.setColour (juce::Label::textColourId, Theme::text);
    lf.setColour (juce::TextButton::buttonColourId, Theme::panelLight);
    lf.setColour (juce::TextButton::buttonOnColourId, Theme::layer1);
    lf.setColour (juce::TextButton::textColourOffId, Theme::text);
    lf.setColour (juce::TextButton::textColourOnId, Theme::background);
    lf.setColour (juce::ComboBox::backgroundColourId, Theme::panelLight);
    lf.setColour (juce::ComboBox::outlineColourId, Theme::border);
    lf.setColour (juce::ComboBox::textColourId, Theme::text);
    lf.setColour (juce::ComboBox::arrowColourId, Theme::dimText);
    lf.setColour (juce::PopupMenu::backgroundColourId, Theme::panel);
    lf.setColour (juce::PopupMenu::textColourId, Theme::text);
    lf.setColour (juce::PopupMenu::highlightedBackgroundColourId, Theme::panelLight);
    lf.setColour (juce::PopupMenu::highlightedTextColourId, Theme::text);
    lf.setColour (juce::Slider::rotarySliderFillColourId, Theme::layer1);
    lf.setColour (juce::Slider::rotarySliderOutlineColourId, Theme::border);
    lf.setColour (juce::Slider::thumbColourId, Theme::text);
    lf.setColour (juce::Slider::textBoxTextColourId, Theme::text);
    lf.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    lf.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    lf.setColour (juce::TooltipWindow::backgroundColourId, Theme::panelLight);
    lf.setColour (juce::TooltipWindow::textColourId, Theme::text);
    lf.setColour (juce::TooltipWindow::outlineColourId, Theme::border);
    lf.setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, Theme::layer1.withAlpha (0.7f));
    lf.setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, Theme::layer1.withAlpha (0.25f));
    setLookAndFeel (&lf);

    for (int layer = 0; layer < rs::numLayers; ++layer)
    {
        auto& panel = layerPanels[(size_t) layer];
        panel = std::make_unique<LayerPanel> (p, layer);
        panel->onOpenSettings = [this] (int l) { showLayerEditor (l); };
        panel->onBrowse       = [this] (int l) { browseForSample (l); };
        addAndMakeVisible (*panel);
    }

    sampleView.onOpenEditor = [this] { showSampleEditor(); };
    addAndMakeVisible (sampleView);

    modeButtons.buttons[0]->setTooltip ("Swell: the build starts with the key, the peak comes later");
    modeButtons.buttons[1]->setTooltip ("Arrive: the peak lands on the note (the plugin reports latency, the host plays earlier)");
    afterPeakBox.setTooltip ("At the PEAK, for all layers: Cut, Release (plays on and fades out), Freeze or Loop (while the key is held)");
    addAndMakeVisible (modeButtons);
    addAndMakeVisible (afterPeakBox);

    attackKnob.setTooltip ("Fade-in of every note (both layers)");
    releaseKnob.setTooltip ("Fade-out after the key is released (both layers)");
    volumeKnob.setTooltip ("Master volume");

    for (auto* knob : { &attackKnob, &releaseKnob, &volumeKnob })
        addAndMakeVisible (*knob);

    for (auto* label : { &statusLabel, &hintLabel })
    {
        label->setFont (juce::FontOptions (13.0f));
        label->setJustificationType (juce::Justification::topLeft);
        label->setMinimumHorizontalScale (1.0f);
        addAndMakeVisible (*label);
    }

    statusLabel.setColour (juce::Label::textColourId, Theme::error);
    hintLabel.setColour (juce::Label::textColourId, Theme::dimText);
    hintLabel.setFont (juce::FontOptions (12.0f));
    hintLabel.setText ("Click a layer to open its settings, click\n"
                       "SAMPLE for the time line and envelopes.\n"
                       "Drop WAV, AIFF, FLAC, OGG, MP3 on a layer,\n"
                       "two files fill both layers.\n"
                       "SAMPLE: Shift-drag a layer moves it,\n"
                       + juce::String (KeyNames::alt) + "-click and hold plays it alone.",
                       juce::dontSendNotification);

    undoButton.setTooltip ("Undo (" + juce::String (KeyNames::command) + "+Z)");
    redoButton.setTooltip ("Redo (" + juce::String (KeyNames::command) + "+Shift+Z"
                          #if ! JUCE_MAC
                           " or Ctrl+Y"
                          #endif
                           ")");
    undoButton.onClick = [this] { undoOrRedo (false); };
    redoButton.onClick = [this] { undoOrRedo (true); };

    for (auto* button : { &undoButton, &redoButton })
    {
        button->setWantsKeyboardFocus (false);
        addAndMakeVisible (*button);
    }

    // Create the safe pointer outside all lambdas: this avoids MSVC resolving
    // `this` as a nested closure rather than as the editor.
    const juce::Component::SafePointer<ReverseSamplerEditor> safeEditor (this);
    fxButton.onClick = [safeEditor]
    {
        if (auto* editor = safeEditor.getComponent())
        {
            editor->layerEditor = nullptr;
            editor->sampleEditor = nullptr;
            editor->modEditor = nullptr;
            editor->fxEditor = std::make_unique<FxRackEditor> (editor->samplerProcessor);
            editor->fxEditor->onClose = [safeEditor]
            {
                juce::MessageManager::callAsync ([safeEditor]
                {
                    if (auto* current = safeEditor.getComponent())
                    {
                        current->fxEditor = nullptr;
                        current->updateMainPageVisibility();
                        current->repaint();
                    }
                });
            };
            editor->fxEditor->setBounds (editor->getLocalBounds().withTrimmedBottom (76).reduced (8));
            editor->addAndMakeVisible (*editor->fxEditor);
            editor->updateMainPageVisibility();
        }
    };
    addAndMakeVisible (fxButton);

    modButton.setTooltip ("LFOs, envelope, macros and the routing of every modulation");
    modButton.onClick = [safeEditor]
    {
        if (auto* editor = safeEditor.getComponent())
            editor->showModMatrix();
    };
    addAndMakeVisible (modButton);

    for (int m = 0; m < rs::mod::numMacros; ++m)
    {
        auto& knob = macroKnobs[(size_t) m];
        knob = std::make_unique<LabelledKnob> (p.getValueTreeState(), rs::ModMatrix::macroID (m), p.getMacroName (m), Theme::modulation);
        knob->setTooltip ("Macro: route it to any parameters in the Mod Matrix, or right-click a knob and add it there");
        addAndMakeVisible (*knob);
    }

    struct MenuListener final : juce::MouseListener
    {
        explicit MenuListener (ReverseSamplerEditor& e) : editor (e) {}
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (! e.mods.isPopupMenu())
                return;
            if (auto* slider = modui::findTaggedSlider (e.eventComponent))
            {
                const juce::Component::SafePointer<ReverseSamplerEditor> safe (&editor);
                modui::showModulationMenu (editor.samplerProcessor, taggedParameter (*slider), *slider,
                                           [safe] { if (auto* current = safe.getComponent()) current->showModMatrix(); });
            }
        }
        ReverseSamplerEditor& editor;
    };
    modulationMenuListener = std::make_unique<MenuListener> (*this);
    addMouseListener (modulationMenuListener.get(), true);
    modulationOverlay = std::make_unique<ModulationOverlay> (p, *this);
    addAndMakeVisible (*modulationOverlay);

    // The shortcuts reach the editor when it, or something inside it, has the keyboard focus.
    setWantsKeyboardFocus (true);

    keyboard.setOctaveForMiddleC (3);   // MIDI 60 is shown as C3
    keyboard.setLowestVisibleKey (36);
    addAndMakeVisible (keyboard);

    setResizable (true, true);
    setResizeLimits (960, 640, 1920, 1200);
    setSize (1100, 720);

    syncWithProcessor();
    startTimerHz (30);
}

ReverseSamplerEditor::~ReverseSamplerEditor()
{
    stopTimer();
    removeMouseListener (modulationMenuListener.get());
    modulationOverlay = nullptr;
    layerEditor = nullptr;
    sampleEditor = nullptr;
    fxEditor = nullptr;
    modEditor = nullptr;
    setLookAndFeel (nullptr);
}

//==============================================================================
void ReverseSamplerEditor::paint (juce::Graphics& g)
{
    g.fillAll (Theme::background);

    auto title = titleArea;
    g.setColour (Theme::text);
    g.setFont (juce::FontOptions (32.0f, juce::Font::bold));
    g.drawText ("REVERSE SAMPLER", title.removeFromTop (38), juce::Justification::centred);
    g.setColour (Theme::dimText);
    g.setFont (juce::FontOptions (12.0f));
    g.drawText ("test build " JucePlugin_VersionString, title, juce::Justification::centredTop);

    paintPlaceholders (g);
}

void ReverseSamplerEditor::paintPlaceholders (juce::Graphics& g)
{
    const auto placeholderText = Theme::dimText.withAlpha (0.8f);
    const auto soonColour = Theme::dimText.withAlpha (0.55f);

    // Library Editor (top left) and Macro Editor (top right).
    const auto corner = [&] (juce::Rectangle<int> area, const juce::String& text, juce::Justification justification)
    {
        g.setColour (placeholderText);
        g.setFont (juce::FontOptions (17.0f, juce::Font::bold));
        g.drawText (text, area.removeFromTop (26), justification);
        g.setColour (soonColour);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (soonText, area.removeFromTop (16), justification);
    };

    corner (libraryArea, "LIBRARY EDITOR", juce::Justification::centredLeft);

    if (macroKnobs[0] != nullptr && macroKnobs[0]->isVisible())
    {
        g.setColour (placeholderText);
        g.setFont (juce::FontOptions (12.5f, juce::Font::bold));
        g.drawText ("MACROS", macrosArea.withHeight (18), juce::Justification::centred);
    }

    // Advanced Settings tab at the bottom.
    if (false && ! advancedArea.isEmpty())
    {
        juce::Path tab;
        tab.addRoundedRectangle ((float) advancedArea.getX(), (float) advancedArea.getY(),
                                 (float) advancedArea.getWidth(), (float) advancedArea.getHeight(),
                                 18.0f, 18.0f, true, true, false, false);
        g.setColour (Theme::panelLight);
        g.fillPath (tab);
        g.setColour (Theme::border);
        g.strokePath (tab, juce::PathStrokeType (1.0f));

        auto text = advancedArea.reduced (12, 2);
        g.setColour (placeholderText);
        g.setFont (juce::FontOptions (14.0f, juce::Font::bold));
        g.drawText ("ADVANCED SETTINGS (FX RACK, NOISE, ETC.)", text.removeFromTop (text.getHeight() * 3 / 5), juce::Justification::centredBottom);
        g.setColour (soonColour);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (soonText, text, juce::Justification::centredTop);
    }
}

void ReverseSamplerEditor::resized()
{
    auto area = getLocalBounds().reduced (14, 10);

    keyboard.setBounds (area.removeFromBottom (60));
    keyboard.setKeyWidth (juce::jmax (16.0f, (float) keyboard.getWidth() / 75.0f));   // 75 white keys fill wide windows
    area.removeFromBottom (6);

    // Three columns: LAYER 1 | SAMPLE | LAYER 2.
    const auto gap = juce::jmax (16, area.getWidth() / 50);
    const auto sideWidth = juce::jlimit (210, 340, (int) ((float) area.getWidth() * 0.23f));
    const auto columns = [&] (juce::Rectangle<int> row)
    {
        auto left = row.removeFromLeft (sideWidth);
        auto right = row.removeFromRight (sideWidth);
        return std::array<juce::Rectangle<int>, 3> { left, row.reduced (gap, 0), right };
    };

    auto header = columns (area.removeFromTop (56));
    libraryArea = header[0].withTrimmedTop (6);
    titleArea = header[1];
    macroEditorArea = header[2].withTrimmedTop (6);

    // Undo left of the title, Redo right of it.
    const auto titleRow = titleArea.withHeight (38);
    undoButton.setBounds (titleRow.withWidth (50).withSizeKeepingCentre (50, 24));
    redoButton.setBounds (titleRow.withTrimmedLeft (titleRow.getWidth() - 50).withSizeKeepingCentre (50, 24));

    macrosArea = area.removeFromTop (110).withSizeKeepingCentre (juce::jmin (header[1].getWidth(), 520), 110);
    {
        auto knobs = macrosArea.withTrimmedTop (18);
        const auto column = knobs.getWidth() / rs::mod::numMacros;
        for (auto& knob : macroKnobs)
            knob->setBounds (knobs.removeFromLeft (column).reduced (8, 0));
    }
    modButton.setBounds (macroEditorArea.removeFromTop (30).withTrimmedLeft (juce::jmax (0, macroEditorArea.getWidth() - 140)));
    area.removeFromTop (12);

    advancedArea = columns (area.removeFromBottom (40))[1];
    fxButton.setBounds (advancedArea.reduced (20, 2));
    area.removeFromBottom (8);

    auto controls = columns (area.removeFromBottom (96));
    area.removeFromBottom (8);
    auto middle = columns (area);

    layerPanels[0]->setBounds (middle[0]);
    sampleView.setBounds (middle[1]);
    layerPanels[1]->setBounds (middle[2]);

    hintLabel.setBounds (controls[0].withTrimmedTop (8));
    statusLabel.setBounds (controls[2].withTrimmedTop (8));

    // SWELL|ARRIVE, After peak, then the three knobs.
    const auto knobWidth = juce::jmin (90, controls[1].getWidth() / 6);
    auto row = controls[1].withSizeKeepingCentre (juce::jmin (controls[1].getWidth(), 120 + 120 + 3 * knobWidth + 16), controls[1].getHeight());
    modeButtons.setBounds (row.removeFromLeft (120).withSizeKeepingCentre (116, 28).translated (0, 8));
    row.removeFromLeft (8);
    afterPeakBox.setBounds (row.removeFromLeft (120).withSizeKeepingCentre (116, 44).translated (0, 8));
    row.removeFromLeft (8);

    for (auto* knob : { &attackKnob, &releaseKnob, &volumeKnob })
        knob->setBounds (row.removeFromLeft (knobWidth).reduced (6, 0));

    // The layer settings cover the middle row and the controls under it.
    layerEditorArea = middle[0].getUnion (controls[2]);

    if (layerEditor != nullptr)
        layerEditor->setBounds (layerEditorArea);

    if (sampleEditor != nullptr)
        sampleEditor->setBounds (layerEditorArea);
    if (fxEditor != nullptr)
        fxEditor->setBounds (getLocalBounds().withTrimmedBottom (76).reduced (8));
    if (modEditor != nullptr)
        modEditor->setBounds (getLocalBounds().withTrimmedBottom (76).reduced (8));
}

//==============================================================================
// Called from buttons inside the overlays, which are deleted when the page changes:
// the change happens after the click has finished.
void ReverseSamplerEditor::showLayerEditorLater (int layer)
{
    const SafePointer<ReverseSamplerEditor> safeThis (this);
    juce::MessageManager::callAsync ([safeThis, layer] { if (safeThis != nullptr) safeThis->showLayerEditor (layer); });
}

bool ReverseSamplerEditor::keyPressed (const juce::KeyPress& key)
{
    const auto modifiers = key.getModifiers();

    if (! modifiers.isCommandDown() || modifiers.isAltDown())
        return false;

    const auto code = juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) key.getKeyCode());

    if (code == 'Z')
    {
        undoOrRedo (modifiers.isShiftDown());
        return true;
    }

    if (code == 'Y' && ! modifiers.isShiftDown())
    {
        undoOrRedo (true);
        return true;
    }

    return false;
}

void ReverseSamplerEditor::undoOrRedo (bool redo)
{
    if (redo ? samplerProcessor.redo() : samplerProcessor.undo())
        syncWithProcessor();
}

void ReverseSamplerEditor::showLayerEditor (int layer)
{
    layerEditor = nullptr;
    sampleEditor = nullptr;

    if (juce::isPositiveAndBelow (layer, rs::numLayers))
    {
        layerEditor = std::make_unique<LayerEditor> (samplerProcessor, layer);
        layerEditor->onClose     = [this] { showLayerEditorLater (-1); };
        layerEditor->onShowLayer = [this] (int other) { showLayerEditorLater (other); };
        layerEditor->onBrowse    = [this] (int l) { browseForSample (l); };
        layerEditor->setBounds (layerEditorArea);
        addAndMakeVisible (*layerEditor);
    }

    updateMainPageVisibility();

    // The page that had the keyboard focus may be gone: keep the shortcuts working.
    if (isShowing() && ! hasKeyboardFocus (true))
        grabKeyboardFocus();
}

void ReverseSamplerEditor::showSampleEditor()
{
    layerEditor = nullptr;
    sampleEditor = std::make_unique<SampleEditor> (samplerProcessor);
    sampleEditor->onClose = [this] { showLayerEditorLater (-1); };
    sampleEditor->setBounds (layerEditorArea);
    addAndMakeVisible (*sampleEditor);

    updateMainPageVisibility();

    if (isShowing() && ! hasKeyboardFocus (true))
        grabKeyboardFocus();
}

void ReverseSamplerEditor::showModMatrix()
{
    layerEditor = nullptr;
    sampleEditor = nullptr;
    fxEditor = nullptr;

    if (modEditor == nullptr)
    {
        modEditor = std::make_unique<ModMatrixEditor> (samplerProcessor);
        modEditor->onClose = [this] { closeModMatrixLater(); };
        modEditor->onMacroNamesChanged = [this] { refreshMacroNames(); };
        modEditor->setBounds (getLocalBounds().withTrimmedBottom (76).reduced (8));
        addAndMakeVisible (*modEditor);
    }

    updateMainPageVisibility();
    repaint();
}

void ReverseSamplerEditor::closeModMatrixLater()
{
    const SafePointer<ReverseSamplerEditor> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis != nullptr)
        {
            safeThis->modEditor = nullptr;
            safeThis->refreshMacroNames();
            safeThis->updateMainPageVisibility();
            safeThis->repaint();
        }
    });
}

void ReverseSamplerEditor::refreshMacroNames()
{
    for (int m = 0; m < rs::mod::numMacros; ++m)
        macroKnobs[(size_t) m]->label.setText (samplerProcessor.getMacroName (m), juce::dontSendNotification);
}

void ReverseSamplerEditor::updateMainPageVisibility()
{
    const auto mainPageVisible = layerEditor == nullptr && sampleEditor == nullptr && fxEditor == nullptr && modEditor == nullptr;
    const auto overlayOpen = fxEditor != nullptr || modEditor != nullptr;
    fxButton.setVisible (! overlayOpen);
    modButton.setVisible (! overlayOpen);

    for (auto& knob : macroKnobs)
        knob->setVisible (! overlayOpen);

    for (auto& panel : layerPanels)
        panel->setVisible (mainPageVisible);

    for (juce::Component* c : { (juce::Component*) &sampleView, (juce::Component*) &modeButtons, (juce::Component*) &afterPeakBox,
                                (juce::Component*) &attackKnob, (juce::Component*) &releaseKnob, (juce::Component*) &volumeKnob,
                                (juce::Component*) &hintLabel, (juce::Component*) &statusLabel })
        c->setVisible (mainPageVisible);
}

//==============================================================================
juce::Array<juce::File> ReverseSamplerEditor::getReadableFiles (const juce::StringArray& files) const
{
    juce::Array<juce::File> readable;

    for (const auto& path : files)
        if (juce::File::isAbsolutePath (path))
            if (const juce::File file (path); file.existsAsFile() && samplerProcessor.canReadFile (file))
                readable.add (file);

    return readable;
}

int ReverseSamplerEditor::findDropLayer (int x, int y) const
{
    if (layerEditor != nullptr)
        return layerEditor->getLayer();

    if (sampleEditor != nullptr)
        return sampleEditor->getLayerAt (juce::Point<int> (x, y) - sampleEditor->getPosition());

    for (const auto& panel : layerPanels)
        if (panel->getBounds().contains (x, y))
            return panel->getLayer();

    if (sampleView.getBounds().contains (x, y))
        return sampleView.getLayerAt (juce::Point<int> (x, y) - sampleView.getPosition());

    for (int layer = 0; layer < rs::numLayers; ++layer)
        if (samplerProcessor.getSample (layer) == nullptr)
            return layer;

    return 0;
}

void ReverseSamplerEditor::showDropTarget (const juce::StringArray& files, int x, int y)
{
    if (layerEditor != nullptr)
    {
        layerEditor->setDragHighlight (true);
        return;
    }

    const auto target = findDropLayer (x, y);
    const auto fillsBothLayers = getReadableFiles (files).size() >= 2;

    for (int layer = 0; layer < rs::numLayers; ++layer)
    {
        const auto highlight = fillsBothLayers || layer == target;
        layerPanels[(size_t) layer]->setDragHighlight (highlight);
        sampleView.setDragHighlight (layer, highlight);

        if (sampleEditor != nullptr)
            sampleEditor->setDragHighlight (layer, highlight);
    }
}

void ReverseSamplerEditor::clearDropTarget()
{
    if (layerEditor != nullptr)
        layerEditor->setDragHighlight (false);

    for (int layer = 0; layer < rs::numLayers; ++layer)
    {
        layerPanels[(size_t) layer]->setDragHighlight (false);
        sampleView.setDragHighlight (layer, false);

        if (sampleEditor != nullptr)
            sampleEditor->setDragHighlight (layer, false);
    }
}

bool ReverseSamplerEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    // Accept any file: unsupported ones are reported in the status line after the drop.
    return ! files.isEmpty();
}

void ReverseSamplerEditor::fileDragEnter (const juce::StringArray& files, int x, int y)   { showDropTarget (files, x, y); }
void ReverseSamplerEditor::fileDragMove (const juce::StringArray& files, int x, int y)    { showDropTarget (files, x, y); }
void ReverseSamplerEditor::fileDragExit (const juce::StringArray&)                        { clearDropTarget(); }

void ReverseSamplerEditor::filesDropped (const juce::StringArray& files, int x, int y)
{
    clearDropTarget();

    const auto target = findDropLayer (x, y);
    const auto readable = getReadableFiles (files);

    if (layerEditor == nullptr && readable.size() >= 2)
    {
        // Two files: the first goes where it was dropped, the second to the other layer.
        samplerProcessor.loadSampleAsync (target, readable[0]);
        samplerProcessor.loadSampleAsync ((target + 1) % rs::numLayers, readable[1]);
    }
    else if (! readable.isEmpty())
    {
        samplerProcessor.loadSampleAsync (target, readable[0]);
    }
    else
    {
        // Nothing playable: try the first item anyway, so the status line says why it failed.
        for (const auto& path : files)
        {
            if (juce::File::isAbsolutePath (path))
            {
                samplerProcessor.loadSampleAsync (target, juce::File (path));
                break;
            }
        }
    }

    syncWithProcessor();
}

//==============================================================================
void ReverseSamplerEditor::browseForSample (int layer)
{
    auto startLocation = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    for (const auto l : { layer, 1 - layer })
    {
        if (const auto sample = samplerProcessor.getSample (l))
        {
            startLocation = sample->file.getParentDirectory();
            break;
        }
    }

    fileChooser = std::make_unique<juce::FileChooser> ("Choose a sample for layer " + juce::String (layer + 1), startLocation,
                                                       samplerProcessor.getSupportedFileWildcard());

    const auto chooserFlags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;

    fileChooser->launchAsync (chooserFlags, [safeThis = SafePointer<ReverseSamplerEditor> (this), layer] (const juce::FileChooser& chooser)
    {
        if (safeThis == nullptr)
            return;

        const auto result = chooser.getResult();

        if (result != juce::File())
            safeThis->samplerProcessor.loadSampleAsync (layer, result);

        safeThis->syncWithProcessor();
    });
}

//==============================================================================
void ReverseSamplerEditor::timerCallback()
{
    syncWithProcessor();
}

void ReverseSamplerEditor::syncWithProcessor()
{
    for (auto& panel : layerPanels)
        panel->sync();

    sampleView.sync();
    refreshMacroNames();

    if (layerEditor != nullptr)
        layerEditor->sync();

    if (sampleEditor != nullptr)
        sampleEditor->sync();

    if (const auto status = samplerProcessor.getStatusMessage(); statusLabel.getText() != status)
        statusLabel.setText (status, juce::dontSendNotification);

    undoButton.setEnabled (samplerProcessor.canUndo());
    redoButton.setEnabled (samplerProcessor.canRedo());
}
