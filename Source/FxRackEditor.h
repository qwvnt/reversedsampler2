#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

class FxRackEditor final : public juce::Component, public juce::DragAndDropContainer
{
public:
    explicit FxRackEditor (ReverseSamplerProcessor&);
    ~FxRackEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    std::function<void()> onClose;
private:
    class Slot;
    ReverseSamplerProcessor& processor;
    juce::TextButton close { "Back" };
    std::array<std::unique_ptr<Slot>, rs::fxSlots> slots;
};
