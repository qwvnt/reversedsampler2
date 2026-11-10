#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "Theme.h"

/** Modifier key names as printed on the keyboard of this platform. */
namespace KeyNames
{
   #if JUCE_MAC
    inline const char* command = "Cmd";
    inline const char* alt     = "Option";
   #else
    inline const char* command = "Ctrl";
    inline const char* alt     = "Alt";
   #endif
}

/** A rotary knob with a caption above it, attached to a parameter.
    The value text (with its unit) comes from the parameter; double-click resets it.
*/
class LabelledKnob final : public juce::Component
{
public:
    LabelledKnob (juce::AudioProcessorValueTreeState& state, const juce::String& parameterID,
                  const juce::String& caption, juce::Colour accent)
    {
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 76, 18);
        slider.setColour (juce::Slider::rotarySliderFillColourId, accent);
        // Set on the slider itself: the value box copies these when it is created.
        slider.setColour (juce::Slider::textBoxTextColourId, Theme::text);
        slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (slider);

        label.setText (caption, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, Theme::dimText);
        label.setFont (juce::FontOptions (13.0f));
        label.setInterceptsMouseClicks (false, false);
        addAndMakeVisible (label);

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (state, parameterID, slider);
        slider.getProperties().set ("rsParam", parameterID);

        if (auto* parameter = state.getParameter (parameterID))
            slider.setDoubleClickReturnValue (true, parameter->convertFrom0to1 (parameter->getDefaultValue()));
    }

    void setTooltip (const juce::String& text)   { slider.setTooltip (text); }

    void resized() override
    {
        auto area = getLocalBounds();
        label.setBounds (area.removeFromTop (16));
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, juce::jlimit (40, 76, getWidth() - 2), 18);
        slider.setBounds (area);
    }

    juce::Slider slider { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label label;

private:
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LabelledKnob)
};

/** A combo box with a caption above it, attached to a choice parameter. */
class LabelledCombo final : public juce::Component
{
public:
    LabelledCombo (juce::AudioProcessorValueTreeState& state, const juce::String& parameterID, const juce::String& caption)
    {
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (parameterID)))
            box.addItemList (choice->choices, 1);

        addAndMakeVisible (box);

        label.setText (caption, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, Theme::dimText);
        label.setFont (juce::FontOptions (13.0f));
        addAndMakeVisible (label);

        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (state, parameterID, box);
    }

    void setTooltip (const juce::String& text)   { box.setTooltip (text); }

    void resized() override
    {
        auto area = getLocalBounds();
        label.setBounds (area.removeFromTop (16));
        box.setBounds (area.removeFromTop (24).reduced (2, 0));
    }

    juce::ComboBox box;
    juce::Label label;

private:
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LabelledCombo)
};

/** A small toggle button ("ON", "R") attached to a bool parameter. */
class ToggleTextButton final : public juce::TextButton
{
public:
    ToggleTextButton (juce::AudioProcessorValueTreeState& state, const juce::String& parameterID,
                      const juce::String& buttonText, juce::Colour onColour, const juce::String& tooltip)
        : juce::TextButton (buttonText)
    {
        setClickingTogglesState (true);
        setColour (juce::TextButton::buttonOnColourId, onColour);
        setColour (juce::TextButton::textColourOnId, Theme::background);
        setTooltip (tooltip);
        attachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (state, parameterID, *this);
    }

private:
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ToggleTextButton)
};

/** One button per choice of a choice parameter, side by side (e.g. SWELL | ARRIVE). */
class ChoiceButtons final : public juce::Component
{
public:
    ChoiceButtons (juce::AudioProcessorValueTreeState& state, const juce::String& parameterID, juce::Colour onColour)
    {
        auto* choice = dynamic_cast<juce::AudioParameterChoice*> (state.getParameter (parameterID));
        jassert (choice != nullptr);

        if (choice == nullptr)
            return;

        for (int i = 0; i < choice->choices.size(); ++i)
        {
            auto* button = buttons.add (new juce::TextButton (choice->choices[i].toUpperCase()));
            button->setColour (juce::TextButton::buttonOnColourId, onColour);
            button->setColour (juce::TextButton::textColourOnId, Theme::background);
            button->setConnectedEdges ((i > 0 ? juce::Button::ConnectedOnLeft : 0)
                                       | (i < choice->choices.size() - 1 ? juce::Button::ConnectedOnRight : 0));
            button->onClick = [this, i] { if (attachment != nullptr) attachment->setValueAsCompleteGesture ((float) i); };
            addAndMakeVisible (button);
        }

        attachment = std::make_unique<juce::ParameterAttachment> (*choice, [this] (float value) { showChoice (juce::roundToInt (value)); });
        attachment->sendInitialUpdate();
    }

    void resized() override
    {
        auto area = getLocalBounds();
        const auto width = area.getWidth() / juce::jmax (1, buttons.size());

        for (auto* button : buttons)
            button->setBounds (button == buttons.getLast() ? area : area.removeFromLeft (width));
    }

    juce::OwnedArray<juce::TextButton> buttons;

private:
    void showChoice (int index)
    {
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setToggleState (i == index, juce::dontSendNotification);
    }

    std::unique_ptr<juce::ParameterAttachment> attachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ChoiceButtons)
};

inline void tagParameter (juce::Slider& slider, const juce::String& parameterID)
{
    slider.getProperties().set ("rsParam", parameterID);
}

inline juce::String taggedParameter (const juce::Component& component)
{
    return component.getProperties()["rsParam"].toString();
}
