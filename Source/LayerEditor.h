#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Controls.h"
#include "EnvelopeEditor.h"
#include "PluginProcessor.h"
#include "WaveformView.h"

/** The settings of one layer. It opens over the two layers on the main page.

    SOURCE  root note, region start and end (also draggable on the waveform)
    TIME    Stretch or Resample, the length (= sample, seconds or beats) and the peak offset
    SOUND   pitch, level, pan, stereo width, low cut, high cut

    Under the waveform: the layer's volume envelope from the start of the build to its peak.
*/
class LayerEditor final : public juce::Component
{
public:
    LayerEditor (ReverseSamplerProcessor&, int layerIndex);
    ~LayerEditor() override;

    int getLayer() const noexcept   { return layer; }

    /** Reads the current sample, settings and playhead. Called by the editor's timer. */
    void sync();

    void setDragHighlight (bool shouldHighlight);

    std::function<void()> onClose;
    std::function<void (int layer)> onBrowse;
    std::function<void (int layer)> onShowLayer;   // the layer tabs in the header

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    juce::ParameterAttachment& getMarkerAttachment (WaveformView::Marker);
    void layoutGroup (juce::Rectangle<int> area, std::initializer_list<juce::Component*> columns);

    ReverseSamplerProcessor& samplerProcessor;
    const int layer;
    const juce::Colour colour;

    std::array<juce::TextButton, rs::numLayers> layerTabs;
    ToggleTextButton onButton, reverseButton;
    juce::TextButton loadButton { "Load..." }, clearButton { "Clear" }, doneButton { "Done" }, flatButton { "Flat" };
    juce::Label infoLabel, timingLabel, warningLabel, envelopeLabel;

    WaveformView waveform;
    std::unique_ptr<juce::ParameterAttachment> startAttachment, endAttachment;
    float startPercent = 0.0f, endPercent = 100.0f;

    EnvelopeEditor envelopeEditor;
    void paintEnvelopeBackground (juce::Graphics&, juce::Rectangle<float> plotArea);

    LabelledKnob rootKnob, startKnob, endKnob;
    LabelledCombo timeModeBox, lengthModeBox;
    LabelledKnob secondsKnob, beatsKnob, offsetKnob;
    LabelledKnob pitchKnob, levelKnob, panKnob, widthKnob, lowCutKnob, highCutKnob;
    juce::Component modeColumn;   // holds the two combo boxes

    juce::Rectangle<int> sourceGroup, timeGroup, soundGroup;

    rs::SampleDataPtr sample;
    rs::LayerSettings shownSettings;
    double playedFraction = 1.0;   // the part of the build before the PEAK
    double trimmedFraction = 0.0;  // the start of the build trimmed at the note start
    int shownSampleVersion = -1;
    bool dragHighlight = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LayerEditor)
};
