#pragma once
#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

namespace modui {
juce::String destinationLabel (ReverseSamplerProcessor&, int destination);
void showModulationMenu (ReverseSamplerProcessor&, const juce::String& paramID, juce::Component& target, std::function<void()> openMatrix);
juce::Slider* findTaggedSlider (juce::Component* component);
}

class ModulationOverlay final : public juce::Component, private juce::Timer
{
public:
    ModulationOverlay (ReverseSamplerProcessor&, juce::Component& root);
    void paint (juce::Graphics&) override;

private:
    struct Ring {
        juce::Rectangle<float> bounds;
        float startAngle = 0.0f, endAngle = 0.0f, base = 0.0f, low = 0.0f, high = 0.0f, current = 0.0f;
        bool operator== (const Ring& o) const noexcept {
            return bounds == o.bounds && juce::exactlyEqual (base, o.base) && juce::exactlyEqual (low, o.low)
                && juce::exactlyEqual (high, o.high) && juce::exactlyEqual (current, o.current);
        }
    };
    void timerCallback() override;
    void collect (juce::Component&, std::vector<Ring>&);

    ReverseSamplerProcessor& processor;
    juce::Component& root;
    std::vector<Ring> rings;
};

class ModMatrixEditor final : public juce::Component, private juce::Timer
{
public:
    explicit ModMatrixEditor (ReverseSamplerProcessor&);
    ~ModMatrixEditor() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    std::function<void()> onClose;
    std::function<void()> onMacroNamesChanged;

private:
    class BarLookAndFeel;
    class LfoPanel;
    class EnvelopePanel;
    class MacroPanel;
    class Row;
    void timerCallback() override;

    ReverseSamplerProcessor& processor;
    std::unique_ptr<BarLookAndFeel> barLookAndFeel;
    juce::TextButton close { "Back" };
    std::array<std::unique_ptr<LfoPanel>, rs::mod::numLfos> lfoPanels;
    std::unique_ptr<EnvelopePanel> envelopePanel;
    std::unique_ptr<MacroPanel> macroPanel;
    std::array<std::unique_ptr<Row>, rs::mod::numRows> rows;
    juce::Rectangle<int> matrixArea, headerArea;
    std::array<int, 9> columnX {};
    juce::String fxSignature;
};
