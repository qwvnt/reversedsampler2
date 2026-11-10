#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Controls.h"
#include "PluginProcessor.h"

/** The LAYER 1 / LAYER 2 boxes at the sides of the main page: ON/OFF, R (reverse),
    what the layer plays, Load and Settings. Clicking the box opens the layer's settings.
*/
class LayerPanel final : public juce::Component
{
public:
    LayerPanel (ReverseSamplerProcessor&, int layerIndex);

    int getLayer() const noexcept   { return layer; }

    /** Reads the current sample and settings. Called by the editor's timer. */
    void sync();

    void setDragHighlight (bool shouldHighlight);

    std::function<void (int layer)> onOpenSettings;
    std::function<void (int layer)> onBrowse;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    static void setTextIfChanged (juce::Label&, const juce::String&);
    void layoutInfo();

    ReverseSamplerProcessor& samplerProcessor;
    const int layer;
    const juce::Colour colour;

    juce::Label titleLabel, fileLabel, lengthLabel, modeLabel, regionLabel, warningLabel;
    ToggleTextButton onButton, reverseButton;
    juce::TextButton loadButton { "Load..." }, settingsButton { "Settings" };

    rs::SampleDataPtr sample;
    int shownSampleVersion = -1;
    juce::Rectangle<int> infoArea;
    bool dragHighlight = false, layerEnabled = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LayerPanel)
};
