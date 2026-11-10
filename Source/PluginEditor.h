#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include "Controls.h"
#include "LayerEditor.h"
#include "FxRackEditor.h"
#include "ModMatrixEditor.h"
#include "LayerPanel.h"
#include "PluginProcessor.h"
#include "SampleEditor.h"
#include "SampleView.h"

//==============================================================================
/** Main page, laid out like the concept sketch:

        LIBRARY EDITOR          REVERSE SAMPLER          MACRO EDITOR
                              4 macro knobs
        [ LAYER 1 ]          [   SAMPLE   ]          [ LAYER 2 ]
           SWELL|ARRIVE  After peak  Attack Release Volume
                           ADVANCED SETTINGS
        on-screen keyboard (test aid)

    Library Editor, Macro Editor, the macros and Advanced Settings are placeholders
    for later versions. Clicking a layer opens its settings over the middle row,
    clicking SAMPLE opens the SAMPLE editor (time line, final envelope, modes) there.
*/
class ReverseSamplerEditor final : public juce::AudioProcessorEditor,
                                   public juce::FileDragAndDropTarget,
                                   private juce::Timer
{
public:
    explicit ReverseSamplerEditor (ReverseSamplerProcessor&);
    ~ReverseSamplerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Undo: Cmd/Ctrl+Z. Redo: Cmd/Ctrl+Shift+Z or Ctrl+Y. */
    bool keyPressed (const juce::KeyPress&) override;
    void undoOrRedo (bool redo);

    /** Opens the settings of a layer over the middle row; -1 closes them. */
    void showLayerEditor (int layer);
    int getOpenLayer() const noexcept   { return layerEditor != nullptr ? layerEditor->getLayer() : -1; }

    /** Opens the SAMPLE editor over the middle row (closes the layer settings). */
    void showSampleEditor();
    bool isSampleEditorOpen() const noexcept   { return sampleEditor != nullptr; }

    bool isInterestedInFileDrag (const juce::StringArray& files) override;
    void fileDragEnter (const juce::StringArray& files, int x, int y) override;
    void fileDragMove (const juce::StringArray& files, int x, int y) override;
    void fileDragExit (const juce::StringArray& files) override;
    void filesDropped (const juce::StringArray& files, int x, int y) override;

private:
    void timerCallback() override;
    void syncWithProcessor();
    void browseForSample (int layer);
    void paintPlaceholders (juce::Graphics&);
    void updateMainPageVisibility();
    void showLayerEditorLater (int layer);
    void showModMatrix();
    void closeModMatrixLater();
    void refreshMacroNames();

    /** The layer a drop at (x, y) goes to: the open settings, the layer panel or waveform
        under the mouse, else the first empty layer. */
    int findDropLayer (int x, int y) const;
    juce::Array<juce::File> getReadableFiles (const juce::StringArray& files) const;
    void showDropTarget (const juce::StringArray& files, int x, int y);
    void clearDropTarget();

    ReverseSamplerProcessor& samplerProcessor;
    juce::LookAndFeel_V4 editorLookAndFeel;

    std::array<std::unique_ptr<LayerPanel>, rs::numLayers> layerPanels;
    SampleView sampleView;
    std::unique_ptr<LayerEditor> layerEditor;
    std::unique_ptr<SampleEditor> sampleEditor;
    std::unique_ptr<FxRackEditor> fxEditor;
    juce::TextButton fxButton { "FX RACK" };
    std::unique_ptr<ModMatrixEditor> modEditor;
    juce::TextButton modButton { "MOD MATRIX" };
    std::array<std::unique_ptr<LabelledKnob>, rs::mod::numMacros> macroKnobs;
    std::unique_ptr<ModulationOverlay> modulationOverlay;
    std::unique_ptr<juce::MouseListener> modulationMenuListener;

    ChoiceButtons modeButtons;
    LabelledCombo afterPeakBox;
    LabelledKnob attackKnob, releaseKnob, volumeKnob;
    juce::Label statusLabel, hintLabel;
    juce::TextButton undoButton { "Undo" }, redoButton { "Redo" };   // for hosts that keep the shortcuts for themselves
    juce::MidiKeyboardComponent keyboard;
    juce::TooltipWindow tooltipWindow { this, 600 };

    std::unique_ptr<juce::FileChooser> fileChooser;

    juce::Rectangle<int> libraryArea, titleArea, macroEditorArea, macrosArea, advancedArea, layerEditorArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverseSamplerEditor)
};
